#include "esp_log.h"
#include "node_role.hpp"

// Gateway: serial in from the primary, WiFi or Ethernet + MQTT out.
//
// Scaffold only. What lands here (TEC-NATKIT-26): esp_wifi or esp_eth,
// esp-mqtt, and esp_netif_sntp for the clock.
//
// The contract it must keep is the one the bridge and backend already speak --
// natKit/receiving/<Topic>-<id>-Json-<Schema> -- so that adopting this fork
// changes nothing server-side. Two notes worth not rediscovering:
//   * On a device's FIRST command the Command topic does not exist yet, and the
//     bridge only forwards topics it has a messenger for (1 s discovery poll).
//   * The vendored components/esp_eth copies were deleted from natKit-IMU in
//     891cc38, so the Ethernet option uses the IDF's own component and needs a
//     PHY chosen first (still an open question on the epic).
//
// Whether the gateway is a separate board or a second chip beside the primary
// is also still open, and it changes the serial link, the enclosure (#320) and
// the power budget. This file assumes only "there is a serial stream coming in".

namespace natkit {

void runGateway() {
  constexpr char kTag[] = "natkit-gateway";

  ESP_LOGW(kTag,
           "gateway role is a scaffold: no serial intake, no WiFi/Ethernet and "
           "no MQTT yet (TEC-NATKIT-26)");

  idleStatusLoop("gateway");
}

}  // namespace natkit
