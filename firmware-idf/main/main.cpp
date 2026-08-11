#include <cinttypes>

#include "device_id.hpp"
#include "espnow_probe.hpp"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "node_role.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "version.hpp"

// natKit-IMU node firmware, native ESP-IDF (EPIC TEC-NATKIT-20).
//
// One tree, three images: app_main logs what it is and hands off to the role
// selected in menuconfig. Every role is a scaffold at this slice
// (TEC-NATKIT-21) -- what this file proves is that the fork builds for both
// targets, boots, and says which of the three it is.
//
// The Arduino firmware in ../embeded is untouched and remains the image on the
// bench; see ../README.md for the rollback command.

#if !CONFIG_IDF_TARGET_ESP32 && !CONFIG_IDF_TARGET_ESP32C3
#error "natKit-IMU fork supports classic ESP32 (the IMU's PICO-D4) and ESP32-C3 only"
#endif

namespace {

constexpr char kTag[] = "natkit-fw";

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external pin";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "other watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep wake";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "unknown";
  }
}

// The banner is the deliverable of this slice, so it carries the things a bench
// question actually needs answered: which firmware and version booted (two
// images speak to the same broker), which role, which chip, which device id
// (the number inside every topic name), and why the board last restarted -- a
// panic reset that scrolls past is how a crash loop gets mistaken for a hang.
void logBootBanner() {
  esp_chip_info_t chip = {};
  esp_chip_info(&chip);

  const uint8_t *mac = natkit::deviceMac();

  ESP_LOGI(kTag, "%s v%s (built %s %s)", NATKIT_IMU_IDF_FIRMWARE_NAME,
           NATKIT_IMU_IDF_VERSION_STRING, NATKIT_IMU_IDF_BUILD_DATE,
           NATKIT_IMU_IDF_BUILD_TIME);
  ESP_LOGI(kTag, "role: %s", natkit::roleName(natkit::kRole));
  // esp_chip_info_t::revision is packed MXX -- wafer major * 100 + minor -- so
  // it is split rather than printed raw (a PICO-V3-02 reads 301, not 3).
  ESP_LOGI(kTag, "target: %s rev %d.%d, %d core(s), ESP-IDF %s",
           CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
           chip.cores, esp_get_idf_version());
  ESP_LOGI(kTag,
           "device id: %" PRIu64 " (mac %02x:%02x:%02x:%02x:%02x:%02x)",
           natkit::deviceId(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  ESP_LOGI(kTag, "last reset: %s, heap free %" PRIu32 " B",
           resetReasonName(esp_reset_reason()),
           static_cast<uint32_t>(esp_get_free_heap_size()));
}

// NVS is initialised here rather than in the roles because the gateway's WiFi
// stack requires it and the primary will want it for the node registry, and
// because a truncated or version-bumped partition has to be erased once -- a
// failure a role stub would otherwise hit as an opaque ESP_ERR_NVS_*.
void initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "erasing NVS partition (%s)", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

}  // namespace

extern "C" void app_main(void) {
  logBootBanner();
  initNvs();

#if CONFIG_NATKIT_ESPNOW_PROBE
  // The bench instrument replaces the role entirely (TEC-NATKIT-23). Kept as an
  // early return rather than woven into the role switch, so no role's code path
  // changes shape because a measurement tool exists.
  ESP_LOGW(kTag, "ESP-NOW PROBE build -- this is a bench tool, not a node");
  natkit::runEspNowProbe();
#endif

  switch (natkit::kRole) {
    case natkit::NodeRole::kLeaf:
      natkit::runLeaf();
      break;
    case natkit::NodeRole::kPrimary:
      natkit::runPrimary();
      break;
    case natkit::NodeRole::kGateway:
      natkit::runGateway();
      break;
  }

  // No role entry point is supposed to return. If one does, idle loudly instead
  // of falling off the end of app_main: that deletes the main task silently and
  // leaves a board that logged a healthy banner and then said nothing, which
  // reads on the bench like a wedged UART rather than a firmware bug.
  ESP_LOGE(kTag, "role %s returned -- this is a bug",
           natkit::roleName(natkit::kRole));
  natkit::idleStatusLoop("returned-role");
}
