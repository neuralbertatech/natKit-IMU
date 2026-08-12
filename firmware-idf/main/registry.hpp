#pragma once

#include <cstdint>

#include "esp_err.h"

namespace natkit {

// The primary's node registry (#348 / TEC-NATKIT-25).
//
// --- What this is actually for, since the obvious job is already done --------
//
// The ticket asks for "which MACs we accept and what stream id each maps to".
// The mapping half is already solved and needs no table: the stream id is
// DERIVED from the MAC by packMac(), pinned by a static_assert against the real
// board, and it is the number already inside every existing topic name. So a
// node's id is known from its first packet, including a data frame that arrives
// before any announce.
//
// What is left is the part that actually needed building:
//
//   1. PERSISTENCE. A roster that evaporates on reboot is not a registry. Kept
//      in NVS so a primary that resets keeps knowing which nodes are its own.
//   2. An ACCEPT/REJECT decision. "A neighbouring rig on the same channel is a
//      real scenario" -- and an unknown node must be logged and dropped rather
//      than silently forwarded into someone's recording.
//   3. The counters that make a rejection visible, because a registry that
//      silently discards is indistinguishable from a radio that is not working.
//
// --- Open until sealed ------------------------------------------------------
//
// A fresh rig accepts and remembers any node that announces itself, which is
// what makes it self-configuring rather than a per-site build. Once the rig is
// the set of nodes you want, SEAL it: the roster freezes and anything not on it
// is counted and dropped.
//
// Open-by-default is the deliberate choice. A registry that must be populated by
// hand before anything works would be fought around -- someone would disable it
// -- and a rig that quietly accepts a stranger is a worse failure than one that
// quietly rejects a friend, only once you have told it who its friends are.

constexpr size_t kRegistryMaxNodes = 8;

struct RegistryEntry {
  uint8_t mac[6];
  uint64_t device_id;
  bool in_use;
};

esp_err_t registryLoad();

// True if this MAC may be forwarded. When the registry is open, an unknown MAC is
// admitted and persisted as a side effect -- that is the self-configuring path.
// When sealed, an unknown MAC is counted and refused.
bool registryAccepts(const uint8_t *mac);

bool registrySealed();

// Sealing persists both the flag and the current roster, so it survives a reset.
esp_err_t registrySeal(bool sealed);

// Forgets every node. Only meaningful while open; a sealed registry that could be
// cleared without unsealing would make the seal decorative.
esp_err_t registryClear();

uint32_t registryCount();
uint32_t registryRejections();
const RegistryEntry *registryEntries();

}  // namespace natkit
