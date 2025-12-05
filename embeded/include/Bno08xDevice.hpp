#pragma once

#include <cstdint>
#include <CircularBuffer.hpp>

#ifdef NAT_BNO08X_SIMULATE_IMU

#include <thread>

#else

#endif // NAT_BNO08X_SIMULATE_IMU

#define NAT_IMU_DELAY_BETWEEN_SAMPLES 20000
#define NAT_ENABLE_ACELEROMETER
#define NAT_ENABLE_RAW_ACCELEROMETER
#define NAT_ENABLE_LINEAR_ACCELERATION
#define NAT_ENABLE_GYROSCOPE_CALIBRATED
#define NAT_ENABLE_GYROSCOPE_UNCALIBRATED
#define NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED
#define NAT_ENABLE_ROATION_VECTOR
#define NAT_ENABLE_GEOMAGNETIC_ROTION

#ifdef NAT_ENABLE_ACELEROMETER

#endif // NAT_ENABLE_ACELEROMETER
#ifdef NAT_ENABLE_RAW_ACCELEROMETER

#endif // NAT_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_ENABLE_LINEAR_ACCELERATION

#endif // NAT_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_ENABLE_GYROSCOPE_CALIBRATED

#endif // NAT_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_ENABLE_GYROSCOPE_UNCALIBRATED

#endif // NAT_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED

#endif // NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_ENABLE_ROATION_VECTOR

#endif // NAT_ENABLE_ROATION_VECTOR
#ifdef NAT_ENABLE_GEOMAGNETIC_ROTION

#endif // NAT_ENABLE_GEOMAGNETIC_ROTION



struct Bno08xEvent {
  enum class EventType {
    Accelerometer,
    RawAccelerometer,
    LinearAcceleration,
    GyroscopeCalibrated,
    GyroscopeUncalibrated,
    MagneticFieldCalibrated,
    RotationVector,
  };

  struct ThreeDimentional {
    float x;
    float y;
    float z;
  };

  struct FourDimentional {
    float real;
    float i;
    float j;
    float k;
  };

  union Data {
    ThreeDimentional threeDimentional;
    FourDimentional fourDimentional;
  };

  Data data;
  uint8_t accuracy;
  EventType eventType;
};

class Bno08xDevice {
  
public:
  Bno08xDevice() {}

  std::string start(bool& was_successful);
  Bno08xEvent try_get_event(bool& was_successful);
};


#ifdef NAT_BNO08X_SIMULATE_IMU

#ifdef NAT_ENABLE_ACELEROMETER
static std::thread accelerometer_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> accelerometer_buffer;
#endif // NAT_ENABLE_ACELEROMETER
#ifdef NAT_ENABLE_RAW_ACCELEROMETER
static std::thread raw_accelerometer_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> raw_accelerometer_buffer;
#endif // NAT_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_ENABLE_LINEAR_ACCELERATION
static std::thread linear_acceleration_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> linear_acceleration_buffer;
#endif // NAT_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_ENABLE_GYROSCOPE_CALIBRATED
static std::thread gyroscope_calibrated_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> gyroscope_calibrated_buffer;
#endif // NAT_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_ENABLE_GYROSCOPE_UNCALIBRATED
static std::thread gyroscope_uncalibrated_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> gyroscope_uncalibrated_buffer;
#endif // NAT_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED
static std::thread magnetic_field_calibrated_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> magnetic_field_calibrated_buffer;
#endif // NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_ENABLE_ROATION_VECTOR
static std::thread rotation_vector_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> accelerometer_buffer;
#endif // NAT_ENABLE_ROATION_VECTOR
#ifdef NAT_ENABLE_GEOMAGNETIC_ROTION
static std::thread geomagnetic_rotation_thread;
static CircularBuffer<Bno08xEvent::ThreeDimentional> accelerometer_buffer;
#endif // NAT_ENABLE_GEOMAGNETIC_RATION

static simulate_bno08x(CircularBuffer<Bno08xEvent::ThreeDimentional>* buffer) {
  while (1) {
    Bno08xEvent::ThreeDimentional data;
    data.x = rand();
    data.y = rand();
    data.z = rand();
    buffer.push(data);
    std::this_thread::sleep_for(std::chrono::microseconds(NAT_IMU_DELAY_BETWEEN_SAMPLES));
  }
}

std::string Bno08xDevice::start(bool& was_successful) {
  was_successful = true;

#ifdef NAT_ENABLE_ACELEROMETER
  accelerometer_thread = std::thread(simulate_bno08x, &accelerometer_buffer);
#endif // NAT_ENABLE_ACELEROMETER
#ifdef NAT_ENABLE_RAW_ACCELEROMETER
  raw_accelerometer_thread = std::thread(simulate_bno08x, &raw_accelerometer_buffer);
#endif // NAT_ENABLE_RAW_ACCELEROMETER
#ifdef NAT_ENABLE_LINEAR_ACCELERATION
  linear_acceleration_thread = std::thread(simulate_bno08x, &linear_acceleration_buffer);
#endif // NAT_ENABLE_LINEAR_ACCELERATION
#ifdef NAT_ENABLE_GYROSCOPE_CALIBRATED
  gyroscope_calibrated_thread = std::thread(simulate_bno08x, &gyroscope_calibrated_buffer);
#endif // NAT_ENABLE_GYROSCOPE_CALIBRATED
#ifdef NAT_ENABLE_GYROSCOPE_UNCALIBRATED
  gyrosocpe_uncalibrated_thread = std::thread(simulate_bno08x, &gyroscope_uncalibrated_buffer);
#endif // NAT_ENABLE_GYROSCOPE_UNCALIBRATED
#ifdef NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED
  magnetic_field_calibrated_thread = std::thread(simulate_bno08x, &magnetic_field_calibrated_buffer);
#endif // NAT_ENABLE_MAGNETIC_FIELD_CALIBRATED
#ifdef NAT_ENABLE_ROATION_VECTOR
  rotation_thread = std::thread(simulate_bno08x, &rotation_buffer);
#endif // NAT_ENABLE_ROATION_VECTOR
#ifdef NAT_ENABLE_GEOMAGNETIC_ROTION
  geomagnetic_rotation_thread = std::thread(simulate_bno08x, &goemagnetic_rotation_buffer);
#endif // NAT_ENABLE_GEOMAGNETIC_ROTION

  return "";
}

Bno08xEvent Bno08xDevice::try_get_event(bool& was_successful) {
  was_successful = true;
}


#else

#endif // NAT_BNO08X_SIMULATE_IMU


