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

}  // namespace natkit
