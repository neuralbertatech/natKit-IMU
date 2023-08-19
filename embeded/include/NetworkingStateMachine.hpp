#pragma once 

#include <ConnectionConfig.hpp>
#include <MicroStateMachine.hpp>
#include <StateActionImpl.hpp>
#include <StateMapRow.hpp>

namespace natKit {

struct NetworkingEventData {
    const ConnectionConfig* connectionConfig;
    const char* url;
    const char* payload;
    const char* response;
    uint16_t responseCode;
};

class NetworkingStateMachine : public MicroStateMachine {
public:
    NetworkingStateMachine() : MicroStateMachine(static_cast<uint8_t>(NetworkingStates::TOTAL_NUMBER_OF_STATES)) {}

    void connect(NetworkingEventData*);
    void disconnect(NoEventData*);
    void getRequest(NetworkingEventData*);
    void postRequest(NetworkingEventData*);

private:
    enum class NetworkingStates {
        DISCONNECTED_STATE=0,
        CONNECTING_STATE,
        CONNECTED_STATE,
        SEND_REQUEST_STATE,
        WAITING_FOR_RESPONSE_STATE,
        TOTAL_NUMBER_OF_STATES
    };

    void handleDisconnectedState(const NoEventData*);
    StateActionImpl<NetworkingStateMachine, NoEventData, &NetworkingStateMachine::handleDisconnectedState> disconnectedState;

    void handleConnectingState(const NetworkingEventData*);
    StateActionImpl<NetworkingStateMachine, NetworkingEventData, &NetworkingStateMachine::handleConnectingState> connectingState;

    void handleConnectedState(const NoEventData*);
    StateActionImpl<NetworkingStateMachine, NoEventData, &NetworkingStateMachine::handleConnectedState> connectedState;

    void handleSendRequestState(const NetworkingEventData*);
    StateActionImpl<NetworkingStateMachine, NetworkingEventData, &NetworkingStateMachine::handleSendRequestState> sendRequestState;


    void handleWaitingForResponseState(const NetworkingEventData*);
    StateActionImpl<NetworkingStateMachine, NetworkingEventData, &NetworkingStateMachine::handleWaitingForResponseState> waitingForResponseState;

    virtual const StateMapRow* getStateMap() override {
        static const StateMapRow stateMap[] = {
            &disconnectedState,
            &connectingState,
            &connectedState,
            &sendRequestState,
            &waitingForResponseState
        };
        static_assert((sizeof(stateMap) / sizeof(StateMapRow)) == static_cast<uint8_t>(NetworkingStates::TOTAL_NUMBER_OF_STATES),
                      "NetworkingStateMachine::getStateMap has the wrong number of mapped states");
        return &stateMap[0];
    }
};

}