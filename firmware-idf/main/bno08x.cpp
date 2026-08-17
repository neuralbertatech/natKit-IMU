#include "bno08x.hpp"

#include <cstring>

#include "board_config.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
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

// Header/channel/sequence of the first few packets, logged once at bring-up.
// The failure this port is chasing shows up in the FIRST second -- a handful of
// good channel-3 reports and then channel-0 noise -- and a once-a-second summary
// cannot show the transition. Bounded rather than a flag, so the trace cannot be
// left on by accident, and emitted only after CS is released so it never lands
// inside an SHTP exchange.
constexpr uint32_t kTracePackets = 24;

spi_device_handle_t sSpi = nullptr;

// SPI staging buffers, deliberately not the caller's.
//
// Two reasons, both measured constraints of spi_master rather than style. First,
// DMA needs its buffers word-aligned and in internal RAM, and sh2's own rx/tx
// buffers are library statics that promise neither; a misaligned buffer is
// rejected with ESP_ERR_INVALID_ARG, which would show up as a transport failure
// with no obvious cause. Second, the ESP32's SPI DMA writes whole 32-bit words,
// so a read of a length that is not a multiple of 4 can scribble up to 3 bytes
// PAST the requested end -- here that overrun lands in our own slack (512 byte
// buffers against sh2's 384-byte maximum transfer) instead of in sh2's state.
WORD_ALIGNED_ATTR uint8_t sRxScratch[kBnoSpiMaxTransferBytes];
WORD_ALIGNED_ATTR uint8_t sTxScratch[kBnoSpiMaxTransferBytes];

// Zeros, and the whole point is that they are transmitted.
//
// Adafruit_SPIDevice::read() memsets the caller's buffer to its sendvalue (0x00
// for this HAL) and then does a FULL-DUPLEX transfer, so MOSI carries zeros for
// every clock of every read. This HAL used to pass tx_buffer = nullptr, which in
// spi_master means "no MOSI phase" -- the pin is not driven at all. The BNO08x is
// full duplex: whatever sits on MOSI while we clock a read is shifted into the
// hub's SHTP receiver, so an undriven line feeds it garbage writes. That matches
// what was measured on hardware (real SH2_RESET events with zero INT timeouts of
// ours, and a read stream decaying to 15-byte channel-0 packets) far better than
// the framing did. Driving zeros is not defensive: it is what the stack that
// streams on this board does.
WORD_ALIGNED_ATTR uint8_t sZeroTx[kBnoSpiMaxTransferBytes];

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

// CS is driven by hand, as a plain GPIO, rather than by the SPI peripheral.
//
// This is the second difference from the stack that streams on this board:
// Adafruit_SPIDevice asserts CS with digitalWrite (setChipSelect(LOW)) before
// beginning its transfers and releases it after, so the hub gets microseconds of
// CS-to-first-clock setup and last-clock-to-CS hold. Peripheral-driven CS gives it
// a fraction of a bit-time. Doing it by hand also makes "one CS assertion per
// SHTP packet" the plain reading of the code instead of an interaction between
// acquire_bus and CS_KEEP_ACTIVE.
inline void csAssert() { gpio_set_level(kBnoCs, 0); }
inline void csRelease() { gpio_set_level(kBnoCs, 1); }

// One transfer inside an already-asserted CS. Always full duplex: rx may be null
// (a write, MISO discarded) but tx never is, because MOSI must be driven.
bool spiTransfer(const uint8_t *tx, uint8_t *rx, size_t len) {
  if (len == 0) {
    return true;
  }
  spi_transaction_t t{};
  t.length = len * 8;
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
  // did not work here. Measured on hardware: the first four sensor reports
  // arrived and then the stream turned to garbage -- 17,593 reads with zero
  // errors, every packet claiming 15 bytes on SHTP channel 0 with sequence
  // numbers jumping around (198, 74, 208, 88) instead of the channel 3 sensor
  // reports we had enabled.
  //
  // Holding CS across both transfers removes the assumption entirely -- the
  // header tells us the length and the body read continues the SAME transfer
  // rather than hoping to see it again. NOTE that the byte-shift reading of that
  // capture is now in doubt: MOSI was undriven at the time (see sZeroTx), so the
  // hub was very likely already wedged by garbage on its own receive line, which
  // would explain the same evidence without the framing being at fault. If this
  // HAL streams, that question is moot; if it does not, the two-transaction
  // framing is worth re-testing now that MOSI and CS behave like the working
  // stack's.
  csAssert();

  if (!spiTransfer(sZeroTx, sRxScratch, 4)) {
    csRelease();
    ++sStats.header_transfer_failed;
    return 0;
  }

  uint16_t packet_size = static_cast<uint16_t>(sRxScratch[0]) |
                         static_cast<uint16_t>(sRxScratch[1]) << 8;
  packet_size &= ~0x8000;  // clear the "continuation" bit
  sStats.last_header = packet_size;
  sStats.last_channel = sRxScratch[2];
  sStats.last_seq = sRxScratch[3];

  if (packet_size == 0) {
    csRelease();
    ++sStats.empty_headers;
    return 0;
  }
  if (packet_size > len || packet_size > sizeof(sRxScratch)) {
    csRelease();
    ++sStats.oversize_headers;
    return 0;
  }

  // A header-only packet is complete already; anything longer has its remainder
  // read straight after the header, into the same staging buffer.
  if (packet_size > 4) {
    if (!spiTransfer(sZeroTx, sRxScratch + 4, packet_size - 4)) {
      csRelease();
      ++sStats.body_transfer_failed;
      return 0;
    }
  }

  csRelease();

  memcpy(buffer, sRxScratch, packet_size);
  ++sStats.packets_read;

  if (sStats.packets_read <= kTracePackets) {
    ESP_LOGI(kTag, "shtp rx #%lu: %u bytes, channel %u, seq %u",
             static_cast<unsigned long>(sStats.packets_read), packet_size,
             sRxScratch[2], sRxScratch[3]);
  }

  return packet_size;
}

int halWrite(sh2_Hal_t *, uint8_t *buffer, unsigned len) {
  if (len == 0 || len > sizeof(sTxScratch)) {
    return 0;
  }
  if (!waitForInt()) {
    return 0;
  }

  // Staged for the same alignment reason as the read path: sh2's tx buffer is a
  // library static with no DMA guarantees.
  memcpy(sTxScratch, buffer, len);

  csAssert();
  const bool ok = spiTransfer(sTxScratch, nullptr, len);
  csRelease();

  if (!ok) {
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
  // Idle high: the hub runs when RESET is released, and a pin left at its 0
  // default holds it in reset from boot until hardwareReset() below.
  ESP_ERROR_CHECK(gpio_set_level(kBnoReset, 1));

  // CS is ours, not the peripheral's (see csAssert). Set idle-high BEFORE the
  // first transfer, or the hub sees a selected bus during bring-up.
  gpio_config_t cs_cfg{};
  cs_cfg.pin_bit_mask = 1ULL << kBnoCs;
  cs_cfg.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cs_cfg));
  ESP_ERROR_CHECK(gpio_set_level(kBnoCs, 1));

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
  dev.spics_io_num = -1;  // CS is driven by hand; see csAssert()
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

  // BEFORE the first enableReports, so a mask restored from NVS is what the hub
  // is first configured with -- applying it a moment later would put one round of
  // unwanted reports on the wire, and on a recording that is a real artefact.
  loadReportMask();
  if (!enableReports()) {
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "BNO08x started");
  return ESP_OK;
}

namespace {
constexpr char kReportNamespace[] = "natkit-imu";
constexpr char kReportMaskKey[] = "reports";
}  // namespace

esp_err_t Bno08x::setReportMask(const uint8_t mask) {
  // ⚠️ REFUSED, not clamped. Silently substituting a working mask would leave the
  // frontend showing a state the device is not in, and the caller has an answer
  // channel to be told on.
  if ((mask & kReportMotionMask) == 0) {
    ESP_LOGE(kTag,
             "refusing report mask 0x%02x: it leaves no motion report, and this "
             "node would stop producing samples entirely",
             mask);
    return ESP_ERR_INVALID_ARG;
  }

  const uint8_t previous = report_mask_;
  report_mask_ = mask & kReportAll;

  // ⚠️ CLEAR WHAT WAS TURNED OFF. The frame builder copies each sensor's last
  // value into every sample and marks freshness separately, so a disabled sensor
  // would otherwise keep contributing the last number it produced, forever. A
  // plausible reading from a switched-off sensor is worse than a zero, because
  // nothing about it looks wrong.
  const uint8_t turned_off = static_cast<uint8_t>(previous & ~report_mask_);
  if ((turned_off & kReportAccel) != 0) {
    readings_.accelerometer = SensorReading{};
  }
  if ((turned_off & kReportGyro) != 0) {
    readings_.gyroscope = SensorReading{};
  }
  if ((turned_off & kReportMagnetometer) != 0) {
    readings_.magnetometer = SensorReading{};
  }
  if ((turned_off & kReportRotation) != 0) {
    readings_.rotation = SensorReading{};
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kReportNamespace, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, kReportMaskKey, report_mask_);
    if (err == ESP_OK) {
      err = nvs_commit(handle);
    }
    nvs_close(handle);
  }
  if (err != ESP_OK) {
    // The mask still applies for this boot; only its persistence failed, and
    // saying which is the difference between "it did not work" and "it will not
    // survive a reboot".
    ESP_LOGE(kTag, "report mask 0x%02x applied but NOT saved: %s", report_mask_,
             esp_err_to_name(err));
  }

  ESP_LOGW(kTag, "reports now: accel %s, gyro %s, mag %s, rotation %s",
           (report_mask_ & kReportAccel) ? "on" : "OFF",
           (report_mask_ & kReportGyro) ? "on" : "OFF",
           (report_mask_ & kReportMagnetometer) ? "on" : "OFF",
           (report_mask_ & kReportRotation) ? "on" : "OFF");
  return enableReports() ? ESP_OK : ESP_FAIL;
}

void Bno08x::resetBurstStats() {
  burst_ = BurstStats{};
  // last_burst_us_ is deliberately NOT cleared: clearing it would throw away the
  // gap that straddles the reset, and that gap is a real observation.
}

void Bno08x::loadReportMask() {
  // ⚠️ EVERY OUTCOME IS LOGGED, including the boring ones. This was written to
  // return quietly when NVS had nothing to say, and when the mask then failed to
  // survive a reboot there was no way to tell "never saved" from "saved and not
  // read" from "read and rejected" -- three different bugs that all look like one
  // symptom. A configuration that silently reverts is worse than one that refuses
  // to change, so this end is noisy on purpose.
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kReportNamespace, NVS_READONLY, &handle);
  if (opened != ESP_OK) {
    ESP_LOGW(kTag,
             "report mask not restored: nvs_open(%s) says %s. Using the "
             "compiled-in default 0x%02x",
             kReportNamespace, esp_err_to_name(opened), report_mask_);
    return;
  }
  uint8_t stored = 0;
  const esp_err_t got = nvs_get_u8(handle, kReportMaskKey, &stored);
  nvs_close(handle);
  if (got != ESP_OK) {
    ESP_LOGW(kTag, "report mask not restored: nvs_get_u8(%s) says %s",
             kReportMaskKey, esp_err_to_name(got));
    return;
  }
  if ((stored & kReportMotionMask) == 0) {
    ESP_LOGE(kTag,
             "stored report mask 0x%02x has no motion report and was IGNORED; "
             "this node would produce nothing at all",
             stored);
    return;
  }
  report_mask_ = stored & kReportAll;
  ESP_LOGW(kTag, "restored report mask 0x%02x from NVS", report_mask_);
}

bool Bno08x::enableReports() {
  // ⚠️ THE HUB DOES NOT DELIVER WHAT IT IS ASKED FOR, and not uniformly, so no
  // single interval lands four reports on one rate. Every number below is
  // measured on hardware (tools/sweep_report_rates.sh reproduces the table).
  //
  // Read it against 100 Hz as a FLOOR, not a target: the frame builder takes a
  // 100 Hz snapshot of each sensor's latest value, so a report above 100 Hz is
  // merely wasted while one below leaves stale values in samples.
  //
  // Uniform requests -- asking for MORE inverts the problem rather than fixing it:
  //
  //     asked     accel  gyro   mag  quat   total
  //     100 Hz      114    95    91    95     395
  //     125 Hz       83   155    78   155     471
  //     150 Hz       65   175    63   175     478
  //     200 Hz       63   174    62   177     476
  //
  // So there is a CEILING near 475 reports/s, and under contention the hub feeds
  // gyro and rotation at the expense of accel and mag.
  //
  // ⚠️ BUT THE 95 Hz AT A 100 Hz REQUEST IS NOT CONTENTION. 395 is well under the
  // ceiling, and freeing budget does almost nothing -- dropping a whole report
  // returns only ~1-2 Hz to the others:
  //
  //     all four                    114 /  95 /  91 /  95
  //     rotation dropped            118 /  96 /  92 /   -
  //     gyroscope dropped           116 /   - /  92 /  95
  //     accel + mag only            121 /   - /  94 /   -
  //
  // What DOES work is asking faster: gyro and rotation have no rate between ~95
  // and ~185, and anything below a 10 ms request snaps them to the high one. That
  // only fits under the ceiling if something else is dropped:
  //
  //     drop rotation, gyro+mag asked 111 Hz    120 / 188 / 96.5 /   -
  //     drop gyroscope, quat asked 111 Hz       114 /   - / 94   / 182
  //     KEEP all four, gyro asked 111 Hz        100 / 172 / 83   /  86   <- worse
  //     KEEP all four, accel throttled to 71 Hz  81 / 154 / 78   / 154   <- worse
  //
  // ⚠️ AND THE MAGNETOMETER CANNOT REACH 100 Hz AT ALL. Its ceiling is ~96.5, and
  // asking for more makes it worse, not better (200 Hz requested -> 91.5). Even
  // alone with just the accelerometer it manages 93.5. That is the sensor, not
  // the schedule -- its datasheet maximum is 100 Hz.
  //
  // ⚠️⚠️ AND NONE OF THE ABOVE REACHES A SAMPLE. Every rate in these tables is
  // counted at the sh2 callback, and the hub does not deliver reports evenly -- it
  // delivers them in BURSTS at about 88 Hz. A "116 Hz" accelerometer is ~1.3
  // reports per burst, not a 116 Hz stream, and the extra one is overwritten
  // before the 100 Hz sampler ever sees it.
  //
  // Measured from the other end, as the fraction of emitted samples carrying a
  // fresh reading of each sensor (the "fresh:" console line): accel 88%, gyro 88%,
  // mag 88%, quat 88% -- all four identical, because they arrive together.
  //
  // ⚠️ THE PROOF THAT THIS IS THE BINDING LIMIT: asking the gyroscope for 111 Hz
  // and dropping rotation to fund it takes its delivered rate to 183 Hz, and its
  // sample-visible freshness stays at 88%. DOUBLE the reports, no change whatever
  // in what reaches the data. So tuning these intervals -- including giving a
  // report up to fund another -- cannot raise the rate of distinct observations.
  // What sets the ~88 Hz burst cadence is the open question, and it is the only
  // lever that would matter (TEC-NATKIT-41).
  //
  // ⚠️ SO THE DEFAULT BELOW IS UNIFORM 10 ms, WHICH IS THE BEST CONFIGURATION
  // THAT KEEPS ALL FOUR REPORTS. Every attempt to beat it while keeping four was
  // measured and was worse. Getting gyro or rotation genuinely above 100 Hz means
  // giving one of them up, which is a decision about what a recording contains
  // rather than a tuning question -- so it is left to whoever makes that call, and
  // the intervals are per-report Kconfig knobs (0 = off) so it is one edit.
  //
  // If that call gets made, it should be made for a reason OTHER than rate --
  // airtime, power, or simply not wanting the channel -- because the freshness
  // measurement above shows it will not buy rate. Should it be made anyway: drop
  // rotation, not gyro. The rotation vector is the hub's FUSION of the other
  // three, so accel + gyro + mag still contains what it was computed from, while
  // the gyroscope is a primary measurement nothing else reconstructs. Rotation is
  // also already absent from the transform/Parquet path, so it would only leave
  // the JSON and viewer paths.

  // Kconfig sets each report's RATE; the runtime mask decides which are on. An
  // interval of 0 still means "compiled off" and the mask cannot resurrect it,
  // because there would be no rate to ask for.
  struct MaskedSpec {
    sh2_SensorId_t id;
    const char *name;
    uint32_t interval_us;
    uint8_t bit;
  };
  const MaskedSpec kReports[] = {
      {SH2_ACCELEROMETER, "accelerometer", CONFIG_NATKIT_IMU_INTERVAL_ACCEL_US,
       kReportAccel},
      {SH2_GYROSCOPE_CALIBRATED, "gyroscope", CONFIG_NATKIT_IMU_INTERVAL_GYRO_US,
       kReportGyro},
      {SH2_MAGNETIC_FIELD_CALIBRATED, "magnetometer",
       CONFIG_NATKIT_IMU_INTERVAL_MAG_US, kReportMagnetometer},
      {SH2_ROTATION_VECTOR, "rotation vector",
       CONFIG_NATKIT_IMU_INTERVAL_QUAT_US, kReportRotation},
  };

  sh2_SensorConfig_t config{};
  config.changeSensitivityEnabled = false;
  config.wakeupEnabled = false;
  config.changeSensitivityRelative = false;
  config.alwaysOnEnabled = false;
  config.changeSensitivity = 0;
  config.batchInterval_us = 0;
  config.sensorSpecific = 0;

  bool all_ok = true;
  for (const MaskedSpec &report : kReports) {
    if (report.interval_us == 0 || (report_mask_ & report.bit) == 0) {
      // Explicitly disable rather than just not enabling: the hub remembers its
      // configuration across a soft reset, so a report left over from a previous
      // firmware would keep arriving and keep spending the ceiling.
      config.reportInterval_us = 0;
      sh2_setSensorConfig(report.id, &config);
      ESP_LOGI(kTag, "%s disabled", report.name);
      continue;
    }
    config.reportInterval_us = report.interval_us;
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
    // ⚠️ Re-enables from report_mask_, which lives in RAM on OUR side and is
    // untouched by the hub resetting itself. That is what stops a hub reset
    // silently reverting a runtime configuration to the compiled-in default while
    // the frontend goes on showing the old one.
    ESP_LOGW(kTag, "hub reset (#%lu); re-enabling reports (mask 0x%02x)",
             static_cast<unsigned long>(reset_count_), report_mask_);
    enableReports();
  }

  // Each report is folded in by the callback as sh2_service() dispatches it, so
  // this counts rather than collects.
  // ⚠️ DRAIN, don't sample. sh2_service() returns after handling what is
  // immediately available, so calling it once per pump caps the report rate at
  // roughly one batch per pump however fast the hub is producing. Loop until a
  // pass yields nothing, bounded so a chattering hub cannot hold the task here.
  sAppliedThisService = 0;
  for (int pass = 0; pass < 8; ++pass) {
    const uint32_t before = sAppliedThisService;
    ++burst_.service_calls;
    sh2_service();
    if (sAppliedThisService == before) {
      break;
    }
  }

  if (sAppliedThisService > 0) {
    ++burst_.productive_calls;
  }
  return sAppliedThisService;
}

void Bno08x::applyEvent(const sh2_SensorValue_t &value) {
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  const uint8_t accuracy = value.status & kStatusAccuracyMask;

  SensorReading *target = nullptr;
  switch (value.sensorId) {
    case SH2_ACCELEROMETER:
      // Gap since the previous accelerometer report, recorded here rather than at
      // the poll: this is the hub speaking, not us looking.
      if (last_burst_us_ != 0) {
        const uint32_t gap = static_cast<uint32_t>(now - last_burst_us_);
        ++burst_.gaps;
        burst_.gap_sum_us += gap;
        if (gap < burst_.gap_min_us) burst_.gap_min_us = gap;
        if (gap > burst_.gap_max_us) burst_.gap_max_us = gap;
        if (gap < 2000) ++burst_.gaps_under_2ms;
      }
      last_burst_us_ = now;
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
