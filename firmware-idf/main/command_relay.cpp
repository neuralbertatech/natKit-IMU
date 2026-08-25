#include "command_relay.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espnow_link.hpp"
#include "gateway_net.hpp"
#include "registry.hpp"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-cmd";

// Whether this image publishes and subscribes for itself. False means it is fed
// by a gateway over the serial uplink, and has no broker session to subscribe on.
#if defined(CONFIG_NATKIT_PRIMARY_WIFI_UPLINK) || \
    defined(CONFIG_NATKIT_PRIMARY_ETH_UPLINK)
constexpr bool kHasOwnNetwork = true;
#else
constexpr bool kHasOwnNetwork = false;
#endif

// The topic the backend publishes on. ⚠️ "receiving" is the bridge's direction,
// not ours: the bridge republishes every Kafka record under natKit/receiving/,
// so what the server SENDS arrives here on receiving/ and what we send goes out
// on sending/. Getting this backwards subscribes to our own output.
constexpr char kCommandTopicTemplate[] =
    "natKit/receiving/Command-%" PRIu64 "-Json-NatExecutionCommandV1";

CommandRelayStats sStats{};

// --- in-flight commands ------------------------------------------------------
//
// ⚠️ A COMMAND IS NOT DELIVERED BECAUSE THE RADIO ACCEPTED IT. esp_now_send
// returning ESP_OK means the packet was queued, not that anything heard it, and
// on this rig nodes are not expected to be always on -- a command for a sleeping
// or out-of-range node would otherwise be reported as sent and simply never
// happen. Measured on the answer path before this existed: a fifth of single
// unacknowledged packets were lost outright.
//
// So every command is held until the device acknowledges it, retransmitted until
// it does, and explicitly FAILED if it never does. The failure is published as an
// ordinary answer, because the server is waiting for one either way and silence
// would just become a timeout with no explanation attached.

// The MQTT payload copy. ⚠️ Not on any callback's stack -- see onMessage.
constexpr size_t kMaxDocument = 512;

constexpr size_t kMaxInFlight = 4;
constexpr uint8_t kMaxAttempts = 6;
constexpr uint32_t kRetryIntervalMs = 400;
constexpr uint32_t kServiceIntervalMs = 100;

struct Pending {
  bool in_use = false;
  CommandFrame frame{};
  uint8_t attempts = 0;
  uint64_t next_attempt_us = 0;
};

Pending sPending[kMaxInFlight];
portMUX_TYPE sPendingLock = portMUX_INITIALIZER_UNLOCKED;

// Publishes the "nobody ever acknowledged this" record. Deliberately shaped like
// a device answer: ok = false, final = true, so whatever is waiting stops waiting
// and is told why rather than timing out.
void publishUndelivered(const CommandFrame &frame) {
  CommandLogFrame log{};
  log.device_id = frame.device_id;
  std::strncpy(log.command_id, frame.command_id, sizeof(log.command_id) - 1);
  log.ok = 0;
  log.final = 1;
  // ⚠️ "NOT ACKNOWLEDGED" IS NOT "NOT EXECUTED", and the message says so because
  // the difference has already caused one wrong conclusion. A command whose
  // acknowledgement was lost on the way back was still received and still ran --
  // the leaf de-duplicates by command id precisely so the retransmissions do not
  // run it again. So this record means "no confirmation", not "no effect", and
  // anything that needs to know what actually happened must ask.
  std::snprintf(log.message, sizeof(log.message),
                "no ack for \"%s\" from %" PRIu64 " after %u tries. Node may be "
                "off or out of range -- but if only the ack was lost it DID run. "
                "Re-read the state; do not assume either way",
                frame.command, frame.device_id, kMaxAttempts);
  espNowPrimaryPublishAnswer(log);
}

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

// ⚠️ THE MQTT TASK ONLY COPIES. Parsing used to happen here, on esp-mqtt's own
// task, with a 512-byte document buffer and cJSON both on its stack -- and the
// primary panicked (ESP_RST_PANIC, found only because the reset reason is
// published; its console cannot be read). The same discipline the leaf already
// follows for its radio callback: copy out, return, do the work on a task that
// owns a stack sized for it.
struct InboundMessage {
  uint64_t device_id;
  uint16_t length;
  char document[kMaxDocument];
};

QueueHandle_t sInbound = nullptr;

// The copy-and-queue half, shared by both ways in. Callers differ only in how
// they learned the device id; from here down there is one path.
bool enqueueDocument(uint64_t device_id, const char *payload,
                     size_t payload_len) {
  if (payload_len == 0 || payload_len >= kMaxDocument || sInbound == nullptr) {
    ++sStats.malformed;
    return false;
  }
  // Allocated from the heap rather than this task's stack for the same reason.
  auto *message = static_cast<InboundMessage *>(malloc(sizeof(InboundMessage)));
  if (message == nullptr) {
    ++sStats.malformed;
    return false;
  }
  message->device_id = device_id;
  message->length = static_cast<uint16_t>(payload_len);
  std::memcpy(message->document, payload, payload_len);
  message->document[payload_len] = '\0';
  if (xQueueSend(sInbound, &message, 0) != pdTRUE) {
    ++sStats.malformed;
    free(message);
    return false;
  }
  return true;
}

void onMessage(const char *topic, size_t topic_len, const char *payload,
               size_t payload_len) {
  ++sStats.received;

  uint64_t device_id = 0;
  if (!deviceIdFromTopic(topic, topic_len, device_id)) {
    ++sStats.malformed;
    return;
  }
  enqueueDocument(device_id, payload, payload_len);
}

void handleMessage(const uint64_t device_id, char *document) {
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

  // Held BEFORE the first send, so an acknowledgement that arrives immediately
  // still finds its entry. Registering after would race on a fast link and leave
  // the command retransmitting against an ack it had already received.
  bool held = false;
  portENTER_CRITICAL(&sPendingLock);
  for (Pending &slot : sPending) {
    if (!slot.in_use) {
      slot.in_use = true;
      slot.frame = frame;
      slot.attempts = 1;
      slot.next_attempt_us = static_cast<uint64_t>(esp_timer_get_time()) +
                             kRetryIntervalMs * 1000ULL;
      held = true;
      break;
    }
  }
  portEXIT_CRITICAL(&sPendingLock);
  if (!held) {
    ESP_LOGW(kTag, "%u commands already in flight; \"%s\" is sent once only",
             static_cast<unsigned>(kMaxInFlight), frame.command);
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

void commandRelayNoteAck(const uint64_t device_id, const char *command_id) {
  portENTER_CRITICAL(&sPendingLock);
  for (Pending &slot : sPending) {
    if (slot.in_use && slot.frame.device_id == device_id &&
        std::strncmp(slot.frame.command_id, command_id, kCommandIdMax) == 0) {
      slot.in_use = false;
      ++sStats.delivered;
      break;
    }
  }
  portEXIT_CRITICAL(&sPendingLock);
}

void commandRelayService() {
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  // The frame is copied OUT of the critical section before it is sent: sending
  // holds the radio and must not run with interrupts disabled.
  CommandFrame to_send{};
  bool have_send = false;
  CommandFrame to_fail{};
  bool have_fail = false;

  portENTER_CRITICAL(&sPendingLock);
  for (Pending &slot : sPending) {
    if (!slot.in_use || now < slot.next_attempt_us) {
      continue;
    }
    if (slot.attempts >= kMaxAttempts) {
      to_fail = slot.frame;
      have_fail = true;
      slot.in_use = false;
      break;
    }
    ++slot.attempts;
    slot.next_attempt_us = now + kRetryIntervalMs * 1000ULL;
    to_send = slot.frame;
    have_send = true;
    break;
  }
  portEXIT_CRITICAL(&sPendingLock);

  if (have_send) {
    ++sStats.retransmits;
    espNowPrimarySendCommand(to_send);
  }
  if (have_fail) {
    ++sStats.undelivered;
    ESP_LOGW(kTag, "giving up on \"%s\" for device %" PRIu64, to_fail.command,
             to_fail.device_id);
    publishUndelivered(to_fail);
  }
}

// Owns both the parsing and the retransmissions, so all command work happens on
// one task with one stack, and neither the MQTT task nor the radio callback does
// anything but hand something over.
void relayTask(void *) {
  InboundMessage *message = nullptr;
  while (true) {
    if (sInbound != nullptr &&
        xQueueReceive(sInbound, &message, pdMS_TO_TICKS(kServiceIntervalMs)) ==
            pdTRUE) {
      handleMessage(message->device_id, message->document);
      free(message);
      message = nullptr;
    }
    commandRelayService();
  }
}

esp_err_t commandRelayStart() {
  // ⚠️ THE QUEUE BEFORE THE SUBSCRIPTION, not after. Installing the handler and
  // subscribing first left a window in which an arriving command found
  // sInbound == nullptr and was counted MALFORMED -- a command discarded and
  // then blamed on the sender. Narrow, but it is the first command after boot
  // that falls into it, which is exactly the one somebody is watching for.
  //
  // 6144, and generously: this task parses JSON, formats 64-bit values into log
  // lines, and holds a CommandFrame or two. It is the only place command work
  // happens, so it is the only stack that has to be right.
  sInbound = xQueueCreate(4, sizeof(InboundMessage *));
  // Its own task rather than the primary's 1 Hz console loop: a retry interval
  // measured in seconds would make a command that needed one arrive after the
  // server had already stopped waiting.
  xTaskCreate(relayTask, "cmd-relay", 6144, nullptr, 4, nullptr);

  // ⚠️ ONLY WHEN THIS PRIMARY HAS A NETWORK OF ITS OWN. Behind a gateway there
  // is no MQTT client, and subscribing anyway would leave `subscriptions`
  // counting topics nobody is listening to -- a counter reporting the wrong
  // thing, on the one path where the operator most needs to know whether
  // commands can arrive. Commands reach a gateway-fed primary through
  // commandRelaySubmit() instead.
  if (kHasOwnNetwork) {
    gatewaySetMessageHandler(onMessage);
    commandRelayRefreshSubscriptions();
  } else {
    ESP_LOGI(kTag,
             "no MQTT on this primary: commands arrive over the serial uplink "
             "(TEC-NATKIT-92), not by subscription");
  }
  return ESP_OK;
}

void commandRelayRefreshSubscriptions() {
  // ⚠️ THE GUARD LIVES HERE, NOT AT THE CALL SITES. It was in commandRelayStart()
  // only, and primary.cpp calls this again every second from its status loop --
  // so a gateway-fed primary went on "subscribing" once a second with no broker
  // session and reported commands_subscriptions = 5.
  //
  // Caught by decoding the primary's own status frame off the broker after the
  // first successful command: every other counter reconciled and that one did
  // not. A counter that says a rig can receive commands by a route it does not
  // have is worse than no counter, because it is the number you would check to
  // find out why a command never arrived.
  if (!kHasOwnNetwork) {
    sStats.subscriptions = 0;
    return;
  }

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

bool commandRelaySubmit(uint64_t device_id, const char *document,
                        size_t length) {
  // Counted as received on the same counter as an MQTT arrival, deliberately:
  // `received` should mean "a command reached this primary", not "a command
  // reached this primary by the route I was thinking of". A rig behind a gateway
  // would otherwise report zero commands received while relaying them fine.
  ++sStats.received;
  return enqueueDocument(device_id, document, length);
}

const CommandRelayStats &commandRelayStats() { return sStats; }

}  // namespace natkit
