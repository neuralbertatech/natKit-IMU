#include <cinttypes>

#include "device_id.hpp"
#include "esp_log.h"
#include "node_role.hpp"

// Leaf (secondary) node: IMU + ESP-NOW, nothing else.
//
// Scaffold only. What lands here, and in which slice:
//   TEC-NATKIT-22  BNO08x over spi_master + the CEVA sh2 driver, carrying the
//                  hardware-found fixes (prime getSensorEvent before enabling
//                  reports; enable gyro dynamic calibration from the sample
//                  loop, not setup(); mask the SH2 status byte; honour
//                  has_data). Do not "tidy" that ordering -- it is load-bearing
//                  timing, not configuration.
//   TEC-NATKIT-23  the on-air frame format, which decides whether this file
//                  fragments a ~5 KB bulk frame or sends a smaller one.
//   TEC-NATKIT-24  esp_now_send of those frames plus following the primary's
//                  1-second timing broadcast.
//
// Deliberately absent, and this is the architecture rather than an omission: no
// esp_wifi_connect, no MQTT client, no SNTP. ESP-NOW needs the WiFi driver
// started but never associated, so a leaf should never link lwIP at all.

namespace natkit {

void runLeaf() {
  constexpr char kTag[] = "natkit-leaf";

  ESP_LOGW(kTag,
           "leaf role is a scaffold: no sensor and no ESP-NOW yet "
           "(TEC-NATKIT-22 / -23 / -24)");
  ESP_LOGI(kTag, "would send frames to the primary as device %" PRIu64,
           deviceId());

  idleStatusLoop("leaf");
}

}  // namespace natkit
