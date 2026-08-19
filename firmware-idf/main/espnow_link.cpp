#include "espnow_link.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "device_id.hpp"
#include "esp_event.h"
#include "esp_crc.h"
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
#include "channel_survey.hpp"
#include "gateway_net.hpp"
#include "registry.hpp"
#include "sdkconfig.h"
#include "time_sync.hpp"
#include "command_relay.hpp"
#include "commands.hpp"
#include "uplink.hpp"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-link";

constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// True when this image drives ESP-NOW on a radio that is ALSO associated with an
// access point (#373). An unset bool Kconfig emits no symbol, so it is resolved
// once here rather than read as a value.
// ⚠️ WIFI ONLY. This flag means "ESP-NOW is sharing an ASSOCIATED radio", which
// is what forces peers onto the interface's channel and stops us setting one. The
// Ethernet uplink does NOT set it: a wired uplink leaves the radio entirely ours,
// so the configured channel still applies and there is nothing to follow.
#ifdef CONFIG_NATKIT_PRIMARY_WIFI_UPLINK
constexpr bool kWifiUplink = true;
#else
constexpr bool kWifiUplink = false;
#endif

// True when this image publishes to MQTT itself, over either uplink -- which is
// what decides whether frames are shifted to wall clock here rather than
// forwarded raw to a gateway.
#if defined(CONFIG_NATKIT_PRIMARY_WIFI_UPLINK) || \
    defined(CONFIG_NATKIT_PRIMARY_ETH_UPLINK)
constexpr bool kSelfPublish = true;
#else
constexpr bool kSelfPublish = false;
#endif

// The channel an ESP-NOW peer is added on. ALWAYS 0, which means "whatever
// channel this interface is currently on".
//
// ⚠️ Never the configured channel, even on a node that pins its own. Pinning a
// peer to a channel the interface is not on is a SILENT failure: every send
// returns success locally and nothing is ever received. Two things now move the
// interface out from under a hard-coded value -- a primary that associates takes
// the AP's channel (#373), and a searching leaf hops -- so the only safe answer
// is to follow rather than to assert.
uint8_t peerChannel() { return 0; }


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
  peer.channel = peerChannel();
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

  if (info->rx_ctrl != nullptr) {
    const int8_t rssi = static_cast<int8_t>(info->rx_ctrl->rssi);
    if (!sStats.rssi_seen) {
      sStats.rssi_seen = true;
      sStats.rssi_best = rssi;
      sStats.rssi_worst = rssi;
    }
    sStats.rssi_last = rssi;
    if (rssi > sStats.rssi_best) sStats.rssi_best = rssi;
    if (rssi < sStats.rssi_worst) sStats.rssi_worst = rssi;

    // The floor this packet was pulled out of. Free here: we are already holding
    // the receive control block for the RSSI, and this is the number that says
    // whether a healthy RSSI is actually a healthy link (TEC-NATKIT-51).
    const int8_t floor_dbm = static_cast<int8_t>(info->rx_ctrl->noise_floor);
    if (!sStats.noise_seen) {
      sStats.noise_seen = true;
      sStats.noise_floor_worst = floor_dbm;
    }
    sStats.noise_floor_last = floor_dbm;
    if (floor_dbm > sStats.noise_floor_worst) {
      sStats.noise_floor_worst = floor_dbm;
    }
  }

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
    case PacketType::kCommand: {
      if (payload_size < sizeof(CommandFrame)) {
        break;
      }
      CommandFrame frame{};
      std::memcpy(&frame, payload, sizeof(frame));
      frame.command_id[kCommandIdMax - 1] = '\0';
      frame.command[kCommandNameMax - 1] = '\0';
      frame.args[kCommandArgsMax - 1] = '\0';
      // ⚠️ ADDRESSED, AND CHECKED HERE. The primary unicasts, but ESP-NOW peers
      // can and do receive frames meant for others, and a leaf executing another
      // node's command would be both wrong and extremely confusing to debug.
      if (frame.device_id != deviceId()) {
        break;
      }

      // ⚠️ ACKNOWLEDGE FIRST, AND ACKNOWLEDGE AGAIN FOR A REPEAT. The primary
      // retransmits until it hears this, so a command whose ack was lost will
      // arrive a second time -- and the right answer to that is another ack, not
      // another execution.
      CommandAck ack{};
      ack.device_id = frame.device_id;
      std::strncpy(ack.command_id, frame.command_id, kCommandIdMax - 1);
      espNowLinkSend(PacketType::kCommandAck, &ack, sizeof(ack));

      // ⚠️ AND EXECUTE AT MOST ONCE. Retransmission plus no de-duplication would
      // run a command several times, which for something like "set the report
      // configuration" is merely wasteful and for anything with a side effect is
      // a bug. Keyed on command_id, which the backend generates uniquely.
      if (commandsAlreadySeen(frame.command_id)) {
        break;
      }
      commandsEnqueue(frame);
      break;
    }
    case PacketType::kSyncMarker: {
      // A held-out sample: converted with the fit, never fed INTO it. Answering
      // from the callback rather than from a task is deliberate -- the answer is
      // "what time do I think it is", and queueing the conversion behind a task
      // switch would fold that task switch into the answer.
      if (payload_size >= sizeof(SyncMarker)) {
        SyncMarker marker{};
        std::memcpy(&marker, payload, sizeof(marker));

        MarkerReport report{};
        report.device_id = deviceId();
        report.marker_seq = marker.seq;
        report.epoch = marker.epoch;
        report.local_us = rx_local_us;
        report.quality = static_cast<uint8_t>(timeSyncStatus().quality);
        if (!timeSyncToPrimary(rx_local_us, report.primary_us)) {
          report.primary_us = 0;  // unsynced: say so rather than answer anyway
        }
        espNowLinkSend(PacketType::kMarkerReport, &report, sizeof(report));
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

// ⚠️ BEACON SILENCE, NOT SEND FAILURES -- the same correction a91f943 already made
// to channel rescanning, which was left unapplied here.
//
// This used to presume the hub gone after five consecutive on-air failures, on
// the reasoning that five at 5 frames/s is a second of silence. Two things make
// that wrong on this rig. The frame rate is 10/s plus heartbeats and sync
// probes, so five failures is a fraction of a second; and an "on-air failure"
// here means no MAC-layer ACK came back, which HAPPENS CONSTANTLY WHILE THE
// FRAMES ARRIVE -- measured at 399 tx failures against a hub that was receiving
// ~10 frames/s and forwarding every one of them.
//
// So a routine run of five spurious failures flipped the leaf to one-try-no-retry
// mode, and THAT caused real loss: frames that a retry would have delivered were
// sent once into a busy channel and dropped. It recovered when one send happened
// to be acknowledged, then repeated. That feedback loop is the ~2 s stall in
// TEC-NATKIT-42 -- self-inflicted, and invisible because the counter it keyed on
// was measuring something real that simply did not mean what it was read to mean.
//
// Beacon silence is the authoritative signal for the same reason it is when
// rescanning: the primary broadcasts every second, broadcasts need no ACK, so
// hearing them proves the hub is there no matter what the transmit path thinks.
constexpr uint64_t kAbsentAfterBeaconSilenceUs = 3ULL * 1000000ULL;

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
      // Failures are COUNTED but no longer decide anything: see
      // kAbsentAfterBeaconSilenceUs. What decides is whether beacons have stopped.
      const uint64_t last_beacon = timeSyncStatus().last_beacon_local_us;
      const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
      const bool beacons_silent =
          last_beacon != 0 && now_us - last_beacon > kAbsentAfterBeaconSilenceUs;
      if (!sStats.primary_absent && beacons_silent) {
        sStats.primary_absent = true;
        // Logged once, on the transition. A line per failed frame would be the
        // loudest thing in the console for the whole outage and would say nothing
        // the counters do not.
        ESP_LOGW(kTag,
                 "no beacon for %llu ms and sends are failing: presuming the "
                 "primary is gone, sending once per frame until it returns",
                 static_cast<unsigned long long>((now_us - last_beacon) / 1000));
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
// How long a leaf listens on one channel before trying the next.
//
// Longer than the primary's beacon interval on purpose: dwelling for less than a
// full beacon period can step past a primary that was about to speak, which
// presents as "the hub is not there" and is really "we were not listening when
// it was".
constexpr uint32_t kChannelDwellMs = 1300;

// How many consecutive send failures mean "this hub is not coming back on this
// channel". Far above kAbsentAfterFailures, which only changes retry policy: this
// one throws away a working association, so it must not fire on a hub that is
// merely rebooting.
// How long the primary must be SILENT before a leaf gives up its channel. The
// hub beacons every second, so 15 s is fifteen consecutive misses -- far beyond
// any interference burst, and comfortably longer than the ~5 s a primary takes to
// reboot (a reboot is survivable: TEC-NATKIT-24 rode out a 30 s outage without
// this existing at all).
constexpr uint64_t kRescanAfterSilenceUs = 15ULL * 1000000ULL;
constexpr uint8_t kMaxChannel = 13;

// Sweep our own transmit power and keep the LOWEST setting that still gets its
// packets acknowledged.
//
// ⚠️ THIS MEASURES THE RIGHT THING, which the channel survey does not. That one
// scores strangers' 802.11 traffic and had to be turned off after it picked a
// channel our own link could not cross. This scores OUR link directly: an ESP-NOW
// unicast reports success only when the far side's MAC acknowledged it, so the
// success ratio at a given power IS the thing we care about, measured on the real
// path with the real packets.
//
// Lowest-that-works rather than highest-that-works, and that is the whole point.
// Measured on this bench: at 19.5 dBm and a few centimetres the hub received 0.4
// frames/s, and at 2 dBm it received 8.5 -- because ~+9 dBm arriving at a 2.4 GHz
// front end saturates it. Sweeping upward and stopping at the first level that
// works therefore lands below the overload region automatically, at whatever
// distance the rig happens to be set up.
//
// It runs in the window where the primary cannot publish anyway (NTP unsynced),
// so like the channel survey it costs nothing that was not already being lost --
// and it uses the DATA FRAMES ALREADY BEING SENT as its probes rather than adding
// traffic.
void sweepTxPower() {
  // Ascending, coarsely: 2, 5, 8.5, 11, 14, 17, 19.5 dBm. Fine steps would cost
  // time without changing the answer -- the transition from saturated to sane is
  // tens of dB wide, not fractions.
  static const int8_t kLevels[] = {8, 20, 34, 44, 56, 68, 78};
  const uint32_t dwell = CONFIG_NATKIT_TX_POWER_SWEEP_DWELL_MS;

  int8_t best = kLevels[0];
  uint32_t best_pct = 0;
  bool any = false;
  ESP_LOGI(kTag, "sweeping transmit power, %lu ms per level",
           static_cast<unsigned long>(dwell));

  for (int8_t level : kLevels) {
    if (esp_wifi_set_max_tx_power(level) != ESP_OK) {
      continue;
    }
    const uint32_t sent0 = sStats.packets_sent;
    const uint32_t fail0 = sStats.send_failures;
    vTaskDelay(pdMS_TO_TICKS(dwell));
    const uint32_t sent = sStats.packets_sent - sent0;
    const uint32_t fail = sStats.send_failures - fail0;
    const uint32_t total = sent + fail;
    const uint32_t pct = total > 0 ? (100 * sent) / total : 0;

    ESP_LOGI(kTag, "  %4.1f dBm: %3lu acked of %3lu (%lu%%)%s", level / 4.0,
             static_cast<unsigned long>(sent), static_cast<unsigned long>(total),
             static_cast<unsigned long>(pct),
             (total > 0 && pct > best_pct) ? "   <- best so far" : "");
    // ⚠️ BEST acknowledgement rate, not "first one above a threshold", and
    // strictly greater so ties go to the LOWER level. An absolute threshold was
    // tried and was silently dead: on this bench the whole table read 2.0 dBm
    // 13%, every higher level 0%, so a 90% bar was never cleared and the sweep
    // fell through to its fallback on EVERY boot -- for weeks, while appearing to
    // work because the fallback happened to be the floor, which is also the right
    // answer here. Changing that fallback to full power is what exposed it.
    if (total > 0 && pct > best_pct) {
      best_pct = pct;
      best = level;
      any = true;
    }
  }

  esp_wifi_set_max_tx_power(best);
  sStats.tx_power_chosen_quarter_dbm = static_cast<uint8_t>(best);
  sStats.tx_power_swept = true;
  // ⚠️ Update the REPORTED value too. It is captured once in startRadio, so
  // without this the console kept printing the boot-time 19.5 dBm while the radio
  // was actually running at 2.0 -- a counter describing a setting it no longer
  // reflected, which is the same trap this codebase keeps falling into.
  sStats.tx_power_quarter_dbm = static_cast<int8_t>(best);
  ESP_LOGW(kTag,
           "transmit power set to %.1f dBm, the best of %u levels at %lu%% "
           "acked%s. ⚠️ TREAT THAT PERCENTAGE AS A RANKING ONLY, NEVER AS A "
           "HEALTH FIGURE: 13%% here coincided with 10.6 frames/s at the hub and "
           "ZERO sequence gaps. The acknowledgement is a MAC-layer reply that the "
           "busy hub often does not get back in time; the data frame lands "
           "regardless. Only the hub's gap count says whether delivery is "
           "actually working.",
           best / 4.0, (unsigned)(sizeof(kLevels) / sizeof(kLevels[0])),
           static_cast<unsigned long>(best_pct),
           any ? "" : " (no level transmitted at all; using the floor)");
}

void announceTask(void *) {
  uint8_t channel = CONFIG_NATKIT_ESPNOW_CHANNEL;
  while (true) {
    Announce announce{};
    announce.device_id = deviceId();
    announce.sample_rate_hz = CONFIG_NATKIT_IMU_DECLARED_RATE_HZ;
    announce.samples_per_frame = CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME;
    announce.firmware_version = 1;

    espNowLinkSend(PacketType::kAnnounce, &announce, sizeof(announce));
    ++sStats.announces;

    // ⚠️ A leaf that has found a hub must be able to LOSE it and look again.
    //
    // Learning the primary used to be permanent, which stranded a leaf on a dead
    // channel forever: move the hub to another channel and the leaf sits happily
    // on the old one, unicasting into nothing, with `primary_absent` set and no
    // way to recover short of a reboot. Found by moving the primary from channel
    // 1 to 11 and watching both leaves fail to follow.
    //
    // So a long run of failures gives the channel back to the search. The
    // threshold is deliberately much longer than a transient outage -- the leaf
    // survived a 30 s primary reboot on TEC-NATKIT-24 without needing this.
    // ⚠️ RESCAN ON SILENCE, NOT ON SEND FAILURES.
    //
    // Basing this on consecutive failures was wrong, and expensively so: a
    // "failure" here means no MAC-layer ACK came back, which on this rig happens
    // constantly while the frames themselves ARRIVE -- measured at 399 tx
    // failures against a hub that was receiving ~10 frames/s and forwarding them
    // to Kafka. So the leaf kept abandoning a primary it was successfully feeding
    // and spending up to 17 s hopping channels, which is what made the frontend
    // swing between 3 and 11 frames/s.
    //
    // Beacon silence is the authoritative signal. The primary broadcasts every
    // second; broadcasts need no ACK, so hearing them proves the hub is present
    // and on this channel no matter what the transmit path thinks.
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t last_beacon = timeSyncStatus().last_beacon_local_us;
    const bool beacons_silent =
        last_beacon != 0 &&
        now_us - last_beacon > kRescanAfterSilenceUs;

#if CONFIG_NATKIT_TX_POWER_SWEEP
    // Once, after the hub is found: there is nothing to measure ACKs against
    // until there is something to acknowledge them.
    if (sPrimaryKnown && !sStats.tx_power_swept) {
      sweepTxPower();
      continue;
    }
#endif

    if (sPrimaryKnown && !beacons_silent) {
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }
    if (sPrimaryKnown) {
      ESP_LOGW(kTag,
               "no beacon for %llu ms; the primary is genuinely gone, so "
               "scanning channels again",
               static_cast<unsigned long long>((now_us - last_beacon) / 1000));
      sPrimaryKnown = false;
      sStats.primary_known = false;
    }

    // --- searching: walk the channels ---------------------------------------
    //
    // A leaf cannot assume the configured channel any more. Once the primary
    // associates with an access point (#373) its channel is the AP's, not ours,
    // and a leaf pinned to channel 1 while the hub sits on 6 hears nothing at
    // all -- every send succeeding locally and nothing ever arriving, which
    // reads exactly like a dead hub.
    //
    // So while no primary is known, hop. This makes a leaf find its hub whatever
    // the site's WiFi is doing, which is the difference between a rig that works
    // in one room and a rig that works.
    channel = static_cast<uint8_t>(channel % kMaxChannel + 1);
    const esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "could not move to channel %u: %s", channel,
               esp_err_to_name(err));
    }
    sStats.scan_channel = channel;
    ++sStats.channel_hops;
    vTaskDelay(pdMS_TO_TICKS(kChannelDwellMs));
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

uint8_t sChosenChannel = CONFIG_NATKIT_ESPNOW_CHANNEL;

esp_err_t startRadio(bool survey) {
  if (kWifiUplink) {
    // The association is brought up FIRST, by gateway_net, because it owns
    // esp_netif and esp_wifi_init. ESP-NOW is then layered on the same radio and
    // must not touch the channel. This is the whole experiment: one chip doing
    // both, instead of the two-board split the epic assumes.
    ESP_ERROR_CHECK(esp_now_init());
    uint8_t channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&channel, &second);
    ESP_LOGW(kTag,
             "ESP-NOW is sharing an ASSOCIATED radio (#373). Channel is the "
             "AP's (%u) and is NOT ours to set; peers are added on channel 0 so "
             "they follow it. Every leaf must reach this channel or it is "
             "shouting into a different one.",
             channel);
    return ESP_OK;
  }
  // No esp_netif_init() and no netif at all: ESP-NOW does not go through lwIP, so
  // a leaf never brings up a network interface. The event loop IS required --
  // esp_wifi_init posts to it.
  // ⚠️ ESP_ERR_INVALID_STATE here means "already created", which is now a NORMAL
  // case rather than a failure: an uplink that brings up a netif first (Ethernet,
  // or WiFi under #373) has already made it. ESP_ERROR_CHECK on this aborted the
  // whole board in a reboot loop the moment the Ethernet uplink was enabled.
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
    return loop;
  }

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

  // Read the radio's ACTUAL transmit power rather than assuming the default.
  //
  // ⚠️ The return code is checked, and that matters: an earlier version ignored
  // it, left `power` at its initialiser, and the console reported "0.0 dBm" --
  // which reads as a radio turned down to nothing rather than as a query that
  // never ran. Reported in quarter-dBm; ~78 is the usual +19.5 dBm maximum.
#if CONFIG_NATKIT_TX_POWER_QUARTER_DBM > 0
  // Deliberately turned DOWN. See NATKIT_TX_POWER_QUARTER_DBM: at bench
  // distances full power overloads the far receiver rather than helping it.
  const esp_err_t set_err =
      esp_wifi_set_max_tx_power(CONFIG_NATKIT_TX_POWER_QUARTER_DBM);
  ESP_LOGW(kTag, "transmit power forced to %d quarter-dBm (%.1f dBm): %s",
           CONFIG_NATKIT_TX_POWER_QUARTER_DBM,
           CONFIG_NATKIT_TX_POWER_QUARTER_DBM / 4.0, esp_err_to_name(set_err));
#endif
  int8_t power = 0;
  const esp_err_t power_err = esp_wifi_get_max_tx_power(&power);
  sStats.tx_power_quarter_dbm = power;
  ESP_LOGI(kTag, "radio tx power %d quarter-dBm (%.1f dBm), query %s", power,
           power / 4.0, esp_err_to_name(power_err));

  // Fixed channel on both ends, and never esp_wifi_connect. A channel mismatch
  // presents as every packet sending successfully while nothing is received, which
  // reads as total loss rather than as a misconfiguration.
  // The survey runs HERE: after the radio is up, before esp_now_init, and only
  // on the primary (a leaf finds whatever channel the hub chose by hopping). It
  // leaves the radio in promiscuous mode while it runs, which is why it cannot
  // happen once ESP-NOW owns the interface.
  uint8_t channel = CONFIG_NATKIT_ESPNOW_CHANNEL;
#if CONFIG_NATKIT_CHANNEL_SURVEY
  if (survey) {
    channel = channelSurveyRun(channel, CONFIG_NATKIT_CHANNEL_SURVEY_DWELL_MS);
  }
#else
  (void)survey;
#endif
  sChosenChannel = channel;
  ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  return ESP_OK;
}

esp_err_t addBroadcastPeer() {
  esp_now_peer_info_t peer{};
  std::memcpy(peer.peer_addr, kBroadcast, 6);
  peer.channel = peerChannel();
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer);
}

}  // namespace

esp_err_t espNowLinkStart() {
  ESP_ERROR_CHECK(startRadio(false));

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

void espNowLinkResetNoiseWorst() {
  sStats.noise_floor_worst = sStats.noise_floor_last;
}

bool espNowLinkHasPrimary() { return sPrimaryKnown; }

// --- Primary side ----------------------------------------------------------

namespace {

NodeState sNodes[kMaxTrackedNodes];
uint32_t sUnknownPackets = 0;
// The hub's own noise floor, from the PHY, sampled on packets it was receiving
// anyway. Rig-level rather than per node: it describes where the primary sits.
bool sPrimaryNoiseSeen = false;
int8_t sPrimaryNoiseLast = 0;
int8_t sPrimaryNoiseWorst = 0;

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

CoherenceStats sCoherence{};

// How far off a pair has to be before it is a scheduling artefact rather than a
// clock disagreement. Twice the single-node figure, because a pair carries two
// nodes' worth of it.
constexpr int64_t kCoherenceExcursionUs = 2000;

// Pairs this node's answer to a marker against every other node's answer to the
// SAME marker.
//
// This is the whole of #315's measurement: one broadcast wavefront, two clocks,
// and the difference between what they each think the time was. Nothing here
// models anything -- there is no cancellation argument and no assumption about
// where the error comes from.
void pairMarker(NodeState &node) {
  for (NodeState &other : sNodes) {
    if (!other.in_use || &other == &node || !other.marker_seen) {
      continue;
    }
    if (other.marker_seq != node.marker_seq) {
      continue;  // different events are not comparable, which is the point
    }
    // A node that could not convert has no answer to compare.
    if (other.marker_primary_us == 0 || node.marker_primary_us == 0) {
      continue;
    }

    const int64_t spread = static_cast<int64_t>(node.marker_primary_us) -
                           static_cast<int64_t>(other.marker_primary_us);

    if (!sCoherence.seen) {
      sCoherence.seen = true;
      sCoherence.spread_min_us = spread;
      sCoherence.spread_max_us = spread;
      sCoherence.device_a = node.device_id;
      sCoherence.device_b = other.device_id;
    }
    sCoherence.spread_us = spread;
    if (spread < sCoherence.spread_min_us) {
      sCoherence.spread_min_us = spread;
    }
    if (spread > sCoherence.spread_max_us) {
      sCoherence.spread_max_us = spread;
    }

    // Same tail treatment as the probe error, for the same reason: an accuracy
    // figure a 1% tail can set is not an accuracy figure.
    const int64_t mean =
        sCoherence.markers_paired > 0
            ? sCoherence.spread_sum_us /
                  static_cast<int64_t>(sCoherence.markers_paired)
            : spread;
    const int64_t deviation = spread - mean;
    const int64_t magnitude = deviation < 0 ? -deviation : deviation;
    if (sCoherence.markers_paired > 0 && magnitude > kCoherenceExcursionUs) {
      ++sCoherence.excursions;
      if (magnitude > sCoherence.excursion_worst_us) {
        sCoherence.excursion_worst_us = magnitude;
      }
      continue;
    }

    sCoherence.spread_sum_us += spread;
    sCoherence.spread_sum_sq += static_cast<uint64_t>(spread * spread);
    ++sCoherence.markers_paired;
  }
}

// Kept short on purpose: this runs on the WiFi task, so it updates counters and
// gets out. All logging happens in the primary's own loop.
// Defined below, next to the unicast helper it pairs with.
void publishCommandLog(const CommandLogFrame &log);

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

  // The registry decides before anything else does. An unrecognised node is
  // counted and dropped rather than forwarded: a neighbouring rig on our channel
  // is a real scenario, and its frames appearing in someone's recording would be
  // very hard to explain afterwards. While the registry is open this admits and
  // remembers the node, which is what makes a fresh rig self-configuring.
  if (!registryAccepts(info->src_addr)) {
    return;  // registryRejections() counts it; the registry logs it, rate-limited
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

  // How strong was it? A near-total loss with a HEALTHY rssi means something
  // above the radio is dropping frames; the same loss with a terrible rssi means
  // the RF path itself. Those have completely different fixes and the frame
  // count cannot tell them apart.
  if (info->rx_ctrl != nullptr) {
    const int8_t rssi = static_cast<int8_t>(info->rx_ctrl->rssi);
    if (!node->rssi_seen) {
      node->rssi_seen = true;
      node->rssi_best = rssi;
      node->rssi_worst = rssi;
    }
    node->rssi_last = rssi;
    if (rssi > node->rssi_best) {
      node->rssi_best = rssi;
    }
    if (rssi < node->rssi_worst) {
      node->rssi_worst = rssi;
    }
    // The HUB's own noise floor. Kept at rig level rather than per node because it
    // is a property of where the primary sits, not of who transmitted -- and it is
    // the control for the leaf-side figure: if the leaves' floor rises and the
    // hub's does not, the noise is at the leaves, and vice versa.
    const int8_t floor_dbm = static_cast<int8_t>(info->rx_ctrl->noise_floor);
    if (!sPrimaryNoiseSeen) {
      sPrimaryNoiseSeen = true;
      sPrimaryNoiseWorst = floor_dbm;
    }
    sPrimaryNoiseLast = floor_dbm;
    if (floor_dbm > sPrimaryNoiseWorst) {
      sPrimaryNoiseWorst = floor_dbm;
    }
  }

  switch (static_cast<PacketType>(data[3])) {
    case PacketType::kData: {
      // Addressed to us, or shouted at everyone? des_addr is the only place this
      // distinction survives -- ESP-NOW hands both to the same callback.
      if (info->des_addr != nullptr && (info->des_addr[0] & 0x01) != 0) {
        ++node->frames_broadcast;
      } else {
        ++node->frames_unicast;
      }
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
          // ⚠️ DROPPED, not forwarded. A duplicate is the SAME frame arriving
          // twice -- the leaf's retry path re-sending one whose send callback
          // was late but which had already landed -- so forwarding it puts the
          // same ten samples into a recording twice.
          //
          // This was deferred from TEC-NATKIT-24 as "dedupe belongs on the
          // primary", and at 50 Hz it looked harmless: dupes ran at ~1%. At
          // 100 Hz (#380) the radio is busy enough that callbacks are late
          // constantly and it became ~50% -- 1073 frames received against a
          // sequence range of 1031, and a broker seeing 16.5 frames/s from a leaf
          // building 9. An inflated rate that looks like MORE data is a worse
          // failure than a gap, because nothing downstream flags it.
          //
          // Only an immediately-repeated sequence is caught, which is what a
          // retry produces. A duplicate arriving after a NEWER frame would still
          // pass; that needs a window rather than one value, and there is no
          // evidence of it happening.
          // The SAME frame twice. Distinguished from a restart because the causes
          // are unrelated and so are the fixes: a duplicate means the frame was
          // transmitted more than once (the leaf's retry path re-sends a frame
          // whose send callback did not arrive, and a frame that actually landed
          // the first time then arrives twice), whereas a restart means the leaf
          // rebooted. Lumping them together made "restarts 6" appear against a
          // leaf whose own heartbeat said it had been up for one 66-second boot.
          ++node->seq_duplicates;
          return;
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

      // --- the time shift, and the instrument that says whether it worked ----
      //
      // The frame's own timestamp is in the LEAF's clock. Shifting it into ours
      // is what makes two nodes' samples comparable, and it is done here rather
      // than on the leaf so that the raw device time survives on the wire and the
      // correction stays undoable.
      applyTimeShift(*node, device_ts_us, arrival_us);

      // Forward VERBATIM. The primary knows how to shift this frame's timestamps
      // and deliberately does not: the fit travels separately in the node-status
      // frame, so raw device time survives to the gateway and the correction
      // stays undoable. Same principle as the leaf not rewriting its own.
      if (kSelfPublish) {
        // This chip IS the last hop, so the shift is applied here. The
        // primary-to-wall half is a LOCAL subtraction on one clock rather than
        // the gateway's cross-serial estimate, which makes it strictly better
        // than the two-board path -- worth remembering when comparing them.
        static uint8_t shifted[kMaxPayload];
        if (!node->sync_seen) {
          ++node->publish_no_sync;
        } else if (!gatewayTimeValid()) {
          ++node->publish_no_time;
        } else if (payload_size <= sizeof(shifted)) {
          std::memcpy(shifted, payload, payload_size);
          const int64_t primary_to_wall =
              static_cast<int64_t>(gatewayWallClockUs()) -
              static_cast<int64_t>(arrival_us);
          if (rewriteFrameTimestamps(shifted, payload_size, node->last_sync,
                                     primary_to_wall)) {
            uplinkSend(UplinkType::kData, node->device_id, shifted, payload_size);
          } else {
            ++node->publish_no_shift;
          }
        }
      } else {
        uplinkSend(UplinkType::kData, node->device_id, payload, payload_size);
      }

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
    case PacketType::kMarkerReport: {
      if (payload_size >= sizeof(MarkerReport)) {
        MarkerReport report{};
        std::memcpy(&report, payload, sizeof(report));
        // Accessor rather than sEpoch directly: the beacon state is declared
        // further down this file, and scoreProbe above reaches for it the same
        // way.
        if (report.epoch == espNowPrimaryEpoch()) {
          node->marker_seen = true;
          node->marker_seq = report.marker_seq;
          node->marker_primary_us = report.primary_us;
          node->marker_local_us = report.local_us;
          node->marker_quality = report.quality;
          ++node->markers_reported;
          pairMarker(*node);
        }
      }
      break;
    }
    case PacketType::kCommandAck: {
      if (payload_size < sizeof(CommandAck)) {
        ++sUnknownPackets;
        break;
      }
      CommandAck ack{};
      std::memcpy(&ack, payload, sizeof(ack));
      ack.command_id[kCommandIdMax - 1] = '\0';
      commandRelayNoteAck(ack.device_id, ack.command_id);
      break;
    }
    case PacketType::kCommandLog: {
      if (payload_size < sizeof(CommandLogFrame)) {
        ++sUnknownPackets;
        break;
      }
      CommandLogFrame log{};
      std::memcpy(&log, payload, sizeof(log));
      log.command_id[kCommandIdMax - 1] = '\0';
      log.message[kCommandMessageMax - 1] = '\0';
      publishCommandLog(log);
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

// One marker every N beacons. Five seconds is often enough to build a
// distribution over a soak and rare enough that it is not competing with the
// data stream for airtime -- the marker costs one broadcast plus one small
// unicast per leaf.
constexpr uint32_t kMarkerEvery = 5;

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

// Unicast, and with a buffer big enough for a CommandFrame. Kept separate from
// broadcastPacket rather than parameterising it: broadcast's 64-byte buffer is a
// deliberate bound on the timing traffic, and widening it to fit a command would
// quietly allow an oversized beacon.
// Builds the JSON the backend is already waiting for and hands it to the uplink.
//
// ⚠️ BUILT BY HAND, not with a JSON library, and the escaping below is the reason
// this is safe: every field that reaches it is either generated by the backend
// (command_id) or produced by our own firmware (message), and the only characters
// either can contain that would break the document are quotes and backslashes.
// A device log line is not attacker-controlled text.
uint32_t sCommandAnswersReceived = 0;
uint32_t sCommandAnswersPublished = 0;
uint32_t sCommandAnswersDuplicate = 0;

// ⚠️ ANSWERS ARRIVE MORE THAN ONCE, MEASURED: ten pings produced twenty-one
// answers. The leaf sends each one exactly once -- what duplicates them is the
// 802.11 MAC retransmitting below ESP-NOW when its acknowledgement does not come
// back in time, the same mechanism that put ~50% duplicates on the data path
// before the transmit power came down. The data path dedupes by frame sequence
// number; a command answer has no sequence, so it needs its own.
//
// Keyed on the CONTENT as well as the id, because one command may legitimately
// produce several records (progress, then a final): only an exact repeat is a
// duplicate.
constexpr size_t kAnswerHistory = 8;
struct AnswerKey {
  uint64_t device_id;
  uint32_t message_crc;
  char command_id[kCommandIdMax];
};
AnswerKey sRecentAnswers[kAnswerHistory] = {};
size_t sRecentAnswerNext = 0;

bool answerAlreadySeen(const CommandLogFrame &log) {
  const uint32_t crc = esp_crc32_le(
      0, reinterpret_cast<const uint8_t *>(log.message),
      static_cast<uint32_t>(std::strlen(log.message)));
  for (const AnswerKey &seen : sRecentAnswers) {
    if (seen.device_id == log.device_id && seen.message_crc == crc &&
        std::strncmp(seen.command_id, log.command_id, kCommandIdMax) == 0) {
      return true;
    }
  }
  AnswerKey &slot = sRecentAnswers[sRecentAnswerNext];
  slot.device_id = log.device_id;
  slot.message_crc = crc;
  std::strncpy(slot.command_id, log.command_id, kCommandIdMax - 1);
  slot.command_id[kCommandIdMax - 1] = '\0';
  sRecentAnswerNext = (sRecentAnswerNext + 1) % kAnswerHistory;
  return false;
}

void publishCommandLog(const CommandLogFrame &log) {
  ++sCommandAnswersReceived;
  if (answerAlreadySeen(log)) {
    ++sCommandAnswersDuplicate;
    return;
  }
  const auto escapeInto = [](char *out, size_t out_size, const char *in) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_size; ++i) {
      const char c = in[i];
      if (c == '"' || c == '\\') {
        out[j++] = '\\';
      } else if (static_cast<unsigned char>(c) < 0x20) {
        continue;  // control characters would make the document unparseable
      }
      out[j++] = c;
    }
    out[j] = '\0';
  };

  char id[kCommandIdMax * 2];
  char message[kCommandMessageMax * 2];
  escapeInto(id, sizeof(id), log.command_id);
  escapeInto(message, sizeof(message), log.message);

  char json[kCommandIdMax * 2 + kCommandMessageMax * 2 + 128];
  const int length = std::snprintf(
      json, sizeof(json),
      // ⚠️ "terminal", NOT "final". The backend's correlation loop waits on
      // exactly this key and the Arduino firmware has always sent it; emitting a
      // differently-named field meant every command reported timed_out=true while
      // carrying a perfectly good answer in its records -- a failure that looks
      // like a dead device and is actually a spelling disagreement. Found only by
      // driving the real backend rather than the broker.
      "{\"schema_version\":\"nat.log.v1\",\"command_id\":\"%s\","
      "\"source\":\"sensor\",\"ok\":%s,\"terminal\":%s,\"message\":\"%s\"}",
      id, log.ok != 0 ? "true" : "false", log.final != 0 ? "true" : "false",
      message);
  if (length <= 0) {
    return;
  }
  if (uplinkSend(UplinkType::kCommandLog, log.device_id, json,
                 static_cast<size_t>(length))) {
    ++sCommandAnswersPublished;
  }
  ESP_LOGI(kTag, "answer for %s from device %" PRIu64 ": %s", log.command_id,
           log.device_id, log.message);
}

bool unicastPacket(const uint8_t *mac, PacketType type, const void *payload,
                   size_t payload_size) {
  uint8_t packet[kEnvelopeSize + sizeof(CommandFrame)];
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
  return esp_now_send(mac, packet, kEnvelopeSize + payload_size) == ESP_OK;
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

    // The held-out sample (#315). Sent on its own cadence, and NOT paired with a
    // follow-up: no leaf feeds it into a fit, so its transmit time does not need
    // to be known accurately. What matters is only that every leaf hears the
    // SAME wavefront, which a single broadcast guarantees for free.
    if (kMarkerEvery > 0 && beacon.seq % kMarkerEvery == 0) {
      SyncMarker marker{};
      marker.seq = beacon.seq;
      marker.epoch = sEpoch;
      broadcastPacket(PacketType::kSyncMarker, &marker, sizeof(marker));
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_NATKIT_TIME_BEACON_MS));
  }
}

}  // namespace

esp_err_t espNowPrimaryStart() {
  // Only the primary surveys; it is the one that chooses.
  ESP_ERROR_CHECK(startRadio(true));

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
           "primary up on channel %u as %02x:%02x:%02x:%02x:%02x:%02x, timing "
           "master for epoch %08lx, beacon + follow-up every 1s, tracking up to "
           "%u nodes",
           sChosenChannel, mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5], static_cast<unsigned long>(sEpoch), (unsigned)kMaxTrackedNodes);
  return ESP_OK;
}

const NodeState *espNowPrimaryNodes() { return sNodes; }

uint32_t espNowPrimaryUnknownPackets() { return sUnknownPackets; }
int8_t espNowPrimaryNoiseFloor() {
  return sPrimaryNoiseSeen ? sPrimaryNoiseWorst : 0;
}

const CoherenceStats &espNowPrimaryCoherence() { return sCoherence; }

// #315's deliverable: one bounded number, plus the honesty about where it came
// from.
//
// The two-tier shape is the point. `typical_us` is what a consumer should expect
// on an ordinary sample; `bound_us` is what it should not exceed, and it carries
// the tail explicitly rather than averaging it away -- because the question "can
// I compare these two sensors' samples" is answered by the worst case, not the
// median.
//
// `measured` is the other half of the honesty. With two or more leaves this is a
// real measurement of one wavefront against two clocks. With one leaf there is
// nothing to be coherent WITH, so it falls back to deriving a figure from the
// single node's own fit against the primary -- which is a weaker claim, and
// says so.
CoherenceMetric espNowPrimaryCoherenceMetric() {
  CoherenceMetric metric{};
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

  // The quality is the WORST of the contributing nodes, never an average: a
  // stream is only as comparable as its least synchronised member, and averaging
  // a locked node with an unsynced one produces a number that describes neither.
  uint8_t worst_quality = static_cast<uint8_t>(SyncQuality::kLocked);
  uint64_t newest = 0;
  bool any = false;
  for (const NodeState &node : sNodes) {
    if (!node.in_use || !node.sync_seen) {
      continue;
    }
    any = true;
    if (node.last_sync.quality < worst_quality) {
      worst_quality = node.last_sync.quality;
    }
    if (node.last_seen_us > newest) {
      newest = node.last_seen_us;
    }
  }
  if (!any) {
    return metric;
  }
  metric.quality = worst_quality;
  metric.stale_us = static_cast<uint32_t>(newest == 0 ? 0 : now - newest);

  if (sCoherence.markers_paired >= 2) {
    metric.measured = true;
    metric.samples = sCoherence.markers_paired;
    const double mean = static_cast<double>(sCoherence.spread_sum_us) /
                        static_cast<double>(sCoherence.markers_paired);
    const double mean_sq = static_cast<double>(sCoherence.spread_sum_sq) /
                           static_cast<double>(sCoherence.markers_paired);
    const double variance = mean_sq - mean * mean;
    const double sd = variance > 0.0 ? std::sqrt(variance) : 0.0;
    // The mean spread is folded into the typical figure rather than subtracted
    // out. Between two NODES a constant offset is not a common-mode nuisance to
    // be calibrated away -- it is exactly the disagreement being reported.
    const double bias = mean < 0.0 ? -mean : mean;
    metric.typical_us = static_cast<uint32_t>(bias + sd);
    metric.bound_us = static_cast<uint32_t>(bias + 3.0 * sd) +
                      static_cast<uint32_t>(sCoherence.excursion_worst_us);
    const int64_t worst = sCoherence.spread_max_us > -sCoherence.spread_min_us
                              ? sCoherence.spread_max_us
                              : -sCoherence.spread_min_us;
    metric.worst_seen_us = static_cast<uint32_t>(worst < 0 ? -worst : worst);
    return metric;
  }

  // Fallback: one leaf, or not enough paired markers yet. Derive from that node's
  // own measured error against the primary and DOUBLE it, because two such nodes
  // would each contribute one. Marked `measured = false`, which is the whole
  // reason that flag exists.
  for (const NodeState &node : sNodes) {
    if (!node.in_use || node.probe_error_count < 2) {
      continue;
    }
    const double mean = static_cast<double>(node.probe_error_sum_us) /
                        static_cast<double>(node.probe_error_count);
    const double mean_sq = static_cast<double>(node.probe_error_sum_sq) /
                           static_cast<double>(node.probe_error_count);
    const double variance = mean_sq - mean * mean;
    const double sd = variance > 0.0 ? std::sqrt(variance) : 0.0;
    // The BIAS is deliberately not included here: it is common-mode between two
    // leaves and cancels. That is a modelled claim rather than a measured one,
    // which is precisely why this branch reports measured = false.
    const uint32_t typical = static_cast<uint32_t>(2.0 * sd);
    if (typical > metric.typical_us) {
      metric.typical_us = typical;
      metric.bound_us = static_cast<uint32_t>(6.0 * sd) +
                        static_cast<uint32_t>(node.probe_excursion_worst_us);
      metric.samples = node.probe_error_count;
    }
  }
  return metric;
}

uint32_t espNowPrimaryEpoch() { return sEpoch; }

uint32_t espNowPrimaryBeaconSeq() { return sBeaconSeq; }

uint32_t espNowPrimaryBeaconsWithoutTxStamp() { return sBeaconsWithoutTxStamp; }

uint64_t espNowPrimaryLastTxTsf() { return sBeaconTxTsfUs; }

uint32_t espNowPrimaryCommandAnswersReceived() { return sCommandAnswersReceived; }
uint32_t espNowPrimaryCommandAnswersPublished() {
  return sCommandAnswersPublished;
}

uint32_t espNowPrimaryCommandAnswersDuplicate() {
  return sCommandAnswersDuplicate;
}

void espNowPrimaryPublishAnswer(const CommandLogFrame &log) {
  publishCommandLog(log);
}

bool espNowPrimarySendCommand(const CommandFrame &command) {
  const RegistryEntry *entries = registryEntries();
  for (size_t i = 0; i < kRegistryMaxNodes; ++i) {
    if (!entries[i].in_use || entries[i].device_id != command.device_id) {
      continue;
    }
    // ⚠️ THE PEER MUST EXIST BEFORE esp_now_send WILL UNICAST TO IT. The primary
    // only ever broadcast before this, so nothing had added leaves as peers, and
    // esp_now_send to an unknown MAC fails with ESP_ERR_ESPNOW_NOT_FOUND -- which
    // would have read as "the node is not there" rather than "we never introduced
    // ourselves".
    if (!esp_now_is_peer_exist(entries[i].mac)) {
      esp_now_peer_info_t peer{};
      std::memcpy(peer.peer_addr, entries[i].mac, 6);
      peer.channel = peerChannel();
      peer.ifidx = WIFI_IF_STA;
      peer.encrypt = false;
      const esp_err_t err = esp_now_add_peer(&peer);
      if (err != ESP_OK) {
        ESP_LOGE(kTag, "could not add peer for device %" PRIu64 ": %s",
                 command.device_id, esp_err_to_name(err));
        return false;
      }
    }
    const bool sent = unicastPacket(entries[i].mac, PacketType::kCommand,
                                    &command, sizeof(command));
    ESP_LOGI(kTag, "command \"%s\" -> device %" PRIu64 " (%s)", command.command,
             command.device_id, sent ? "sent" : "SEND FAILED");
    return sent;
  }
  ESP_LOGW(kTag,
           "command \"%s\" is for device %" PRIu64
           ", which is not in the registry -- refusing rather than dropping it",
           command.command, command.device_id);
  return false;
}

}  // namespace natkit
