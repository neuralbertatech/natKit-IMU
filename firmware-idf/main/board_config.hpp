#pragma once

// Hardware configuration for the natKit-IMU board (ESP32-PICO-V3-02).
//
// These GPIO numbers are copied from ../../embeded/include/BoardConfig.hpp and
// MUST match it: the same physical board is flashed with either firmware, so a
// disagreement here does not fail to build, it just talks to the wrong pins.
// If the board revision changes, both files change together.

#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace natkit {

// --- BNO08x IMU, SPI ------------------------------------------------------
constexpr gpio_num_t kBnoCs = GPIO_NUM_15;
constexpr gpio_num_t kBnoInt = GPIO_NUM_32;
constexpr gpio_num_t kBnoReset = GPIO_NUM_14;
constexpr gpio_num_t kBnoSck = GPIO_NUM_5;
constexpr gpio_num_t kBnoMiso = GPIO_NUM_21;
constexpr gpio_num_t kBnoMosi = GPIO_NUM_19;

// SPI2 (HSPI). SPI3/VSPI is free; SPI1 is the flash. Any of the six pins above
// being a non-default GPIO means the transfers go through the GPIO matrix rather
// than the IOMUX fast path, which is fine at 1 MHz.
constexpr spi_host_device_t kBnoSpiHost = SPI2_HOST;

// 1 MHz, SPI mode 3, MSB first — the settings the Adafruit stack used on this
// board and the ones it has been streaming with. The BNO08x datasheet allows
// 3 MHz; going faster is a deliberate experiment with a measurement attached,
// not a free win, because this hub has proven fragile about timing.
constexpr int kBnoSpiClockHz = 1'000'000;
constexpr int kBnoSpiMode = 3;

// The driver's own limits (SH2_HAL_MAX_TRANSFER_IN 384, SH2_HAL_DMA_SIZE 512).
constexpr int kBnoSpiMaxTransferBytes = 512;

}  // namespace natkit
