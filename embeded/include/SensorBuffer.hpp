#pragma once

#include <CircularBuffer.hpp>
#include <optional>
#include <utility>

// Minimal inline optional (no heap, no C++17). Drop-in for the .value()/
// .has_value()/`= T{}` subset used by SensorDataPoint — replaces
// nat::core::Optional<T>, which heap-allocated its value on every copy.
template <typename T>
struct InlineOptional {
    T val{};
    bool present = false;

    InlineOptional() = default;
    InlineOptional(const T& v) : val(v), present(true) {}
    InlineOptional& operator=(const T& v) { val = v; present = true; return *this; }

    bool has_value() const { return present; }
    T& value() { return val; }
    const T& value() const { return val; }
};

template <typename T>
struct SensorDataPoint {
    uint64_t timestamp;
    // InlineOptional stores T INLINE (no heap). The previous nat::core::Optional<T>
    // heap-allocated its value (new T) on every copy; with SensorDataPoints copied
    // many times per sample through 3×512-slot circular buffers, that churned/grew
    // the heap until encodeToBytes() hit std::bad_alloc and the device rebooted.
    // (std::optional would need C++17; the Arduino build here is gnu++11.)
    InlineOptional<T> dataMaybe;
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
