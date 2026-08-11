#include "bno08x.hpp"

#include <cstring>

#include "board_config.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sh2.h"
#include "sh2_err.h"
#include "sh2_hal.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-bno08x";

// Bits 1:0 of the SH2 status byte are the accuracy (0..3); bits 7:2 are the
// report delay. Using the byte unmasked leaks the delay into the accuracy, and
// because accuracies are later packed two bits per sensor, a nonzero delay
// corrupts the NEIGHBOURING sensors' fields too. Found on hardware; the mask is
// not cosmetic.
constexpr uint8_t kStatusAccuracyMask = 0x03;

// SH2_CAL_ACCEL | SH2_CAL_GYRO | SH2_CAL_MAG. The hub's default is 0x05 (gyro
// OFF), which is the whole reason rotation never left Unreliable.
constexpr uint8_t kDesiredCalibration = 0x07;

constexpr uint64_t kCalibrationEnableAfterStreamingUs = 5'000'000;
constexpr uint64_t kCalibrationRetryUs = 5'000'000;
constexpr uint8_t kCalibrationMaxAttempts = 3;

// INT is active-low and open-drain. 500 polls at 1ms is the Adafruit HAL's own
// budget, kept because it is what has worked on this board; it also depends on a
// 1ms tick, hence CONFIG_FREERTOS_HZ=1000 in sdkconfig.defaults (a 10ms tick
// would stretch this to 5s and coarsen every SHTP exchange tenfold).
constexpr int kIntWaitPolls = 500;

spi_device_handle_t sSpi = nullptr;

// SHTP transport counters. A sensor bring-up fails in the transport far more
// often than in the decode, and "no reports" looks identical whether the hub is
// silent, the header is unreadable, or every packet is being dropped as
// oversized. These make those cases distinguishable from the console.
HalStats sStats{};

// The decode target for the sh2 sensor callback.
//
// Adafruit's stack kept a file-static POINTER here that its callback wrote
// through, and it was NULL until the first getSensorEvent() call assigned it --
// so a report arriving during any earlier sh2 op (each one pumps SHTP while it
// waits for its reply) wrote to address 0 and the chip died with StoreProhibited
// inside sh2_decodeSensorEvent. That whole failure mode is designed out here:
// the target is a real object with static storage duration, valid before sh2 is
// even opened, so there is no window in which a delivered report has nowhere to
// go. The "prime getSensorEvent before enabling reports" fix is therefore
// structural rather than a call that must not be forgotten.
sh2_SensorValue_t sDecoded{};
bool sResetOccurred = false;

// The instance the sh2 callback folds reports into. sh2_service() can dispatch
// SEVERAL reports per call, so the callback applies each one as it arrives
// instead of leaving a "pending" flag for service() to notice -- with four
// reports at ~55 Hz each, a one-slot handoff would quietly drop most of them and
// the rate line would under-report while looking healthy.
Bno08x *sActive = nullptr;
int sAppliedThisService = 0;

void hardwareReset() {
  // HIGH, LOW, HIGH with 10ms settles, exactly as the working stack did.
  gpio_set_level(kBnoReset, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(kBnoReset, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(kBnoReset, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
}

// Waits for the hub to assert INT, meaning it is ready for a transfer. On
// timeout it resets the hub, which is the only recovery the vendor HAL has.
bool waitForInt() {
  for (int i = 0; i < kIntWaitPolls; ++i) {
    if (gpio_get_level(kBnoInt) == 0) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  ++sStats.int_timeouts;
  ESP_LOGW(kTag, "INT timeout waiting for the hub; resetting it");
  hardwareReset();
  return false;
}

// One CS-framed transfer. spi_master drives CS per transaction, which is the
// same framing the Adafruit_SPIDevice read/write calls produced.
bool spiTransfer(const uint8_t *tx, uint8_t *rx, size_t len) {
  if (len == 0) {
    return true;
  }
  spi_transaction_t t{};
  t.length = len * 8;
  // A null tx_buffer means "no MOSI phase" and a null rx_buffer means "discard
  // MISO"; the clock is driven either way, which is all the hub needs to shift a
  // packet out on a read.
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  return spi_device_polling_transmit(sSpi, &t) == ESP_OK;
}

int halOpen(sh2_Hal_t *) {
  waitForInt();
  return 0;
}

void halClose(sh2_Hal_t *) {}

int halRead(sh2_Hal_t *, uint8_t *buffer, unsigned len, uint32_t *t_us) {
  (void)t_us;
  ++sStats.read_calls;

  if (!waitForInt()) {
    return 0;
  }

  // ONE continuous CS assertion for header + body.
  //
  // The Adafruit HAL this replaces did two independently CS-framed reads: 4 bytes
  // for the SHTP header, then the whole packet again from byte 0, relying on the
  // hub re-presenting an unread packet after CS deasserts. Ported literally, that
  // does not work here. Measured on hardware: the first four sensor reports
  // arrived and then the stream turned to garbage -- 17,593 reads with zero
  // errors, every packet claiming 15 bytes on SHTP channel 0 with sequence
  // numbers jumping around (198, 74, 208, 88) instead of the channel 3 sensor
  // reports we had enabled. That is the signature of a byte-shifted stream: the
  // header read consumed data the body read then missed.
  //
  // Holding CS across both transfers removes the assumption entirely -- the
  // header tells us the length and the body read continues the SAME transfer
  // rather than hoping to see it again. CS_KEEP_ACTIVE requires the bus to be
  // acquired first, and the final transfer must NOT set it or CS never releases.
  esp_err_t acquired = spi_device_acquire_bus(sSpi, portMAX_DELAY);
  if (acquired != ESP_OK) {
    ++sStats.header_transfer_failed;
    return 0;
  }

  spi_transaction_t header{};
  header.length = 4 * 8;
  header.rx_buffer = buffer;
  header.flags = SPI_TRANS_CS_KEEP_ACTIVE;
  if (spi_device_polling_transmit(sSpi, &header) != ESP_OK) {
    ++sStats.header_transfer_failed;
    spi_device_release_bus(sSpi);
    return 0;
  }

  uint16_t packet_size =
      static_cast<uint16_t>(buffer[0]) | static_cast<uint16_t>(buffer[1]) << 8;
  packet_size &= ~0x8000;  // clear the "continuation" bit
  sStats.last_header = packet_size;
  sStats.last_channel = buffer[2];
  sStats.last_seq = buffer[3];

  if (packet_size == 0) {
    ++sStats.empty_headers;
    spi_device_release_bus(sSpi);  // releasing deasserts CS
    return 0;
  }
  if (packet_size > len) {
    ++sStats.oversize_headers;
    spi_device_release_bus(sSpi);
    return 0;
  }

  // A header-only packet is complete already; anything longer has its remainder
  // read straight after the header, into the same buffer.
  if (packet_size > 4) {
    spi_transaction_t body{};
    body.length = (packet_size - 4) * 8;
    body.rx_buffer = buffer + 4;
    if (spi_device_polling_transmit(sSpi, &body) != ESP_OK) {
      ++sStats.body_transfer_failed;
      spi_device_release_bus(sSpi);
      return 0;
    }
  }

  spi_device_release_bus(sSpi);
  ++sStats.packets_read;
  return packet_size;
}

int halWrite(sh2_Hal_t *, uint8_t *buffer, unsigned len) {
  if (!waitForInt()) {
    return 0;
  }
  if (!spiTransfer(buffer, nullptr, len)) {
    return 0;
  }
  return static_cast<int>(len);
}

uint32_t halGetTimeUs(sh2_Hal_t *) {
  // Deliberately quantised to milliseconds, because the Arduino HAL returned
  // millis() * 1000 and sh2 uses this clock for its own op timeouts and report
  // timestamps. True microsecond resolution is almost certainly an improvement,
  // but it is a behavioural change to this hub's timing and belongs in its own
  // measured experiment rather than smuggled into the port.
  return static_cast<uint32_t>(esp_timer_get_time() / 1000) * 1000;
}

void asyncEventHandler(void *, sh2_AsyncEvent_t *event) {
  if (event->eventId == SH2_RESET) {
    sResetOccurred = true;
  }
}

void sensorEventHandler(void *, sh2_SensorEvent_t *event) {
  const int rc = sh2_decodeSensorEvent(&sDecoded, event);
  if (rc != SH2_OK) {
    ESP_LOGW(kTag, "sh2_decodeSensorEvent failed with %d", rc);
    return;
  }
  if (sActive != nullptr) {
    sActive->applyEvent(sDecoded);
    ++sAppliedThisService;
  }
}

sh2_Hal_t sHal{};

}  // namespace

const HalStats &Bno08x::halStats() { return sStats; }

int Bno08x::intLevel() { return gpio_get_level(kBnoInt); }

esp_err_t Bno08x::begin() {
  gpio_config_t reset_cfg{};
  reset_cfg.pin_bit_mask = 1ULL << kBnoReset;
  reset_cfg.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&reset_cfg));

  gpio_config_t int_cfg{};
  int_cfg.pin_bit_mask = 1ULL << kBnoInt;
  int_cfg.mode = GPIO_MODE_INPUT;
  int_cfg.pull_up_en = GPIO_PULLUP_ENABLE;  // INT is open-drain, active low
  ESP_ERROR_CHECK(gpio_config(&int_cfg));

  spi_bus_config_t bus{};
  bus.mosi_io_num = kBnoMosi;
  bus.miso_io_num = kBnoMiso;
  bus.sclk_io_num = kBnoSck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kBnoSpiMaxTransferBytes;
  esp_err_t err = spi_bus_initialize(kBnoSpiHost, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return err;
  }

  spi_device_interface_config_t dev{};
  dev.mode = kBnoSpiMode;
  dev.clock_speed_hz = kBnoSpiClockHz;
  dev.spics_io_num = kBnoCs;
  dev.queue_size = 1;
  err = spi_bus_add_device(kBnoSpiHost, &dev, &sSpi);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return err;
  }

  hardwareReset();

  sHal.open = halOpen;
  sHal.close = halClose;
  sHal.read = halRead;
  sHal.write = halWrite;
  sHal.getTimeUs = halGetTimeUs;

  const int open_status = sh2_open(&sHal, asyncEventHandler, nullptr);
  if (open_status != SH2_OK) {
    ESP_LOGE(kTag, "sh2_open failed with %d", open_status);
    return ESP_FAIL;
  }

  // Ask for the product ids. This is the first real hub round trip, so it is
  // also the check that the sensor is actually there and talking -- without it a
  // miswired bus looks like a sensor that simply never reports.
  sh2_ProductIds_t product_ids{};
  memset(&product_ids, 0, sizeof(product_ids));
  const int id_status = sh2_getProdIds(&product_ids);
  if (id_status != SH2_OK) {
    ESP_LOGE(kTag, "sh2_getProdIds failed with %d (is the BNO08x wired up?)",
             id_status);
    return ESP_FAIL;
  }
  for (unsigned i = 0; i < product_ids.numEntries; ++i) {
    const sh2_ProductId_t &id = product_ids.entry[i];
    ESP_LOGI(kTag, "hub part %lu: version %u.%u.%u build %lu",
             static_cast<unsigned long>(id.swPartNumber), id.swVersionMajor,
             id.swVersionMinor, id.swVersionPatch,
             static_cast<unsigned long>(id.swBuildNumber));
  }

  // Registered BEFORE the callback, so a report delivered by the very first
  // dispatch already has somewhere to go.
  sActive = this;
  sh2_setSensorCallback(sensorEventHandler, nullptr);

  // ---- The setup ORDER below is load-bearing timing, not configuration. ----
  //
  // What follows mirrors ../../embeded's verified-streaming sequence operation
  // for operation, because every sh2 op pumps SHTP while it waits for its reply,
  // so the SEQUENCE is itself part of what gets this hub going. Removing an op
  // whose result is unused is therefore not a cleanup -- it is a timing change.
  //
  // The Arduino version's first step was a getSensorEvent() called purely to
  // point a library static at a valid decode target. That pointer hazard is
  // designed out here (see sDecoded above), but the call also pumped SHTP once in
  // this position, so the pump is kept on its own.
  sh2_service();

  // NO sh2_setCalConfig here, and that is a DIFFERENCE from ../../embeded that
  // was forced by hardware.
  //
  // On the Arduino firmware that call sits here and fails with SH2_ERR_HUB, and
  // its failure is harmless. Measured on this fork (2026-08-11): here it
  // SUCCEEDS -- and a succeeding early setCalConfig is the documented way to
  // wedge this hub. Observed exactly that: it returned 0, the hub then stopped
  // asserting INT, the next op failed with -5, and the stream died after 2
  // reports and never recovered.
  //
  // Why the difference is timing, not code: the Arduino firmware starts the IMU
  // late in its own boot, after WiFi and MQTT setup, whereas a leaf has none of
  // that and gets here ~350ms after power-on. The hub is still early enough in
  // its own startup to accept the command, and accepting it is what breaks it.
  //
  // What the old comment about this call being "load-bearing" actually protects
  // is the SHTP pumping it did while awaiting a reply, not the command itself.
  // So the pumping is kept explicitly (above and below) and the real calibration
  // enable happens from the sample loop, ~5s into streaming, where it is
  // measured to work -- setCalConfig(0x07) -> 0 with a 0x07 read-back.
  //
  // sh2_setDcdAutoSave is deferred for the same reason: it is a hub WRITE, and
  // this early window is exactly where a write is dangerous. It now runs
  // alongside the calibration enable.

  // A READ in the position the writes used to occupy: it keeps the SHTP pumping
  // that the sequence depends on without asking the hub to change anything this
  // early. The value is logged as diagnostic only -- probing every mask on this
  // hub showed it accepting all of them while continuing to report 0x05, so it
  // says what the hub claims, not what is in effect.
  uint8_t reported_cal = 0;
  const int read_back = sh2_getCalConfig(&reported_cal);
  if (read_back == SH2_OK) {
    ESP_LOGI(kTag, "hub reports calibration 0x%02x (accel=%d gyro=%d mag=%d)",
             reported_cal, (reported_cal & 0x01) ? 1 : 0,
             (reported_cal & 0x02) ? 1 : 0, (reported_cal & 0x04) ? 1 : 0);
  } else {
    ESP_LOGW(kTag, "sh2_getCalConfig failed with %d", read_back);
  }

  if (!enableReports()) {
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "BNO08x started");
  return ESP_OK;
}

bool Bno08x::enableReports() {
  // Exactly the four reports ../../embeded enables, at its interval. Changing
  // this set changes what a recording contains, so it is not a knob to twiddle
  // while porting.
  struct ReportSpec {
    sh2_SensorId_t id;
    const char *name;
  };
  static constexpr ReportSpec kReports[] = {
      {SH2_ACCELEROMETER, "accelerometer"},
      {SH2_GYROSCOPE_CALIBRATED, "gyroscope"},
      {SH2_MAGNETIC_FIELD_CALIBRATED, "magnetometer"},
      {SH2_ROTATION_VECTOR, "rotation vector"},
  };

  sh2_SensorConfig_t config{};
  config.changeSensitivityEnabled = false;
  config.wakeupEnabled = false;
  config.changeSensitivityRelative = false;
  config.alwaysOnEnabled = false;
  config.changeSensitivity = 0;
  config.batchInterval_us = 0;
  config.sensorSpecific = 0;
  config.reportInterval_us = CONFIG_NATKIT_IMU_REPORT_INTERVAL_US;

  bool all_ok = true;
  for (const ReportSpec &report : kReports) {
    const int status = sh2_setSensorConfig(report.id, &config);
    if (status != SH2_OK) {
      ESP_LOGE(kTag, "could not enable %s (%d)", report.name, status);
      all_ok = false;
    }
  }
  return all_ok;
}

int Bno08x::service() {
  if (sResetOccurred) {
    sResetOccurred = false;
    ++reset_count_;
    ESP_LOGW(kTag, "hub reset (#%lu); re-enabling reports",
             static_cast<unsigned long>(reset_count_));
    enableReports();
  }

  // Each report is folded in by the callback as sh2_service() dispatches it, so
  // this counts rather than collects.
  sAppliedThisService = 0;
  sh2_service();
  return sAppliedThisService;
}

void Bno08x::applyEvent(const sh2_SensorValue_t &value) {
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  const uint8_t accuracy = value.status & kStatusAccuracyMask;

  SensorReading *target = nullptr;
  switch (value.sensorId) {
    case SH2_ACCELEROMETER:
      target = &readings_.accelerometer;
      target->x = value.un.accelerometer.x;
      target->y = value.un.accelerometer.y;
      target->z = value.un.accelerometer.z;
      break;
    case SH2_GYROSCOPE_CALIBRATED:
      target = &readings_.gyroscope;
      target->x = value.un.gyroscope.x;
      target->y = value.un.gyroscope.y;
      target->z = value.un.gyroscope.z;
      break;
    case SH2_MAGNETIC_FIELD_CALIBRATED:
      target = &readings_.magnetometer;
      target->x = value.un.magneticField.x;
      target->y = value.un.magneticField.y;
      target->z = value.un.magneticField.z;
      break;
    case SH2_ROTATION_VECTOR:
      target = &readings_.rotation;
      target->x = value.un.rotationVector.real;
      target->y = value.un.rotationVector.i;
      target->z = value.un.rotationVector.j;
      target->w = value.un.rotationVector.k;
      // The float error estimate, kept alongside the 2-bit status: this is the
      // rotation vector's real quality signal, and whether the status accuracy
      // is ever populated for this report is still an open question.
      target->rotation_accuracy_rad = value.un.rotationVector.accuracy;
      break;
    default:
      // A report we did not enable. Counted nowhere on purpose -- silently
      // folding it into another sensor would be worse than ignoring it.
      return;
  }

  target->accuracy = accuracy;
  target->last_us = now;
  target->has_data = true;
  ++target->count;

  ++total_reports_;
  if (first_report_us_ == 0) {
    first_report_us_ = now;
  }
}

void Bno08x::enableDynamicCalibrationOnce() {
  if (calibration_settled_ || first_report_us_ == 0) {
    return;
  }

  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  if (now - first_report_us_ < kCalibrationEnableAfterStreamingUs) {
    return;
  }
  if (calibration_attempts_ > 0 &&
      now - calibration_last_attempt_us_ < kCalibrationRetryUs) {
    return;
  }

  ++calibration_attempts_;
  calibration_last_attempt_us_ = now;

  const int status = sh2_setCalConfig(kDesiredCalibration);
  uint8_t read_back = 0;
  (void)sh2_getCalConfig(&read_back);

  // Deferred out of setup() with the calibration enable, because it is a hub
  // WRITE and the early window is where writes wedge this hub.
  const int dcd_status = sh2_setDcdAutoSave(true);
  if (dcd_status != SH2_OK) {
    ESP_LOGW(kTag,
             "sh2_setDcdAutoSave(true) failed with %d -- calibration will not "
             "persist across power cycles",
             dcd_status);
  }

  if (status == SH2_OK) {
    calibration_settled_ = true;
    calibration_enabled_ = true;
    ESP_LOGI(kTag,
             "dynamic calibration enabled after %lu reports: "
             "setCalConfig(0x%02x) -> 0, read-back 0x%02x",
             static_cast<unsigned long>(total_reports_), kDesiredCalibration,
             read_back);
    // The read-back is diagnostic only: probing every mask on this hub showed it
    // accepting all of them while still reporting 0x05, so it cannot be used to
    // confirm what is actually in effect.
    return;
  }

  ESP_LOGW(kTag, "setCalConfig(0x%02x) attempt %u failed with %d",
           kDesiredCalibration, calibration_attempts_, status);
  if (calibration_attempts_ >= kCalibrationMaxAttempts) {
    calibration_settled_ = true;
    ESP_LOGE(kTag,
             "giving up on dynamic calibration: gyro calibration is OFF, so "
             "rotation will stay Unreliable");
  }
}

}  // namespace natkit
