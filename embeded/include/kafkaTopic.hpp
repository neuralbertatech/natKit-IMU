#pragma once

#include <Arduino.h>
#include <ConnectionConfig.hpp>
#include <macros.hpp>
//#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <CommandChannel.hpp>
#include <ImuData.hpp>
#include <libnatkit-core.hpp>
#include <PubSubClient.h>
// #include "mqtt_client.h"

#ifdef USE_FREE_RTOS_LOCKS
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <mutex>
#endif

// Wire-frame packaging. Modelled on the natVR EMG firmware, which flushes a small
// fixed-size frame the instant it fills (50 samples/frame → ~20 fps) and drains it
// with a notify-driven publish, so the live stream is smooth rather than bursty.
// The old 100-sample batch only produced ~2.6 frames/s (a ~5 KB burst every
// ~380 ms), which read as choppy in the viewer. The BNO08x merged accel/gyro/
// rotation stream runs ~250 Hz, so 10 samples/frame ≈ 25 fps of small (~0.5 KB)
// frames. This is the frame batch size ONLY — decoupled from the declared rate.
#define IMU_SAMPLES_PER_FRAME 10
// Declared sample rate advertised in the frame header + heartbeat (informational;
// each sample also carries its own timestamp, which is the source of truth for the
// viewer's time axis).
#define IMU_SAMPLE_RATE_HZ 100
#define IMU_DELAY_BETWEEN_POLL_US 1000
#define IMU_HEARTBEAT_INTERVAL_MS 1000

// NTP-adjusted wall-clock time in microseconds; defined in main.cpp.
int64_t getAdjustedLocalTimeUs();

char kafkaUrlBuffer[256];
byte kafkaRecordDataBuffer[6200];
//char mqttBuffer[16384];
uint32_t indexes[10];
uint32_t currentIndex = 0;
nat::core::NatImuDataSchema imuDataList[IMU_SAMPLES_PER_FRAME];
uint32_t currentImuDataIndex = 0;

class KafkaTopic {
    String name;
    nat::core::BasicMetaInfoSchema meta;
    String dataTopicString;
    String bulkDataTopicString;
    String metaTopicString;
    String statusTopicString;
    String commandTopicString;
    String logTopicString;
    // String clusterId;
    nat::core::Stream dataStream;
    nat::core::Stream metaStream;
    nat::core::Stream bulkDataStream;
    nat::core::Stream statusStream;
    nat::core::Stream commandStream;
    nat::core::Stream logStream;
    // Subscriptions do not survive a reconnect, so serviceConnection() re-subscribes.
    bool subscribedToCommands = false;

    nat::core::NatImuBulkDataSchema bulkImuData{};
    bool bulkImuDataReadyToSend;
    uint64_t bulkSeqNo = 0;       // monotonic per-device frame counter
    uint64_t heartbeatSeqNo = 0;  // monotonic per-device status counter
    uint32_t lastHeartbeatMs = 0; // millis() of the last status publish
    #ifdef USE_FREE_RTOS_LOCKS
    SemaphoreHandle_t bulkImuDataLock;
    #else
    std::mutex bulkImuDataLock{};
    #endif // USE_FREE_RTOS_LOCKS

    // static String createTopicPostStringTemplate;
    // static String createTopicUrlTemplate;
    // static String writeRecordUrlTemplate;
    // static String writeRecordPostTemplate;
    static String mqttUrlTemplate;
    // static String dataRecordDataTemplate;
    // static String metaRecordDataTemplate;
public:
    KafkaTopic(uint64_t id, const String& name)
      : name(name), meta(name.c_str()),
      dataStream(std::string(name.c_str()), nat::core::StreamType::DATA, id, nat::core::toString(nat::core::SerializationType::Json), nat::core::NatImuDataSchema::name),
      metaStream(std::string(name.c_str()), nat::core::StreamType::META, id, nat::core::toString(nat::core::SerializationType::Json), nat::core::BasicMetaInfoSchema::name),
      bulkDataStream(std::string(name.c_str()), nat::core::StreamType::DATA, id, nat::core::toString(nat::core::SerializationType::Binary), nat::core::NatImuBulkDataSchema::name),
      statusStream(std::string(name.c_str()), nat::core::StreamType::LOGGING_HEARTBEAT, id, nat::core::toString(nat::core::SerializationType::Json), std::string("DeviceFirmwareStatusV1")),
      commandStream(std::string(name.c_str()), nat::core::StreamType::EXECUTION_COMMAND, id, nat::core::toString(nat::core::SerializationType::Json), std::string("NatExecutionCommandV1")),
      logStream(std::string(name.c_str()), nat::core::StreamType::LOGGING_LOG, id, nat::core::toString(nat::core::SerializationType::Json), std::string("NatLogV1")),
      bulkImuDataReadyToSend(false) {
        dataTopicString = dataStream.toTopicString().c_str();
        bulkDataTopicString = bulkDataStream.toTopicString().c_str();
        metaTopicString = metaStream.toTopicString().c_str();
        statusTopicString = statusStream.toTopicString().c_str();
        commandTopicString = commandStream.toTopicString().c_str();
        logTopicString = logStream.toTopicString().c_str();

        #ifdef USE_FREE_RTOS_LOCKS
        bulkImuDataLock = xSemaphoreCreateMutex();
        assert(bulkImuDataLock != nullptr);
        #endif // USE_FREE_RTOS_LOCKS
      }

    ~KafkaTopic() {
        #ifdef USE_FREE_RTOS_LOCKS
        if (bulkImuDataLock) {
            vSemaphoreDelete(bulkImuDataLock);
        }
        #endif // USE_FREE_RTOS_LOCKS
    }

    //void createKafkaStream(const ConnectionConfig& connectionConfig);

    // void writeMetaRecord(const ConnectionConfig& connectionConfig, MQTTClient& mqttClient);
    void writeMetaRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);
    // void writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data, MQTTClient& mqttClient);
    bool writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data, PubSubClient& mqttClient);

    void writeBulkDataRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);

    // Keeps the MQTT link healthy independent of the data rate: reconnects if the
    // broker dropped us and pumps loop() so keepalive PINGs are sent and a dead
    // link is detected promptly. MUST be called only from the MQTT-owning task
    // (sendMessageTask), on a fixed cadence — not just when data is published.
    void serviceConnection(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);

    // Publishes a ~1 Hz device.firmware.status.v1 heartbeat on the Heartbeat
    // topic. Piggybacked on the data publisher, so it only fires while data is
    // flowing (a total capture stall stops the heartbeat too).
    void writeStatusRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);

    // Subscribes to this device's command topic. The bridge republishes Kafka
    // records under natKit/receiving/<topic>, which is the direction opposite to
    // everything else here. Idempotent, and re-run by serviceConnection() after a
    // reconnect (MQTT subscriptions are per-session).
    bool subscribeToCommands(PubSubClient& mqttClient);

    // Publishes one NatLogV1 record on this device's log topic. MQTT-owning task
    // only, like every other publisher here.
    void writeLogRecord(const natkit_command::CommandLog& record, PubSubClient& mqttClient);

    const String& getName() const { return name; }
    const String& getCommandTopic() const { return commandTopicString; }
    const String& getLogTopic() const { return logTopicString; }

    // boardId is deliberately unused: callers build it as String{UNIQUE_ID} from a
    // uint64_t, and Arduino String has no uint64_t constructor, so the value gets
    // narrowed to a single byte -- the device name came out as "ESP32-<one garbage
    // char>". It only ever reached log/meta payloads (the topic identifier comes
    // from topicId, which was always correct), so nothing was keyed off it, but the
    // command channel matches a command's "target" against this name, so it has to
    // be the real id. Formatted from topicId here instead.
    static KafkaTopic* create(uint64_t topicId, const String& boardId) {
        (void)boardId;
        char nameBuffer[32];
        snprintf(nameBuffer, sizeof(nameBuffer), "ESP32-%llu",
                 (unsigned long long)topicId);
        return new KafkaTopic(topicId, String(nameBuffer));
    }

    // static String getKafkaCluster(const ConnectionConfig& config, uint8_t& size);
};


String KafkaTopic::mqttUrlTemplate = "natKit/sending/%s";
// Server -> device. The bridge publishes every Kafka record it forwards under
// this prefix; it subscribes to natKit/sending/# for the other direction, so the
// two never feed each other.
static const char* MQTT_RECEIVING_URL_TEMPLATE = "natKit/receiving/%s";

bool KafkaTopic::subscribeToCommands(PubSubClient& mqttClient) {
    if (!mqttClient.connected()) {
        return false;
    }
    char topicBuffer[256];
    snprintf(topicBuffer, sizeof(topicBuffer), MQTT_RECEIVING_URL_TEMPLATE,
             commandTopicString.c_str());
    if (mqttClient.subscribe(topicBuffer)) {
        subscribedToCommands = true;
        DEBUG_SERIAL.printf("Subscribed to command topic %s\n", topicBuffer);
        return true;
    }
    subscribedToCommands = false;
    DEBUG_SERIAL.printf("Failed to subscribe to command topic %s\n", topicBuffer);
    return false;
}

void KafkaTopic::writeLogRecord(const natkit_command::CommandLog& record,
                                PubSubClient& mqttClient) {
    if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
        return;
    }

    // Escape the message: it is the one field built from a handler's printf, so
    // it can contain a quote or a backslash and must not break the record.
    char escaped[natkit_command::LOG_MESSAGE_MAX * 2];
    size_t written = 0;
    for (const char* c = record.message; *c != '\0' && written + 2 < sizeof(escaped); ++c) {
        if (*c == '"' || *c == '\\') {
            escaped[written++] = '\\';
            escaped[written++] = *c;
        } else if (*c == '\n') {
            escaped[written++] = '\\';
            escaped[written++] = 'n';
        } else if ((unsigned char)*c < 0x20) {
            escaped[written++] = ' ';
        } else {
            escaped[written++] = *c;
        }
    }
    escaped[written] = '\0';

    char logBuffer[768];
    const int size = snprintf(
        logBuffer, sizeof(logBuffer),
        "{\"schema_version\":\"nat.log.v1\",\"source\":\"%s\","
        "\"command_id\":\"%s\",\"command\":\"%s\",\"level\":\"%s\","
        "\"ok\":%s,\"terminal\":%s,\"message\":\"%s\",\"emitted_at_us\":%lld}",
        name.c_str(), record.command_id, record.command, record.level,
        record.ok ? "true" : "false", record.terminal ? "true" : "false",
        escaped, (long long)getAdjustedLocalTimeUs());
    if (size <= 0 || size >= (int)sizeof(logBuffer)) {
        DEBUG_SERIAL.println("Error: log record did not fit in buffer");
        return;
    }

    sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), logTopicString.c_str());
    if (!mqttClient.publish(kafkaUrlBuffer, (const uint8_t*)logBuffer, size)) {
        DEBUG_SERIAL.printf("Error: failed to publish log record to %s\n", kafkaUrlBuffer);
    }
}

void KafkaTopic::writeMetaRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient) {
    
    static const auto delay = 10 / portTICK_PERIOD_MS; // 10ms
    if (WiFi.status() == WL_CONNECTED) {

        while (!mqttClient.connect("natKit-IMU")) vTaskDelay(delay);
        sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), metaTopicString.c_str());
        const auto bytes = meta.encodeToBytes(nat::core::SerializationType::Json);
        for (int i = 0; i < bytes->size(); ++i)
            kafkaRecordDataBuffer[i] = (*bytes)[i];
        kafkaRecordDataBuffer[bytes->size()] = 0;
        // memcpy(kafkaRecordDataBuffer, bytes.get(), bytes->size());
        // esp_mqtt_client_publish(mqttClient, kafkaUrlBuffer, kafkaRecordDataBuffer, strlen(kafkaUrlBuffer), 0, false);
        mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer, bytes->size() + 1);

    }
}

void KafkaTopic::writeStatusRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient) {
    if (WiFi.status() != WL_CONNECTED || !mqttClient.connected())
        return;

    char statusBuffer[512];
    const int written = snprintf(
        statusBuffer, sizeof(statusBuffer),
        "{\"schema_version\":\"device.firmware.status.v1\","
        "\"device_id\":\"%s\",\"seq_no\":%llu,\"transport_mode\":\"wireless\","
        "\"frames_published\":%llu,\"sample_rate_hz\":%d,"
        "\"wifi_connected\":%s,\"mqtt_connected\":%s,"
        "\"rssi_dbm\":%d,\"uptime_ms\":%lu,\"emitted_at_us\":%lld}",
        name.c_str(),
        (unsigned long long)heartbeatSeqNo++,
        (unsigned long long)bulkSeqNo,
        IMU_SAMPLE_RATE_HZ,
        (WiFi.status() == WL_CONNECTED) ? "true" : "false",
        mqttClient.connected() ? "true" : "false",
        (int)WiFi.RSSI(),
        (unsigned long)millis(),
        (long long)getAdjustedLocalTimeUs());

    if (written <= 0 || written >= (int)sizeof(statusBuffer)) {
        DEBUG_SERIAL.println("Error: status record did not fit in buffer");
        return;
    }

    sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), statusTopicString.c_str());
    mqttClient.publish(kafkaUrlBuffer, (const uint8_t*)statusBuffer, written);
}

bool KafkaTopic::writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& imuDatum, PubSubClient& mqttClient) {

        nat::core::NatImuDataSchema data{imuDatum.timestamp, imuDatum.accuracies, imuDatum.has_data, imuDatum.data, 10};
        imuDataList[currentImuDataIndex++] = data;
        if (currentImuDataIndex == IMU_SAMPLES_PER_FRAME) {
            DEBUG_SERIAL.println("Bulk Message Is Ready to Send");
            currentImuDataIndex = 0;
            // DEBUG_SERIAL.println("AA");
            {
                // DEBUG_SERIAL.println("BB");
                #ifdef USE_FREE_RTOS_LOCKS
                xSemaphoreTake(bulkImuDataLock, portMAX_DELAY);
                #else
                std::lock_guard<std::mutex> guard(bulkImuDataLock);
                #endif // USE_FREE_RTOS_LOCKS
                // DEBUG_SERIAL.println("CC");
                bulkImuData.setData(imuDataList, IMU_SAMPLES_PER_FRAME);
                // Frame envelope: deviceTsUs is the first sample's timestamp in
                // microseconds (per-sample time is carried in milliseconds).
                const uint64_t deviceTsUs = static_cast<uint64_t>(imuDataList[0].getTime()) * 1000ULL;
                bulkImuData.setFrameHeader(bulkSeqNo++, deviceTsUs, IMU_SAMPLE_RATE_HZ);
                // DEBUG_SERIAL.println("DD");
                bulkImuDataReadyToSend = true;
                #ifdef USE_FREE_RTOS_LOCKS
                xSemaphoreGive(bulkImuDataLock);
                #endif // USE_FREE_RTOS_LOCKS
                // DEBUG_SERIAL.println("EE");
                //digitalWrite(12, LOW);
                // DEBUG_SERIAL.println("FF");
                return true;
            }
            // DEBUG_SERIAL.println("GG");
        } else {
            //digitalWrite(12, LOW);
            return false;
        }

}

void KafkaTopic::serviceConnection(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient) {
    if (WiFi.status() != WL_CONNECTED) {
        return; // WiFi layer handles its own reconnect; nothing to do here.
    }
    if (!mqttClient.connected()) {
        // Broker dropped us (or first connect). Attempt one non-blocking
        // reconnect per tick; if it fails we retry on the next service call.
        if (mqttClient.connect("natKit-IMU")) {
            DEBUG_SERIAL.println("MQTT (re)connected");
            // The old session's subscriptions died with it.
            subscribedToCommands = false;
        } else {
            DEBUG_SERIAL.println("MQTT reconnect pending...");
            return;
        }
    }
    if (!subscribedToCommands) {
        subscribeToCommands(mqttClient);
    }
    // Pump the client so keepalive PINGREQs go out and a dead link is detected
    // even when the sensor is producing no data. Inbound command messages are
    // also delivered from here, via the subscribe callback.
    mqttClient.loop();
}

void KafkaTopic::writeBulkDataRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient) {
    {
        // DEBUG_SERIAL.println("Sending Bulk Message");
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(bulkImuDataLock, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(bulkImuDataLock);
        #endif // USE_FREE_RTOS_LOCKS
        if (!bulkImuDataReadyToSend) {
            DEBUG_SERIAL.println("Exit Early");
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(bulkImuDataLock);
            #endif // USE_FREE_RTOS_LOCKS
            return;
        }
        //static const auto delay_len = 10 / portTICK_PERIOD_MS; // 10ms
        if (WiFi.status() == WL_CONNECTED) {

            if (!mqttClient.connected()) {
                #ifdef USE_FREE_RTOS_LOCKS
                xSemaphoreGive(bulkImuDataLock);
                #endif // USE_FREE_RTOS_LOCKS
                return;
            }

            const auto bytes = bulkImuData.encodeToBytes(nat::core::SerializationType::Binary);
            // const auto bytes = bulkImuData.encodeToBytes(nat::core::SerializationType::Csv);
            // DEBUG_SERIAL.println("Bytes Encoded");
            if (bytes == nullptr) {
                DEBUG_SERIAL.println("Error: Failed to encode CSV data!");
            }

            DEBUG_SERIAL.println("Bytes Encoded");
            DEBUG_SERIAL.printf("%d\n", bytes->size());
            for (int i = 0; i < bytes->size(); ++i)
                kafkaRecordDataBuffer[i] = (*bytes)[i];
            // DEBUG_SERIAL.println("Bytes moved into buffer");
            //kafkaRecordDataBuffer[bytes->size()] = 0;
            // DEBUG_SERIAL.printf("%d\n", bytes->size());
            // DEBUG_SERIAL.printf("%s\n", kafkaRecordDataBuffer);
            sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), bulkDataTopicString.c_str());

            if (mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer, bytes->size())) {
                DEBUG_SERIAL.printf("-------------------------------- Sent message to %s --------------------------------\n", kafkaUrlBuffer);
                bulkImuDataReadyToSend = false;

                // Emit a heartbeat at most once per interval, on the same task
                // that just published data (so PubSubClient stays single-writer).
                const uint32_t nowMs = millis();
                if (nowMs - lastHeartbeatMs >= IMU_HEARTBEAT_INTERVAL_MS) {
                    lastHeartbeatMs = nowMs;
                    writeStatusRecord(connectionConfig, mqttClient);
                }
            } else {
                DEBUG_SERIAL.printf("-------------------------------- ERROR --------------------------------\n");
                //digitalWrite(13, HIGH);
            }
            // DEBUG_SERIAL.printf("HELLO size: %u\n%s\n", strlen(payloadBuffer), payloadBuffer);
            // const int httpResponseCode = httpClient.POST(payloadBuffer);
            // if (httpResponseCode != 200) {
            //     DEBUG_SERIAL.printf("Error: Failed to POST a new meta record: %d\n", httpResponseCode);
            // }
            mqttClient.loop();
            //digitalWrite(27, LOW);
            //digitalWrite(12, LOW);
        }

        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreGive(bulkImuDataLock);
        #endif // USE_FREE_RTOS_LOCKS
    }
}

