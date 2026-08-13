#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace natkit {

// The gateway's whole networking surface (#349 / TEC-NATKIT-26).
//
// This is the mirror image of espnow_link.hpp, and the contrast IS the epic: a
// leaf has no association, no lwIP, no MQTT and no NTP, and this file has all
// four. Concentrating them in the one internet-facing device is the whole point
// of the architecture, so everything lwIP-shaped belongs here and nowhere else.
//
// --- The reconnect discipline, which was learned the hard way ---------------
//
// The current firmware's networking failures were not exotic. They were:
//
//   * a HALF-OPEN SOCKET returning success from publish, so the device believed
//     it was streaming into a connection that had gone away;
//   * a broker that was DOWN AT BOOT stranding the device until someone reset
//     it by hand;
//   * NTP tripping lwIP's thread-safety assert, which IDF 5.x defaults ON.
//
// So: esp-mqtt owns its own reconnect (it has a real state machine, unlike the
// hand-rolled loop that had to learn this), publishes report failure rather
// than being fire-and-forget, connection state is tracked from EVENTS rather
// than inferred from a return code, and the clock uses esp_netif_sntp rather
// than a raw lwIP client.

struct GatewayNetStats {
  bool wifi_connected = false;
  bool mqtt_connected = false;
  bool time_synced = false;
  uint32_t wifi_disconnects = 0;
  uint32_t mqtt_disconnects = 0;
  uint32_t mqtt_errors = 0;
  uint32_t publishes_ok = 0;
  uint32_t publishes_failed = 0;   // enqueue refused: the one that matters
  uint64_t bytes_published = 0;
  int8_t rssi = 0;
};

// Brings up WiFi, SNTP and the MQTT client. Returns as soon as they are STARTED,
// not once they are connected -- a gateway that blocked here would be unable to
// report why it was stuck, and the serial side should keep draining regardless
// so the primary is never back-pressured by our network problems.
esp_err_t gatewayNetStart();

// The same two halves, separately, for the primary running #373's WiFi uplink.
//
// That role needs the association brought up BEFORE esp_now_init (one radio,
// one WiFi driver, and ESP-NOW has to be told to follow the associated channel
// rather than pin its own), and the services brought up after. Splitting them is
// what lets one chip do both without two components each trying to own
// esp_wifi_init.
esp_err_t gatewayWifiStart();
esp_err_t gatewayServicesStart();

// The channel the association actually landed on. 0 until associated.
//
// This is the number the whole one-chip question turns on: ESP-NOW peers must
// share a channel, and an associated station does not choose its own -- the AP
// does. Every leaf has to end up here or it is talking into a different channel.
uint8_t gatewayWifiChannel();

// True once the clock is real rather than 1970. Everything that stamps a wall
// time has to check this: publishing a frame timestamped in 1970 is worse than
// not publishing it, because it silently poisons a recording's time axis.
bool gatewayTimeValid();

// Wall clock in microseconds since the epoch, or 0 when not yet synced.
uint64_t gatewayWallClockUs();

// Publishes one payload. QoS 0 and non-blocking: this is a sensor stream, the
// freshest frame is the valuable one, and a queue that grows while the broker is
// away is a heap leak with extra steps. Returns false when the client refused it,
// which is counted rather than swallowed.
bool gatewayPublish(const char *topic, const void *payload, size_t length);

const GatewayNetStats &gatewayNetStats();

}  // namespace natkit
