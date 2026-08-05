#pragma once

#include <BoardConfig.hpp>


#include <Arduino.h>
#include <array>

#include <Arduino.h>
#include <Adafruit_BNO08x.h>
#include <SPI.h>

#include <ImuData.hpp>
#include <macros.hpp>
#include <Time.hpp>
#include <Adafruit_NeoPixel.h>
#include <SensorBuffer.hpp>
#include <Bno08xDevice2.hpp>

// Pins and the sample interval live in BoardConfig.hpp.
#define ONBOARD_NEOPIXEL_BRIGHTNESS 24

// Status NeoPixels DISABLED: Adafruit_NeoPixel::show() re-installs the ESP32 RMT
// driver on every call and leaks channels, so after ~11 calls rmt_driver_install
// asserts (xQueueGenericSend null queue) and the device reboots (~14 s loop). The
// cosmetic calibration/source LEDs are not worth crashing the stream. Re-enable in
// Phase 1 with a fixed RMT path (install the driver once, don't show() per poll).
#define IMU_STATUS_LED_ENABLED 0


struct euler_t {
  float yaw;
  float pitch;
  float roll;
} ypr;

#define FAST_MODE

#ifdef FAST_MODE
  // Top frequency is reported to be 1000Hz (but freq is somewhat variable)
  sh2_SensorId_t reportType = SH2_ROTATION_VECTOR;
//   sh2_SensorId_t reportType = SH2_GYRO_INTEGRATED_RV;
  long reportIntervalUs = 5000;
#else
  // Top frequency is about 250Hz but this report is more accurate
  //sh2_SensorId_t reportType = SH2_ARVR_STABILIZED_RV;
  sh2_SensorId_t reportType = SH2_ROTATION_VECTOR;
  long reportIntervalUs = 10000;
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
protected:
    using UnitVector<T, 3>::data;

public:
    UnitVector3D() = default;
    UnitVector3D(T x, T y, T z) { data = {x, y, z}; }

    T& x() { return data[0]; }
    T& y() { return data[1]; }
    T& z() { return data[2]; }
};

template <typename T>
class RotationVectorData {
    std::array<T, 4> data;
    byte _accuracy;

public:
    RotationVectorData() = default;
    RotationVectorData(T real, T i, T j, T k, byte accuracy) { data = {real, i, j, k}; _accuracy = accuracy; }

    T& real() { return data[0]; }
    T& i() { return data[1]; }
    T& j() { return data[2]; }
    T& k() { return data[3]; }
    byte& accuracy() { return _accuracy; }
};

//using AccelerometerData = UnitVector3D<float>;
class AccelerometerData : public UnitVector3D<float> {
    byte _accuracy;

public:
    AccelerometerData() = default;
    AccelerometerData(float x, float y, float z, byte accuracy) { data = {x, y, z}; this->_accuracy = accuracy; }

    byte& accuracy() { return _accuracy; }
};
//using GyroData = UnitVector3D<float>;
class GyroData : public UnitVector3D<float> {
    byte _accuracy;

public:
    GyroData() = default;
    GyroData(float x, float y, float z, byte accuracy) { data = {x, y, z}; this->_accuracy = accuracy; }

    byte& accuracy() { return _accuracy; }
};
//using MagnetometerData = UnitVector3D<float>;
class MagnetometerData : public UnitVector3D<float> {
    byte _accuracy;

public:
    MagnetometerData() = default;
    MagnetometerData(float x, float y, float z, byte accuracy) { data = {x, y, z}; this->_accuracy = accuracy; }

    byte& accuracy() { return _accuracy; }
};

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

struct Color {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

struct LedMessage {
  virtual Color getColor(size_t tick_number) = 0;
};

class StaticLedMessage : public LedMessage {
  Color color;

public:
  StaticLedMessage(Color color)
    : color(color) {}

  virtual Color getColor(size_t tick_number) {
    return color;
  }
};

template <size_t N>
struct DataArray {
  std::array<float, N> data;

  DataArray()
    : data() {}

  DataArray(const DataArray<N>& other) {
    for (size_t i = 0; i < N; ++i) {
      data[i] = other.data[i];
    }
  }

  DataArray(DataArray<N>&& other) {
    for (size_t i = 0; i < N; ++i) {
      data[i] = other.data[i];
    }
  }

  // DataArray() = default;
  // DataArray(const DataArray<N>& other) = default;
  // DataArray(DataArray<N>&& other) = default;
};

class ImuReader {
    SPIClass spiClass{};

    Adafruit_BNO08x bno08x{BNO08X_RESET};
    Bno08xDevice bno08x_device{};

    sh2_SensorValue_t sensorValue;

    // TODO Add filter
    //ImuFilter selectedFilter;
    AccelerometerData accelerometerData;
    GyroData gyroData;
    MagnetometerData magnetometerData;
    RotationVectorData<float> rotationVectorData;
    int accuracy;

    Adafruit_Sensor* accelerometer;
    Adafruit_Sensor* gyroscope;
    Adafruit_Sensor* magnetometer;

    int lateAcceleromerterCount = 0;
    int lateGyroCount = 0;
    int lateMagnetometerCount = 0;
    int lateRotationCount = 0;
    uint64_t lastAccelerometerTimestampUs = 0;
    uint64_t lastGyroTimestampUs = 0;
    uint64_t lastMagnetometerTimestampUs = 0;
    uint64_t lastRotationTimestampUs = 0;
    bool accelerometerRead = false;
    bool gyroRead = false;
    bool magnometerRead = false;
    bool rotationRead = false;
    int accelerometerReads = 0;
    int gyroReads = 0;
    int magnetometerReads = 0;
    int rotationReads = 0;
    uint64_t largestAccelGap = 0;
    uint64_t largestGyroGap = 0;
    uint64_t largestMagnoGap = 0;
    uint64_t largestRotationGap = 0;

    uint64_t loopIteration = -1;
    uint64_t loopLastTimestamp = 0;
    uint64_t samplePollLastTimestamp = 0;

    static constexpr uint64_t SENSOR_BUFFER_EXPECTED_DELAY_US = DELAY_BETWEEN_SAMPLES;
    static constexpr uint64_t SENSOR_BUFFER_MAX_DELAY_US = DELAY_BETWEEN_SAMPLES * 4;
    SensorBuffer<Bno08xEvent::ThreeDimensional> accelerometerBuffer{SENSOR_BUFFER_EXPECTED_DELAY_US, SENSOR_BUFFER_MAX_DELAY_US};
    SensorBuffer<Bno08xEvent::ThreeDimensional> gyroscopeBuffer{SENSOR_BUFFER_EXPECTED_DELAY_US, SENSOR_BUFFER_MAX_DELAY_US};
    // SensorBuffer<Bno08xEvent::ThreeDimensional> magnetometerBuffer{SENSOR_BUFFER_EXPECTED_DELAY_US, SENSOR_BUFFER_MAX_DELAY_US};
    SensorBuffer<Bno08xEvent::FourDimensional> rotationBuffer{SENSOR_BUFFER_EXPECTED_DELAY_US, SENSOR_BUFFER_MAX_DELAY_US};
    SensorDataPoint<Bno08xEvent::ThreeDimensional> latestAccelerometerSample{};
    SensorDataPoint<Bno08xEvent::ThreeDimensional> latestGyroscopeSample{};
    SensorDataPoint<Bno08xEvent::FourDimensional> latestRotationSample{};
    bool hasLatestAccelerometerSample = false;
    bool hasLatestGyroscopeSample = false;
    bool hasLatestRotationSample = false;
    Adafruit_NeoPixel statusPixel{STATUS_NEOPIXEL_NUM_PIXELS, STATUS_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800};
    Adafruit_NeoPixel sourceIndicatorPixel{ONBOARD_NEOPIXEL_NUM_PIXELS, ONBOARD_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800};
    bool statusPixelInitialized = false;
    uint8_t accelerometerCalibration = 0;
    uint8_t gyroscopeCalibration = 0;
    uint8_t rotationCalibration = 0;
    bool hasAccelerometerCalibration = false;
    bool hasGyroscopeCalibration = false;
    bool hasRotationCalibration = false;
    uint8_t lastOverallCalibration = 255;

    enum class CalibrationSource : uint8_t {
        None = 0,
        Accelerometer,
        Gyroscope,
        Rotation,
    };

    struct CalibrationSelection {
        uint8_t calibration;
        CalibrationSource source;
    };

    uint8_t sanitizeCalibration(uint8_t calibration) const {
        return calibration & 0b11;
    }

    CalibrationSelection getDisplayedCalibration() const {
        bool hasAnyCalibration = false;
        CalibrationSelection selection{};
        selection.calibration = 0;
        selection.source = CalibrationSource::None;

        if (hasAccelerometerCalibration) {
            hasAnyCalibration = true;
            selection.calibration = accelerometerCalibration;
            selection.source = CalibrationSource::Accelerometer;
        }
        if (hasGyroscopeCalibration) {
            if (!hasAnyCalibration || gyroscopeCalibration < selection.calibration) {
                selection.calibration = gyroscopeCalibration;
                selection.source = CalibrationSource::Gyroscope;
            }
            hasAnyCalibration = true;
        }
        if (hasRotationCalibration) {
            if (!hasAnyCalibration || rotationCalibration < selection.calibration) {
                selection.calibration = rotationCalibration;
                selection.source = CalibrationSource::Rotation;
            }
            hasAnyCalibration = true;
        }

        if (!hasAnyCalibration) {
            selection.calibration = 0;
            selection.source = CalibrationSource::None;
        }

        return selection;
    }

    uint32_t calibrationToColor(uint8_t calibration) {
        switch (sanitizeCalibration(calibration)) {
            case 0: // Unknown
                return statusPixel.Color(0, 0, 255);
            case 1: // Low
                return statusPixel.Color(255, 0, 0);
            case 2: // Medium
                return statusPixel.Color(255, 160, 0);
            case 3: // High
                return statusPixel.Color(0, 255, 0);
            default:
                return statusPixel.Color(0, 0, 255);
        }
    }

    void updateSourceIndicatorLed(CalibrationSource source) {
        uint32_t sourceColor = 0;
        switch (source) {
            case CalibrationSource::Accelerometer:
                sourceColor = sourceIndicatorPixel.Color(255, 120, 0); // Orange
                break;
            case CalibrationSource::Gyroscope:
                sourceColor = sourceIndicatorPixel.Color(0, 180, 255); // Cyan
                break;
            case CalibrationSource::Rotation:
                sourceColor = sourceIndicatorPixel.Color(200, 0, 255); // Purple
                break;
            case CalibrationSource::None:
            default:
                sourceColor = 0;
                break;
        }
        sourceIndicatorPixel.setPixelColor(0, sourceColor);
        sourceIndicatorPixel.show();
    }

    void updateCalibrationLed() {
#if !IMU_STATUS_LED_ENABLED
        return; // LEDs disabled — see IMU_STATUS_LED_ENABLED (RMT crash).
#endif
        if (!statusPixelInitialized) {
            return;
        }
        const CalibrationSelection selection = getDisplayedCalibration();
        if (selection.calibration != lastOverallCalibration) {
            lastOverallCalibration = selection.calibration;
            statusPixel.setPixelColor(0, calibrationToColor(selection.calibration));
            statusPixel.show();
        }
        updateSourceIndicatorLed(selection.source);
    }

    void initializeCalibrationLed() {
#if IMU_STATUS_LED_ENABLED
        pinMode(ONBOARD_NEOPIXEL_POWER_PIN, OUTPUT);
        digitalWrite(ONBOARD_NEOPIXEL_POWER_PIN, HIGH);

        statusPixel.begin();
        statusPixel.setBrightness(STATUS_NEOPIXEL_BRIGHTNESS);

        sourceIndicatorPixel.begin();
        sourceIndicatorPixel.setBrightness(ONBOARD_NEOPIXEL_BRIGHTNESS);
        sourceIndicatorPixel.setPixelColor(0, 0);
        sourceIndicatorPixel.show();

        statusPixelInitialized = true;
        updateCalibrationLed();
#endif
    }

public:
    ImuReader() {
        // spiClass.begin(BNO08X_SCK, BNO08X_MISO, BNO08X_MOSI);
    }

    void setupReports(Adafruit_BNO08x& bno08x, long report_interval) {
        // if (!bno08x.enableReport(SH2_ACCELEROMETER, 8000)) {
        //     Serial.println("Could not enable accelerometer");
        // }
        if (!bno08x.enableReport(SH2_RAW_ACCELEROMETER, 20000)) {
            Serial.println("Could not enable accelerometer");
        }
        // if (!bno08x.enableReport(SH2_LINEAR_ACCELERATION, 8000)) {
        //     Serial.println("Could not enable accelerometer");
        // }
        if (!bno08x.enableReport(SH2_GYROSCOPE_CALIBRATED, 20000)) {
            Serial.println("Could not enable gyroscope");
        }

        if (!bno08x.enableReport(SH2_ROTATION_VECTOR, 20000)) {
            Serial.println("Could not enable rotation vector");
        }
        // if (!bno08x.enableReport(SH2_GEOMAGNETIC_ROTATION_VECTOR, 5000)) {
        //   Serial.println("Could not eanble geo rotation vector");
        // }
    }

    void start() {
        Serial.println("Starting the BNO08X");
         while (!bno08x.begin_SPI(BNO08X_CS, BNO08X_INT, &spiClass)) {
          Serial.println("Failed to start the BNO08X, trying again");
            DEBUG_SERIAL.println("Failed to find BNO08x chip");
            int i = 0;
            while (++i < 100) { delay(10); }
        }
        Serial.println("BNO08X started");
        DEBUG_SERIAL.println("BNO08x Found!");

        setupReports(bno08x, reportIntervalUs);

        DEBUG_SERIAL.println("Reading events");
    }

    void start2() {
      initializeCalibrationLed();
      Serial.println("Starting the BNO08X");
      bool was_successful;
      std::string error_maybe = bno08x_device.start(was_successful);
      if (!was_successful) {
        Serial.printf("Failed to start the BNO08X chip because: %s\n", error_maybe.c_str());
        return;
      }
      Serial.println("BNO08X Device Started");
    }

    void update_loop() {
      static const uint64_t timeBetweenEachLoopUs = 1000; // 1ms

        uint64_t current_timestamp = esp_timer_get_time();
        if (current_timestamp - loopLastTimestamp < timeBetweenEachLoopUs) {
          return;
        }
        if (current_timestamp - samplePollLastTimestamp >= 10 * 1000) {
          samplePollLastTimestamp = current_timestamp;
          ++loopIteration;
          if (loopIteration % 1000 == 0) {
            Serial.printf("Late Counts");
            if (accelerometerRead)
              DEBUG_SERIAL.printf(", Accel: %llu/%llu (%llu us)", (unsigned long long)lateAcceleromerterCount, (unsigned long long)accelerometerReads, (unsigned long long)largestAccelGap);
            if (gyroRead)
              DEBUG_SERIAL.printf(", Gyro: %llu/%llu (%llu us)", (unsigned long long)lateGyroCount, (unsigned long long)gyroReads, (unsigned long long)largestGyroGap);
            if (magnometerRead)
              DEBUG_SERIAL.printf(", Magno: %llu/%llu (%llu us)", (unsigned long long)lateMagnetometerCount, (unsigned long long)magnetometerReads, (unsigned long long)largestMagnoGap);
            if (rotationRead)
              DEBUG_SERIAL.printf(", Rotation: %llu/%llu (%llu us)", (unsigned long long)lateRotationCount, (unsigned long long)rotationReads, (unsigned long long)largestRotationGap);
            DEBUG_SERIAL.printf("\n");
            lateAcceleromerterCount = 0;
            lateGyroCount = 0;
            lateMagnetometerCount = 0;
            lateRotationCount = 0;
            accelerometerRead = false;
            gyroRead = false;
            magnometerRead = false;
            rotationRead = false;
            accelerometerReads = 0;
            gyroReads = 0;
            magnetometerReads = 0;
            rotationReads = 0;
            largestAccelGap = 0;
            largestGyroGap = 0;
            largestMagnoGap = 0;
            largestRotationGap = 0;
            digitalWrite(12, LOW);
          }
        }
        loopLastTimestamp = current_timestamp;
        update();
    }

    void update() {
      const uint64_t maxTimeBetweenUpdatesUs = 10 * 1000; // 10ms
        if (bno08x.wasReset()) {
            DEBUG_SERIAL.print("sensor was reset \n");
            setupReports(bno08x, reportIntervalUs);
        }


        digitalWrite(13, HIGH);
        if (!bno08x.getSensorEvent(&sensorValue)) {
            DEBUG_SERIAL.printf("There was an error!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        }
        uint64_t currentTimestamp{};
        uint64_t timeGap{};
        // in this demo only one report type will be received depending on FAST_MODE define (above)
        switch (sensorValue.sensorId) {
            case SH2_ACCELEROMETER:
                accelerometerData.x() = sensorValue.un.accelerometer.x;
                accelerometerData.y() = sensorValue.un.accelerometer.y;
                accelerometerData.z() = sensorValue.un.accelerometer.z;
                accelerometerData.accuracy() = sensorValue.status;
                accelerometerReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastAccelerometerTimestampUs;
                if (timeGap > (13 * 1000) && lastAccelerometerTimestampUs != 0) {
                  ++lateAcceleromerterCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestAccelGap) {
                  largestAccelGap = timeGap;
                }
                lastAccelerometerTimestampUs = currentTimestamp;
                accelerometerRead = true;
                // accelerometer_updated = true;
                // accelerometer_count += 1;
                break;

            case SH2_RAW_ACCELEROMETER:
                accelerometerData.x() = sensorValue.un.linearAcceleration.x;
                accelerometerData.y() = sensorValue.un.linearAcceleration.y;
                accelerometerData.z() = sensorValue.un.linearAcceleration.z;
                accelerometerData.accuracy() = sensorValue.status;
                accelerometerReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastAccelerometerTimestampUs;
                if (timeGap > (13 * 1000) && lastAccelerometerTimestampUs != 0) {
                  ++lateAcceleromerterCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestAccelGap) {
                  largestAccelGap = timeGap;
                }
                lastAccelerometerTimestampUs = currentTimestamp;
                accelerometerRead = true;
                // accelerometer_updated = true;
                // accelerometer_count += 1;
                break;

            case SH2_LINEAR_ACCELERATION:
                accelerometerData.x() = sensorValue.un.linearAcceleration.x;
                accelerometerData.y() = sensorValue.un.linearAcceleration.y;
                accelerometerData.z() = sensorValue.un.linearAcceleration.z;
                accelerometerData.accuracy() = sensorValue.status;
                accelerometerReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastAccelerometerTimestampUs;
                if (timeGap > (13 * 1000) && lastAccelerometerTimestampUs != 0) {
                  ++lateAcceleromerterCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestAccelGap) {
                  largestAccelGap = timeGap;
                }
                lastAccelerometerTimestampUs = currentTimestamp;
                accelerometerRead = true;
                // accelerometer_updated = true;
                // accelerometer_count += 1;
                break;

            case SH2_GYROSCOPE_CALIBRATED:
                gyroData.x() = sensorValue.un.gyroscope.x;
                gyroData.y() = sensorValue.un.gyroscope.y;
                gyroData.z() = sensorValue.un.gyroscope.z;
                gyroData.accuracy() = sensorValue.status;
                gyroReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastGyroTimestampUs;
                if (timeGap > (13 * 1000) && lastGyroTimestampUs != 0) {
                  ++lateGyroCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestGyroGap) {
                  largestGyroGap = timeGap;
                }
                lastGyroTimestampUs = currentTimestamp;
                gyroRead = true;
                // gyro_updated = true;
                // gyro_count += 1;
                break;

            case SH2_GYROSCOPE_UNCALIBRATED:
                gyroData.x() = sensorValue.un.gyroscope.x;
                gyroData.y() = sensorValue.un.gyroscope.y;
                gyroData.z() = sensorValue.un.gyroscope.z;
                gyroData.accuracy() = sensorValue.status;
                gyroReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastGyroTimestampUs;
                if (timeGap > (13 * 1000) && lastGyroTimestampUs != 0) {
                  ++lateGyroCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestGyroGap) {
                  largestGyroGap = timeGap;
                }
                lastGyroTimestampUs = currentTimestamp;
                gyroRead = true;
                // gyro_updated = true;
                // gyro_count += 1;
                break;

            case SH2_MAGNETIC_FIELD_CALIBRATED:
                magnetometerData.x() = sensorValue.un.magneticField.x;
                magnetometerData.y() = sensorValue.un.magneticField.y;
                magnetometerData.z() = sensorValue.un.magneticField.z;
                magnetometerData.accuracy() = sensorValue.status;
                magnetometerReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastMagnetometerTimestampUs;
                if (timeGap > (13 * 1000) && lastMagnetometerTimestampUs != 0) {
                  ++lateMagnetometerCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestMagnoGap) {
                  largestMagnoGap = timeGap;
                }
                lastMagnetometerTimestampUs = currentTimestamp;
                magnometerRead = true;
                // magnometer_updated = true;
                // magnometer_count += 1;
                break;

            case SH2_ROTATION_VECTOR:
                rotationVectorData.real() = sensorValue.un.rotationVector.real;
                rotationVectorData.i() = sensorValue.un.rotationVector.i;
                rotationVectorData.j() = sensorValue.un.rotationVector.j;
                rotationVectorData.k() = sensorValue.un.rotationVector.k;
                rotationVectorData.accuracy() = sensorValue.status;
                rotationReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastRotationTimestampUs;
                if (timeGap > (13 * 1000) && lastRotationTimestampUs != 0) {
                  ++lateRotationCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestRotationGap) {
                  largestRotationGap = timeGap;
                }
                lastRotationTimestampUs = currentTimestamp;
                rotationRead = true;
                // rotation_updated = true;
                // rotation_count += 1;
                break;

            case SH2_GEOMAGNETIC_ROTATION_VECTOR:
                rotationVectorData.real() = sensorValue.un.rotationVector.real;
                rotationVectorData.i() = sensorValue.un.rotationVector.i;
                rotationVectorData.j() = sensorValue.un.rotationVector.j;
                rotationVectorData.k() = sensorValue.un.rotationVector.k;
                rotationVectorData.accuracy() = sensorValue.status;
                rotationReads += 1;
                currentTimestamp = esp_timer_get_time();
                timeGap = currentTimestamp - lastRotationTimestampUs;
                if (timeGap > (13 * 1000) && lastRotationTimestampUs != 0) {
                  ++lateRotationCount;
                  digitalWrite(12, HIGH);
                }
                if (timeGap > largestAccelGap) {
                  largestAccelGap = timeGap;
                }
                lastRotationTimestampUs = currentTimestamp;
                rotationRead = true;
                // rotation_updated = true;
                // rotation_count += 1;
                break;


        }
            // else {
            //     DEBUG_SERIAL.printf("Accelerometer: %d, Gyro: %d, Magno: %d, Rotation: %d, Avr: %d, Other Gyro: %d, Other: %d\n", accelerometer_count, gyro_count, magnometer_count, rotation_count, arvr_count, gyro_other_count, other_count);
            // }
        digitalWrite(13, LOW);
    }

    void update_loop2() {
      static const uint64_t timeBetweenEachLoopUs = 500; // 1ms

        uint64_t current_timestamp = esp_timer_get_time();
        if (current_timestamp - loopLastTimestamp < timeBetweenEachLoopUs) {
          return;
        }
        if (current_timestamp - samplePollLastTimestamp >= 10 * 1000) {
          samplePollLastTimestamp = current_timestamp;
          ++loopIteration;
          if (loopIteration % 1000 == 0) {
            Serial.printf("Late Counts");
            int total = 0;
            int dropped = 0;
            int max_dropped = 0;
            int current_dropped = 0;
            while (true) {
              const auto sample_maybe = accelerometerBuffer.tryGetNext();
              if (!sample_maybe) {
                break;
              }

              ++total;
              if (!sample_maybe->dataMaybe.has_value()) {
                ++dropped;
                ++current_dropped;
              } else {
                if (current_dropped > max_dropped) {
                  max_dropped = current_dropped;
                }
                current_dropped = 0;
              }
            }
            DEBUG_SERIAL.printf(", Accel: %d/%d (%d)", dropped, total, max_dropped);

            total = 0;
            dropped = 0;
            max_dropped = 0;
            current_dropped = 0;
            while (true) {
              const auto sample_maybe = gyroscopeBuffer.tryGetNext();
              if (!sample_maybe) {
                break;
              }

              ++total;
              if (!sample_maybe->dataMaybe.has_value()) {
                ++dropped;
                ++current_dropped;
              } else {
                if (current_dropped > max_dropped) {
                  max_dropped = current_dropped;
                }
                current_dropped = 0;
              }
            }
            DEBUG_SERIAL.printf(", Gyro: %d/%d (%d)", dropped, total, max_dropped);

            total = 0;
            dropped = 0;
            max_dropped = 0;
            current_dropped = 0;
            while (true) {
              const auto sample_maybe = rotationBuffer.tryGetNext();
              if (!sample_maybe) {
                break;
              }

              ++total;
              if (!sample_maybe->dataMaybe.has_value()) {
                ++dropped;
                ++current_dropped;
              } else {
                if (current_dropped > max_dropped) {
                  max_dropped = current_dropped;
                }
                current_dropped = 0;
              }
            }
            DEBUG_SERIAL.printf(", Rotation: %d/%d (%d)", dropped, total, max_dropped);
            DEBUG_SERIAL.printf("\n");
            lateAcceleromerterCount = 0;
            lateGyroCount = 0;
            lateMagnetometerCount = 0;
            lateRotationCount = 0;
            accelerometerRead = false;
            gyroRead = false;
            magnometerRead = false;
            rotationRead = false;
            accelerometerReads = 0;
            gyroReads = 0;
            magnetometerReads = 0;
            rotationReads = 0;
            largestAccelGap = 0;
            largestGyroGap = 0;
            largestMagnoGap = 0;
            largestRotationGap = 0;
            digitalWrite(12, LOW);
          }
        }
        loopLastTimestamp = current_timestamp;
        update3();
    }

    void update2() {
      const uint64_t maxTimeBetweenUpdatesUs = 10 * 1000; // 10ms
        if (bno08x.wasReset()) {
            DEBUG_SERIAL.print("sensor was reset \n");
            setupReports(bno08x, reportIntervalUs);
        }


        digitalWrite(13, HIGH);
        if (!bno08x.getSensorEvent(&sensorValue)) {
            DEBUG_SERIAL.printf("There was an error!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        }
        uint64_t currentTimestamp{};
        uint64_t timeGap{};
        // in this demo only one report type will be received depending on FAST_MODE define (above)
        //DEBUG_SERIAL.printf("Starting\n");
        switch (sensorValue.sensorId) {
            case SH2_ACCELEROMETER: {
                SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::ThreeDimensional();
                data_point.dataMaybe.value().x = sensorValue.un.accelerometer.x;
                data_point.dataMaybe.value().y = sensorValue.un.accelerometer.y;
                data_point.dataMaybe.value().z = sensorValue.un.accelerometer.z;
                accelerometerBuffer.push(data_point);
                accelerometerRead = true;
                break;
              }

            case SH2_RAW_ACCELEROMETER: {
                SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::ThreeDimensional();
                data_point.dataMaybe.value().x = sensorValue.un.accelerometer.x;
                data_point.dataMaybe.value().y = sensorValue.un.accelerometer.y;
                data_point.dataMaybe.value().z = sensorValue.un.accelerometer.z;
                accelerometerBuffer.push(data_point);
                accelerometerRead = true;
                break;
              }

            case SH2_LINEAR_ACCELERATION: {
                SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::ThreeDimensional();
                data_point.dataMaybe.value().x = sensorValue.un.linearAcceleration.x;
                data_point.dataMaybe.value().y = sensorValue.un.linearAcceleration.y;
                data_point.dataMaybe.value().z = sensorValue.un.linearAcceleration.z;
                accelerometerBuffer.push(data_point);
                accelerometerRead = true;
                break;
              }

            case SH2_GYROSCOPE_CALIBRATED: {
                SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::ThreeDimensional();
                data_point.dataMaybe.value().x = sensorValue.un.gyroscope.x;
                data_point.dataMaybe.value().y = sensorValue.un.gyroscope.y;
                data_point.dataMaybe.value().z = sensorValue.un.gyroscope.z;
                gyroscopeBuffer.push(data_point);
                gyroRead = true;
                break;
              }

            case SH2_GYROSCOPE_UNCALIBRATED: {
                SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::ThreeDimensional();
                data_point.dataMaybe.value().x = sensorValue.un.gyroscope.x;
                data_point.dataMaybe.value().y = sensorValue.un.gyroscope.y;
                data_point.dataMaybe.value().z = sensorValue.un.gyroscope.z;
                gyroscopeBuffer.push(data_point);
                gyroRead = true;
                break;
              }


            case SH2_ROTATION_VECTOR: {
                SensorDataPoint<Bno08xEvent::FourDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::FourDimensional();
                data_point.dataMaybe.value().real = sensorValue.un.rotationVector.real;
                data_point.dataMaybe.value().i = sensorValue.un.rotationVector.i;
                data_point.dataMaybe.value().j = sensorValue.un.rotationVector.j;
                data_point.dataMaybe.value().k = sensorValue.un.rotationVector.k;
                rotationBuffer.push(data_point);
                rotationRead = true;
                break;
              }

            case SH2_GEOMAGNETIC_ROTATION_VECTOR: {
                SensorDataPoint<Bno08xEvent::FourDimensional> data_point{};
                currentTimestamp = esp_timer_get_time();
                data_point.timestamp = currentTimestamp;
                data_point.calibration = sensorValue.status;
                data_point.dataMaybe = Bno08xEvent::FourDimensional();
                data_point.dataMaybe.value().real = sensorValue.un.rotationVector.real;
                data_point.dataMaybe.value().i = sensorValue.un.rotationVector.i;
                data_point.dataMaybe.value().j = sensorValue.un.rotationVector.j;
                data_point.dataMaybe.value().k = sensorValue.un.rotationVector.k;
                rotationBuffer.push(data_point);
                rotationRead = true;
                break;
              }


        }
        //DEBUG_SERIAL.printf("Collected\n");
            // else {
            //     DEBUG_SERIAL.printf("Accelerometer: %d, Gyro: %d, Magno: %d, Rotation: %d, Avr: %d, Other Gyro: %d, Other: %d\n", accelerometer_count, gyro_count, magnometer_count, rotation_count, arvr_count, gyro_other_count, other_count);
            // }
        digitalWrite(13, LOW);
    }

    void update3() {
      bool was_successful;
      const auto event = bno08x_device.get_event(was_successful);
      if (!was_successful) {
        // DEBUG_SERIAL.printf("There was an error!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        return;
      } else {
        // DEBUG_SERIAL.printf("All good\n");
      }
      uint64_t currentTimestamp{};
      bool calibrationUpdated = false;
      switch (event.event_type) {
          case Bno08xEvent::EventType::Accelerometer:
          case Bno08xEvent::EventType::RawAccelerometer:
          case Bno08xEvent::EventType::LinearAcceleration: {
            SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
            currentTimestamp = getTimeNowAsUs();
            data_point.timestamp = currentTimestamp;
            data_point.calibration = event.accuracy;
            data_point.dataMaybe = event.data.three_dimensional;
            // DEBUG_SERIAL.printf("Accelerometer\n");
            accelerometerBuffer.push(data_point);
            accelerometerCalibration = sanitizeCalibration(event.accuracy);
            hasAccelerometerCalibration = true;
            calibrationUpdated = true;
            break;
          }

          case Bno08xEvent::EventType::GyroscopeCalibrated:
          case Bno08xEvent::EventType::GyroscopeUncalibrated: {
            SensorDataPoint<Bno08xEvent::ThreeDimensional> data_point{};
            currentTimestamp = getTimeNowAsUs();
            data_point.timestamp = currentTimestamp;
            data_point.calibration = event.accuracy;
            data_point.dataMaybe = event.data.three_dimensional;
            // DEBUG_SERIAL.printf("Gyro\n");
            gyroscopeBuffer.push(data_point);
            gyroscopeCalibration = sanitizeCalibration(event.accuracy);
            hasGyroscopeCalibration = true;
            calibrationUpdated = true;
            break;
          }

          case Bno08xEvent::EventType::MagneticFieldCalibrated: {

            break;
          }

          case Bno08xEvent::EventType::RotationVector:
          case Bno08xEvent::EventType::GeomagneticRotationVector: {
            SensorDataPoint<Bno08xEvent::FourDimensional> data_point{};
            currentTimestamp = getTimeNowAsUs();
            data_point.timestamp = currentTimestamp;
            data_point.calibration = event.accuracy;
            data_point.dataMaybe = event.data.four_dimensional;
            // DEBUG_SERIAL.printf("Rotation\n");
            rotationBuffer.push(data_point);
            rotationCalibration = sanitizeCalibration(event.accuracy);
            hasRotationCalibration = true;
            calibrationUpdated = true;
            break;
            }

          default:
            break;
      }
      if (calibrationUpdated) {
        updateCalibrationLed();
      }
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



    // Calibration operations, forwarded to the sensor. Call only from the task
    // that drives update3() — these issue SH2 hub commands over the same bus.
    int saveCalibrationNow() { return bno08x_device.saveCalibrationNow(); }
    int getCalibrationConfig(uint8_t& mask) {
        return bno08x_device.getCalibrationConfig(mask);
    }
    int setCalibrationConfig(uint8_t mask) {
        return bno08x_device.setCalibrationConfig(mask);
    }

    bool getImuData(ImuData* data) {
        constexpr uint8_t accelerometer_bit = 0b100;
        constexpr uint8_t gyroscopt_bit = 0b010;
        constexpr uint8_t rotation_bit = 0b001;
        bool hasAnyFreshSample = false;

        while (true) {
            auto accelerometer_sample_maybe = accelerometerBuffer.tryGetNext();
            if (accelerometer_sample_maybe == nullptr) {
                break;
            }
            latestAccelerometerSample = *accelerometer_sample_maybe;
            hasLatestAccelerometerSample = accelerometer_sample_maybe->dataMaybe.has_value();
            hasAnyFreshSample = true;
        }

        while (true) {
            auto gyroscope_sample_maybe = gyroscopeBuffer.tryGetNext();
            if (gyroscope_sample_maybe == nullptr) {
                break;
            }
            latestGyroscopeSample = *gyroscope_sample_maybe;
            hasLatestGyroscopeSample = gyroscope_sample_maybe->dataMaybe.has_value();
            hasAnyFreshSample = true;
        }

        while (true) {
            auto rotation_sample_maybe = rotationBuffer.tryGetNext();
            if (rotation_sample_maybe == nullptr) {
                break;
            }
            latestRotationSample = *rotation_sample_maybe;
            hasLatestRotationSample = rotation_sample_maybe->dataMaybe.has_value();
            hasAnyFreshSample = true;
        }

        if (!hasAnyFreshSample) {
            return false;
        }

        data->accuracies = 0;
        data->has_data = 0;

        uint64_t max_timestamp = 0;
        if (hasLatestAccelerometerSample) {
            data->accuracies |= static_cast<uint8_t>(latestAccelerometerSample.calibration) << 4;
            data->has_data |= accelerometer_bit;
            data->data[0] = latestAccelerometerSample.dataMaybe.value().x;
            data->data[1] = latestAccelerometerSample.dataMaybe.value().y;
            data->data[2] = latestAccelerometerSample.dataMaybe.value().z;
            if (latestAccelerometerSample.timestamp > max_timestamp) {
                max_timestamp = latestAccelerometerSample.timestamp;
            }
        }
        if (hasLatestGyroscopeSample) {
            data->accuracies |= static_cast<uint8_t>(latestGyroscopeSample.calibration) << 2;
            data->has_data |= gyroscopt_bit;
            data->data[3] = latestGyroscopeSample.dataMaybe.value().x;
            data->data[4] = latestGyroscopeSample.dataMaybe.value().y;
            data->data[5] = latestGyroscopeSample.dataMaybe.value().z;
            if (latestGyroscopeSample.timestamp > max_timestamp) {
                max_timestamp = latestGyroscopeSample.timestamp;
            }
        }
        if (hasLatestRotationSample) {
            data->accuracies |= static_cast<uint8_t>(latestRotationSample.calibration);
            data->has_data |= rotation_bit;
            data->data[6] = latestRotationSample.dataMaybe.value().real;
            data->data[7] = latestRotationSample.dataMaybe.value().i;
            data->data[8] = latestRotationSample.dataMaybe.value().j;
            data->data[9] = latestRotationSample.dataMaybe.value().k;
            if (latestRotationSample.timestamp > max_timestamp) {
                max_timestamp = latestRotationSample.timestamp;
            }
        }

        if (max_timestamp == 0) {
            return false;
        } else {
            // Convert from microseconds to milliseconds to match frontend marker timestamps
            data->timestamp = max_timestamp / 1000;
            return true;
        }
    }
};
