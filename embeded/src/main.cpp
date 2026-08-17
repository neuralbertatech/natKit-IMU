#define USE_FREE_RTOS_LOCKS

#include <WiFi.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ConnectionConfig.hpp>
#include <BoardConfig.hpp>
#include <macros.hpp>
#include <kafkaTopic.hpp>
#include <version.hpp>

// Development configuration - copy DevConfig.hpp.example to DevConfig.hpp
// and update with your local settings (DevConfig.hpp is gitignored)
#ifndef ENABLE_CAPTIVE_PORTAL
#if __has_include(<DevConfig.hpp>)
#include <DevConfig.hpp>
#else
#error "DevConfig.hpp not found. Copy DevConfig.hpp.example to DevConfig.hpp and configure your settings."
#endif
#endif

//#include <Arduino.h> //not needed in the arduino ide

#ifdef ENABLE_CAPTIVE_PORTAL
//Captive Portal
#include <DNSServer.h>
#include <esp_wifi.h> //Used for mpdu_rx_disable android workaround
#include <AsyncTCP.h> 	//https://github.com/me-no-dev/AsyncTCP using the latest dev version from @me-no-dev
#include <ESPAsyncWebServer.h> //https://github.com/me-no-dev/ESPAsyncWebServer using the latest dev version from @me-no-dev
#include <HTTPClient.h>
#endif // ENABLE_CAPTIVE_PORTAL

#include <esp_random.h>
#include <esp_mac.h>       // esp_efuse_mac_get_default moved here in IDF 5.x
#include <esp_task_wdt.h>
#include <ImuData.hpp>
#include <ImuReader.hpp>
#include <libnatkit-core.hpp>
#include <Time.hpp>
#include <esp_timer.h>
#include <PubSubClient.h>
// #include "mqtt_client.h"
#include "esp_netif.h"
//#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <ESPNtpClient.h>

#ifdef USE_FREE_RTOS_LOCKS
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <mutex>
#endif

// Dependency Graph (these are the libary versions used by this version of the code)
// |-- AsyncTCP @ 1.1.1+sha.ca8ac5f //Latest version of the main branch
// |-- ESP Async WebServer @ 1.2.3+sha.f71e3d4 //Latest version of the main branch
// |   |-- AsyncTCP @ 1.1.1+sha.ca8ac5f
// |   |-- FS @ 2.0.0
// |   |-- WiFi @ 2.0.0
// |-- DNSServer @ 2.0.0
// |   |-- WiFi @ 2.0.0

//Pre reading on the fundamentals of captive portals https://textslashplain.com/2022/06/24/captive-portals/



//const char* ntpServer = "pool.ntp.org";
// const long  gmtOffset_sec = 3600;
// const int   daylightOffset_sec = 3600;
const char* ntpServer = nullptr;
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;

#ifdef ENABLE_CAPTIVE_PORTAL
char ssid[32];
const char * password = NULL; // no password
#endif // ENABLE_CAPTIVE_PORTAL

// Network configuration
#define MAX_CLIENTS 4  // ESP32 supports up to 10 but not tested
#define WIFI_CHANNEL 6 // 2.4GHz channel 6

// MQTT configuration
constexpr uint16_t MQTT_BUFFER_SIZE = 16384;          // Buffer size for MQTT messages
constexpr uint16_t MQTT_CONNECT_BUFFER_SIZE = 6200;   // Buffer size during connection phase
constexpr uint8_t MQTT_MAX_CONNECT_RETRIES = 30;      // Max connection attempts before giving up
constexpr uint16_t MQTT_CONNECT_RETRY_DELAY_MS = 500; // Delay between connection retries

// FreeRTOS task stack sizes (in bytes)
constexpr uint32_t SEND_MESSAGE_TASK_STACK_SIZE = 18432;  // 16384 + 2048 for sendMessageTask
constexpr uint32_t AP_REQUESTS_TASK_STACK_SIZE = 16384;   // For handleApRequestsTask
constexpr uint32_t NTP_TASK_STACK_SIZE = 2048;            // For handleNtpTask

// xTaskCreate expects stack depth in words, not bytes.
constexpr uint32_t stackBytesToWords(uint32_t bytes) {
  return (bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t);
}

// WiFi reconnection settings
constexpr uint8_t WIFI_MAX_RECONNECT_ATTEMPTS = 10;       // Max reconnection attempts before restart
constexpr uint16_t WIFI_RECONNECT_DELAY_MS = 5000;        // Delay between reconnection attempts

// Watchdog timer settings
constexpr uint32_t WATCHDOG_TIMEOUT_SEC = 30;             // Watchdog timeout in seconds

// NTP synchronization settings
constexpr uint8_t NTP_MAX_WAIT_SECONDS = 60;              // Max time to wait for NTP sync before proceeding


const IPAddress localIP(4, 3, 2, 1); // the IP address the web server, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1); // IP address of the network should be the same as the local IP for captive portals
const IPAddress subnetMask(255,255,255,0); //no need to change: https://avinetworks.com/glossary/subnet-mask/

const String localIPURL = "http://4.3.2.1"; //a string version of the local IP with http, used for redirecting clients to your webpage

uint8_t MAC_ADDRESS[6];
uint64_t UNIQUE_ID;

#ifdef ENABLE_CAPTIVE_PORTAL
//WARNING IOS (and maybe macos) WILL NOT POP UP IF IT CONTAINS THE WORD "Success" https://www.esp8266.com/viewtopic.php?f=34&t=4398
//SAFARI (IOS) there is a 128KB limit to the size of the HTML. The HTML can reference external resources/images that bring the total over 128KB
//SAFARI (IOS) popup browser has some severe limitations (javascript disabled, cookies disabled, no .gz extension (even though gzip files are supported))
const char indexHtml[] PROGMEM = R"=====(
  <!DOCTYPE html> <html>
    <head>
      <title>natKit ESP32 Captive Portal</title>
      <style>
        body {background-color:#1B9FD6;}
        h1 {color: #202020;}
        h2 {color: #202020;}
      </style>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
    </head>
    <body>
      <h1>ESP32 Configuration Panel</h1>
      <form action="/action">
        <label for="networkSsid">SSID:</label><br>
        <input type="text" id="networkSsid" name="networkSsid" value="selk"><br>
        <label for="networkPassword">Password:</label><br>
        <input type="password" id="networkPassword" name="networkPassword" value=""><br>
        <label for="natKitServerAddress">natKit Core Server Address:</label><br>
        <input type="text" id="natKitServerAddress" name="natKitServerAddress" value=""><br>
        <label for="natKitServerPort">natKit Core Server Port:</label><br>
        <input type="text" id="natKitServerPort" name="natKitServerPort" value="38082"><br><br>
        <input type="submit" value="Submit">
      </form>
    </body>
  </html>
)=====";
const char formCompletionResponseHtml[] PROGMEM = R"=====(
  <!DOCTYPE html> <html>
    <head>
      <title>natKit ESP32 Captive Portal</title>
      <style>
        body {background-color:#1B9FD6;}
        h1 {color: #202020;}
        h2 {color: #202020;}
      </style>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
    </head>
    <body>
      <h1>Form Submitted, Please Wait While Configuring...</h1>
    </body>
  </html>
)=====";

DNSServer dnsServer;
AsyncWebServer server(80);
#endif // ENABLE_CAPTIVE_PORTAL

ConnectionConfig connectionConfig{};

WiFiClient wifiClient;
PubSubClient mqttClient{};

// WiFi connection state (volatile for ISR access)
volatile bool wifiConnected = false;
volatile bool wifiReconnectNeeded = false;
uint8_t wifiReconnectAttempts = 0;

enum class NetworkingStage {
  Disconnected,
  RecievedWifiCredentials,
  WriteData
};
NetworkingStage currentNetworkingStage = NetworkingStage::Disconnected;
KafkaTopic* kafkaTopic = nullptr;

ImuReader imuReader{};
// TODO: Create a list of these objects so they can be queued
ImuData imuData{};
int imuCalibration{0};
bool IMU_DUMMY_DATA{false};

TaskHandle_t sendMessageTaskHandle = NULL;
TaskHandle_t networkingAndImuTaskHandle = NULL;

// --- deferred dynamic-calibration enable ---------------------------------
// The BNO08x will not accept sh2_setCalConfig during bring-up: every position in
// setup() either returns SH2_ERR_HUB or makes the hub stop producing reports
// (all three measured -- see the NOTE in Bno08xDevice2.hpp). Issued once the hub
// has actually been streaming for a few seconds, the same call succeeds and gyro
// accuracy reaches High within a second.
//
// Without this the hub runs on its default 0x05 (accel|mag, gyro OFF), which is
// what made the rotation vector sit at Unreliable no matter how carefully the
// board was calibrated by hand.
constexpr uint32_t CALIBRATION_ENABLE_AFTER_STREAMING_MS = 5000;
constexpr uint32_t CALIBRATION_ENABLE_RETRY_MS = 5000;
constexpr uint8_t CALIBRATION_ENABLE_MAX_ATTEMPTS = 3;

uint32_t firstSampleAtMs = 0;
uint32_t samplesRead = 0;

void noteSampleRead() {
  ++samplesRead;
  if (firstSampleAtMs == 0) {
    firstSampleAtMs = millis();
  }
}

void enableDynamicCalibrationOnce() {
  static bool settled = false;
  static uint8_t attempts = 0;
  static uint32_t lastAttemptMs = 0;

  if (settled || firstSampleAtMs == 0) {
    return;
  }
  const uint32_t nowMs = millis();
  if (nowMs - firstSampleAtMs < CALIBRATION_ENABLE_AFTER_STREAMING_MS) {
    return;
  }
  if (attempts > 0 && nowMs - lastAttemptMs < CALIBRATION_ENABLE_RETRY_MS) {
    return;
  }

  ++attempts;
  lastAttemptMs = nowMs;
  const uint8_t desired = 0x07;  // SH2_CAL_ACCEL | SH2_CAL_GYRO | SH2_CAL_MAG
  const int status = imuReader.setCalibrationConfig(desired);
  uint8_t readBack = 0;
  imuReader.getCalibrationConfig(readBack);

  if (status == 0) {
    settled = true;
    DEBUG_SERIAL.printf(
        "BNO08X: dynamic calibration enabled after %lu samples "
        "(setCalConfig(0x%02x) -> 0, read-back 0x%02x)\n",
        (unsigned long)samplesRead, desired, readBack);
    // Reported on the log channel too, so this is visible from the server
    // without a serial cable. command_id is empty: nobody asked for it.
    natkit_command::emitLog("", "calibrate.auto", "info", true, true,
                            "dynamic calibration enabled: 0x%02x (read-back "
                            "0x%02x) after %lu samples",
                            desired, readBack, (unsigned long)samplesRead);
    return;
  }

  DEBUG_SERIAL.printf(
      "BNO08X: setCalConfig(0x%02x) attempt %u FAILED with %d\n", desired,
      attempts, status);
  if (attempts >= CALIBRATION_ENABLE_MAX_ATTEMPTS) {
    settled = true;  // stop trying; say so once, loudly.
    DEBUG_SERIAL.println(
        "BNO08X: giving up on enabling dynamic calibration. Gyro calibration is "
        "OFF, so rotation will stay Unreliable. Try calibrate.set_config over "
        "the command channel.");
    natkit_command::emitLog(
        "", "calibrate.auto", "error", false, true,
        "could not enable dynamic calibration after %u attempts (last error "
        "%d); gyro calibration is OFF and rotation will stay Unreliable",
        attempts, status);
  }
}

// --- EXECUTION_COMMAND handlers ------------------------------------------
// Runs on the networking/IMU task, because these touch the SH2 hub. Output goes
// out on the log channel, correlated by command_id; nothing is published from
// here directly (that would race the MQTT-owning task).
void executeCommand(const natkit_command::CommandRequest& request) {
  using natkit_command::emitLog;
  const char* id = request.command_id;
  const char* command = request.command;

  if (strcmp(command, "ping") == 0) {
    emitLog(id, command, "info", true, true, "pong from %s, up %lu ms",
            kafkaTopic != nullptr ? kafkaTopic->getName().c_str() : "device",
            (unsigned long)millis());
    return;
  }

  if (strcmp(command, "calibrate.save_dcd") == 0) {
    // Datasheet §3.4: the hub only writes dynamic calibration to FRS on a
    // non-power-up reset, so a device that is just switched off loses what it
    // learned since boot. This is the explicit save for that case.
    const int status = imuReader.saveCalibrationNow();
    if (status == 0) {
      emitLog(id, command, "info", true, true,
              "saved dynamic calibration to flash");
    } else {
      emitLog(id, command, "error", false, true,
              "sh2_saveDcdNow failed with %d", status);
    }
    return;
  }

  if (strcmp(command, "imu.diag") == 0) {
    // Raw per-report view. The packed accuracies byte cannot tell "the hub says
    // Unreliable" from "no such report has ever arrived", and the rotation
    // vector's own error estimate (radians) is not in the status bits at all --
    // which is exactly the distinction needed when rotation will not leave 0.
    const auto& d = imuReader.getSensorDiagnostics();
    const auto describe = [](uint8_t status) -> int {
      return status == 0xff ? -1 : (status & 0x03);
    };
    emitLog(id, command, "info", true, true,
            "accel[n=%lu st=0x%02x acc=%d] gyro[n=%lu st=0x%02x acc=%d] "
            "mag[n=%lu st=0x%02x acc=%d] rot[n=%lu st=0x%02x acc=%d err=%.4frad]",
            (unsigned long)d.count_accel, d.last_status_accel,
            describe(d.last_status_accel), (unsigned long)d.count_gyro,
            d.last_status_gyro, describe(d.last_status_gyro),
            (unsigned long)d.count_magnetometer, d.last_status_magnetometer,
            describe(d.last_status_magnetometer),
            (unsigned long)d.count_rotation, d.last_status_rotation,
            describe(d.last_status_rotation), d.last_rotation_accuracy_rad);
    return;
  }

  if (strcmp(command, "calibrate.set_config") == 0) {
    // args: {"mask": 7} or {"mask": "0x07"}. Default enables accel+gyro+mag,
    // which is what setup() asks for and fails to get.
    long requested = 0x07;
    natkit_command::readNumberField(request.args, "mask", requested);
    const uint8_t mask = static_cast<uint8_t>(requested & 0x0f);
    const int status = imuReader.setCalibrationConfig(mask);
    uint8_t read_back = 0;
    const int read_status = imuReader.getCalibrationConfig(read_back);
    // Both numbers are reported because they disagree on this hub: the read-back
    // has been observed to stay 0x05 no matter what was written, so it cannot be
    // used to confirm the write took. The RETURN CODE is the real signal.
    emitLog(id, command, status == 0 ? "info" : "error", status == 0, true,
            "setCalConfig(0x%02x) returned %d; read-back %s0x%02x (accel=%d "
            "gyro=%d mag=%d)",
            mask, status, read_status == 0 ? "" : "unavailable, ", read_back,
            (read_back & 0x01) ? 1 : 0, (read_back & 0x02) ? 1 : 0,
            (read_back & 0x04) ? 1 : 0);
    return;
  }

  if (strcmp(command, "calibrate.status") == 0) {
    uint8_t mask = 0;
    const int status = imuReader.getCalibrationConfig(mask);
    // Accuracy bits, as packed by NatImuDataSchema: 5-4 accel, 3-2 gyro,
    // 1-0 rotation. 0 = Unreliable ... 3 = High.
    const uint8_t accuracies = imuData.accuracies;
    if (status == 0) {
      emitLog(id, command, "info", true, true,
              "cal_config=0x%02x (accel=%d gyro=%d mag=%d) accuracy accel=%d "
              "gyro=%d rotation=%d has_data=0x%02x",
              mask, (mask & 0x01) ? 1 : 0, (mask & 0x02) ? 1 : 0,
              (mask & 0x04) ? 1 : 0, (accuracies >> 4) & 0x03,
              (accuracies >> 2) & 0x03, accuracies & 0x03, imuData.has_data);
    } else {
      emitLog(id, command, "error", false, true,
              "sh2_getCalConfig failed with %d", status);
    }
    return;
  }

  emitLog(id, command, "error", false, true, "unknown command \"%s\"", command);
}

// Drains whatever the server has queued for us. Bounded per pass so a burst
// cannot stall the sample loop.
void dispatchPendingCommands() {
  constexpr uint8_t MAX_COMMANDS_PER_PASS = 2;
  natkit_command::CommandRequest request{};
  for (uint8_t handled = 0; handled < MAX_COMMANDS_PER_PASS; ++handled) {
    if (!natkit_command::tryTakeRequest(request)) {
      return;
    }
    DEBUG_SERIAL.printf("COMMAND: executing %s\n", request.command);
    executeCommand(request);
  }
}

// FreeRTOS task stack size for networking/IMU task
constexpr uint32_t NETWORKING_IMU_TASK_STACK_SIZE = 16384;

#ifdef ENABLE_CAPTIVE_PORTAL
void handleApRequestsTask(void*) {
  const auto delay = 1000 / portTICK_PERIOD_MS; // 1s
  while(true) {
    dnsServer.processNextRequest(); //I call this atleast every 10ms in my other projects (can be higher but I haven't tested it for stability)
    vTaskDelay(delay);
  }

  vTaskDelete( NULL );
}
#endif // ENABLE_CAPTIVE_PORTAL

void sendMessageTask(void*) {
  while (true) {
    // Wake on a "bulk ready" notification, or at least every 200ms so the MQTT
    // link is serviced (keepalive PINGs + dropped-link detection/reconnect) even
    // when the sensor produces no data for a while. All MQTT I/O stays on this
    // one task because PubSubClient is not thread-safe.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    // Only own MQTT once the other task has finished the initial connect + meta
    // publish (WriteData stage). Before that, mqttClient belongs to the
    // networking-stage task and touching it here would race.
    if (kafkaTopic != nullptr && currentNetworkingStage == NetworkingStage::WriteData) {
      kafkaTopic->serviceConnection(connectionConfig, mqttClient);
      kafkaTopic->writeBulkDataRecord(connectionConfig, mqttClient);
      // Command output, produced on the IMU task, is published from here so
      // PubSubClient keeps a single writer.
      natkit_command::CommandLog logRecord{};
      while (natkit_command::tryTakeLog(logRecord)) {
        kafkaTopic->writeLogRecord(logRecord, mqttClient);
      }
    }
  }
}

// Could use an atomic_thread_fence instead
#ifdef USE_FREE_RTOS_LOCKS
SemaphoreHandle_t has_ntp_update_happened_yet_lock;
#else
std::mutex has_ntp_update_happened_yet_lock{};
#endif // USE_FREE_RTOS_LOCKS
bool has_ntp_update_happened_yet = true;

// Could use an atomic_thread_fence instead
#ifdef USE_FREE_RTOS_LOCKS
SemaphoreHandle_t local_time_offset_lock;
#else
std::mutex local_time_offset_lock{};
#endif // USE_FREE_RTOS_LOCKS
int64_t local_time_offset_us = 0;

void calculateLocalTimeOffset() {
  int64_t timestamp_before_us = esp_timer_get_time();
  int64_t current_ntp_time_us = getTimeNowAsUs();
  int64_t timestamp_after_us = esp_timer_get_time();
  int64_t average_timestamp = timestamp_before_us + ((timestamp_after_us - timestamp_before_us) / 2);

  // TODO: Interpolate if time change is too much
  {
    #ifdef USE_FREE_RTOS_LOCKS
    xSemaphoreTake(local_time_offset_lock, portMAX_DELAY);
    #else
    std::lock_guard<std::mutex> guard(local_time_offset_lock);
    #endif // USE_FREE_RTOS_LOCKS
    local_time_offset_us = current_ntp_time_us - average_timestamp;
    #ifdef USE_FREE_RTOS_LOCKS
    xSemaphoreGive(local_time_offset_lock);
    #endif // USE_FREE_RTOS_LOCKS
  }
}

void ntpSyncNotification(struct timeval* tv) {
  DEBUG_SERIAL.println("ntpSyncNotification");
  calculateLocalTimeOffset();
  {
    #ifdef USE_FREE_RTOS_LOCKS
    xSemaphoreTake(has_ntp_update_happened_yet_lock, portMAX_DELAY);
    #else
    std::lock_guard<std::mutex> guard(has_ntp_update_happened_yet_lock);
    #endif // USE_FREE_RTOS_LOCKS
    has_ntp_update_happened_yet = true;
    #ifdef USE_FREE_RTOS_LOCKS
    xSemaphoreGive(has_ntp_update_happened_yet_lock);
    #endif // USE_FREE_RTOS_LOCKS
  }
  DEBUG_SERIAL.println("NTP Time Callback was hit");
}

void processSyncEvent (NTPEvent_t ntpEvent) {
  switch (ntpEvent.event) {
      case timeSyncd: {
        DEBUG_SERIAL.println("ntpSyncNotification");
        calculateLocalTimeOffset();
        {
          #ifdef USE_FREE_RTOS_LOCKS
          xSemaphoreTake(has_ntp_update_happened_yet_lock, portMAX_DELAY);
          #else
          std::lock_guard<std::mutex> guard(has_ntp_update_happened_yet_lock);
          #endif // USE_FREE_RTOS_LOCKS
          has_ntp_update_happened_yet = true;
          #ifdef USE_FREE_RTOS_LOCKS
          xSemaphoreGive(has_ntp_update_happened_yet_lock);
          #endif // USE_FREE_RTOS_LOCKS
        }
        DEBUG_SERIAL.println("NTP Time Callback was hit");
        break;
      }

      case partlySync:
      case syncNotNeeded:
      case accuracyError:
          DEBUG_SERIAL.printf ("[NTP-event] %s\n", NTP.ntpEvent2str (ntpEvent));
          break;
      default:
          break;
  }
}

int64_t getAdjustedLocalTimeUs() {
  int64_t offset = 0;
  {
    #ifdef USE_FREE_RTOS_LOCKS
    xSemaphoreTake(local_time_offset_lock, portMAX_DELAY);
    #else
    std::lock_guard<std::mutex> guard(local_time_offset_lock);
    #endif // USE_FREE_RTOS_LOCKS
    offset = local_time_offset_us;
    #ifdef USE_FREE_RTOS_LOCKS
    xSemaphoreGive(local_time_offset_lock);
    #endif // USE_FREE_RTOS_LOCKS
  }

  return offset + esp_timer_get_time();
}

void handleNtpTask(void*) {
  const auto delay = (1000 / portTICK_PERIOD_MS) * 10; // 10s

  NTP.onNTPSyncEvent ([] (NTPEvent_t event) {
    processSyncEvent(event);
  });
  NTP.setTimeZone (TZ_Etc_UTC);
  NTP.setInterval (10*1000); // 10s
  NTP.setNTPTimeout (5000);
  // NTP.setMinSyncAccuracy (5000);
  // NTP.settimeSyncThreshold (3000);
  NTP.begin (ntpServer);


  DEBUG_SERIAL.printf("NTP Was setup using %s\n", ntpServer);

  vTaskDelete( NULL );
}


void handleNetworkingStagesAndImuJoinedTask(void*) {
  const auto delay_len = 10 / portTICK_PERIOD_MS; // 10ms
  const auto delay_len_us = IMU_DELAY_BETWEEN_POLL_US;
  const auto half_delay_len_us = IMU_DELAY_BETWEEN_POLL_US / 2;
  int task_delta = 0;
  uint64_t last_timestamp = 0;
  uint64_t current_timestamp = 0;
  Serial.println("Starting Network!");
  uint64_t next_expected_reading_timestamp = 0;
  while(true) {
    last_timestamp = esp_timer_get_time();
    switch(currentNetworkingStage) {
      case NetworkingStage::Disconnected:
        break;

      case NetworkingStage::RecievedWifiCredentials:
        if (connectionConfig.networkSsid != nullptr && connectionConfig.networkPassword != nullptr) {
          WiFi.begin(connectionConfig.networkSsid, connectionConfig.networkPassword);
          while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(delay_len*100);
            DEBUG_SERIAL.println("Connecting to WiFi..");
          }
          IPAddress ipAddress{};
          ipAddress.fromString(connectionConfig.natKitServerAddress);
          mqttClient.setClient(wifiClient);
          mqttClient.setServer(ipAddress, NATKIT_MQTT_PORT);
          if (!mqttClient.setBufferSize(MQTT_CONNECT_BUFFER_SIZE)) {
            DEBUG_SERIAL.println("Failed to set MQTT buffer size");
            currentNetworkingStage = NetworkingStage::Disconnected;
            break;
          }

          // Connect to MQTT with retry limit
          uint8_t mqttRetries = 0;
          bool mqttConnectRetryExhausted = false;
          while (!mqttClient.connect(natkitMqttClientId())) {
            mqttRetries++;
            if (mqttRetries >= MQTT_MAX_CONNECT_RETRIES) {
              // Don't strand the device (the old code went to a dead Disconnected
              // state, needing a manual reset if the broker was down at boot).
              // Back off and re-run the whole WiFi+MQTT bring-up from the top;
              // nothing has been allocated yet (kafkaTopic is created only after a
              // successful connect below), so retrying forever is safe.
              DEBUG_SERIAL.println("MQTT connect failed after max retries; backing off and retrying bring-up...");
              vTaskDelay((MQTT_CONNECT_RETRY_DELAY_MS * 4) / portTICK_PERIOD_MS);
              currentNetworkingStage = NetworkingStage::RecievedWifiCredentials;
              mqttConnectRetryExhausted = true;
              break;
            }
            DEBUG_SERIAL.printf("Connecting to MQTT... (attempt %d/%d)\n", mqttRetries, MQTT_MAX_CONNECT_RETRIES);
            vTaskDelay(MQTT_CONNECT_RETRY_DELAY_MS / portTICK_PERIOD_MS);
          }
          if (mqttConnectRetryExhausted) {
            break; // re-enter RecievedWifiCredentials next loop and try again
          }

          static std::string ntpServerAddressString = connectionConfig.natKitServerAddress;
          ntpServer = ntpServerAddressString.c_str();

          DEBUG_SERIAL.print("ESP32 IP on the WiFi network: ");
          DEBUG_SERIAL.println(WiFi.localIP());

          DEBUG_SERIAL.println("Creating Kafka topic...");
          const String boardId{UNIQUE_ID};

          kafkaTopic = KafkaTopic::create(UNIQUE_ID, boardId);
          DEBUG_SERIAL.println("Kafka topic created");

          // Command channel: queues first (the subscribe callback fires as soon
          // as we subscribe, and it enqueues), then the callback, then subscribe.
          if (!natkit_command::init(kafkaTopic->getName().c_str())) {
            DEBUG_SERIAL.println("Error: failed to create command channel queues");
          } else {
            mqttClient.setCallback(natkit_command::onMqttMessage);
            kafkaTopic->subscribeToCommands(mqttClient);
          }

          vTaskDelay(500);
          DEBUG_SERIAL.println("Writing meta record...");
          kafkaTopic->writeMetaRecord(connectionConfig, mqttClient);
          DEBUG_SERIAL.println("Meta record written");

            TaskHandle_t handleNtpTaskHandle = NULL;
            xTaskCreate(handleNtpTask, "handleNtpTask", stackBytesToWords(NTP_TASK_STACK_SIZE), NULL, tskIDLE_PRIORITY+1, &handleNtpTaskHandle);

            // Wait for NTP sync with timeout
            uint8_t ntpWaitSeconds = 0;
            bool ntpSynced = false;
            while (ntpWaitSeconds < NTP_MAX_WAIT_SECONDS) {
              {
                #ifdef USE_FREE_RTOS_LOCKS
                xSemaphoreTake(has_ntp_update_happened_yet_lock, portMAX_DELAY);
                #else
                std::lock_guard<std::mutex> guard(has_ntp_update_happened_yet_lock);
                #endif // USE_FREE_RTOS_LOCKS
                if (has_ntp_update_happened_yet) {
                  ntpSynced = true;
                  #ifdef USE_FREE_RTOS_LOCKS
                  xSemaphoreGive(has_ntp_update_happened_yet_lock);
                  #endif // USE_FREE_RTOS_LOCKS
                  break;
                }
                #ifdef USE_FREE_RTOS_LOCKS
                xSemaphoreGive(has_ntp_update_happened_yet_lock);
                #endif // USE_FREE_RTOS_LOCKS
              }
              DEBUG_SERIAL.printf("Waiting for NTP sync... (%d/%d sec)\n", ntpWaitSeconds, NTP_MAX_WAIT_SECONDS);
              vTaskDelay(1000 / portTICK_PERIOD_MS);
              ntpWaitSeconds++;
            }
            
            if (ntpSynced) {
              DEBUG_SERIAL.println("NTP synchronized successfully");
            } else {
              DEBUG_SERIAL.println("WARNING: NTP sync timeout - proceeding with local time");
              // Use local time as fallback - timestamps may be inaccurate
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);

            currentNetworkingStage = NetworkingStage::WriteData;
            next_expected_reading_timestamp = (((getAdjustedLocalTimeUs() + delay_len_us) / 1000000) + 1) * 1000000; // The start of the next second
        } else {
          DEBUG_SERIAL.println("Error: Either the network SSID or the network password was not set");
        }
        break;

      case NetworkingStage::WriteData:
        if (connectionConfig.networkSsid != nullptr && connectionConfig.networkPassword != nullptr) {
          // Server-issued commands run here rather than in the MQTT callback:
          // they talk to the SH2 hub, which belongs to this task.
          dispatchPendingCommands();
          imuReader.update3();
          bool isBulkReady = false;

          if (imuReader.getImuData(&imuData)) {
            noteSampleRead();
            isBulkReady = kafkaTopic->writeDataRecord(connectionConfig, imuData, mqttClient);
          } else {
            DEBUG_SERIAL.println("Nothing to read");
          }

          // Once the hub has been streaming for a few seconds, turn on dynamic
          // calibration -- the one moment it accepts the request.
          enableDynamicCalibrationOnce();

          if (isBulkReady) {
            if (sendMessageTaskHandle != NULL) {
              xTaskNotifyGive(sendMessageTaskHandle);
            } else {
              DEBUG_SERIAL.println("Send message task handle is null");
            }
          }
        }
        break;

    default:
        break;
    }

    // Handle WiFi reconnection if needed
    if (wifiReconnectNeeded && currentNetworkingStage == NetworkingStage::WriteData) {
      DEBUG_SERIAL.println("Attempting WiFi reconnection...");
      wifiReconnectAttempts++;
      
      if (wifiReconnectAttempts > WIFI_MAX_RECONNECT_ATTEMPTS) {
        DEBUG_SERIAL.println("Max reconnection attempts reached - restarting device");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        ESP.restart();
      }
      
      DEBUG_SERIAL.printf("Reconnection attempt %d/%d\n", wifiReconnectAttempts, WIFI_MAX_RECONNECT_ATTEMPTS);
      WiFi.disconnect();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      WiFi.begin(connectionConfig.networkSsid, connectionConfig.networkPassword);
      
      // Wait for connection with timeout
      int waitCount = 0;
      while (WiFi.status() != WL_CONNECTED && waitCount < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        waitCount++;
        DEBUG_SERIAL.print(".");
      }
      DEBUG_SERIAL.println();
      
      if (WiFi.status() == WL_CONNECTED) {
        DEBUG_SERIAL.println("WiFi reconnected successfully");
        wifiReconnectNeeded = false;
        wifiConnected = true;
        wifiReconnectAttempts = 0;
        // MQTT reconnection is handled by KafkaTopic::serviceConnection() on the
        // sender task (single MQTT owner) — don't touch mqttClient from this task
        // or it races the sender's publish/loop.
      } else {
        DEBUG_SERIAL.println("WiFi reconnection failed - will retry");
        vTaskDelay(WIFI_RECONNECT_DELAY_MS / portTICK_PERIOD_MS);
      }
    }
  }
}

void handleUpdateImuTask(void*) {
  const auto delayUs = reportIntervalUs;
  // const auto delay = (reportIntervalUs / 1000) / portTICK_PERIOD_MS; // 1s
  while(true) {
    const auto startTimeUs = esp_timer_get_time();
    imuReader.update();
    const auto endTimeUs = esp_timer_get_time();

    const auto delta = endTimeUs - startTimeUs;
    const auto delay = (std::max(0LL, delayUs - delta) / 1000) / portTICK_PERIOD_MS;
    vTaskDelay(delay);
  }

  vTaskDelete( NULL );
}

void WiFiEvent(WiFiEvent_t event)
{
  log_i("[WiFi-event] event: %d", event);
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      log_i("Obtained IP address");
      wifiConnected = true;
      wifiReconnectNeeded = false;
      wifiReconnectAttempts = 0;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      log_i("Disconnected from WiFi access point");
      wifiConnected = false;
      // Only trigger reconnect if we were previously in WriteData stage
      if (currentNetworkingStage == NetworkingStage::WriteData) {
        wifiReconnectNeeded = true;
        DEBUG_SERIAL.println("WiFi disconnected - reconnection needed");
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      log_i("Lost IP address");
      wifiConnected = false;
      break;
    default:
      // arduino-esp32 3.x renamed the WiFi-event enum to ARDUINO_EVENT_*; we only
      // act on the STA connectivity events above, so ignore the rest.
      break;
  }
}

void setup(){ //the order of the code is important and it is critical the the android workaround is after the dns and sofAP setup
  pinMode(2, OUTPUT);
  pinMode(12, OUTPUT);
  pinMode(13, OUTPUT);
  pinMode(27, OUTPUT);

  // Initialize watchdog timer. arduino-esp32 3.x already inits the Task WDT and
  // IDF 5.x takes a config struct (not the old (timeout, panic) signature), so
  // reconfigure it, then subscribe this task.
  esp_task_wdt_config_t twdtConfig = {};
  twdtConfig.timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000;
  twdtConfig.idle_core_mask = 0;
  twdtConfig.trigger_panic = true;
  esp_task_wdt_reconfigure(&twdtConfig);
  esp_task_wdt_add(NULL); // Add current task (loop task) to watchdog
  DEBUG_SERIAL.printf("Watchdog initialized with %d second timeout\n", WATCHDOG_TIMEOUT_SEC);

  #ifndef ENABLE_CAPTIVE_PORTAL
  // Development config loaded from DevConfig.hpp (gitignored)
  connectionConfig.networkSsid = DEV_WIFI_SSID;
  connectionConfig.networkPassword = DEV_WIFI_PASSWORD;
  connectionConfig.natKitServerAddress = DEV_NATKIT_SERVER_ADDRESS;
  connectionConfig.natKitServerPort = DEV_NATKIT_SERVER_PORT;
  currentNetworkingStage = NetworkingStage::RecievedWifiCredentials;
  #endif // ENABLE_CAPTIVE_PORTAL

  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  WiFi.onEvent(WiFiEvent);

  esp_efuse_mac_get_default(MAC_ADDRESS);
  UNIQUE_ID = (static_cast<uint64_t>(MAC_ADDRESS[0]) << 8*5) +
              (static_cast<uint64_t>(MAC_ADDRESS[1]) << 8*4) +
              (static_cast<uint64_t>(MAC_ADDRESS[2]) << 8*3) +
              (static_cast<uint64_t>(MAC_ADDRESS[3]) << 8*2) +
              (static_cast<uint64_t>(MAC_ADDRESS[4]) << 8*1) +
              (MAC_ADDRESS[5]);
  #if USE_SERIAL == true
  Serial.begin(115200);
  while (!Serial);
  Serial.printf("\n\nnatKit-IMU v%s (built %s %s)\n", NATKIT_IMU_VERSION_STRING, NATKIT_IMU_BUILD_DATE, NATKIT_IMU_BUILD_TIME);
  Serial.printf("Unique ID: %llu\n", UNIQUE_ID);
  #endif

  #ifdef USE_FREE_RTOS_LOCKS
  has_ntp_update_happened_yet_lock = xSemaphoreCreateMutex();
  if (has_ntp_update_happened_yet_lock == nullptr) {
    DEBUG_SERIAL.println("FATAL: Failed to create NTP update mutex");
    while(true) { vTaskDelay(1000 / portTICK_PERIOD_MS); } // Halt - unrecoverable
  }

  local_time_offset_lock = xSemaphoreCreateMutex();
  if (local_time_offset_lock == nullptr) {
    DEBUG_SERIAL.println("FATAL: Failed to create time offset mutex");
    while(true) { vTaskDelay(1000 / portTICK_PERIOD_MS); } // Halt - unrecoverable
  }
  #endif // USE_FREE_RTOS_LOCKS

  #ifdef ENABLE_CAPTIVE_PORTAL
  sprintf(ssid, "natKit-ESP32-%lld", UNIQUE_ID);
  #endif // ENABLE_CAPTIVE_PORTAL

  if (!IMU_DUMMY_DATA) {
    DEBUG_SERIAL.println("Starting IMU Reader");
    imuReader.start2();
    DEBUG_SERIAL.println("Finished starting IMU Reader");
  }

  #ifdef ENABLE_CAPTIVE_PORTAL
  WiFi.mode(WIFI_AP_STA); //access point mode
  WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
  WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, MAX_CLIENTS);

  dnsServer.setTTL(300); //set 5min client side cache for DNS
  dnsServer.start(53, "*", localIP); //if DNSServer is started with "*" for domain name, it will reply with provided IP to all DNS request

  //ampdu_rx_disable android workaround see https://github.com/espressif/arduino-esp32/issues/4423
  esp_wifi_stop();
  esp_wifi_deinit();

  wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();   //We use the default config ...
  my_config.ampdu_rx_enable = false;                           //... and modify only what we want.

  esp_wifi_init(&my_config); //set the new config
  esp_wifi_start(); //Restart WiFi
  delay(100); //this is necessary don't ask me why

	//Required
	server.on("/connecttest.txt",[](AsyncWebServerRequest *request){request->redirect("http://logout.net");}); //windows 11 captive portal workaround
	server.on("/wpad.dat",[](AsyncWebServerRequest *request){request->send(404);}); //Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)

	//Background responses: Probably not all are Required, but some are. Others might speed things up?
	//A Tier (commonly used by modern systems)
	server.on("/generate_204",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); // android captive portal redirect
	server.on("/redirect",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //microsoft redirect
	server.on("/hotspot-detect.html",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //apple call home
	server.on("/canonical.html",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //firefox captive portal call home
	server.on("/success.txt",[](AsyncWebServerRequest *request){request->send(200);}); //firefox captive portal call home
	server.on("/ncsi.txt",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //windows call home

  // Return 404 for favicon
  server.on("/favicon.ico",[](AsyncWebServerRequest *request){request->send(404);});

  //Serve Basic HTML Page
  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", indexHtml);
    response->addHeader("Cache-Control", "public,max-age=31536000"); //save this file to cache for 1 year (unless you refresh)
    request->send(response);
    DEBUG_SERIAL.println("Served Basic HTML Page");
  });

  // Respond to form
  server.on("/action", HTTP_ANY, [](AsyncWebServerRequest *request) {
      DEBUG_SERIAL.println("Rescieved Form Response");

      int params = request->params();
      for (int i = 0; i < params; ++i) {
        connectionConfig.configureSetting(request->getParam(i));
        //DEBUG_SERIAL.printf("POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
      }
      request->send_P(200, "text/html", formCompletionResponseHtml);
      currentNetworkingStage = NetworkingStage::RecievedWifiCredentials;
    }
  );

  //the catch all
  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect(localIPURL);
    DEBUG_SERIAL.print("onNotFound ");
    DEBUG_SERIAL.print(request->host());       //This gives some insight into whatever was being requested on the serial monitor
    DEBUG_SERIAL.print(" ");
    DEBUG_SERIAL.print(request->url());
    DEBUG_SERIAL.print(" sent redirect to " + localIPURL +"\n");
  });

  server.begin();
  #endif // ENABLE_CAPTIVE_PORTAL

  DEBUG_SERIAL.print("\n");
  DEBUG_SERIAL.print("Startup Time:"); //should be somewhere between 270-350 for Generic ESP32 (D0WDQ6 chip, can have a higher startup time on first boot)
  DEBUG_SERIAL.println(millis());
  DEBUG_SERIAL.print("\n");

  #ifdef ENABLE_CAPTIVE_PORTAL
  TaskHandle_t handleApRequestsTaskHandle = NULL;
  xTaskCreate(handleApRequestsTask, "HandleApRequestsTask", stackBytesToWords(AP_REQUESTS_TASK_STACK_SIZE), NULL, tskIDLE_PRIORITY, &handleApRequestsTaskHandle);
  #endif // ENABLE_CAPTIVE_PORTAL

  BaseType_t sendTaskResult = xTaskCreate(
    sendMessageTask,
    "SendMessageTask",
    stackBytesToWords(SEND_MESSAGE_TASK_STACK_SIZE),
    NULL,
    tskIDLE_PRIORITY + 2,
    &sendMessageTaskHandle
  );
  if (sendTaskResult != pdPASS) {
    DEBUG_SERIAL.println("FATAL: Failed to create send message task");
    while(true) { vTaskDelay(1000 / portTICK_PERIOD_MS); }
  }

  // Start the main networking and IMU task
  BaseType_t result = xTaskCreate(
    handleNetworkingStagesAndImuJoinedTask,
    "NetworkingImuTask",
    stackBytesToWords(NETWORKING_IMU_TASK_STACK_SIZE),
    NULL,
    tskIDLE_PRIORITY + 1,
    &networkingAndImuTaskHandle
  );
  if (result != pdPASS) {
    DEBUG_SERIAL.println("FATAL: Failed to create networking/IMU task");
    while(true) { vTaskDelay(1000 / portTICK_PERIOD_MS); } // Halt - unrecoverable
  }
  DEBUG_SERIAL.println("Networking/IMU task started");
}

void loop(){
  // Feed the watchdog - if this doesn't happen within WATCHDOG_TIMEOUT_SEC, device will restart
  esp_task_wdt_reset();
  
  // The main work is done in the networkingAndImuTask FreeRTOS task
  // This loop just needs to feed the watchdog periodically
  vTaskDelay(100 / portTICK_PERIOD_MS);
}
