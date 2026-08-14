#include "commands.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "device_id.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "version.hpp"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-cmd";

// Short on purpose. Commands are human-initiated and answered within a second;
// a deep queue would only let a burst pile up and be answered after the backend
// had already timed out on all of them.
constexpr size_t kQueueDepth = 4;

// Three tries, 40 ms apart. Enough to survive a burst without the leaf spending
// meaningful airtime: a command is human-initiated, so three small packets are
// nothing next to the 10 frames/s of data sharing the same radio.
constexpr int kReplyAttempts = 3;
constexpr uint32_t kReplyGapMs = 40;

QueueHandle_t sQueue = nullptr;
CommandStats sStats{};

// Recently accepted command ids, so a retransmitted command is acknowledged
// again but executed only once. Small: the primary gives up after a couple of
// seconds, so nothing older than that can still be in flight.
constexpr size_t kSeenIds = 8;
char sSeenIds[kSeenIds][kCommandIdMax] = {};
size_t sSeenNext = 0;

QueueHandle_t queue() {
  if (sQueue == nullptr) {
    sQueue = xQueueCreate(kQueueDepth, sizeof(CommandFrame));
  }
  return sQueue;
}

void reply(const CommandFrame &request, const bool ok, const bool final,
           const char *format, ...) __attribute__((format(printf, 4, 5)));

void reply(const CommandFrame &request, const bool ok, const bool final,
           const char *format, ...) {
  CommandLogFrame log{};
  log.device_id = deviceId();
  std::strncpy(log.command_id, request.command_id, sizeof(log.command_id) - 1);
  log.ok = ok ? 1 : 0;
  log.final = final ? 1 : 0;

  va_list args;
  va_start(args, format);
  vsnprintf(log.message, sizeof(log.message), format, args);
  va_end(args);

  // ⚠️ SENT MORE THAN ONCE, DELIBERATELY, and it costs nothing because the
  // primary already deduplicates answers.
  //
  // Measured before this: ten commands were relayed 10/10, but only 8 of the 10
  // ANSWERS came back -- a fifth of them lost on the leg with no retry. Data
  // frames survive that because seqNo makes a gap visible and the next frame is
  // along in 10 ms; an answer is a single packet that nobody will ever send
  // again, and its loss looks exactly like a node that ignored the command.
  //
  // The primary already had to dedupe these -- the 802.11 MAC retransmits them
  // below ESP-NOW anyway, which is where the duplicates came from -- so repeating
  // deliberately adds no new failure mode, only redundancy. Spaced rather than
  // back-to-back, because the losses that matter are bursts.
  bool any_sent = false;
  for (int attempt = 0; attempt < kReplyAttempts; ++attempt) {
    if (espNowLinkSend(PacketType::kCommandLog, &log, sizeof(log))) {
      any_sent = true;
    }
    if (attempt + 1 < kReplyAttempts) {
      vTaskDelay(pdMS_TO_TICKS(kReplyGapMs));
    }
  }
  if (!any_sent) {
    ++sStats.reply_failed;
    // Logged locally as well, because an answer that could not be sent is
    // exactly the case where the console is the only remaining witness.
    ESP_LOGW(kTag, "could not send the answer to \"%s\": %s", request.command,
             log.message);
  }
}

// --- the commands themselves -------------------------------------------------

bool runPing(const CommandFrame &request) {
  reply(request, true, true, "pong from device %" PRIu64 ", up %llu s",
        deviceId(),
        static_cast<unsigned long long>(esp_timer_get_time() / 1000000));
  return true;
}

bool runVersion(const CommandFrame &request) {
  const esp_app_desc_t *app = esp_app_get_description();
  reply(request, true, true, "firmware %s, idf %s, built %s %s",
        NATKIT_IMU_IDF_FIRMWARE_NAME " " NATKIT_IMU_IDF_VERSION_STRING, app != nullptr ? app->idf_ver : "?",
        app != nullptr ? app->date : "?", app != nullptr ? app->time : "?");
  return true;
}

}  // namespace

bool commandsAlreadySeen(const char *command_id) {
  // A command with no id cannot be de-duplicated, and is let through: the
  // backend always sends one, so this is a hand-published command and running it
  // is the more useful failure.
  if (command_id == nullptr || command_id[0] == '\0') {
    return false;
  }
  for (const auto &seen : sSeenIds) {
    if (std::strncmp(seen, command_id, kCommandIdMax) == 0) {
      return true;
    }
  }
  std::strncpy(sSeenIds[sSeenNext], command_id, kCommandIdMax - 1);
  sSeenIds[sSeenNext][kCommandIdMax - 1] = '\0';
  sSeenNext = (sSeenNext + 1) % kSeenIds;
  return false;
}

bool commandsEnqueue(const CommandFrame &frame) {
  QueueHandle_t q = queue();
  if (q == nullptr) {
    return false;
  }
  ++sStats.received;
  // Zero timeout: this runs on the WiFi task and must not block it.
  if (xQueueSend(q, &frame, 0) != pdTRUE) {
    ++sStats.dropped_full;
    return false;
  }
  return true;
}

void commandsService() {
  QueueHandle_t q = queue();
  if (q == nullptr) {
    return;
  }
  CommandFrame request{};
  if (xQueueReceive(q, &request, 0) != pdTRUE) {
    return;
  }
  request.command[kCommandNameMax - 1] = '\0';
  request.command_id[kCommandIdMax - 1] = '\0';
  request.args[kCommandArgsMax - 1] = '\0';

  ESP_LOGI(kTag, "executing \"%s\" (id %s, args %s)", request.command,
           request.command_id[0] != '\0' ? request.command_id : "-",
           request.args[0] != '\0' ? request.args : "-");

  if (std::strcmp(request.command, "ping") == 0) {
    runPing(request);
  } else if (std::strcmp(request.command, "version") == 0) {
    runVersion(request);
  } else {
    // ⚠️ AN UNKNOWN COMMAND IS ANSWERED, not ignored. Silence is
    // indistinguishable from a node that never received it, from a radio that
    // dropped it, and from a node that is not there -- and those need completely
    // different debugging. Saying "I do not know that one" rules out all three.
    ++sStats.unknown;
    reply(request, false, true, "unknown command \"%s\"", request.command);
    return;
  }
  ++sStats.executed;
}

const CommandStats &commandStats() { return sStats; }

}  // namespace natkit
