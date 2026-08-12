#include <cinttypes>

#include "device_id.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "node_role.hpp"
#include "sdkconfig.h"

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

}  // namespace

void runPrimary() {
  ESP_LOGI(kTag, "primary: device %" PRIu64 ", ESP-NOW hub", deviceId());

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

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));

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

    if (!any) {
      ESP_LOGI(kTag, "no nodes yet (beaconing; %lu foreign/unhandled packets)",
               static_cast<unsigned long>(espNowPrimaryUnknownPackets()));
    }
  }
}

}  // namespace natkit
