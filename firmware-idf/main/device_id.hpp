#pragma once

#include <cstdint>

namespace natkit {

// Pack the six bytes of a MAC big-endian into a uint64.
//
// This MUST stay bit-identical to the Arduino firmware's UNIQUE_ID
// (../../embeded/src/main.cpp): the default efuse MAC, most significant byte
// first, rendered in DECIMAL -- e.g. 0c:8b:95:96:b9:f4 -> 13793649670644. That
// number is baked into the Kafka/MQTT topic names the bridge and backend
// already use (Data-13793649670644-Json-...,
// Command-13793649670644-Json-NatExecutionCommandV1, and so on), so a fork
// image that derived its id any other way would appear as a different device.
//
// constexpr so device_id.cpp can static_assert it against the real board.
constexpr uint64_t packMac(const uint8_t mac[6]) {
  return (static_cast<uint64_t>(mac[0]) << 8 * 5) +
         (static_cast<uint64_t>(mac[1]) << 8 * 4) +
         (static_cast<uint64_t>(mac[2]) << 8 * 3) +
         (static_cast<uint64_t>(mac[3]) << 8 * 2) +
         (static_cast<uint64_t>(mac[4]) << 8 * 1) +
         static_cast<uint64_t>(mac[5]);
}

// This device's id, cached after the first call (the efuse does not change at
// runtime).
uint64_t deviceId();

// The same MAC as the six raw bytes, for the ESP-NOW peer registry -- a primary
// pairs by MAC, not by the decimal id.
const uint8_t *deviceMac();

}  // namespace natkit
