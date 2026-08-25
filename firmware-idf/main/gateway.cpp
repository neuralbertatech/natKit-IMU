#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "device_id.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_net.hpp"
#include "imu_frame.hpp"
#include "node_role.hpp"
#include "sdkconfig.h"
#include "time_sync.hpp"
#include "uplink.hpp"
#include "uplink_reader.hpp"

// Gateway: serial in from the primary, WiFi + MQTT out (TEC-NATKIT-26).
//
// The only internet-facing device, and the only one with a wall clock. It reads
// the primary's framed stream, turns device-relative timestamps into real ones,
// and publishes on the topics the bridge and backend already expect -- so
// nothing server-side changes for this epic. That constraint is what lets the
// fork be evaluated with the same viewer, exporter and recordings.
//
// --- Where the chain of custody for time ENDS -------------------------------
//
// This is the last hop, and it is worth stating whole because three slices built
// toward it:
//
//   leaf device time  --(the leaf's own fit, #340)-->  primary time
//   primary time      --(this file)-->                 wall clock
//
// No node before this one has a wall clock, by design. The leaf sends raw
// device-monotonic timestamps and its FIT as a separate value; the primary
// forwards the frame verbatim and passes the fit along; and only here, where NTP
// exists, does anything become a real time. Doing it in that order is what keeps
// the correction undoable: the raw device time is still on the wire at every hop
// before this one.
//
// ⚠️ THE PRIMARY-TO-WALL HALF IS THE CRUDE ONE, and the asymmetry is deliberate
// rather than overlooked. Leaf-to-primary is a rolling least-squares fit over 32
// beacons, good to ~16 us node-to-node. Primary-to-wall is a single subtraction
// taken when a primary-status frame arrives, so it carries the serial link's
// latency and the gateway's own scheduling -- probably a millisecond or two, and
// unmeasured. That is enough for a wall-clock axis (NTP itself is ~10 ms) and it
// is NOT enough to compare two nodes; the node-to-node figure is the one that
// survives this hop intact, because both nodes take the same offset.

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-gateway";

// ⚠️ THE TOPIC NAMES ARE NOT DEFINED HERE ANY MORE (TEC-NATKIT-88).
//
// They used to be, as a second copy of uplink.cpp's set, and the copies
// diverged exactly as you would expect: this one never grew the two STATUS
// topics, so a WiFi rig published its data perfectly and published nothing at
// all about its own health -- no device pill, no Logs page, and a recording that
// sealed "no clock data" on every run. Confirmed on hardware 2026-08-25: four
// Data topics on the broker and zero Log- topics, while this file's own counters
// showed 972 node-status and 243 primary-status frames arriving and being
// discarded.
//
// uplinkTopicTemplate() is now the single mapping and both publishers ask it.

// Per-node state: the leaf's clock fit, as forwarded by the primary. Without it
// a data frame's timestamps cannot be turned into anything publishable.
struct NodeClock {
  bool in_use = false;
  uint64_t device_id = 0;
  SyncState sync{};
  bool sync_valid = false;
  uint32_t published = 0;
  uint32_t dropped_no_clock = 0;
  uint32_t dropped_no_time = 0;
  uint32_t dropped_publish = 0;
};

NodeClock sNodes[kMaxTrackedNodes];

// primary uptime -> wall clock. One subtraction, refreshed whenever a primary
// status frame arrives.
bool sPrimaryOffsetValid = false;
int64_t sPrimaryToWallUs = 0;
uint32_t sPrimaryOffsetUpdates = 0;

uint32_t sFramesNoRoute = 0;
// ⚠️ These two used to be bare `return`s with no counter -- the same defect
// shape as TEC-NATKIT-86, where a frame too large for the shift buffer vanished
// and every visible number said the rig was healthy. A drop that is not counted
// is a drop nobody can find.
uint32_t sFramesOversize = 0;
uint32_t sStatusPublished = 0;
uint32_t sStatusRefused = 0;
uint32_t sStatusTooShort = 0;

NodeClock *nodeFor(uint64_t device_id) {
  for (NodeClock &node : sNodes) {
    if (node.in_use && node.device_id == device_id) {
      return &node;
    }
  }
  for (NodeClock &node : sNodes) {
    if (!node.in_use) {
      node = NodeClock{};
      node.in_use = true;
      node.device_id = device_id;
      ESP_LOGI(kTag, "routing stream %" PRIu64, device_id);
      return &node;
    }
  }
  return nullptr;
}

template <typename T>
T readLe(const uint8_t *p) {
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(p[i]) << (8 * i);
  }
  return value;
}

template <typename T>
void writeLe(uint8_t *out, T value) {
  for (size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

// One reusable buffer. Nothing on this path allocates after startup -- the old
// firmware's std::bad_alloc came from the frame path allocating per copy.
uint8_t sFrame[kUplinkMaxPayload];
char sTopic[96];

// Republishes a status frame verbatim, under the id of the device it DESCRIBES.
//
// ⚠️ That id is in the PAYLOAD at offset 0, not in the uplink header. A
// primary-status frame is about the primary, and the uplink header's stream id
// is whatever the sender put there -- publishing under the wrong one would file
// the hub's health against a leaf, or against the gateway.
// UplinkNodeStatus/UplinkPrimaryStatus both assert offsetof(device_id) == 0.
//
// ⚠️ Length is NOT validated against sizeof(the struct). The fleet is flashed one
// board at a time, so a 168-byte node status from older firmware and a 192-byte
// one from current firmware are both live on the same broker, and libnatkit-core
// accepts both by design. Refusing the short one here would blank the health
// panel for exactly the devices that have not been reflashed yet.
void republishStatus(UplinkType type, const uint8_t *payload, size_t length) {
  if (length < sizeof(uint64_t)) {
    ++sStatusTooShort;  // cannot even name the device it is about
    return;
  }
  const uint64_t about = readLe<uint64_t>(payload);
  std::snprintf(sTopic, sizeof(sTopic), uplinkTopicTemplate(type), about);
  if (gatewayPublish(sTopic, payload, length)) {
    ++sStatusPublished;
  } else {
    ++sStatusRefused;
  }
}

void onFrame(UplinkType type, uint64_t stream_id, const uint8_t *payload,
             size_t length) {
  switch (type) {
    case UplinkType::kPrimaryStatus: {
      // ⚠️ PUBLISHED FIRST, and deliberately BEFORE the clock check below.
      // The hub's own health is the thing you need most when the clock is NOT
      // valid -- "why is nothing being published" is answered by this frame, and
      // gating it on the same condition it explains would hide it exactly when
      // it matters. It carries no timestamp of ours, so it needs no wall clock.
      republishStatus(type, payload, length);
      if (length < 16 || !gatewayTimeValid()) {
        return;
      }
      // The primary's uptime at the moment it built this frame, against our wall
      // clock now. The serial link's latency is inside this difference and is not
      // corrected for -- see the warning at the top of this file.
      const uint64_t primary_uptime_us = readLe<uint64_t>(payload + 8);
      sPrimaryToWallUs = static_cast<int64_t>(gatewayWallClockUs()) -
                         static_cast<int64_t>(primary_uptime_us);
      sPrimaryOffsetValid = true;
      ++sPrimaryOffsetUpdates;
      return;
    }
    case UplinkType::kNodeStatus: {
      // Same reasoning: out to the broker first, then used for our own routing.
      // A leaf's counters are how a silent leaf is told from a dead one, so they
      // must survive a frame we cannot fully parse.
      republishStatus(type, payload, length);
      if (length < sizeof(UplinkNodeStatus)) {
        // Older firmware, shorter struct. Published above regardless -- the host
        // decoders read both sizes -- but we cannot lift a clock fit out of it.
        return;
      }
      UplinkNodeStatus status{};
      std::memcpy(&status, payload, sizeof(status));
      NodeClock *node = nodeFor(status.device_id);
      if (node == nullptr) {
        return;
      }
      node->sync = status.sync;
      node->sync_valid = status.sync_valid != 0;
      return;
    }
    case UplinkType::kCommandLog: {
      // A command's answer, already JSON, already addressed. Republished verbatim
      // and NOT time-corrected: a log record is stamped by the backend that asked
      // for it and correlated by command_id, so it needs no clock of ours -- and
      // it must go out even when a node has no valid sync, because "why is this
      // node not producing data" is exactly what someone would be asking it.
      if (length > sizeof(sFrame)) {
        ++sFramesOversize;
        return;
      }
      std::memcpy(sFrame, payload, length);
      std::snprintf(sTopic, sizeof(sTopic),
                    uplinkTopicTemplate(UplinkType::kCommandLog), stream_id);
      gatewayPublish(sTopic, sFrame, length);
      return;
    }
    case UplinkType::kData: {
      NodeClock *node = nodeFor(stream_id);
      if (node == nullptr) {
        ++sFramesNoRoute;
        return;
      }
      if (!node->sync_valid || !sPrimaryOffsetValid) {
        // Refused rather than published raw. A frame whose timestamps were never
        // corrected is indistinguishable downstream from one that was, and it
        // would silently poison a recording's time axis with 1970-era values --
        // which is worse than a gap that seqNo already makes visible.
        ++node->dropped_no_clock;
        return;
      }
      if (!gatewayTimeValid()) {
        ++node->dropped_no_time;
        return;
      }
      if (length > sizeof(sFrame)) {
        ++sFramesOversize;
        return;
      }

      std::memcpy(sFrame, payload, length);
      if (!rewriteFrameTimestamps(sFrame, length, node->sync,
                                  sPrimaryToWallUs)) {
        ++node->dropped_no_clock;
        return;
      }

      std::snprintf(sTopic, sizeof(sTopic),
                    uplinkTopicTemplate(UplinkType::kData), stream_id);
      if (gatewayPublish(sTopic, sFrame, length)) {
        ++node->published;
      } else {
        ++node->dropped_publish;
      }
      return;
    }
  }
}

}  // namespace

void runGateway() {
  ESP_LOGI(kTag, "gateway: device %" PRIu64 ", serial in, WiFi + MQTT out",
           deviceId());

  if (gatewayNetStart() != ESP_OK) {
    ESP_LOGE(kTag, "networking did not start");
    idleStatusLoop("gateway (no network)");
  }
  if (uplinkReaderStart(onFrame) != ESP_OK) {
    ESP_LOGE(kTag, "serial reader did not start");
    idleStatusLoop("gateway (no serial)");
  }

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    const UplinkReaderStats &rx = uplinkReaderStats();
    const GatewayNetStats &net = gatewayNetStats();

    ESP_LOGI(kTag,
             "serial: %lu frames (%lu data, %lu node, %lu primary) | %lu CRC "
             "failures, %llu B read, %llu B skipped resyncing | uplink seq gaps "
             "%lu",
             static_cast<unsigned long>(rx.frames_ok),
             static_cast<unsigned long>(rx.frames_data),
             static_cast<unsigned long>(rx.frames_node_status),
             static_cast<unsigned long>(rx.frames_primary_status),
             static_cast<unsigned long>(rx.crc_failures),
             static_cast<unsigned long long>(rx.bytes_read),
             static_cast<unsigned long long>(rx.bytes_skipped),
             static_cast<unsigned long>(rx.uplink_seq_gaps));

    ESP_LOGI(kTag,
             "net: wifi %s (rssi %d), mqtt %s, clock %s | published %lu (%llu "
             "B), refused %lu | wifi drops %lu, mqtt drops %lu",
             net.wifi_connected ? "up" : "DOWN", net.rssi,
             net.mqtt_connected ? "up" : "DOWN",
             gatewayTimeValid() ? "synced" : "NOT SYNCED",
             static_cast<unsigned long>(net.publishes_ok),
             static_cast<unsigned long long>(net.bytes_published),
             static_cast<unsigned long>(net.publishes_failed),
             static_cast<unsigned long>(net.wifi_disconnects),
             static_cast<unsigned long>(net.mqtt_disconnects));

    // The republished telemetry, counted separately from the data stream. Without
    // this line the fix for TEC-NATKIT-88 would itself be unverifiable from the
    // device -- and "the panel is blank" would still not say whether the frames
    // never arrived, were never published, or were refused by the broker.
    ESP_LOGI(kTag,
             "status republished: %lu published, %lu refused by mqtt, %lu too "
             "short to address | %lu frames oversize, %lu no route",
             static_cast<unsigned long>(sStatusPublished),
             static_cast<unsigned long>(sStatusRefused),
             static_cast<unsigned long>(sStatusTooShort),
             static_cast<unsigned long>(sFramesOversize),
             static_cast<unsigned long>(sFramesNoRoute));

    for (const NodeClock &node : sNodes) {
      if (!node.in_use) {
        continue;
      }
      ESP_LOGI(kTag,
               "  stream %" PRIu64 ": %lu published, %lu held for no clock, %lu "
               "for no wall time, %lu refused by mqtt | fit %s",
               node.device_id, static_cast<unsigned long>(node.published),
               static_cast<unsigned long>(node.dropped_no_clock),
               static_cast<unsigned long>(node.dropped_no_time),
               static_cast<unsigned long>(node.dropped_publish),
               node.sync_valid ? "known" : "MISSING");
    }

    if (sPrimaryOffsetValid) {
      ESP_LOGI(kTag,
               "  primary->wall offset %+lld us, refreshed %lu times | heap "
               "%" PRIu32 " B",
               static_cast<long long>(sPrimaryToWallUs),
               static_cast<unsigned long>(sPrimaryOffsetUpdates),
               static_cast<uint32_t>(esp_get_free_heap_size()));
    }
  }
}

}  // namespace natkit
