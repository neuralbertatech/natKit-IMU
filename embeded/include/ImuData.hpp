#pragma once

#include <cstdint>


struct ImuData {
    uint64_t timestamp;
    float data[10];
    uint8_t accuracies;
    uint8_t has_data;
};
