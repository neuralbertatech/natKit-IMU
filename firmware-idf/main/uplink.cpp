#include "uplink.hpp"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "gateway_net.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-uplink";

constexpr uart_port_t kUartPort =
    static_cast<uart_port_t>(CONFIG_NATKIT_UPLINK_UART_NUM);

// True when the framed stream shares the wire with the human console. A bring-up
// mode, not the design: see the log hook below for what it costs.
constexpr bool kOnConsole = CONFIG_NATKIT_UPLINK_UART_NUM == 0;

struct TxFrame {
  size_t length;
  uint8_t bytes[kUplinkMaxFrame];
};

QueueHandle_t sQueue = nullptr;
SemaphoreHandle_t sWireLock = nullptr;
UplinkStats sStats{};
uint32_t sSequence = 0;

// --- sharing the console, when asked to ------------------------------------
//
// Writing binary frames to the same UART the logger uses means two writers on one
// wire, and a log line landing inside a frame corrupts it -- the reader would
// resync and drop it, which works but throws away most frames.
//
// So in that mode the logger is routed through here and takes the same lock the
// frame writer does. It makes the console-shared path actually usable rather than
// nominally supported. It is still the fallback: the real uplink is its own UART,
// and this exists so the protocol can be exercised on a bench with no USB-to-TTL
// adapter on it.
int (*sPreviousVprintf)(const char *, va_list) = nullptr;

int lockedVprintf(const char *format, va_list args) {
  if (sWireLock != nullptr) {
    xSemaphoreTake(sWireLock, portMAX_DELAY);
  }
  const int written = sPreviousVprintf != nullptr
                          ? sPreviousVprintf(format, args)
                          : vprintf(format, args);
  if (sWireLock != nullptr) {
    xSemaphoreGive(sWireLock);
  }
  return written;
}

template <typename T>
size_t writeLe(uint8_t *out, T value) {
  for (size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
  return sizeof(T);
}

// EITHER uplink publishes rather than writing to a wire. The exit is the same
// (MQTT); only how the packets reach the network differs, and nothing below this
// point cares which.
#if defined(CONFIG_NATKIT_PRIMARY_WIFI_UPLINK) || \
    defined(CONFIG_NATKIT_PRIMARY_ETH_UPLINK)
constexpr bool kWifiUplink = true;
#else
constexpr bool kWifiUplink = false;
#endif

// The topic the bridge already listens on -- the same one the gateway publishes,
// because the whole point of #373 is that nothing server-side can tell which
// architecture produced the data.
constexpr char kTopicTemplate[] =
    "natKit/sending/Data-%" PRIu64 "-Binary-NatImuBulkDataSchema";

char sTopic[96];

// Unwraps one of our own uplink frames and publishes its payload.
//
// Only data frames go out: node and primary status are for a gateway that is not
// in this architecture, and publishing them on the data topic would corrupt the
// stream. They are still queued and counted, so the counters stay comparable
// against the two-board runs -- which is the comparison this whole switch exists
// to make.
void publishFrame(const uint8_t *frame, size_t length) {
  if (length < kUplinkHeaderSize + kUplinkCrcSize) {
    return;
  }
  if (static_cast<UplinkType>(frame[3]) != UplinkType::kData) {
    ++sStats.frames_sent;  // consumed, just not published
    return;
  }
  uint64_t stream_id = 0;
  uint16_t payload_length = 0;
  std::memcpy(&stream_id, frame + 4, sizeof(stream_id));
  std::memcpy(&payload_length, frame + 16, sizeof(payload_length));
  if (kUplinkHeaderSize + payload_length + kUplinkCrcSize > length) {
    return;
  }

  std::snprintf(sTopic, sizeof(sTopic), kTopicTemplate, stream_id);
  if (gatewayPublish(sTopic, frame + kUplinkHeaderSize, payload_length)) {
    ++sStats.frames_sent;
    sStats.bytes_sent += payload_length;
  } else {
    ++sStats.write_timeouts;  // the broker refused it; same slot, same meaning
  }
}

void drainTask(void *) {
  TxFrame frame{};
  while (true) {
    if (xQueueReceive(sQueue, &frame, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    // --- #373: publish instead of writing to a wire -------------------------
    //
    // Same queue, same drop-oldest policy, same counters -- only the exit
    // changes. Publishing HERE rather than from the ESP-NOW receive callback is
    // deliberate: that callback runs on the WiFi task, and a socket write on it
    // would stall reception for every node to relieve congestion caused by one.
    if (kWifiUplink) {
      publishFrame(frame.bytes, frame.length);
      continue;
    }

    if (sWireLock != nullptr) {
      xSemaphoreTake(sWireLock, portMAX_DELAY);
    }

    int written = 0;
    esp_err_t flushed = ESP_OK;
    if (kOnConsole) {
      // stdout, NOT uart_write_bytes. The console UART has no driver installed
      // -- ESP-IDF's default console goes through VFS and the ROM writer -- so
      // uart_write_bytes on it fails on a null driver object. Writing through
      // stdout uses the same path the logger does, which is also what makes the
      // shared lock meaningful.
      written = static_cast<int>(
          fwrite(frame.bytes, 1, frame.length, stdout));
      fflush(stdout);
    } else {
      // A bounded wait, not portMAX_DELAY. If the far end has stopped reading
      // and the driver's buffer is full, this task must come back and let the
      // queue's drop policy run -- blocking here forever would turn
      // back-pressure into a silent stall, the failure natVR already hit once.
      written = uart_write_bytes(kUartPort,
                                 reinterpret_cast<const char *>(frame.bytes),
                                 frame.length);
      flushed = uart_wait_tx_done(
          kUartPort, pdMS_TO_TICKS(CONFIG_NATKIT_UPLINK_WRITE_TIMEOUT_MS));
    }

    if (sWireLock != nullptr) {
      xSemaphoreGive(sWireLock);
    }

    if (written < static_cast<int>(frame.length) || flushed != ESP_OK) {
      ++sStats.write_timeouts;
      continue;
    }
    ++sStats.frames_sent;
    sStats.bytes_sent += static_cast<uint64_t>(written);
  }
}

}  // namespace

esp_err_t uplinkStart() {
  uart_config_t cfg{};
  cfg.baud_rate = CONFIG_NATKIT_UPLINK_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (!kOnConsole && !kWifiUplink) {
    // Only install a driver and re-pin when this is OUR uart. Doing either to
    // UART0 would fight the console driver that is already there, and under
    // #373 there is no wire at all -- the exit is the radio.
    ESP_ERROR_CHECK(uart_driver_install(kUartPort,
                                        CONFIG_NATKIT_UPLINK_RX_BUFFER,
                                        CONFIG_NATKIT_UPLINK_TX_BUFFER, 0,
                                        nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kUartPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kUartPort, CONFIG_NATKIT_UPLINK_TX_GPIO,
                                 CONFIG_NATKIT_UPLINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  }

  sQueue = xQueueCreate(CONFIG_NATKIT_UPLINK_QUEUE_DEPTH, sizeof(TxFrame));
  sWireLock = xSemaphoreCreateMutex();
  if (sQueue == nullptr || sWireLock == nullptr) {
    ESP_LOGE(kTag, "could not create the uplink queue (%u x %u bytes)",
             (unsigned)CONFIG_NATKIT_UPLINK_QUEUE_DEPTH, (unsigned)sizeof(TxFrame));
    return ESP_ERR_NO_MEM;
  }

  if (kOnConsole) {
    // ⚠️ WITHOUT THIS, EVERY BINARY FRAME IS CORRUPTED, and it took a flash cycle
    // to find. `CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF` is on by default, so the
    // VFS writer expands every 0x0A byte written to stdout into 0x0D 0x0A. That
    // is invisible for log text and fatal for binary: a 524-byte data frame
    // almost always contains a newline byte and never survives, while a
    // 106-byte status frame often contains none and gets through. The symptom is
    // therefore not "nothing works" but "only the small frames work", which
    // reads like a length bug rather than a translation one.
    //
    // Only affects the console path; the real uplink writes through
    // uart_write_bytes and never touches the VFS.
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          ESP_LINE_ENDINGS_LF);
    sPreviousVprintf = esp_log_set_vprintf(lockedVprintf);
  }

  xTaskCreate(drainTask, "natkit-uplink", 4096, nullptr, 6, nullptr);

  // The budget, printed rather than assumed. natVR stalled its pipeline by
  // running ~19 KB/s of frames into an 11.5 KB/s console UART, and the arithmetic
  // that would have caught it is one line long.
  const uint32_t per_node_bytes_per_s =
      (kUplinkHeaderSize + kFrameHeaderSize +
       CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME * kSampleSize + kUplinkCrcSize) *
      1000000UL / (CONFIG_NATKIT_IMU_SAMPLE_INTERVAL_US *
                   CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME);
  // In console-shared mode our configured baud is not the one in force -- the
  // console's is, because we never reconfigure UART0. Quoting the wrong one here
  // would make the budget below a fiction.
  //
  // ⚠️ CONFIG_ESP_CONSOLE_UART_BAUDRATE DOES NOT EXIST on a target whose console
  // is the native USB Serial/JTAG (the ESP32-S3 board is one), so it cannot be
  // read unconditionally -- referencing it there is a build failure, not a zero.
#ifdef CONFIG_ESP_CONSOLE_UART_BAUDRATE
  const uint32_t console_baud = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
#else
  // USB Serial/JTAG: not a baud-limited link in any meaningful sense, so there
  // is no budget to compute. Reported as 0 and handled below rather than
  // invented, because a made-up ceiling is worse than an absent one.
  const uint32_t console_baud = 0;
#endif
  const uint32_t effective_baud =
      kOnConsole ? console_baud : CONFIG_NATKIT_UPLINK_BAUD;
  const uint32_t wire_bytes_per_s = effective_baud / 10;  // 8N1
  ESP_LOGI(kTag,
           "uplink on UART%d at %d baud%s (tx gpio %d): ~%lu B/s per node against "
           "~%lu B/s on the wire, so ~%lu nodes before the link is the limit. "
           "Queue %d x %u B, full = drop OLDEST.",
           CONFIG_NATKIT_UPLINK_UART_NUM, effective_baud,
           kOnConsole ? " SHARED WITH THE CONSOLE (bring-up mode)" : "",
           kOnConsole ? -1 : CONFIG_NATKIT_UPLINK_TX_GPIO,
           static_cast<unsigned long>(per_node_bytes_per_s),
           static_cast<unsigned long>(wire_bytes_per_s),
           static_cast<unsigned long>(per_node_bytes_per_s == 0
                                          ? 0
                                          : wire_bytes_per_s / per_node_bytes_per_s),
           CONFIG_NATKIT_UPLINK_QUEUE_DEPTH, (unsigned)sizeof(TxFrame));
  return ESP_OK;
}

bool uplinkSend(UplinkType type, uint64_t stream_id, const void *payload,
                size_t payload_size) {
  if (sQueue == nullptr) {
    return false;
  }
  if (payload_size > kUplinkMaxPayload) {
    ++sStats.oversize_rejected;
    return false;
  }

  TxFrame frame{};
  uint8_t *p = frame.bytes;
  *p++ = kUplinkMagic0;
  *p++ = kUplinkMagic1;
  *p++ = kUplinkVersion;
  *p++ = static_cast<uint8_t>(type);
  p += writeLe<uint64_t>(p, stream_id);
  p += writeLe<uint32_t>(p, ++sSequence);
  p += writeLe<uint16_t>(p, static_cast<uint16_t>(payload_size));
  if (payload != nullptr && payload_size > 0) {
    std::memcpy(p, payload, payload_size);
    p += payload_size;
  }
  // esp_crc32_le rather than a hand-rolled table: it is in ROM, it is the same
  // polynomial the host-side zlib crc32 uses, and a reader written in Python can
  // check it with one call from the standard library.
  const uint32_t crc =
      esp_crc32_le(0, frame.bytes, static_cast<uint32_t>(p - frame.bytes));
  p += writeLe<uint32_t>(p, crc);
  frame.length = static_cast<size_t>(p - frame.bytes);

  ++sStats.frames_queued;

  const UBaseType_t waiting = uxQueueMessagesWaiting(sQueue);
  if (waiting > sStats.queue_high_water) {
    sStats.queue_high_water = static_cast<uint32_t>(waiting);
  }

  if (xQueueSend(sQueue, &frame, 0) == pdTRUE) {
    return true;
  }

  // Full. Drop the OLDEST, same policy and same reasoning as the leaf's radio
  // queue: for a sensor stream the freshest frame is the valuable one, and the
  // gap is detectable on the far side from both sequence numbers in the header.
  TxFrame discarded{};
  if (xQueueReceive(sQueue, &discarded, 0) == pdTRUE) {
    ++sStats.frames_dropped;
  }
  if (xQueueSend(sQueue, &frame, 0) != pdTRUE) {
    ++sStats.frames_dropped;
  }
  return true;
}

const UplinkStats &uplinkStats() { return sStats; }

}  // namespace natkit
