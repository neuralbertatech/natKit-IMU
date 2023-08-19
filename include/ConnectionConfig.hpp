#pragma once

#include <ESPAsyncWebServer.h>

#include <macros.hpp>


struct ConnectionConfig {
    const char* networkSsid = nullptr;
    const char* networkPassword = nullptr;
    const char* natKitServerAddress = nullptr;
    const char* natKitServerPort = nullptr;

    void configureSetting(AsyncWebParameter* parameter) {
        if (parameter == nullptr) {
            DEBUG_SERIAL.printf("Error: ConnectionConfig::configureSetting received a nullptr!");
            return;
        }

        const auto name = parameter->name().c_str();
        const auto value = parameter->value().c_str();
        if (strcmp(name, "networkSsid") == 0) {
            setString(networkSsid, value);
            DEBUG_SERIAL.printf("Info: In ConnectionConfig setting '%s' to '%s'\n", "networkSsid", networkSsid);
        } else if (strcmp(name, "networkPassword") == 0) {
            setString(networkPassword, value);
            DEBUG_SERIAL.printf("Info: In ConnectionConfig setting '%s' to '%s'\n", "networkPassword", networkPassword);
        } else if (strcmp(name, "natKitServerAddress") == 0) {
            setString(natKitServerAddress, value);
            DEBUG_SERIAL.printf("Info: In ConnectionConfig setting '%s' to '%s'\n", "natKitServerAddress", natKitServerAddress);
        } else if (strcmp(name, "natKitServerPort") == 0) {
            setString(natKitServerPort, value);
            DEBUG_SERIAL.printf("Info: In ConnectionConfig setting '%s' to '%s'\n", "natKitServerPort", natKitServerPort);
        } else {
            DEBUG_SERIAL.printf("Error: ConnectionConfig does not have the setting '%s'\n", parameter->name().c_str());
        }
    }

private:
    void setString(const char* &variable, const char* value) {
        free((char*)variable);
        variable = strdup(value);
    }
};