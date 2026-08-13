#include <cinttypes>
#include <cmath>
#include <cstring>

#include "device_id.hpp"
#include "driver/gpio.h"
#include "esp_system.h"
#include "ethernet_net.hpp"
#include "gateway_net.hpp"
#include "registry.hpp"
#include "uplink.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "node_role.hpp"
#include "sdkconfig.h"
#include "time_sync.hpp"

// Primary: the ESP-NOW hub, the timing master, and the serial uplink.
//
// What is here is ONLY what TEC-NATKIT-24 needs in order to be verifiable -- its
// "done when" is "a leaf streams real sensor data to a primary that logs it", and
// nothing could log it. So this receives natKit packets, derives each node's id
// from its MAC, reads each frame's own seqNo to count real losses, and prints what
// it sees. It also beacons so leaves can discover it.
//
// What is deliberately NOT here, and stays with its own slice:
//   TEC-NATKIT-25  the node registry as a real thing (persistence, the
//                  MAC-to-stream-id mapping the gateway contract needs), the serial
//                  mux for N leaves down one link, and backpressure.
//   #340           the 1-second ESP-NOW timing broadcast and the time-shift proxy.
//                  The beacon here already runs at 1s, so that slice should replace
//                  it rather than sit alongside it.
//
// There is no reassembly and there never will be: TEC-NATKIT-23 measured the frame
// at 524 bytes against a 1470-byte ESP-NOW ceiling and decided one frame is one
// packet, so a lost packet is a whole missing frame that seqNo makes detectable.
//
// Budget the serial link before designing the mux: natVR already hit a ceiling at
// ~960-byte JSON frames at 20 fps (~19 KB/s) against a 115200 console UART
// (~11.5 KB/s), and the pipeline stalled. N leaves through one hub through one link
// is the same arithmetic with a bigger N. At the measured leaf rate (2.5 KB/s) that
// is ~4 nodes at 115200 and ~35 at 921600.

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-primary";

// #373: this image runs ESP-NOW and an associated WiFi station on one radio, and
// publishes to the broker itself. An unset bool Kconfig emits no symbol, so it is
// resolved to a constant once rather than read as a value.
#ifdef CONFIG_NATKIT_PRIMARY_WIFI_UPLINK
constexpr bool kWifiUplink = true;
#else
constexpr bool kWifiUplink = false;
#endif

// The wired alternative, and the one that does not fight the radio.
#ifdef CONFIG_NATKIT_PRIMARY_ETH_UPLINK
constexpr bool kEthUplink = true;
#else
constexpr bool kEthUplink = false;
#endif

const char *accuracyName(uint8_t accuracy) {
  switch (accuracy) {
    case 0:
      return "unreliable";
    case 1:
      return "low";
    case 2:
      return "medium";
    case 3:
      return "high";
    default:
      return "?";
  }
}

const char *syncQualityName(uint8_t quality) {
  switch (static_cast<SyncQuality>(quality)) {
    case SyncQuality::kUnsynced:
      return "UNSYNCED";
    case SyncQuality::kCoarse:
      return "coarse";
    case SyncQuality::kLocked:
      return "locked";
    default:
      return "?";
  }
}

// Fills the per-node status the gateway needs, INCLUDING that node's clock fit --
// without which a data frame's device-relative timestamps cannot be turned into
// anything publishable, because the leaf deliberately does not rewrite its own.
void fillNodeStatus(const NodeState &node, UplinkNodeStatus &out) {
  out = UplinkNodeStatus{};
  out.device_id = node.device_id;
  std::memcpy(out.mac, node.mac, 6);
  out.data_frames = node.data_frames;
  out.seq_gaps = node.seq_gaps;
  out.seq_duplicates = node.seq_duplicates;
  out.seq_restarts = node.seq_restarts;
  out.heartbeats = node.heartbeats;
  out.last_seen_us = node.last_seen_us;
  out.sync = node.last_sync;
  out.sync_valid = node.sync_seen ? 1 : 0;
}

void fillPrimaryStatus(UplinkPrimaryStatus &out) {
  out = UplinkPrimaryStatus{};
  out.device_id = deviceId();
  out.uptime_us = static_cast<uint64_t>(esp_timer_get_time());
  out.epoch = espNowPrimaryEpoch();
  out.free_heap = static_cast<uint32_t>(esp_get_free_heap_size());
  out.min_free_heap = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
  out.nodes_known = registryCount();
  out.nodes_rejected = registryRejections();
  out.unknown_packets = espNowPrimaryUnknownPackets();

  const UplinkStats &up = uplinkStats();
  out.frames_queued = up.frames_queued;
  out.frames_sent = up.frames_sent;
  out.frames_dropped = up.frames_dropped;
  out.write_timeouts = up.write_timeouts;
  out.bytes_sent = up.bytes_sent;

  const CoherenceMetric metric = espNowPrimaryCoherenceMetric();
  out.coherence_typical_us = metric.typical_us;
  out.coherence_bound_us = metric.bound_us;
  out.coherence_worst_us = metric.worst_seen_us;
  out.coherence_samples = metric.samples;
  out.coherence_quality = metric.quality;
  out.coherence_measured = metric.measured ? 1 : 0;
  out.registry_sealed = registrySealed() ? 1 : 0;
}

}  // namespace

void runPrimary() {
  ESP_LOGI(kTag, "primary: device %" PRIu64 ", ESP-NOW hub", deviceId());

  // Registry BEFORE the radio, so the first packet to arrive is already judged
  // against the roster rather than admitted because we had not finished loading.
  registryLoad();

#if CONFIG_NATKIT_HOLD_RCP_IN_RESET
  // Hold the ESP32-H2 radio co-processor in reset.
  //
  // ⚠️ This is an EXPERIMENT with a measurement attached, not a tidy-up. The ESP
  // Thread Border Router board carries an H2 a few millimetres from the S3, and
  // an H2 running stock RCP firmware transmits 802.15.4 in the SAME 2.4 GHz band
  // ESP-NOW uses. Measured on this board: the S3's transmits are heard fine by
  // the leaves (26 beacons seen, clock locked) while the leaves' unicasts to it
  // almost all fail to be acknowledged -- a transmitter that works and a
  // receiver that is deaf, which is what an in-band interferer inches away looks
  // like. Disabling the Ethernet uplink changed nothing, so the W5500 is not it.
  //
  // If holding the H2 down fixes reception, the co-processor is the cause and
  // this board needs it managed rather than ignored.
  gpio_config_t rcp{};
  rcp.pin_bit_mask = 1ULL << CONFIG_NATKIT_RCP_RESET_GPIO;
  rcp.mode = GPIO_MODE_OUTPUT;
  gpio_config(&rcp);
  gpio_set_level(static_cast<gpio_num_t>(CONFIG_NATKIT_RCP_RESET_GPIO), 0);
  ESP_LOGW(kTag,
           "holding the ESP32-H2 co-processor in reset on GPIO %d -- testing "
           "whether it is desensitising this board's ESP-NOW receiver",
           CONFIG_NATKIT_RCP_RESET_GPIO);
#endif

  // #373: associate FIRST, before esp_now_init, because the association owns the
  // radio's channel and ESP-NOW has to follow it. Doing this the other way round
  // gives an ESP-NOW hub pinned to a channel the AP then moves it off, which is
  // silent -- sends succeed and nothing arrives.
  if (kWifiUplink) {
    if (gatewayWifiStart() != ESP_OK) {
      ESP_LOGE(kTag, "WiFi did not start; continuing as an ESP-NOW hub only");
    }
  }
  if (kEthUplink) {
    // Before esp_now_init for the same reason as the WiFi path: the netif and
    // event loop should exist before anything else wants them. Unlike WiFi
    // there is nothing here for ESP-NOW to inherit -- no channel, no
    // association, no shared airtime.
    if (ethernetStart() != ESP_OK) {
      ESP_LOGE(kTag, "Ethernet did not start; continuing as an ESP-NOW hub only");
    }
  }

  if (uplinkStart() != ESP_OK) {
    ESP_LOGE(kTag,
             "uplink did not start -- continuing as a hub so the console still "
             "shows what the radio is doing");
  }

  if (espNowPrimaryStart() != ESP_OK) {
    ESP_LOGE(kTag, "ESP-NOW did not start");
    idleStatusLoop("primary (no radio)");
  }

  if (kEthUplink) {
    // The SAME services as the WiFi path, unchanged: SNTP and MQTT are
    // netif-agnostic, so the whole publish chain proven end-to-end into Kafka on
    // TEC-NATKIT-26 works here with no modification. That is the payoff for
    // having split gatewayWifiStart from gatewayServicesStart.
    if (gatewayServicesStart() != ESP_OK) {
      ESP_LOGE(kTag, "MQTT/SNTP did not start; nodes still land on the console");
    }
    ESP_LOGW(kTag,
             "ETHERNET UPLINK IS ON: one chip running the ESP-NOW hub, the "
             "timing master and a WIRED uplink, with no radio contention -- "
             "which is the thing #373 could not achieve over WiFi.");
  }

  if (kWifiUplink) {
    // Services after ESP-NOW: SNTP and MQTT both need the association, and
    // neither needs to exist before the radio is hearing nodes.
    if (gatewayServicesStart() != ESP_OK) {
      ESP_LOGE(kTag, "MQTT/SNTP did not start; nodes still land on the console");
    }
    ESP_LOGW(kTag,
             "#373 WIFI UPLINK IS ON: this one chip is running ESP-NOW and an "
             "associated WiFi station at once, and publishing straight to the "
             "broker. No serial link and no gateway. Watch the ESP-NOW counters "
             "against the two-board baselines -- that comparison is the point.");
  }

  const uint32_t interval_s =
      CONFIG_NATKIT_STATUS_LOG_INTERVAL_S > 0
          ? static_cast<uint32_t>(CONFIG_NATKIT_STATUS_LOG_INTERVAL_S)
          : 10;
  // Once a second regardless of the status interval: this is the instrument for the
  // leaf slice, and a 10-second window hides a stall.
  (void)interval_s;

  uint32_t previous_frames[kMaxTrackedNodes] = {};
  uint32_t ticks = 0;
  uint64_t next_status_us = 0;

  // How long this loop's own console output blocks, and the worst seen.
  //
  // Not idle curiosity: the probe's sync error shows occasional millisecond
  // excursions, and the leading hypothesis is that they are the ESP-NOW receive
  // callback being late because ESP_LOGI is blocking on a full UART FIFO -- the
  // same mechanism that made a leaf's report rate appear to collapse in the
  // TEC-NATKIT-22 soak. Measuring it turns that from a plausible story into an
  // arithmetic check: at one probe per second, a console that blocks for X ms per
  // second should collide with roughly X/1000 of them.
  uint32_t console_us = 0;
  uint32_t console_worst_us = 0;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    const uint64_t console_started = static_cast<uint64_t>(esp_timer_get_time());
    const NodeState *nodes = espNowPrimaryNodes();
    bool any = false;

    for (size_t i = 0; i < kMaxTrackedNodes; ++i) {
      const NodeState &node = nodes[i];
      if (!node.in_use) {
        continue;
      }
      any = true;

      const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
      const uint64_t silent_ms =
          node.last_seen_us == 0 ? 0 : (now - node.last_seen_us) / 1000;
      const uint32_t delta = node.data_frames - previous_frames[i];
      previous_frames[i] = node.data_frames;

      ESP_LOGI(kTag,
               "node %" PRIu64 " (%02x:%02x:%02x:%02x:%02x:%02x): %lu data frames "
               "(+%lu/s), %lu B, seq %llu, gaps %lu, dupes %lu, restarts %lu, "
               "silent %llu ms | last frame %u samples @ %lu Hz declared",
               node.device_id, node.mac[0], node.mac[1], node.mac[2], node.mac[3],
               node.mac[4], node.mac[5],
               static_cast<unsigned long>(node.data_frames),
               static_cast<unsigned long>(delta),
               static_cast<unsigned long>(node.bytes),
               static_cast<unsigned long long>(node.last_seq),
               static_cast<unsigned long>(node.seq_gaps),
               static_cast<unsigned long>(node.seq_duplicates),
               static_cast<unsigned long>(node.seq_restarts),
               static_cast<unsigned long long>(silent_ms), node.last_sample_count,
               static_cast<unsigned long>(node.last_declared_rate));

      // --- the time-shift proxy, and its two instruments (#340) -------------
      if (node.publish_no_sync || node.publish_no_time || node.publish_no_shift) {
        ESP_LOGW(kTag,
                 "  NOT PUBLISHED: %lu no leaf fit, %lu no wall clock, %lu "
                 "rewrite refused -- these never reached the uplink queue",
                 static_cast<unsigned long>(node.publish_no_sync),
                 static_cast<unsigned long>(node.publish_no_time),
                 static_cast<unsigned long>(node.publish_no_shift));
      }
      if (node.rssi_seen) {
        ESP_LOGI(kTag, "  rssi %d dBm (best %d, worst %d) over %lu packets",
                 node.rssi_last, node.rssi_best, node.rssi_worst,
                 static_cast<unsigned long>(node.data_frames + node.announces +
                                            node.heartbeats));
      }
      if (node.sync_seen) {
        const SyncState &sync = node.last_sync;
        ESP_LOGI(kTag,
                 "  clock: leaf reports %s, epoch %08lx%s | offset %+lld us, "
                 "skew %+ld ppb | fit %u pts, residual %lu ns rms | beacons %lu "
                 "seen / %lu missed, %lu outliers",
                 syncQualityName(sync.quality),
                 static_cast<unsigned long>(sync.epoch),
                 sync.epoch == espNowPrimaryEpoch() ? "" : " (NOT OURS)",
                 static_cast<long long>(sync.ref_offset_us),
                 static_cast<long>(sync.skew_ppb),
                 static_cast<unsigned>(sync.samples_used),
                 static_cast<unsigned long>(sync.residual_rms_ns),
                 static_cast<unsigned long>(sync.beacons_seen),
                 static_cast<unsigned long>(sync.beacons_missed),
                 static_cast<unsigned long>(sync.outliers_rejected));
      }

      if (node.probe_error_count > 0) {
        // THE number for this slice: measured minus predicted, and the leaf's own
        // fit did not produce it. RMS in integer microseconds via the running
        // sum of squares -- no history buffer, so a soak's length does not size
        // anything.
        const int64_t mean =
            node.probe_error_sum_us / static_cast<int64_t>(node.probe_error_count);
        // Standard deviation, not RMS about zero. The error has a real constant
        // BIAS -- the two directions' callback latencies do not cancel -- and an
        // RMS about zero folds that bias into what is supposed to describe the
        // scatter, reporting ~170 us of "noise" for data whose 5th-to-95th
        // percentile spans 101 us. The bias is the mean, printed beside it, and
        // it is common-mode: it shifts every node the same way, so it very
        // largely cancels between two leaves, which is the comparison that
        // actually matters.
        const double mean_sq =
            static_cast<double>(node.probe_error_sum_sq) /
            static_cast<double>(node.probe_error_count);
        const double variance =
            mean_sq - static_cast<double>(mean) * static_cast<double>(mean);
        const uint32_t rms = static_cast<uint32_t>(
            variance > 0.0 ? std::sqrt(variance) : 0.0);
        ESP_LOGI(kTag,
                 "  sync error (measured - predicted): now %+lld us | mean "
                 "%+lld us (bias), sd %lu us over %lu typical probes | range "
                 "%+lld .. %+lld us | %lu excursions (worst %lld us off mean), "
                 "%lu unpredictable, %lu orphaned",
                 static_cast<long long>(node.probe_error_us),
                 static_cast<long long>(mean), static_cast<unsigned long>(rms),
                 static_cast<unsigned long>(node.probe_error_count),
                 static_cast<long long>(node.probe_error_min_us),
                 static_cast<long long>(node.probe_error_max_us),
                 static_cast<unsigned long>(node.probe_excursions),
                 static_cast<long long>(node.probe_excursion_worst_us),
                 static_cast<unsigned long>(node.probes_unpredictable),
                 static_cast<unsigned long>(node.probes_orphaned));

        // What the rolling fit buys over syncing once at startup. This is the
        // number that grows with the length of a recording, which is exactly the
        // failure a single snapshot comparison cannot show.
        ESP_LOGI(kTag,
                 "  vs sync-once-at-startup: naive error now %+lld us, worst "
                 "%lld us | rolling fit worst %lld us",
                 static_cast<long long>(node.naive_error_us),
                 static_cast<long long>(node.naive_error_worst_us),
                 static_cast<long long>(
                     node.probe_error_max_us - node.probe_error_min_us));
      }

      if (node.delta_seen) {
        // ⚠️ THIS IS NOT THE ACCURACY FIGURE, and it was measured before it was
        // labelled. It shows that the shift is being APPLIED to real frames --
        // the ~21 ms between the raw and shifted columns is the correction doing
        // its job -- but its spread says nothing about how good the clock fit is.
        //
        // Two reasons, both measured: the frame's timestamp is the FIRST of ten
        // samples spanning 200 ms and the batch's span jitters with the sensor's
        // own report timing, which put a ~74 ms range on both columns; and that
        // timestamp is quantised to milliseconds by the encoder
        // (`sample.time_ms = newest_us / 1000`), three orders of magnitude above
        // what is being estimated. The probe line above is the measurement.
        ESP_LOGI(kTag,
                 "  frame shift applied (arrival - sampled, NOT an accuracy "
                 "figure -- leaf batching dominates it): raw %+lld us -> shifted "
                 "%s%+lld us | %lu frames arrived before the leaf had a fit",
                 static_cast<long long>(node.raw_delta_us),
                 node.shift_valid ? "" : "(stale) ",
                 static_cast<long long>(node.shifted_delta_us),
                 static_cast<unsigned long>(node.shift_failures));
      }

      if (node.heartbeat_seen) {
        const Heartbeat &beat = node.last_heartbeat;
        // The leaf's own view, which is what makes a drop attributable: frames it
        // dropped from a full queue are ITS back-pressure, not our loss, and the
        // two are indistinguishable from the receiving side alone.
        ESP_LOGI(kTag,
                 "  heartbeat: up %llu s, built %lu, sent %lu, dropped %lu, tx "
                 "fail %lu, reports %lu, hub resets %lu, heap %lu | accel %s gyro "
                 "%s mag %s rot %s",
                 static_cast<unsigned long long>(beat.uptime_us / 1000000ULL),
                 static_cast<unsigned long>(beat.frames_built),
                 static_cast<unsigned long>(beat.frames_sent),
                 static_cast<unsigned long>(beat.frames_dropped),
                 static_cast<unsigned long>(beat.send_failures),
                 static_cast<unsigned long>(beat.sensor_reports),
                 static_cast<unsigned long>(beat.hub_resets),
                 static_cast<unsigned long>(beat.free_heap),
                 accuracyName(beat.accuracy_accel),
                 accuracyName(beat.accuracy_gyro),
                 accuracyName(beat.accuracy_mag),
                 accuracyName(beat.accuracy_rotation));
      }
    }

    // --- node-to-node coherence, and the metric #315 asks for ---------------
    if (any) {
      const CoherenceStats &c = espNowPrimaryCoherence();
      const CoherenceMetric m = espNowPrimaryCoherenceMetric();

      if (c.markers_paired >= 2) {
        const double mean = static_cast<double>(c.spread_sum_us) /
                            static_cast<double>(c.markers_paired);
        const double mean_sq = static_cast<double>(c.spread_sum_sq) /
                               static_cast<double>(c.markers_paired);
        const double variance = mean_sq - mean * mean;
        const uint32_t sd =
            static_cast<uint32_t>(variance > 0.0 ? std::sqrt(variance) : 0.0);
        // One broadcast wavefront, two clocks, and the difference between what
        // they each say the time was. No model in it.
        ESP_LOGI(kTag,
                 "coherence %" PRIu64 " vs %" PRIu64
                 ": spread now %+lld us | mean %+lld us, sd %lu us over %lu "
                 "paired markers | range %+lld .. %+lld us | %lu excursions "
                 "(worst %lld us)",
                 c.device_a, c.device_b,
                 static_cast<long long>(c.spread_us),
                 static_cast<long long>(mean), static_cast<unsigned long>(sd),
                 static_cast<unsigned long>(c.markers_paired),
                 static_cast<long long>(c.spread_min_us),
                 static_cast<long long>(c.spread_max_us),
                 static_cast<unsigned long>(c.excursions),
                 static_cast<long long>(c.excursion_worst_us));
      }

      if (m.typical_us > 0 || m.measured) {
        ESP_LOGI(kTag,
                 "TIME COHERENCE METRIC: %s, typical %lu us, bound %lu us, "
                 "worst seen %lu us | %s from %lu samples | newest node data "
                 "%lu ms old",
                 syncQualityName(m.quality),
                 static_cast<unsigned long>(m.typical_us),
                 static_cast<unsigned long>(m.bound_us),
                 static_cast<unsigned long>(m.worst_seen_us),
                 m.measured ? "MEASURED node-to-node against a held-out marker"
                            : "DERIVED from one node's fit (no second node to be "
                              "coherent with)",
                 static_cast<unsigned long>(m.samples),
                 static_cast<unsigned long>(m.stale_us / 1000));
      }
    }

    if (!any) {
      ESP_LOGI(kTag,
               "no nodes yet (timing broadcast at beacon %lu of epoch %08lx; "
               "%lu foreign/unhandled packets)",
               static_cast<unsigned long>(espNowPrimaryBeaconSeq()),
               static_cast<unsigned long>(espNowPrimaryEpoch()),
               static_cast<unsigned long>(espNowPrimaryUnknownPackets()));
    }

    // The timing master's own health, every tenth pass. The TSF reading is here
    // because #340's first recommended approach rests on it: the IDF documents
    // esp_wifi_get_tsf_time as returning 0 on an unassociated station, and no
    // node in this architecture ever associates, so this line is where that stops
    // being a citation and becomes a measurement.
    //
    // Counted here rather than off the beacon sequence: this loop and the beacon
    // both run at 1 Hz but are not the same task, so a modulo of the beacon
    // number would print twice in one second and then not at all in the next.
    if (++ticks % 10 == 0) {
      ESP_LOGI(kTag,
               "timing master: beacon %lu, epoch %08lx, %lu beacons with no tx "
               "stamp | esp_wifi_get_tsf_time = %llu (0 is expected and is the "
               "finding: TSF needs an association we deliberately never make) | "
               "this console blocked %lu us last pass, %lu us worst",
               static_cast<unsigned long>(espNowPrimaryBeaconSeq()),
               static_cast<unsigned long>(espNowPrimaryEpoch()),
               static_cast<unsigned long>(espNowPrimaryBeaconsWithoutTxStamp()),
               static_cast<unsigned long long>(espNowPrimaryLastTxTsf()),
               static_cast<unsigned long>(console_us),
               static_cast<unsigned long>(console_worst_us));
    }

    console_us = static_cast<uint32_t>(
        static_cast<uint64_t>(esp_timer_get_time()) - console_started);
    if (console_us > console_worst_us) {
      console_worst_us = console_us;
    }

    // --- telemetry down the uplink ------------------------------------------
    //
    // Sent on its own cadence rather than with the console lines, because the
    // console is a human convenience and this is the gateway's only view of what
    // the primary discarded. The two must not share a fate.
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    if (now_us >= next_status_us) {
      next_status_us =
          now_us + static_cast<uint64_t>(CONFIG_NATKIT_UPLINK_STATUS_MS) * 1000ULL;

      const NodeState *status_nodes = espNowPrimaryNodes();
      for (size_t i = 0; i < kMaxTrackedNodes; ++i) {
        if (!status_nodes[i].in_use) {
          continue;
        }
        UplinkNodeStatus node_status{};
        fillNodeStatus(status_nodes[i], node_status);
        uplinkSend(UplinkType::kNodeStatus, status_nodes[i].device_id,
                   &node_status, sizeof(node_status));
      }

      UplinkPrimaryStatus primary_status{};
      fillPrimaryStatus(primary_status);
      uplinkSend(UplinkType::kPrimaryStatus, deviceId(), &primary_status,
                 sizeof(primary_status));
    }

    // The wired uplink's health. Without this the Ethernet path was INVISIBLE --
    // no link state, no MQTT state, no clock state -- which is how a publish path
    // discarding 75% of its frames went unnoticed while every radio counter read
    // perfectly.
    if (kEthUplink && ticks % 5 == 0) {
      const EthernetStats &eth = ethernetStats();
      const GatewayNetStats &net = gatewayNetStats();
      ESP_LOGI(kTag,
               "WIRED UPLINK: link %s, ip %lu.%lu.%lu.%lu, mqtt %s, clock %s | "
               "published %lu (%llu B), refused %lu | link drops %lu, mqtt drops "
               "%lu",
               eth.link_up ? "up" : "DOWN",
               static_cast<unsigned long>(eth.ip & 0xFF),
               static_cast<unsigned long>((eth.ip >> 8) & 0xFF),
               static_cast<unsigned long>((eth.ip >> 16) & 0xFF),
               static_cast<unsigned long>((eth.ip >> 24) & 0xFF),
               net.mqtt_connected ? "up" : "DOWN",
               gatewayTimeValid() ? "SYNCED" : "NOT SYNCED",
               static_cast<unsigned long>(net.publishes_ok),
               static_cast<unsigned long long>(net.bytes_published),
               static_cast<unsigned long>(net.publishes_failed),
               static_cast<unsigned long>(eth.link_downs),
               static_cast<unsigned long>(net.mqtt_disconnects));
    }

    // #373's headline line: the one chip's two jobs, side by side. The channel
    // is the number the whole question turns on -- if it is not the channel the
    // leaves are on, there is no hub as far as they are concerned.
    if (kWifiUplink && ticks % 5 == 0) {
      const GatewayNetStats &net = gatewayNetStats();
      ESP_LOGI(kTag,
               "ONE-CHIP UPLINK: wifi %s (rssi %d, CHANNEL %u), mqtt %s, clock "
               "%s | published %lu (%llu B), refused %lu | wifi drops %lu, mqtt "
               "drops %lu",
               net.wifi_connected ? "up" : "DOWN", net.rssi,
               gatewayWifiChannel(), net.mqtt_connected ? "up" : "DOWN",
               gatewayTimeValid() ? "synced" : "NOT SYNCED",
               static_cast<unsigned long>(net.publishes_ok),
               static_cast<unsigned long long>(net.bytes_published),
               static_cast<unsigned long>(net.publishes_failed),
               static_cast<unsigned long>(net.wifi_disconnects),
               static_cast<unsigned long>(net.mqtt_disconnects));
    }

    // The uplink's own health on the console too, since a gateway that is not
    // reading is otherwise invisible from this end.
    if (ticks % 10 == 0) {
      const UplinkStats &up = uplinkStats();
      ESP_LOGI(kTag,
               "uplink: %lu frames queued, %lu sent (%llu B), %lu DROPPED, %lu "
               "write timeouts, queue high water %lu/%d | registry %s, %lu "
               "node(s), %lu rejected",
               static_cast<unsigned long>(up.frames_queued),
               static_cast<unsigned long>(up.frames_sent),
               static_cast<unsigned long long>(up.bytes_sent),
               static_cast<unsigned long>(up.frames_dropped),
               static_cast<unsigned long>(up.write_timeouts),
               static_cast<unsigned long>(up.queue_high_water),
               CONFIG_NATKIT_UPLINK_QUEUE_DEPTH,
               registrySealed() ? "SEALED" : "open",
               static_cast<unsigned long>(registryCount()),
               static_cast<unsigned long>(registryRejections()));
    }
  }
}

}  // namespace natkit
