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
// 2 since #340: kPrimaryHere was replaced by the timing broadcast, so a v1 and a
// v2 build in the same room disagree about what a hub announcement even is. Both
// boards are reflashed together, and the version byte is what turns a half-flashed
// pair into one warning line instead of a silence that reads like a dead radio.
constexpr uint8_t kEspNowProtocolVersion = 2;

enum class PacketType : uint8_t {
  kData = 1,       // payload is a canonical NatImuBulkDataSchema Binary frame
  kHeartbeat = 2,  // payload is a Heartbeat
  kAnnounce = 3,   // leaf -> broadcast: "here I am"; payload is an Announce
  // 4 was kPrimaryHere. RETIRED rather than reused: the timing broadcast below
  // carries discovery as well, so there is one 1 Hz broadcast instead of two. The
  // number is left burnt so a stray v1 packet cannot be mistaken for a new type.
  kTimeBeacon = 5,    // primary -> broadcast: payload is a TimeBeacon
  kTimeFollowUp = 6,  // primary -> broadcast: payload is a TimeFollowUp
  kSyncState = 7,     // leaf -> primary: payload is a SyncState
  kTimeProbe = 8,          // leaf -> primary: payload is a TimeProbe
  kTimeProbeFollowUp = 9,  // leaf -> primary: payload is a TimeProbeFollowUp
  kSyncMarker = 10,        // primary -> broadcast: payload is a SyncMarker
  kMarkerReport = 11,      // leaf -> primary: payload is a MarkerReport
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

// --- Timing broadcast (#340 / TEC-NATKIT-17) --------------------------------
//
// Two packets per second, one second apart, and the split between them is the
// whole mechanism rather than an optimisation.
//
// The beacon goes out first and says only "this is beacon N of epoch E". The
// primary then reads its own clock INSIDE the ESP-NOW send callback for that
// beacon -- the closest to on-air the API gets -- and sends that reading in a
// follow-up. Timing the beacon by reading the clock before esp_now_send would
// measure the transmit queue instead: CSMA backoff and driver queueing are
// milliseconds and vary packet to packet, which is three orders of magnitude
// worse than what is being estimated. Two-step is what PTP does, for this reason.
//
// The beacon also replaces the old kPrimaryHere announcement, so a leaf still
// discovers its hub from it -- one broadcast doing both jobs, which is what
// primary.cpp asked for when this slice was still ahead of it.

struct TimeBeacon {
  uint32_t seq;    // pairs a beacon with its follow-up; gaps count missed beacons
  // Identifies this primary BOOT, not this primary. A reboot restarts esp_timer
  // at zero, so a leaf holding a fit against the old origin is wrong by the
  // primary's entire previous uptime -- and a step change like that is invisible
  // to a slope. Changing this is what tells a leaf to throw its window away.
  uint32_t epoch;
  // The primary's clock at ENQUEUE. Not the sync source -- the follow-up is --
  // and present so a leaf that has lost follow-ups still holds a millisecond-grade
  // fix rather than nothing at all.
  uint64_t enqueue_us;
  // The primary's wall clock, or 0 for "there isn't one". It is 0 today and that
  // is the design: the primary has no NTP, and the gateway (#349) is the only
  // device that will. The field is the seam that chain of custody plugs into --
  // leaf device time, to primary time (this slice), to wall clock (#349) -- and 0
  // reads as unknown rather than as 1970.
  uint64_t wall_us;
};

struct TimeFollowUp {
  uint32_t seq;    // the beacon this describes
  uint32_t epoch;
  // esp_timer read inside the send callback for beacon `seq`. 0 means the
  // callback never fired, which a leaf must treat as "no sample" rather than as
  // "time zero".
  uint64_t tx_us;
  // esp_wifi_get_tsf_time() at the same instant. EXPECTED TO BE 0 on every node
  // in this architecture: the IDF documents TSF as reading 0 on a station that is
  // not associated, and no node here ever associates. It is sent anyway so the
  // claim is measured on hardware instead of merely cited, and so that a future
  // SoftAP-based primary would light it up without a protocol change.
  uint64_t tx_tsf_us;
};

// A leaf's fit, as the primary and later the gateway need it.
//
// The leaf sends its FIT rather than pre-corrected timestamps, and the frames
// stay in raw device-monotonic time. Three reasons, all of which cost something
// to learn the other way round: a correction already baked into stored samples
// cannot be undone or improved later; a leaf that steps its own clock emits
// non-monotonic sample times mid-frame; and #318 wants the sync quality to travel
// ALONGSIDE the stream, which means it has to be a value, not an adjustment that
// has silently already happened. Applying it is the "proxy" of #340's title.
struct SyncState {
  uint64_t device_id;
  uint32_t epoch;
  uint64_t ref_local_us;   // anchor, in the leaf's own clock
  int64_t ref_offset_us;   // primary_us - local_us at the anchor
  int32_t skew_ppb;        // relative crystal rate, parts per billion
  uint32_t residual_rms_ns;
  uint32_t peak_residual_ns;
  uint64_t last_beacon_local_us;
  uint32_t beacons_seen;
  uint32_t beacons_missed;
  uint32_t pairs_used;
  uint32_t pairs_orphaned;
  uint32_t outliers_rejected;
  uint32_t epoch_changes;
  uint32_t mac_spread_us;  // receive-callback jitter, measured (see time_sync.hpp)
  uint16_t samples_used;
  uint8_t quality;         // SyncQuality
  uint8_t reserved;
};

// --- The probe: the primary measuring what the leaf claims -------------------
//
// A clock correction that reports only its own residual is grading its own
// homework -- the fit can be beautifully self-consistent and still be wrong, and
// nothing in the leaf can tell. So the leaf runs the SAME two-step trick back at
// the primary, once a second, and the primary compares what it MEASURES against
// what the leaf's fit PREDICTS. The difference is the sync error, measured
// independently of the thing being tested.
//
// This exists rather than using the data frame's own timestamp because that field
// is quantised to milliseconds (`sample.time_ms = newest_us / 1000`), which is a
// noise floor three orders of magnitude above what is being estimated. The frame
// delta is still tracked, for the slow drift picture; this is what gives the
// number.
//
// What is left in the residual, stated rather than hidden: the leaf's
// send-callback latency and the primary's receive-callback latency, each tens of
// microseconds. Propagation is nanoseconds and ignored.

struct TimeProbe {
  uint64_t device_id;
  uint32_t seq;
  uint32_t epoch;  // the primary epoch the leaf's fit is against
};

struct TimeProbeFollowUp {
  uint64_t device_id;
  uint32_t seq;
  uint32_t epoch;
  // The leaf's clock, read inside its own ESP-NOW send callback for probe `seq`.
  // Same reasoning as the primary's follow-up: reading it before the send would
  // measure the transmit queue.
  uint64_t tx_us;
};

// --- The marker: node-to-node coherence, measured (#315 / TEC-NATKIT-4) -----
//
// Everything above measures each leaf against the PRIMARY. Node-to-node
// coherence -- which is what #315 is actually about, and what a recording with
// two sensors on it depends on -- was until now only INFERRED from two such
// figures plus an argument that their common-mode bias cancels. That argument is
// reasonable and was untested.
//
// A marker tests it. The primary broadcasts one; both leaves receive THE SAME
// WAVEFRONT (propagation between them differs by nanoseconds); each converts its
// own local receive time into primary time using its own fit and reports the
// answer. Two independent clocks, one physical event, and the difference between
// their answers IS the node-to-node error -- no model, no cancellation argument.
//
// ⚠️ The marker is deliberately NOT fed into any fit. A leaf that estimated its
// clock from these packets and was then scored on them would be marking its own
// exam, which is exactly the circularity that makes a fit residual a poor
// accuracy figure. It is a held-out sample.

struct SyncMarker {
  uint32_t seq;
  uint32_t epoch;
};

struct MarkerReport {
  uint64_t device_id;
  uint32_t marker_seq;
  uint32_t epoch;
  // The leaf's own receive time for that marker, already converted into the
  // primary's time base by the leaf's fit. Two leaves reporting the same number
  // for the same marker are coherent; the spread between them is the error.
  uint64_t primary_us;
  // The raw local receive time as well, so the primary can re-derive the
  // conversion rather than having to trust it -- and so a fit that is later
  // improved can be re-applied to the same held-out sample.
  uint64_t local_us;
  uint8_t quality;  // SyncQuality at the moment of conversion
  uint8_t reserved[7];
};

// These structs go on the wire by memcpy, so their layout is a contract
// between two images rather than an internal detail -- and #349's gateway will
// have to parse them from the other side of a serial link. Pinned here so a field
// added in the middle is a build failure instead of a silently misread packet.
// (The canonical IMU frame is byte-serialised for the stronger version of this
// reason; see imu_frame.hpp. These are ours end to end, so the size assert is the
// proportionate guard.)
static_assert(sizeof(TimeBeacon) == 24, "TimeBeacon layout is a wire contract");
static_assert(sizeof(TimeFollowUp) == 24,
              "TimeFollowUp layout is a wire contract");
static_assert(sizeof(TimeProbe) == 16, "TimeProbe layout is a wire contract");
static_assert(sizeof(TimeProbeFollowUp) == 24,
              "TimeProbeFollowUp layout is a wire contract");
static_assert(sizeof(SyncState) == 88, "SyncState layout is a wire contract");
static_assert(sizeof(SyncMarker) == 8, "SyncMarker layout is a wire contract");
static_assert(sizeof(MarkerReport) == 40,
              "MarkerReport layout is a wire contract");

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
  // Channel search (#373). A leaf can no longer assume the configured channel:
  // once the primary associates with an access point its channel is the AP's,
  // and a leaf pinned elsewhere hears nothing while every send succeeds locally.
  uint8_t scan_channel = 0;
  uint32_t channel_hops = 0;
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

  // --- Time-shift proxy, and its own instrument (#340) ----------------------
  //
  // The primary holds the leaf's fit and applies it to that leaf's frames. It
  // also keeps the evidence that doing so works, because a clock correction that
  // reports its own quality and nothing else is grading its own homework.
  //
  // The instrument is the delta between when a frame ARRIVED (our clock) and when
  // the leaf says it was SAMPLED (its clock). Uncorrected, that delta must walk
  // steadily as the two crystals diverge -- tens of ppm is tens of milliseconds
  // over a five-minute soak. Corrected, it must sit still, because the only thing
  // left in it is the leaf's build-and-send latency. Watching the two side by
  // side is a direct end-to-end demonstration rather than a self-report, and the
  // rate at which the raw one walks is a SECOND, independent estimate of the skew
  // the regression reports.
  SyncState last_sync{};
  bool sync_seen = false;

  bool delta_seen = false;
  bool shifted_delta_seen = false;
  bool shift_valid = false;
  int64_t raw_delta_us = 0;
  int64_t shifted_delta_us = 0;
  int64_t first_raw_delta_us = 0;
  int64_t first_shifted_delta_us = 0;
  int64_t raw_delta_min = 0;
  int64_t raw_delta_max = 0;
  int64_t shifted_delta_min = 0;
  int64_t shifted_delta_max = 0;
  uint32_t shift_failures = 0;  // frames that arrived while the leaf was unsynced

  // The probe instrument: measured minus predicted, in microseconds. This is the
  // headline accuracy figure for #340 and the raw material for #315's confidence
  // metric, because it is the only number here that the leaf's own fit did not
  // produce.
  bool probe_pending = false;
  uint32_t probe_pending_seq = 0;
  uint64_t probe_arrival_us = 0;
  uint32_t probes_seen = 0;
  uint32_t probes_paired = 0;
  uint32_t probes_orphaned = 0;
  uint32_t probes_unpredictable = 0;  // arrived while the leaf had no fit
  bool probe_error_seen = false;
  int64_t probe_error_us = 0;
  int64_t probe_error_min_us = 0;
  int64_t probe_error_max_us = 0;
  // Sum and sum-of-squares rather than a stored history: an RMS over the whole
  // run is what a soak wants, and keeping the samples would be a buffer sized by
  // how long someone leaves it running.
  // Sums over TYPICAL samples only. A handful of millisecond excursions -- 2 in
  // 301 on the first soak -- pulled the reported RMS from ~50 us to 240 us, which
  // reads as five times worse than every percentile of the same data says it is.
  // An accuracy figure a 0.7% tail can set is not an accuracy figure, so the tail
  // is counted separately and loudly rather than averaged in silently.
  int64_t probe_error_sum_us = 0;
  uint64_t probe_error_sum_sq = 0;
  uint32_t probe_error_count = 0;
  int64_t probe_error_mean_us = 0;  // running, over typical samples
  uint32_t probe_excursions = 0;    // beyond kExcursionUs of the running mean
  int64_t probe_excursion_worst_us = 0;

  // The same probe scored against a NAIVE clock model: the first offset we ever
  // saw, held forever, with no skew term. That is what "sync once at startup"
  // would have given, so the gap between this and probe_error_us is what the
  // rolling fit is actually buying -- and it grows with the recording, which a
  // single instantaneous comparison would never show.
  bool naive_seen = false;
  int64_t naive_offset_us = 0;
  int64_t naive_error_us = 0;
  int64_t naive_error_worst_us = 0;

  // This node's answer to the most recent marker it reported (#315). Held so the
  // primary can pair it against another node's answer to the SAME marker.
  bool marker_seen = false;
  uint32_t marker_seq = 0;
  uint64_t marker_primary_us = 0;
  uint64_t marker_local_us = 0;
  uint8_t marker_quality = 0;
  uint32_t markers_reported = 0;
};

esp_err_t espNowPrimaryStart();

// Snapshot of the tracked nodes. Returned by pointer to a fixed array rather than
// by value because the caller is a logging loop, not a consumer of history.
const NodeState *espNowPrimaryNodes();
uint32_t espNowPrimaryUnknownPackets();

// --- Node-to-node coherence (#315) ------------------------------------------
//
// Computed on the primary because it is the only device that hears every leaf's
// answer to the same marker. It is a property of a PAIR, not of a node, which is
// why it does not live in NodeState.
struct CoherenceStats {
  bool seen = false;
  uint32_t markers_paired = 0;
  int64_t spread_us = 0;       // latest: leaf A's answer minus leaf B's
  int64_t spread_min_us = 0;
  int64_t spread_max_us = 0;
  int64_t spread_sum_us = 0;
  uint64_t spread_sum_sq = 0;
  uint32_t excursions = 0;     // beyond kCoherenceExcursionUs of the running mean
  int64_t excursion_worst_us = 0;
  uint64_t device_a = 0;
  uint64_t device_b = 0;
};

// One number for "how far apart can two of this rig's nodes be", in microseconds,
// and a quality level to go with it. This is #315's deliverable: what a stream
// should carry so a consumer can decide whether two sensors' samples may be
// compared.
struct CoherenceMetric {
  uint8_t quality = 0;         // SyncQuality, the WORST across contributing nodes
  bool measured = false;       // true = from markers; false = derived from fits
  uint32_t typical_us = 0;     // 1 sd, the everyday figure
  uint32_t bound_us = 0;       // the conservative one: 3 sd + the excursion tail
  uint32_t worst_seen_us = 0;
  uint32_t samples = 0;
  uint32_t stale_us = 0;       // age of the newest contributing measurement
};

const CoherenceStats &espNowPrimaryCoherence();
CoherenceMetric espNowPrimaryCoherenceMetric();

// Timing-master state (#340), for the console and for whatever forwards it.
uint32_t espNowPrimaryEpoch();
uint32_t espNowPrimaryBeaconSeq();
// Beacons whose send callback never fired, so no follow-up could carry a real
// transmit stamp. Counted because it is the one failure mode that would quietly
// starve every leaf's fit while the beacons themselves kept arriving.
uint32_t espNowPrimaryBeaconsWithoutTxStamp();
// esp_wifi_get_tsf_time() as read in the last beacon's send callback. Expected to
// be 0 -- see TimeFollowUp::tx_tsf_us -- and exposed so that expectation is a
// measurement on the console rather than an assumption in a comment.
uint64_t espNowPrimaryLastTxTsf();

}  // namespace natkit
