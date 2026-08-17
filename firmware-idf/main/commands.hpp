#pragma once

#include <cstddef>
#include <cstdint>

#include "espnow_link.hpp"

namespace natkit {

// Command execution on the leaf.
//
// --- Why a queue and not a call ---------------------------------------------
//
// A command arrives on the ESP-NOW receive callback, which runs on the WiFi task.
// Executing it there would do sensor I/O from inside the radio's callback, and
// anything slow there costs received packets -- the same mechanism that made
// sampling and imu.service() sharing one loop cost 15% of sample slots. So the
// callback copies the frame into a queue and returns, and the leaf's own loop
// drains it.
//
// --- The answer travels the same way ----------------------------------------
//
// Replies go back as kCommandLog packets, which the primary turns into the JSON
// the backend is already waiting for. A command may produce several records; only
// the last carries final = 1, and that is what the backend's correlation ends on.

// Copies a command into the pending queue. Called from the receive callback, so
// it never blocks and never allocates. Returns false if the queue is full, which
// is counted -- a dropped command is invisible otherwise.
bool commandsEnqueue(const CommandFrame &frame);

// True if this command id has already been accepted, and records it if not.
//
// ⚠️ Called from the RECEIVE CALLBACK, before enqueueing, because the primary
// retransmits until acknowledged: without this, a lost acknowledgement would run
// the command again rather than merely re-answering it.
bool commandsAlreadySeen(const char *command_id);

// Executes at most one pending command. Call from the leaf's main loop.
//
// ⚠️ ONE PER CALL, deliberately. A burst of commands should not stall the loop
// that also services the sensor and the link; they are answered in order, one per
// pass, and the queue is short enough that the delay is bounded.
void commandsService();

struct CommandStats {
  uint32_t received = 0;
  uint32_t executed = 0;
  uint32_t unknown = 0;      // no handler for that name
  uint32_t dropped_full = 0; // arrived while the queue was full
  uint32_t reply_failed = 0; // the answer could not be sent
};

// The leaf's IMU, so get_reports/set_reports can reach it. Set once at startup.
//
// ⚠️ A POINTER RATHER THAN A CALL INTO leaf.cpp, because commands execute on the
// leaf's own loop and that is the only task allowed to touch the hub. Handing the
// object over makes the ownership explicit rather than implied by which file the
// code happens to sit in.
void commandsSetImu(class Bno08x *imu);

const CommandStats &commandStats();

}  // namespace natkit
