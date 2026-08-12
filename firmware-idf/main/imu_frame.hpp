#pragma once

#include <cstddef>
#include <cstdint>

#include "bno08x.hpp"

namespace natkit {

// The canonical natKit IMU bulk frame, built ON THE NODE.
//
// That placement is TEC-NATKIT-23's recorded decision, not an implementation
// detail: the node is the only thing that knows its own sensor, one frame is one
// ESP-NOW packet (524 bytes against a measured 1470-byte ceiling), and loss
// therefore degrades to whole missing frames that seqNo already makes detectable.
// The primary forwards the frame it receives rather than rebuilding it.
//
// ⚠️ THIS ENCODER IS A SECOND IMPLEMENTATION OF A WIRE FORMAT THAT LIVES
// ELSEWHERE. The first is libnatkit-core's
// NatImuBulkDataSchema::encodeToBytes(Binary), which the current firmware uses
// and which the bridge, backend and every stored Parquet file already agree with.
// The fork deliberately does not pull libnatkit-core in -- a leaf that links a
// C++ schema library is not the lean node this epic is testing -- so the layout is
// reproduced here instead, which means the two CAN drift.
//
// What guards against that, in order of strength:
//   1. the static_asserts below, which pin every offset and size;
//   2. the layout being derived from the encoder rather than from a document;
//   3. verification on the wire -- a leaf frame decoded with the same script used
//      on the current firmware's frames (natkit-verification/.../decode_frame.py).
// If libnatkit-core's Binary encoding ever changes, this file must change with it
// and the frame version must move.
//
// Layout, little-endian throughout:
//   frame header, 24 bytes:
//     uint16 schemaVersion, uint16 sampleCount, uint32 sampleRateHz,
//     uint64 seqNo, uint64 deviceTsUs
//   each sample, 50 bytes:
//     uint64 time, 10 x float, uint8 accuracies, uint8 has_data
//   the ten floats:
//     0-2 accel x/y/z (m/s^2), 3-5 gyro x/y/z (rad/s), 6-9 quat real/i/j/k
//   accuracies: bits 5-4 accel, 3-2 gyro, 1-0 rotation (0 unreliable .. 3 high)
//   has_data:   bit 2 accel, bit 1 gyro, bit 0 rotation

constexpr uint16_t kFrameSchemaVersion = 1;
constexpr size_t kFrameHeaderSize = 24;
constexpr size_t kSampleSize = 50;
constexpr size_t kFrameFloats = 10;

// Compatibility constants, matching ../../embeded so a recording from the fork is
// comparable with one from the current firmware:
//   IMU_SAMPLES_PER_FRAME 10   (kafkaTopic.hpp)
//   IMU_SAMPLE_RATE_HZ    100  (declared in the header; informational, because
//                               every sample carries its own timestamp)
constexpr size_t kMaxSamplesPerFrame = 16;

static_assert(kFrameHeaderSize == 2 + 2 + 4 + 8 + 8,
              "frame header is uint16+uint16+uint32+uint64+uint64");
static_assert(kSampleSize == 8 + kFrameFloats * 4 + 1 + 1,
              "sample is uint64 time + 10 floats + accuracies + has_data");
static_assert(kFrameHeaderSize + 10 * kSampleSize == 524,
              "10 samples must encode to exactly the 524 bytes the current "
              "firmware puts on the wire, and that TEC-NATKIT-23 measured "
              "against the ESP-NOW ceiling");
static_assert(sizeof(float) == 4, "the wire format is IEEE-754 binary32");

// One merged sample: the latest reading of each sensor at the moment it was taken.
//
// "Merged" rather than per-sensor because that is what the schema carries and what
// the current firmware sends: the BNO08x delivers accel, gyro, magnetometer and
// rotation as SEPARATE reports at their own rates, and a sample is a snapshot
// across them. The magnetometer is deliberately absent -- the schema has no field
// for it, so it is observable in the console and in calibration state but is not
// on the wire, exactly as today.
struct ImuSample {
  uint64_t time_ms = 0;  // milliseconds, matching the schema's own unit
  float values[kFrameFloats] = {};
  uint8_t accuracies = 0;
  uint8_t has_data = 0;
};

// Fills `sample` from the sensor's current readings. Returns false when no sensor
// has produced anything yet, so a frame is never padded with zeroed samples that
// would be indistinguishable from a real reading of zero.
bool sampleFromReadings(const SensorSet &readings, ImuSample &sample);

// Encodes `count` samples into `out`, which must hold at least
// kFrameHeaderSize + count * kSampleSize bytes. Returns the number of bytes
// written, or 0 if the buffer is too small or count is 0.
//
// deviceTsUs is the frame's own timestamp. NOTE that on a leaf it is MONOTONIC
// SINCE BOOT, not wall clock: a leaf has no NTP by design (that is the point of
// the architecture), so whatever forwards these frames has to translate them
// against the primary's clock. A gateway that publishes them unmodified would
// advertise 1970-era timestamps. Closing that properly is the timing slice (#340).
size_t encodeFrame(const ImuSample *samples, size_t count, uint64_t seq_no,
                   uint64_t device_ts_us, uint32_t sample_rate_hz, uint8_t *out,
                   size_t out_size);

}  // namespace natkit
