#pragma once

// What this board can be asked to do, as it advertises it (TEC-NATKIT-10).
//
// --- Why a mask on the radio and JSON at the gateway ------------------------
//
// ⚠️ THE LEAF SENDS TWELVE BYTES, NOT A DOCUMENT. espnow_link.hpp is explicit
// that "the leaf never sees JSON" -- a leaf is a sensor and a radio, and the
// primary is where documents get built, because that is where there is a console
// and a broker to complain to. A ControlsFrame is a device id and a bitmask; the
// primary renders it into the advertisement the server reads. The alternative,
// shipping ~400 bytes of prose per leaf per advertisement, buys nothing: the
// words are not on the device anyway, they are in libnatkit-core's registry.
//
// --- What lives here and what does not --------------------------------------
//
// ⚠️ THIS TABLE IS THE CONTRACT AND IS THE ONLY COPY OF IT. Which controls
// exist, their kind, the commands that drive them, the args key, the type and
// any range. It is the device's half, and the server never invents it.
//
// The WORDS are not here. label/description/unit live in libnatkit-core's
// DeviceControlDescriptorRegistry, resolved by control id on the server, so a
// third-party library can describe -- or translate -- a control without
// reflashing anything. Putting them in both places is what an earlier draft did
// and is exactly how the two drift into disagreeing.
//
// ⚠️ Every command named below must exist in commands.cpp's dispatch. A control
// advertising a command the firmware does not implement produces a button that
// answers "I do not know that one", which is worse than no button.

#include <cstddef>
#include <cstdint>

namespace natkit {

// One bit per capability a board may or may not have. A leaf with no IMU does
// not advertise the reports; a target with no LED does not advertise identify.
enum ControlBits : uint32_t {
  kControlIdentify = 1u << 0,
  // The four BNO08x reports, advertised as one group: they are read together by
  // get_reports and written one field at a time by set_reports.
  kControlReports = 1u << 1,
  kControlTxPower = 1u << 2,
  kControlLed = 1u << 3,
};

// What a leaf sends its primary. ⚠️ Twelve bytes -- keep it that way; this rides
// the same radio as the data.
struct ControlsFrame {
  uint64_t device_id;
  uint32_t mask;
};

static_assert(sizeof(ControlsFrame) == 16,
              "ControlsFrame is published on the wire; it is padded to 16 by the "
              "uint64 and that is fine, but a field added here changes the frame");

// What this build supports, given what the board actually has.
uint32_t deviceControlsMask(bool has_imu);

// Render the advertisement for `device_id`. Returns the number of bytes written,
// or 0 if it did not fit -- ⚠️ never a truncated document, because half a JSON
// object on a Configuration topic decodes as nothing and looks like silence.
size_t deviceControlsRenderJson(char *out, size_t out_size, uint64_t device_id,
                                uint32_t mask);

}  // namespace natkit
