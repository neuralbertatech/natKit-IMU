#pragma once

#include <cstdint>

#include "esp_err.h"

namespace natkit {

// Server -> device commands, on the primary.
//
// --- Why this exists here and not on the leaf --------------------------------
//
// The old firmware's command channel worked because every node was a full MQTT
// client. In this architecture a leaf has NO IP AT ALL, so a command for a leaf
// arrives here, at the only node with a network, and is relayed over ESP-NOW.
//
// --- What this does, and deliberately does not -------------------------------
//
// It parses the JSON. The leaf receives a fixed-size POD (CommandFrame) and never
// links a JSON parser, which is the whole point of the fork: a leaf is a sensor
// and a radio. It also means a malformed command is rejected where there is a
// console and a broker to complain to, rather than on a node nobody can watch.
//
// ⚠️ IT DOES NOT DECIDE WHAT A COMMAND MEANS. Every command is relayed to the
// named device and answered by it. The relay knows about addressing and framing
// and nothing else, so adding a command touches the leaf and the frontend, never
// this file.
//
// --- The subscription is per node, not a wildcard ----------------------------
//
// The backend addresses a device on Command-<device_id>-Json-..., and a wildcard
// subscription would also deliver commands for devices on other rigs sharing the
// broker. Subscribing per registered node means an unknown device's command is
// never even received, which is a better failure than receiving it and having to
// decide whether to act.

// Subscribes for every node currently in the registry and installs the MQTT
// handler. Safe to call repeatedly -- new nodes appear as they announce, and
// already-subscribed topics are skipped.
esp_err_t commandRelayStart();

// Call periodically so nodes that announce after startup get subscribed too. A
// node that joins late would otherwise be uncommandable until the next reboot,
// silently.
void commandRelayRefreshSubscriptions();

// A device acknowledged a command. Stops the retransmissions for it.
void commandRelayNoteAck(uint64_t device_id, const char *command_id);

// Retransmits unacknowledged commands and fails the ones that have run out of
// attempts. Driven from its own task; exposed for tests.
void commandRelayService();

struct CommandRelayStats {
  uint32_t received = 0;      // MQTT messages on a command topic
  uint32_t relayed = 0;       // successfully unicast to a node
  uint32_t malformed = 0;     // not JSON, or missing a required field
  uint32_t unknown_device = 0;// well-formed, but for a device not in the registry
  uint32_t send_failed = 0;   // the radio refused it
  uint32_t subscriptions = 0; // command topics currently subscribed
  // ⚠️ DELIVERED IS NOT RELAYED. `relayed` means the radio accepted the packet;
  // `delivered` means a device said it has it. On a rig where nodes are not
  // expected to be always on, the gap between those two is the whole point.
  uint32_t delivered = 0;     // acknowledged by the device
  uint32_t retransmits = 0;
  uint32_t undelivered = 0;   // gave up: never acknowledged
};

const CommandRelayStats &commandRelayStats();

}  // namespace natkit
