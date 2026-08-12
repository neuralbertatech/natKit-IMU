#include "time_sync.hpp"

#include <cmath>
#include <cstring>

#include "esp_log.h"

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

void resetWindow() {
  sCount = 0;
  sNext = 0;
  sStatus.quality = SyncQuality::kUnsynced;
  sStatus.samples_used = 0;
  sStatus.residual_rms_ns = 0;
  sStatus.peak_residual_ns = 0;
  sStatus.skew_ppb = 0;
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

  const uint64_t x0 = sWindow[0].local_us;
  const uint64_t y0 = sWindow[0].primary_us;

  double sum_x = 0.0, sum_y = 0.0;
  for (size_t i = 0; i < sCount; ++i) {
    sum_x += static_cast<double>(sWindow[i].local_us - x0);
    sum_y += static_cast<double>(static_cast<int64_t>(sWindow[i].primary_us - y0));
  }
  const double mean_x = sum_x / static_cast<double>(sCount);
  const double mean_y = sum_y / static_cast<double>(sCount);

  double sxx = 0.0, sxy = 0.0;
  for (size_t i = 0; i < sCount; ++i) {
    const double dx = static_cast<double>(sWindow[i].local_us - x0) - mean_x;
    const double dy =
        static_cast<double>(static_cast<int64_t>(sWindow[i].primary_us - y0)) -
        mean_y;
    sxx += dx * dx;
    sxy += dx * dy;
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
    const double x = static_cast<double>(sWindow[i].local_us - x0);
    const double y =
        static_cast<double>(static_cast<int64_t>(sWindow[i].primary_us - y0));
    const double residual = y - (intercept + slope * x);
    sum_sq += residual * residual;
    const double magnitude = residual < 0.0 ? -residual : residual;
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  const double rms = std::sqrt(sum_sq / static_cast<double>(sCount));

  // Anchor the fit at the newest sample rather than at the window's origin: every
  // conversion downstream is of a timestamp near NOW, so extrapolating from the
  // newest point keeps the skew term's lever arm short, and the anchor is a real
  // measured instant rather than a projection.
  const Pair &newest = sWindow[(sNext + kSyncWindow - 1) % kSyncWindow];
  const double x_new = static_cast<double>(newest.local_us - x0);
  const double predicted_y = intercept + slope * x_new;
  const int64_t predicted_primary =
      static_cast<int64_t>(y0) + static_cast<int64_t>(predicted_y + 0.5);

  sStatus.ref_local_us = newest.local_us;
  sStatus.ref_offset_us =
      predicted_primary - static_cast<int64_t>(newest.local_us);
  sStatus.skew_ppb = static_cast<int32_t>(skew_ppb_d);
  sStatus.residual_rms_ns = static_cast<uint32_t>(rms * 1000.0);
  sStatus.peak_residual_ns = static_cast<uint32_t>(peak * 1000.0);
  sStatus.samples_used = sCount;
  sStatus.quality =
      sCount >= kMinFitSamples ? SyncQuality::kLocked : SyncQuality::kCoarse;

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
  if (sStatus.quality != SyncQuality::kLocked) {
    return false;
  }
  uint64_t predicted = 0;
  if (!timeSyncToPrimary(local_us, predicted)) {
    return false;
  }
  const int64_t residual_us =
      static_cast<int64_t>(primary_us) - static_cast<int64_t>(predicted);
  const int64_t magnitude = residual_us < 0 ? -residual_us : residual_us;

  // Three sigma, with a floor: early on the residual RMS can be small enough that
  // three times it rejects ordinary samples and the window freezes with whatever
  // it happened to contain.
  const int64_t sigma_us =
      static_cast<int64_t>(sStatus.residual_rms_ns / 1000) + 1;
  const int64_t limit = sigma_us * 3 < 500 ? 500 : sigma_us * 3;
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

}  // namespace natkit
