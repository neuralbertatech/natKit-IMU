#pragma once

#include <cstdint>


struct ImuData {
    uint64_t timestamp;
    float data[13];
    int accuracy;
};
