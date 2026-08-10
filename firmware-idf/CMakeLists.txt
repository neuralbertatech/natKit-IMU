cmake_minimum_required(VERSION 3.16)

# Native ESP-IDF fork of the natKit-IMU node firmware (EPIC TEC-NATKIT-20).
#
# This tree is ADDITIVE: the Arduino/PlatformIO firmware in ../embeded is the
# image that is known to work on the bench and no part of this fork edits it.
# ../README.md is the one place that records which firmware is on which board
# and the single command that puts the current firmware back.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(natkit_imu_idf)
