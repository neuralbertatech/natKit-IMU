#include <cinttypes>
#include <cmath>
#include <cstring>

#include "device_id.hpp"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "driver/gpio.h"
#include "esp_system.h"
#include "ethernet_net.hpp"
#include "gateway_net.hpp"
#include "command_relay.hpp"
#include "registry.hpp"
#include "uplink.hpp"
#include "uplink_reader.hpp"
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

// --- the hub's die temperature (TEC-NATKIT-50) -------------------------------
//
// ⚠️ HERE BECAUSE THE LEADING EXPLANATION IS THE PRIMARY'S TRANSMIT PATH. The
// 2026-08-18 impairment was entirely hub->leaf: four leaves lost up to 65% of the
// hub's beacons and the same fraction of its MAC acknowledgements, while leaf->hub
// lost 0 of 25,199 frames. Every leaf is downstream of ONE transmitter, which is
// why "shared and simultaneous" needs no coincidence -- and a transmit path that
// degrades for tens of minutes at a time is thermal until proven otherwise.
//
// Not available on every target (the classic ESP32 has no such sensor), so this is
// compiled out rather than faked, and reports -128 where it does not exist. A
// plausible-looking 25 C from a chip with no sensor would be worse than nothing.
#if SOC_TEMP_SENSOR_SUPPORTED
temperature_sensor_handle_t sTempSensor = nullptr;
// ⚠️ WHY IT IS NOT WORKING, PUBLISHED. The first version of this reported only
// -128 ("unavailable"), which on a board whose USB console RESETS it is
// indistinguishable from "this target has no sensor" and from "nobody wired it up".
// A silent unavailable is the exact failure mode this rig keeps re-learning, so the
// esp_err_t travels out in a byte the status struct already reserved.
uint8_t sTempErr = 0;

void primaryTempSensorStart() {
  // ⚠️ -10..80, NOT an arbitrary span. The sensor has a set of PREDEFINED range
  // buckets (50..125, 20..100, -10..80, -30..50, -40..20) and a request that
  // straddles two of them is rejected with ESP_ERR_INVALID_ARG. The first attempt
  // here asked for -10..110, which spans three, and reported "unavailable" -- found
  // only because the error code is published (chip_temp_err read 2 = 0x102).
  // -10..80 covers every die temperature this board can survive.
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  esp_err_t err = temperature_sensor_install(&cfg, &sTempSensor);
  if (err == ESP_OK) {
    err = temperature_sensor_enable(sTempSensor);
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "die temperature sensor did not start (%s); will retry",
             esp_err_to_name(err));
    sTempSensor = nullptr;
  }
  sTempErr = static_cast<uint8_t>(err & 0xff);
}

int8_t primaryChipTempC() {
  // Retried rather than given up on: this is started before the radio, and the PHY
  // claims the same sensor on some targets, so an ordering conflict at boot should
  // not cost the measurement for the whole uptime.
  if (sTempSensor == nullptr) {
    primaryTempSensorStart();
    if (sTempSensor == nullptr) {
      return -128;
    }
  }
  float celsius = 0.0f;
  const esp_err_t err = temperature_sensor_get_celsius(sTempSensor, &celsius);
  if (err != ESP_OK) {
    sTempErr = static_cast<uint8_t>(err & 0xff);
    return -128;
  }
  sTempErr = 0;
  if (celsius < -127.0f || celsius > 126.0f) {
    return -128;
  }
  return static_cast<int8_t>(celsius);
}
uint8_t primaryTempErr() { return sTempErr; }
#else
void primaryTempSensorStart() {}
int8_t primaryChipTempC() { return -128; }
uint8_t primaryTempErr() { return 0xff; }  // 0xff = no sensor on this target
#endif

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
  out.rssi_last = node.rssi_last;
  out.rssi_best = node.rssi_best;
  out.rssi_worst = node.rssi_worst;
  out.rssi_seen = node.rssi_seen ? 1 : 0;
  if (node.heartbeat_seen) {
    out.leaf_frames_built = node.last_heartbeat.frames_built;
    out.leaf_frames_dropped = node.last_heartbeat.frames_dropped;
    out.leaf_send_failures = node.last_heartbeat.send_failures;
    out.leaf_channel_hops = node.last_heartbeat.channel_hops;
    out.leaf_scan_channel = node.last_heartbeat.scan_channel;
    out.leaf_rssi_of_primary = node.last_heartbeat.rssi_of_primary;
    out.leaf_tx_power_quarter_dbm = node.last_heartbeat.tx_power_quarter_dbm;
    out.leaf_noise_floor_dbm = node.last_heartbeat.noise_floor_dbm;
  }
  out.publish_no_sync = node.publish_no_sync;
  out.publish_no_shift = node.publish_no_shift;
  // The raw probe accumulators, so a consumer can window them (TEC-NATKIT-52).
  out.probe_error_sum_us = node.probe_error_sum_us;
  out.probe_error_sum_sq = node.probe_error_sum_sq;
  out.probe_error_count = node.probe_error_count;
}

// --- presence, as against the roster (TEC-NATKIT-81) -------------------------
//
// A leaf stopped talking to this hub and the hub did not notice: it kept
// composing a status frame for it once a second out of the state the node had
// when it went away, and `nodes_known` -- the persistent registry count -- went
// on saying 4. Every visible number described a complete rig while a board sat
// dead on the bench.
//
// The fix is NOT to age the node out. Two things would break: the roster is what
// a seal freezes, so a sealed rig would evict the node it exists to accept and
// then refuse it on its return; and dropping its NodeState would stop the status
// frame entirely, which loses the only evidence that the node was ever here. The
// panel's "last heard 2m ago" is worth more than silence, because silence cannot
// be told from a node that never existed.
//
// So the hub keeps publishing, and reports separately how many nodes it can
// actually hear.
constexpr uint64_t kPresenceTimeoutUs =
    static_cast<uint64_t>(CONFIG_NATKIT_NODE_PRESENCE_TIMEOUT_MS) * 1000ULL;

// ⚠️ last_seen_us == 0 means never heard, not "heard at time zero". An entry can
// be in_use with no packet behind it only transiently, but treating 0 as recent
// at boot would report a node present before it has said anything.
bool nodeIsPresent(const NodeState &node, uint64_t now_us) {
  if (!node.in_use || node.last_seen_us == 0) {
    return false;
  }
  // Saturating: the primary's esp_timer clock is monotonic, so now < last_seen
  // should be impossible, but an unsigned wrap here would report a dead node as
  // present forever, which is the exact failure this is here to end.
  return now_us >= node.last_seen_us &&
         (now_us - node.last_seen_us) <= kPresenceTimeoutUs;
}

uint8_t countNodesPresent(uint64_t now_us) {
  const NodeState *nodes = espNowPrimaryNodes();
  uint8_t present = 0;
  for (size_t i = 0; i < kMaxTrackedNodes; ++i) {
    if (nodeIsPresent(nodes[i], now_us)) {
      ++present;
    }
  }
  return present;
}

void fillPrimaryStatus(UplinkPrimaryStatus &out) {
  out = UplinkPrimaryStatus{};
  const CommandRelayStats &commands = commandRelayStats();
  out.commands_received = commands.received;
  out.commands_relayed = commands.relayed;
  out.commands_malformed = commands.malformed;
  out.commands_unknown_device = commands.unknown_device;
  out.commands_send_failed = commands.send_failed;
  out.command_subscriptions = commands.subscriptions;
  out.command_answers_received = espNowPrimaryCommandAnswersReceived();
  out.command_answers_published = espNowPrimaryCommandAnswersPublished();
  out.command_answers_duplicate = espNowPrimaryCommandAnswersDuplicate();
  out.commands_delivered = commands.delivered;
  out.command_retransmits = commands.retransmits;
  out.commands_undelivered = commands.undelivered;
  out.reset_reason = static_cast<uint32_t>(esp_reset_reason());
  out.device_id = deviceId();
  out.uptime_us = static_cast<uint64_t>(esp_timer_get_time());
  out.epoch = espNowPrimaryEpoch();
  out.free_heap = static_cast<uint32_t>(esp_get_free_heap_size());
  out.min_free_heap = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
  out.nodes_known = registryCount();
  out.nodes_present =
      countNodesPresent(static_cast<uint64_t>(esp_timer_get_time()));
  out.nodes_present_valid = 1;
  out.nodes_rejected = registryRejections();
  out.unknown_packets = espNowPrimaryUnknownPackets();
  out.noise_floor_dbm = espNowPrimaryNoiseFloor();
  out.chip_temp_c = primaryChipTempC();
  out.chip_temp_err = primaryTempErr();

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
  // ...and the sums behind them, so "how good is the rig right now" is answerable
  // without resetting the primary to clear its history (TEC-NATKIT-52).
  {
    const CoherenceStats &coherence = espNowPrimaryCoherence();
    out.spread_sum_us = coherence.spread_sum_us;
    out.spread_sum_sq = coherence.spread_sum_sq;
    out.markers_paired = coherence.markers_paired;
  }
  out.coherence_quality = metric.quality;
  out.coherence_measured = metric.measured ? 1 : 0;
  out.registry_sealed = registrySealed() ? 1 : 0;
}

}  // namespace

// A frame arriving FROM the gateway. The only type that travels this way.
//
// ⚠️ Runs on the reader task, so it does the same thing the MQTT handler does:
// hand the document over and return. The relay owns a stack sized for parsing;
// this one is not it.
void onDownlinkFrame(UplinkType type, uint64_t stream_id,
                     const uint8_t *payload, size_t length) {
  if (type != UplinkType::kCommand) {
    // Nothing else is expected downward. Counted by the reader as a valid frame
    // either way, so a mistake here shows up as frames_ok climbing with no
    // commands relayed rather than as silence.
    return;
  }
  // ⚠️ stream_id is the addressed device, taken by the gateway from the topic.
  // Not re-derived from the document, which has no device field.
  commandRelaySubmit(stream_id, reinterpret_cast<const char *>(payload), length);
}

void runPrimary() {
  ESP_LOGI(kTag, "primary: device %" PRIu64 ", ESP-NOW hub", deviceId());

  // Registry BEFORE the radio, so the first packet to arrive is already judged
  // against the roster rather than admitted because we had not finished loading.
  registryLoad();
  primaryTempSensorStart();

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

  if (kEthUplink) {
    // ⚠️ SERVICES BEFORE THE RADIO, so NTP syncs WHILE the channel survey runs.
    //
    // The survey was added to fill the ~33 s in which NTP has not synced and
    // nothing can be published. Started after the radio, it did not fill that
    // window -- it QUEUED IN FRONT OF IT, pushing first publish from ~33 s to
    // ~60 s. The two only overlap if SNTP is already running when the survey
    // begins, and it can be: SNTP and MQTT are netif-agnostic and the Ethernet
    // link is already up by here.
    //
    // The same split that made this reorder possible is what let the whole
    // publish chain from TEC-NATKIT-26 be reused unchanged.
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

  if (espNowPrimaryStart() != ESP_OK) {
    ESP_LOGE(kTag, "ESP-NOW did not start");
    idleStatusLoop("primary (no radio)");
  }

  const uint32_t interval_s =
      CONFIG_NATKIT_STATUS_LOG_INTERVAL_S > 0
          ? static_cast<uint32_t>(CONFIG_NATKIT_STATUS_LOG_INTERVAL_S)
          : 10;
  // Once a second regardless of the status interval: this is the instrument for the
  // leaf slice, and a 10-second window hides a stall.
  (void)interval_s;

  uint32_t previous_frames[kMaxTrackedNodes] = {};
  // Whether each node was present on the PREVIOUS pass, so the console says
  // something once when a node leaves or returns instead of printing a "silent
  // 4,215,880 ms" that scrolls past with everything else. An operator reads an
  // edge; nobody reads a gauge that has been wrong for an hour.
  bool was_present[kMaxTrackedNodes] = {};
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

  // ⚠️ AFTER the radio, so the registry is loaded and any node that announced
  // during startup is already subscribable. Refreshed every second below, because
  // a node that announces later would otherwise be uncommandable until reboot.
  commandRelayStart();

  // The downward command path (TEC-NATKIT-92). Behind a gateway this primary has
  // no broker session at all, so a command reaches it as a kCommand frame on the
  // uplink's RX -- the pin claimed at design time for exactly this, and until now
  // never read from.
  //
  // ⚠️ Only when there IS a wire. Under the Ethernet and #373 WiFi uplinks the
  // exit is the radio, uplinkUartEnsure() installs nothing, and commands arrive
  // by subscription as they always did.
  if (!kWifiUplink && !kEthUplink) {
    if (uplinkReaderStart(onDownlinkFrame) != ESP_OK) {
      ESP_LOGE(kTag,
               "downlink reader did not start -- device commands cannot reach "
               "this rig, though data will keep flowing out");
    }
  }

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    commandRelayRefreshSubscriptions();

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

      // --- the hub noticing, once, that a node left or came back -------------
      //
      // ⚠️ THIS IS THE HALF THAT WAS MISSING (TEC-NATKIT-81). The "silent N ms"
      // figure below has always been correct and has never been read: it is one
      // number inside a line the hub prints for every node every second, so a leaf
      // going dark looks exactly like a leaf that is fine unless somebody is
      // diffing consecutive lines at the moment it happens. A WARN on the edge is
      // a thing you can find afterwards, and it names the count that stopped.
      const bool present = nodeIsPresent(node, now);
      if (present != was_present[i]) {
        was_present[i] = present;
        if (present) {
          ESP_LOGW(kTag,
                   "node %" PRIu64 " is being heard (%lu data frames so far); "
                   "%lu of %lu roster node(s) present",
                   node.device_id, static_cast<unsigned long>(node.data_frames),
                   static_cast<unsigned long>(countNodesPresent(now)),
                   static_cast<unsigned long>(registryCount()));
        } else {
          ESP_LOGW(kTag,
                   "node %" PRIu64 " (%02x:%02x:%02x:%02x:%02x:%02x) HAS GONE "
                   "QUIET: nothing heard for %llu ms, stopped at %lu data frames. "
                   "It stays on the roster and its status frame keeps being "
                   "published from its last known state -- that frame is now "
                   "HISTORY, not news. %lu of %lu roster node(s) present.",
                   node.device_id, node.mac[0], node.mac[1], node.mac[2],
                   node.mac[3], node.mac[4], node.mac[5],
                   static_cast<unsigned long long>(silent_ms),
                   static_cast<unsigned long>(node.data_frames),
                   static_cast<unsigned long>(countNodesPresent(now)),
                   static_cast<unsigned long>(registryCount()));
        }
      }

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
      // ⚠️ EVERY WAY A FRAME CAN FAIL TO REACH THE QUEUE, or this line lies by
      // omission -- which is exactly what TEC-NATKIT-86 was. publish_too_big
      // existed as a code path and not as a counter, so a dropped frame showed
      // up only as a gap between what the hub said it received and what the
      // broker saw, with nothing anywhere to explain it. Adding the branch to
      // the guard chain without adding it here would leave the bug half fixed.
      if (node.publish_no_sync || node.publish_no_time ||
          node.publish_no_shift || node.publish_too_big) {
        ESP_LOGW(kTag,
                 "  NOT PUBLISHED: %lu no leaf fit, %lu no wall clock, %lu "
                 "rewrite refused, %lu TOO BIG for the shift buffer -- these "
                 "never reached the uplink queue",
                 static_cast<unsigned long>(node.publish_no_sync),
                 static_cast<unsigned long>(node.publish_no_time),
                 static_cast<unsigned long>(node.publish_no_shift),
                 static_cast<unsigned long>(node.publish_too_big));
      }
      if (node.frames_unicast || node.frames_broadcast) {
        ESP_LOGI(kTag, "  delivery: %lu unicast, %lu BROADCAST fallback",
                 static_cast<unsigned long>(node.frames_unicast),
                 static_cast<unsigned long>(node.frames_broadcast));
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
