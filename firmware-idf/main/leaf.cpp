#include <cinttypes>

#include "bno08x.hpp"
#include "device_id.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "espnow_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu_frame.hpp"
#include "node_role.hpp"
#include "sdkconfig.h"

// Leaf (secondary) node: IMU + ESP-NOW, and that is the whole device.
//
// TEC-NATKIT-22 put the sensor in. TEC-NATKIT-23 decided the on-air format: the
// canonical NatImuBulkDataSchema frame is built HERE, on the node, and one frame is
// one packet -- 524 bytes against a measured 1470-byte ceiling, so there is no
// fragmentation and no reassembly state on the primary. This file is
// TEC-NATKIT-24: it turns the sensor's reports into those frames and puts them on
// the radio.
//
// Deliberately absent, and this is the architecture rather than an omission: no
// esp_wifi_connect, no MQTT client, no SNTP, no HTTP stack, and no netif at all.
// The headroom that buys is the point of the whole epic.
//
// ⚠️ Timestamps here are MONOTONIC SINCE BOOT, not wall clock. A leaf has no NTP by
// design, so whatever forwards these frames must translate them against the
// primary's clock -- a gateway that published them unmodified would advertise
// 1970-era timestamps. That translation is the timing slice (#340), and it is not
// done here; the frames are honest about carrying device-relative time.

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-leaf";

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

void runLeaf() {
  ESP_LOGI(kTag, "leaf: device %" PRIu64 ", IMU on SPI, ESP-NOW only",
           deviceId());

  Bno08x imu;
  if (imu.begin() != ESP_OK) {
    ESP_LOGE(kTag,
             "IMU did not start -- staying alive so the console is still "
             "readable rather than rebooting in a loop");
    idleStatusLoop("leaf (no IMU)");
  }

  if (espNowLinkStart() != ESP_OK) {
    ESP_LOGE(kTag, "ESP-NOW did not start; the sensor still runs, so the console "
                   "remains useful for diagnosing it");
  }

  // The sample loop pumps SHTP as fast as it can rather than on a timer: the hub
  // decides when reports are ready (INT), the report interval is configured on the
  // hub itself, and a sleep here just adds latency. Sampling for the FRAME is on a
  // fixed interval, which is a different thing -- see below.
  constexpr TickType_t kPumpDelay = pdMS_TO_TICKS(1);
  constexpr uint64_t kLogIntervalUs =
      static_cast<uint64_t>(CONFIG_NATKIT_IMU_LOG_INTERVAL_MS) * 1000ULL;
  constexpr uint64_t kSampleIntervalUs = CONFIG_NATKIT_IMU_SAMPLE_INTERVAL_US;
  constexpr uint64_t kHeartbeatIntervalUs =
      static_cast<uint64_t>(CONFIG_NATKIT_ESPNOW_HEARTBEAT_MS) * 1000ULL;
  constexpr size_t kSamplesPerFrame = CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME;
  static_assert(kSamplesPerFrame <= kMaxSamplesPerFrame,
                "samples per frame exceeds the transmit buffer sized from it");

  ImuSample samples[kMaxSamplesPerFrame]{};
  size_t sample_count = 0;
  uint64_t frame_seq = 0;
  uint32_t frames_built = 0;

  // One frame buffer, reused. Not an optimisation: the old firmware's
  // std::bad_alloc came from the frame path allocating per copy, so nothing on this
  // path allocates after startup.
  static uint8_t frame[kFrameHeaderSize + kMaxSamplesPerFrame * kSampleSize];

  uint64_t next_log_us = 0;
  uint64_t next_sample_us = 0;
  uint64_t next_heartbeat_us = kHeartbeatIntervalUs;
  uint32_t reports_at_last_log = 0;
  uint32_t frames_at_last_log = 0;
  uint64_t last_log_us = 0;

  while (true) {
    imu.service();
    imu.enableDynamicCalibrationOnce();

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

    // --- pack a sample on a fixed interval -----------------------------------
    //
    // A snapshot of the latest reading of each sensor, taken every
    // CONFIG_NATKIT_IMU_SAMPLE_INTERVAL_US. The four SH2 reports arrive
    // asynchronously at their own rates (~64 Hz accel, ~98 Hz for the rest), so a
    // "sample" is necessarily a merge across them -- which is exactly what the
    // schema carries and what the current firmware sends.
    if (now >= next_sample_us) {
      next_sample_us = now + kSampleIntervalUs;
      ImuSample sample{};
      if (sampleFromReadings(imu.readings(), sample) &&
          sample_count < kSamplesPerFrame) {
        samples[sample_count++] = sample;
      }

      if (sample_count == kSamplesPerFrame) {
        // deviceTsUs is the FIRST sample's time, matching the current firmware
        // (kafkaTopic.hpp uses imuDataList[0].getTime() * 1000).
        const uint64_t device_ts_us = samples[0].time_ms * 1000ULL;
        const size_t length =
            encodeFrame(samples, sample_count, frame_seq, device_ts_us,
                        CONFIG_NATKIT_IMU_DECLARED_RATE_HZ, frame, sizeof(frame));
        sample_count = 0;

        if (length == 0) {
          ESP_LOGE(kTag, "frame encoding failed -- layout constants disagree");
        } else {
          ++frame_seq;
          ++frames_built;
          // Never blocks, whether or not a primary is listening. That is the
          // requirement: the sample loop must survive the primary being powered
          // off, dropping frames rather than stalling.
          espNowLinkSend(PacketType::kData, frame, length);
        }
      }
    }

    // --- heartbeat -----------------------------------------------------------
    if (kHeartbeatIntervalUs > 0 && now >= next_heartbeat_us) {
      next_heartbeat_us = now + kHeartbeatIntervalUs;
      const SensorSet &r = imu.readings();
      const LinkStats &link = espNowLinkStats();

      Heartbeat beat{};
      beat.device_id = deviceId();
      beat.uptime_us = now;
      beat.frames_built = frames_built;
      beat.frames_sent = link.packets_sent;
      beat.frames_dropped = link.packets_dropped;
      beat.send_failures = link.send_failures;
      beat.sensor_reports = imu.totalReports();
      beat.hub_resets = imu.resetCount();
      beat.free_heap = static_cast<uint32_t>(esp_get_free_heap_size());
      beat.accuracy_accel = r.accelerometer.accuracy;
      beat.accuracy_gyro = r.gyroscope.accuracy;
      beat.accuracy_mag = r.magnetometer.accuracy;
      beat.accuracy_rotation = r.rotation.accuracy;
      espNowLinkSend(PacketType::kHeartbeat, &beat, sizeof(beat));
    }

    // --- console -------------------------------------------------------------
    if (kLogIntervalUs > 0 && now >= next_log_us) {
      const SensorSet &r = imu.readings();
      const uint32_t total = imu.totalReports();
      const LinkStats &link = espNowLinkStats();

      // Rates from the actual elapsed time, not assumed from the log interval: a
      // loop that falls behind is exactly what these lines exist to make visible.
      const uint64_t elapsed_us = last_log_us == 0 ? 0 : now - last_log_us;
      const float report_hz =
          elapsed_us == 0 ? 0.0F
                          : static_cast<float>(total - reports_at_last_log) *
                                1'000'000.0F / static_cast<float>(elapsed_us);
      const float frame_hz =
          elapsed_us == 0 ? 0.0F
                          : static_cast<float>(frames_built - frames_at_last_log) *
                                1'000'000.0F / static_cast<float>(elapsed_us);

      ESP_LOGI(kTag,
               "accel %+7.3f %+7.3f %+7.3f (%s) | gyro %+7.3f %+7.3f %+7.3f "
               "(%s) | quat %+6.3f %+6.3f %+6.3f %+6.3f (%s)",
               r.accelerometer.x, r.accelerometer.y, r.accelerometer.z,
               accuracyName(r.accelerometer.accuracy), r.gyroscope.x,
               r.gyroscope.y, r.gyroscope.z, accuracyName(r.gyroscope.accuracy),
               r.rotation.x, r.rotation.y, r.rotation.z, r.rotation.w,
               accuracyName(r.rotation.accuracy));
      ESP_LOGI(kTag,
               "sensor: %lu reports @ %.1f Hz | resets %lu | cal %s | heap "
               "%" PRIu32 " B (min %" PRIu32 " B)",
               static_cast<unsigned long>(total), report_hz,
               static_cast<unsigned long>(imu.resetCount()),
               imu.calibrationEnabled() ? "on" : "pending",
               static_cast<uint32_t>(esp_get_free_heap_size()),
               static_cast<uint32_t>(esp_get_minimum_free_heap_size()));
      // Data frames and total packets are labelled separately on purpose: the leaf
      // also sends heartbeats and announces down this path, so a single "sent"
      // figure next to the frame count reads as though more frames were sent than
      // were ever built.
      ESP_LOGI(kTag,
               "link: %s | data frames built %lu @ %.1f/s | packets sent %lu, "
               "dropped %lu, tx failures %lu, retries %lu, announces %lu",
               !espNowLinkHasPrimary() ? "SEARCHING for a primary"
                   : link.primary_absent ? "primary PRESUMED GONE (unicast, 1 try)"
                                         : "primary known (unicast)",
               static_cast<unsigned long>(frames_built), frame_hz,
               static_cast<unsigned long>(link.packets_sent),
               static_cast<unsigned long>(link.packets_dropped),
               static_cast<unsigned long>(link.send_failures),
               static_cast<unsigned long>(link.send_retries),
               static_cast<unsigned long>(link.announces));

      reports_at_last_log = total;
      frames_at_last_log = frames_built;
      last_log_us = now;
      next_log_us = now + kLogIntervalUs;
    }

    vTaskDelay(kPumpDelay);
  }
}

}  // namespace natkit
