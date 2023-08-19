#pragma once

#include <array>

#include <Adafruit_SensorLab.h>
#include <Adafruit_Sensor_Calibration.h>
#include <Adafruit_AHRS.h>

#include <ImuData.hpp>
#include <macros.hpp>
#include <Time.hpp>

#if defined(ADAFRUIT_SENSOR_CALIBRATION_USE_EEPROM)
  using AdafruitSensorCalibration_t = Adafruit_Sensor_Calibration_EEPROM;
#else
  using AdafruitSensorCalibration_t = Adafruit_Sensor_Calibration_SDFat;
#endif

template <typename T, uint32_t Size>
struct UnitVector {
    UnitVector() = default;
    std::array<T, Size> data;
};

template <typename T>
class UnitVector3D : protected UnitVector<T, 3> {
    using UnitVector<T, 3>::data;

public:
    UnitVector3D() = default;
    UnitVector3D(T x, T y, T z) { data = {x, y, z}; }

    T& x() { return data[0]; }
    T& y() { return data[1]; }
    T& z() { return data[2]; }
};

using AccelerometerData = UnitVector3D<float>;
using GyroData = UnitVector3D<float>;
using MagnetometerData = UnitVector3D<float>;

class RotationData : protected UnitVector<float, 3> {
    using UnitVector<float, 3>::data;

public:
    RotationData() = default;
    RotationData(float pitch, float roll, float heading) { data = {pitch, roll, heading}; }

    float& pitch() { return data[0]; }
    float& roll() { return data[1]; }
    float& heading() { return data[2]; }
};

enum class ImuFilter {
    NxpSensorFusion,  // Slowest
    Madgwick,
    Mahony            // Fastest
};

class ImuReader {
    Adafruit_SensorLab sensorLab;
    // TODO Add calibration
    //AdafruitSensorCalibration_t calibrator;

    AccelerometerData accelerometerData;
    GyroData gyroData;
    MagnetometerData magnetometerData;
    // TODO Add rotational data
    //RotationData rotationData;

    Adafruit_Sensor* accelerometer;
    Adafruit_Sensor* gyroscope;
    Adafruit_Sensor* magnetometer;

    uint32_t samplingRate;

    // TODO Add filter
    //ImuFilter selectedFilter;

public:
    ImuReader(uint32_t samplingRate) : samplingRate(samplingRate) {}

    void start() {
        sensorLab.begin();

        accelerometer = sensorLab.getAccelerometer();
        if (accelerometer == nullptr) {
            DEBUG_SERIAL.println("Could not find accelerometer");
        }

        gyroscope = sensorLab.getAccelerometer();
        if (gyroscope == nullptr) {
            DEBUG_SERIAL.println("Could not find gyroscope");
        }

        magnetometer = sensorLab.getAccelerometer();
        if (magnetometer == nullptr) {
            DEBUG_SERIAL.println("Could not find magnetometer");
        }
    }

    void update() {
        sensors_event_t accelerometerEvent;
        sensors_event_t gyroscopeEvent;
        sensors_event_t magnetometerEvent;

        accelerometer->getEvent(&accelerometerEvent);
        gyroscope->getEvent(&gyroscopeEvent);
        magnetometer->getEvent(&magnetometerEvent);

        accelerometerData.x() = accelerometerEvent.acceleration.x;
        accelerometerData.y() = accelerometerEvent.acceleration.y;
        accelerometerData.z() = accelerometerEvent.acceleration.z;

        // Gyroscope needs to be converted from Rad/s to Degree/s
        // the rest are not unit-important
        gyroData.x() = gyroscopeEvent.gyro.x * SENSORS_RADS_TO_DPS;
        gyroData.y() = gyroscopeEvent.gyro.y * SENSORS_RADS_TO_DPS;
        gyroData.z() = gyroscopeEvent.gyro.z * SENSORS_RADS_TO_DPS;

        magnetometerData.x() = magnetometerEvent.magnetic.x;
        magnetometerData.y() = magnetometerEvent.magnetic.y;
        magnetometerData.z() = magnetometerEvent.magnetic.z;
    }

    void calibrate() {
        ;
    }

    void getAccelerometerData(AccelerometerData* data) {
        *data = accelerometerData;
    }

    void getGryoData(GyroData* data) {
        *data = gyroData;
    }

    void getMagetometerData(MagnetometerData* data) {
        *data = magnetometerData;
    }

    void getImuData(ImuData* data) {
        data->timestamp = getTime();

        // TODO: Assigning a reference here, don't do that
        data->data[0] = accelerometerData.x();
        data->data[1] = accelerometerData.y();
        data->data[2] = accelerometerData.z();

        data->data[3] = gyroData.x();
        data->data[4] = gyroData.y();
        data->data[5] = gyroData.z();

        data->data[6] = magnetometerData.x();
        data->data[7] = magnetometerData.y();
        data->data[8] = magnetometerData.z();

        data->calibration = 0;
    }
};