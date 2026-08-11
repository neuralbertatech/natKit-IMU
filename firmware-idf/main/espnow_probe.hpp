#pragma once

namespace natkit {

// Bench instrument for TEC-NATKIT-23, not part of the node architecture: reports
// the chip's ESP-NOW version and real payload limit, sweeps payload sizes, and
// measures send rate with the actual 524-byte IMU frame. Built instead of a node
// role when CONFIG_NATKIT_ESPNOW_PROBE is set, and doubles as the sender/receiver
// pair for the multi-node loss run. Never returns.
[[noreturn]] void runEspNowProbe();

}  // namespace natkit
