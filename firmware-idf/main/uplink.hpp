#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "espnow_link.hpp"
#include "imu_frame.hpp"  // the frame constants the queue item is sized from

namespace natkit {

// The primary's framed serial uplink to the gateway (#348 / TEC-NATKIT-25).
//
// --- Why a frame at all -----------------------------------------------------
//
// The far end is a second microcontroller reading a byte stream that it may join
// half way through, after either end resets, and possibly with another writer on
// the same wire. So the format has to answer three questions on its own: where
// does a frame start, how long is it, and did it arrive intact. Magic, length and
// CRC, in that order.
//
// It is NOT a reassembly protocol. #346 measured the canonical frame at 524 bytes
// against a 1470-byte ESP-NOW ceiling and decided one frame is one packet, so
// nothing arriving over the radio is ever partial. This ticket's "reassembly"
// scope is therefore dead rather than deferred, and a lost packet stays what it
// already was: a whole missing frame that seqNo makes detectable.
//
// --- Layout, little-endian throughout ---------------------------------------
//
//   0   1  magic 'N'
//   1   1  magic 'K'
//   2   1  version
//   3   1  type (UplinkType)
//   4   8  stream id -- the device id that is already inside every topic name
//  12   4  uplink sequence, the PRIMARY's own, so uplink loss is distinguishable
//          from radio loss (the payload carries the radio's sequence separately)
//  16   2  payload length
//  18   N  payload
// 18+N  4  CRC32 over bytes [0, 18+N)
//
// The two sequence numbers are the point of the header. A gap in the radio
// sequence means a node's frame never reached us; a gap in the uplink sequence
// means we dropped it or the wire ate it. Those have different causes and
// different fixes, and one counter cannot tell them apart.

constexpr uint8_t kUplinkMagic0 = 'N';
constexpr uint8_t kUplinkMagic1 = 'K';
constexpr uint8_t kUplinkVersion = 1;
constexpr size_t kUplinkHeaderSize = 18;
constexpr size_t kUplinkCrcSize = 4;

enum class UplinkType : uint8_t {
  // Payload is the canonical NatImuBulkDataSchema Binary frame, VERBATIM.
  //
  // Passed through untouched on purpose. The primary knows how to shift this
  // node's timestamps into its own clock, and deliberately does not: the
  // correction travels as a value in kNodeStatus below, so the raw device time
  // survives to the gateway and the shift stays undoable and improvable. Same
  // reasoning as the leaf not rewriting its own timestamps (#340).
  kData = 1,
  // Per-node counters and that node's clock fit. The gateway needs this to turn
  // a kData frame's device-relative timestamps into anything publishable.
  kNodeStatus = 2,
  // A command's answer, as a JSON document already built by the primary. Goes to
  // Log-<id>-Json-NatLogV1, the topic the backend already correlates against.
  kCommandLog = 4,
  // The primary's own health, including what it dropped and the rig's
  // time-coherence metric (#315).
  kPrimaryStatus = 3,
  // ⚠️ THE ONLY TYPE THAT TRAVELS GATEWAY -> PRIMARY (TEC-NATKIT-92). Payload is
  // the command JSON exactly as the backend published it, and `stream_id` is the
  // device it is addressed to -- taken from the TOPIC the gateway received it
  // on, never from the document, which has no device field.
  //
  // The frame format was directionless already: magic, version, type, id,
  // sequence, length, CRC say nothing about which way they are going. What was
  // missing was a reader on the primary and a writer on the gateway, not a
  // protocol.
  //
  // ⚠️ It is never published to MQTT in either direction. It is consumed at the
  // primary and turned into an ESP-NOW unicast; the ANSWER comes back up as
  // kCommandLog, which is a different type on a different topic.
  kCommand = 5,
  // The device's control advertisement, as a JSON document the primary built
  // from a leaf's ControlsFrame. Goes to
  // Configuration-<id>-Json-NatKitDeviceControlsV1 (TEC-NATKIT-10).
  //
  // ⚠️ PUBLISHED RETAINED, and it is the only type that is. A late subscriber
  // must learn what a board offers without waiting for it to change. That is
  // safe only because REACHABILITY lives on the Heartbeat channel instead: a
  // retained advertisement stays informative without being actionable, so
  // nothing has to be cleared when a device goes away.
  kControls = 6,
};

// What the gateway needs about one node: who it is, whether we are losing it, and
// how to interpret its clock.
struct UplinkNodeStatus {
  uint64_t device_id;
  uint8_t mac[6];
  // ⚠️ THE NOISE FLOOR AT THIS LEAF, in dBm, worst since its previous heartbeat.
  // The measurement this rig has never had (TEC-NATKIT-51): every other figure here
  // describes a WANTED signal, and all of them read healthy through an event that
  // cost 65% of the hub's beacons with RSSI flat to within 2 dB. 0 = never sampled;
  // a real floor is tens of dB negative, so 0 is unambiguous.
  int8_t leaf_noise_floor_dbm;
  uint8_t reserved0;
  uint32_t data_frames;
  uint32_t seq_gaps;
  uint32_t seq_duplicates;
  uint32_t seq_restarts;
  uint32_t heartbeats;
  uint64_t last_seen_us;      // in the PRIMARY's clock
  SyncState sync;             // the leaf's fit; apply with syncStateToPrimary()
  uint8_t sync_valid;
  // ⚠️ HOW STRONGLY THE HUB HEARS THIS NODE, which it has always tracked and
  // never published. Without it the only per-node signal leaving the rig is a
  // frame count, and a node delivering nothing at -30 dBm and one delivering
  // nothing at -85 dBm need completely different fixes. Diagnosing the leaves
  // that keep swapping which one works (TEC-NATKIT-37) meant reading it off a
  // console that resets the board it is printed on.
  //
  // Placed in the reserved bytes so the struct's size does not move.
  int8_t rssi_last;
  int8_t rssi_best;
  int8_t rssi_worst;
  uint8_t rssi_seen;
  uint8_t leaf_scan_channel;   // non-zero while that leaf is hopping channels
  // ⚠️ THE LEAF'S OWN VIEW, so the reciprocity check can finally be made from the
  // published data. `rssi_last` above is how loudly the HUB hears the node;
  // `leaf_rssi_of_primary` is how loudly the NODE hears the hub. A passive path
  // is reciprocal, so a large gap between the two is a receiver fault or
  // interference rather than distance -- the single diagnostic that has found the
  // most RF faults on this rig, and until now it needed a console that resets the
  // board it prints on.
  //
  // `leaf_tx_power_quarter_dbm` is there because the power sweep picks a level
  // PER NODE, independently, at every boot, and nothing on the wire said what it
  // chose. Two nodes silently landing on different powers is exactly the shape of
  // TEC-NATKIT-37, and it was being diagnosed without the number.
  //
  // Both live in the two bytes `reserved1` already had, so the struct stays 168
  // bytes and existing decoders do not move. ⚠️ 0 means UNKNOWN for both.
  int8_t leaf_rssi_of_primary;
  uint8_t leaf_tx_power_quarter_dbm;
  // The LEAF's own counters, relayed from its heartbeat. These separate the three
  // ways a stall can happen and which the primary alone cannot tell apart:
  // the leaf stopped BUILDING frames, its send queue OVERFLOWED because the radio
  // was blocked, or its sends were REFUSED on air.
  uint32_t leaf_frames_built;
  uint32_t leaf_frames_dropped;
  uint32_t leaf_send_failures;
  uint32_t leaf_channel_hops;
  // ⚠️ FRAMES THE PRIMARY RECEIVED AND THEN THREW AWAY ITSELF, which until now
  // were counted and never published. They are dropped BEFORE the uplink queue,
  // so uplink's frames_dropped stays at zero; they arrive in sequence, so the
  // node's seq_gaps stays at zero; and the primary keeps publishing its own
  // status throughout. Every counter that was visible said the rig was healthy
  // while a fifth of a leaf's frames were being discarded here.
  uint32_t publish_no_sync;   // no clock fit for that node yet
  uint32_t publish_no_shift;  // fit present but rewriteFrameTimestamps refused
  // ⚠️ THE RAW ACCUMULATORS, published alongside the derived figures
  // (TEC-NATKIT-52). The derived ones -- residual_rms_ns and the rest -- are
  // averages SINCE THE PRIMARY BOOTED, computed in here where nothing outside can
  // difference them. Two readings a day apart differ mostly because more history
  // accumulated, not because the rig changed, which makes them useless for the
  // one thing they keep being needed for: comparing two conditions.
  //
  // With the sums a consumer differences two samples and computes the mean and
  // standard deviation OVER THAT WINDOW, exactly as beacons.py already does for
  // beacon loss. A standard deviation is not a counter, which is why sampling and
  // subtracting the derived value cannot substitute for this.
  //
  // Sums are over TYPICAL samples only, matching probe_error_count -- the
  // excursion tail is counted separately on purpose (see NodeState), so a 0.7%
  // tail cannot set the accuracy figure.
  int64_t probe_error_sum_us;
  uint64_t probe_error_sum_sq;
  uint32_t probe_error_count;
  uint32_t reserved_probe;  // keeps the 8-byte alignment explicit
};

// ⚠️ THESE SIZES ARE THE WIRE FORMAT. Every field added since 2026-08-17 has gone
// into bytes the struct already reserved, precisely so decoders do not move -- and
// the only thing enforcing that was care. A struct that grows here decodes as
// plausible nonsense on the far side: a spurious 4 bytes in the primary layout once
// came to the right total by coincidence and shifted every field after it, and the
// size check passed. Assert it instead.
static_assert(sizeof(UplinkNodeStatus) == 192,
              "UplinkNodeStatus is published binary; use its reserved bytes rather "
              "than growing it, or bump the topic's V1 and update every decoder");

struct UplinkPrimaryStatus {
  uint64_t device_id;
  uint64_t uptime_us;
  uint32_t epoch;
  uint32_t free_heap;
  uint32_t min_free_heap;
  // ⚠️ THE ROSTER, NOT THE FLEET. This is registryCount(): how many nodes the hub
  // has ever admitted and remembers in NVS. It is deliberately persistent — that
  // is what makes a roster a roster, and what a seal freezes — so it does NOT fall
  // when a leaf stops talking. Read alone it says the rig is complete when a board
  // is dead on the bench (TEC-NATKIT-81). Pair it with nodes_present.
  uint32_t nodes_known;
  uint32_t nodes_rejected;      // packets from MACs the registry will not accept
  uint32_t unknown_packets;
  // Uplink counters. `dropped` is the number that matters: it is the primary
  // saying what it discarded, which is the only way to trust an aggregator.
  uint32_t frames_queued;
  uint32_t frames_sent;
  uint32_t frames_dropped;
  uint32_t write_timeouts;
  uint64_t bytes_sent;
  // #315's metric for the whole rig.
  uint32_t coherence_typical_us;
  uint32_t coherence_bound_us;
  uint32_t coherence_worst_us;
  uint32_t coherence_samples;
  uint8_t coherence_quality;
  uint8_t coherence_measured;
  uint8_t registry_sealed;
  // ⚠️ THE HUB'S OWN NOISE FLOOR, and it is the CONTROL for the per-leaf figure.
  // If the leaves' floor rises and this one does not, the noise is at the leaves;
  // if both rise, it is the room. Neither reading means much alone.
  int8_t noise_floor_dbm;
  // ⚠️ THE HUB'S DIE TEMPERATURE, in whole degrees C. Here because the leading
  // explanation for TEC-NATKIT-50 is the PRIMARY'S TRANSMIT PATH degrading for tens
  // of minutes at a time -- the impairment is entirely hub->leaf, with leaf->hub
  // losing 0 of 25,199 frames -- and thermal is the first cause to rule in or out.
  // Whole degrees is deliberate: the question is 40 vs 70, not 40.0 vs 40.1, and a
  // byte keeps this struct at 144 so no decoder moves.
  // -128 = unavailable (no sensor on this target, or it failed to start).
  int8_t chip_temp_c;
  // The esp_err_t (low byte) behind a chip_temp_c of -128, so "unavailable" is
  // diagnosable from the broker rather than from a console that resets the board.
  // 0 = fine, 0xff = this target has no sensor.
  uint8_t chip_temp_err;
  // ⚠️ THE FLEET, as against nodes_known's roster: how many nodes the hub has
  // actually HEARD inside the presence window. This is the number that falls when
  // a board dies, and the discrepancy between the two is the whole finding of
  // TEC-NATKIT-81 — a leaf went quiet for hours while nodes_known held at 4 and
  // the hub kept composing a status frame for it out of its last known state.
  //
  // Two counts rather than one because they are different questions and both are
  // wanted: "which nodes are mine" survives a reboot and a leaf's absence, "which
  // nodes are here" is now. Expiring the ROSTER to make one number do both would
  // be wrong twice over: a sealed rig would evict the very node it is sealed to
  // accept, and it would then be unable to tell an absent node from an unknown one.
  //
  // ⚠️ `nodes_present_valid` exists because a legacy frame's spare bytes are ZERO,
  // and zero is a legitimate reading here — every leaf gone is exactly the state
  // this field is for. Without the flag, firmware too old to report it would be
  // indistinguishable from a rig with nothing left alive, which is the more
  // alarming of the two. 0 = this hub does not report presence; ignore the count.
  uint8_t nodes_present;
  uint8_t nodes_present_valid;
  // ⚠️ NO SPARE BYTES REMAIN in the declared body: nodes_present took the last two
  // that `reserved[2]` held. The next field either uses bytes 140..143 — real tail
  // padding today, which must be DECLARED before it can be written, since padding
  // is not guaranteed to be transmitted as anything in particular — or bumps V1.
  // Command relay (TEC-NATKIT-39). ⚠️ These are here rather than on the console
  // because THE PRIMARY'S CONSOLE CANNOT BE READ: the ESP32-S3 resets when its
  // native USB console is opened AND re-enumerates, so the reading process loses
  // the handle and gets nothing at all. Every command-path fault has to be
  // diagnosed from this struct.
  uint32_t commands_received;
  uint32_t commands_relayed;
  uint32_t commands_malformed;
  uint32_t commands_unknown_device;
  uint32_t commands_send_failed;
  uint32_t command_subscriptions;
  // ⚠️ THE ANSWER'S OWN COUNTERS, and they exist because unknown_packets could
  // not tell "the reply arrived and was handled" from "the reply never arrived":
  // both leave it at zero. Two counters that CAN be different are worth more than
  // one that is always right for the wrong reason.
  uint32_t command_answers_received;   // kCommandLog packets from a leaf
  uint32_t command_answers_published;  // ... that reached the broker
  uint32_t command_answers_duplicate;  // ... suppressed as an exact repeat
  // ⚠️ delivered is the one that means anything: relayed says the radio took the
  // packet, delivered says a device acknowledged it.
  uint32_t commands_delivered;
  uint32_t command_retransmits;
  uint32_t commands_undelivered;
  // ⚠️ WHY THE HUB LAST RESTARTED, because there is no other way to find out.
  // Its USB console resets it AND re-enumerates, so a reader gets an empty file
  // and a fresh boot rather than the panic it was trying to read. esp_reset_reason
  // survives the restart; without it, "uptime went backwards" is the entire
  // diagnosis available.
  uint32_t reset_reason;
  // ⚠️ Same reasoning as the per-leaf sums above (TEC-NATKIT-52): coherence_typical_us
  // and coherence_bound_us are averages since boot, so they cannot compare two
  // conditions. Measured 2026-08-18, the primary had ~20 h of uptime and reported
  // 66 us typical -- a figure that averages TEC-NATKIT-50's interference episode
  // together with the clean period after it.
  //
  // coherence_worst_us needs no companion: it is a high-water mark and is already
  // windowable by differencing, which is how beacons.py reports worst_rose_us.
  int64_t spread_sum_us;
  uint64_t spread_sum_sq;
  uint32_t markers_paired;
  uint32_t reserved_coherence;  // keeps the 8-byte alignment explicit
};

static_assert(sizeof(UplinkPrimaryStatus) == 168,
              "UplinkPrimaryStatus is published binary; use its reserved bytes "
              "rather than growing it, or bump the topic's V1");

// A SNAPSHOT of the uplink counters, not a live view.
//
// ⚠️ Returned by value on purpose (TEC-NATKIT-75). Four of these are written from
// TWO tasks -- the ESP-NOW receive callback on the WiFi task and the 1 Hz status
// loop both call uplinkSend() -- and a plain `++` on a shared word loses
// increments to the race. On the live rig frames_queued read ~30 BELOW frames_sent
// with frames_dropped at 0, which the code makes otherwise impossible.
//
// The atomics live inside uplink.cpp rather than in this struct so that the wire
// side stays a plain POD, and so a reader gets figures that were all read at
// roughly the same moment instead of fields that can move between two reads.
struct UplinkStats {
  // ⚠️ Producer side: written from more than one task, so accumulated atomically.
  uint32_t frames_queued = 0;
  uint32_t frames_dropped = 0;   // queue was full; OLDEST discarded
  uint32_t oversize_rejected = 0;
  uint32_t queue_high_water = 0;
  // ⚠️ Drain side: written ONLY by drainTask, which is why these were never the
  // ones that drifted. Left as plain words deliberately -- bytes_sent is 64-bit
  // and std::atomic<uint64_t> on a 32-bit target is not lock-free, so making it
  // atomic would pull libatomic in for no defect.
  uint32_t frames_sent = 0;
  uint32_t write_timeouts = 0;
  uint64_t bytes_sent = 0;
};

// Installs and configures the uplink UART, exactly once.
//
// ⚠️ EXISTS BECAUSE BOTH ENDS NOW READ AND WRITE THE SAME PORT (TEC-NATKIT-92),
// and ESP-IDF allows one driver install per UART. Before the downward command
// path there was a clean split -- the primary called uplinkStart() and installed
// with a TX buffer, the gateway called uplinkReaderStart() and installed with RX
// only -- and each side called exactly one of them. Now each side calls both,
// so the second install would fail on an already-installed driver.
//
// Idempotent, and sized for BOTH directions rather than for whichever caller
// happens to run first. Safe to call from either, in either order.
esp_err_t uplinkUartEnsure();

// Brings up the uplink UART and its drain task.
esp_err_t uplinkStart();

// Queues one frame. NEVER BLOCKS: like the leaf's radio queue, a full queue drops
// the OLDEST frame rather than stalling the caller. The caller here is the
// ESP-NOW receive callback running on the WiFi task, and blocking it would lose
// packets from every node to relieve congestion caused by one.
//
// Returns false only if the payload cannot fit a frame at all, which is a
// programming error rather than back-pressure.
bool uplinkSend(UplinkType type, uint64_t stream_id, const void *payload,
                size_t payload_size);

UplinkStats uplinkStats();

// The MQTT topic template for a frame type -- one "%" PRIu64 for the device id.
//
// Shared between the primary's direct publisher and the gateway's republisher on
// purpose (TEC-NATKIT-88): they are the two ways a frame reaches the broker, the
// backend correlates on these exact names, and when each kept its own copy the
// gateway's silently lacked the two status topics entirely.
const char *uplinkTopicTemplate(UplinkType type);

// The largest payload the uplink will carry: the canonical frame at its
// configured maximum. Sized from the frame constants rather than a round number
// so a change to samples-per-frame cannot silently overflow a queue item.
constexpr size_t kUplinkMaxPayload =
    kFrameHeaderSize + kMaxSamplesPerFrame * kSampleSize;
constexpr size_t kUplinkMaxFrame =
    kUplinkHeaderSize + kUplinkMaxPayload + kUplinkCrcSize;

// ⚠️ THE OFFSETS THE HOST DECODER READS, asserted here by the TARGET compiler.
//
// libnatkit-core decodes these frames byte by byte at hard-coded offsets, because
// they are a memcpy of these structs built for xtensa and a struct declared on the
// host would make correctness depend on two compilers agreeing about padding. The
// weakness of that approach is that the offsets live in a different repository
// from the layout they describe, so a field inserted here goes unnoticed there
// until somebody reads a plausible wrong number off a panel.
//
// These asserts close that gap from this side: add or reorder a field and the
// FIRMWARE stops compiling, naming the offset that moved. The matching constants
// are in NatKitNodeStatusV1Schema.cpp / NatKitPrimaryStatusV1Schema.cpp.
static_assert(offsetof(UplinkNodeStatus, device_id) == 0, "node: device_id moved");
static_assert(offsetof(UplinkNodeStatus, mac) == 8, "node: mac moved");
static_assert(offsetof(UplinkNodeStatus, last_seen_us) == 40, "node: last_seen_us moved");
static_assert(offsetof(UplinkNodeStatus, sync) == 48, "node: sync moved");
static_assert(offsetof(UplinkNodeStatus, sync_valid) == 136, "node: sync_valid moved");
static_assert(offsetof(UplinkNodeStatus, leaf_frames_built) == 144, "node: leaf_frames_built moved");
static_assert(offsetof(UplinkNodeStatus, publish_no_shift) == 164, "node: publish_no_shift moved");
static_assert(offsetof(UplinkNodeStatus, probe_error_sum_us) == 168, "node: probe sums moved");
static_assert(offsetof(UplinkNodeStatus, probe_error_sum_sq) == 176, "node: probe sums moved");
static_assert(offsetof(UplinkNodeStatus, probe_error_count) == 184, "node: probe sums moved");

static_assert(offsetof(UplinkPrimaryStatus, device_id) == 0, "primary: device_id moved");
static_assert(offsetof(UplinkPrimaryStatus, uptime_us) == 8, "primary: uptime_us moved");
static_assert(offsetof(UplinkPrimaryStatus, bytes_sent) == 56, "primary: bytes_sent moved");
static_assert(offsetof(UplinkPrimaryStatus, coherence_typical_us) == 64, "primary: coherence moved");
static_assert(offsetof(UplinkPrimaryStatus, commands_received) == 88, "primary: command counters moved");
static_assert(offsetof(UplinkPrimaryStatus, reset_reason) == 136, "primary: reset_reason moved");
static_assert(offsetof(UplinkPrimaryStatus, spread_sum_us) == 144, "primary: coherence sums moved");
static_assert(offsetof(UplinkPrimaryStatus, spread_sum_sq) == 152, "primary: coherence sums moved");
static_assert(offsetof(UplinkPrimaryStatus, markers_paired) == 160, "primary: coherence sums moved");

static_assert(sizeof(UplinkNodeStatus) <= kUplinkMaxPayload,
              "node status must fit the frame the queue is sized for");
static_assert(sizeof(UplinkPrimaryStatus) <= kUplinkMaxPayload,
              "primary status must fit the frame the queue is sized for");

}  // namespace natkit
