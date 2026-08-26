#include "gateway_net.hpp"

#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "DevConfig.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-gwnet";

GatewayNetStats sStats{};
esp_mqtt_client_handle_t sMqtt = nullptr;

// Subscriptions are REMEMBERED, not fire-and-forget. esp-mqtt reconnects by
// itself and does not restore subscriptions, so one made at startup survives
// exactly until the first disconnect -- after which commands would vanish with
// nothing logging that they had.
constexpr size_t kMaxSubscriptions = 8;
constexpr size_t kMaxTopicLength = 96;
char sSubscriptions[kMaxSubscriptions][kMaxTopicLength] = {};
size_t sSubscriptionCount = 0;
GatewayMessageHandler sMessageHandler = nullptr;

void resubscribeAll() {
  for (size_t i = 0; i < sSubscriptionCount; ++i) {
    const int id = esp_mqtt_client_subscribe(sMqtt, sSubscriptions[i], 0);
    if (id < 0) {
      ESP_LOGW(kTag, "could not subscribe to %s", sSubscriptions[i]);
    }
  }
  if (sSubscriptionCount > 0) {
    ESP_LOGI(kTag, "(re)subscribed to %u topic(s)",
             static_cast<unsigned>(sSubscriptionCount));
  }
}

void wifiEventHandler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    sStats.wifi_connected = false;
    ++sStats.wifi_disconnects;
    // Reconnect immediately and keep doing so. The failure this avoids is the
    // one the current firmware had: a device that gives up on the network and
    // needs a human to power-cycle it, in a rig that is otherwise unattended.
    esp_wifi_connect();
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<ip_event_got_ip_t *>(data);
    sStats.wifi_connected = true;
    ESP_LOGI(kTag, "wifi up, ip " IPSTR, IP2STR(&event->ip_info.ip));
  }
}

void mqttEventHandler(void *, esp_event_base_t, int32_t id, void *data) {
  const auto *event = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
      sStats.mqtt_connected = true;
      ESP_LOGI(kTag, "mqtt connected to %s", DEV_MQTT_URI);
      resubscribeAll();
      break;
    case MQTT_EVENT_DATA:
      if (sMessageHandler != nullptr && event != nullptr) {
        ++sStats.mqtt_messages_received;
        sMessageHandler(event->topic, static_cast<size_t>(event->topic_len),
                        event->data, static_cast<size_t>(event->data_len));
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      // Tracked from the EVENT, never inferred from a publish return code. A
      // half-open socket returns success from publish, which is exactly how the
      // old firmware convinced itself it was still streaming into nothing.
      sStats.mqtt_connected = false;
      ++sStats.mqtt_disconnects;
      ESP_LOGW(kTag, "mqtt disconnected (%lu so far); the client will retry",
               static_cast<unsigned long>(sStats.mqtt_disconnects));
      break;
    case MQTT_EVENT_ERROR:
      ++sStats.mqtt_errors;
      if (event != nullptr && event->error_handle != nullptr) {
        ESP_LOGW(kTag, "mqtt error, type %d", event->error_handle->error_type);
      }
      break;
    default:
      break;
  }
}

void sntpSyncCallback(struct timeval *tv) {
  sStats.time_synced = true;
  ESP_LOGI(kTag, "clock synced: %lld s since the epoch",
           static_cast<long long>(tv->tv_sec));
}

}  // namespace

uint8_t gatewayWifiChannel() {
  if (!sStats.wifi_connected) {
    return 0;
  }
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
    return 0;
  }
  return primary;
}

esp_err_t gatewayWifiStart() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr, nullptr));

  wifi_config_t wifi{};
  std::strncpy(reinterpret_cast<char *>(wifi.sta.ssid), DEV_WIFI_SSID,
               sizeof(wifi.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(wifi.sta.password), DEV_WIFI_PASSWORD,
               sizeof(wifi.sta.password) - 1);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());
  // ⚠️ Power save OFF, and leaving it unsaid was a measurable defect rather than
  // an omission of style. An associated station defaults to WIFI_PS_MIN_MODEM
  // and sleeps between the AP's beacons, so publishes queue up and go out in
  // bursts at beacon boundaries. Measured at the broker: frames arriving 3 ms
  // apart and then not for 2.8 s, against a steady 200 ms cadence one hop
  // earlier -- which is what "the live stream looks choppy" actually was.
  //
  // The leaf and primary already set this for #340's timing work; the gateway
  // needs it for a plainer reason. It is mains-adjacent, always on, and its
  // entire job is forwarding promptly.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  return ESP_OK;
}

esp_err_t gatewayServicesStart() {
  // esp_netif_sntp, NOT a raw lwIP client. IDF 5.x defaults the lwIP
  // thread-safety assert ON, which is what tripped ESPNtpClient in the current
  // firmware; this wrapper does its work on the right task.
  esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(DEV_NTP_SERVER);
  sntp.sync_cb = sntpSyncCallback;
  sntp.start = true;
  sntp.server_from_dhcp = false;
  // ⚠️⚠️ LEFT ON THE DEFAULT (IMMED), AND SNTP_SYNC_MODE_SMOOTH WAS TRIED AND IS
  // WORSE. The argument for smooth was real -- this clock stamps every sample, so a
  // backwards STEP writes non-monotonic timestamps into a recording. But measured
  // on this board, smooth mode is a much bigger problem than the one it solves:
  //
  //   after a reflash the hub came up 41 SECONDS out and adjtime slewed it back at
  //   ~4.4 s per 300 s -- all four leaves reporting an identical +20.8 s offset
  //   forty minutes later, with healthy fits. Every sample recorded in that window
  //   is stamped tens of seconds wrong. IMMED would have stepped it at the first
  //   poll. The 35-minute threshold at which smooth mode gives up and steps is far
  //   too loose to protect against this.
  //
  // What actually removes the non-monotonicity risk is the 60 s poll below, not the
  // sync mode: the error between polls is ~2 ms rather than the ~119 ms an hourly
  // poll accumulates, so any step is ~2 ms. Small steps at a known cadence beat a
  // slew that cannot catch up.
  ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

  esp_mqtt_client_config_t mqtt{};
  mqtt.broker.address.uri = DEV_MQTT_URI;
  // Let esp-mqtt own reconnection. It has a real state machine; the hand-rolled
  // loop in the current firmware had to learn backoff, keepalive and half-open
  // detection one outage at a time.
  mqtt.network.reconnect_timeout_ms = 2000;
  mqtt.network.timeout_ms = 5000;
  mqtt.session.keepalive = 30;
  // A broker that is DOWN AT BOOT must not strand the device. esp-mqtt retries
  // on this timer forever rather than failing start, which is the behaviour the
  // old firmware needed a manual reset to recover from.
  sMqtt = esp_mqtt_client_init(&mqtt);
  if (sMqtt == nullptr) {
    ESP_LOGE(kTag, "could not create the mqtt client");
    return ESP_FAIL;
  }
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(
      sMqtt, MQTT_EVENT_ANY, mqttEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_mqtt_client_start(sMqtt));

  ESP_LOGI(kTag,
           "networking started: ssid '%s', broker %s, ntp %s. Not waiting for "
           "any of them -- the intake side keeps draining so nothing upstream is "
           "back-pressured by our network.",
           DEV_WIFI_SSID, DEV_MQTT_URI, DEV_NTP_SERVER);
  return ESP_OK;
}

esp_err_t gatewayNetStart() {
  const esp_err_t err = gatewayWifiStart();
  if (err != ESP_OK) {
    return err;
  }
  return gatewayServicesStart();
}

bool gatewayTimeValid() {
  // Anything before 2020 is the RTC's power-on value, not a synced clock. The
  // sync callback is the primary signal; this is the belt-and-braces check that
  // stops a 1970 timestamp reaching a recording if the callback is ever missed.
  return sStats.time_synced && gatewayWallClockUs() > 1577836800ULL * 1000000ULL;
}

uint64_t gatewayWallClockUs() {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(tv.tv_usec);
}

bool gatewayPublish(const char *topic, const void *payload,
                    const size_t length, const bool retain) {
  if (sMqtt == nullptr || !sStats.mqtt_connected) {
    ++sStats.publishes_failed;
    return false;
  }
  // ⚠️ esp_mqtt_client_publish, NOT esp_mqtt_client_enqueue, and this was
  // measured rather than reasoned.
  //
  // `enqueue(..., store=true)` looked like the right call: non-blocking, and it
  // returns a message id so nothing appears to fail. It caps throughput at
  // roughly ONE MESSAGE PER MQTT POLL CYCLE, because the queued outbox is
  // serviced by the client's own task loop. Measured: the gateway reported
  // publishing ~5 frames/s with "refused 0" while mosquitto received 1.07/s and
  // Kafka 1.10/s -- about 78% of the stream lost, with every counter on the
  // device saying it was fine, because the counter was counting ENQUEUES rather
  // than deliveries.
  //
  // publish() hands the frame to the socket on this task instead. At QoS 0 there
  // is no acknowledgement to wait for, so it is quick, and a failure is returned
  // rather than absorbed.
  const int id = esp_mqtt_client_publish(
      sMqtt, topic, static_cast<const char *>(payload),
      static_cast<int>(length), 0, retain ? 1 : 0);
  if (id < 0) {
    ++sStats.publishes_failed;
    return false;
  }
  ++sStats.publishes_ok;
  sStats.bytes_published += length;
  return true;
}

void gatewaySetMessageHandler(const GatewayMessageHandler handler) {
  sMessageHandler = handler;
}

bool gatewaySubscribe(const char *topic) {
  if (topic == nullptr || sSubscriptionCount >= kMaxSubscriptions) {
    ESP_LOGE(kTag, "no room to subscribe to %s (%u of %u used)",
             topic == nullptr ? "(null)" : topic,
             static_cast<unsigned>(sSubscriptionCount),
             static_cast<unsigned>(kMaxSubscriptions));
    return false;
  }
  if (std::strlen(topic) >= kMaxTopicLength) {
    ESP_LOGE(kTag, "topic too long to remember: %s", topic);
    return false;
  }
  // Already subscribed is success, not a duplicate: nodes re-announce, and this
  // is called per node.
  for (size_t i = 0; i < sSubscriptionCount; ++i) {
    if (std::strcmp(sSubscriptions[i], topic) == 0) {
      return true;
    }
  }
  std::strncpy(sSubscriptions[sSubscriptionCount], topic, kMaxTopicLength - 1);
  ++sSubscriptionCount;
  if (sMqtt != nullptr && sStats.mqtt_connected) {
    return esp_mqtt_client_subscribe(sMqtt, topic, 0) >= 0;
  }
  // Not connected yet is fine -- MQTT_EVENT_CONNECTED replays the whole list.
  return true;
}

const GatewayNetStats &gatewayNetStats() {
  wifi_ap_record_t ap{};
  if (sStats.wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    sStats.rssi = ap.rssi;
  }
  return sStats;
}

}  // namespace natkit
