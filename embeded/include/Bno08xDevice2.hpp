#pragma once

#include <BoardConfig.hpp>

#include <macros.hpp>

#define NAT_USE_FREE_RTOS_THREADS
// #define NAT_SIMULATE_BNO08X

#ifdef NAT_SIMULATE_BNO08X

#include <chrono>
#include <random>
#include <CircularBuffer.hpp>
#ifdef NAT_USE_FREE_RTOS_THREADS
#else
#include <thread>
#endif // NAT_USE_FREE_RTOS_THREADS

#define NAT_BNO08X_BUFFER_SIZE 128

#else
#include <Adafruit_BNO08x.h>

// SPI pins for the BNO08x live in BoardConfig.hpp.

#endif // NAT_SIMULATE_BNO08X


// #define NAT_BNO08X_FAST_MODE
#define NAT_BNO08X_ENABLE_ACCELEROMETER
// #define NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
// #define NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
// #define NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#define NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
// #define NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#define NAT_BNO08X_ENABLE_ROTATION_VECTOR
// #define NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR


#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
#define NAT_BNO08X_ENABLE_ACCELEROMETER_VAL 1
#else
#define NAT_BNO08X_ENABLE_ACCELEROMETER_VAL 0
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#define NAT_BNO08X_ENABLE_RAW_ACCELEROMETER_VAL 1
#else
#define NAT_BNO08X_ENABLE_RAW_ACCELEROMETER_VAL 0
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#define NAT_BNO08X_ENABLE_LINEAR_ACCELERATION_VAL 1
#else
#define NAT_BNO08X_ENABLE_LINEAR_ACCELERATION_VAL 0
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#define NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED_VAL 1
#else
#define NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED_VAL 0
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#define NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED_VAL 1
#else
#define NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED_VAL 0
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#define NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED_VAL 1
#else
#define NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED_VAL 0
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
#define NAT_BNO08X_ENABLE_ROTATION_VECTOR_VAL 1
#else
#define NAT_BNO08X_ENABLE_ROTATION_VECTOR_VAL 0
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
#define NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR_VAL 1
#else
#define NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR_VAL 0
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

#define NAT_BNO08X_NUMBER_OF_ENABLED_REPORTS (\
    NAT_BNO08X_ENABLE_ACCELEROMETER_VAL +\
    NAT_BNO08X_ENABLE_RAW_ACCELEROMETER_VAL +\
    NAT_BNO08X_ENABLE_LINEAR_ACCELERATION_VAL +\
    NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED_VAL +\
    NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED_VAL +\
    NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED_VAL +\
    NAT_BNO08X_ENABLE_ROTATION_VECTOR_VAL +\
    NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR_VAL)



#ifdef NAT_BNO08X_FAST_MODE
// Set polling rate to 100 Hz
#define NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US 9000
#define NAT_BNO08X_DELAY_BETWEEN_SAMPLES_MS (NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US / 1000)
#define NAT_BNO08X_DELAY_BETWEEN_SAMPLES_TICK (NAT_BNO08X_DELAY_BETWEEN_SAMPLES_MS / portTICK_PERIOD_MS)
#else
// Set polling rate to 50 Hz
#define NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US 18000
#define NAT_BNO08X_DELAY_BETWEEN_SAMPLES_MS (NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US / 1000)
#define NAT_BNO08X_DELAY_BETWEEN_SAMPLES_TICK (NAT_BNO08X_DELAY_BETWEEN_SAMPLES_MS / portTICK_PERIOD_MS)
#endif // NAT_BNO08X_FAST_MODE


struct Bno08xEvent {
    enum class EventType {
        Accelerometer = 1,
        RawAccelerometer,
        LinearAcceleration,
        GyroscopeUncalibrated,
        GyroscopeCalibrated,
        MagneticFieldCalibrated,
        RotationVector,
        GeomagneticRotationVector,
    };
    struct ThreeDimensional {
        float x;
        float y;
        float z;
    };
    struct FourDimensional {
        float real;
        float i;
        float j;
        float k;
    };

    union Data {
        FourDimensional four_dimensional;
        ThreeDimensional three_dimensional;
    };
    Data data;
    uint8_t accuracy;
    EventType event_type;
};

class Bno08xDevice {

#ifdef NAT_SIMULATE_BNO08X
    Bno08xEvent::EventType event_types[NAT_BNO08X_NUMBER_OF_ENABLED_REPORTS];
#else
    SPIClass spiClass{};
    Adafruit_BNO08x bno08x{BNO08X_RESET};
    // Zeroed: getSensorEvent() decides "no new event" by checking timestamp == 0
    // AND sensorId != SH2_GYRO_INTEGRATED_RV, so an uninitialised sensorId that
    // happened to equal SH2_GYRO_INTEGRATED_RV would report a garbage first event.
    sh2_SensorValue_t sensorValue{};
#endif // NAT_SIMULATE_BNO08X

    std::string setup(bool& was_successful);

public:
    Bno08xDevice() {}

    std::string start(bool& was_successful);
    Bno08xEvent get_event(bool& was_successful);

    // --- calibration operations, driven by the EXECUTION_COMMAND channel -----
    // These talk to the SH2 hub, so they MUST be called from the task that owns
    // the sensor (the networking/IMU task). Each returns an SH2 status code:
    // SH2_OK (0) on success, negative on failure.

    // Persists the current dynamic calibration (DCD) to flash immediately.
    // Per the BNO080/085 datasheet §3.4 the hub only writes DCD to FRS on a
    // non-power-up reset, so a device that is simply powered off loses whatever
    // it learned since boot. This is the explicit "save it now" the datasheet
    // provides for exactly that case.
    int saveCalibrationNow() {
        #ifdef NAT_SIMULATE_BNO08X
        return 0;
        #else
        return sh2_saveDcdNow();
        #endif // NAT_SIMULATE_BNO08X
    }

    // Reads the hub's dynamic-calibration mask. NOTE: on this hub the read-back
    // is not faithful (see the note in setup()) — it reports 0x05 regardless of
    // what was written — so treat this as diagnostic only.
    int getCalibrationConfig(uint8_t& mask) {
        #ifdef NAT_SIMULATE_BNO08X
        mask = 0x07;
        return 0;
        #else
        return sh2_getCalConfig(&mask);
        #endif // NAT_SIMULATE_BNO08X
    }
};


#ifdef NAT_SIMULATE_BNO08X

template <typename T>
struct ThreadBundle {
    T buffer{};
    std::seed_seq seed;
    std::mt19937 engine;
    std::uniform_real_distribution<double> distribution;

    ThreadBundle(int thread_id)
        : seed{static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()), static_cast<unsigned int>(thread_id)} {
        engine = std::mt19937(seed);
        distribution = std::uniform_real_distribution<double>(0.0, 100.0);
    }
};

#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
static ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>> accelerometer_thread_bundle(0);
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
static ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>> raw_accelerometer_thread_bundle(1);
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
static ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>> linear_acceleration_thread_bundle(2);
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
static ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>> gyroscope_uncalibrated_thread_bundle(3);
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
static ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>> gyroscope_calibrated_thread_bundle(4);
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
static ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>> magnetic_field_calibrated_thread_bundle(5);
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
static ThreadBundle<CircularBuffer<Bno08xEvent::FourDimensional, NAT_BNO08X_BUFFER_SIZE>> rotation_vector_thread_bundle(6);
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
static ThreadBundle<CircularBuffer<Bno08xEvent::FourDimensional>> geomagnetic_rotation_vector_thread_bundle(7);
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

#ifdef NAT_USE_FREE_RTOS_THREADS

#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
static TaskHandle_t accelerometer_thread = NULL;
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
static TaskHandle_t raw_accelerometer_thread = NULL;
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
static TaskHandle_t linear_acceleration_thread = NULL;
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
static TaskHandle_t gyroscope_uncalibrated_thread = NULL;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
static TaskHandle_t gyroscope_calibrated_thread = NULL;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
static TaskHandle_t magnetic_field_calibrated_thread = NULL;
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
static TaskHandle_t rotation_vector_thread = NULL;
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
static TaskHandle_t geomagnetic_rotation_thread = NULL;
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

#else

#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
static std::thread accelerometer_thread;
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
static std::thread raw_accelerometer_thread;
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
static std::thread linear_acceleration_thread;
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
static std::thread gyroscope_uncalibrated_thread;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
static std::thread gyroscope_calibrated_thread;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
static std::thread magnetic_field_calibrated_thread;
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
static std::thread rotation_vector_thread;
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
static std::thread geomagnetic_rotation_thread;
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

#endif // NAT_USE_FREE_RTOS_THREADS


std::string Bno08xDevice::setup(bool& was_successful) {
    was_successful = true;

    uint8_t index = 0;
#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
    event_types[index++] = Bno08xEvent::EventType::Accelerometer;
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
    event_types[index++] = Bno08xEvent::EventType::RawAccelerometer;
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
    event_types[index++] = Bno08xEvent::EventType::LinearAcceleration;
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
    event_types[index++] = Bno08xEvent::EventType::GyroscopeUncalibrated;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
    event_types[index++] = Bno08xEvent::EventType::GyroscopeCalibrated;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
    event_types[index++] = Bno08xEvent::EventType::MagneticFieldCalibrated;
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
    event_types[index++] = Bno08xEvent::EventType::RotationVector;
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
    event_types[index++] = Bno08xEvent::EventType::GeomagneticRotationVector;
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

    return "";
}

static float rand_float() {
    int integral_portion = rand();
    int fractional_portion = rand() % 1000;
    return static_cast<float>(integral_portion) + (static_cast<float>(fractional_portion) / 1000);
}

static void simulate_bno08x_3d(void* opaque_thread_bundle) {
    ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>>* thread_bundle = (ThreadBundle<CircularBuffer<Bno08xEvent::ThreeDimensional, NAT_BNO08X_BUFFER_SIZE>>*)opaque_thread_bundle;
    while (1) {
        Bno08xEvent::ThreeDimensional data;
        data.x = thread_bundle->distribution(thread_bundle->engine);
        data.y = thread_bundle->distribution(thread_bundle->engine);
        data.z = thread_bundle->distribution(thread_bundle->engine);
        thread_bundle->buffer.push(data);
#ifdef NAT_USE_FREE_RTOS_THREADS
        vTaskDelay(NAT_BNO08X_DELAY_BETWEEN_SAMPLES_TICK);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US));
#endif // NAT_USE_FREE_RTOS_THREADS
    }

#ifdef NAT_USE_FREE_RTOS_THREADS
    vTaskDelete(NULL);
#endif // NAT_USE_FREE_RTOS_THREADS
}

static void simulate_bno08x_4d(void* opaque_thread_bundle) {
    ThreadBundle<CircularBuffer<Bno08xEvent::FourDimensional, NAT_BNO08X_BUFFER_SIZE>>* thread_bundle = (ThreadBundle<CircularBuffer<Bno08xEvent::FourDimensional, NAT_BNO08X_BUFFER_SIZE>>*)opaque_thread_bundle;
    while (1) {
    Bno08xEvent::FourDimensional data;
        data.real = thread_bundle->distribution(thread_bundle->engine);
        data.i = thread_bundle->distribution(thread_bundle->engine);
        data.j = thread_bundle->distribution(thread_bundle->engine);
        data.k = thread_bundle->distribution(thread_bundle->engine);
        thread_bundle->buffer.push(data);
#ifdef NAT_USE_FREE_RTOS_THREADS
        vTaskDelay(NAT_BNO08X_DELAY_BETWEEN_SAMPLES_TICK);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US));
#endif // NAT_USE_FREE_RTOS_THREADS
    }

#ifdef NAT_USE_FREE_RTOS_THREADS
    vTaskDelete(NULL);
#endif // NAT_USE_FREE_RTOS_THREADS
}

std::string Bno08xDevice::start(bool& was_successful) {
    was_successful = true;

#ifdef NAT_USE_FREE_RTOS_THREADS

#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
    xTaskCreate(simulate_bno08x_3d, "simulate_accelerometer_task", 1024, &accelerometer_thread_bundle, tskIDLE_PRIORITY + 1, &accelerometer_thread);
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
    xTaskCreate(simulate_bno08x_3d, "simulate_raw_accelerometer_task", 1024, &raw_accelerometer_thread_bundle, tskIDLE_PRIORITY + 1, &raw_accelerometer_thread);
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
    xTaskCreate(simulate_bno08x_3d, "simulate_linear_acceleration_task", 1024, &linear_acceleration_thread_bundle, tskIDLE_PRIORITY + 1, &linear_acceleration_thread);
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
    xTaskCreate(simulate_bno08x_3d, "simulate_gyroscope_uncalibrated_task", 1024, &gyroscope_uncalibrated_thread_bundle, tskIDLE_PRIORITY + 1, &gyroscope_uncalibrated_thread);
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
    xTaskCreate(simulate_bno08x_3d, "simulate_gyroscope_calibrated_task", 1024, &gyroscope_calibrated_thread_bundle, tskIDLE_PRIORITY + 1, &gyroscope_calibrated_thread);
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
    xTaskCreate(simulate_bno08x_3d, "simulate_magnetic_field_calibrated_task", 1024, &magnetic_field_calibrated_thread_bundle, tskIDLE_PRIORITY + 1, &magnetic_field_calibrated_thread);
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
    xTaskCreate(simulate_bno08x_4d, "simulate_rotation_vector_task", 1024, &rotation_vector_thread_bundle, tskIDLE_PRIORITY + 1, &rotation_vector_thread);
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
    xTaskCreate(simulate_bno08x_4d, "simulate_geomagnetic_rotation_vector_task", 1024, &geomagnetic_rotation_vector_thread_bundle, tskIDLE_PRIORITY + 1, &geomagnetic_rotation_thread);
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

#else

#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
  accelerometer_thread = std::thread(simulate_bno08x_3d, &accelerometer_thread_bundle);
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
  raw_accelerometer_thread = std::thread(simulate_bno08x_3d, &raw_accelerometer_thread_bundle);
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
  linear_acceleration_thread = std::thread(simulate_bno08x_3d, &linear_acceleration_thread_bundle);
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
  gyrosocpe_uncalibrated_thread = std::thread(simulate_bno08x_3d, &gyroscope_uncalibrated_thread_bundle);
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
  gyroscope_calibrated_thread = std::thread(simulate_bno08x_3d, &gyroscope_calibrated_thread_bundle);
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
  magnetic_field_calibrated_thread = std::thread(simulate_bno08x_3d, &magnetic_field_calibrated_thread_bundle);
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
  rotation_thread = std::thread(simulate_bno08x_4d, &rotation_thread_bundle);
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
  geomagnetic_rotation_thread = std::thread(simulate_bno08x_4d, &goemagnetic_rotation_thread_bundle);
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

#endif // NAT_USE_FREE_RTOS_THREADS

  return setup(was_successful);
}

Bno08xEvent Bno08xDevice::get_event(bool& was_successful) {
    was_successful = true;

    Bno08xEvent::EventType event_type = event_types[rand() % NAT_BNO08X_NUMBER_OF_ENABLED_REPORTS]; // TODO: Maybe use round robin instead of rand()
    Bno08xEvent event{};
    event.event_type = event_type;
    event.accuracy = rand() % 3;

    switch (event_type) {
        case Bno08xEvent::EventType::Accelerometer: {
#ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
            auto data_maybe = accelerometer_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.three_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_ACCELEROMETER
            break;
        }

        case Bno08xEvent::EventType::RawAccelerometer: {
#ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
            auto data_maybe = raw_accelerometer_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.three_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
            break;
        }

        case Bno08xEvent::EventType::LinearAcceleration: {
#ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
            auto data_maybe = linear_acceleration_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.three_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
            break;
        }

        case Bno08xEvent::EventType::GyroscopeCalibrated: {
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
            auto data_maybe = gyroscope_calibrated_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.three_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
            break;
        }

        case Bno08xEvent::EventType::GyroscopeUncalibrated: {
#ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
            auto data_maybe = gyroscope_uncalibrated_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.three_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
            break;
        }

        case Bno08xEvent::EventType::MagneticFieldCalibrated: {
#ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
            auto data_maybe = magnetic_field_calibrated_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.three_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
            break;
        }

        case Bno08xEvent::EventType::RotationVector: {
#ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
            auto data_maybe = rotation_vector_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.four_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR
            break;
        }

        case Bno08xEvent::EventType::GeomagneticRotationVector: {
#ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
            auto data_maybe = geomagnetic_rotation_vector_thread_bundle.buffer.pop();
            if (data_maybe == nullptr) {
                was_successful = false;
            } else {
                event.data.four_dimensional = *data_maybe;
            }
#else
            was_successful = false;
#endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
            break;
        }

        default:
            was_successful = false;
    }

    return event;
}


#else

std::string Bno08xDevice::setup(bool& was_successful) {
    was_successful = true;

    std::string error_msg = "";

    // Adafruit_BNO08x keeps a single file-static sh2_SensorValue_t* that its SH2
    // sensor callback writes through, and it is NULL until the first
    // getSensorEvent() call assigns it. Every sh2 op (enableReport,
    // sh2_setCalConfig, ...) pumps SHTP while waiting for its reply, so as soon as
    // ONE report is enabled a queued sensor event can be delivered mid-op — the
    // callback then writes through that NULL and the chip panics with
    // StoreProhibited at address 0 inside sh2_decodeSensorEvent.
    //
    // Priming it here points that static at our long-lived member before anything
    // can arrive, which closes the window for every op below. &sensorValue must
    // outlive the library's use of it, hence a member and not a local.
    // The call returns false (nothing is enabled yet); we only want the side effect.
    //
    // NOTE: this priming is what makes the ordering below safe. An earlier version
    // of this fix ALSO moved the calibration setup ahead of the enableReport calls
    // "for good measure" -- but the hub rejects sh2_setCalConfig that early with
    // SH2_ERR_HUB (-5), which silently left gyro dynamic calibration disabled and
    // pinned reported accuracy at Unreliable. Calibration is configured at the END
    // of setup, where the hub accepts it.
    (void)bno08x.getSensorEvent(&sensorValue);

    // Calibration config sits here: after the priming getSensorEvent() above and
    // before the reports are enabled. This is the ordering VERIFIED streaming on
    // hardware.
    //
    // sh2_setCalConfig returns SH2_ERR_HUB (-5) here, and has done since Feb 2026
    // (71e3be8) when its return value was discarded. Do not "fix" that by moving it
    // to be the first hub command: measured on hardware, the call then succeeds but
    // the BNO08x stops producing sensor reports altogether ("Nothing to read"), so
    // nothing streams. The failure is currently harmless -- see below.
    //
    // sh2_getCalConfig is NOT a faithful read-back on this hub: probing every mask
    // (0x01, 0x02, 0x04, 0x03, 0x05, 0x07, 0x0f) as the first command showed all of
    // them accepted (return 0) while the read-back stayed 0x05 in every case --
    // including when only SH2_CAL_ACCEL was set. So the logged value below says
    // what the hub reports, not what is actually in effect, and it cannot be used
    // to confirm gyro dynamic calibration is on.
    //
    // Dynamic calibration for accel/gyro/mag, plus periodic saving of the
    // calibration data. These return codes are CHECKED and logged: discarding
    // them meant a silent failure here looked exactly like a sensor that would
    // not calibrate, with nothing on the console to say why.
    const uint8_t desired_cal =
        SH2_CAL_ACCEL | SH2_CAL_GYRO | SH2_CAL_MAG;
    const int cal_config_status = sh2_setCalConfig(desired_cal);
    if (cal_config_status != SH2_OK) {
        Serial.printf(
            "BNO08X: sh2_setCalConfig(0x%02x) returned %d (SH2_ERR_HUB) — "
            "long-standing and so far harmless; see the note above.\n",
            desired_cal, cal_config_status);
    }
    const int dcd_status = sh2_setDcdAutoSave(true);
    if (dcd_status != SH2_OK) {
        Serial.printf(
            "BNO08X: sh2_setDcdAutoSave(true) FAILED with %d — calibration "
            "will not persist across power cycles.\n",
            dcd_status);
    }

    // Read back what the chip actually has enabled, rather than trusting that the
    // write took.
    uint8_t actual_cal = 0;
    const int read_back = sh2_getCalConfig(&actual_cal);
    if (read_back == SH2_OK) {
        Serial.printf(
            "BNO08X: dynamic calibration enabled = 0x%02x (accel=%d gyro=%d "
            "mag=%d)\n",
            actual_cal, (actual_cal & SH2_CAL_ACCEL) ? 1 : 0,
            (actual_cal & SH2_CAL_GYRO) ? 1 : 0,
            (actual_cal & SH2_CAL_MAG) ? 1 : 0);
    } else {
        Serial.printf("BNO08X: sh2_getCalConfig failed with %d\n", read_back);
    }

    #ifdef NAT_BNO08X_ENABLE_ACCELEROMETER
    if (!bno08x.enableReport(SH2_ACCELEROMETER, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable accelerometer\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_ACCELEROMETER

    #ifdef NAT_BNO08X_ENABLE_RAW_ACCELEROMETER
    if (!bno08x.enableReport(SH2_RAW_ACCELEROMETER, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable raw accelerometer\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_RAW_ACCELEROMETER

    #ifdef NAT_BNO08X_ENABLE_LINEAR_ACCELERATION
    if (!bno08x.enableReport(SH2_LINEAR_ACCELERATION, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable linear acceleation\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_LINEAR_ACCELERATION

    #ifdef NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED
    if (!bno08x.enableReport(SH2_GYROSCOPE_UNCALIBRATED, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable gyroscope uncalibrated\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_GYROSCOPE_UNCALIBRATED

    #ifdef NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED
    if (!bno08x.enableReport(SH2_GYROSCOPE_CALIBRATED, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable gyroscope calibration\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_GYROSCOPE_CALIBRATED

    #ifdef NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED
    if (!bno08x.enableReport(SH2_MAGNETIC_FIELD_CALIBRATED, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable magnetic field\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_MAGNETIC_FIELD_CALIBRATED

    #ifdef NAT_BNO08X_ENABLE_ROTATION_VECTOR
    if (!bno08x.enableReport(SH2_ROTATION_VECTOR, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable rotation vector\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_ROTATION_VECTOR

    #ifdef NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR
    if (!bno08x.enableReport(SH2_GEOMAGNETIC_ROTATION_VECTOR, NAT_BNO08X_DELAY_BETWEEN_SAMPLES_US)) {
        error_msg.append("Could not enable geomagnetic rotation vector\n");
        was_successful = false;
    }
    #endif // NAT_BNO08X_ENABLE_GEOMAGNETIC_ROTATION_VECTOR

    return error_msg;
}

std::string Bno08xDevice::start(bool& was_successful) {
    was_successful = true;
    std::string error_msg = "";
    const uint32_t max_retries = 100;
    uint32_t i = 0;
    spiClass.begin(BNO08X_SCK, BNO08X_MISO, BNO08X_MOSI);
    while (!bno08x.begin_SPI(BNO08X_CS, BNO08X_INT, &spiClass)) {
        if (i++ > max_retries) {
            was_successful = false;
            error_msg.append("Failed to start SPI on the BNO08X chip");
            return error_msg;
        }
        delay(100);
    }

    return setup(was_successful);
}

Bno08xEvent Bno08xDevice::get_event(bool& was_successful) {
    was_successful = true;

    Bno08xEvent event{};
    if (!bno08x.getSensorEvent(&sensorValue)) {
        was_successful = false;
        return event;
    }

    switch (sensorValue.sensorId) {
        case SH2_ACCELEROMETER: {
            event.event_type = Bno08xEvent::EventType::Accelerometer;
            event.accuracy = sensorValue.status;

            Bno08xEvent::ThreeDimensional data{};
            data.x = sensorValue.un.accelerometer.x;
            data.y = sensorValue.un.accelerometer.y;
            data.z = sensorValue.un.accelerometer.z;
            event.data.three_dimensional = data;

            break;
        }

        case SH2_RAW_ACCELEROMETER: {
            event.event_type = Bno08xEvent::EventType::RawAccelerometer;
            event.accuracy = sensorValue.status;

            Bno08xEvent::ThreeDimensional data{};
            data.x = sensorValue.un.rawAccelerometer.x;
            data.y = sensorValue.un.rawAccelerometer.y;
            data.z = sensorValue.un.rawAccelerometer.z;
            event.data.three_dimensional = data;
            
            break;
        }

        case SH2_LINEAR_ACCELERATION: {
            event.event_type = Bno08xEvent::EventType::LinearAcceleration;
            event.accuracy = sensorValue.status;

            Bno08xEvent::ThreeDimensional data{};
            data.x = sensorValue.un.linearAcceleration.x;
            data.y = sensorValue.un.linearAcceleration.y;
            data.z = sensorValue.un.linearAcceleration.z;
            event.data.three_dimensional = data;
            
            break;
        }

        case SH2_GYROSCOPE_CALIBRATED: {
            event.event_type = Bno08xEvent::EventType::GyroscopeCalibrated;
            event.accuracy = sensorValue.status;

            Bno08xEvent::ThreeDimensional data{};
            data.x = sensorValue.un.gyroscope.x;
            data.y = sensorValue.un.gyroscope.y;
            data.z = sensorValue.un.gyroscope.z;
            event.data.three_dimensional = data;
            
            break;
        }

        case SH2_GYROSCOPE_UNCALIBRATED: {
            event.event_type = Bno08xEvent::EventType::GyroscopeUncalibrated;
            event.accuracy = sensorValue.status;

            Bno08xEvent::ThreeDimensional data{};
            data.x = sensorValue.un.gyroscopeUncal.x;
            data.y = sensorValue.un.gyroscopeUncal.y;
            data.z = sensorValue.un.gyroscopeUncal.z;
            event.data.three_dimensional = data;
            
            break;
        }

        case SH2_MAGNETIC_FIELD_CALIBRATED: {
            event.event_type = Bno08xEvent::EventType::MagneticFieldCalibrated;
            event.accuracy = sensorValue.status;

            Bno08xEvent::ThreeDimensional data{};
            data.x = sensorValue.un.magneticField.x;
            data.y = sensorValue.un.magneticField.y;
            data.z = sensorValue.un.magneticField.z;
            event.data.three_dimensional = data;
            
            break;
        }

        case SH2_ROTATION_VECTOR: {
            event.event_type = Bno08xEvent::EventType::RotationVector;
            event.accuracy = sensorValue.status;

            Bno08xEvent::FourDimensional data{};
            data.real = sensorValue.un.rotationVector.real;
            data.i = sensorValue.un.rotationVector.i;
            data.j = sensorValue.un.rotationVector.j;
            data.k = sensorValue.un.rotationVector.k;
            event.data.four_dimensional = data;
            
            break;
        }

        case SH2_GEOMAGNETIC_ROTATION_VECTOR: {
            event.event_type = Bno08xEvent::EventType::GeomagneticRotationVector;
            event.accuracy = sensorValue.status;

            Bno08xEvent::FourDimensional data{};
            data.real = sensorValue.un.geoMagRotationVector.real;
            data.i = sensorValue.un.geoMagRotationVector.i;
            data.j = sensorValue.un.geoMagRotationVector.j;
            data.k = sensorValue.un.geoMagRotationVector.k;
            event.data.four_dimensional = data;
            
            break;
        }
    }

    return event;
}

#endif // NAT_SIMULATE_BNO08X
