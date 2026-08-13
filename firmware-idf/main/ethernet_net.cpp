#include "ethernet_net.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "sdkconfig.h"

namespace natkit {
namespace {

constexpr char kTag[] = "natkit-eth";

EthernetStats sStats{};
esp_eth_handle_t sEth = nullptr;
esp_netif_t *sNetif = nullptr;

void ethEventHandler(void *, esp_event_base_t, int32_t id, void *data) {
  switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
      sStats.link_up = true;
      ++sStats.link_ups;
      esp_eth_handle_t handle = *static_cast<esp_eth_handle_t *>(data);
      uint8_t mac[6] = {};
      esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
      std::memcpy(sStats.mac, mac, 6);
      ESP_LOGI(kTag, "link up, mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
      break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
      sStats.link_up = false;
      sStats.got_ip = false;  // an address without a link is not an address
      ++sStats.link_downs;
      ESP_LOGW(kTag, "link DOWN (%lu so far) -- cable, switch, or the W5500",
               static_cast<unsigned long>(sStats.link_downs));
      break;
    default:
      break;
  }
}

void ipEventHandler(void *, esp_event_base_t, int32_t, void *data) {
  const auto *event = static_cast<ip_event_got_ip_t *>(data);
  sStats.got_ip = true;
  sStats.ip = event->ip_info.ip.addr;
  ESP_LOGI(kTag, "ip " IPSTR, IP2STR(&event->ip_info.ip));
}

}  // namespace

esp_err_t ethernetStart() {
  ESP_ERROR_CHECK(esp_netif_init());
  // Tolerated rather than checked: the WiFi path may already have created the
  // default loop, and on this board ESP-NOW will want it too. ESP_ERR_INVALID_STATE
  // here means "already exists", which is fine and is not an error worth aborting
  // a boot over.
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
    return loop;
  }

  esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
  sNetif = esp_netif_new(&netif_cfg);
  if (sNetif == nullptr) {
    ESP_LOGE(kTag, "could not create the ethernet netif");
    return ESP_FAIL;
  }

  // The SPI bus. A host of its OWN, not the sensor's: the BNO08x sits on
  // SPI2_HOST (board_config.hpp), and while this board carries no sensor, sharing
  // a bus between a 1 MHz hub that is fussy about timing and a 36 MHz Ethernet
  // controller is a class of problem worth never having.
  spi_bus_config_t bus{};
  bus.mosi_io_num = CONFIG_NATKIT_ETH_SPI_MOSI_GPIO;
  bus.miso_io_num = CONFIG_NATKIT_ETH_SPI_MISO_GPIO;
  bus.sclk_io_num = CONFIG_NATKIT_ETH_SPI_SCLK_GPIO;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  ESP_ERROR_CHECK(spi_bus_initialize(
      static_cast<spi_host_device_t>(CONFIG_NATKIT_ETH_SPI_HOST), &bus,
      SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev{};
  dev.mode = 0;
  dev.clock_speed_hz = CONFIG_NATKIT_ETH_SPI_CLOCK_MHZ * 1000 * 1000;
  dev.queue_size = 20;
  dev.spics_io_num = CONFIG_NATKIT_ETH_SPI_CS_GPIO;

  eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(
      static_cast<spi_host_device_t>(CONFIG_NATKIT_ETH_SPI_HOST), &dev);
  w5500.int_gpio_num = CONFIG_NATKIT_ETH_SPI_INT_GPIO;

  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.phy_addr = 1;
  phy_cfg.reset_gpio_num = CONFIG_NATKIT_ETH_PHY_RST_GPIO;

  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
  if (mac == nullptr || phy == nullptr) {
    ESP_LOGE(kTag, "could not create the W5500 mac/phy");
    return ESP_FAIL;
  }

  // The W5500 signals over a GPIO interrupt, so the shared ISR service has to
  // exist before the driver registers its handler. Without it the driver logs
  // "GPIO isr service is not installed" and the link never comes up -- it does
  // not fail loudly, it just never reports a connection.
  const esp_err_t isr = gpio_install_isr_service(0);
  if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "could not install the GPIO ISR service: %s",
             esp_err_to_name(isr));
    return isr;
  }

  esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &sEth));

  // ⚠️ The W5500 HAS NO MAC ADDRESS OF ITS OWN -- no eFuse, no OTP. Left unset it
  // comes up as all-zeroes or a vendor default, and two of these boards on one
  // network would collide. Derived from the S3's own base MAC so it is stable per
  // board and unique across boards, which is the same property the device id
  // relies on.
  uint8_t derived[6] = {};
  ESP_ERROR_CHECK(esp_read_mac(derived, ESP_MAC_ETH));
  ESP_ERROR_CHECK(esp_eth_ioctl(sEth, ETH_CMD_S_MAC_ADDR, derived));

  ESP_ERROR_CHECK(
      esp_netif_attach(sNetif, esp_eth_new_netif_glue(sEth)));
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             ethEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             ipEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_eth_start(sEth));

  ESP_LOGI(kTag,
           "W5500 up on spi host %d at %d MHz (sclk %d, mosi %d, miso %d, cs %d, int "
           "%d, rst %d), mac %02x:%02x:%02x:%02x:%02x:%02x. Waiting for a link "
           "in the background -- the radio side keeps running regardless.",
           CONFIG_NATKIT_ETH_SPI_HOST, CONFIG_NATKIT_ETH_SPI_CLOCK_MHZ,
           CONFIG_NATKIT_ETH_SPI_SCLK_GPIO, CONFIG_NATKIT_ETH_SPI_MOSI_GPIO,
           CONFIG_NATKIT_ETH_SPI_MISO_GPIO, CONFIG_NATKIT_ETH_SPI_CS_GPIO,
           CONFIG_NATKIT_ETH_SPI_INT_GPIO, CONFIG_NATKIT_ETH_PHY_RST_GPIO,
           derived[0], derived[1], derived[2], derived[3], derived[4], derived[5]);
  return ESP_OK;
}

const EthernetStats &ethernetStats() { return sStats; }

}  // namespace natkit
