#include <KafkaNetworking.hpp>
#include <macros.hpp>

#define MAX_CLIENTS 4 //ESP32 supports up to 10 but I have not tested it yet
#define WIFI_CHANNEL 6 //2.4ghz channel 6 https://en.wikipedia.org/wiki/List_of_WLAN_channels#2.4_GHz_(802.11b/g/n/ax)


namespace natKit {

const IPAddress localIP(4, 3, 2, 1); // the IP address the web server, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1); // IP address of the network should be the same as the local IP for captive portals
const IPAddress subnetMask(255,255,255,0); //no need to change: https://avinetworks.com/glossary/subnet-mask/


void KafkaNetworking::connect(KafkaNetworkingData* data) {
    BEGIN_TRANSITION_MAP                            // - Current State -
        TRANSITION_MAP_ENTRY (ATTEMPT_WIFI_CONNECTION_STATE)  // NO_CONNECTION
        TRANSITION_MAP_ENTRY (EVENT_IGNORED)  // ATTEMPT_WIFI_CONNECTION_STATE
        TRANSITION_MAP_ENTRY (ATTEMPT_BROKER_CONNECTION_STATE)  // NO_CONNONLY_WIFI_CONNECTED_STATE
        TRANSITION_MAP_ENTRY (EVENT_IGNORED)  // ATTEMPT_BROKER_CONNECTION_STATE
        TRANSITION_MAP_ENTRY (EVENT_IGNORED)  // CONNECTED_TO_BROKER
        TRANSITION_MAP_ENTRY (EVENT_IGNORED)  // CREATE_TOPIC
        TRANSITION_MAP_ENTRY (EVENT_IGNORED)  // CREATE_STREAM
        TRANSITION_MAP_ENTRY (EVENT_IGNORED)  // WRITE_TO_TOPIC
    END_TRANSITION_MAP(data)
}

void KafkaNetworking::handleNoConnection(KafkaNetworkingData* data) {
    DEBUG_SERIAL.printf("No connection state. Data is %p\n", data);
}

void KafkaNetworking::handleAttemptWifiConnection(KafkaNetworkingData* data) {
    DEBUG_SERIAL.printf("Attempting to connect to wifi. Data is %p\n", data);

    WiFi.mode(WIFI_AP_STA); //access point mode
    WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
    WiFi.softAP(data->connectionConfig.networkSsid, data->connectionConfig.networkPassword, WIFI_CHANNEL, 0, MAX_CLIENTS);
}

}