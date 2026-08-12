#include "imu_frame.hpp"

#include <cstring>

namespace natkit {
namespace {

// has_data bit positions, matching ../../embeded/include/ImuReader.hpp exactly
// (accelerometer_bit 0b100, gyroscopt_bit 0b010, rotation_bit 0b001 -- the typo is
// in the original and is quoted here only so the correspondence is greppable).
constexpr uint8_t kHasAccel = 0b100;
constexpr uint8_t kHasGyro = 0b010;
constexpr uint8_t kHasRotation = 0b001;

// Accuracies are two bits per sensor: accel << 4, gyro << 2, rotation << 0.
constexpr uint8_t kAccuracyMask = 0x03;

// Little-endian writes, done byte by byte rather than by memcpy of a struct.
//
// Not fussiness: a packed struct would still leave the ESP32's native endianness
// and the compiler's padding as unstated assumptions in a format the bridge and
// every stored Parquet file already agree with. Both targets are little-endian
// today, so a memcpy would work and would silently stop working on a big-endian
// port. This is explicit instead.
template <typename T>
size_t writeLe(uint8_t *out, T value) {
  for (size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
  return sizeof(T);
}

size_t writeFloat(uint8_t *out, float value) {
  // memcpy rather than a cast: this must preserve the BIT PATTERN, and type
  // punning through a pointer cast is undefined behaviour that -O2 is entitled to
  // act on. libnatkit-core's encoder uses memcpy here for the same reason, and
  // says so.
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return writeLe<uint32_t>(out, bits);
}

}  // namespace

bool sampleFromReadings(const SensorSet &readings, ImuSample &sample) {
  sample = ImuSample{};

  if (!readings.accelerometer.has_data && !readings.gyroscope.has_data &&
      !readings.rotation.has_data) {
    return false;
  }

  // The sample's timestamp is the newest contributing report's, not "now".
  //
  // This mirrors the current firmware, which takes the max of the contributing
  // reports' timestamps rather than reading the clock at pack time. It matters
  // because the reports arrive asynchronously: stamping at pack time would fold
  // the pack loop's own jitter into the data's time axis.
  uint64_t newest_us = 0;

  if (readings.accelerometer.has_data) {
    sample.values[0] = readings.accelerometer.x;
    sample.values[1] = readings.accelerometer.y;
    sample.values[2] = readings.accelerometer.z;
    sample.accuracies |=
        static_cast<uint8_t>((readings.accelerometer.accuracy & kAccuracyMask) << 4);
    sample.has_data |= kHasAccel;
    if (readings.accelerometer.last_us > newest_us) {
      newest_us = readings.accelerometer.last_us;
    }
  }

  if (readings.gyroscope.has_data) {
    sample.values[3] = readings.gyroscope.x;
    sample.values[4] = readings.gyroscope.y;
    sample.values[5] = readings.gyroscope.z;
    sample.accuracies |=
        static_cast<uint8_t>((readings.gyroscope.accuracy & kAccuracyMask) << 2);
    sample.has_data |= kHasGyro;
    if (readings.gyroscope.last_us > newest_us) {
      newest_us = readings.gyroscope.last_us;
    }
  }

  if (readings.rotation.has_data) {
    // Quaternion order is real, i, j, k -- which is NOT the order the sensor
    // struct lists them in, and not x/y/z/w. SensorReading reuses x/y/z/w as
    // real/i/j/k for the rotation vector (see bno08x.hpp), so this mapping is
    // deliberate rather than a transcription slip.
    sample.values[6] = readings.rotation.x;  // real
    sample.values[7] = readings.rotation.y;  // i
    sample.values[8] = readings.rotation.z;  // j
    sample.values[9] = readings.rotation.w;  // k
    sample.accuracies |=
        static_cast<uint8_t>(readings.rotation.accuracy & kAccuracyMask);
    sample.has_data |= kHasRotation;
    if (readings.rotation.last_us > newest_us) {
      newest_us = readings.rotation.last_us;
    }
  }

  sample.time_ms = newest_us / 1000;
  return true;
}

size_t encodeFrame(const ImuSample *samples, size_t count, uint64_t seq_no,
                   uint64_t device_ts_us, uint32_t sample_rate_hz, uint8_t *out,
                   size_t out_size) {
  if (samples == nullptr || out == nullptr || count == 0) {
    return 0;
  }
  const size_t needed = kFrameHeaderSize + count * kSampleSize;
  if (out_size < needed) {
    return 0;
  }

  uint8_t *p = out;
  p += writeLe<uint16_t>(p, kFrameSchemaVersion);
  p += writeLe<uint16_t>(p, static_cast<uint16_t>(count));
  p += writeLe<uint32_t>(p, sample_rate_hz);
  p += writeLe<uint64_t>(p, seq_no);
  p += writeLe<uint64_t>(p, device_ts_us);

  for (size_t i = 0; i < count; ++i) {
    p += writeLe<uint64_t>(p, samples[i].time_ms);
    for (size_t j = 0; j < kFrameFloats; ++j) {
      p += writeFloat(p, samples[i].values[j]);
    }
    p += writeLe<uint8_t>(p, samples[i].accuracies);
    p += writeLe<uint8_t>(p, samples[i].has_data);
  }

  const size_t written = static_cast<size_t>(p - out);
  // A mismatch here means the layout constants and the writes have diverged,
  // which is the exact failure this file's duplication risks. libnatkit-core
  // asserts the same identity at the same point.
  return written == needed ? written : 0;
}

}  // namespace natkit
