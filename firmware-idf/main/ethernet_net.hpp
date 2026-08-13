#pragma once

#include <cstdint>

#include "esp_err.h"

namespace natkit {

// Wired uplink for the ESP32-S3 board (the Ethernet route).
//
// --- Why this exists, and why it is the better answer ------------------------
//
// The epic assumed a primary and a gateway had to be two chips bridged over
// serial, because one radio cannot do ESP-NOW and associated WiFi at once. #373
// measured exactly how badly: with the primary associated, ~85% of ESP-NOW frames
// were lost -- acknowledged at the MAC layer and then dropped above it by a WiFi
// task busy servicing the association -- while the leaves stayed perfectly
// healthy.
//
// Ethernet sidesteps the whole argument. It is OFF THE RADIO: the uplink is a
// wire, so ESP-NOW keeps a channel WE choose on a radio nobody else is using, and
// there is no association to inherit a channel from. One chip, no serial bridge,
// and none of #373's contention.
//
// --- The hardware -----------------------------------------------------------
//
// ⚠️ THE ESP32-S3 HAS NO INTERNAL ETHERNET MAC. The epic's long-running "which
// PHY?" question belongs to the classic ESP32 and does not apply here: this is a
// W5500, a MAC+PHY on the far side of SPI, so throughput and latency are the
// SPI bus's, not an EMAC's.
//
// The pin numbers are not guesses. They are the ones Espressif's own
// basic_thread_border_router ships for this board, which Zach confirmed working
// on this exact hardware with the stock firmware -- so they are known-good rather
// than derived from a schematic reading. See NATKIT_ETH_* in Kconfig.

struct EthernetStats {
  bool link_up = false;
  bool got_ip = false;
  uint32_t link_ups = 0;
  uint32_t link_downs = 0;
  uint8_t mac[6] = {};
  uint32_t ip = 0;
};

// Brings up the W5500 and its netif. Returns once STARTED, not once the link is
// up or an address is held -- same contract as the WiFi path, and for the same
// reason: the intake side must keep draining whether or not the uplink is ready,
// so nothing upstream is back-pressured by our network.
//
// Bring this up BEFORE esp_now_init, like the WiFi path, so the netif and event
// loop exist before anything else wants them.
esp_err_t ethernetStart();

const EthernetStats &ethernetStats();

}  // namespace natkit
