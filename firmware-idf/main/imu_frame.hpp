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
//   each sample, 62 bytes:
//     uint64 time, 13 x float, uint8 accuracies, uint8 has_data
//   the thirteen floats:
//     0-2 accel x/y/z (m/s^2), 3-5 gyro x/y/z (rad/s), 6-9 quat real/i/j/k,
//     10-12 mag x/y/z (uT)
//   accuracies: bits 7-6 mag, 5-4 accel, 3-2 gyro, 1-0 rotation (0 unreliable .. 3 high)
//   has_data:   bit 3 mag, bit 2 accel, bit 1 gyro, bit 0 rotation
//
// ⚠️ has_data MEANS "A FRESH REPORT ARRIVED FOR THIS SAMPLE", NOT "this sensor
// exists". Version 1 used it stickily -- true from a sensor's first report until
// reboot, so it was true on essentially every sample and said nothing. Version 2
// makes it per-sample, which is the only place the sensors' real rates become
// visible in the DATA rather than in a comment: the hub delivers accel at ~114 Hz
// but gyro and rotation at ~95 and the magnetometer at ~91, against a 100 Hz
// sampler, so roughly 5% of samples carry no fresh gyro or quaternion and ~9% no
// fresh magnetic field.
//
// ⚠️ AND THE FLOATS STILL HOLD THE PREVIOUS VALUE when the bit is clear, rather
// than being zeroed. Sample-and-hold with a freshness flag serves both readers: a
// consumer that checks the bit can reconstruct the sensor's true ~95 Hz series
// with gaps, and one that ignores it gets a sensible held value instead of a
// spike to zero. Zeroing would corrupt the second reader to no benefit for the
// first.
//
// This is NOT the same as the magnetometer being absent from a version 1 frame.
// That is STRUCTURAL absence -- the field does not exist -- and it reads as zeroed
// floats with the bit clear. Temporal staleness reads as a held value with the bit
// clear. A consumer that needs to tell them apart uses the frame version.
//
// ⚠️ THIS IS FRAME VERSION 2, AND VERSION 1 IS 50-BYTE SAMPLES OF TEN FLOATS.
// The leaf only ever WRITES the current version, so there is no v1 path here --
// but libnatkit-core still reads both, and that is what keeps every recording
// made before 2026-08 decodable. If this ever needs to emit v1 again, the
// per-version layout already exists there (binarySampleSize) rather than here.

constexpr uint16_t kFrameSchemaVersion = 2;
constexpr size_t kFrameHeaderSize = 24;
constexpr size_t kSampleSize = 62;
constexpr size_t kFrameFloats = 13;

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
static_assert(kFrameHeaderSize + 10 * kSampleSize == 644,
              "10 samples must encode to exactly 644 bytes -- version 2, with the "
              "magnetometer. This was 524 at version 1; both are far below the "
              "1470-byte ESP-NOW ceiling TEC-NATKIT-23 measured, so the frame is "
              "still one packet and loss still degrades to whole missing frames");
static_assert(sizeof(float) == 4, "the wire format is IEEE-754 binary32");

// One merged sample: the latest reading of each sensor at the moment it was taken.
//
// "Merged" rather than per-sensor because that is what the schema carries: the
// BNO08x delivers accel, gyro, magnetometer and rotation as SEPARATE reports at
// their own rates, and a sample is a snapshot across them.
//
// ⚠️ AND THE RATES ARE NOT EQUAL, so a snapshot is not four fresh readings.
// Measured against a 100 Hz sampler: accel arrives at ~117 Hz, gyro and quat at
// ~95, mag at ~91. Roughly 5% of samples therefore repeat the previous gyro and
// quaternion, and ~9% the previous magnetometer. has_data says a sensor has
// EVER reported, not that this sample is fresh -- there is no per-sample
// staleness flag, and adding one would change the wire format again.
// Per-sensor report counts as of the previous sample, owned by the CALLER.
//
// ⚠️ Freshness is computed by comparing counters rather than by setting and
// clearing a flag, and that is deliberate. Reports are applied on the service
// task while samples are built on the sampling task, on a dual-core part, so a
// flag cleared by the sampler could lose a report the service task set at the
// same moment -- and the failure would be silent and would over-report staleness,
// which is the direction that looks like a real finding. Comparing a counter the
// sampler never writes has no such race.
struct SampleCursor {
  uint32_t accelerometer = 0;
  uint32_t gyroscope = 0;
  uint32_t magnetometer = 0;
  uint32_t rotation = 0;
};

struct ImuSample {
  uint64_t time_ms = 0;  // milliseconds, matching the schema's own unit
  float values[kFrameFloats] = {};
  uint8_t accuracies = 0;
  uint8_t has_data = 0;
};

// Fills `sample` from the sensor's current readings. Returns false when no sensor
// has produced anything yet, so a frame is never padded with zeroed samples that
// would be indistinguishable from a real reading of zero.
bool sampleFromReadings(const SensorSet &readings, SampleCursor &cursor,
                        ImuSample &sample);

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
