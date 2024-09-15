#pragma once

#include <Arduino.h>
#include <ConnectionConfig.hpp>
#include <macros.hpp>
//#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ImuData.hpp>
#include <MQTT.h>
#include <libnatkit-core.hpp>
#include <PubSubClient.h>
// #include "mqtt_client.h"


char kafkaUrlBuffer[256];
char kafkaRecordDataBuffer[512];

class KafkaTopic {
    String name;
    String dataTopicString;
    String metaTopicString;
    // String clusterId;
    nat::core::Stream dataStream;
    nat::core::Stream metaStream;
    nat::core::BasicMetaInfoSchema meta;
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
      metaStream(std::string(name.c_str()), nat::core::StreamType::META, id, nat::core::toString(nat::core::SerializationType::Json), nat::core::BasicMetaInfoSchema::name) {
        dataTopicString = dataStream.toTopicString().c_str();
        metaTopicString = metaStream.toTopicString().c_str();
      }

    //void createKafkaStream(const ConnectionConfig& connectionConfig);

    // void writeMetaRecord(const ConnectionConfig& connectionConfig, MQTTClient& mqttClient);
    void writeMetaRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);
    // void writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data, MQTTClient& mqttClient);
    void writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data, PubSubClient& mqttClient);

    static KafkaTopic* create(uint64_t topicId, const String& boardId) {
        const String name = "ESP32-" + boardId;
        return new KafkaTopic(topicId, name);
    }

    // static String getKafkaCluster(const ConnectionConfig& config, uint8_t& size);
};

// String KafkaTopic::createTopicUrlTemplate = "http://%s:%s/v3/clusters/%s/topics";
// String KafkaTopic::createTopicPostStringTemplate = R"=====(
//     {
//         "topic_name": "%s",
//         "partitions_count": 1,
//         "replication_factor": 1,
//         "configs": [
//             {
//                 "name": "cleanup.policy",
//                 "value": "compact"
//             },
//             {
//                 "name": "compression.type",
//                 "value": "gzip"
//             }
//         ]
//     }
//     )=====";

// String KafkaTopic::writeRecordUrlTemplate = "http://%s:%s/v3/clusters/%s/topics/%s/records";
// String KafkaTopic::writeRecordPostTemplate = R"=====(
//     {
//         "key": {
//             "type": "BINARY",
//             "data": "Zm9vYmFy"
//         },
//         "value": {
//             "type": "JSON",
//             "data": %s
//         }
//     }
//     )=====";
// String KafkaTopic::mqttUrlTemplate = "natKit/reciving/%s";
String KafkaTopic::mqttUrlTemplate = "natKit/sending/%s";
// String KafkaTopic::dataRecordDataTemplate = R"==({"timestamp": %llu, "data": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f], "calibration": %d})==";
// String KafkaTopic::metaRecordDataTemplate = R"==({"Stream Name": "%s"})==";

// void KafkaTopic::createKafkaStream(const ConnectionConfig& connectionConfig) {
//     if (WiFi.status() == WL_CONNECTED) {
//         const uint32_t urlSize = createTopicUrlTemplate.length() +
//             strlen(connectionConfig.natKitServerAddress) + strlen(connectionConfig.natKitServerPort) +
//             clusterId.length() + 1;

//         if (urlSize > 256) {
//             DEBUG_SERIAL.println("Error: Cannot create kafka stream because url is larger than 127 characters!");
//             return;
//         }
//         char urlBuffer[256];
//         char payloadBuffer[1024];
//         sprintf(urlBuffer, createTopicUrlTemplate.c_str(), connectionConfig.natKitServerAddress,
//             connectionConfig.natKitServerPort, clusterId.c_str());

//         DEBUG_SERIAL.printf("URL for topic creater is: %s\n", urlBuffer);
//         {
//             HTTPClient httpClient;
//             httpClient.addHeader("Content-Type", "application/json");
//             httpClient.addHeader("Accept", "application/json");
//             httpClient.begin(urlBuffer);

//             if (dataTopicString.length() + createTopicPostStringTemplate.length() + 1 > 1024) {
//                 DEBUG_SERIAL.println("Error: Cannot create kafka stream because POST message is larger than 1023 characters!");
//                 return;
//             }
//             sprintf(payloadBuffer, createTopicPostStringTemplate.c_str(), dataTopicString.c_str());
//             DEBUG_SERIAL.printf("Payload for data topic is: %s\n", payloadBuffer);
//             const int httpResponseCode = httpClient.POST(payloadBuffer);
//             if (httpResponseCode != 201) {
//                 DEBUG_SERIAL.printf("Error: Failed to POST a new Kafka Data Topic: %d\n", httpResponseCode);
//             } else {
//                 DEBUG_SERIAL.printf("Succesful POST for Data topic: %d\n", httpResponseCode);
//             }
//         }
//         {
//             HTTPClient httpClient;
//             httpClient.addHeader("Content-Type", "application/json");
//             httpClient.addHeader("Accept", "application/json");
//             httpClient.begin(urlBuffer);

//             if (metaTopicString.length() + createTopicPostStringTemplate.length() + 1 > 512) {
//                 DEBUG_SERIAL.println("Error: Cannot create kafka stream because POST message is larger than 511 characters!");
//                 return;
//             }
//             sprintf(payloadBuffer, createTopicPostStringTemplate.c_str(), metaTopicString.c_str());
//             const int httpResponseCode = httpClient.POST(payloadBuffer);
//             DEBUG_SERIAL.printf("Payload for Meta topic is: %s\n", payloadBuffer);
//             if (httpResponseCode != 201) {
//                 DEBUG_SERIAL.printf("Error: Failed to POST a new Kafka Meta Topic: %d\n", httpResponseCode);
//             } else {
//                 DEBUG_SERIAL.printf("Succesful POST for Meta topic: %d\n", httpResponseCode);
//             }
//         }
//     }
// }

void KafkaTopic::writeMetaRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient) {
    static const auto delay = 10 / portTICK_PERIOD_MS; // 10ms
    if (WiFi.status() == WL_CONNECTED) {
        // const uint32_t urlSize = writeRecordUrlTemplate.length() +
        //     strlen(connectionConfig.natKitServerAddress) + strlen(connectionConfig.natKitServerPort) +
        //     clusterId.length() + metaTopicString.length() + 1;

        // if (urlSize > 256) {
        //     DEBUG_SERIAL.println("Error: Cannot write a meta record because url is larger than 255 characters!");
        //     return;
        // }
        //char payloadBuffer[1024];
        // sprintf(kafkaUrlBuffer, writeRecordUrlTemplate.c_str(), connectionConfig.natKitServerAddress,
        //     connectionConfig.natKitServerPort, clusterId.c_str(), metaTopicString.c_str());

        // HTTPClient httpClient;
        // httpClient.addHeader("Content-Type", "application/json");
        // httpClient.addHeader("Accept", "application/json");
        // httpClient.begin(urlBuffer);

        // if (name.length() + writeRecordPostTemplate.length() + 1 > 1024) {
        //     DEBUG_SERIAL.println("Error: Cannot write a meta record because POST message is larger than 1023 characters!");
        //     return;
        // }
        // sprintf(kafkaRecordDataBuffer, metaRecordDataTemplate.c_str(), name.c_str());
        //sprintf(payloadBuffer, writeRecordPostTemplate.c_str(), recordDataBuffer);
        //DEBUG_SERIAL.printf("Payload for meta record is: %s\n", payloadBuffer);
        //const int httpResponseCode = httpClient.POST(payloadBuffer);
        // if (httpResponseCode != 200) {
        //     DEBUG_SERIAL.printf("Error: Failed to POST a new meta record: %d\n", httpResponseCode);
        // }
        //String recordDataString{recordDataBuffer};

        while (!mqttClient.connect("natKit-IMU")) vTaskDelay(delay);
        sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), metaTopicString.c_str());
        const auto bytes = meta.encodeToBytes(nat::core::SerializationType::Json);
        for (int i = 0; i < bytes->size(); ++i)
            kafkaRecordDataBuffer[i] = (*bytes)[i];
        kafkaRecordDataBuffer[bytes->size()] = 0;
        // memcpy(kafkaRecordDataBuffer, bytes.get(), bytes->size());
        // esp_mqtt_client_publish(mqttClient, kafkaUrlBuffer, kafkaRecordDataBuffer, strlen(kafkaUrlBuffer), 0, false);
        mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer);
    }
}

void KafkaTopic::writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& imuData, PubSubClient& mqttClient) {
    static const auto delay = 10 / portTICK_PERIOD_MS; // 10ms
    if (WiFi.status() == WL_CONNECTED) {
        // const uint32_t urlSize = writeRecordUrlTemplate.length() +
        //     strlen(connectionConfig.natKitServerAddress) + strlen(connectionConfig.natKitServerPort) +
        //     clusterId.length() + 1;

        // if (urlSize > 256) {
        //     DEBUG_SERIAL.println("Error: Cannot write a meta record because url is larger than 127 characters!");
        //     return;
        // }
        
        //char payloadBuffer[1024];
        // sprintf(kafkaUrlBuffer, writeRecordUrlTemplate.c_str(), connectionConfig.natKitServerAddress,
        //     connectionConfig.natKitServerPort, clusterId.c_str(), dataTopicString.c_str());

        // HTTPClient httpClient;
        // httpClient.addHeader("Content-Type", "application/json");
        // httpClient.addHeader("Accept", "application/json");
        // httpClient.begin(urlBuffer);

        // if (writeRecordPostTemplate.length() + 1 > 1024) {
        //     DEBUG_SERIAL.println("Error: Cannot write a meta record because POST message is larger than 1023 characters!");
        //     return;
        // }
        // void *memory = malloc(1000);
        // if (memory == NULL)
        //     log_i("Out of Memory!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        // else
        //     log_i("Memory Sucessfully Allocated\n");
        // free(memory);

        while (!mqttClient.connect("natKit-IMU")) {
            if (WiFi.status() != WL_CONNECTED) {
                WiFi.begin(connectionConfig.networkSsid, connectionConfig.networkPassword);
                while (WiFi.status() != WL_CONNECTED) {
                    vTaskDelay(delay*100);
                    DEBUG_SERIAL.println("Connecting to WiFi..");
                }
            }
            vTaskDelay(delay);
        }
        DEBUG_SERIAL.println("HIHIHIHIHIHI");
        nat::core::NatImuDataSchema data{imuData.timestamp, nat::core::NatImuDataSchema::convertIntToSensorAccuracy(imuData.accuracy), imuData.data, 13};
        const auto bytes = data.encodeToBytes(nat::core::SerializationType::Json);
        if (bytes == nullptr) {
            DEBUG_SERIAL.println("Error: Failed to encode json data!");
        }
        for (int i = 0; i < bytes->size(); ++i)
            kafkaRecordDataBuffer[i] = (*bytes)[i];
        kafkaRecordDataBuffer[bytes->size()] = 0;
        DEBUG_SERIAL.printf("WORLD size: %u\n%s\n", strlen(kafkaRecordDataBuffer), kafkaRecordDataBuffer);
        DEBUG_SERIAL.printf("There: %s\n", kafkaRecordDataBuffer);
        sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), dataTopicString.c_str());
        // esp_mqtt_client_publish(mqttClient, kafkaUrlBuffer, kafkaRecordDataBuffer, strlen(kafkaUrlBuffer), 0, false);
        if (mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer))
            DEBUG_SERIAL.printf("-------------------------------- Sent message to %s --------------------------------\n", kafkaUrlBuffer);
        else
            DEBUG_SERIAL.printf("-------------------------------- ERROR --------------------------------\n");

        // DEBUG_SERIAL.printf("HELLO size: %u\n%s\n", strlen(payloadBuffer), payloadBuffer);
        // const int httpResponseCode = httpClient.POST(payloadBuffer);
        // if (httpResponseCode != 200) {
        //     DEBUG_SERIAL.printf("Error: Failed to POST a new meta record: %d\n", httpResponseCode);
        // }
    }
}

// String KafkaTopic::getKafkaCluster(const ConnectionConfig& config, uint8_t& size) {
//     String clusterId = "";
//     size = 0;

//     if (WiFi.status() == WL_CONNECTED) {
//         HTTPClient httpClient;
//         httpClient.addHeader("Content-Type", "application/json");
//         httpClient.addHeader("Accept", "application/json");
//         String url = "http://" + String{config.natKitServerAddress} + ":" + String{config.natKitServerPort} + "/v3/clusters/";
//         httpClient.begin(url.c_str());
//         int httpResponseCode = httpClient.GET();
        
//         if (httpResponseCode == 200) {
//             DEBUG_SERIAL.print("HTTP Response code: ");
//             DEBUG_SERIAL.println(httpResponseCode);
//             String payload = httpClient.getString();
//             DEBUG_SERIAL.println(payload);
//             DynamicJsonDocument jsonDoc(payload.length());
//             deserializeJson(jsonDoc, payload);
//             if (jsonDoc == nullptr) {
//                 DEBUG_SERIAL.println("jsonDoc is NULL");
//             } else  {
//                 DEBUG_SERIAL.println("jsonDoc is not NULL");
//             }
//             const auto jsonObject = jsonDoc.as<JsonObject>();
//             if (jsonObject == nullptr) {
//                 DEBUG_SERIAL.println("JsonObject is NULL");
//             } else  {
//                 DEBUG_SERIAL.println("JsonObject is not NULL");
//             }
//             const auto jsonArray = jsonObject["data"].as<JsonArray>();
//             if (jsonArray == nullptr) {
//                 DEBUG_SERIAL.println("jsonArray is NULL");
//             } else  {
//                 DEBUG_SERIAL.println("jsonArray is not NULL");
//             }
//             size = jsonArray.size();
//             //clusterIds = (String*)malloc(size*sizeof(String*));
//             int index = 0;
//             for (const auto& jsonVariant : jsonArray) {
//                 const String cluserIdValue = jsonVariant.as<JsonObject>()["cluster_id"].as<String>();
//                 DEBUG_SERIAL.println(cluserIdValue);
//                 if (index == 0) {
//                     clusterId = cluserIdValue;
//                     DEBUG_SERIAL.print("Set clustrer id to ");
//                     for (int i = 0; i < clusterId.length(); ++i) {
//                         DEBUG_SERIAL.print(clusterId[i]);
//                     }
//                     DEBUG_SERIAL.println();
//                 }
//                 ++index;
//             }
//         } else {
//             DEBUG_SERIAL.print("Error code: ");
//             DEBUG_SERIAL.println(httpResponseCode);
//         }
//         // Free resources
//         httpClient.end();
//     }

//     return clusterId;
// }