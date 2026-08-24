#include "status_led.hpp"

#include <cstring>

#include "board_config.hpp"
#include "esp_log.h"
#include "led_strip.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace natkit {
namespace {

constexpr char kTag[] = "led";
constexpr char kNamespace[] = "natkit";
constexpr char kKeyColour[] = "led_colour";

led_strip_handle_t sStrip = nullptr;
LedColour sCurrent = kLedOff;

esp_err_t ensureStrip() {
  if (sStrip != nullptr) {
    return ESP_OK;
  }
  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = kStatusLedGpio;
  strip_config.max_leds = kStatusLedCount;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  strip_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10 * 1000 * 1000;  // 10 MHz, 0.1 us per tick
  rmt_config.mem_block_symbols = 0;             // driver default
  rmt_config.flags.with_dma = false;            // one pixel needs no DMA

  const esp_err_t err =
      led_strip_new_rmt_device(&strip_config, &rmt_config, &sStrip);
  if (err != ESP_OK) {
    // Warn, never abort. A board with no LED soldered on, or an RMT channel that
    // could not be had, must not be a board that refuses to stream.
    ESP_LOGW(kTag, "indicator unavailable (%s); carrying on without it",
             esp_err_to_name(err));
    sStrip = nullptr;
  }
  return err;
}

esp_err_t apply(const LedColour &colour) {
  const esp_err_t ready = ensureStrip();
  if (ready != ESP_OK) {
    return ready;
  }
  // Brightness is applied here rather than expected of the caller, so the
  // frontend can send the colour a person picked and a cap still holds. A WS2812
  // at full output is both dazzling on a bench and a real current draw next to a
  // transmitting radio.
  const auto scale = [&](uint8_t component) -> uint32_t {
    return static_cast<uint32_t>(component) * colour.brightness / 255;
  };
  esp_err_t err = led_strip_set_pixel(sStrip, 0, scale(colour.r), scale(colour.g),
                                      scale(colour.b));
  if (err == ESP_OK) {
    err = led_strip_refresh(sStrip);
  }
  return err;
}

}  // namespace

LedColour statusLedCurrent() { return sCurrent; }

esp_err_t statusLedRestore() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    // No namespace yet: nothing has ever been set. Dark is the right answer.
    ESP_LOGI(kTag, "no stored colour; indicator stays off");
    return ESP_OK;
  }
  LedColour stored{};
  size_t size = sizeof(stored);
  err = nvs_get_blob(handle, kKeyColour, &stored, &size);
  nvs_close(handle);
  if (err != ESP_OK || size != sizeof(stored)) {
    ESP_LOGI(kTag, "no stored colour; indicator stays off");
    return ESP_OK;
  }
  sCurrent = stored;
  const esp_err_t applied = apply(sCurrent);
  ESP_LOGI(kTag, "restored indicator r=%u g=%u b=%u brightness=%u (%s)",
           sCurrent.r, sCurrent.g, sCurrent.b, sCurrent.brightness,
           esp_err_to_name(applied));
  return applied;
}

esp_err_t statusLedSet(const LedColour &colour) {
  const esp_err_t applied = apply(colour);
  if (applied != ESP_OK) {
    return applied;
  }
  sCurrent = colour;

  // ⚠️ Persisted only AFTER it visibly worked. Storing first would mean a board
  // whose LED cannot be driven comes back from every reboot claiming a colour it
  // has never shown.
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "colour applied but not persisted (%s)", esp_err_to_name(err));
    return ESP_OK;  // the light is right; losing it on reboot is the lesser fault
  }
  err = nvs_set_blob(handle, kKeyColour, &sCurrent, sizeof(sCurrent));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "colour applied but not persisted (%s)", esp_err_to_name(err));
  }
  return ESP_OK;
}

}  // namespace natkit
