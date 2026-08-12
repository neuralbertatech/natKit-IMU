#include "espnow_probe.hpp"

#include <atomic>
#include <cinttypes>
#include <cstring>

#include "device_id.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// Bench tool for TEC-NATKIT-23: measure what ESP-NOW actually gives us on the
// chips we have, rather than designing the on-air frame format around the 250
// bytes the documentation leads with.
//
// It is a build-time alternative to the node roles (CONFIG_NATKIT_ESPNOW_PROBE)
// rather than a fourth role, because it is not part of the architecture -- it is
// the instrument used to decide part of it, and it doubles as the sender/receiver
// harness for the multi-node loss run.

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-espnow";

constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The real frame this epic has to carry: NatImuBulkDataSchema Binary is
// 24 + 50 * sampleCount, and the running firmware publishes 10 samples, so 524
// bytes is not a guess -- it is what the node puts on the wire today.
constexpr size_t kRealFrameBytes = 524;

// Atomics rather than volatile: these are written from the WiFi task's callbacks
// and read from the probe task, which is a genuine cross-task handoff. (volatile
// would also warn -- ++ on a volatile is deprecated in C++20 -- but the reason to
// change it is correctness, not the warning.)
std::atomic<uint32_t> sSendOk{0};
std::atomic<uint32_t> sSendFail{0};
std::atomic<uint32_t> sRecvPackets{0};
std::atomic<uint32_t> sRecvBytes{0};
std::atomic<uint32_t> sRecvSeqGaps{0};
std::atomic<uint32_t> sRecvLastSeq{0};
std::atomic<bool> sRecvSeen{false};
// A sequence that goes BACKWARDS means the sender rebooted (or a second sender is
// on the channel), not that packets were lost. Counted rather than ignored,
// because a silent restart otherwise reads as one clean stream and quietly makes
// the packet total the sum of two runs -- which is exactly how the first loss run
// produced 243 received against 219 sent.
std::atomic<uint32_t> sRecvRestarts{0};

// ONE monotonic sequence for the whole probe run, advanced ONLY when
// esp_now_send accepts the frame.
//
// This is load-bearing for the measurement, not bookkeeping. Three earlier
// mistakes all corrupted the receiver's loss figure:
//   - measureSendRate used the loop index, so each run restarted at 0 and the
//     runs could not be told apart;
//   - the index advanced even when esp_now_send REJECTED the frame, so flat out
//     (340 of 500 refused at the API) the receiver saw seq jump 12 -> 393 and
//     counted ~380 "gaps" for frames that were never transmitted -- reporting
//     back-pressure as packet loss, in the one run where loss is the question;
//   - the payload sweep wrote the payload SIZE into the sequence slot, injecting
//     values like 1470 into the receiver's arithmetic.
// With the sequence advanced only on acceptance, a gap on the receiving side is
// an on-air loss and nothing else.
uint32_t sNextSeq = 0;

void sendCallback(const wifi_tx_info_t *, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    ++sSendOk;
  } else {
    ++sSendFail;
  }
}

// Every probe packet starts with a 4-byte little-endian sequence number, which
// is what makes loss measurable on the receiving side rather than merely
// "fewer packets than expected, probably".
void recvCallback(const esp_now_recv_info_t *info, const uint8_t *data,
                  int len) {
  (void)info;
  ++sRecvPackets;
  sRecvBytes.fetch_add(static_cast<uint32_t>(len));

  if (len >= 4) {
    uint32_t seq = 0;
    memcpy(&seq, data, sizeof(seq));
    const uint32_t last = sRecvLastSeq.load();
    if (sRecvSeen.load()) {
      if (seq > last + 1) {
        sRecvSeqGaps.fetch_add(seq - last - 1);
      } else if (seq <= last) {
        // Backwards or repeated: a sender restart, not a loss. Counted so the
        // packet total is readable as "one run" or "several".
        ++sRecvRestarts;
      }
    }
    sRecvLastSeq.store(seq);
    sRecvSeen.store(true);
  }
}

// Sends one frame carrying the next sequence number, consuming that number only
// if the API accepted the frame. Returns true when accepted.
bool sendSequenced(uint8_t *buffer, size_t frame_bytes) {
  memcpy(buffer, &sNextSeq, sizeof(sNextSeq));
  if (esp_now_send(kBroadcast, buffer, frame_bytes) != ESP_OK) {
    return false;
  }
  ++sNextSeq;
  return true;
}

esp_err_t startRadio() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  // RAM storage: nothing here should persist a WiFi config to NVS, because a
  // leaf never associates and stale credentials on a node are exactly the class
  // of problem this architecture exists to remove.
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Fixed channel on both ends. Without this the two boards can sit on
  // different channels and every packet is "sent" while nothing is received,
  // which looks like 100% loss rather than a misconfiguration.
  ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_NATKIT_ESPNOW_CHANNEL,
                                       WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(sendCallback));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(recvCallback));

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, kBroadcast, sizeof(kBroadcast));
  peer.channel = CONFIG_NATKIT_ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));

  return ESP_OK;
}

void reportCapabilities() {
  uint32_t version = 0;
  const esp_err_t err = esp_now_get_version(&version);
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  uint8_t channel = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&channel, &second);

  ESP_LOGI(kTag, "target %s, device %" PRIu64, CONFIG_IDF_TARGET, deviceId());
  ESP_LOGI(kTag, "sta mac %02x:%02x:%02x:%02x:%02x:%02x, channel %u", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5], channel);
  ESP_LOGI(kTag,
           "esp_now_get_version -> %s, version %lu  (v1 limit %d, v2 limit %d)",
           esp_err_to_name(err), static_cast<unsigned long>(version),
           ESP_NOW_MAX_IE_DATA_LEN, ESP_NOW_MAX_DATA_LEN_V2);
  ESP_LOGI(kTag, "the frame this epic must carry is %u bytes (24 + 50 * 10)",
           static_cast<unsigned>(kRealFrameBytes));
}

// Sweep payload sizes and report, for each, whether esp_now_send ACCEPTED it and
// whether the radio then reported the transmission succeeded. Those are two
// different failures and conflating them is how "250 bytes" becomes folklore:
// a length the API rejects outright is a hard limit, while one it accepts and
// then fails to transmit is a channel or peer problem.
void sweepPayloadSizes() {
  static const size_t kSizes[] = {
      1, 200, 250, 251, 300, kRealFrameBytes, 1000, 1400, 1470, 1471, 1500};

  static uint8_t buffer[1600];
  memset(buffer, 0xA5, sizeof(buffer));

  ESP_LOGI(kTag, "--- payload sweep (broadcast, no peer required) ---");
  for (size_t size : kSizes) {
    if (size > sizeof(buffer)) {
      continue;
    }
    const uint32_t ok_before = sSendOk.load();
    const uint32_t fail_before = sSendFail.load();
    // Carries a real sequence number like every other packet. It used to write
    // the payload SIZE into that slot as "harmless", which it is not once a
    // receiver is listening: it injected values like 1470 into the receiver's gap
    // arithmetic and made the loss figure meaningless.
    memcpy(buffer, &sNextSeq, sizeof(sNextSeq));
    const esp_err_t err = esp_now_send(kBroadcast, buffer, size);
    if (err == ESP_OK) {
      ++sNextSeq;
    }
    // The send callback is asynchronous; give it a moment before reading.
    vTaskDelay(pdMS_TO_TICKS(50));

    const bool confirmed = sSendOk.load() > ok_before;
    const bool failed = sSendFail.load() > fail_before;
    ESP_LOGI(kTag, "%4u bytes: esp_now_send -> %-28s tx %s", (unsigned)size,
             esp_err_to_name(err),
             confirmed ? "confirmed" : (failed ? "FAILED" : "no callback"));
  }
}

// Sustained send at the rate the IMU actually produces, and then flat out, using
// the real 524-byte frame. The first number says "does the architecture work at
// our data rate"; the second says how much headroom there is for more nodes.
void measureSendRate(const char *label, size_t frame_bytes, int frames,
                     int delay_ms) {
  static uint8_t buffer[1600];
  memset(buffer, 0x5A, sizeof(buffer));

  const uint32_t ok_before = sSendOk.load();
  const uint32_t fail_before = sSendFail.load();
  const int64_t start = esp_timer_get_time();
  int accepted = 0;
  int rejected = 0;

  for (int i = 0; i < frames; ++i) {
    if (sendSequenced(buffer, frame_bytes)) {
      ++accepted;
    } else {
      ++rejected;
      // ESP_ERR_ESPNOW_NO_MEM means the queue is full: back off rather than
      // spinning, since a tight retry loop measures the retry loop.
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
  }

  vTaskDelay(pdMS_TO_TICKS(200));  // let the last callbacks land
  const int64_t elapsed_us = esp_timer_get_time() - start;
  const float seconds = static_cast<float>(elapsed_us) / 1'000'000.0F;
  const uint32_t confirmed = sSendOk.load() - ok_before;
  const uint32_t failed = sSendFail.load() - fail_before;

  ESP_LOGI(kTag,
           "%s: %d x %u B in %.2fs -> accepted %d, rejected %d, tx confirmed "
           "%lu, tx failed %lu (%.1f frames/s, %.1f KB/s)",
           label, frames, static_cast<unsigned>(frame_bytes), seconds, accepted,
           rejected, static_cast<unsigned long>(confirmed),
           static_cast<unsigned long>(failed),
           seconds > 0 ? static_cast<float>(confirmed) / seconds : 0.0F,
           seconds > 0
               ? static_cast<float>(confirmed) * frame_bytes / seconds / 1024.0F
               : 0.0F);
}

}  // namespace

void runEspNowProbe() {
  ESP_ERROR_CHECK(startRadio());
  reportCapabilities();

#if CONFIG_NATKIT_ESPNOW_PROBE_RECEIVER
  ESP_LOGI(kTag, "--- receiver: counting packets and sequence gaps ---");
  uint32_t last_packets = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t packets = sRecvPackets.load();
    ESP_LOGI(kTag,
             "recv %lu packets (+%lu/s), %lu bytes, seq gaps %lu, sender "
             "restarts %lu, last seq %lu",
             static_cast<unsigned long>(packets),
             static_cast<unsigned long>(packets - last_packets),
             static_cast<unsigned long>(sRecvBytes.load()),
             static_cast<unsigned long>(sRecvSeqGaps.load()),
             static_cast<unsigned long>(sRecvRestarts.load()),
             static_cast<unsigned long>(sRecvLastSeq.load()));
    last_packets = packets;
  }
#else
  sweepPayloadSizes();

  ESP_LOGI(kTag, "--- send rate ---");
  // 50 frames at 200ms = the real 5 frames/s the IMU produces, over 10s.
  measureSendRate("at IMU rate", kRealFrameBytes, 50, 200);
  // Flat out: how much headroom exists for more nodes on one channel.
  measureSendRate("flat out", kRealFrameBytes, 500, 0);

  // The sequence counter IS the expected receive count, which is what makes loss
  // arithmetic on the other board a subtraction rather than an inference: the
  // receiver should report exactly this many packets, a last seq of one less, and
  // zero gaps.
  ESP_LOGI(kTag,
           "probe done: %lu frames transmitted (seq 0..%lu), tx confirmed %lu, "
           "tx failed %lu. A receiver should report %lu packets, last seq %lu, "
           "0 gaps.",
           static_cast<unsigned long>(sNextSeq),
           static_cast<unsigned long>(sNextSeq > 0 ? sNextSeq - 1 : 0),
           static_cast<unsigned long>(sSendOk.load()),
           static_cast<unsigned long>(sSendFail.load()),
           static_cast<unsigned long>(sNextSeq),
           static_cast<unsigned long>(sNextSeq > 0 ? sNextSeq - 1 : 0));
  ESP_LOGI(kTag,
           "idling. Flash a second board with "
           "CONFIG_NATKIT_ESPNOW_PROBE_RECEIVER=y (./build-role.sh "
           "espnow-probe-receiver esp32) to measure loss.");
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(kTag, "idle: tx ok %lu, tx failed %lu, rx %lu",
             static_cast<unsigned long>(sSendOk.load()),
             static_cast<unsigned long>(sSendFail.load()),
             static_cast<unsigned long>(sRecvPackets.load()));
  }
#endif
}

}  // namespace natkit
