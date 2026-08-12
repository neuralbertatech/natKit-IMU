#include "registry.hpp"

#include <cinttypes>
#include <cstring>

#include "device_id.hpp"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-registry";

// An unset `bool` Kconfig emits NO SYMBOL, so this cannot be read as a value the
// way an int option can -- referencing it directly fails to compile whenever the
// option is off. The same shape already broke every role's build once, when
// CONFIG_NATKIT_ESPNOW_CHANNEL was made to depend on the probe switch. Resolved
// to a real constant here, once.
#ifdef CONFIG_NATKIT_REGISTRY_SEALED
constexpr bool kSealedByBuild = true;
#else
constexpr bool kSealedByBuild = false;
#endif
constexpr char kNamespace[] = "natkit-reg";
constexpr char kKeyRoster[] = "roster";
constexpr char kKeySealed[] = "sealed";

RegistryEntry sEntries[kRegistryMaxNodes];
bool sSealed = false;
uint32_t sRejections = 0;

// Persisted as one blob rather than a key per node. The roster is small, always
// read and written whole, and a partially-written set of per-node keys after a
// power cut is a state this would then have to reason about.
esp_err_t persistRoster() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_blob(handle, kKeyRoster, sEntries, sizeof(sEntries));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err;
}

RegistryEntry *find(const uint8_t *mac) {
  for (RegistryEntry &entry : sEntries) {
    if (entry.in_use && std::memcmp(entry.mac, mac, 6) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

esp_err_t registryLoad() {
  std::memset(sEntries, 0, sizeof(sEntries));

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "no NVS namespace (%s); starting open with an empty roster",
             esp_err_to_name(err));
    return err;
  }

  size_t length = sizeof(sEntries);
  err = nvs_get_blob(handle, kKeyRoster, sEntries, &length);
  if (err != ESP_OK || length != sizeof(sEntries)) {
    // Not an error worth failing on: a first boot has no roster, and a roster
    // whose size no longer matches is from an older layout. Either way, starting
    // empty and open is the safe state -- it self-configures rather than
    // rejecting everything.
    std::memset(sEntries, 0, sizeof(sEntries));
    if (err != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(kTag, "roster unreadable (%s, %u bytes); starting empty",
               esp_err_to_name(err), (unsigned)length);
    }
  }

  uint8_t sealed = 0;
  if (nvs_get_u8(handle, kKeySealed, &sealed) == ESP_OK) {
    sSealed = sealed != 0;
  }
  nvs_close(handle);

#if CONFIG_NATKIT_REGISTRY_CLEAR_ON_BOOT
  // Before the seal is applied, or clearing a sealed registry would be refused
  // by registryClear() and the bench control would silently do nothing.
  ESP_LOGW(kTag, "CLEAR_ON_BOOT is set: forgetting every remembered node");
  sSealed = false;
  std::memset(sEntries, 0, sizeof(sEntries));
  persistRoster();
#endif

  // The build option is the state of the rig, applied every boot rather than
  // once. With no downward command channel yet (TEC-NATKIT-26's decision), this
  // is how a fielded primary gets frozen -- and making it idempotent means a
  // reflash cannot leave a rig sealed when its build says otherwise.
  if (sSealed != kSealedByBuild) {
    registrySeal(kSealedByBuild);
  }

  ESP_LOGI(kTag, "registry %s with %lu node(s) remembered",
           sSealed ? "SEALED" : "open (will learn any node that announces)",
           static_cast<unsigned long>(registryCount()));
  for (const RegistryEntry &entry : sEntries) {
    if (entry.in_use) {
      ESP_LOGI(kTag, "  %02x:%02x:%02x:%02x:%02x:%02x -> stream %" PRIu64,
               entry.mac[0], entry.mac[1], entry.mac[2], entry.mac[3],
               entry.mac[4], entry.mac[5], entry.device_id);
    }
  }
  return ESP_OK;
}

bool registryAccepts(const uint8_t *mac) {
  if (mac == nullptr) {
    return false;
  }
  if (find(mac) != nullptr) {
    return true;
  }

  if (sSealed) {
    ++sRejections;
    // Rate-limited by being on the rejection count rather than the packet: a
    // stranger streaming at 5 frames/s would otherwise own the console, and the
    // first few lines say everything the hundredth would.
    if (sRejections <= 5 || sRejections % 1000 == 0) {
      ESP_LOGW(kTag,
               "REJECTED %02x:%02x:%02x:%02x:%02x:%02x -- not on the sealed "
               "roster (%lu rejected so far). Another rig on our channel, or a "
               "node that needs adding before sealing.",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               static_cast<unsigned long>(sRejections));
    }
    return false;
  }

  for (RegistryEntry &entry : sEntries) {
    if (!entry.in_use) {
      entry.in_use = true;
      std::memcpy(entry.mac, mac, 6);
      entry.device_id = packMac(mac);
      ESP_LOGI(kTag,
               "learned %02x:%02x:%02x:%02x:%02x:%02x -> stream %" PRIu64
               " (registry is open; seal it to freeze the roster)",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], entry.device_id);
      const esp_err_t err = persistRoster();
      if (err != ESP_OK) {
        // Kept in RAM regardless. Losing the roster on the next boot is far
        // better than refusing a node that is really ours because NVS is full.
        ESP_LOGW(kTag, "could not persist the roster (%s); it holds until reboot",
                 esp_err_to_name(err));
      }
      return true;
    }
  }

  ++sRejections;
  ESP_LOGW(kTag,
           "REJECTED %02x:%02x:%02x:%02x:%02x:%02x -- the roster is full at %u "
           "nodes",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           (unsigned)kRegistryMaxNodes);
  return false;
}

bool registrySealed() { return sSealed; }

esp_err_t registrySeal(bool sealed) {
  sSealed = sealed;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_u8(handle, kKeySealed, sealed ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err == ESP_OK) {
    // Persist the roster at the same moment. Sealing a roster that then failed to
    // survive a reboot would reject every node on the next boot, which is the
    // worst possible outcome of a safety feature.
    err = persistRoster();
  }

  ESP_LOGW(kTag, "registry is now %s with %lu node(s)",
           sealed ? "SEALED" : "OPEN",
           static_cast<unsigned long>(registryCount()));
  return err;
}

esp_err_t registryClear() {
  if (sSealed) {
    ESP_LOGW(kTag, "refusing to clear a sealed registry; unseal it first");
    return ESP_ERR_INVALID_STATE;
  }
  std::memset(sEntries, 0, sizeof(sEntries));
  sRejections = 0;
  return persistRoster();
}

uint32_t registryCount() {
  uint32_t count = 0;
  for (const RegistryEntry &entry : sEntries) {
    if (entry.in_use) {
      ++count;
    }
  }
  return count;
}

uint32_t registryRejections() { return sRejections; }

const RegistryEntry *registryEntries() { return sEntries; }

}  // namespace natkit
