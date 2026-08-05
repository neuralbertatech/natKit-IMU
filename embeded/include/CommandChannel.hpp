#pragma once

// EXECUTION_COMMAND / LOGGING_LOG channel plumbing.
//
// The server addresses this device on a per-device command topic
// (Command-<id>-Json-NatExecutionCommandV1). The bridge republishes every Kafka
// record to MQTT under natKit/receiving/<topic>, so the device subscribes there.
// Whatever the command produces is published back on the device's log topic
// (Log-<id>-Json-NatLogV1), correlated by command_id.
//
// THREADING. PubSubClient is not thread-safe and all MQTT I/O lives on
// sendMessageTask, so the subscribe callback also runs on that task. But the SH2
// hub belongs to the networking/IMU task, so commands cannot be executed inline
// in the callback. Hence two queues:
//
//   MQTT callback (sendMessageTask) --requestQueue--> dispatch (IMU task)
//   dispatch (IMU task)             --logQueue-----> publish (sendMessageTask)
//
// Both carry fixed-size PODs, so nothing is allocated on either side and neither
// task can block the other.

#include <Arduino.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace natkit_command {

constexpr size_t COMMAND_ID_MAX = 40;
constexpr size_t COMMAND_NAME_MAX = 40;
constexpr size_t COMMAND_ARGS_MAX = 96;
constexpr size_t LOG_MESSAGE_MAX = 192;
// A command is a human-scale action (calibrate, ping); a handful in flight is
// already more than the device can meaningfully be doing at once.
constexpr uint8_t REQUEST_QUEUE_DEPTH = 4;
// Deeper, because one command may emit several progress records.
constexpr uint8_t LOG_QUEUE_DEPTH = 12;
// Commands are small flat JSON objects. Anything larger is not one of ours.
constexpr size_t MAX_COMMAND_PAYLOAD = 512;

struct CommandRequest {
    char command_id[COMMAND_ID_MAX];
    char command[COMMAND_NAME_MAX];
    char args[COMMAND_ARGS_MAX];  // raw JSON of the "args" object, if any
};

struct CommandLog {
    char command_id[COMMAND_ID_MAX];
    char command[COMMAND_NAME_MAX];
    char level[8];  // "info" | "warn" | "error"
    char message[LOG_MESSAGE_MAX];
    bool ok;
    bool terminal;  // true on the last record for this command_id
};

QueueHandle_t requestQueue = nullptr;
QueueHandle_t logQueue = nullptr;
// Set once at bring-up; used to decide whether a command addressed to a specific
// device is for us.
char deviceId[COMMAND_NAME_MAX] = {0};
// Diagnostics: how many commands arrived, ran, and were dropped.
uint32_t commandsReceived = 0;
uint32_t commandsDropped = 0;

inline bool init(const char* device_id) {
    strlcpy(deviceId, device_id == nullptr ? "" : device_id, sizeof(deviceId));
    if (requestQueue == nullptr) {
        requestQueue = xQueueCreate(REQUEST_QUEUE_DEPTH, sizeof(CommandRequest));
    }
    if (logQueue == nullptr) {
        logQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(CommandLog));
    }
    return requestQueue != nullptr && logQueue != nullptr;
}

// --- minimal JSON field reads --------------------------------------------
// Commands are flat objects with string/short values, so a full parser would be
// a dependency (and a heap) we do not need. These deliberately only understand
// that shape: a key at the top level whose value is a string, or a nested object
// captured verbatim. Anything else reads as absent.

// Copies the string value of "key" into out. Returns false if the key is missing
// or its value is not a string.
inline bool readStringField(const char* json, const char* key, char* out,
                            size_t out_size) {
    if (json == nullptr || key == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    char needle[COMMAND_NAME_MAX + 3];
    const int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_len <= 0 || needle_len >= (int)sizeof(needle)) {
        return false;
    }
    const char* at = strstr(json, needle);
    if (at == nullptr) {
        return false;
    }
    const char* cursor = at + needle_len;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor != ':') return false;
    ++cursor;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor != '"') return false;
    ++cursor;

    size_t written = 0;
    while (*cursor != '\0' && *cursor != '"' && written + 1 < out_size) {
        // Unescape only what a command payload can legitimately contain.
        if (*cursor == '\\' && *(cursor + 1) != '\0') {
            ++cursor;
            switch (*cursor) {
                case 'n': out[written++] = '\n'; break;
                case 't': out[written++] = '\t'; break;
                default:  out[written++] = *cursor; break;
            }
            ++cursor;
            continue;
        }
        out[written++] = *cursor++;
    }
    out[written] = '\0';
    // An unterminated string means the payload was truncated; treat as absent
    // rather than acting on half a value.
    return *cursor == '"';
}

// Captures the raw JSON of a nested object value (e.g. "args") verbatim, so a
// handler can read whatever it needs out of it. Returns false if absent or if it
// does not fit.
inline bool readObjectField(const char* json, const char* key, char* out,
                            size_t out_size) {
    if (json == nullptr || out == nullptr || out_size == 0) return false;
    out[0] = '\0';
    char needle[COMMAND_NAME_MAX + 3];
    const int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_len <= 0 || needle_len >= (int)sizeof(needle)) return false;
    const char* at = strstr(json, needle);
    if (at == nullptr) return false;
    const char* cursor = at + needle_len;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor != ':') return false;
    ++cursor;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor != '{') return false;

    int depth = 0;
    bool in_string = false;
    const char* start = cursor;
    for (; *cursor != '\0'; ++cursor) {
        if (in_string) {
            if (*cursor == '\\' && *(cursor + 1) != '\0') { ++cursor; continue; }
            if (*cursor == '"') in_string = false;
            continue;
        }
        if (*cursor == '"') { in_string = true; continue; }
        if (*cursor == '{') ++depth;
        if (*cursor == '}') {
            --depth;
            if (depth == 0) {
                const size_t length = (size_t)(cursor - start) + 1;
                if (length + 1 > out_size) return false;
                memcpy(out, start, length);
                out[length] = '\0';
                return true;
            }
        }
    }
    return false;
}

// --- log records ---------------------------------------------------------

// Queues a log record for publication. Safe to call from the command handler;
// never blocks (a full queue drops the record and says so on the console rather
// than stalling the IMU task).
inline bool emitLog(const char* command_id, const char* command,
                    const char* level, bool ok, bool terminal,
                    const char* format, ...) {
    if (logQueue == nullptr) return false;
    CommandLog record{};
    strlcpy(record.command_id, command_id == nullptr ? "" : command_id,
            sizeof(record.command_id));
    strlcpy(record.command, command == nullptr ? "" : command,
            sizeof(record.command));
    strlcpy(record.level, level == nullptr ? "info" : level, sizeof(record.level));
    record.ok = ok;
    record.terminal = terminal;

    va_list args;
    va_start(args, format);
    vsnprintf(record.message, sizeof(record.message), format, args);
    va_end(args);

    if (xQueueSend(logQueue, &record, 0) != pdTRUE) {
        Serial.printf("COMMAND: log queue full, dropped: %s\n", record.message);
        return false;
    }
    return true;
}

inline bool tryTakeLog(CommandLog& out) {
    return logQueue != nullptr && xQueueReceive(logQueue, &out, 0) == pdTRUE;
}

inline bool tryTakeRequest(CommandRequest& out) {
    return requestQueue != nullptr &&
           xQueueReceive(requestQueue, &out, 0) == pdTRUE;
}

// --- inbound MQTT ---------------------------------------------------------

// PubSubClient subscribe callback. Runs on the MQTT-owning task: it only parses
// and enqueues, never executes.
inline void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    if (payload == nullptr || length == 0) return;
    if (length >= MAX_COMMAND_PAYLOAD) {
        Serial.printf("COMMAND: payload of %u bytes on %s is too large; ignored\n",
                      length, topic == nullptr ? "?" : topic);
        ++commandsDropped;
        return;
    }
    char json[MAX_COMMAND_PAYLOAD];
    memcpy(json, payload, length);
    json[length] = '\0';
    ++commandsReceived;

    CommandRequest request{};
    if (!readStringField(json, "command", request.command,
                         sizeof(request.command)) ||
        request.command[0] == '\0') {
        Serial.printf("COMMAND: no \"command\" field in payload; ignored: %.80s\n",
                      json);
        ++commandsDropped;
        return;
    }
    // command_id is optional but is what correlates the log output, so a caller
    // that omits it gets records it cannot match up.
    readStringField(json, "command_id", request.command_id,
                    sizeof(request.command_id));
    readObjectField(json, "args", request.args, sizeof(request.args));

    // Addressing. "sensor" (or this device's id) is for us; "server" is not.
    // An absent target means "whoever is subscribed", i.e. us.
    char target[COMMAND_NAME_MAX] = {0};
    if (readStringField(json, "target", target, sizeof(target)) &&
        target[0] != '\0') {
        const bool for_us = strcmp(target, "sensor") == 0 ||
                            strcmp(target, deviceId) == 0;
        if (!for_us) {
            Serial.printf("COMMAND: %s targets \"%s\", not this device; ignored\n",
                          request.command, target);
            return;
        }
    }

    if (xQueueSend(requestQueue, &request, 0) != pdTRUE) {
        ++commandsDropped;
        emitLog(request.command_id, request.command, "error", false, true,
                "device busy: command queue full");
        return;
    }
    Serial.printf("COMMAND: queued %s (id=%s)\n", request.command,
                  request.command_id[0] == '\0' ? "-" : request.command_id);
}

}  // namespace natkit_command
