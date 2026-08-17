#include "uplink_reader.hpp"

#include <cstring>

#include "driver/uart.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-gwrx";

constexpr uart_port_t kUartPort =
    static_cast<uart_port_t>(CONFIG_NATKIT_UPLINK_UART_NUM);
constexpr bool kOnConsole = CONFIG_NATKIT_UPLINK_UART_NUM == 0;

#ifdef CONFIG_ESP_CONSOLE_UART_BAUDRATE
constexpr int kConsoleBaud = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
#else
constexpr int kConsoleBaud = 0;  // USB Serial/JTAG: not a baud-limited link
#endif

// Room for a few maximum-size frames plus whatever garbage sits between them.
// Sized from the frame constants rather than a round number, so a change to
// samples-per-frame cannot silently make this too small to hold one frame.
constexpr size_t kScanBuffer = kUplinkMaxFrame * 4;

UplinkReaderStats sStats{};
UplinkFrameHandler sHandler = nullptr;

uint8_t sBuffer[kScanBuffer];
size_t sFilled = 0;

template <typename T>
T readLe(const uint8_t *p) {
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(p[i]) << (8 * i);
  }
  return value;
}

// Tries to validate a frame starting at sBuffer[at].
//
// Returns the total frame length on success, 0 when this is definitely not a
// frame, and -1 when it might be one but there are not enough bytes yet -- a
// distinction the caller needs, because "wait for more" and "skip a byte" are
// opposite actions and confusing them either stalls the reader or eats frames.
int tryFrame(size_t at, size_t available) {
  if (available < kUplinkHeaderSize + kUplinkCrcSize) {
    return -1;
  }
  const uint8_t *p = sBuffer + at;
  if (p[0] != kUplinkMagic0 || p[1] != kUplinkMagic1) {
    return 0;
  }
  const uint16_t length = readLe<uint16_t>(p + 16);
  if (length > kUplinkMaxPayload) {
    return 0;  // implausible: a corrupt length, not a long frame
  }
  const size_t total = kUplinkHeaderSize + length + kUplinkCrcSize;
  if (available < total) {
    return -1;
  }

  const uint32_t want = readLe<uint32_t>(p + kUplinkHeaderSize + length);
  const uint32_t got =
      esp_crc32_le(0, p, static_cast<uint32_t>(kUplinkHeaderSize + length));
  if (want != got) {
    ++sStats.crc_failures;
    return 0;
  }
  if (p[2] != kUplinkVersion) {
    ++sStats.version_mismatches;
    return 0;
  }
  return static_cast<int>(total);
}

void dispatch(size_t at) {
  const uint8_t *p = sBuffer + at;
  const auto type = static_cast<UplinkType>(p[3]);
  const uint64_t stream_id = readLe<uint64_t>(p + 4);
  const uint32_t seq = readLe<uint32_t>(p + 12);
  const uint16_t length = readLe<uint16_t>(p + 16);

  ++sStats.frames_ok;
  switch (type) {
    case UplinkType::kData:
      ++sStats.frames_data;
      break;
    case UplinkType::kNodeStatus:
      ++sStats.frames_node_status;
      break;
    case UplinkType::kPrimaryStatus:
      ++sStats.frames_primary_status;
      break;
    case UplinkType::kCommandLog:
      ++sStats.frames_command_log;
      break;
  }

  // The uplink's own sequence, which answers a different question from the radio
  // sequence inside a data payload: this one says the PRIMARY dropped a frame or
  // the wire ate it, that one says a node's frame never reached the primary.
  if (sStats.seq_seen && seq > sStats.last_seq + 1) {
    sStats.uplink_seq_gaps += seq - sStats.last_seq - 1;
  }
  sStats.last_seq = seq;
  sStats.seq_seen = true;

  if (sHandler != nullptr) {
    sHandler(type, stream_id, p + kUplinkHeaderSize, length);
  }
}

void readerTask(void *) {
  while (true) {
    // Read into whatever room is left. A full buffer with no frame in it means
    // we are looking at garbage, which the compaction below discards.
    size_t room = sizeof(sBuffer) - sFilled;
    if (room == 0) {
      // Nothing valid in a whole buffer: drop all but the last few bytes, since
      // a frame could still straddle the boundary. Counted as skipped, because
      // silently discarding is how a reader claims a clean stream it never saw.
      const size_t keep = kUplinkHeaderSize;
      std::memmove(sBuffer, sBuffer + sFilled - keep, keep);
      sStats.bytes_skipped += sFilled - keep;
      sFilled = keep;
      room = sizeof(sBuffer) - sFilled;
    }

    // A SHORT timeout, because `room` is most of the scan buffer and therefore
    // almost never fills -- so this call returns on the timeout nearly every
    // time, and the timeout IS the batching interval. At 50 ms the reader
    // collected several frames and published them back to back, which showed up
    // at the broker as bursts 3 ms apart separated by long gaps. 5 ms keeps the
    // task cheap (it still blocks rather than spinning) while forwarding a frame
    // about as soon as it lands.
    const int read = uart_read_bytes(kUartPort, sBuffer + sFilled, room,
                                     pdMS_TO_TICKS(5));
    if (read > 0) {
      sFilled += static_cast<size_t>(read);
      sStats.bytes_read += static_cast<uint64_t>(read);
    }

    size_t at = 0;
    while (at < sFilled) {
      const int total = tryFrame(at, sFilled - at);
      if (total > 0) {
        dispatch(at);
        at += static_cast<size_t>(total);
        continue;
      }
      if (total < 0) {
        break;  // might be a frame; wait for more bytes rather than eating it
      }
      // Definitely not a frame here. Advance exactly ONE byte: a corrupt length
      // field is the case where trusting it walks past the next good frame.
      ++at;
      ++sStats.bytes_skipped;
    }

    if (at > 0) {
      std::memmove(sBuffer, sBuffer + at, sFilled - at);
      sFilled -= at;
    }
  }
}

}  // namespace

esp_err_t uplinkReaderStart(UplinkFrameHandler handler) {
  sHandler = handler;

  uart_config_t cfg{};
  cfg.baud_rate = CONFIG_NATKIT_UPLINK_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  // A generous RX buffer: the reader task competes with the network stack, and
  // an overrun here is a lost frame that no counter upstream can attribute.
  ESP_ERROR_CHECK(uart_driver_install(kUartPort, kScanBuffer * 2, 0, 0, nullptr,
                                      0));
  if (!kOnConsole) {
    ESP_ERROR_CHECK(uart_param_config(kUartPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kUartPort, CONFIG_NATKIT_UPLINK_TX_GPIO,
                                 CONFIG_NATKIT_UPLINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  }

  xTaskCreate(readerTask, "natkit-gwrx", 4096, nullptr, 6, nullptr);
  ESP_LOGI(kTag,
           "reading the primary's framed stream on UART%d at %d baud%s; "
           "resynchronising by magic + CRC, one byte at a time",
           CONFIG_NATKIT_UPLINK_UART_NUM,
           // ⚠️ CONFIG_ESP_CONSOLE_UART_BAUDRATE does not exist on a target whose
           // console is the native USB Serial/JTAG (the ESP32-S3 board), so it
           // cannot be referenced unconditionally -- it is a build failure there
           // rather than a zero.
           kOnConsole ? kConsoleBaud : CONFIG_NATKIT_UPLINK_BAUD,
           kOnConsole ? " (SHARED WITH THE CONSOLE, bring-up mode)" : "");
  return ESP_OK;
}

const UplinkReaderStats &uplinkReaderStats() { return sStats; }

}  // namespace natkit
