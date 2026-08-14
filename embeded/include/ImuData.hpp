#pragma once

#include <cstdint>


// ⚠️ THIRTEEN FLOATS, MATCHING NatImuDataSchema. Frame version 2 added the
// magnetometer at 10-12; layout is accel 0-2, gyro 3-5, quat 6-9, mag 10-12.
// This must stay the same width as the schema, because kafkaTopic.hpp hands the
// array straight to it.
struct ImuData {
    uint64_t timestamp;
    float data[13];
    uint8_t accuracies;
    uint8_t has_data;
};
