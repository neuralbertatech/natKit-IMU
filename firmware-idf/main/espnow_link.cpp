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
#include "imu_frame.hpp"
#include "sdkconfig.h"

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

void sendCallback(const wifi_tx_info_t *, esp_now_send_status_t status) {
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
  if (data[3] == static_cast<uint8_t>(PacketType::kPrimaryHere)) {
    learnPrimary(info->src_addr);
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

  const uint8_t *payload = data + kEnvelopeSize;
  const size_t payload_size = static_cast<size_t>(len) - kEnvelopeSize;
  node->last_seen_us = static_cast<uint64_t>(esp_timer_get_time());
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
      std::memcpy(&sample_count, payload + 2, sizeof(sample_count));
      std::memcpy(&declared_rate, payload + 4, sizeof(declared_rate));
      std::memcpy(&seq, payload + 8, sizeof(seq));
      node->last_sample_count = sample_count;
      node->last_declared_rate = declared_rate;

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
    case PacketType::kPrimaryHere:
      // Another primary on our channel. Not handled here beyond being counted --
      // deciding what two hubs do about each other is a registry question, so it
      // belongs to TEC-NATKIT-25.
      ++sUnknownPackets;
      break;
    default:
      ++sUnknownPackets;
      break;
  }
}

// Broadcasts "I am the hub" so leaves can discover us.
//
// One second because that is also the timing broadcast's cadence in #340, and when
// that slice lands this beacon is the obvious thing for it to replace rather than
// sit alongside.
void beaconTask(void *) {
  uint8_t packet[kEnvelopeSize] = {kEspNowMagic0, kEspNowMagic1,
                                   kEspNowProtocolVersion,
                                   static_cast<uint8_t>(PacketType::kPrimaryHere)};
  while (true) {
    esp_now_send(kBroadcast, packet, sizeof(packet));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

esp_err_t espNowPrimaryStart() {
  ESP_ERROR_CHECK(startRadio());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(primaryRecvCallback));
  ESP_ERROR_CHECK(addBroadcastPeer());
  xTaskCreate(beaconTask, "natkit-beacon", 3072, nullptr, 4, nullptr);

  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  ESP_LOGI(kTag,
           "primary up on channel %d as %02x:%02x:%02x:%02x:%02x:%02x, "
           "beaconing every 1s, tracking up to %u nodes",
           CONFIG_NATKIT_ESPNOW_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5], (unsigned)kMaxTrackedNodes);
  return ESP_OK;
}

const NodeState *espNowPrimaryNodes() { return sNodes; }

uint32_t espNowPrimaryUnknownPackets() { return sUnknownPackets; }

}  // namespace natkit
