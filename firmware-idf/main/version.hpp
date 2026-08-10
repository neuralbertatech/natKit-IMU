#pragma once

// Version of the ESP-IDF fork.
//
// Deliberately numbered separately from the Arduino firmware
// (../../embeded/include/version.hpp, currently 0.5.0). Both images can sit on
// the bench at the same time and speak to the same broker, so a shared version
// string would make a console log or a heartbeat ambiguous about which
// firmware actually booted. The name in the banner is the disambiguator.

#define NATKIT_IMU_IDF_VERSION_STRING "0.1.0"
#define NATKIT_IMU_IDF_FIRMWARE_NAME "natKit-IMU-idf"

#define NATKIT_IMU_IDF_BUILD_DATE __DATE__
#define NATKIT_IMU_IDF_BUILD_TIME __TIME__
