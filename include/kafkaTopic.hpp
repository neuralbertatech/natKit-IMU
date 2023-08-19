#pragma once

#include <Arduino.h>
#include <ConnectionConfig.hpp>
#include <macros.hpp>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ImuData.hpp>


class KafkaTopic {
    String name;
    String dataTopicString;
    String metaTopicString;
    String clusterId;
    static String createTopicPostStringTemplate;
    static String createTopicUrlTemplate;
    static String writeRecordUrlTemplate;
    static String writeRecordPostTemplate;
    static String dataRecordDataTemplate;
    static String metaRecordDataTemplate;
public:
    KafkaTopic(const String& name, const String& dataTopic, const String& metaTopic, const String& clusterId)
      : name(name), dataTopicString(dataTopic), metaTopicString(metaTopic), clusterId(clusterId) {}

    void createKafkaStream(const ConnectionConfig& connectionConfig);

    void writeMetaRecord(const ConnectionConfig& connectionConfig);
    void writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data);

    static KafkaTopic* create(uint64_t topicId, const String& clusterId, const String& boardId) {
        const String idString{topicId};
        const String dataTopicString = "data-" + idString + "-JsonEncoder-ImuDataSchema";
        const String metaTopicString = "meta-" + idString + "-JsonEncoder-BasicMetaInfoSchema";
        const String name = "ESP32-" + boardId;
        return new KafkaTopic(name, dataTopicString, metaTopicString, clusterId);
    }

    static String getKafkaCluster(const ConnectionConfig& config, uint8_t& size);
};

String KafkaTopic::createTopicUrlTemplate = "http://%s:%s/v3/clusters/%s/topics";
String KafkaTopic::createTopicPostStringTemplate = R"=====(
    {
        "topic_name": "%s",
        "partitions_count": 1,
        "replication_factor": 1,
        "configs": [
            {
                "name": "cleanup.policy",
                "value": "compact"
            },
            {
                "name": "compression.type",
                "value": "gzip"
            }
        ]
    }
    )=====";

String KafkaTopic::writeRecordUrlTemplate = "http://%s:%s/v3/clusters/%s/topics/%s/records";
String KafkaTopic::writeRecordPostTemplate = R"=====(
    {
        "key": {
            "type": "BINARY",
            "data": "Zm9vYmFy"
        },
        "value": {
            "type": "JSON",
            "data": %s
        }
    }
    )=====";
String KafkaTopic::dataRecordDataTemplate = R"==({"timestamp": %llu, "data": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f], "calibration": %d})==";
String KafkaTopic::metaRecordDataTemplate = R"==({"Stream Name": "%s"})==";

void KafkaTopic::createKafkaStream(const ConnectionConfig& connectionConfig) {
    if (WiFi.status() == WL_CONNECTED) {
        const uint32_t urlSize = createTopicUrlTemplate.length() +
            strlen(connectionConfig.natKitServerAddress) + strlen(connectionConfig.natKitServerPort) +
            clusterId.length() + 1;

        if (urlSize > 256) {
            DEBUG_SERIAL.println("Error: Cannot create kafka stream because url is larger than 127 characters!");
            return;
        }
        char urlBuffer[256];
        char payloadBuffer[1024];
        sprintf(urlBuffer, createTopicUrlTemplate.c_str(), connectionConfig.natKitServerAddress,
            connectionConfig.natKitServerPort, clusterId.c_str());

        DEBUG_SERIAL.printf("URL for topic creater is: %s\n", urlBuffer);
        {
            HTTPClient httpClient;
            httpClient.addHeader("Content-Type", "application/json");
            httpClient.addHeader("Accept", "application/json");
            httpClient.begin(urlBuffer);

            if (dataTopicString.length() + createTopicPostStringTemplate.length() + 1 > 1024) {
                DEBUG_SERIAL.println("Error: Cannot create kafka stream because POST message is larger than 1023 characters!");
                return;
            }
            sprintf(payloadBuffer, createTopicPostStringTemplate.c_str(), dataTopicString.c_str());
            DEBUG_SERIAL.printf("Payload for data topic is: %s\n", payloadBuffer);
            const int httpResponseCode = httpClient.POST(payloadBuffer);
            if (httpResponseCode != 201) {
                DEBUG_SERIAL.printf("Error: Failed to POST a new Kafka Data Topic: %d\n", httpResponseCode);
            } else {
                DEBUG_SERIAL.printf("Succesful POST for Data topic: %d\n", httpResponseCode);
            }
        }
        {
            HTTPClient httpClient;
            httpClient.addHeader("Content-Type", "application/json");
            httpClient.addHeader("Accept", "application/json");
            httpClient.begin(urlBuffer);

            if (metaTopicString.length() + createTopicPostStringTemplate.length() + 1 > 512) {
                DEBUG_SERIAL.println("Error: Cannot create kafka stream because POST message is larger than 511 characters!");
                return;
            }
            sprintf(payloadBuffer, createTopicPostStringTemplate.c_str(), metaTopicString.c_str());
            const int httpResponseCode = httpClient.POST(payloadBuffer);
            DEBUG_SERIAL.printf("Payload for Meta topic is: %s\n", payloadBuffer);
            if (httpResponseCode != 201) {
                DEBUG_SERIAL.printf("Error: Failed to POST a new Kafka Meta Topic: %d\n", httpResponseCode);
            } else {
                DEBUG_SERIAL.printf("Succesful POST for Meta topic: %d\n", httpResponseCode);
            }
        }
    }
}

void KafkaTopic::writeMetaRecord(const ConnectionConfig& connectionConfig) {
    if (WiFi.status() == WL_CONNECTED) {
        const uint32_t urlSize = writeRecordUrlTemplate.length() +
            strlen(connectionConfig.natKitServerAddress) + strlen(connectionConfig.natKitServerPort) +
            clusterId.length() + metaTopicString.length() + 1;

        if (urlSize > 256) {
            DEBUG_SERIAL.println("Error: Cannot write a meta record because url is larger than 127 characters!");
            return;
        }
        char urlBuffer[256];
        char recordDataBuffer[64];
        char payloadBuffer[1024];
        sprintf(urlBuffer, writeRecordUrlTemplate.c_str(), connectionConfig.natKitServerAddress,
            connectionConfig.natKitServerPort, clusterId.c_str(), metaTopicString.c_str());

        HTTPClient httpClient;
        httpClient.addHeader("Content-Type", "application/json");
        httpClient.addHeader("Accept", "application/json");
        httpClient.begin(urlBuffer);

        if (name.length() + writeRecordPostTemplate.length() + 1 > 1024) {
            DEBUG_SERIAL.println("Error: Cannot write a meta record because POST message is larger than 1023 characters!");
            return;
        }
        sprintf(recordDataBuffer, metaRecordDataTemplate.c_str(), name.c_str());
        sprintf(payloadBuffer, writeRecordPostTemplate.c_str(), recordDataBuffer);
        DEBUG_SERIAL.printf("Payload for meta record is: %s\n", payloadBuffer);
        const int httpResponseCode = httpClient.POST(payloadBuffer);
        if (httpResponseCode != 200) {
            DEBUG_SERIAL.printf("Error: Failed to POST a new meta record: %d\n", httpResponseCode);
        }
    }
}

void KafkaTopic::writeDataRecord(const ConnectionConfig& connectionConfig, const ImuData& data) {
    if (WiFi.status() == WL_CONNECTED) {
        const uint32_t urlSize = writeRecordUrlTemplate.length() +
            strlen(connectionConfig.natKitServerAddress) + strlen(connectionConfig.natKitServerPort) +
            clusterId.length() + 1;

        if (urlSize > 256) {
            DEBUG_SERIAL.println("Error: Cannot write a meta record because url is larger than 127 characters!");
            return;
        }
        char urlBuffer[256];
        char recordDataBuffer[256];
        char payloadBuffer[1024];
        sprintf(urlBuffer, writeRecordUrlTemplate.c_str(), connectionConfig.natKitServerAddress,
            connectionConfig.natKitServerPort, clusterId.c_str(), dataTopicString.c_str());

        HTTPClient httpClient;
        httpClient.addHeader("Content-Type", "application/json");
        httpClient.addHeader("Accept", "application/json");
        httpClient.begin(urlBuffer);

        if (writeRecordPostTemplate.length() + 1 > 1024) {
            DEBUG_SERIAL.println("Error: Cannot write a meta record because POST message is larger than 1023 characters!");
            return;
        }
        DEBUG_SERIAL.println("HIHIHIHIHIHI");
        sprintf(recordDataBuffer, dataRecordDataTemplate.c_str(), data.timestamp, data.data[0], data.data[1], data.data[2], data.data[3], data.data[4], data.data[5], data.data[6], data.data[7], data.data[8], data.calibration);
        sprintf(payloadBuffer, writeRecordPostTemplate.c_str(), recordDataBuffer);
        DEBUG_SERIAL.printf("HELLO size: %u\n%s\n", strlen(payloadBuffer), payloadBuffer);
        DEBUG_SERIAL.printf("WORLD size: %u\n%s\n", strlen(recordDataBuffer), recordDataBuffer);
        const int httpResponseCode = httpClient.POST(payloadBuffer);
        if (httpResponseCode != 200) {
            DEBUG_SERIAL.printf("Error: Failed to POST a new meta record: %d\n", httpResponseCode);
        }
    }
}

String KafkaTopic::getKafkaCluster(const ConnectionConfig& config, uint8_t& size) {
    String clusterId = "";
    size = 0;

    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient httpClient;
        httpClient.addHeader("Content-Type", "application/json");
        httpClient.addHeader("Accept", "application/json");
        String url = "http://" + String{config.natKitServerAddress} + ":" + String{config.natKitServerPort} + "/v3/clusters/";
        httpClient.begin(url.c_str());
        int httpResponseCode = httpClient.GET();
        
        if (httpResponseCode == 200) {
            DEBUG_SERIAL.print("HTTP Response code: ");
            DEBUG_SERIAL.println(httpResponseCode);
            String payload = httpClient.getString();
            DEBUG_SERIAL.println(payload);
            DynamicJsonDocument jsonDoc(payload.length());
            deserializeJson(jsonDoc, payload);
            if (jsonDoc == nullptr) {
                DEBUG_SERIAL.println("jsonDoc is NULL");
            } else  {
                DEBUG_SERIAL.println("jsonDoc is not NULL");
            }
            const auto jsonObject = jsonDoc.as<JsonObject>();
            if (jsonObject == nullptr) {
                DEBUG_SERIAL.println("JsonObject is NULL");
            } else  {
                DEBUG_SERIAL.println("JsonObject is not NULL");
            }
            const auto jsonArray = jsonObject["data"].as<JsonArray>();
            if (jsonArray == nullptr) {
                DEBUG_SERIAL.println("jsonArray is NULL");
            } else  {
                DEBUG_SERIAL.println("jsonArray is not NULL");
            }
            size = jsonArray.size();
            //clusterIds = (String*)malloc(size*sizeof(String*));
            int index = 0;
            for (const auto& jsonVariant : jsonArray) {
                const String cluserIdValue = jsonVariant.as<JsonObject>()["cluster_id"].as<String>();
                DEBUG_SERIAL.println(cluserIdValue);
                if (index == 0) {
                    clusterId = cluserIdValue;
                    DEBUG_SERIAL.print("Set clustrer id to ");
                    for (int i = 0; i < clusterId.length(); ++i) {
                        DEBUG_SERIAL.print(clusterId[i]);
                    }
                    DEBUG_SERIAL.println();
                }
                ++index;
            }
        } else {
            DEBUG_SERIAL.print("Error code: ");
            DEBUG_SERIAL.println(httpResponseCode);
        }
        // Free resources
        httpClient.end();
    }

    return clusterId;
}