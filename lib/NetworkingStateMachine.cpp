#include <NetworkingStateMachine.hpp>

namespace natKit {

void NetworkingStateMachine::connect(const NetworkingEventData* data) {
    static const uint8_t transisions[] = {
        static_cast<uint8_t>(NetworkingStates::CONNECTING_STATE),  // DISCONNECTED_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                       // CONNECTING_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                       // CONNECTED_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                       // SEND_REQUEST_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                       // WAITING_FOR_RESPONSE_STATE
    };
    processExternalEvent(transisions[getCurrentState()], data);
    static_assert((sizeof(transisions) / sizeof(uint8_t)) == static_cast<uint8_t>(NetworkingStates::TOTAL_NUMBER_OF_STATES),
                  "NetworkingStateMachine::connect has the wrong number of transsitions");
}

void NetworkingStateMachine::disconnect(const NetworkingEventData* data) {
    static const uint8_t transisions[] = {
        static_cast<uint8_t>(EVENT_IGNORED),                         // DISCONNECTED_STATE
        static_cast<uint8_t>(NetworkingStates::DISCONNECTED_STATE),  // CONNECTING_STATE
        static_cast<uint8_t>(NetworkingStates::DISCONNECTED_STATE),  // CONNECTED_STATE
        static_cast<uint8_t>(NetworkingStates::DISCONNECTED_STATE),  // SEND_REQUEST_STATE
        static_cast<uint8_t>(NetworkingStates::DISCONNECTED_STATE),  // WAITING_FOR_RESPONSE_STATE
    };
    processExternalEvent(transisions[getCurrentState()], data);
    static_assert((sizeof(transisions) / sizeof(uint8_t)) == static_cast<uint8_t>(NetworkingStates::TOTAL_NUMBER_OF_STATES),
                  "NetworkingStateMachine::disconnect has the wrong number of transsitions");
}

void NetworkingStateMachine::getRequest(const NetworkingEventData* data) {
    static const uint8_t transisions[] = {
        static_cast<uint8_t>(EVENT_IGNORED),                         // DISCONNECTED_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                         // CONNECTING_STATE
        static_cast<uint8_t>(NetworkingStates::SEND_REQUEST_STATE),  // CONNECTED_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                         // SEND_REQUEST_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                         // WAITING_FOR_RESPONSE_STATE
    };
    processExternalEvent(transisions[getCurrentState()], data);
    static_assert((sizeof(transisions) / sizeof(uint8_t)) == static_cast<uint8_t>(NetworkingStates::TOTAL_NUMBER_OF_STATES),
                  "NetworkingStateMachine::getRequest has the wrong number of transsitions");
}

void NetworkingStateMachine::postRequest(const NetworkingEventData* data) {
    static const uint8_t transisions[] = {
        static_cast<uint8_t>(EVENT_IGNORED),                         // DISCONNECTED_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                         // CONNECTING_STATE
        static_cast<uint8_t>(NetworkingStates::SEND_REQUEST_STATE),  // CONNECTED_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                         // SEND_REQUEST_STATE
        static_cast<uint8_t>(EVENT_IGNORED),                         // WAITING_FOR_RESPONSE_STATE
    };
    processExternalEvent(transisions[getCurrentState()], data);
    static_assert((sizeof(transisions) / sizeof(uint8_t)) == static_cast<uint8_t>(NetworkingStates::TOTAL_NUMBER_OF_STATES),
                  "NetworkingStateMachine::postRequest has the wrong number of transsitions");
}

}