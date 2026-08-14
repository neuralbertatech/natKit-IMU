#include "command_relay.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "espnow_link.hpp"
#include "gateway_net.hpp"
#include "registry.hpp"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-cmd";

// The topic the backend publishes on. ⚠️ "receiving" is the bridge's direction,
// not ours: the bridge republishes every Kafka record under natKit/receiving/,
// so what the server SENDS arrives here on receiving/ and what we send goes out
// on sending/. Getting this backwards subscribes to our own output.
constexpr char kCommandTopicTemplate[] =
    "natKit/receiving/Command-%" PRIu64 "-Json-NatExecutionCommandV1";

CommandRelayStats sStats{};

// Copies a string field out of a cJSON object, always NUL-terminating.
bool copyStringField(const cJSON *root, const char *name, char *out,
                     size_t out_size) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  std::strncpy(out, item->valuestring, out_size - 1);
  out[out_size - 1] = '\0';
  return true;
}

// Pulls the device id back out of the topic we subscribed to.
//
// ⚠️ FROM THE TOPIC, NOT FROM THE PAYLOAD. The command document has no device
// field -- addressing is the topic's job -- and inventing one would let a command
// published on one device's topic be executed by another.
bool deviceIdFromTopic(const char *topic, size_t topic_len, uint64_t &out) {
  static constexpr char kPrefix[] = "natKit/receiving/Command-";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (topic_len <= kPrefixLen || std::strncmp(topic, kPrefix, kPrefixLen) != 0) {
    return false;
  }
  uint64_t value = 0;
  size_t i = kPrefixLen;
  for (; i < topic_len && topic[i] >= '0' && topic[i] <= '9'; ++i) {
    value = value * 10 + static_cast<uint64_t>(topic[i] - '0');
  }
  if (i == kPrefixLen || value == 0) {
    return false;
  }
  out = value;
  return true;
}

void onMessage(const char *topic, size_t topic_len, const char *payload,
               size_t payload_len) {
  ++sStats.received;

  uint64_t device_id = 0;
  if (!deviceIdFromTopic(topic, topic_len, device_id)) {
    ++sStats.malformed;
    ESP_LOGW(kTag, "message on an unrecognised topic (%.*s)",
             static_cast<int>(topic_len), topic);
    return;
  }

  // ⚠️ THE PAYLOAD IS NOT NUL-TERMINATED. esp-mqtt hands out a pointer into its
  // own receive buffer with a separate length, and cJSON_Parse would read past
  // the end of it. Copied into a bounded buffer rather than parsed in place.
  char document[512];
  if (payload_len == 0 || payload_len >= sizeof(document)) {
    ++sStats.malformed;
    ESP_LOGW(kTag, "command payload is %u bytes, which does not fit %u",
             static_cast<unsigned>(payload_len),
             static_cast<unsigned>(sizeof(document)));
    return;
  }
  std::memcpy(document, payload, payload_len);
  document[payload_len] = '\0';

  cJSON *root = cJSON_Parse(document);
  if (root == nullptr) {
    ++sStats.malformed;
    ESP_LOGW(kTag, "command for device %" PRIu64 " is not valid JSON", device_id);
    return;
  }

  CommandFrame frame{};
  frame.device_id = device_id;
  const bool have_command =
      copyStringField(root, "command", frame.command, sizeof(frame.command));
  // command_id is optional: without one the backend cannot correlate an answer,
  // but the command is still worth executing and saying so beats refusing it.
  copyStringField(root, "command_id", frame.command_id, sizeof(frame.command_id));

  // args travels as raw JSON so the leaf can hand it to whatever knows the
  // command, without this file needing to know any command's shape.
  const cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
  if (cJSON_IsObject(args)) {
    char *printed = cJSON_PrintUnformatted(args);
    if (printed != nullptr) {
      if (std::strlen(printed) < sizeof(frame.args)) {
        std::strncpy(frame.args, printed, sizeof(frame.args) - 1);
      } else {
        // Truncating JSON produces a document that will not parse, so it is
        // dropped whole and said out loud rather than sent half-formed.
        ESP_LOGW(kTag, "args for \"%s\" are %u bytes, over the %u limit; dropped",
                 frame.command, static_cast<unsigned>(std::strlen(printed)),
                 static_cast<unsigned>(sizeof(frame.args)));
      }
      cJSON_free(printed);
    }
  }
  cJSON_Delete(root);

  if (!have_command) {
    ++sStats.malformed;
    ESP_LOGW(kTag, "command for device %" PRIu64 " has no \"command\" field",
             device_id);
    return;
  }

  if (!espNowPrimarySendCommand(frame)) {
    // espNowPrimarySendCommand distinguishes these in its own log line; both are
    // counted here because from the server's point of view they are the same
    // event -- it asked, and nothing happened.
    if (registryCount() == 0) {
      ++sStats.unknown_device;
    } else {
      ++sStats.send_failed;
    }
    return;
  }
  ++sStats.relayed;
}

}  // namespace

esp_err_t commandRelayStart() {
  gatewaySetMessageHandler(onMessage);
  commandRelayRefreshSubscriptions();
  return ESP_OK;
}

void commandRelayRefreshSubscriptions() {
  const RegistryEntry *entries = registryEntries();
  uint32_t subscribed = 0;
  char topic[96];
  for (size_t i = 0; i < kRegistryMaxNodes; ++i) {
    if (!entries[i].in_use) {
      continue;
    }
    std::snprintf(topic, sizeof(topic), kCommandTopicTemplate,
                  entries[i].device_id);
    if (gatewaySubscribe(topic)) {
      subscribed++;
    }
  }
  sStats.subscriptions = subscribed;
}

const CommandRelayStats &commandRelayStats() { return sStats; }

}  // namespace natkit
