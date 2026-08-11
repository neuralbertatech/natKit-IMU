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

 private:
  bool enableReports();

  SensorSet readings_{};
  uint64_t first_report_us_ = 0;
  uint32_t total_reports_ = 0;
  uint32_t reset_count_ = 0;

  bool calibration_settled_ = false;
  bool calibration_enabled_ = false;
  uint8_t calibration_attempts_ = 0;
  uint64_t calibration_last_attempt_us_ = 0;
};

}  // namespace natkit
