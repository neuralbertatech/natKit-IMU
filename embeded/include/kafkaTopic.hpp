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
#include <mutex>
// #include "mqtt_client.h"


char kafkaUrlBuffer[256];
byte kafkaRecordDataBuffer[6200];
//char mqttBuffer[16384];
uint32_t indexes[10];
uint32_t currentIndex = 0;
nat::core::NatImuDataSchema imuDataList[100];
uint32_t currentImuDataIndex = 0;

class KafkaTopic {
    String name;
    nat::core::BasicMetaInfoSchema meta;
    String dataTopicString;
    String bulkDataTopicString;
    String metaTopicString;
    // String clusterId;
    nat::core::Stream dataStream;
    nat::core::Stream metaStream;
    nat::core::Stream bulkDataStream;

    nat::core::NatImuBulkDataSchema bulkImuData{};
    bool bulkImuDataReadyToSend;
    std::mutex bulkImuDataLock;
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
      bulkDataStream(std::string(name.c_str()), nat::core::StreamType::DATA, id, nat::core::toString(nat::core::SerializationType::Binary), nat::core::NatImuBulkDataSchema::name),
      metaStream(std::string(name.c_str()), nat::core::StreamType::META, id, nat::core::toString(nat::core::SerializationType::Json), nat::core::BasicMetaInfoSchema::name),
      bulkImuDataReadyToSend(false) {
        dataTopicString = dataStream.toTopicString().c_str();
        bulkDataTopicString = bulkDataStream.toTopicString().c_str();
        metaTopicString = metaStream.toTopicString().c_str();
      }

    //void createKafkaStream(const ConnectionConfig& connectionConfig);

    // void writeMetaRecord(const ConnectionConfig& connectionConfig, MQTTClient& mqttClient);
    void writeMetaRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);
    // void writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data, MQTTClient& mqttClient);
    bool writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data, PubSubClient& mqttClient);

    void writeBulkDataRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient);

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
        mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer, bytes->size() + 1);
        
    }
}

bool KafkaTopic::writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& imuDatum, PubSubClient& mqttClient) {
    //static const auto delay_len = 10 / portTICK_PERIOD_MS; // 10ms
    //if (WiFi.status() == WL_CONNECTED) {
        //digitalWrite(12, HIGH);
        //digitalWrite(13, HIGH);
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

        // while (!mqttClient.connect("natKit-IMU")) {
        //     if (WiFi.status() != WL_CONNECTED) {
        //         WiFi.begin(connectionConfig.networkSsid, connectionConfig.networkPassword);
        //         while (WiFi.status() != WL_CONNECTED) {
        //             //vTaskDelay(delay_len*100);
        //             DEBUG_SERIAL.println("Connecting to WiFi..");
        //         }
        //     }
        //     //vTaskDelay(delay_len);
        // }

        nat::core::NatImuDataSchema data{imuDatum.timestamp, nat::core::NatImuDataSchema::convertIntToSensorAccuracy(imuDatum.accuracy), imuDatum.data, 13};
        imuDataList[currentImuDataIndex++] = data;
        if (currentImuDataIndex == 100) {
            DEBUG_SERIAL.println("Bulk Message Is Ready to Send");
            currentImuDataIndex = 0;
            DEBUG_SERIAL.println("AA");
            {
                DEBUG_SERIAL.println("BB");
                std::lock_guard<std::mutex> gaurd(bulkImuDataLock);
                DEBUG_SERIAL.println("CC");
                bulkImuData.setData(imuDataList, 100);
                DEBUG_SERIAL.println("DD");
                bulkImuDataReadyToSend = true;
                DEBUG_SERIAL.println("EE");
                //digitalWrite(12, LOW);
                DEBUG_SERIAL.println("FF");
                return true;
            }
            DEBUG_SERIAL.println("GG");
        } else {
            //digitalWrite(12, LOW);
            return false;
        }
        // const auto bytes = data.encodeToBytes(nat::core::SerializationType::Json);
        // if (bytes == nullptr) {
        //     DEBUG_SERIAL.println("Error: Failed to encode json data!");
        // }

        // int offset = indexes[currentIndex];
        // for (int i = 0; i < bytes->size(); ++i)
        //     mqttBuffer[offset+i] = (*bytes)[i];
        // kafkaRecordDataBuffer[offset+bytes->size()] = 0;
        // if (currentIndex == 9) {
        //     sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), dataTopicString.c_str());
        //     //mqttClient.beginPublish();
        // } else {
        //     indexes[currentIndex] = offset+bytes->size() + 1;
        // }
        // // for (int i = 0; i < bytes->size(); ++i)
        // //     kafkaRecordDataBuffer[i] = (*bytes)[i];
        // // kafkaRecordDataBuffer[bytes->size()] = 0;
        // // DEBUG_SERIAL.printf("WORLD size: %u\n%s\n", strlen(kafkaRecordDataBuffer), kafkaRecordDataBuffer);
        // // DEBUG_SERIAL.printf("There: %s\n", kafkaRecordDataBuffer);
        // sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), dataTopicString.c_str());
        // // esp_mqtt_client_publish(mqttClient, kafkaUrlBuffer, kafkaRecordDataBuffer, strlen(kafkaUrlBuffer), 0, false);
        // if (mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer)) {
        //     DEBUG_SERIAL.printf("-------------------------------- Sent message to %s --------------------------------\n", kafkaUrlBuffer);
        // } else {
        //     DEBUG_SERIAL.printf("-------------------------------- ERROR --------------------------------\n");
        //     digitalWrite(13, HIGH);
        // }
        // // DEBUG_SERIAL.printf("HELLO size: %u\n%s\n", strlen(payloadBuffer), payloadBuffer);
        // // const int httpResponseCode = httpClient.POST(payloadBuffer);
        // // if (httpResponseCode != 200) {
        // //     DEBUG_SERIAL.printf("Error: Failed to POST a new meta record: %d\n", httpResponseCode);
        // // }
        // mqttClient.loop();
        // digitalWrite(12, LOW);
    //}
}

void KafkaTopic::writeBulkDataRecord(const ConnectionConfig& connectionConfig, PubSubClient& mqttClient) {
    {
        DEBUG_SERIAL.println("Sending Bulk Message");
        std::lock_guard<std::mutex> gaurd(bulkImuDataLock);
        if (!bulkImuDataReadyToSend) {
            DEBUG_SERIAL.println("Exit Early");
            return;
        }
        //static const auto delay_len = 10 / portTICK_PERIOD_MS; // 10ms
        if (WiFi.status() == WL_CONNECTED) {
            digitalWrite(27, HIGH);
            //digitalWrite(12, HIGH);
            //digitalWrite(13, LOW);
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
                        //vTaskDelay(delay_len*100);
                        DEBUG_SERIAL.println("Connecting to WiFi..");
                    }
                }
                //vTaskDelay(delay_len);
            }
            DEBUG_SERIAL.println("MQTT Client is connected");

            const auto bytes = bulkImuData.encodeToBytes(nat::core::SerializationType::Binary);
            DEBUG_SERIAL.println("Bytes Encoded");
            if (bytes == nullptr) {
                DEBUG_SERIAL.println("Error: Failed to encode CSV data!");
            }

            DEBUG_SERIAL.println("Bytes Encoded");
            DEBUG_SERIAL.printf("%d\n", bytes->size());
            for (int i = 0; i < bytes->size(); ++i)
                kafkaRecordDataBuffer[i] = (*bytes)[i];
            DEBUG_SERIAL.println("Bytes moved into buffer");
            //kafkaRecordDataBuffer[bytes->size()] = 0;
            DEBUG_SERIAL.printf("%d\n", bytes->size());
            // DEBUG_SERIAL.printf("%s\n", kafkaRecordDataBuffer);
            sprintf(kafkaUrlBuffer, mqttUrlTemplate.c_str(), bulkDataTopicString.c_str());
            // DEBUG_SERIAL.printf("%s\n", kafkaUrlBuffer);
            // for (int i = 0; i < bytes->size(); ++i)
            //     kafkaRecordDataBuffer[i] = (*bytes)[i];
            // kafkaRecordDataBuffer[bytes->size()] = 0;
            // DEBUG_SERIAL.printf("WORLD size: %u\n%s\n", strlen(kafkaRecordDataBuffer), kafkaRecordDataBuffer);
            // DEBUG_SERIAL.printf("There: %s\n", kafkaRecordDataBuffer);
            // esp_mqtt_client_publish(mqttClient, kafkaUrlBuffer, kafkaRecordDataBuffer, strlen(kafkaUrlBuffer), 0, false);
            //if (mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer, bytes->size())) {
            if (mqttClient.publish(kafkaUrlBuffer, kafkaRecordDataBuffer, bytes->size())) {
                DEBUG_SERIAL.printf("-------------------------------- Sent message to %s --------------------------------\n", kafkaUrlBuffer);
                bulkImuDataReadyToSend = false;
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
            digitalWrite(27, LOW);
            //digitalWrite(12, LOW);
        }
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