#include <cinttypes>

#include "bno08x.hpp"
#include "status_led.hpp"
#include "device_id.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "commands.hpp"
#include "espnow_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu_frame.hpp"
#include "node_role.hpp"
#include "sdkconfig.h"
#include "time_sync.hpp"

// Leaf (secondary) node: IMU + ESP-NOW, and that is the whole device.
//
// TEC-NATKIT-22 put the sensor in. TEC-NATKIT-23 decided the on-air format: the
// canonical NatImuBulkDataSchema frame is built HERE, on the node, and one frame is
// one packet -- 524 bytes against a measured 1470-byte ceiling, so there is no
// fragmentation and no reassembly state on the primary. This file is
// TEC-NATKIT-24: it turns the sensor's reports into those frames and puts them on
// the radio.
//
// Deliberately absent, and this is the architecture rather than an omission: no
// esp_wifi_connect, no MQTT client, no SNTP, no HTTP stack, and no netif at all.
// The headroom that buys is the point of the whole epic.
//
// ⚠️ Timestamps here are MONOTONIC SINCE BOOT, not wall clock. A leaf has no NTP by
// design, so whatever forwards these frames must translate them against the
// primary's clock -- a gateway that published them unmodified would advertise
// 1970-era timestamps. That translation is the timing slice (#340), and it is not
// done here; the frames are honest about carrying device-relative time.

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-leaf";

// --- sampling runs in its OWN task, and that is the fix rather than a tidy-up --
//
// It used to share the main loop with imu.service(), and at a 20 ms interval that
// was fine. At 10 ms (#380) it was not: a service() call that overran the deadline
// cost a sample, and MEASURED 15.2% of slots were lost that way -- 592 of 3903 --
// while the radio reported zero packet loss to explain the missing data. Making
// the delay deadline-aware did not help, because the overrun is inside service()
// itself.
//
// So the cadence gets its own task at a higher priority, paced by
// xTaskDelayUntil, which is drift-free by construction. All it does is SNAPSHOT
// readings that service() has already decoded -- no SPI, no blocking -- so its
// deadline does not depend on how long the sensor takes.
//
// ⚠️ The snapshot races service()'s writes, deliberately. A torn read would mix
// one report's axes with another's, which is precisely what a "merged snapshot
// across asynchronous reports" already is (see sampleFromReadings) -- so the race
// changes nothing semantically and a lock here would put SPI latency back into
// the cadence, which is the whole problem being fixed.
Bno08x *sImu = nullptr;
volatile uint32_t sFramesBuilt = 0;
// Samples emitted, and how many of them carried a FRESH reading of each sensor.
// ⚠️ These exist as a cross-check, not as decoration: fresh/emitted must come out
// at the sensor's delivered rate divided by 100, so they confirm the per-report
// rates from a completely independent count. If the two disagree, one of them is
// lying and it matters which.
volatile uint32_t sSamplesEmitted = 0;
volatile uint32_t sFreshCount[4] = {};
volatile uint32_t sMissedSlots = 0;

// Ingests sensor reports, and nothing else.
//
// Separate from BOTH the sampling task and the main loop. The sampler must not be
// delayed by a slow SPI read; the ingest must not be delayed by console logging.
// One task each is the only arrangement where neither is true.
void serviceTask(void *) {
  while (true) {
    if (sImu != nullptr) {
      sImu->service();
      sImu->enableDynamicCalibrationOnce();
    }
    // 1 ms, matching what the main loop used to give it. The hub asserts INT when
    // it has something, and halRead waits on that, so this is a floor on how
    // often we ask rather than a throttle on what arrives.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void samplingTask(void *) {
  constexpr size_t kSamplesPerFrame = CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME;
  static ImuSample samples[kMaxSamplesPerFrame]{};
  static uint8_t frame[kFrameHeaderSize + kMaxSamplesPerFrame * kSampleSize];
  size_t sample_count = 0;
  uint64_t frame_seq = 0;

  // Whole ticks: the tick rate is 1000 Hz, so a 10000 us interval is exactly 10.
  const TickType_t period = pdMS_TO_TICKS(CONFIG_NATKIT_IMU_SAMPLE_INTERVAL_US / 1000);
  TickType_t last = xTaskGetTickCount();

  while (true) {
    xTaskDelayUntil(&last, period);
    if (sImu == nullptr) {
      continue;
    }

    ImuSample sample{};
    static SampleCursor cursor{};
    if (!sampleFromReadings(sImu->readings(), cursor, sample)) {
      ++sMissedSlots;  // nothing decoded yet: a real slot with no reading in it
      continue;
    }
    ++sSamplesEmitted;
    if (sample.has_data & 0b0100) ++sFreshCount[0];  // accel
    if (sample.has_data & 0b0010) ++sFreshCount[1];  // gyro
    if (sample.has_data & 0b1000) ++sFreshCount[2];  // mag
    if (sample.has_data & 0b0001) ++sFreshCount[3];  // rotation
    samples[sample_count++] = sample;
    if (sample_count < kSamplesPerFrame) {
      continue;
    }

    // deviceTsUs is the FIRST sample's time, matching the current firmware.
    const uint64_t device_ts_us = samples[0].time_ms * 1000ULL;
    const size_t length =
        encodeFrame(samples, sample_count, frame_seq, device_ts_us,
                    CONFIG_NATKIT_IMU_DECLARED_RATE_HZ, frame, sizeof(frame));
    sample_count = 0;
    if (length == 0) {
      ESP_LOGE(kTag, "frame encoding failed -- layout constants disagree");
      continue;
    }
    ++frame_seq;
    ++sFramesBuilt;
    // Never blocks, whether or not a primary is listening.
    espNowLinkSend(PacketType::kData, frame, length);
  }
}

const char *accuracyName(uint8_t accuracy) {
  switch (accuracy) {
    case 0:
      return "unreliable";
    case 1:
      return "low";
    case 2:
      return "medium";
    case 3:
      return "high";
    default:
      return "?";
  }
}

const char *syncQualityName(SyncQuality quality) {
  switch (quality) {
    case SyncQuality::kUnsynced:
      return "UNSYNCED";
    case SyncQuality::kCoarse:
      return "coarse";
    case SyncQuality::kLocked:
      return "locked";
    default:
      return "?";
  }
}

}  // namespace

void runLeaf() {
  ESP_LOGI(kTag, "leaf: device %" PRIu64 ", IMU on SPI, ESP-NOW only",
           deviceId());

  // A leaf whose sensor did not start STILL JOINS THE RADIO.
  //
  // It used to fall into idleStatusLoop here, which made a sensor failure
  // visible only to whoever was holding a USB cable -- the primary saw an absent
  // node, indistinguishable from one that was unplugged or out of range. Now it
  // announces, heartbeats and takes part in the timing broadcast, so the failure
  // is diagnosable from the hub. It sends no data frames, because it has no data:
  // sampleFromReadings would refuse anyway, and a frame of zeroes is worse than
  // no frame.
  //
  // It is also what makes a sensorless board usable as a second timing node,
  // which is how TEC-NATKIT-4 measures node-to-node coherence.
  Bno08x imu;
  const bool have_imu = imu.begin() == ESP_OK;
  commandsSetImu(&imu);
  if (!have_imu) {
    ESP_LOGE(kTag,
             "IMU did not start -- continuing as a SENSORLESS leaf: no data "
             "frames, but the radio, the heartbeat and the clock fit all run, so "
             "this node is diagnosable from the primary rather than merely "
             "absent");
  }

  sImu = &imu;
  if (have_imu) {
    // Priority 6: above the main loop (1) so a long service() cannot delay the
    // cadence, and level with the tx task so neither starves the other.
    xTaskCreate(samplingTask, "natkit-sample", 4096, nullptr, 6, nullptr);
    // ⚠️ REPORT INGESTION GETS ITS OWN TASK TOO, and this is the second half of a
    // fix whose first half was incomplete. Sampling was moved off the main loop
    // because imu.service() overrunning cost 15% of sample slots -- but
    // imu.service() itself stayed on the main loop, which also does the console
    // logging. So the console still stalled the thing that INGESTS reports, and
    // the samples were dutifully taken on time with nothing new in them.
    //
    // Measured: at a 1 Hz log interval the accelerometer showed gaps of up to
    // 160 ms once a second and 87% of samples carried fresh data. At a 10 s
    // interval the same firmware reached 92-99%. That 12% was ours, not the
    // sensor's -- and it is what was previously written up as an "~88 Hz burst
    // cadence" of the hub. The hub emits evenly at ~115 Hz; we were not listening.
    //
    // Priority 5: above the main loop so logging cannot block it, below sampling
    // so a slow SPI read cannot push a sample slot late.
    xTaskCreate(serviceTask, "natkit-imu-svc", 4096, nullptr, 5, nullptr);
  }

  if (espNowLinkStart() != ESP_OK) {
    ESP_LOGE(kTag, "ESP-NOW did not start; the sensor still runs, so the console "
                   "remains useful for diagnosing it");
  }

  // The sample loop pumps SHTP as fast as it can rather than on a timer: the hub
  // decides when reports are ready (INT), the report interval is configured on the
  // hub itself, and a sleep here just adds latency. Sampling for the FRAME is on a
  // fixed interval, which is a different thing -- see below.
  constexpr TickType_t kPumpDelay = pdMS_TO_TICKS(1);
  constexpr uint64_t kLogIntervalUs =
      static_cast<uint64_t>(CONFIG_NATKIT_IMU_LOG_INTERVAL_MS) * 1000ULL;
  constexpr uint64_t kSampleIntervalUs = CONFIG_NATKIT_IMU_SAMPLE_INTERVAL_US;
  constexpr uint64_t kHeartbeatIntervalUs =
      static_cast<uint64_t>(CONFIG_NATKIT_ESPNOW_HEARTBEAT_MS) * 1000ULL;
  constexpr size_t kSamplesPerFrame = CONFIG_NATKIT_IMU_SAMPLES_PER_FRAME;
  static_assert(kSamplesPerFrame <= kMaxSamplesPerFrame,
                "samples per frame exceeds the transmit buffer sized from it");

  ImuSample samples[kMaxSamplesPerFrame]{};
  size_t sample_count = 0;
  uint64_t frame_seq = 0;
  uint32_t frames_built = 0;

  // One frame buffer, reused. Not an optimisation: the old firmware's
  // std::bad_alloc came from the frame path allocating per copy, so nothing on this
  // path allocates after startup.
  static uint8_t frame[kFrameHeaderSize + kMaxSamplesPerFrame * kSampleSize];

  uint64_t next_log_us = 0;
  uint64_t next_heartbeat_us = kHeartbeatIntervalUs;
  uint32_t reports_at_last_log = 0;
  // Per-report, not just the total: the aggregate cannot tell "all four at 100 Hz"
  // from "three at 133 Hz and one dead", and those need different fixes.
  uint32_t per_report_at_last_log[4] = {};
  uint32_t fresh_at_last_log[4] = {};
  uint32_t samples_at_last_log = 0;
  uint32_t frames_at_last_log = 0;
  uint64_t last_log_us = 0;

  while (true) {
    if (have_imu) {
      // ⚠️ imu.service() is NOT called here any more -- see serviceTask. Putting it
      // back would re-couple report ingestion to this loop's console logging.
      commandsService();
    }

    // Advance an identify flash, if one is running. ⚠️ OUTSIDE the have_imu gate:
    // an armed sequence must finish even on a board whose sensor did not come up,
    // or the LED would be left stuck mid-flash. And here rather than inside the
    // command handler because a blocking flash would stall this loop, which also
    // services the link and the console (see status_led.hpp). Returns immediately
    // when nothing is armed.
    statusLedService();

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

    // --- heartbeat -----------------------------------------------------------
    if (kHeartbeatIntervalUs > 0 && now >= next_heartbeat_us) {
      next_heartbeat_us = now + kHeartbeatIntervalUs;
      const SensorSet &r = imu.readings();
      const LinkStats &link = espNowLinkStats();

      Heartbeat beat{};
      beat.device_id = deviceId();
      beat.uptime_us = now;
      beat.frames_built = sFramesBuilt;
      beat.frames_sent = link.packets_sent;
      beat.frames_dropped = link.packets_dropped;
      beat.send_failures = link.send_failures;
      beat.sensor_reports = imu.totalReports();
      beat.hub_resets = imu.resetCount();
      beat.free_heap = static_cast<uint32_t>(esp_get_free_heap_size());
      beat.accuracy_accel = r.accelerometer.accuracy;
      beat.accuracy_gyro = r.gyroscope.accuracy;
      beat.accuracy_mag = r.magnetometer.accuracy;
      beat.accuracy_rotation = r.rotation.accuracy;
      beat.channel_hops = link.channel_hops;
      beat.scan_channel = link.scan_channel;
      // 0 when this leaf has not heard the hub yet -- see Heartbeat, where 0 is
      // the "unknown" sentinel rather than a measurement.
      beat.rssi_of_primary = link.rssi_seen ? link.rssi_last : 0;
      beat.tx_power_quarter_dbm =
          link.tx_power_quarter_dbm > 0
              ? static_cast<uint8_t>(link.tx_power_quarter_dbm)
              : 0;
      // Worst floor since the last heartbeat, then cleared, so a spike between
      // heartbeats is reported rather than averaged away. 0 = never sampled.
      beat.noise_floor_dbm = link.noise_seen ? link.noise_floor_worst : 0;
      espNowLinkResetNoiseWorst();
      espNowLinkSend(PacketType::kHeartbeat, &beat, sizeof(beat));
    }

    // --- console -------------------------------------------------------------
    if (kLogIntervalUs > 0 && now >= next_log_us) {
      const SensorSet &r = imu.readings();
      const uint32_t total = imu.totalReports();
      const LinkStats &link = espNowLinkStats();

      // Rates from the actual elapsed time, not assumed from the log interval: a
      // loop that falls behind is exactly what these lines exist to make visible.
      const uint64_t elapsed_us = last_log_us == 0 ? 0 : now - last_log_us;
      const float report_hz =
          elapsed_us == 0 ? 0.0F
                          : static_cast<float>(total - reports_at_last_log) *
                                1'000'000.0F / static_cast<float>(elapsed_us);
      const float frame_hz =
          elapsed_us == 0 ? 0.0F
                          : static_cast<float>(sFramesBuilt - frames_at_last_log) *
                                1'000'000.0F / static_cast<float>(elapsed_us);

      if (!have_imu) {
        ESP_LOGW(kTag,
                 "sensorless leaf: no IMU on this board, so no data frames. Heap "
                 "%" PRIu32 " B (min %" PRIu32 " B)",
                 static_cast<uint32_t>(esp_get_free_heap_size()),
                 static_cast<uint32_t>(esp_get_minimum_free_heap_size()));
      }
      if (have_imu) {
      ESP_LOGI(kTag,
               "accel %+7.3f %+7.3f %+7.3f (%s) | gyro %+7.3f %+7.3f %+7.3f "
               "(%s) | quat %+6.3f %+6.3f %+6.3f %+6.3f (%s)",
               r.accelerometer.x, r.accelerometer.y, r.accelerometer.z,
               accuracyName(r.accelerometer.accuracy), r.gyroscope.x,
               r.gyroscope.y, r.gyroscope.z, accuracyName(r.gyroscope.accuracy),
               r.rotation.x, r.rotation.y, r.rotation.z, r.rotation.w,
               accuracyName(r.rotation.accuracy));
      // ⚠️ THE MAGNETOMETER IS ENABLED AND ARRIVING, it is simply not on the wire
      // -- the schema has no field for it (see imu_frame.hpp). So this line is the
      // only place it is visible, and it is what says whether carrying it would
      // cost anything: if mag is already at the same rate as the other three, the
      // sensor and the SPI link are ALREADY paying for it.
      {
        const uint32_t per_report[4] = {r.accelerometer.count, r.gyroscope.count,
                                        r.magnetometer.count, r.rotation.count};
        const char *names[4] = {"accel", "gyro", "mag", "quat"};
        char breakdown[128];
        int written = 0;
        for (int i = 0; i < 4; ++i) {
          const float hz =
              elapsed_us == 0
                  ? 0.0F
                  : static_cast<float>(per_report[i] - per_report_at_last_log[i]) *
                        1'000'000.0F / static_cast<float>(elapsed_us);
          written += snprintf(breakdown + written, sizeof(breakdown) - written,
                              "%s%s %.1f Hz", i ? " | " : "", names[i], hz);
          per_report_at_last_log[i] = per_report[i];
        }
        const auto askedHz = [](int us) { return us > 0 ? 1'000'000.0 / us : 0.0; };
        // The same four sensors seen from the OTHER end: what fraction of emitted
        // samples carried a fresh reading. Should equal the delivered rate above
        // divided by the 100 Hz sample rate, computed from a separate counter.
        const uint32_t emitted = sSamplesEmitted - samples_at_last_log;
        char freshness[128];
        int fw = 0;
        for (int i = 0; i < 4; ++i) {
          const uint32_t f = sFreshCount[i] - fresh_at_last_log[i];
          fw += snprintf(freshness + fw, sizeof(freshness) - fw, "%s%s %.0f%%",
                         i ? " | " : "", names[i],
                         emitted > 0 ? 100.0 * f / emitted : 0.0);
          fresh_at_last_log[i] = sFreshCount[i];
        }
        samples_at_last_log = sSamplesEmitted;
        ESP_LOGI(kTag, "fresh:   %s   (of %lu samples emitted)", freshness,
                 static_cast<unsigned long>(emitted));
        // ⚠️ SPREAD, NOT JUST THE AVERAGE. A hub emitting on its own clock gives a
        // tight min/max; our loop only looking occasionally gives a wide one. Both
        // average the same, so the average alone cannot say which (TEC-NATKIT-41).
        const auto &burst = imu.burstStats();
        if (burst.gaps > 0 && elapsed_us > 0) {
          ESP_LOGI(kTag,
                   "accel gaps: %lu | avg %lu us, min %lu, max %lu | %lu%% under "
                   "2 ms (same burst) | sh2_service %.0f/s, %lu productive",
                   static_cast<unsigned long>(burst.gaps),
                   static_cast<unsigned long>(burst.gap_sum_us / burst.gaps),
                   static_cast<unsigned long>(burst.gap_min_us),
                   static_cast<unsigned long>(burst.gap_max_us),
                   static_cast<unsigned long>(100UL * burst.gaps_under_2ms /
                                              burst.gaps),
                   burst.service_calls * 1e6f / static_cast<float>(elapsed_us),
                   static_cast<unsigned long>(burst.productive_calls));
        }
        imu.resetBurstStats();
        ESP_LOGI(kTag,
                 "reports: %s   (asked %.0f/%.0f/%.0f/%.0f Hz, 0 = off)",
                 breakdown, askedHz(CONFIG_NATKIT_IMU_INTERVAL_ACCEL_US),
                 askedHz(CONFIG_NATKIT_IMU_INTERVAL_GYRO_US),
                 askedHz(CONFIG_NATKIT_IMU_INTERVAL_MAG_US),
                 askedHz(CONFIG_NATKIT_IMU_INTERVAL_QUAT_US));
      }
      ESP_LOGI(kTag,
               "sensor: %lu reports @ %.1f Hz | resets %lu | cal %s | heap "
               "%" PRIu32 " B (min %" PRIu32 " B)",
               static_cast<unsigned long>(total), report_hz,
               static_cast<unsigned long>(imu.resetCount()),
               imu.calibrationEnabled() ? "on" : "pending",
               static_cast<uint32_t>(esp_get_free_heap_size()),
               static_cast<uint32_t>(esp_get_minimum_free_heap_size()));
      }
      // Data frames and total packets are labelled separately on purpose: the leaf
      // also sends heartbeats and announces down this path, so a single "sent"
      // figure next to the frame count reads as though more frames were sent than
      // were ever built.
      // Put the link state on the board's own LED, so "which one of these is the
      // problem" is answerable on the bench rather than only in the panel
      // (TEC-NATKIT-84). ⚠️ Called every pass but writes only on a CHANGE -- see
      // statusLedShowFault.
      statusLedShowFault(!espNowLinkHasPrimary()
                             ? LinkFault::kNoPrimary
                             : link.primary_absent
                                   ? LinkFault::kUnheardByPrimary
                                   : LinkFault::kNone);

      ESP_LOGI(kTag,
               "link: %s | data frames built %lu @ %.1f/s | packets sent %lu, "
               "dropped %lu, tx failures %lu, retries %lu, announces %lu",
               !espNowLinkHasPrimary() ? "SEARCHING for a primary"
                   : link.primary_absent ? "primary PRESUMED GONE (unicast, 1 try)"
                                         : "primary known (unicast)",
               static_cast<unsigned long>(sFramesBuilt), frame_hz,
               static_cast<unsigned long>(link.packets_sent),
               static_cast<unsigned long>(link.packets_dropped),
               static_cast<unsigned long>(link.send_failures),
               static_cast<unsigned long>(link.send_retries),
               static_cast<unsigned long>(link.announces));

      if (link.rssi_seen) {
        ESP_LOGI(kTag,
                 "link: primary heard at %d dBm (best %d, worst %d) | our tx "
                 "power %.1f dBm",
                 link.rssi_last, link.rssi_best, link.rssi_worst,
                 link.tx_power_quarter_dbm / 4.0);
      }
      // How often the sample loop lost a slot. At 10 ms (#380) a single
      // imu.service() that overruns costs a sample, and 9% of slots missed is
      // exactly the gap between 100 samples/s declared and 91 produced.
      ESP_LOGI(kTag, "sample loop: %lu slots missed of ~%lu due (%.1f%%)",
               static_cast<unsigned long>(sMissedSlots),
               static_cast<unsigned long>(now / kSampleIntervalUs),
               100.0 * sMissedSlots /
                   (now / kSampleIntervalUs > 0 ? now / kSampleIntervalUs : 1));
      // The clock fit (#340). Printed next to the link line because the two fail
      // together: a leaf that has lost its primary stops being able to say when
      // anything happened as well as where it went.
      const TimeSyncStatus &sync = timeSyncStatus();
      const uint64_t beacon_age_ms =
          sync.last_beacon_local_us == 0
              ? 0
              : (now - sync.last_beacon_local_us) / 1000;
      ESP_LOGI(kTag,
               "clock: %s vs primary epoch %08lx | offset %+lld us, skew %+ld "
               "ppb | fit %u pts, residual %lu ns rms (peak %lu ns) | beacons "
               "%lu seen, %lu missed, pairs %lu used, %lu orphaned, %lu "
               "outliers | last beacon %llu ms ago | rx callback jitter %lu us",
               syncQualityName(sync.quality),
               static_cast<unsigned long>(sync.epoch),
               static_cast<long long>(sync.ref_offset_us),
               static_cast<long>(sync.skew_ppb),
               static_cast<unsigned>(sync.samples_used),
               static_cast<unsigned long>(sync.residual_rms_ns),
               static_cast<unsigned long>(sync.peak_residual_ns),
               static_cast<unsigned long>(sync.beacons_seen),
               static_cast<unsigned long>(sync.beacons_missed),
               static_cast<unsigned long>(sync.pairs_used),
               static_cast<unsigned long>(sync.pairs_orphaned),
               static_cast<unsigned long>(sync.outliers_rejected),
               static_cast<unsigned long long>(beacon_age_ms),
               static_cast<unsigned long>(sync.mac_spread_us));
      // ⚠️ The two counters that say WHY a window was thrown away. Without them
      // a fit that keeps restarting from a handful of points is a mystery: the
      // leaf reports "0 outliers" while its window is plainly being cleared, and
      // the remaining causes -- a primary reboot, or a fit rejected as
      // implausible -- were both invisible.
      ESP_LOGI(kTag,
               "clock: window resets -- %lu implausible fits, %lu epoch changes "
               "| pairs %lu used of %lu beacons",
               static_cast<unsigned long>(sync.implausible_fits),
               static_cast<unsigned long>(sync.epoch_changes),
               static_cast<unsigned long>(sync.pairs_used),
               static_cast<unsigned long>(sync.beacons_seen));
      // The number that justifies the two-step protocol: how long the primary's
      // beacon sat between being handed to esp_now_send and actually going out.
      // That delay is what a one-step beacon would have folded straight into the
      // offset estimate, so if it were small the follow-up packet would be buying
      // nothing -- and that should be visible rather than asserted.
      if (sync.queue_delay_seen) {
        ESP_LOGI(kTag,
                 "clock: primary tx queue delay %lu us now, %lu..%lu us seen -- "
                 "this is what the beacon/follow-up split keeps out of the fit "
                 "| MAC rx stamp - esp_timer = %+lld us (spread %lu us)",
                 static_cast<unsigned long>(sync.queue_delay_us),
                 static_cast<unsigned long>(sync.queue_delay_min_us),
                 static_cast<unsigned long>(sync.queue_delay_max_us),
                 static_cast<long long>(sync.mac_minus_timer_us),
                 static_cast<unsigned long>(sync.mac_spread_us));
      // ⚠️ The two counters that say WHY a window was thrown away. Without them
      // a fit that keeps restarting from a handful of points is a mystery: the
      // leaf reports "0 outliers" while its window is plainly being cleared, and
      // the remaining causes -- a primary reboot, or a fit rejected as
      // implausible -- were both invisible.
      ESP_LOGI(kTag,
               "clock: window resets -- %lu implausible fits, %lu epoch changes "
               "| pairs %lu used of %lu beacons",
               static_cast<unsigned long>(sync.implausible_fits),
               static_cast<unsigned long>(sync.epoch_changes),
               static_cast<unsigned long>(sync.pairs_used),
               static_cast<unsigned long>(sync.beacons_seen));
      }

      reports_at_last_log = total;
      frames_at_last_log = sFramesBuilt;
      last_log_us = now;
      next_log_us = now + kLogIntervalUs;
    }

    // A plain tick. The deadline-aware version this replaces was left behind when
    // sampling moved to its own task: it computed its wait from `next_sample_us`,
    // which nothing advances any more, so `remaining` was always 0 and this
    // became a BUSY SPIN calling imu.service() as fast as the CPU allowed. That
    // hammered SPI continuously and cost 81% of received beacons -- at -22 dBm,
    // so it read as a radio problem and was not one.
    //
    // Sampling no longer depends on this loop's timing, so the pump only has to
    // keep up with the hub's reports.
    vTaskDelay(kPumpDelay);
  }
}

}  // namespace natkit
