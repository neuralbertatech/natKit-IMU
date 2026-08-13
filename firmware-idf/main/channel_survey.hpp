#pragma once

#include <cstdint>

namespace natkit {

// Pick the ESP-NOW channel by measuring the band, at boot (Zach's suggestion).
//
// --- Why this is free -------------------------------------------------------
//
// The primary cannot publish anything for the first ~33 seconds after a reset
// anyway: NTP has not synced, so every data frame is refused rather than
// published with a 1970 timestamp. That dead time is exactly long enough to
// listen to all thirteen channels, so the survey costs nothing that was not
// already being lost.
//
// --- Why it is worth doing at all -------------------------------------------
//
// Channel choice has been the single largest factor in this rig's throughput,
// and it is a property of the SITE rather than of the firmware. Measured on one
// bench, the hub's view of a leaf a few centimetres away:
//
//   channel 3   -22 dBm, the full 10 frames/s
//   channel 6   -21 to -57 dBm, varying
//   channel 9   -61 dBm, nothing delivered
//   channel 11  -83 dBm, nothing delivered
//   channel 1   -82 dBm, jammed by the Thread Border Router board itself
//
// A hard-coded number that is right in one room is wrong in the next, and the
// failure is silent and slow to diagnose -- it presents as a bad radio, not as a
// bad channel. Measuring beats guessing, and the rig already has the receiver.
//
// --- How -------------------------------------------------------------------
//
// Promiscuous mode, dwelling on each channel in turn and counting the frames
// that arrive and how strong they are. Busy channels are loud; quiet ones are
// not. The score is energy rather than a raw count, because one strong
// interferer hurts more than several weak ones -- summing 10^(rssi/10) is the
// physical quantity that matters, and a count alone would rank a channel with
// twenty distant beacons worse than one with a neighbouring access point.
//
// ⚠️ This does NOT survey the interference that mattered most on this bench.
// Channel 1's problem is the Thread Border Router board's own clocks, which are
// not 802.11 frames and so are invisible to a promiscuous receiver. The survey
// finds crowded channels, not noisy ones -- so it complements the reciprocity
// check (compare the two directions' RSSI) rather than replacing it.

struct ChannelSurveyResult {
  bool ran = false;
  uint8_t chosen = 0;
  uint16_t packets[14] = {};   // indexed by channel; [0] unused
  int8_t strongest[14] = {};   // best RSSI seen on each channel
  uint32_t score[14] = {};     // relative energy; lower is quieter
};

// Sweeps the band and returns the quietest channel. WiFi must already be
// started; ESP-NOW must NOT be initialised yet, since this leaves the radio in
// promiscuous mode while it runs.
//
// Returns the fallback unchanged if the survey cannot run, so a failure here
// degrades to the configured channel rather than to no radio at all.
uint8_t channelSurveyRun(uint8_t fallback, uint32_t dwell_ms);

const ChannelSurveyResult &channelSurveyResult();

}  // namespace natkit
