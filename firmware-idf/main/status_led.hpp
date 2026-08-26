#pragma once

// The board's indicator LED, set from the frontend and remembered across reboots.
//
// ⚠️ WHY IT EXISTS: four leaves on a bench are physically indistinguishable. Their
// device ids differ by two digits in the middle, they are only readable over a
// console (which on the primary RESETS the board when opened), and every diagnosis
// that starts "which one is 0644?" starts by unplugging things. A colour answers it
// from across the room.
//
// ⚠️ WHO CHOOSES: the operator, over EXECUTION_COMMAND, not the firmware. A colour
// baked into the firmware means a table in two places -- one in code, one in
// somebody's head -- that disagree the first time a board is swapped. The frontend
// already knows every device and can show the mapping it set.
//
// ⚠️ SET ONCE, NOT PER LOOP. The Arduino firmware disabled its status pixels for a
// reason worth not rediscovering: `Adafruit_NeoPixel::show()` re-installed the
// ESP32 RMT driver on every call and leaked channels, so after ~11 calls
// rmt_driver_install asserted and the board rebooted in a ~14 s loop. Its own note
// says to "install the driver once, don't show() per poll" -- which is what a
// colour that only changes on command needs, and what a blinking status light would
// not have given us.

#include <cstdint>

#include "esp_err.h"

namespace natkit {

struct LedColour {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  // 0 turns the LED off. Kept separate from r/g/b so "off" survives a round trip
  // as an intention rather than as three zeroes that could equally be black.
  uint8_t brightness;
};

// The default: dark. A board that has never been told a colour shows nothing,
// which is honest -- inventing one would put a colour on the bench that the
// frontend does not know it assigned.
constexpr LedColour kLedOff{0, 0, 0, 0};

/**
 * Restore the colour last set over EXECUTION_COMMAND, or stay dark.
 *
 * Call once at boot, after NVS is up. ⚠️ Persistence is the point rather than a
 * nicety: a colour that vanished on every power cycle would have to be re-set from
 * the UI exactly when somebody is power-cycling boards to work out which is which.
 */
esp_err_t statusLedRestore();

/** Apply and persist a colour. Called by the `set_led` command. */
esp_err_t statusLedSet(const LedColour &colour);

/** What is currently showing, for the `get_led` command. */
LedColour statusLedCurrent();

// --- Fault signalling (TEC-NATKIT-84) ---------------------------------------
//
// ⚠️ WHY THE LED AND NOT JUST THE PANEL: the panel already says which devices are
// not delivering, but it cannot say WHICH BOARD ON THE BENCH that is. When the
// answer is "the antenna on one of these is bad" and the antennas are soldered to
// the PCBs, the only useful output is on the board itself.
//
// The leaf is the one that can tell these apart, because it is the end that knows
// both whether it hears the hub and whether the hub answers:
enum class LinkFault {
  // Delivering. Shows whatever colour the operator set.
  kNone,
  // ⚠️ Hears the hub, cannot be heard BY it. The asymmetry is the diagnosis: the
  // hub's transmits arrive and its acknowledgements do not, which is a receive
  // fault at the hub, not a transmit fault here. If EVERY leaf shows this, the
  // problem is the one board they all talk to.
  kUnheardByPrimary,
  // Cannot hear the hub at all: it is off, out of range, or on another channel.
  kNoPrimary,
};

/**
 * Show a link fault, overriding the identity colour until it clears.
 *
 * ⚠️ Only touches the LED when the state CHANGES. That is not an optimisation --
 * it is the constraint the Arduino firmware's crash loop taught: driving the pixel
 * on every pass through a loop is what exhausted the RMT channels. A fault that
 * persists is written once.
 */
void statusLedShowFault(LinkFault fault);

// --- Identify (TEC-NATKIT-99) -----------------------------------------------
//
// Flash the LED, so "which board on the bench is this stream?" is answerable by
// clicking the node that carries it. The colour set by `set_led` answers the same
// question, but only once somebody has already worked out the mapping and assigned
// colours; identify answers it for a rig nobody has labelled yet, which is every
// rig the first time.
//
// ⚠️ FLASHING IS SAFE HERE, despite the warning above. That warning is about the
// ARDUINO firmware's `Adafruit_NeoPixel::show()`, which re-installed the RMT
// driver on every call. This file drives the `led_strip` component: the RMT device
// is created ONCE into a static handle and updates are set_pixel + refresh, which
// is also how statusLedShowFault already changes colour at runtime.
//
// ⚠️ NON-BLOCKING, and that is a requirement rather than a preference. The command
// that triggers this executes on the LEAF'S MAIN LOOP -- the loop that also
// services the link and the console -- and commands.hpp is explicit that a burst
// must not stall it. Sleeping ~1 s here would cost samples the same way sharing a
// loop with imu.service() once cost 15% of sample slots. So this ARMS a sequence
// and statusLedService() advances it.
void statusLedIdentify(uint8_t flashes);

/**
 * Advance an armed identify sequence. Call from the leaf's main loop every pass;
 * it returns immediately when nothing is running.
 */
void statusLedService();

/** True while an identify sequence is in progress. */
bool statusLedIdentifying();

}  // namespace natkit
