#pragma once

#include <CircularBuffer.hpp>
#include <optional>
#include <utility>

template <typename T>
struct SensorDataPoint {
    uint64_t timestamp;
    // nat::core::Optional<T> now stores T inline (no heap) — so copying a
    // SensorDataPoint per sample no longer allocates. (Previously it wrapped a
    // std::unique_ptr<T>, heap-allocating the value on every copy, which churned
    // the heap until encodeToBytes() hit std::bad_alloc and rebooted the device.)
    nat::core::Optional<T> dataMaybe;
    uint8_t calibration;

    SensorDataPoint()
      : timestamp(0), dataMaybe(), calibration(0) {}
      
    SensorDataPoint(const SensorDataPoint<T>& other)
      : timestamp(other.timestamp), dataMaybe(other.dataMaybe), calibration(other.calibration) {}
};

// @ThreadSafe iff there is only a single reader. Multiple readers on seperate threads will not work as expected
template <typename T>
class SensorBuffer {
    // 32 slots is ample: producer (update3) and consumer (getImuData) run in the
    // same task each loop, so backlog stays ~1-2. The default 512 × 3 buffers ×
    // inline SensorDataPoint would waste ~40 KB of .bss (and starved static-init
    // heap → bad_alloc at boot).
    CircularBuffer<SensorDataPoint<T>, 32> buffer;
    std::unique_ptr<std::pair<SensorDataPoint<T>, uint64_t>> previous_data_point_maybe;
    uint64_t expected_delay;
    uint64_t max_delay;

public:
    SensorBuffer(uint64_t expected_delay, uint64_t max_delay) 
      : buffer(), expected_delay(expected_delay), max_delay(max_delay) {}

    void push(const SensorDataPoint<T>& data) {
        buffer.push(data);
    }

    // Will return on of the following:
    //     * The next data point if it exists, with data interpolated to match the expected_delay via linear interpolation
    //     * A synthetic data point with an expected timestamp and no data if no samples were polled within the max_delay passed
    //     * An empty Optional if the buffer is empty
    std::unique_ptr<SensorDataPoint<T>> tryGetNext() {
        if (previous_data_point_maybe) {
            auto previous_data_point = previous_data_point_maybe->first;
            auto adjusted_previous_timestamp = previous_data_point_maybe->second;
            auto data_point_maybe = buffer.peek();
            if (!data_point_maybe) {
                // Buffer is empty
                return {};
            }
            SensorDataPoint<T> current_data_point = *data_point_maybe;

            assert(current_data_point.timestamp > adjusted_previous_timestamp && "Timestamps must be monotonic!");
            if (current_data_point.timestamp - adjusted_previous_timestamp <= max_delay) {
                // Sample exists and is interpolated
                buffer.pop();
                previous_data_point_maybe = nat::core::make_unique<std::pair<SensorDataPoint<T>, uint64_t>>(std::make_pair(*data_point_maybe, current_data_point.timestamp));
                return data_point_maybe;
            } else {
                // If stream timing drifts, prefer emitting the real buffered sample
                // instead of synthesizing an empty sample that clears has_data.
                buffer.pop();
                previous_data_point_maybe = nat::core::make_unique<std::pair<SensorDataPoint<T>, uint64_t>>(std::make_pair(*data_point_maybe, current_data_point.timestamp));
                return data_point_maybe;
            }
        } else {
            // TODO: We are doing this if/else because we don't know when the expected first timestamp should be.
            //       It may be worth while to pass in an initial timestamp to avoid subtle bugs here
            auto data_point_maybe = buffer.pop();
            if (!data_point_maybe) {
                // Buffer is empty
                //DEBUG_SERIAL.printf("No good\n");
                return {};
            }
            SensorDataPoint<T> current_data_point = *data_point_maybe;
            previous_data_point_maybe = nat::core::make_unique<std::pair<SensorDataPoint<T>, uint64_t>>(std::make_pair(*data_point_maybe, current_data_point.timestamp));
            return nat::core::make_unique<SensorDataPoint<T>>(current_data_point);

        }
    }
};
