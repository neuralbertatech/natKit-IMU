#pragma once

#include <array>

#include <Arduino.h>
#include <Adafruit_BNO08x.h>
#include <SPI.h>

#include <ImuData.hpp>
#include <macros.hpp>
#include <Time.hpp>

// For SPI mode, we need a CS pin
#define BNO08X_CS 15
#define BNO08X_INT 32

// For SPI mode, we also need a RESET 
#define BNO08X_RESET 14

#define BNO08X_SCK 5
#define BNO08X_MISO 21
#define BNO08X_MOSI 19

struct euler_t {
  float yaw;
  float pitch;
  float roll;
} ypr;




// #define FAST_MODE

#ifdef FAST_MODE
  // Top frequency is reported to be 1000Hz (but freq is somewhat variable)
  sh2_SensorId_t reportType = SH2_GYRO_INTEGRATED_RV;
  long reportIntervalUs = 2000;
#else
  // Top frequency is about 250Hz but this report is more accurate
  sh2_SensorId_t reportType = SH2_ARVR_STABILIZED_RV;
  long reportIntervalUs = 5000;
//   long reportIntervalUs = 10000;
#endif


// #if defined(ADAFRUIT_SENSOR_CALIBRATION_USE_EEPROM)
//   using AdafruitSensorCalibration_t = Adafruit_Sensor_Calibration_EEPROM;
// #else
//   using AdafruitSensorCalibration_t = Adafruit_Sensor_Calibration_SDFat;
// #endif

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

// using AccelerometerData = UnitVector3D<float>;
// using GyroData = UnitVector3D<float>;
// using MagnetometerData = UnitVector3D<float>;

class RotationData : protected UnitVector<float, 3> {
    using UnitVector<float, 3>::data;

public:
    RotationData() = default;
    RotationData(float pitch, float roll, float heading) { data = {pitch, roll, heading}; }

    float& pitch() { return data[0]; }
    float& roll() { return data[1]; }
    float& heading() { return data[2]; }
};

// enum class ImuFilter {
//     NxpSensorFusion,  // Slowest
//     Madgwick,
//     Mahony            // Fastest
// };

class ImuReader {
    SPIClass spiClass{};

    Adafruit_BNO08x bno08x{BNO08X_RESET};

    sh2_SensorValue_t sensorValue;

    // TODO Add filter
    //ImuFilter selectedFilter;

public:
    ImuReader() {
        spiClass.begin(BNO08X_SCK, BNO08X_MISO, BNO08X_MOSI);
    }

    // DEBUG_SERIAL.println("Could not enable stabilized remote vector");


    void setReports(sh2_SensorId_t reportType, long report_interval) {
        DEBUG_SERIAL.println("Setting desired reports");
        if (! bno08x.enableReport(reportType, report_interval)) {
            DEBUG_SERIAL.println("Could not enable stabilized remote vector");
        }
    }

    void start() {
         if (!bno08x.begin_SPI(BNO08X_CS, BNO08X_INT, &spiClass)) {
            DEBUG_SERIAL.println("Failed to find BNO08x chip");
            while (1) { delay(10); }
        }
        DEBUG_SERIAL.println("BNO08x Found!");
        
        setReports(reportType, reportIntervalUs);

        DEBUG_SERIAL.println("Reading events");
    }

    void update() {
        if (bno08x.wasReset()) {
            DEBUG_SERIAL.print("sensor was reset ");
            setReports(reportType, reportIntervalUs);
        }
        
        if (bno08x.getSensorEvent(&sensorValue)) {
            // in this demo only one report type will be received depending on FAST_MODE define (above)
            switch (sensorValue.sensorId) {
                case SH2_ARVR_STABILIZED_RV:
                    quaternionToEulerRV(&sensorValue.un.arvrStabilizedRV, &ypr, true);
                    break;
                case SH2_GYRO_INTEGRATED_RV:
                    // faster (more noise?)
                    quaternionToEulerGI(&sensorValue.un.gyroIntegratedRV, &ypr, true);
                    break;
            }
            static long last = 0;
            long now = micros();
            // DEBUG_SERIAL.print(now - last);             DEBUG_SERIAL.print("\t");
            last = now;
            // DEBUG_SERIAL.print(sensorValue.status);     DEBUG_SERIAL.print("\t");  // This is accuracy in the range of 0 to 3
            // DEBUG_SERIAL.print(ypr.yaw);                DEBUG_SERIAL.print("\t");
            // DEBUG_SERIAL.print(ypr.pitch);              DEBUG_SERIAL.print("\t");
            // DEBUG_SERIAL.println(ypr.roll);
        }
        // sensors_event_t accelerometerEvent;
        // sensors_event_t gyroscopeEvent;
        // sensors_event_t magnetometerEvent;

        // accelerometer->getEvent(&accelerometerEvent);
        // gyroscope->getEvent(&gyroscopeEvent);
        // magnetometer->getEvent(&magnetometerEvent);

        // accelerometerData.x() = accelerometerEvent.acceleration.x;
        // accelerometerData.y() = accelerometerEvent.acceleration.y;
        // accelerometerData.z() = accelerometerEvent.acceleration.z;

        // // Gyroscope needs to be converted from Rad/s to Degree/s
        // // the rest are not unit-important
        // gyroData.x() = gyroscopeEvent.gyro.x * SENSORS_RADS_TO_DPS;
        // gyroData.y() = gyroscopeEvent.gyro.y * SENSORS_RADS_TO_DPS;
        // gyroData.z() = gyroscopeEvent.gyro.z * SENSORS_RADS_TO_DPS;

        // magnetometerData.x() = magnetometerEvent.magnetic.x;
        // magnetometerData.y() = magnetometerEvent.magnetic.y;
        // magnetometerData.z() = magnetometerEvent.magnetic.z;
    }

    void calibrate() {
        // DEBUG_SERIAL.print(sensorValue.status);     DEBUG_SERIAL.print("\t");  // This is accuracy in the range of 0 to 3
        ;
    }

    void quaternionToEuler(float qr, float qi, float qj, float qk, euler_t* ypr, bool degrees = false) {

        float sqr = sq(qr);
        float sqi = sq(qi);
        float sqj = sq(qj);
        float sqk = sq(qk);

        ypr->yaw = atan2(2.0 * (qi * qj + qk * qr), (sqi - sqj - sqk + sqr));
        ypr->pitch = asin(-2.0 * (qi * qk - qj * qr) / (sqi + sqj + sqk + sqr));
        ypr->roll = atan2(2.0 * (qj * qk + qi * qr), (-sqi - sqj + sqk + sqr));

        if (degrees) {
        ypr->yaw *= RAD_TO_DEG;
        ypr->pitch *= RAD_TO_DEG;
        ypr->roll *= RAD_TO_DEG;
        }
    }

    void quaternionToEulerRV(sh2_RotationVectorWAcc_t* rotational_vector, euler_t* ypr, bool degrees = false) {
        quaternionToEuler(rotational_vector->real, rotational_vector->i, rotational_vector->j, rotational_vector->k, ypr, degrees);
    }

    void quaternionToEulerGI(sh2_GyroIntegratedRV_t* rotational_vector, euler_t* ypr, bool degrees = false) {
        quaternionToEuler(rotational_vector->real, rotational_vector->i, rotational_vector->j, rotational_vector->k, ypr, degrees);
    }


    void getImuData(ImuData* data) {
        data->timestamp = getTime();

        // // TODO: Assigning a reference here, don't do that
        data->data[0] = ypr.yaw;
        data->data[1] = ypr.pitch;
        data->data[2] = ypr.roll;

        // data->data[0] = accelerometerData.x();
        // data->data[1] = accelerometerData.y();
        // data->data[2] = accelerometerData.z();

        // data->data[3] = gyroData.x();
        // data->data[4] = gyroData.y();
        // data->data[5] = gyroData.z();

        // data->data[6] = magnetometerData.x();
        // data->data[7] = magnetometerData.y();
        // data->data[8] = magnetometerData.z();

        data->calibration = sensorValue.status;
    }
};
