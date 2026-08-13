#include "channel_survey.hpp"

#include <cmath>

#include "esp_log.h"
#include "registry.hpp"
#include <cstring>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-survey";
constexpr uint8_t kMaxChannel = 13;

ChannelSurveyResult sResult{};
volatile uint8_t sCurrentChannel = 0;

// Runs on the WiFi task for EVERY frame in the air, so it does the least
// possible: one add and one comparison. Anything heavier here would distort the
// very measurement it is taking, by delaying the receiver.
void promiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (buf == nullptr || sCurrentChannel == 0 || sCurrentChannel > kMaxChannel) {
    return;
  }
  // Management, data and control frames all occupy airtime, so all of them
  // count. Filtering to data would under-report an access point that is mostly
  // beaconing -- which is exactly what a quiet-looking but crowded channel is.
  (void)type;
  const auto *pkt = static_cast<wifi_promiscuous_pkt_t *>(buf);
  const int8_t rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);

  // ⚠️ IGNORE OUR OWN NODES. A leaf that has not found a hub HOPS channels, so
  // during a survey it sprays its own traffic across the band -- at a few
  // centimetres that arrives around -20 dBm, which is louder than any access
  // point and swamps the score. Measured: channels 12 and 13 scored 560 million
  // and 1.2 billion against a genuine access point's 4 million, purely because
  // the leaf's hop happened to land there. Whichever channels get polluted is
  // arbitrary, so left in it would make the choice effectively random.
  //
  // addr2 is the transmitter address, at offset 10 of the 802.11 MAC header for
  // every frame type this can see.
  if (pkt->rx_ctrl.sig_len >= 16) {
    const uint8_t *addr2 = pkt->payload + 10;
    const RegistryEntry *entries = registryEntries();
    for (size_t i = 0; i < kRegistryMaxNodes; ++i) {
      if (entries[i].in_use && std::memcmp(entries[i].mac, addr2, 6) == 0) {
        return;
      }
    }
  }

  const uint8_t ch = sCurrentChannel;
  if (sResult.packets[ch] < 0xFFFF) {
    ++sResult.packets[ch];
  }
  if (sResult.packets[ch] == 1 || rssi > sResult.strongest[ch]) {
    sResult.strongest[ch] = rssi;
  }
  // Energy, not a count. 10^(rssi/10) in arbitrary units, scaled so a -100 dBm
  // frame contributes ~1 and a -20 dBm one contributes ~10^8 -- which is the
  // right ratio, because one loud neighbour ruins a channel that a hundred
  // distant beacons would not.
  const double energy = std::pow(10.0, (rssi + 100) / 10.0);
  const uint32_t capped =
      energy > 4000000000.0 ? 4000000000u : static_cast<uint32_t>(energy);
  if (sResult.score[ch] < 4000000000u - capped) {
    sResult.score[ch] += capped;
  }
}

}  // namespace

uint8_t channelSurveyRun(uint8_t fallback, uint32_t dwell_ms) {
  sResult = ChannelSurveyResult{};

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
      esp_wifi_set_promiscuous_rx_cb(promiscuousCallback) != ESP_OK ||
      esp_wifi_set_promiscuous(true) != ESP_OK) {
    ESP_LOGW(kTag, "could not enter promiscuous mode; keeping channel %u",
             fallback);
    return fallback;
  }

  ESP_LOGI(kTag,
           "surveying all %u channels at %lu ms each (~%lu s) -- this fits "
           "inside the window where NTP has not synced and nothing could be "
           "published anyway",
           kMaxChannel, static_cast<unsigned long>(dwell_ms),
           static_cast<unsigned long>(kMaxChannel * dwell_ms / 1000));

  for (uint8_t ch = 1; ch <= kMaxChannel; ++ch) {
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
      continue;
    }
    // Set AFTER the channel change, so frames still draining from the previous
    // channel are not charged to this one.
    sCurrentChannel = ch;
    vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    sCurrentChannel = 0;
  }

  esp_wifi_set_promiscuous(false);

  uint8_t best = fallback;
  uint32_t best_score = 0xFFFFFFFFu;
  for (uint8_t ch = 1; ch <= kMaxChannel; ++ch) {
    if (sResult.score[ch] < best_score) {
      best_score = sResult.score[ch];
      best = ch;
    }
  }

  sResult.ran = true;
  sResult.chosen = best;

  for (uint8_t ch = 1; ch <= kMaxChannel; ++ch) {
    ESP_LOGI(kTag, "  ch %2u: %5u frames, strongest %4d dBm, score %10lu%s", ch,
             sResult.packets[ch],
             sResult.packets[ch] > 0 ? sResult.strongest[ch] : 0,
             static_cast<unsigned long>(sResult.score[ch]),
             ch == best ? "   <- chosen" : "");
  }
  ESP_LOGW(kTag,
           "channel %u is the quietest of %u surveyed. ⚠️ This finds CROWDED "
           "channels, not NOISY ones -- non-802.11 interference (the Thread BR "
           "board's own clocks on channel 1) is invisible here, so still compare "
           "the two directions' RSSI if throughput disappoints.",
           best, kMaxChannel);
  return best;
}

const ChannelSurveyResult &channelSurveyResult() { return sResult; }

}  // namespace natkit
