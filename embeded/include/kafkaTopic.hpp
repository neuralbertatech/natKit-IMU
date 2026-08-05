#pragma once

#include <Arduino.h>
#include <ConnectionConfig.hpp>
#include <macros.hpp>
//#include <ArduinoJson.h>
#include <HTTPClient.h>
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
    // String clusterId;
    nat::core::Stream dataStream;
    nat::core::Stream metaStream;
    nat::core::Stream bulkDataStream;
    nat::core::Stream statusStream;

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
      bulkImuDataReadyToSend(false) {
        dataTopicString = dataStream.toTopicString().c_str();
        bulkDataTopicString = bulkDataStream.toTopicString().c_str();
        metaTopicString = metaStream.toTopicString().c_str();
        statusTopicString = statusStream.toTopicString().c_str();

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

    static KafkaTopic* create(uint64_t topicId, const String& boardId) {
        const String name = "ESP32-" + boardId;
        return new KafkaTopic(topicId, name);
    }

    // static String getKafkaCluster(const ConnectionConfig& config, uint8_t& size);
};


String KafkaTopic::mqttUrlTemplate = "natKit/sending/%s";

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
        } else {
            DEBUG_SERIAL.println("MQTT reconnect pending...");
            return;
        }
    }
    // Pump the client so keepalive PINGREQs go out and a dead link is detected
    // even when the sensor is producing no data.
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

