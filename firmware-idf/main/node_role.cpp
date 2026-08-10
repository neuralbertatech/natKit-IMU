#include "node_role.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "esp_system.h"  // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-node";

}  // namespace

const char *roleName(NodeRole role) {
  switch (role) {
    case NodeRole::kLeaf:
      return "leaf";
    case NodeRole::kPrimary:
      return "primary";
    case NodeRole::kGateway:
      return "gateway";
  }
  return "unknown";
}

// `what` is unused when the interval is 0 (status logging off), hence
// maybe_unused rather than a second overload.
[[noreturn]] void idleStatusLoop([[maybe_unused]] const char *what) {
#if CONFIG_NATKIT_STATUS_LOG_INTERVAL_S > 0
  constexpr TickType_t kInterval =
      pdMS_TO_TICKS(CONFIG_NATKIT_STATUS_LOG_INTERVAL_S * 1000);
#else
  // Interval 0 means "no status line". Still tick, rather than spinning or
  // blocking forever, so the idle task and any future watchdog stay happy.
  constexpr TickType_t kInterval = pdMS_TO_TICKS(1000);
#endif

  while (true) {
    vTaskDelay(kInterval);
#if CONFIG_NATKIT_STATUS_LOG_INTERVAL_S > 0
    ESP_LOGI(kTag,
             "%s alive: up %" PRId64 "s, heap free %" PRIu32 " B (min %" PRIu32
             " B)",
             what, esp_timer_get_time() / 1000000,
             static_cast<uint32_t>(esp_get_free_heap_size()),
             static_cast<uint32_t>(esp_get_minimum_free_heap_size()));
#endif
  }
}

}  // namespace natkit
