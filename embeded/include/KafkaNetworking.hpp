#pragma once

#include <MicroStateMachine.hpp>
#include <kafkaTopic.hpp>
#include <ConnectionConfig.hpp>


namespace natKit {

struct KafkaNetworkingData : public EventData {
    ConnectionConfig connectionConfig;
    KafkaTopic topic;
};

class KafkaNetworking : public MicroStateMachine {
public:
    KafkaNetworking() : MicroStateMachine(TOTAL_NUMBER_OF_STATES) {}

    void connect(KafkaNetworkingData*);

private:
    void handleNoConnection(KafkaNetworkingData*);
    void handleAttemptWifiConnection(KafkaNetworkingData*);
    void handleOnlyWifiConnected(KafkaNetworkingData*);
    void handleAttemptBrokerConnection(KafkaNetworkingData*);
    void handleConnectedToBroker(KafkaNetworkingData*);
    void handleCreateTopic(KafkaNetworkingData*);
    void handleCreateStream(KafkaNetworkingData*);
    void handleWriteToTopic(KafkaNetworkingData*);

    BEGIN_STATE_MAP
		STATE_MAP_ENTRY(&KafkaNetworking::handleNoConnection)
        STATE_MAP_ENTRY(&KafkaNetworking::handleConnectedToBroker)
        STATE_MAP_ENTRY(&KafkaNetworking::handleCreateTopic)
        STATE_MAP_ENTRY(&KafkaNetworking::handleCreateStream)
        STATE_MAP_ENTRY(&KafkaNetworking::handleWriteToTopic)
    END_STATE_MAP

    enum States {
        NO_CONNECTION_STATE=0,
        ATTEMPT_WIFI_CONNECTION_STATE,
        ONLY_WIFI_CONNECTED_STATE,
        ATTEMPT_BROKER_CONNECTION_STATE,
        CONNECTED_TO_BROKER_STATE,
        CREATE_TOPIC_STATE,
        CREATE_STREAM_STATE,
        WRITE_TO_TOPIC_STATE,
        TOTAL_NUMBER_OF_STATES
    };
};

}