#include <cinttypes>

#include "bno08x.hpp"
#include "device_id.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "node_role.hpp"
#include "sdkconfig.h"

// Leaf (secondary) node: IMU + ESP-NOW, nothing else.
//
// The sensor is in (TEC-NATKIT-22). Still to come:
//   TEC-NATKIT-23  the on-air frame format, which decides whether this file
//                  fragments a ~5 KB bulk frame or sends a smaller one.
//   TEC-NATKIT-24  esp_now_send of those frames plus following the primary's
//                  1-second timing broadcast.
//
// Until then the samples go to the console, which is what makes them comparable
// with the current firmware's output by eye and by soak.
//
// Deliberately absent, and this is the architecture rather than an omission: no
// esp_wifi_connect, no MQTT client, no SNTP. ESP-NOW needs the WiFi driver
// started but never associated, so a leaf should never link lwIP at all.

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
  ESP_LOGI(kTag, "leaf: device %" PRIu64 ", IMU on SPI", deviceId());

  Bno08x imu;
  if (imu.begin() != ESP_OK) {
    ESP_LOGE(kTag,
             "IMU did not start -- staying alive so the console is still "
             "readable rather than rebooting in a loop");
    idleStatusLoop("leaf (no IMU)");
  }

  // The sample loop pumps SHTP as fast as it can rather than on a timer: the hub
  // decides when reports are ready (INT), the report interval is configured on
  // the hub itself, and a sleep here just adds latency and risks the queue
  // backing up. The 1ms delay keeps the idle task and the watchdog fed.
  constexpr TickType_t kPumpDelay = pdMS_TO_TICKS(1);
  constexpr uint64_t kLogIntervalUs =
      static_cast<uint64_t>(CONFIG_NATKIT_IMU_LOG_INTERVAL_MS) * 1000ULL;

  uint64_t next_log_us = 0;
  uint32_t reports_at_last_log = 0;
  uint64_t last_log_us = 0;

  while (true) {
    imu.service();
    imu.enableDynamicCalibrationOnce();

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    if (kLogIntervalUs > 0 && now >= next_log_us) {
      const SensorSet &r = imu.readings();
      const uint32_t total = imu.totalReports();

      // Rate is computed from the actual elapsed time, not assumed from the log
      // interval: a pump loop that falls behind is exactly what this line exists
      // to make visible, and dividing by the nominal interval would hide it.
      const uint64_t elapsed_us = last_log_us == 0 ? 0 : now - last_log_us;
      const float hz = elapsed_us == 0
                           ? 0.0F
                           : static_cast<float>(total - reports_at_last_log) *
                                 1'000'000.0F / static_cast<float>(elapsed_us);

      ESP_LOGI(kTag,
               "accel %+7.3f %+7.3f %+7.3f (%s) | gyro %+7.3f %+7.3f %+7.3f "
               "(%s) | mag %+7.1f %+7.1f %+7.1f (%s)",
               r.accelerometer.x, r.accelerometer.y, r.accelerometer.z,
               accuracyName(r.accelerometer.accuracy), r.gyroscope.x,
               r.gyroscope.y, r.gyroscope.z,
               accuracyName(r.gyroscope.accuracy), r.magnetometer.x,
               r.magnetometer.y, r.magnetometer.z,
               accuracyName(r.magnetometer.accuracy));
      ESP_LOGI(kTag,
               "quat %+6.3f %+6.3f %+6.3f %+6.3f (%s, %.3f rad) | %lu reports "
               "@ %.1f Hz | resets %lu | cal %s | heap %" PRIu32 " B (min %" PRIu32
               " B)",
               r.rotation.x, r.rotation.y, r.rotation.z, r.rotation.w,
               accuracyName(r.rotation.accuracy),
               r.rotation.rotation_accuracy_rad,
               static_cast<unsigned long>(total), hz,
               static_cast<unsigned long>(imu.resetCount()),
               imu.calibrationEnabled() ? "on" : "pending",
               static_cast<uint32_t>(esp_get_free_heap_size()),
               static_cast<uint32_t>(esp_get_minimum_free_heap_size()));

      // Per-sensor counts, not just the total. The total alone cannot say
      // whether all four reports are arriving at the configured interval or one
      // of them is running fast and masking another that is silent -- and the
      // hub's actual rate is not the rate we asked for
      // (CONFIG_NATKIT_IMU_REPORT_INTERVAL_US), which is a comparability
      // question for any recording made with this firmware.
      ESP_LOGI(kTag,
               "reports: accel %lu, gyro %lu, mag %lu, rotation %lu (interval "
               "asked %d us)",
               static_cast<unsigned long>(r.accelerometer.count),
               static_cast<unsigned long>(r.gyroscope.count),
               static_cast<unsigned long>(r.magnetometer.count),
               static_cast<unsigned long>(r.rotation.count),
               CONFIG_NATKIT_IMU_REPORT_INTERVAL_US);

      const HalStats &hal = Bno08x::halStats();
      ESP_LOGI(kTag,
               "shtp: %lu reads -> %lu packets, empty %lu, oversize %lu, spi "
               "fail %lu/%lu, INT timeouts %lu, last header %u, INT now %d",
               static_cast<unsigned long>(hal.read_calls),
               static_cast<unsigned long>(hal.packets_read),
               static_cast<unsigned long>(hal.empty_headers),
               static_cast<unsigned long>(hal.oversize_headers),
               static_cast<unsigned long>(hal.header_transfer_failed),
               static_cast<unsigned long>(hal.body_transfer_failed),
               static_cast<unsigned long>(hal.int_timeouts), hal.last_header,
               Bno08x::intLevel());
      ESP_LOGI(kTag, "shtp: last packet channel %u seq %u", hal.last_channel,
               hal.last_seq);

      reports_at_last_log = total;
      last_log_us = now;
      next_log_us = now + kLogIntervalUs;
    }

    vTaskDelay(kPumpDelay);
  }
}

}  // namespace natkit
