#pragma once

// Hardware configuration for the natKit-IMU board (ESP32-PICO-V3-02).
//
// These GPIO numbers are copied from ../../embeded/include/BoardConfig.hpp and
// MUST match it: the same physical board is flashed with either firmware, so a
// disagreement here does not fail to build, it just talks to the wrong pins.
// If the board revision changes, both files change together.

// ⚠️ sdkconfig.h FIRST, and it is load-bearing rather than tidy: every
// CONFIG_IDF_TARGET_* below is an ordinary preprocessor symbol, so without it
// they all evaluate FALSE and every target silently takes the ESP32 branch. That
// is the same class of failure this file already warns about -- a header that
// compiles and lies -- reached by a different route.
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace natkit {

// --- BNO08x IMU, SPI ------------------------------------------------------
//
// ⚠️ TARGET-CONDITIONAL BECAUSE THE NUMBERS BELOW DO NOT EXIST EVERYWHERE
// (TEC-NATKIT-34). The ESP32-C3 has only GPIO 0-21, so `GPIO_NUM_32` is not a
// number it is a compile error -- and since every source is compiled into every
// image by design, that one line took the WHOLE C3 TARGET down for five slices
// without anyone selecting a C3 role. "All six images build" was false from
// b42d763 (2026-08-13) until this commit.
#if CONFIG_IDF_TARGET_ESP32C3
// ⚠️ THE C3 HAS NO SENSOR WIRING, AND THIS FILE REFUSES TO INVENT ONE.
//
// The C3 in this project is the WiFi gateway (the two-board rig: an ESP32 hub on
// ESP-NOW, a wire, a C3 on WiFi and MQTT). It carries no BNO08x, so there is no
// honest pinout to write here -- and guessing six numbers to make a compiler
// happy produces a header that builds and lies, which is strictly worse than one
// that fails. That was the reason TEC-NATKIT-34 was filed rather than fixed.
//
// So: GPIO_NUM_NC, which is what "not connected" is spelled as, and a hard
// #error below the moment anyone selects the one role that would actually
// dereference these. The target builds; the lie does not get written.
constexpr gpio_num_t kBnoCs = GPIO_NUM_NC;
constexpr gpio_num_t kBnoInt = GPIO_NUM_NC;
constexpr gpio_num_t kBnoReset = GPIO_NUM_NC;
constexpr gpio_num_t kBnoSck = GPIO_NUM_NC;
constexpr gpio_num_t kBnoMiso = GPIO_NUM_NC;
constexpr gpio_num_t kBnoMosi = GPIO_NUM_NC;

#if CONFIG_NATKIT_ROLE_LEAF
#error \
    "No BNO08x pinout exists for the ESP32-C3 (TEC-NATKIT-34). A C3 is the \
gateway in this rig and carries no sensor. If a C3 leaf is now real, wire one \
and put ITS MEASURED pin numbers here -- do not copy the ESP32 block above, \
which is the ESP32-PICO-V3-02's wiring and would fail at runtime as a silent \
all-zero sensor rather than a build error."
#endif

#else  // classic ESP32 and ESP32-S3
constexpr gpio_num_t kBnoCs = GPIO_NUM_15;
constexpr gpio_num_t kBnoInt = GPIO_NUM_32;
constexpr gpio_num_t kBnoReset = GPIO_NUM_14;
constexpr gpio_num_t kBnoSck = GPIO_NUM_5;
constexpr gpio_num_t kBnoMiso = GPIO_NUM_21;
constexpr gpio_num_t kBnoMosi = GPIO_NUM_19;
#endif

// SPI2 (HSPI). SPI3/VSPI is free; SPI1 is the flash. Any of the six pins above
// being a non-default GPIO means the transfers go through the GPIO matrix rather
// than the IOMUX fast path, which is fine at 1 MHz.
constexpr spi_host_device_t kBnoSpiHost = SPI2_HOST;

// --- Identity LED ---------------------------------------------------------
//
// The NeoPixel SOLDERED ONTO the boards, on the same pin the Arduino firmware
// used (STATUS_NEOPIXEL_PIN 4 in ../../embeded/include/BoardConfig.hpp). Free of
// the BNO08x pins above (15/32/14/5/21/19), and the firmware uses RMT nowhere
// else, so the identity light cannot contend with anything.
//
// ⚠️ NOT the board's ONBOARD pixel, which is GPIO 0 and needs its power rail
// switched on via GPIO 2 first (ONBOARD_NEOPIXEL_POWER_PIN). GPIO 0 is also the
// strapping pin that selects download mode, so driving it is a way to make a
// board that will not boot.
constexpr gpio_num_t kStatusLedGpio = GPIO_NUM_4;
constexpr uint32_t kStatusLedCount = 1;
// 24/255, matching the Arduino firmware: enough to read across a bench, not
// enough to dazzle or to matter to the power budget next to a transmitting radio.
// A default, not a cap -- the frontend may send its own, and this is what a colour
// sent without one gets.
constexpr uint8_t kStatusLedDefaultBrightness = 24;

// 1 MHz, SPI mode 3, MSB first — the settings the Adafruit stack used on this
// board and the ones it has been streaming with.
//
// ⚠️ 3 MHz WAS TRIED AND BUYS NOTHING. The datasheet allows it, and the report
// rates at 3 MHz are identical to 1 MHz to within noise: accel 115/114/117,
// gyro 93/93/94, mag 90/90/90, quat 93/93/94 Hz in both cases. The report rate
// is therefore NOT limited by how fast the bytes move, so raising the clock only
// adds risk on a hub that has proven fragile about timing. Left at 1 MHz.
constexpr int kBnoSpiClockHz = 1'000'000;
constexpr int kBnoSpiMode = 3;

// The driver's own limits (SH2_HAL_MAX_TRANSFER_IN 384, SH2_HAL_DMA_SIZE 512).
constexpr int kBnoSpiMaxTransferBytes = 512;

}  // namespace natkit
