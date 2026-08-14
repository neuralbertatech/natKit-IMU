#pragma once

#include <cstdint>

#include "esp_err.h"
#include "sh2_SensorValue.h"

namespace natkit {

// BNO08x over native ESP-IDF: spi_master transport plus the CEVA sh2 driver
// (components/sh2), replacing the Adafruit_BNO08x stack. TEC-NATKIT-22.
//
// The reports enabled, their interval, and the order of the setup operations are
// all deliberately identical to ../../embeded, because the point of this port is
// that a recording from it is comparable with one from the current firmware.

// One sensor's latest reading plus the bookkeeping needed to tell "no data yet"
// from "data that happens to be zero" -- at rest the accelerometer really does
// read near zero on two axes, so a zero-check is not a validity check.
struct SensorReading {
  float x = 0.0F;  // real for a quaternion
  float y = 0.0F;  // i
  float z = 0.0F;  // j
  float w = 0.0F;  // k (quaternion only)
  uint8_t accuracy = 0;
  uint32_t count = 0;      // reports seen since boot
  uint64_t last_us = 0;    // esp_timer time of the last report
  bool has_data = false;   // false until the first report arrives

  // Radians of estimated error, rotation vector only. This is the BNO08x's real
  // quality signal for the fused quaternion; the 2-bit status accuracy above may
  // simply never be populated for this report, which is the open question behind
  // "Rotation: Unreliable" and is why BOTH are surfaced rather than one.
  float rotation_accuracy_rad = 0.0F;
};

// SHTP transport counters, exposed because a sensor bring-up fails in the
// transport far more often than in the decode, and from the outside "no reports"
// looks the same whether the hub is silent, the header is unreadable, or every
// packet is being dropped as oversized.
struct HalStats {
  uint32_t read_calls = 0;
  uint32_t packets_read = 0;
  uint32_t empty_headers = 0;            // header said length 0
  uint32_t oversize_headers = 0;         // longer than sh2's buffer
  uint32_t header_transfer_failed = 0;   // SPI itself failed
  uint32_t body_transfer_failed = 0;
  uint32_t int_timeouts = 0;             // hub never asserted INT
  uint16_t last_header = 0;
  // SHTP channel and sequence of the last packet. The sequence is the tell for
  // "are these fresh packets or the same one re-presented forever", which the
  // read/packet counters alone cannot distinguish.
  uint8_t last_channel = 0;
  uint8_t last_seq = 0;
};

struct SensorSet {
  SensorReading accelerometer;
  SensorReading gyroscope;
  SensorReading magnetometer;
  SensorReading rotation;
};

class Bno08x {
 public:
  // Brings up SPI, resets the hub, opens sh2, and runs the setup sequence.
  // Returns ESP_OK only when the hub answered its product-id request, so a
  // miswired or dead sensor fails here rather than looking like silence later.
  esp_err_t begin();

  // Pumps SHTP once and folds any delivered reports into the set below.
  // Returns the number of reports decoded in this call.
  int service();

  const SensorSet &readings() const { return readings_; }

  // True once at least one report of any kind has arrived.
  bool streaming() const { return first_report_us_ != 0; }
  uint64_t firstReportUs() const { return first_report_us_; }
  uint32_t totalReports() const { return total_reports_; }

  // The hub reset itself (usually a brownout or a wedged bus). Reports have to
  // be re-enabled when this happens or the stream stops silently; service()
  // handles it and counts it, because a rising count is a hardware problem that
  // would otherwise look like a firmware one.
  uint32_t resetCount() const { return reset_count_; }

  // Dynamic calibration, enabled from the SAMPLE LOOP rather than setup --
  // sh2_setCalConfig returns SH2_ERR_HUB during setup and has done since Feb
  // 2026, which is what left gyro calibration off and rotation pinned at
  // Unreliable. Call this every loop; it does its work about 5s after the first
  // report, retries up to 3 times, and then says so once, loudly.
  void enableDynamicCalibrationOnce();
  bool calibrationEnabled() const { return calibration_enabled_; }

  // Public only because the sh2 sensor callback is a free C function and folds
  // each report in as it is dispatched. Not part of the interface a caller
  // should reach for: use service().
  void applyEvent(const sh2_SensorValue_t &value);

  // Transport counters and the raw INT line. Static because the HAL the sh2
  // driver calls is a set of free functions over one SPI device.
  static const HalStats &halStats();
  static int intLevel();

  // --- runtime report configuration (TEC-NATKIT-40) --------------------------
  //
  // Which reports the hub is asked for, as a bit mask. ⚠️ THE BITS ARE THE SAME
  // ONES has_data USES on the wire -- accel 0b100, gyro 0b010, rotation 0b001,
  // magnetometer 0b1000 -- so a mask and a sample's presence byte can be compared
  // directly, and neither has to be translated to read the other.
  static constexpr uint8_t kReportAccel = 0b0100;
  static constexpr uint8_t kReportGyro = 0b0010;
  static constexpr uint8_t kReportRotation = 0b0001;
  static constexpr uint8_t kReportMagnetometer = 0b1000;
  static constexpr uint8_t kReportAll = 0b1111;

  // ⚠️ AT LEAST ONE MOTION REPORT IS REQUIRED. sampleFromReadings emits nothing
  // unless the accelerometer, gyroscope or rotation vector has data, so a node
  // with only the magnetometer enabled goes silent -- correctly, but
  // indistinguishably from a fault.
  static constexpr uint8_t kReportMotionMask =
      kReportAccel | kReportGyro | kReportRotation;

  uint8_t reportMask() const { return report_mask_; }

  // --- burst cadence instrumentation (TEC-NATKIT-41) -------------------------
  //
  // The hub delivers reports in bursts at ~88 Hz rather than at the rates it is
  // asked for, and that cadence is the real ceiling on distinct observations. It
  // is insensitive to the report count and to the requested rates, so the open
  // question is whether it is the HUB's schedule or OUR polling.
  //
  // ⚠️ THE TWO ARE TOLD APART BY SPREAD, NOT BY THE AVERAGE. A hub emitting on
  // its own clock gives a tight interval; a loop that only looks every so often
  // gives a mean near its own period with a wide spread. Both average ~11 ms, so
  // the average alone cannot distinguish them -- which is why the minimum and
  // maximum are kept rather than just a rate.
  // ⚠️ MEASURED AT THE REPORT, NOT AT THE POLL. The first version of this counted
  // sh2_service() invocations that produced anything, and it disagreed with the
  // freshness counter by a factor of two -- 47 "bursts"/s against 87% of 100
  // samples carrying fresh data, which cannot both be true. The drain loop
  // collapses several arrivals into one invocation, so invocations say how often
  // WE LOOKED, not how often the hub SPOKE. Only the gap between consecutive
  // reports of one sensor answers the actual question.
  struct BurstStats {
    uint32_t service_calls = 0;     // sh2_service() invocations
    uint32_t productive_calls = 0;  // ... that produced at least one report
    // Gaps between consecutive ACCELEROMETER reports. One sensor, because the
    // question is what cadence the hub emits at and mixing sensors would blur
    // four schedules into one histogram.
    uint32_t gaps = 0;
    uint32_t gap_sum_us = 0;
    uint32_t gap_min_us = 0xFFFFFFFF;
    uint32_t gap_max_us = 0;
    // Split at 2 ms: anything closer is the same burst arriving, anything wider
    // is a wait. The ratio is what says bursty or even.
    uint32_t gaps_under_2ms = 0;
  };

  const BurstStats &burstStats() const { return burst_; }
  void resetBurstStats();

  // Applies a mask, persists it, and re-configures the hub. Returns an error
  // without changing anything if no motion report would be left.
  //
  // ⚠️ DISABLING CLEARS THAT SENSOR'S READING. Without it the frame builder keeps
  // copying the last value it saw into every subsequent sample forever -- a
  // plausible-looking number from a sensor that was switched off, which is worse
  // than a zero because nothing about it looks wrong.
  esp_err_t setReportMask(uint8_t mask);

  // ⚠️ Called from begin(), BEFORE the first enableReports(), so a restored mask
  // is what the hub is first configured with rather than something applied a
  // moment later -- which would put one round of unwanted reports on the wire.
  void loadReportMask();

 private:
  bool enableReports();


  SensorSet readings_{};
  uint64_t first_report_us_ = 0;
  uint32_t total_reports_ = 0;
  uint32_t reset_count_ = 0;
  uint8_t report_mask_ = kReportAll;
  BurstStats burst_{};
  uint64_t last_burst_us_ = 0;

  bool calibration_settled_ = false;
  bool calibration_enabled_ = false;
  uint8_t calibration_attempts_ = 0;
  uint64_t calibration_last_attempt_us_ = 0;
};

}  // namespace natkit
