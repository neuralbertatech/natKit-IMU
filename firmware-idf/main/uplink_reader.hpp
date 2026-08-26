#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "uplink.hpp"

namespace natkit {

// The gateway's side of the primary's framed serial link (#349 / TEC-NATKIT-26).
//
// The counterpart of uplink.cpp, and it exists because the reading half is where
// framing is actually tested: anyone can write a frame, and only a reader that
// joined the stream half way through discovers whether the format can be
// recovered from.
//
// --- Resynchronisation is the whole job -------------------------------------
//
// This reader assumes it may start mid-frame, that either end may reset without
// warning, and that another writer may be on the same wire (which is literally
// the case in the bring-up mode, where the primary's frames are interleaved with
// its console). So:
//
//   * scan for the magic, and trust NOTHING until the CRC validates;
//   * on a bad frame advance by exactly ONE byte, never by the claimed length --
//     a corrupt length field is precisely the case where trusting it walks you
//     past the next good frame;
//   * count every discarded byte, so "it resynchronised" is a number rather than
//     an impression.

struct UplinkReaderStats {
  uint32_t frames_ok = 0;
  uint32_t frames_data = 0;
  uint32_t frames_node_status = 0;
  uint32_t frames_primary_status = 0;
  uint32_t frames_command_log = 0;
  // Control advertisements seen on the line (TEC-NATKIT-10). Counted separately
  // so "the leaf never advertised" and "the gateway never published it" are
  // different numbers rather than one silence.
  uint32_t frames_controls = 0;
  // Downward commands (TEC-NATKIT-92). Non-zero only on the PRIMARY, which is
  // the only end that reads this type -- a gateway seeing these has its wires
  // crossed, or something else is writing on the line.
  uint32_t frames_command = 0;
  uint32_t crc_failures = 0;
  uint32_t version_mismatches = 0;
  uint64_t bytes_read = 0;
  uint64_t bytes_skipped = 0;   // resynchronising past garbage or console text
  uint32_t uplink_seq_gaps = 0; // the PRIMARY dropped it, or the wire ate it
  uint32_t overruns = 0;        // the driver's buffer filled before we drained it
  bool seq_seen = false;
  uint32_t last_seq = 0;
};

// Called for each validated frame. Runs on the reader task, so it should not
// block for long -- publishing is fine, waiting on a network is not.
using UplinkFrameHandler = void (*)(UplinkType type, uint64_t stream_id,
                                    const uint8_t *payload, size_t length);

esp_err_t uplinkReaderStart(UplinkFrameHandler handler);

const UplinkReaderStats &uplinkReaderStats();

}  // namespace natkit
