#pragma once

#include "sdkconfig.h"

namespace natkit {

// The three roles of the primary/secondary architecture (EPIC TEC-NATKIT-20).
enum class NodeRole { kLeaf, kPrimary, kGateway };

#if defined(CONFIG_NATKIT_ROLE_LEAF)
constexpr NodeRole kRole = NodeRole::kLeaf;
#elif defined(CONFIG_NATKIT_ROLE_PRIMARY)
constexpr NodeRole kRole = NodeRole::kPrimary;
#elif defined(CONFIG_NATKIT_ROLE_GATEWAY)
constexpr NodeRole kRole = NodeRole::kGateway;
#else
// Not a "can't happen": if sdkconfig.h is not reached (a stale include path, a
// component that forgot to depend on the config) every #if above is false and
// the image would silently build as whatever the last branch happened to be.
#error "No node role selected -- expected CONFIG_NATKIT_ROLE_{LEAF,PRIMARY,GATEWAY} from main/Kconfig.projbuild"
#endif

const char *roleName(NodeRole role);

// Role entry points. Each is the seam a later slice fills in, and each is
// expected NOT to return:
//   runLeaf    -- TEC-NATKIT-24 (sensor + ESP-NOW), sensor from TEC-NATKIT-22
//   runPrimary -- TEC-NATKIT-25 (hub, registry, reassembly, serial uplink)
//   runGateway -- TEC-NATKIT-26 (WiFi/Ethernet, esp-mqtt, SNTP)
void runLeaf();
void runPrimary();
void runGateway();

// The scaffold's idle loop: writes a status line every
// CONFIG_NATKIT_STATUS_LOG_INTERVAL_S seconds and never returns. Every role
// stub ends in this, so an unimplemented role is a board that is visibly alive
// and reporting its heap rather than a board that looks bricked.
[[noreturn]] void idleStatusLoop(const char *what);

}  // namespace natkit
