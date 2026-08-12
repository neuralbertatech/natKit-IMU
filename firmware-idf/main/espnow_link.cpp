#include "espnow_link.hpp"

#include <cinttypes>
#include <cstring>

#include "device_id.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "imu_frame.hpp"
#include "sdkconfig.h"
#include "time_sync.hpp"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-link";

constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The largest payload we ever queue is a full canonical frame; the envelope rides
// on top. Sized from the frame constants rather than a round number so a change to
// samples-per-frame cannot silently overflow the queue item.
constexpr size_t kMaxPayload =
    kFrameHeaderSize + kMaxSamplesPerFrame * kSampleSize;
constexpr size_t kMaxPacket = kEnvelopeSize + kMaxPayload;

static_assert(kMaxPacket <= 1470,
              "ESP-NOW v2 on this chip refuses anything over 1470 bytes -- "
              "measured, not read off a header (TEC-NATKIT-23)");

struct TxItem {
  size_t length;
  uint8_t bytes[kMaxPacket];
};

QueueHandle_t sTxQueue = nullptr;
SemaphoreHandle_t sSendDone = nullptr;
LinkStats sStats{};

// Set by the send callback, read by the transmit task after it takes sSendDone.
volatile esp_now_send_status_t sLastSendStatus = ESP_NOW_SEND_SUCCESS;

uint8_t sPrimaryMac[6] = {};
volatile bool sPrimaryKnown = false;

// The leaf's clock as read inside its own send callback, which is the closest to
// on-air this API gets. Written here and read by txTask after it takes sSendDone,
// so the probe's follow-up can carry a transmit stamp rather than an enqueue one.
volatile uint64_t sLastTxUs = 0;

void sendCallback(const wifi_tx_info_t *, esp_now_send_status_t status) {
  sLastTxUs = static_cast<uint64_t>(esp_timer_get_time());
  sLastSendStatus = status;
  if (sSendDone != nullptr) {
    xSemaphoreGive(sSendDone);
  }
}

// Learns the primary from any well-formed packet it sends us, and adds it as a
// unicast peer.
//
// Discovery rather than a compile-time MAC, because a hand-configured peer MAC is
// a per-board build -- the thing this epic is trying to get away from -- and
// because the primary has to end up with a registry of leaf MACs anyway, so the
// pairing conversation has to exist in some form. This is the minimum version of
// it: the leaf broadcasts an announce until a primary answers.
void learnPrimary(const uint8_t *mac) {
  if (sPrimaryKnown && std::memcmp(sPrimaryMac, mac, 6) == 0) {
    return;
  }

  esp_now_peer_info_t peer{};
  std::memcpy(peer.peer_addr, mac, 6);
  peer.channel = CONFIG_NATKIT_ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  const esp_err_t err =
      esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "could not add primary as a peer: %s", esp_err_to_name(err));
    return;
  }

  std::memcpy(sPrimaryMac, mac, 6);
  std::memcpy(sStats.primary_mac, mac, 6);
  sPrimaryKnown = true;
  sStats.primary_known = true;
  ESP_LOGI(kTag, "primary is %02x:%02x:%02x:%02x:%02x:%02x -- data path is now unicast",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void leafRecvCallback(const esp_now_recv_info_t *info, const uint8_t *data,
                      int len) {
  if (info == nullptr || data == nullptr || len < static_cast<int>(kEnvelopeSize)) {
    return;
  }
  if (data[0] != kEspNowMagic0 || data[1] != kEspNowMagic1) {
    return;  // not ours; a shared channel carries other people's broadcasts
  }
  if (data[2] != kEspNowProtocolVersion) {
    ESP_LOGW(kTag, "ignoring protocol version %u (we speak %u)", data[2],
             kEspNowProtocolVersion);
    return;
  }

  // The receive time is taken FIRST, before any of the work below, because every
  // line of it is latency folded straight into the offset estimate.
  const uint64_t rx_local_us = static_cast<uint64_t>(esp_timer_get_time());
  const uint32_t rx_mac_us =
      info->rx_ctrl != nullptr ? static_cast<uint32_t>(info->rx_ctrl->timestamp)
                               : 0;

  const uint8_t *payload = data + kEnvelopeSize;
  const size_t payload_size = static_cast<size_t>(len) - kEnvelopeSize;

  switch (static_cast<PacketType>(data[3])) {
    case PacketType::kTimeBeacon: {
      // Discovery rides on the timing broadcast rather than on a packet of its
      // own: it is the same 1 Hz broadcast from the same device, and two of them
      // would be two things to keep in step.
      learnPrimary(info->src_addr);
      if (payload_size >= sizeof(TimeBeacon)) {
        TimeBeacon beacon{};
        std::memcpy(&beacon, payload, sizeof(beacon));
        timeSyncOnBeacon(beacon, rx_local_us, rx_mac_us);
      }
      break;
    }
    case PacketType::kTimeFollowUp: {
      if (payload_size >= sizeof(TimeFollowUp)) {
        TimeFollowUp follow_up{};
        std::memcpy(&follow_up, payload, sizeof(follow_up));
        timeSyncOnFollowUp(follow_up);
      }
      break;
    }
    default:
      break;
  }
}

// Sends one packet and waits for its transmit callback, retrying with backoff.
//
// Retry belongs here rather than in the caller because "accepted by the API" and
// "confirmed on air" are different events: esp_now_send returning ESP_OK only
// means the frame was queued in the WiFi driver. Both failures are counted
// separately for the same reason.
bool transmit(const TxItem &item) {
  const uint8_t *target = sPrimaryKnown ? sPrimaryMac : kBroadcast;

  // Announces are the exception: they are how a primary gets discovered in the
  // first place, so they always go out broadcast.
  if (item.length >= kEnvelopeSize &&
      item.bytes[3] == static_cast<uint8_t>(PacketType::kAnnounce)) {
    target = kBroadcast;
  }

  // Retry hard while the primary is believed present, ONCE while it is not.
  //
  // Measured reason, not a guess: through a 30-second outage the three-attempt path
  // burned 322 retries -- each one an on-air transmission plus up to 50ms of
  // waiting, roughly 16 seconds of radio time in a 30-second window. That is
  // airtime other nodes need, spent on a peer that is known to be gone. Retrying is
  // right for a transient failure and pointless for an absent hub, and the
  // difference between those is exactly what a run of consecutive failures tells
  // you.
  //
  // It also puts the bounded queue back in charge of what happens during an outage:
  // with three slow attempts per packet, the transmit task consumed frames at
  // almost exactly the rate the sample loop produced them, so the queue never
  // filled and its drop policy never ran.
  const int max_attempts = sStats.primary_absent ? 1 : 3;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (attempt > 0) {
      ++sStats.send_retries;
      // Backoff, doubling: 4ms, 8ms. A tight retry loop measures the retry loop
      // and starves the queue behind it.
      vTaskDelay(pdMS_TO_TICKS(4 << (attempt - 1)));
    }

    xSemaphoreTake(sSendDone, 0);  // clear any stale completion
    const esp_err_t err = esp_now_send(target, item.bytes, item.length);
    if (err != ESP_OK) {
      // ESP_ERR_ESPNOW_NO_MEM is the driver queue being full: that is
      // back-pressure, and retrying after a backoff is the right answer.
      continue;
    }

    if (xSemaphoreTake(sSendDone, pdMS_TO_TICKS(50)) != pdTRUE) {
      continue;  // no callback: treat as a failure and retry
    }
    if (sLastSendStatus == ESP_NOW_SEND_SUCCESS) {
      return true;
    }
  }
  return false;
}

// How many consecutive on-air failures mean "the hub is gone" rather than "the air
// was busy". Five at 5 frames/s is a second of silence, which is far longer than
// any contention this link sees and far shorter than a reboot.
constexpr uint32_t kAbsentAfterFailures = 5;

void txTask(void *) {
  TxItem item{};
  while (true) {
    if (xQueueReceive(sTxQueue, &item, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (transmit(item)) {
      ++sStats.packets_sent;
      sStats.consecutive_failures = 0;
      if (sStats.primary_absent) {
        sStats.primary_absent = false;
        ESP_LOGI(kTag, "primary is answering again; back to full retries");
      }
      // A probe that made it out is immediately followed by the stamp of when it
      // did. Sent from here, inline, rather than queued: everything ahead of it in
      // a queue would be latency between the probe and the description of it, and
      // the pair only means anything while the primary is still holding the probe.
      // transmit() is only ever called from this task, so calling it again here is
      // not a second owner of the radio.
      if (item.length >= kEnvelopeSize &&
          item.bytes[3] == static_cast<uint8_t>(PacketType::kTimeProbe)) {
        TimeProbe probe{};
        std::memcpy(&probe, item.bytes + kEnvelopeSize, sizeof(probe));

        TimeProbeFollowUp follow_up{};
        follow_up.device_id = probe.device_id;
        follow_up.seq = probe.seq;
        follow_up.epoch = probe.epoch;
        follow_up.tx_us = sLastTxUs;

        TxItem reply{};
        reply.bytes[0] = kEspNowMagic0;
        reply.bytes[1] = kEspNowMagic1;
        reply.bytes[2] = kEspNowProtocolVersion;
        reply.bytes[3] = static_cast<uint8_t>(PacketType::kTimeProbeFollowUp);
        std::memcpy(reply.bytes + kEnvelopeSize, &follow_up, sizeof(follow_up));
        reply.length = kEnvelopeSize + sizeof(follow_up);
        if (transmit(reply)) {
          ++sStats.packets_sent;
        } else {
          ++sStats.send_failures;
        }
      }
    } else {
      ++sStats.send_failures;
      ++sStats.consecutive_failures;
      if (!sStats.primary_absent &&
          sStats.consecutive_failures >= kAbsentAfterFailures) {
        sStats.primary_absent = true;
        // Logged once, on the transition. A line per failed frame would be the
        // loudest thing in the console for the whole outage and would say nothing
        // the counters do not.
        ESP_LOGW(kTag,
                 "primary has not answered %lu times: presuming it is gone, "
                 "sending once per frame until it returns (the queue now decides "
                 "what to drop)",
                 static_cast<unsigned long>(sStats.consecutive_failures));
      }
    }
  }
}

// Broadcasts an announce until a primary answers.
//
// It keeps announcing after that too, at a slower cadence, because the primary can
// reboot and forget its registry while the leaf is still happily unicasting into a
// void -- a leaf that only announces once is undiscoverable for the rest of its
// uptime.
void announceTask(void *) {
  while (true) {
    Announce announce{};
    announce.device_id = deviceId();
    announce.sample_rate_hz = CONFIG_NATKIT_IMU_DECLARED_RATE_HZ;
    announce.samples_per_frame = CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME;
    announce.firmware_version = 1;

    espNowLinkSend(PacketType::kAnnounce, &announce, sizeof(announce));
    ++sStats.announces;

    vTaskDelay(pdMS_TO_TICKS(sPrimaryKnown ? 10000 : 1000));
  }
}

// The leaf's half of the timing conversation: says where its clock thinks it is,
// then asks the primary to check.
//
// Both go out on the same cadence and in this order on purpose. The primary can
// only score a probe against a fit it already holds, so a probe that overtook its
// SyncState would be scored against a stale one -- which would show up as sync
// error that is really just a late report of a good fit.
void syncTask(void *) {
  uint32_t seq = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(CONFIG_NATKIT_TIME_SYNC_PROBE_MS));
    if (!sPrimaryKnown) {
      continue;  // nothing to talk to, and probes are unicast
    }

    SyncState state{};
    timeSyncFillWire(state);
    state.device_id = deviceId();
    espNowLinkSend(PacketType::kSyncState, &state, sizeof(state));

    TimeProbe probe{};
    probe.device_id = deviceId();
    probe.seq = ++seq;
    probe.epoch = state.epoch;
    espNowLinkSend(PacketType::kTimeProbe, &probe, sizeof(probe));
  }
}

esp_err_t startRadio() {
  // No esp_netif_init() and no netif at all: ESP-NOW does not go through lwIP, so
  // a leaf never brings up a network interface. The event loop IS required --
  // esp_wifi_init posts to it.
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  // RAM storage: nothing here should persist a WiFi config to NVS. Stale
  // credentials on a node are exactly the class of problem this architecture
  // exists to remove.
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Power save off, and this is a timing requirement rather than a performance
  // one (#340). The IDF documents the MAC receive timestamp as "precise only if
  // modem sleep or light sleep is not enabled", and a radio that is asleep when a
  // beacon arrives adds its wake latency to the offset estimate. The default for
  // a station is WIFI_PS_MIN_MODEM, so leaving this unsaid would have meant
  // measuring the power-save state machine.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  // Fixed channel on both ends, and never esp_wifi_connect. A channel mismatch
  // presents as every packet sending successfully while nothing is received, which
  // reads as total loss rather than as a misconfiguration.
  ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_NATKIT_ESPNOW_CHANNEL,
                                       WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  return ESP_OK;
}

esp_err_t addBroadcastPeer() {
  esp_now_peer_info_t peer{};
  std::memcpy(peer.peer_addr, kBroadcast, 6);
  peer.channel = CONFIG_NATKIT_ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer);
}

}  // namespace

esp_err_t espNowLinkStart() {
  ESP_ERROR_CHECK(startRadio());

  sTxQueue = xQueueCreate(CONFIG_NATKIT_ESPNOW_TX_QUEUE_DEPTH, sizeof(TxItem));
  sSendDone = xSemaphoreCreateBinary();
  if (sTxQueue == nullptr || sSendDone == nullptr) {
    ESP_LOGE(kTag, "could not create the transmit queue (%u x %u bytes)",
             (unsigned)CONFIG_NATKIT_ESPNOW_TX_QUEUE_DEPTH, (unsigned)sizeof(TxItem));
    return ESP_ERR_NO_MEM;
  }

  ESP_ERROR_CHECK(esp_now_register_send_cb(sendCallback));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(leafRecvCallback));
  ESP_ERROR_CHECK(addBroadcastPeer());

  xTaskCreate(txTask, "natkit-tx", 4096, nullptr, 5, nullptr);
  xTaskCreate(announceTask, "natkit-announce", 3072, nullptr, 4, nullptr);
  xTaskCreate(syncTask, "natkit-sync", 3072, nullptr, 4, nullptr);

  ESP_LOGI(kTag,
           "ESP-NOW up on channel %d, no association, no netif. Queue depth %d "
           "(%u B/item). Announcing until a primary answers.",
           CONFIG_NATKIT_ESPNOW_CHANNEL, CONFIG_NATKIT_ESPNOW_TX_QUEUE_DEPTH,
           (unsigned)sizeof(TxItem));
  return ESP_OK;
}

bool espNowLinkSend(PacketType type, const void *payload, size_t payload_size) {
  if (sTxQueue == nullptr || payload_size > kMaxPayload) {
    return false;
  }

  TxItem item{};
  item.bytes[0] = kEspNowMagic0;
  item.bytes[1] = kEspNowMagic1;
  item.bytes[2] = kEspNowProtocolVersion;
  item.bytes[3] = static_cast<uint8_t>(type);
  if (payload != nullptr && payload_size > 0) {
    std::memcpy(item.bytes + kEnvelopeSize, payload, payload_size);
  }
  item.length = kEnvelopeSize + payload_size;

  ++sStats.packets_queued;

  // Non-blocking by construction. When the queue is full, drop the OLDEST item to
  // make room: for a sensor stream the freshest frame is the valuable one, and the
  // gap is detectable on the far side from seqNo. The alternative -- blocking the
  // caller -- would stall the sample loop, which is precisely what a leaf must
  // never do while the primary is away.
  if (xQueueSend(sTxQueue, &item, 0) == pdTRUE) {
    return true;
  }

  TxItem discarded{};
  if (xQueueReceive(sTxQueue, &discarded, 0) == pdTRUE) {
    ++sStats.packets_dropped;
  }
  if (xQueueSend(sTxQueue, &item, 0) != pdTRUE) {
    // Losing the race for the slot we just freed means another producer took it.
    // Drop ours rather than retrying: this function's contract is that it does not
    // block.
    ++sStats.packets_dropped;
    return true;
  }
  return true;
}

const LinkStats &espNowLinkStats() { return sStats; }

bool espNowLinkHasPrimary() { return sPrimaryKnown; }

// --- Primary side ----------------------------------------------------------

namespace {

NodeState sNodes[kMaxTrackedNodes];
uint32_t sUnknownPackets = 0;

NodeState *nodeFor(const uint8_t *mac) {
  for (NodeState &node : sNodes) {
    if (node.in_use && std::memcmp(node.mac, mac, 6) == 0) {
      return &node;
    }
  }
  for (NodeState &node : sNodes) {
    if (!node.in_use) {
      node = NodeState{};
      node.in_use = true;
      std::memcpy(node.mac, mac, 6);
      // The id is DERIVED from the MAC rather than taken from the announce, so it
      // is known from the very first packet -- including a data frame that arrives
      // before any announce. The announce's copy is then a cross-check.
      node.device_id = packMac(mac);
      return &node;
    }
  }
  return nullptr;  // more leaves than this scaffold tracks; counted by the caller
}

// Shifts one frame's device timestamp into our clock, and keeps the evidence.
//
// The deltas either side of the shift are the slow instrument: uncorrected, the
// gap between when a frame was sampled and when it arrived must WALK as the two
// crystals diverge; corrected, it must sit still. Both are kept because the
// walk-rate of the raw one is a second, independent estimate of the skew the
// leaf's regression reports, and this epic has now been bitten three times by a
// single counter that turned out to be measuring something else.
//
// Resolution caveat, stated because it bounds what these two numbers can show:
// the frame's timestamp is quantised to MILLISECONDS by the encoder
// (`sample.time_ms = newest_us / 1000`), so per-frame these carry about a
// millisecond of quantisation noise. That is fine for a drift of tens of
// milliseconds and useless for judging a correction good to microseconds -- which
// is what the probe exists to measure instead.
void applyTimeShift(NodeState &node, uint64_t device_ts_us, uint64_t arrival_us) {
  if (device_ts_us == 0) {
    return;
  }

  const int64_t raw_delta =
      static_cast<int64_t>(arrival_us) - static_cast<int64_t>(device_ts_us);

  uint64_t shifted = 0;
  const bool shifted_ok =
      node.sync_seen && syncStateToPrimary(node.last_sync, device_ts_us, shifted);
  if (!shifted_ok) {
    ++node.shift_failures;
  }
  const int64_t shifted_delta =
      shifted_ok ? static_cast<int64_t>(arrival_us) - static_cast<int64_t>(shifted)
                 : 0;

  node.raw_delta_us = raw_delta;
  node.shift_valid = shifted_ok;
  node.shifted_delta_us = shifted_delta;

  if (!node.delta_seen) {
    node.delta_seen = true;
    node.first_raw_delta_us = raw_delta;
    node.raw_delta_min = raw_delta;
    node.raw_delta_max = raw_delta;
  }
  if (raw_delta < node.raw_delta_min) {
    node.raw_delta_min = raw_delta;
  }
  if (raw_delta > node.raw_delta_max) {
    node.raw_delta_max = raw_delta;
  }

  if (shifted_ok) {
    // The corrected series starts at the first frame we could actually correct,
    // not at the first frame: seeding it from an uncorrected value would put a
    // whole boot's worth of offset into its range and make a flat line look like
    // a wild one. Tracked with its own flag rather than by testing the values
    // against 0 -- a genuine first delta of exactly zero would re-seed the range
    // on every frame and the spread would read 0 forever.
    if (!node.shifted_delta_seen) {
      node.shifted_delta_seen = true;
      node.first_shifted_delta_us = shifted_delta;
      node.shifted_delta_min = shifted_delta;
      node.shifted_delta_max = shifted_delta;
    }
    if (shifted_delta < node.shifted_delta_min) {
      node.shifted_delta_min = shifted_delta;
    }
    if (shifted_delta > node.shifted_delta_max) {
      node.shifted_delta_max = shifted_delta;
    }
  }
}

// Scores a probe pair: what we measured against what the leaf's fit predicted.
void scoreProbe(NodeState &node, const TimeProbeFollowUp &follow_up) {
  if (!node.probe_pending || node.probe_pending_seq != follow_up.seq) {
    ++node.probes_orphaned;
    node.probe_pending = false;
    return;
  }
  node.probe_pending = false;
  ++node.probes_paired;

  if (!node.sync_seen || follow_up.tx_us == 0) {
    ++node.probes_unpredictable;
    return;
  }
  // A fit against a different epoch is a fit against a clock origin we no longer
  // have. Scoring against it would report our own reboot as sync error.
  if (node.last_sync.epoch != espNowPrimaryEpoch()) {
    ++node.probes_unpredictable;
    return;
  }

  uint64_t predicted = 0;
  if (!syncStateToPrimary(node.last_sync, follow_up.tx_us, predicted)) {
    ++node.probes_unpredictable;
    return;
  }

  // Measured minus predicted. The probe left the leaf at tx_us on ITS clock and
  // arrived here at probe_arrival_us on OURS; if the fit were perfect and the
  // radio instantaneous these would be the same instant.
  const int64_t error = static_cast<int64_t>(node.probe_arrival_us) -
                        static_cast<int64_t>(predicted);

  // An error of more than ten seconds is not a clock estimate that drifted, it is
  // a fit against the wrong epoch or a torn read. Kept out of the running sums
  // rather than clamped: one such value would dominate a sum of squares
  // permanently, and an accuracy figure that a single bad sample can set is not
  // an accuracy figure. Counted where it will be seen.
  constexpr int64_t kAbsurdErrorUs = 10'000'000;
  if (error > kAbsurdErrorUs || error < -kAbsurdErrorUs) {
    ++node.probes_unpredictable;
    return;
  }

  // Score the same probe against "sync once and never again". The offset is
  // latched from the first usable probe -- measured here rather than taken from
  // the leaf's fit, so the naive model gets a fair starting point rather than a
  // handicapped one.
  const int64_t measured_offset = static_cast<int64_t>(node.probe_arrival_us) -
                                  static_cast<int64_t>(follow_up.tx_us);
  if (!node.naive_seen) {
    node.naive_seen = true;
    node.naive_offset_us = measured_offset;
  }
  node.naive_error_us = measured_offset - node.naive_offset_us;
  const int64_t naive_magnitude =
      node.naive_error_us < 0 ? -node.naive_error_us : node.naive_error_us;
  if (naive_magnitude > node.naive_error_worst_us) {
    node.naive_error_worst_us = naive_magnitude;
  }

  node.probe_error_us = error;
  if (!node.probe_error_seen) {
    node.probe_error_seen = true;
    node.probe_error_min_us = error;
    node.probe_error_max_us = error;
    node.probe_error_mean_us = error;
  }
  // min/max span EVERYTHING, including excursions: the point of a range is that
  // nothing is hidden from it.
  if (error < node.probe_error_min_us) {
    node.probe_error_min_us = error;
  }
  if (error > node.probe_error_max_us) {
    node.probe_error_max_us = error;
  }

  // An excursion is a scheduling artefact, not a clock estimate: measured on the
  // bench at 20x the ordinary jitter and roughly one per 150 probes, which is
  // about what a console that blocks the callback for a few milliseconds a second
  // would produce. Counted, worst-case kept, and left out of the RMS.
  constexpr int64_t kExcursionUs = 1000;
  const int64_t deviation = error - node.probe_error_mean_us;
  const int64_t deviation_magnitude = deviation < 0 ? -deviation : deviation;
  if (node.probe_error_count > 0 && deviation_magnitude > kExcursionUs) {
    ++node.probe_excursions;
    if (deviation_magnitude > node.probe_excursion_worst_us) {
      node.probe_excursion_worst_us = deviation_magnitude;
    }
    return;
  }

  node.probe_error_sum_us += error;
  node.probe_error_sum_sq += static_cast<uint64_t>(error * error);
  ++node.probe_error_count;
  node.probe_error_mean_us =
      node.probe_error_sum_us / static_cast<int64_t>(node.probe_error_count);
}

// Kept short on purpose: this runs on the WiFi task, so it updates counters and
// gets out. All logging happens in the primary's own loop.
void primaryRecvCallback(const esp_now_recv_info_t *info, const uint8_t *data,
                         int len) {
  if (info == nullptr || data == nullptr || len < static_cast<int>(kEnvelopeSize)) {
    return;
  }
  if (data[0] != kEspNowMagic0 || data[1] != kEspNowMagic1 ||
      data[2] != kEspNowProtocolVersion) {
    ++sUnknownPackets;
    return;
  }

  NodeState *node = nodeFor(info->src_addr);
  if (node == nullptr) {
    ++sUnknownPackets;
    return;
  }

  // Taken before anything else in this callback, for the same reason the leaf
  // takes its own first: this is the arrival time the time-shift instrument
  // compares against, so any work done ahead of it is error added to it.
  const uint64_t arrival_us = static_cast<uint64_t>(esp_timer_get_time());

  const uint8_t *payload = data + kEnvelopeSize;
  const size_t payload_size = static_cast<size_t>(len) - kEnvelopeSize;
  node->last_seen_us = arrival_us;
  node->bytes += static_cast<uint32_t>(len);

  switch (static_cast<PacketType>(data[3])) {
    case PacketType::kData: {
      if (payload_size < kFrameHeaderSize) {
        ++sUnknownPackets;
        return;
      }
      ++node->data_frames;
      // Read the canonical frame's own header rather than trusting the envelope:
      // seqNo is what makes loss detectable, and sampleCount plus the declared
      // rate are what say the stream is real and not a stuck buffer.
      uint16_t sample_count = 0;
      uint32_t declared_rate = 0;
      uint64_t seq = 0;
      uint64_t device_ts_us = 0;
      std::memcpy(&sample_count, payload + 2, sizeof(sample_count));
      std::memcpy(&declared_rate, payload + 4, sizeof(declared_rate));
      std::memcpy(&seq, payload + 8, sizeof(seq));
      std::memcpy(&device_ts_us, payload + 16, sizeof(device_ts_us));
      node->last_sample_count = sample_count;
      node->last_declared_rate = declared_rate;

      // --- the time shift, and the instrument that says whether it worked ----
      //
      // The frame's own timestamp is in the LEAF's clock. Shifting it into ours
      // is what makes two nodes' samples comparable, and it is done here rather
      // than on the leaf so that the raw device time survives on the wire and the
      // correction stays undoable.
      applyTimeShift(*node, device_ts_us, arrival_us);

      if (node->seq_seen) {
        // The expected case is spelled out FIRST and does nothing, rather than
        // being left to fall through the others. Leaving it implicit is what broke
        // this once already: splitting "duplicate" out of an original
        // `else if (seq <= last)` left `seq == last + 1` -- every ordinary frame --
        // dropping into the final else, so the primary reported 315 sender
        // restarts against 320 frames from a leaf that had booted once.
        if (seq == node->last_seq + 1) {
          // In order, nothing to record.
        } else if (seq > node->last_seq + 1) {
          node->seq_gaps += static_cast<uint32_t>(seq - node->last_seq - 1);
        } else if (seq == node->last_seq) {
          // The SAME frame twice. Distinguished from a restart because the causes
          // are unrelated and so are the fixes: a duplicate means the frame was
          // transmitted more than once (the leaf's retry path re-sends a frame
          // whose send callback did not arrive, and a frame that actually landed
          // the first time then arrives twice), whereas a restart means the leaf
          // rebooted. Lumping them together made "restarts 6" appear against a
          // leaf whose own heartbeat said it had been up for one 66-second boot.
          ++node->seq_duplicates;
        } else {
          // Strictly backwards: the leaf rebooted and began a new sequence.
          // Counted for the same reason the ESP-NOW probe had to learn to -- a
          // silent restart otherwise reads as one clean stream and quietly makes
          // the frame total the sum of two runs.
          ++node->seq_restarts;
        }
      }
      node->last_seq = seq;
      node->seq_seen = true;
      break;
    }
    case PacketType::kHeartbeat:
      ++node->heartbeats;
      if (payload_size >= sizeof(Heartbeat)) {
        std::memcpy(&node->last_heartbeat, payload, sizeof(Heartbeat));
        node->heartbeat_seen = true;
      }
      break;
    case PacketType::kAnnounce:
      ++node->announces;
      break;
    case PacketType::kSyncState:
      if (payload_size >= sizeof(SyncState)) {
        std::memcpy(&node->last_sync, payload, sizeof(SyncState));
        node->sync_seen = true;
      }
      break;
    case PacketType::kTimeProbe: {
      ++node->probes_seen;
      if (node->probe_pending) {
        // The previous probe's follow-up never arrived, so it can never be
        // scored. Counted rather than quietly overwritten.
        ++node->probes_orphaned;
      }
      if (payload_size >= sizeof(TimeProbe)) {
        TimeProbe probe{};
        std::memcpy(&probe, payload, sizeof(probe));
        node->probe_pending = true;
        node->probe_pending_seq = probe.seq;
        // The arrival stamp taken at the top of this callback, NOT one read here
        // after the switch and the memcpys above it.
        node->probe_arrival_us = arrival_us;
      }
      break;
    }
    case PacketType::kTimeProbeFollowUp: {
      if (payload_size >= sizeof(TimeProbeFollowUp)) {
        TimeProbeFollowUp follow_up{};
        std::memcpy(&follow_up, payload, sizeof(follow_up));
        scoreProbe(*node, follow_up);
      }
      break;
    }
    case PacketType::kTimeBeacon:
    case PacketType::kTimeFollowUp:
      // Another primary's timing broadcast on our channel. Counted, not acted on:
      // two timing masters in one room is a registry question, and the registry
      // is TEC-NATKIT-25.
      ++sUnknownPackets;
      break;
    default:
      ++sUnknownPackets;
      break;
  }
}

// --- The timing broadcast (#340) --------------------------------------------
//
// This REPLACES the old kPrimaryHere beacon rather than running beside it. That
// beacon existed so leaves could discover the hub, it already ran at exactly this
// cadence, and the beacon below still does that job -- so there is one 1 Hz
// broadcast from the primary, not two that could drift apart.

// Captured by the primary's send callback and read by the beacon task. volatile
// because the callback runs on the WiFi task.
volatile uint64_t sBeaconTxUs = 0;
volatile uint64_t sBeaconTxTsfUs = 0;
SemaphoreHandle_t sBeaconSent = nullptr;

// Set while a beacon is in flight, so the follow-up's own send callback -- which
// fires a moment later on the same path -- cannot be mistaken for the beacon's.
volatile bool sAwaitingBeaconTx = false;

uint32_t sEpoch = 0;
uint32_t sBeaconSeq = 0;
uint32_t sBeaconsWithoutTxStamp = 0;

void primarySendCallback(const wifi_tx_info_t *, esp_now_send_status_t) {
  if (!sAwaitingBeaconTx) {
    return;
  }
  // The whole point of the two-step protocol is this line and where it sits: the
  // clock is read here, after the frame has actually gone out, rather than before
  // esp_now_send -- which would time the transmit queue (CSMA backoff and driver
  // queueing, milliseconds and variable) instead of the clock.
  sBeaconTxUs = static_cast<uint64_t>(esp_timer_get_time());
  // Expected to be 0 throughout this architecture: the IDF returns 0 from
  // esp_wifi_get_tsf_time on a station that is not associated, and no node here
  // ever associates. Read anyway so the claim is a hardware measurement rather
  // than a citation -- #340's first recommended approach turns on it.
  sBeaconTxTsfUs = static_cast<uint64_t>(esp_wifi_get_tsf_time(WIFI_IF_STA));
  sAwaitingBeaconTx = false;
  if (sBeaconSent != nullptr) {
    xSemaphoreGive(sBeaconSent);
  }
}

bool broadcastPacket(PacketType type, const void *payload, size_t payload_size) {
  uint8_t packet[kEnvelopeSize + 64];
  if (payload_size > sizeof(packet) - kEnvelopeSize) {
    return false;
  }
  packet[0] = kEspNowMagic0;
  packet[1] = kEspNowMagic1;
  packet[2] = kEspNowProtocolVersion;
  packet[3] = static_cast<uint8_t>(type);
  if (payload != nullptr && payload_size > 0) {
    std::memcpy(packet + kEnvelopeSize, payload, payload_size);
  }
  return esp_now_send(kBroadcast, packet, kEnvelopeSize + payload_size) == ESP_OK;
}

void beaconTask(void *) {
  while (true) {
    TimeBeacon beacon{};
    beacon.seq = ++sBeaconSeq;
    beacon.epoch = sEpoch;
    beacon.enqueue_us = static_cast<uint64_t>(esp_timer_get_time());
    // 0 for "no wall clock", which is the truth: the primary has no NTP by
    // design. #349's gateway is what fills this in.
    beacon.wall_us = 0;

    sBeaconTxUs = 0;
    sBeaconTxTsfUs = 0;
    xSemaphoreTake(sBeaconSent, 0);  // clear any stale completion
    sAwaitingBeaconTx = true;

    if (!broadcastPacket(PacketType::kTimeBeacon, &beacon, sizeof(beacon))) {
      sAwaitingBeaconTx = false;
      vTaskDelay(pdMS_TO_TICKS(CONFIG_NATKIT_TIME_BEACON_MS));
      continue;
    }

    // Wait for the transmit callback, then say what it saw. A follow-up is only
    // worth sending if there is a real stamp in it -- tx_us of 0 tells a leaf to
    // discard the pair rather than anchor a sample to a time nothing measured.
    TimeFollowUp follow_up{};
    follow_up.seq = beacon.seq;
    follow_up.epoch = beacon.epoch;
    if (xSemaphoreTake(sBeaconSent, pdMS_TO_TICKS(50)) == pdTRUE) {
      follow_up.tx_us = sBeaconTxUs;
      follow_up.tx_tsf_us = sBeaconTxTsfUs;
    } else {
      sAwaitingBeaconTx = false;
      ++sBeaconsWithoutTxStamp;
    }
    broadcastPacket(PacketType::kTimeFollowUp, &follow_up, sizeof(follow_up));

    vTaskDelay(pdMS_TO_TICKS(CONFIG_NATKIT_TIME_BEACON_MS));
  }
}

}  // namespace

esp_err_t espNowPrimaryStart() {
  ESP_ERROR_CHECK(startRadio());

  // A boot identifier, not a device identifier: what a leaf needs to detect is
  // that this primary's esp_timer restarted at zero, and only a value that
  // changes per boot says that. Forced non-zero because 0 is the leaf's "no epoch
  // yet", and an epoch that collides with it would look like a primary that never
  // rebooted.
  sEpoch = esp_random();
  if (sEpoch == 0) {
    sEpoch = 1;
  }

  sBeaconSent = xSemaphoreCreateBinary();
  if (sBeaconSent == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  ESP_ERROR_CHECK(esp_now_register_recv_cb(primaryRecvCallback));
  ESP_ERROR_CHECK(esp_now_register_send_cb(primarySendCallback));
  ESP_ERROR_CHECK(addBroadcastPeer());
  xTaskCreate(beaconTask, "natkit-beacon", 3072, nullptr, 4, nullptr);

  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  ESP_LOGI(kTag,
           "primary up on channel %d as %02x:%02x:%02x:%02x:%02x:%02x, timing "
           "master for epoch %08lx, beacon + follow-up every 1s, tracking up to "
           "%u nodes",
           CONFIG_NATKIT_ESPNOW_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5], static_cast<unsigned long>(sEpoch), (unsigned)kMaxTrackedNodes);
  return ESP_OK;
}

const NodeState *espNowPrimaryNodes() { return sNodes; }

uint32_t espNowPrimaryUnknownPackets() { return sUnknownPackets; }

uint32_t espNowPrimaryEpoch() { return sEpoch; }

uint32_t espNowPrimaryBeaconSeq() { return sBeaconSeq; }

uint32_t espNowPrimaryBeaconsWithoutTxStamp() { return sBeaconsWithoutTxStamp; }

uint64_t espNowPrimaryLastTxTsf() { return sBeaconTxTsfUs; }

}  // namespace natkit
