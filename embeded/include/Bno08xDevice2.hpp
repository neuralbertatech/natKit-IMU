#pragma once

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
#endif // NAT_USE_FREE_RTOS_THREADS\

#define NAT_BNO08X_BUFFER_SIZE 128

#else
#include <Adafruit_SensorLab.h>
#include <Adafruit_BNO08x.h>

// Set the pins needed for SPI mode on the BNO08x chip
#define BNO08X_CS 15
#define BNO08X_INT 32
#define BNO08X_RESET 14
#define BNO08X_SCK 5
#define BNO08X_MISO 21
#define BNO08X_MOSI 19

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
    Adafruit_SensorLab sensorLab;
    sh2_SensorValue_t sensorValue;
#endif // NAT_SIMULATE_BNO08X

    std::string setup(bool& was_successful);

public:
    Bno08xDevice() {}

    std::string start(bool& was_successful);
    Bno08xEvent get_event(bool& was_successful);
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

    // Best-effort dynamic calibration and persistence setup.
    (void)sh2_setCalConfig(SH2_CAL_ACCEL | SH2_CAL_GYRO | SH2_CAL_MAG);
    (void)sh2_setDcdAutoSave(true);

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
