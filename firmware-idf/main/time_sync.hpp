#pragma once

#include <cstddef>
#include <cstdint>

#include "espnow_link.hpp"

namespace natkit {

// Estimating the primary's clock from its timing broadcast (#340 / TEC-NATKIT-17).
//
// A leaf has no NTP and no wall clock by design, so its sample timestamps are
// monotonic since ITS OWN boot. Two nodes' timestamps are therefore not
// comparable, which is the problem this file exists to solve: it fits the leaf's
// clock to the primary's, so that anything downstream can put two nodes' samples
// on one time axis.
//
// --- What we fit, and why it is a fit rather than a subtraction ---------------
//
// A single beacon gives one offset estimate, and that estimate is wrong by
// whatever jitter that one packet suffered. Worse, the two crystals run at
// different rates -- tens of ppm is ordinary, which is tens of milliseconds of
// divergence over a five-minute recording -- so an offset captured once is stale
// almost immediately. So we fit a LINE over a window of beacons:
//
//   primary_us  ~=  ref_offset_us + local_us + (local_us - ref_local_us) * skew
//
// The slope gives the relative crystal rate (reported as skew_ppb) and the
// residual around the line gives an honest quality figure. This is the ticket's
// third recommended approach ("linear regression clock fitting"), and the
// residual is the raw material #315 needs for a confidence metric.
//
// --- Where the samples come from ---------------------------------------------
//
// Each sample pairs a leaf-side receive time with a primary-side transmit time,
// and BOTH ends of that pair are taken as close to the radio as the API allows:
//
//   y (primary): read inside the primary's ESP-NOW send callback for the beacon,
//     and delivered afterwards in a separate follow-up packet. Reading the clock
//     before esp_now_send would measure the transmit queue -- CSMA and driver
//     queueing are milliseconds and variable -- rather than the clock.
//   x (leaf): esp_timer at the receive callback, which is the domain sample
//     timestamps are already in, so the fit is directly applicable to data.
//
// Propagation delay is ignored on purpose: these are metres apart, so time of
// flight is nanoseconds -- four orders of magnitude below the jitter we are
// actually fighting. It would be a constant offset even if it mattered.
//
// --- The MAC receive stamp, and why it is a diagnostic rather than the input ---
//
// wifi_pkt_rx_ctrl_t::timestamp IS a hardware receive stamp taken below FreeRTOS,
// which is what the ticket asks for, but it is in the MAC's clock domain rather
// than esp_timer's -- and sample timestamps are in esp_timer's. Using it as x
// would mean fitting leaf-MAC-time to primary time and then needing a SECOND fit
// from esp_timer to MAC time to make it usable, which just moves the jitter.
//
// So it is carried as a measurement instead: the per-beacon difference between
// the MAC stamp and the esp_timer read tells us how much of our residual is
// receive-callback scheduling latency. If that spread turns out to be the
// dominant term, this decision should be revisited with the number in hand --
// which is the point of measuring it rather than assuming either way.

// 32 beacons at 1 Hz: long enough for the slope to be worth fitting, short enough
// that the fit follows a real temperature-driven drift instead of averaging it
// away. Also the window over which the MAC-stamp spread is reported.
constexpr size_t kSyncWindow = 32;

// A crystal pair further apart than this is not a crystal pair -- it is a bad
// fit, a misparsed packet or a beacon from something that is not our primary.
// Ordinary ESP32 crystals are +/-10..40 ppm each, so 200 ppm is generous by about
// a factor of three and still nowhere near the range where a wrong answer looks
// plausible.
constexpr int32_t kMaxPlausibleSkewPpb = 200'000;  // 200 ppm

enum class SyncQuality : uint8_t {
  kUnsynced = 0,  // nothing usable: no beacons, or the fit was rejected
  kCoarse = 1,    // offset known, slope not yet fitted (fewer than kMinFitSamples)
  kLocked = 2,    // offset and slope both fitted over a populated window
};

// Below this many paired samples we hold the offset but do not claim a slope: a
// two-point "slope" through two jittered samples one second apart is not a
// crystal measurement, it is the jitter divided by one second.
constexpr size_t kMinFitSamples = 8;

struct TimeSyncStatus {
  SyncQuality quality = SyncQuality::kUnsynced;

  // Which primary boot this fit belongs to. A primary that reboots restarts its
  // esp_timer at zero, so every fit against the old epoch is not merely stale but
  // wrong by the whole previous uptime -- and it would look perfectly healthy,
  // because a step change is invisible to a slope. The epoch is what makes that
  // detectable instead of silent.
  uint32_t epoch = 0;

  // The fit, anchored at ref_local_us so the arithmetic downstream stays in
  // integers: primary_us = local_us + ref_offset_us + (local_us - ref_local_us) *
  // skew_ppb / 1e9.
  uint64_t ref_local_us = 0;
  int64_t ref_offset_us = 0;
  int32_t skew_ppb = 0;

  // Fit quality. RMS in nanoseconds because a good fit here is expected to land
  // well under a microsecond of residual and an integer count of microseconds
  // would report "0" for everything worth distinguishing.
  uint32_t residual_rms_ns = 0;
  uint32_t peak_residual_ns = 0;
  size_t samples_used = 0;

  uint64_t last_beacon_local_us = 0;

  // Counters. Named for what they count (this epic has now been bitten three
  // times by a counter whose name was one step off what it measured).
  uint32_t beacons_seen = 0;
  uint32_t followups_seen = 0;
  uint32_t pairs_used = 0;
  uint32_t pairs_orphaned = 0;   // a beacon whose follow-up never arrived
  uint32_t outliers_rejected = 0;
  uint32_t beacons_missed = 0;   // from gaps in the beacon sequence
  uint32_t epoch_changes = 0;
  uint32_t implausible_fits = 0;

  // The MAC-stamp diagnostic described in the header comment. The difference
  // itself is a large arbitrary constant (two clock origins), so only its SPREAD
  // over the window is meaningful -- that spread is the receive-callback
  // scheduling jitter we are choosing to live with.
  bool mac_stamp_valid = false;
  int64_t mac_minus_timer_us = 0;
  uint32_t mac_spread_us = 0;

  // The transmit-queue delay the two-step protocol exists to remove: the beacon
  // carries the primary's clock at ENQUEUE, the follow-up carries it at TRANSMIT,
  // and the difference is how long the packet sat in CSMA and the driver queue.
  // Kept because it is the measurement that justifies the design -- if it turned
  // out to be tens of microseconds, the follow-up would be an extra packet per
  // second buying nothing, and that should be visible rather than assumed.
  bool queue_delay_seen = false;
  uint32_t queue_delay_us = 0;
  uint32_t queue_delay_min_us = 0;
  uint32_t queue_delay_max_us = 0;
};

// Feeds one received beacon. rx_local_us is esp_timer read in the receive
// callback; rx_mac_us is rx_ctrl->timestamp, the raw 32-bit MAC stamp (pass 0
// when unavailable). Holds the beacon until its follow-up arrives.
void timeSyncOnBeacon(const TimeBeacon &beacon, uint64_t rx_local_us,
                      uint32_t rx_mac_us);

// Feeds the follow-up carrying the transmit stamp of an earlier beacon. Completes
// the pair and updates the fit.
void timeSyncOnFollowUp(const TimeFollowUp &follow_up);

const TimeSyncStatus &timeSyncStatus();

// Converts a leaf-local timestamp into the primary's time base. Returns false
// while unsynced rather than returning a plausible-looking number -- a converted
// timestamp that is silently uncorrected is worse than an absent one, because
// nothing downstream can tell the difference.
bool timeSyncToPrimary(uint64_t local_us, uint64_t &primary_us);

// The fit as it goes on the wire, for the primary and the gateway to apply.
void timeSyncFillWire(SyncState &out);

// Applies a fit received over the wire. Deliberately a free function taking the
// wire struct rather than a method: the primary applies a fit it did not compute,
// for a clock it does not have, which is exactly what makes this a proxy.
bool syncStateToPrimary(const SyncState &state, uint64_t local_us,
                        uint64_t &primary_us);

// Rewrites a canonical frame's timestamps from leaf-device time into wall clock,
// IN PLACE, applying both hops: the leaf's fit to reach primary time, then
// `primary_to_wall_us` to reach the epoch.
//
// Lives here rather than with either caller because BOTH the gateway
// (TEC-NATKIT-26) and the primary running #373's WiFi uplink need it, and it is
// a time transformation rather than frame construction. Patched in place: a
// decode and re-encode round trip would be a third implementation of an encoding
// that already has two (see imu_frame.hpp).
//
// ⚠️ The two fields use DIFFERENT UNITS, which is the easy mistake: the frame
// header's deviceTsUs is microseconds, each sample's time is milliseconds.
bool rewriteFrameTimestamps(uint8_t *frame, size_t length, const SyncState &sync,
                            int64_t primary_to_wall_us);

}  // namespace natkit
