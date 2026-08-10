#include "device_id.hpp"

#include "esp_mac.h"

namespace natkit {
namespace {

uint8_t sMac[6] = {0};
uint64_t sDeviceId = 0;
bool sLoaded = false;

void loadOnce() {
  if (sLoaded) {
    return;
  }
  // esp_efuse_mac_get_default is what the Arduino firmware calls, so the bytes
  // are the same six bytes in the same order. Deriving the id from
  // esp_wifi_get_mac or esp_read_mac(ESP_MAC_WIFI_SOFTAP) instead would shift
  // it, and a leaf never brings WiFi up anyway.
  esp_efuse_mac_get_default(sMac);
  sDeviceId = packMac(sMac);
  sLoaded = true;
}

// The natKit-IMU node's real MAC and the device id it appears as in every topic
// name today (Data-13793649670644-Json-..., Command-13793649670644-Json-...).
// The static_assert below is the regression guard for the packing: this is the
// kind of "tidy-up" that compiles perfectly and silently renames the device, at
// which point its recordings stop being comparable with the current firmware's
// and the bench in TEC-NATKIT-27 is measuring two different things.
constexpr uint8_t kKnownImuMac[6] = {0x0c, 0x8b, 0x95, 0x96, 0xb9, 0xf4};
static_assert(packMac(kKnownImuMac) == 13793649670644ULL,
              "device id packing no longer matches the deployed topic names");

}  // namespace

uint64_t deviceId() {
  loadOnce();
  return sDeviceId;
}

const uint8_t *deviceMac() {
  loadOnce();
  return sMac;
}

}  // namespace natkit
