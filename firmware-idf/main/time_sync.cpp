#include "time_sync.hpp"

#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "imu_frame.hpp"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-sync";

struct Pair {
  uint64_t local_us = 0;    // x: leaf esp_timer at the receive callback
  uint64_t primary_us = 0;  // y: primary esp_timer inside its send callback
  int64_t mac_delta_us = 0; // diagnostic: MAC stamp minus the esp_timer read
};

Pair sWindow[kSyncWindow];
size_t sCount = 0;  // how many slots are populated (saturates at kSyncWindow)
size_t sNext = 0;   // ring cursor

TimeSyncStatus sStatus{};

// The beacon we are holding while its follow-up is in flight. One slot, not a
// queue: the follow-up is sent immediately after its beacon's send callback, so
// at 1 Hz there is never a second beacon outstanding. A beacon that is still
// waiting when the next one arrives is an orphan and is counted as one.
struct PendingBeacon {
  bool valid = false;
  uint32_t seq = 0;
  uint32_t epoch = 0;
  uint64_t rx_local_us = 0;
  uint64_t enqueue_us = 0;
  int64_t mac_delta_us = 0;
  bool mac_valid = false;
};
PendingBeacon sPending{};

// Beacon sequence tracking, so a missed beacon is counted rather than merely
// absent from the window.
uint32_t sLastBeaconSeq = 0;
bool sBeaconSeqSeen = false;

// --- 32-bit MAC stamp extension ---------------------------------------------
//
// rx_ctrl->timestamp is 32 bits of microseconds, so it wraps every 2^32 us =
// ~4295 s, a little over 71 minutes. A soak longer than that would see the
// diagnostic invert without this, which is exactly the sort of thing that gets
// read as a clock bug.
uint32_t sLastMacRaw = 0;
uint64_t sMacWrapBase = 0;
bool sMacSeen = false;

uint64_t extendMacStamp(uint32_t raw) {
  if (sMacSeen && raw < sLastMacRaw) {
    sMacWrapBase += (1ULL << 32);
  }
  sLastMacRaw = raw;
  sMacSeen = true;
  return sMacWrapBase + raw;
}

// A window this far off its own line, REPEATEDLY. One is a transient -- a
// mispaired follow-up, a sample taken across a scheduling stall -- and throwing
// the whole window away for it costs ~2 s of kUnsynced, which the primary turns
// into dropped frames. Three in a row is a window that is not going to recover
// on its own, which is the case #392 found had no way out.
//
// Same shape as kRejectsBeforeReset below and for the same reason: reset is the
// escape hatch, not the first response.
constexpr uint32_t kImplausibleBeforeReset = 3;
uint32_t sConsecutiveImplausible = 0;

void resetWindow() {
  sConsecutiveImplausible = 0;
  sCount = 0;
  sNext = 0;
  sStatus.quality = SyncQuality::kUnsynced;
  sStatus.samples_used = 0;
  sStatus.residual_rms_ns = 0;
  sStatus.peak_residual_ns = 0;
  sStatus.skew_ppb = 0;
}

// Store a residual without inventing a value for one that does not fit.
//
// ⚠️ THE CAST THIS REPLACES WAS THE WHOLE OF #392 / TEC-NATKIT-47. It was
// `static_cast<uint32_t>(rms * 1000.0)`, which is undefined for a double past
// UINT32_MAX and on Xtensa produced 0xFFFFFFFF -- a specific wrong number, not a
// flag. isOutlier() then read it back as a sigma, so the rejection limit became
// ~12.9 s, nothing was ever an outlier again, and the window could not shed the
// samples that had made it bad. `outliers_rejected` stayed at 0 throughout,
// which reads as a clean signal rather than a dead guard.
uint32_t residualToNs(double us) {
  if (!std::isfinite(us) || us < 0.0) {
    return kResidualSaturatedNs;
  }
  const double ns = us * 1000.0;
  if (ns >= static_cast<double>(kResidualSaturatedNs)) {
    return kResidualSaturatedNs;
  }
  return static_cast<uint32_t>(ns);
}

// Least squares over the populated window.
//
// The window is small (32) and this runs once a second, so the cost of doing it
// in double on a chip with no double-precision FPU is irrelevant -- and the
// magnitudes involved (uptimes in microseconds, so ~1e8 after an hour and ~1e11
// after a day) are exactly where float's 24-bit mantissa starts quantising the
// input to tens of microseconds. Subtracting the window's first sample keeps the
// numbers small; doing it in double as well means the guard is belt and braces
// rather than load-bearing.
void refit() {
  if (sCount < 2) {
    return;
  }

  // ⚠️ BOTH DIFFERENCES ARE SIGNED, AND THE x ONE USED NOT TO BE. That was a
  // two-second data outage every 32 seconds, on every leaf, for as long as this
  // estimator has existed.
  //
  // sWindow is a RING BUFFER. While it is filling, sWindow[0] is the oldest
  // sample and every difference below is positive. The moment it wraps -- at
  // exactly kSyncWindow samples, so 32 seconds at one beacon a second --
  // sWindow[0] is overwritten with the NEWEST sample, and every other entry is
  // then older than the origin. In uint64 arithmetic those differences underflow
  // to about 1.8e19 instead of going negative, which destroys the slope, which
  // trips the implausible-skew guard, which resets the window.
  //
  // The guard did its job: it refused a fit that was genuinely nonsense. But the
  // leaf then reported kUnsynced for the ~2 s it took to rebuild, and the primary
  // silently DROPS every frame it cannot timestamp-shift (publish_no_shift), so
  // ~20 frames per leaf per 32 s never reached the broker. Measured before and
  // after on the same node.
  //
  // The y term already had the int64_t cast and so was immune; the x term did
  // not. Nothing about the two lines suggested one was protected and the other
  // was not, which is why this survived so long.
  const uint64_t x0 = sWindow[0].local_us;
  const uint64_t y0 = sWindow[0].primary_us;

  // ⚠️ ONE PLACE, BECAUSE THE SECOND PLACE IS WHAT WENT WRONG. The signed
  // difference was open-coded in three loops below and the cast was missing from
  // exactly one of them -- the residual loop's x -- for as long as this estimator
  // has existed. It is the SAME defect the comment above describes and the same
  // one that was fixed in the sums; fixing it there and not here is what
  // TEC-NATKIT-47 actually was. These two lambdas exist so there is no longer a
  // second place to forget.
  const auto dx = [&](size_t i) {
    return static_cast<double>(static_cast<int64_t>(sWindow[i].local_us - x0));
  };
  const auto dy = [&](size_t i) {
    return static_cast<double>(static_cast<int64_t>(sWindow[i].primary_us - y0));
  };

  double sum_x = 0.0, sum_y = 0.0;
  for (size_t i = 0; i < sCount; ++i) {
    sum_x += dx(i);
    sum_y += dy(i);
  }
  const double mean_x = sum_x / static_cast<double>(sCount);
  const double mean_y = sum_y / static_cast<double>(sCount);

  double sxx = 0.0, sxy = 0.0;
  for (size_t i = 0; i < sCount; ++i) {
    const double cx = dx(i) - mean_x;
    const double cy = dy(i) - mean_y;
    sxx += cx * cx;
    sxy += cx * cy;
  }
  if (sxx <= 0.0) {
    return;  // every sample at the same instant: nothing to fit
  }

  const double slope = sxy / sxx;
  const double intercept = mean_y - slope * mean_x;

  // Reject a fit that claims a crystal pair no crystal pair could be. Counted
  // rather than silently clamped: clamping would produce a wrong answer that
  // looks like a right one, and the whole value of this estimator is that its
  // quality figures can be believed.
  const double skew_ppb_d = (slope - 1.0) * 1e9;
  if (!std::isfinite(skew_ppb_d) ||
      skew_ppb_d > static_cast<double>(kMaxPlausibleSkewPpb) ||
      skew_ppb_d < -static_cast<double>(kMaxPlausibleSkewPpb)) {
    ++sStatus.implausible_fits;
    ESP_LOGW(kTag,
             "discarding a fit claiming %.0f ppb of skew over %u samples -- no "
             "crystal pair is that far apart, so this is a bad window rather "
             "than a measurement",
             skew_ppb_d, static_cast<unsigned>(sCount));
    resetWindow();
    return;
  }

  double sum_sq = 0.0;
  double peak = 0.0;
  for (size_t i = 0; i < sCount; ++i) {
    const double residual = dy(i) - (intercept + slope * dx(i));
    sum_sq += residual * residual;
    const double magnitude = residual < 0.0 ? -residual : residual;
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  const double rms = std::sqrt(sum_sq / static_cast<double>(sCount));

  // A fit whose samples are nowhere near its own line, handled the way the
  // implausible-skew guard above handles a slope no crystal pair could produce:
  // say so, do not publish it as a measurement, and eventually throw the window
  // away -- because the per-sample guard cannot clean this one. It is downstream
  // of this very number, which is what #392 found.
  //
  // ⚠️ NOT AN IMMEDIATE RESET, and the reason is on the hardware right now:
  // three of the four leaves on the bench are sitting at a saturated residual
  // today. An unconditional reset here would put those three into a rebuild
  // every second -- ~2 s of kUnsynced each time, which the primary turns
  // straight into dropped frames (publish_no_sync). That trade is the one the
  // 32-second-reset bug already taught this file not to make.
  if (!std::isfinite(rms) || rms > kMaxPlausibleResidualUs) {
    ++sStatus.implausible_residuals;
    // Publish the state we are actually in. The old code returned here leaving
    // the PREVIOUS fit's residual in place, so a window that had gone bad kept
    // advertising the last good number it happened to hold.
    sStatus.residual_rms_ns = kResidualSaturatedNs;
    sStatus.peak_residual_ns = kResidualSaturatedNs;
    sStatus.quality = SyncQuality::kCoarse;
    if (++sConsecutiveImplausible >= kImplausibleBeforeReset) {
      ESP_LOGW(kTag,
               "%lu fits in a row whose samples sit off their own line (last "
               "%.0f us rms over %u samples): rebuilding the window, because no "
               "amount of new samples fixes one that is anchored wrong",
               static_cast<unsigned long>(sConsecutiveImplausible), rms,
               static_cast<unsigned>(sCount));
      resetWindow();  // which clears the streak
    }
    return;
  }
  sConsecutiveImplausible = 0;

  // Anchor the fit at the newest sample rather than at the window's origin: every
  // conversion downstream is of a timestamp near NOW, so extrapolating from the
  // newest point keeps the skew term's lever arm short, and the anchor is a real
  // measured instant rather than a projection.
  const size_t newest_i = (sNext + kSyncWindow - 1) % kSyncWindow;
  const Pair &newest = sWindow[newest_i];
  const double predicted_y = intercept + slope * dx(newest_i);
  const int64_t predicted_primary =
      static_cast<int64_t>(y0) + static_cast<int64_t>(predicted_y + 0.5);

  sStatus.ref_local_us = newest.local_us;
  sStatus.ref_offset_us =
      predicted_primary - static_cast<int64_t>(newest.local_us);
  sStatus.skew_ppb = static_cast<int32_t>(skew_ppb_d);
  sStatus.residual_rms_ns = residualToNs(rms);
  sStatus.peak_residual_ns = residualToNs(peak);
  sStatus.samples_used = sCount;
  // ⚠️ QUALITY IS NOW ABOUT THE FIT, NOT JUST ABOUT HOW MANY SAMPLES WENT INTO
  // IT. It used to be `sCount >= kMinFitSamples ? kLocked : kCoarse`, which is a
  // sample count wearing the word "quality": a window latched onto a bad sample
  // reported "good" for its whole life, and every instrument downstream --
  // including the primary's worst-of-all-nodes figure -- repeated it. A fit can
  // now say that it is a poor one, which is the whole point of measuring the
  // residual.
  //
  // kCoarse rather than kUnsynced on purpose: the fit is still the best estimate
  // we have and timestamps still convert through it. Refusing to convert would
  // turn a quality signal into dropped data (publish_no_shift), which is the
  // trade the 32-second reset bug already taught us not to make.
  sStatus.quality = sCount < kMinFitSamples || rms > kLockResidualUs
                        ? SyncQuality::kCoarse
                        : SyncQuality::kLocked;

  // Spread, not value: the MAC-versus-timer difference is a large arbitrary
  // constant set by two clock origins, and only how much it MOVES is a
  // measurement of anything.
  int64_t min_delta = sWindow[0].mac_delta_us;
  int64_t max_delta = sWindow[0].mac_delta_us;
  for (size_t i = 1; i < sCount; ++i) {
    if (sWindow[i].mac_delta_us < min_delta) {
      min_delta = sWindow[i].mac_delta_us;
    }
    if (sWindow[i].mac_delta_us > max_delta) {
      max_delta = sWindow[i].mac_delta_us;
    }
  }
  sStatus.mac_spread_us = static_cast<uint32_t>(max_delta - min_delta);
}

// True if this pair sits so far off the established fit that it is a jitter spike
// rather than a clock reading.
//
// Only applied once locked: before that there is no fit to be off, and rejecting
// against a fit built from three samples would just entrench whichever three
// arrived first.
bool isOutlier(uint64_t local_us, uint64_t primary_us) {
  // ⚠️ GATE ON HAVING A FIT, NOT ON THE FIT BEING GOOD. This used to require
  // kLocked, which was harmless while kLocked meant "enough samples" -- but now
  // that a poor residual demotes the fit to kCoarse, that test would switch the
  // guard off at exactly the moment its work matters: when the residual is
  // climbing and the window needs to shed something.
  if (sStatus.quality == SyncQuality::kUnsynced || sCount < kMinFitSamples) {
    return false;
  }
  uint64_t predicted = 0;
  if (!timeSyncToPrimary(local_us, predicted)) {
    return false;
  }
  const int64_t residual_us =
      static_cast<int64_t>(primary_us) - static_cast<int64_t>(predicted);
  const int64_t magnitude = residual_us < 0 ? -residual_us : residual_us;

  // Three sigma, clamped at BOTH ends.
  //
  // The floor is the original reason: early on the residual RMS can be small
  // enough that three times it rejects ordinary samples and the window freezes
  // with whatever it happened to contain.
  //
  // ⚠️ THE CEILING IS #392. Deriving a limit from residual_rms_ns means deriving
  // it from a field with a maximum, and a saturated field produced a ~12.9 s
  // limit -- a gate that rejects nothing is a gate that is not there. refit()
  // now throws away any window past kMaxPlausibleResidualUs, so the clamp should
  // never bind; it is here because "should never" is what the previous version
  // of this line assumed too.
  constexpr int64_t kMinOutlierLimitUs = 500;
  constexpr int64_t kMaxOutlierLimitUs =
      3 * static_cast<int64_t>(kMaxPlausibleResidualUs);
  const int64_t sigma_us =
      static_cast<int64_t>(sStatus.residual_rms_ns / 1000) + 1;
  int64_t limit = sigma_us * 3;
  if (limit < kMinOutlierLimitUs) {
    limit = kMinOutlierLimitUs;
  } else if (limit > kMaxOutlierLimitUs) {
    limit = kMaxOutlierLimitUs;
  }
  return magnitude > limit;
}

// A run of rejections means the world changed (the primary was power-cycled onto
// a fresh epoch we somehow missed, or the leaf's clock stepped), not that every
// packet is suddenly noise. Rebuild rather than reject forever.
constexpr uint32_t kRejectsBeforeReset = 8;
uint32_t sConsecutiveRejects = 0;

void push(uint64_t local_us, uint64_t primary_us, int64_t mac_delta_us) {
  if (isOutlier(local_us, primary_us)) {
    ++sStatus.outliers_rejected;
    if (++sConsecutiveRejects >= kRejectsBeforeReset) {
      ESP_LOGW(kTag,
               "%lu consecutive samples rejected against the fit: rebuilding it "
               "rather than holding a fit reality has left behind",
               static_cast<unsigned long>(sConsecutiveRejects));
      sConsecutiveRejects = 0;
      resetWindow();
    }
    return;
  }
  sConsecutiveRejects = 0;

  sWindow[sNext] = Pair{local_us, primary_us, mac_delta_us};
  sNext = (sNext + 1) % kSyncWindow;
  if (sCount < kSyncWindow) {
    ++sCount;
  }
  ++sStatus.pairs_used;
  refit();
}

}  // namespace

void timeSyncOnBeacon(const TimeBeacon &beacon, uint64_t rx_local_us,
                      uint32_t rx_mac_us) {
  ++sStatus.beacons_seen;
  sStatus.last_beacon_local_us = rx_local_us;

  // A primary that rebooted restarted its esp_timer at zero. Every sample in the
  // window is against the old origin, so the fit is not stale -- it is wrong by
  // the primary's entire previous uptime, and a step change like that is
  // invisible to a slope. Throw the window away.
  if (sStatus.epoch != beacon.epoch) {
    if (sStatus.epoch != 0) {
      ++sStatus.epoch_changes;
      ESP_LOGW(kTag,
               "primary epoch changed %08lx -> %08lx: it rebooted, so its clock "
               "restarted at zero and every sample we hold is against an origin "
               "that no longer exists",
               static_cast<unsigned long>(sStatus.epoch),
               static_cast<unsigned long>(beacon.epoch));
    }
    sStatus.epoch = beacon.epoch;
    sBeaconSeqSeen = false;
    sPending = PendingBeacon{};
    resetWindow();
  }

  if (sBeaconSeqSeen && beacon.seq > sLastBeaconSeq + 1) {
    sStatus.beacons_missed += beacon.seq - sLastBeaconSeq - 1;
  }
  sLastBeaconSeq = beacon.seq;
  sBeaconSeqSeen = true;

  if (sPending.valid) {
    // The previous beacon's follow-up never arrived. Counted, because a leaf
    // whose window stops filling while beacons keep coming is diagnosing a lost
    // follow-up, not a lost beacon, and those have different causes.
    ++sStatus.pairs_orphaned;
  }

  sPending.valid = true;
  sPending.seq = beacon.seq;
  sPending.epoch = beacon.epoch;
  sPending.rx_local_us = rx_local_us;
  sPending.enqueue_us = beacon.enqueue_us;
  sPending.mac_valid = rx_mac_us != 0;
  if (sPending.mac_valid) {
    const uint64_t mac_us = extendMacStamp(rx_mac_us);
    sPending.mac_delta_us =
        static_cast<int64_t>(mac_us) - static_cast<int64_t>(rx_local_us);
    sStatus.mac_stamp_valid = true;
    sStatus.mac_minus_timer_us = sPending.mac_delta_us;
  } else {
    sPending.mac_delta_us = 0;
  }
}

void timeSyncOnFollowUp(const TimeFollowUp &follow_up) {
  ++sStatus.followups_seen;

  if (!sPending.valid || sPending.seq != follow_up.seq ||
      sPending.epoch != follow_up.epoch) {
    return;  // not the beacon we are holding
  }
  sPending.valid = false;

  // tx_us of 0 means the primary's send callback never fired for that beacon, so
  // it has no transmit stamp to give us. Dropping the pair is right: the
  // alternative is a sample anchored to a time nothing measured.
  if (follow_up.tx_us == 0) {
    ++sStatus.pairs_orphaned;
    return;
  }

  // Both stamps come from the primary's own clock, so their difference is a
  // clean measurement of how long the beacon sat between esp_now_send returning
  // and the frame actually going out -- no clock comparison involved, and the
  // number this whole two-step protocol exists to keep out of the fit.
  if (follow_up.tx_us > sPending.enqueue_us && sPending.enqueue_us != 0) {
    const uint32_t delay =
        static_cast<uint32_t>(follow_up.tx_us - sPending.enqueue_us);
    sStatus.queue_delay_us = delay;
    if (!sStatus.queue_delay_seen) {
      sStatus.queue_delay_seen = true;
      sStatus.queue_delay_min_us = delay;
      sStatus.queue_delay_max_us = delay;
    }
    if (delay < sStatus.queue_delay_min_us) {
      sStatus.queue_delay_min_us = delay;
    }
    if (delay > sStatus.queue_delay_max_us) {
      sStatus.queue_delay_max_us = delay;
    }
  }

  push(sPending.rx_local_us, follow_up.tx_us, sPending.mac_delta_us);
}

const TimeSyncStatus &timeSyncStatus() { return sStatus; }

bool timeSyncToPrimary(uint64_t local_us, uint64_t &primary_us) {
  if (sStatus.quality == SyncQuality::kUnsynced) {
    return false;
  }
  const int64_t elapsed =
      static_cast<int64_t>(local_us) - static_cast<int64_t>(sStatus.ref_local_us);
  // Integer throughout. elapsed is bounded by how long a conversion can lag the
  // anchor (seconds, so ~1e7 us) and skew_ppb by kMaxPlausibleSkewPpb (2e5), so
  // the product is ~1e12 and nowhere near int64's range -- but the multiply comes
  // first on purpose, because dividing by 1e9 first would round every realistic
  // correction to zero.
  const int64_t drift = (elapsed * static_cast<int64_t>(sStatus.skew_ppb)) /
                        1'000'000'000LL;
  const int64_t result =
      static_cast<int64_t>(local_us) + sStatus.ref_offset_us + drift;
  if (result < 0) {
    return false;
  }
  primary_us = static_cast<uint64_t>(result);
  return true;
}

void timeSyncFillWire(SyncState &out) {
  out = SyncState{};
  out.epoch = sStatus.epoch;
  out.ref_local_us = sStatus.ref_local_us;
  out.ref_offset_us = sStatus.ref_offset_us;
  out.skew_ppb = sStatus.skew_ppb;
  out.residual_rms_ns = sStatus.residual_rms_ns;
  out.peak_residual_ns = sStatus.peak_residual_ns;
  out.last_beacon_local_us = sStatus.last_beacon_local_us;
  out.samples_used = static_cast<uint16_t>(sStatus.samples_used);
  out.quality = static_cast<uint8_t>(sStatus.quality);
  out.beacons_seen = sStatus.beacons_seen;
  out.beacons_missed = sStatus.beacons_missed;
  out.pairs_used = sStatus.pairs_used;
  out.pairs_orphaned = sStatus.pairs_orphaned;
  out.outliers_rejected = sStatus.outliers_rejected;
  out.epoch_changes = sStatus.epoch_changes;
  out.mac_spread_us = sStatus.mac_spread_us;
  // Saturating, because it lives in a single spare byte (see espnow_link.hpp).
  // 255 means "at least 255"; the distinction that matters is 0 versus not-0.
  out.implausible_residuals =
      sStatus.implausible_residuals > 255
          ? 255
          : static_cast<uint8_t>(sStatus.implausible_residuals);
}

bool syncStateToPrimary(const SyncState &state, uint64_t local_us,
                        uint64_t &primary_us) {
  if (state.quality == static_cast<uint8_t>(SyncQuality::kUnsynced)) {
    return false;
  }
  const int64_t elapsed =
      static_cast<int64_t>(local_us) - static_cast<int64_t>(state.ref_local_us);
  const int64_t drift =
      (elapsed * static_cast<int64_t>(state.skew_ppb)) / 1'000'000'000LL;
  const int64_t result =
      static_cast<int64_t>(local_us) + state.ref_offset_us + drift;
  if (result < 0) {
    return false;
  }
  primary_us = static_cast<uint64_t>(result);
  return true;
}


namespace {

template <typename T>
T readLeBytes(const uint8_t *p) {
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(p[i]) << (8 * i);
  }
  return value;
}

template <typename T>
void writeLeBytes(uint8_t *out, T value) {
  for (size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

}  // namespace

// Rewrites a canonical frame's timestamps from leaf-device time into wall clock,
// IN PLACE.
//
// Patched in place rather than decoded and re-encoded: the layout is fixed and
// pinned by static_asserts in imu_frame.hpp, and a decode/re-encode round trip
// would be a third implementation of an encoding that already has two (see that
// file's warning). Touching only the timestamp fields cannot disturb the sample
// data, which is the property that matters.
//
// ⚠️ Units differ between the two fields and this is the easy mistake: the frame
// header's deviceTsUs is MICROseconds, while each sample's time is MILLIseconds.
bool rewriteFrameTimestamps(uint8_t *frame, size_t length,
                            const SyncState &sync, int64_t primary_to_wall_us) {
  if (length < kFrameHeaderSize) {
    return false;
  }
  const uint16_t samples = readLeBytes<uint16_t>(frame + 2);

  // ⚠️ THE SAMPLE SIZE COMES FROM THE FRAME, NOT FROM kSampleSize. This function
  // used the compile-time constant, which meant that the moment frame version 2
  // made kSampleSize 62, every version 1 frame -- 524 bytes for ten samples --
  // failed the length check below and was refused. The primary then SILENTLY
  // DROPPED it into publish_no_shift, so a node running older firmware appeared
  // to be transmitting nothing at all while the hub was receiving every frame.
  //
  // Found when a leaf was swapped for a board that had not been reflashed: 29
  // frames received, 28 dropped, and no Data topic for it. The whole point of
  // putting a version in the header is that both can coexist, and this was the
  // one place that ignored it.
  const uint16_t frame_version = readLeBytes<uint16_t>(frame);
  const size_t sample_size = frame_version >= 2 ? 62u : 50u;
  if (frame_version == 0 || frame_version > kFrameSchemaVersion) {
    return false;  // a newer writer than this build understands
  }
  if (length < kFrameHeaderSize + static_cast<size_t>(samples) * sample_size) {
    return false;
  }

  const auto toWall = [&](uint64_t device_us, uint64_t &wall_us) {
    uint64_t primary_us = 0;
    if (!syncStateToPrimary(sync, device_us, primary_us)) {
      return false;
    }
    const int64_t wall =
        static_cast<int64_t>(primary_us) + primary_to_wall_us;
    if (wall <= 0) {
      return false;
    }
    wall_us = static_cast<uint64_t>(wall);
    return true;
  };

  uint64_t header_wall = 0;
  if (!toWall(readLeBytes<uint64_t>(frame + 16), header_wall)) {
    return false;
  }
  writeLeBytes<uint64_t>(frame + 16, header_wall);

  for (uint16_t i = 0; i < samples; ++i) {
    uint8_t *sample = frame + kFrameHeaderSize + static_cast<size_t>(i) * sample_size;
    const uint64_t device_ms = readLeBytes<uint64_t>(sample);
    uint64_t wall_us = 0;
    if (!toWall(device_ms * 1000ULL, wall_us)) {
      return false;
    }
    writeLeBytes<uint64_t>(sample, wall_us / 1000ULL);
  }
  return true;
}


}  // namespace natkit
