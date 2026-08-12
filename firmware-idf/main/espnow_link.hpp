#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace natkit {

// The leaf's whole networking surface: ESP-NOW, and nothing else.
//
// Deliberately absent, and this IS the architecture rather than an omission: no
// esp_wifi_connect, no MQTT, no SNTP, no HTTP. ESP-NOW needs the WiFi driver
// started but never associated. It also does not call esp_netif_init() -- ESP-NOW
// does not go through lwIP, so a leaf never brings up a network interface at all,
// which is the claim TEC-NATKIT-24 asks to be able to make.

// --- On-air envelope --------------------------------------------------------
//
// Every packet is a 4-byte envelope followed by a payload. TEC-NATKIT-23 decided
// that one canonical frame is one packet, and this does not walk that back: the
// canonical 524-byte frame travels intact as the payload of a data packet. The
// envelope exists because the channel carries more than data -- heartbeats and
// discovery share it -- and a primary must be able to tell them apart without
// inferring type from length. Four bytes against a measured 1470-byte ceiling,
// with a 524-byte frame, is free.
//
// The magic is not decoration either: on a shared channel, broadcast traffic from
// unrelated ESP-NOW devices arrives at our receive callback, and a version byte is
// what makes a mismatched pair of builds diagnosable instead of mysterious.

constexpr uint8_t kEspNowMagic0 = 'N';
constexpr uint8_t kEspNowMagic1 = 'K';
constexpr uint8_t kEspNowProtocolVersion = 1;

enum class PacketType : uint8_t {
  kData = 1,       // payload is a canonical NatImuBulkDataSchema Binary frame
  kHeartbeat = 2,  // payload is a Heartbeat
  kAnnounce = 3,   // leaf -> broadcast: "here I am"; payload is an Announce
  kPrimaryHere = 4,  // primary -> broadcast/unicast: "I am the hub"; no payload
};

struct EspNowEnvelope {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t version;
  uint8_t type;
};

constexpr size_t kEnvelopeSize = 4;
static_assert(sizeof(EspNowEnvelope) == kEnvelopeSize,
              "the envelope is written to the wire, so it must not gain padding");

// What a leaf tells the hub about itself, so the primary's registry can map a MAC
// to a stream id without being configured by hand. The device id is derived from
// the MAC (see device_id.hpp), so this is strictly redundant with the sender MAC
// ESP-NOW already provides -- and it is sent anyway, because the id is the number
// baked into every existing topic name and having the node state it makes a
// mismatch in that derivation visible rather than silent.
struct Announce {
  uint64_t device_id;
  uint32_t sample_rate_hz;
  uint16_t samples_per_frame;
  uint16_t firmware_version;
};

// Status, on the same path as the data, so that a silent node is distinguishable
// from a node with nothing to say -- which is the whole reason this exists.
struct Heartbeat {
  uint64_t device_id;
  uint64_t uptime_us;
  uint32_t frames_built;     // DATA frames encoded
  uint32_t frames_sent;      // PACKETS of every type, tx-confirmed (see LinkStats)
  uint32_t frames_dropped;   // queue was full: the bounded-queue policy at work
  uint32_t send_failures;    // accepted then failed on air, after retries
  uint32_t sensor_reports;   // total SH2 reports decoded
  uint32_t hub_resets;
  uint32_t free_heap;
  uint8_t accuracy_accel;
  uint8_t accuracy_gyro;
  uint8_t accuracy_mag;
  uint8_t accuracy_rotation;
};

// --- Link ------------------------------------------------------------------

// Counters are named for what they actually count: PACKETS, of every type, not
// data frames. The distinction is not pedantry -- a leaf sends heartbeats and
// announces on the same path, so a "frames sent" figure sitting next to the leaf's
// data-frame count read as though it had sent more frames than it built. That is
// how a number gets misquoted later.
struct LinkStats {
  uint32_t packets_queued = 0;
  uint32_t packets_sent = 0;     // accepted AND tx-confirmed, all types
  uint32_t packets_dropped = 0;  // queue was full
  uint32_t send_failures = 0;    // accepted, then failed on air after retries
  uint32_t send_retries = 0;
  uint32_t announces = 0;
  uint32_t consecutive_failures = 0;
  bool primary_known = false;
  // True once a run of consecutive failures says the hub is gone rather than the
  // air merely busy. While set, each packet gets ONE attempt instead of three --
  // see the reasoning in transmit(). Cleared by the first success.
  bool primary_absent = false;
  uint8_t primary_mac[6] = {};
};

// Starts the radio and the transmit task. Never associates.
esp_err_t espNowLinkStart();

// Queues one packet. NEVER BLOCKS and never fails the caller: when the queue is
// full the OLDEST queued packet is dropped to make room, because for a sensor
// stream the freshest frame is the valuable one and seqNo already makes the gap
// detectable on the far side. Returns false only if the payload does not fit.
//
// The sample loop must not be able to stall on the radio -- that is the
// requirement this signature exists to satisfy.
bool espNowLinkSend(PacketType type, const void *payload, size_t payload_size);

const LinkStats &espNowLinkStats();

// True once a primary has been discovered and added as a unicast peer. Until then
// announces go out as broadcast; the data path is unicast only.
bool espNowLinkHasPrimary();

// --- Primary side ----------------------------------------------------------
//
// Minimal deliberately: this is the smallest thing that lets TEC-NATKIT-24 be
// verified ("a leaf streams real sensor data to a primary that logs it"). The node
// registry proper -- persistence, the MAC-to-stream-id mapping the gateway needs,
// the serial mux for N leaves down one link, and backpressure -- is TEC-NATKIT-25,
// and the timing broadcast is #340. None of that is here.

constexpr size_t kMaxTrackedNodes = 8;

// What the primary knows about one leaf. Enough to answer "is this node alive, and
// am I losing its frames", which is what verifying the leaf slice needs.
struct NodeState {
  bool in_use = false;
  uint8_t mac[6] = {};
  uint64_t device_id = 0;
  uint32_t data_frames = 0;
  uint32_t heartbeats = 0;
  uint32_t announces = 0;
  uint32_t bytes = 0;
  uint64_t last_seq = 0;
  bool seq_seen = false;
  uint32_t seq_gaps = 0;        // frames the leaf sent that never arrived
  uint32_t seq_duplicates = 0;  // same seq twice: transmitted more than once
  uint32_t seq_restarts = 0;    // seq went strictly backwards: the leaf rebooted
  uint64_t last_seen_us = 0;
  // Last decoded frame header, so the console shows the stream is real rather
  // than merely present.
  uint16_t last_sample_count = 0;
  uint32_t last_declared_rate = 0;
  Heartbeat last_heartbeat{};
  bool heartbeat_seen = false;
};

esp_err_t espNowPrimaryStart();

// Snapshot of the tracked nodes. Returned by pointer to a fixed array rather than
// by value because the caller is a logging loop, not a consumer of history.
const NodeState *espNowPrimaryNodes();
uint32_t espNowPrimaryUnknownPackets();

}  // namespace natkit
