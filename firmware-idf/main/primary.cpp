#include "esp_log.h"
#include "node_role.hpp"

// Primary: the ESP-NOW hub, the timing master, and the serial uplink.
//
// Scaffold only. What lands here, and in which slice:
//   TEC-NATKIT-23  frame reassembly, paired with whatever the leaf fragments.
//   TEC-NATKIT-25  the node registry (which MACs this primary will listen to
//                  and what stream id each maps to), the serial mux for N
//                  leaves down one link, and backpressure.
//   #340           the 1-second ESP-NOW timing broadcast and the time-shift
//                  proxy. That ticket is this epic's timing slice; it is
//                  related to the epic rather than a child of it.
//
// Budget the serial link before designing the mux: natVR already hit a ceiling
// at ~960-byte JSON frames at 20 fps (~19 KB/s) against a 115200 console UART
// (~11.5 KB/s), and the pipeline stalled. N leaves through one hub through one
// link is the same arithmetic with a bigger N.

namespace natkit {

void runPrimary() {
  constexpr char kTag[] = "natkit-primary";

  ESP_LOGW(kTag,
           "primary role is a scaffold: no ESP-NOW hub, no registry and no "
           "serial uplink yet (TEC-NATKIT-23 / -25, timing in #340)");

  idleStatusLoop("primary");
}

}  // namespace natkit
