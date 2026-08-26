#include "device_controls.hpp"

#include <cstdio>
#include <cstring>

#include "board_config.hpp"

namespace natkit {
namespace {

// One entry per control, in the order they should appear in the UI.
//
// ⚠️ `field` is the ARGS KEY the device parses, not a label. `set_reports`
// expects {"accel":false}; `set_tx_power` expects {"quarter_dbm":34}. Getting
// this wrong produces a control that reports success and changes nothing.
struct ControlSpec {
  uint32_t bit;
  const char *id;
  const char *kind;
  const char *group;
  const char *read_command;
  const char *write_command;
  const char *field;
  const char *value_type;
  bool ranged;
  int min_value;
  int max_value;
};

constexpr ControlSpec kControls[] = {
    {kControlIdentify, "identify", "button", "", "", "identify", "", "", false, 0, 0},

    {kControlReports, "reports.accel", "toggle", "reports", "get_reports",
     "set_reports", "accel", "bool", false, 0, 0},
    {kControlReports, "reports.gyro", "toggle", "reports", "get_reports",
     "set_reports", "gyro", "bool", false, 0, 0},
    {kControlReports, "reports.mag", "toggle", "reports", "get_reports",
     "set_reports", "mag", "bool", false, 0, 0},
    {kControlReports, "reports.rotation", "toggle", "reports", "get_reports",
     "set_reports", "rotation", "bool", false, 0, 0},

    // ⚠️ Quarter-dBm is the radio's own unit. The range is advertised because
    // this is the end that ENFORCES it -- espNowLinkPinTxPower clamps, and a UI
    // slider built from a different number would let an operator ask for
    // something the device silently refuses.
    {kControlTxPower, "tx_power", "input", "", "get_tx_power", "set_tx_power",
     "quarter_dbm", "int16", true, 8, 80},

    // ⚠️ NO `led` CONTROL, deliberately. set_led's real argument is a COLOUR
    // (r/g/b plus brightness); advertising a lone "brightness" input would put a
    // control on screen that cannot express what the command actually takes, and
    // a control that misrepresents its own command is worse than none. It comes
    // back when there is a colour kind to advertise it as.
};

}  // namespace

uint32_t deviceControlsMask(const bool has_imu) {
  // identify and the LED need a pixel to drive; every target this firmware
  // builds for has one declared in board_config.hpp.
  uint32_t mask = kControlIdentify | kControlTxPower;
  // ⚠️ Only if the sensor actually came up. A board whose BNO08x failed to
  // initialise still runs, and offering report toggles on it would be a control
  // that cannot work -- the exact thing this feature exists to stop.
  if (has_imu) {
    mask |= kControlReports;
  }
  return mask;
}

size_t deviceControlsRenderJson(char *out, const size_t out_size,
                                const uint64_t device_id, const uint32_t mask) {
  if (out == nullptr || out_size == 0) {
    return 0;
  }
  int written = std::snprintf(
      out, out_size,
      "{\"schema_version\":\"nat.controls.v1\",\"device_id\":%llu,\"controls\":[",
      static_cast<unsigned long long>(device_id));
  if (written < 0 || static_cast<size_t>(written) >= out_size) {
    return 0;
  }
  size_t used = static_cast<size_t>(written);
  bool first = true;

  for (const auto &spec : kControls) {
    if ((mask & spec.bit) == 0) {
      continue;
    }
    // Built field by field so the optional keys are genuinely omitted rather
    // than emitted empty -- the decoder treats an empty read command as "this is
    // a button", and an empty string would make every toggle look like one.
    char entry[256];
    int n = std::snprintf(entry, sizeof(entry),
                          "%s{\"id\":\"%s\",\"kind\":\"%s\",\"write\":\"%s\"",
                          first ? "" : ",", spec.id, spec.kind,
                          spec.write_command);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(entry)) return 0;
    size_t e = static_cast<size_t>(n);

    const auto append = [&](const char *fmt, const char *value) -> bool {
      if (value == nullptr || value[0] == '\0') return true;
      const int m = std::snprintf(entry + e, sizeof(entry) - e, fmt, value);
      if (m < 0 || static_cast<size_t>(m) >= sizeof(entry) - e) return false;
      e += static_cast<size_t>(m);
      return true;
    };
    if (!append(",\"read\":\"%s\"", spec.read_command)) return 0;
    if (!append(",\"group\":\"%s\"", spec.group)) return 0;
    if (!append(",\"field\":\"%s\"", spec.field)) return 0;
    if (!append(",\"type\":\"%s\"", spec.value_type)) return 0;

    if (spec.ranged) {
      const int m = std::snprintf(entry + e, sizeof(entry) - e,
                                  ",\"min\":%d,\"max\":%d", spec.min_value,
                                  spec.max_value);
      if (m < 0 || static_cast<size_t>(m) >= sizeof(entry) - e) return 0;
      e += static_cast<size_t>(m);
    }
    if (e + 2 >= sizeof(entry)) return 0;
    entry[e++] = '}';
    entry[e] = '\0';

    // ⚠️ REFUSE RATHER THAN TRUNCATE. Half a JSON object on a Configuration
    // topic decodes as nothing, which on screen is indistinguishable from a
    // device that never advertised -- the failure this whole design removes.
    if (used + e + 2 >= out_size) {
      return 0;
    }
    std::memcpy(out + used, entry, e);
    used += e;
    first = false;
  }

  if (used + 3 >= out_size) {
    return 0;
  }
  out[used++] = ']';
  out[used++] = '}';
  out[used] = '\0';
  return used;
}

}  // namespace natkit
