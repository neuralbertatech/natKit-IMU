#pragma once

// Single place for the board's hardware and runtime configuration.
//
// These values used to be duplicated: the BNO08x SPI pins were defined
// identically in BOTH Bno08xDevice2.hpp and ImuReader.hpp, and the MQTT port was
// a bare 1883 in the middle of main.cpp. Identical macro redefinitions are legal,
// which is exactly why the duplication was dangerous — change one copy and the
// build stays quiet while half the firmware talks to different pins.
//
// Pins are ESP32 GPIO numbers for the natKit-IMU board (ESP32-PICO-V3-02).

// --- BNO08x IMU, SPI ------------------------------------------------------
#define BNO08X_CS 15
#define BNO08X_INT 32
#define BNO08X_RESET 14
#define BNO08X_SCK 5
#define BNO08X_MISO 21
#define BNO08X_MOSI 19

// --- Status LEDs ----------------------------------------------------------
#define STATUS_NEOPIXEL_PIN 4
#define STATUS_NEOPIXEL_NUM_PIXELS 1
#define STATUS_NEOPIXEL_BRIGHTNESS 24
#define ONBOARD_NEOPIXEL_PIN 0
#define ONBOARD_NEOPIXEL_POWER_PIN 2
#define ONBOARD_NEOPIXEL_NUM_PIXELS 1

// --- Sampling -------------------------------------------------------------
// Interval between IMU samples, in MICROSECONDS. 20000 us = 50 Hz.
#define DELAY_BETWEEN_SAMPLES 20000

// --- Broker ---------------------------------------------------------------
// The MQTT port. The host itself is resolved at runtime (mDNS / configured
// address), so only the port belongs here.
#ifndef NATKIT_MQTT_PORT
#define NATKIT_MQTT_PORT 1883
#endif
