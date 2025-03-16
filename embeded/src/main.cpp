#include <WiFi.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ConnectionConfig.hpp>
#include <macros.hpp>
#include <kafkaTopic.hpp>

//#include <Arduino.h> //not needed in the arduino ide

//Captive Portal
#include <DNSServer.h>
#include <esp_wifi.h> //Used for mpdu_rx_disable android workaround
#include <AsyncTCP.h> 	//https://github.com/me-no-dev/AsyncTCP using the latest dev version from @me-no-dev
#include <ESPAsyncWebServer.h> //https://github.com/me-no-dev/ESPAsyncWebServer using the latest dev version from @me-no-dev
#include <HTTPClient.h>
#include <esp_random.h>
#include <ImuData.hpp>
#include <ImuReader.hpp>
#include <libnatkit-core.hpp>
#include <MQTT.h>
#include <Time.hpp>
#include <esp_timer.h>
#include <PubSubClient.h>
// #include "mqtt_client.h"
#include "esp_netif.h"
//#include "esp_netif_sntp.h"
#include "esp_sntp.h"

// Dependency Graph (these are the libary versions used by this version of the code)
// |-- AsyncTCP @ 1.1.1+sha.ca8ac5f //Latest version of the main branch
// |-- ESP Async WebServer @ 1.2.3+sha.f71e3d4 //Latest version of the main branch
// |   |-- AsyncTCP @ 1.1.1+sha.ca8ac5f
// |   |-- FS @ 2.0.0
// |   |-- WiFi @ 2.0.0
// |-- DNSServer @ 2.0.0
// |   |-- WiFi @ 2.0.0

//Pre reading on the fundamentals of captive portals https://textslashplain.com/2022/06/24/captive-portals/

enum class NetworkMode {
  GetConfigFromUser,
  ConnectingToWifi,
  ConnectedToWifi
};

NetworkMode currentNetworkMode = NetworkMode::GetConfigFromUser;

//const char* ntpServer = "pool.ntp.org";
// const long  gmtOffset_sec = 3600;
// const int   daylightOffset_sec = 3600;
const char* ntpServer = nullptr;
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;

char ssid[32];
//const char * password = "12345678"; //Atleast 8 chars
const char * password = NULL; // no password

#define MAX_CLIENTS 4 //ESP32 supports up to 10 but I have not tested it yet
#define WIFI_CHANNEL 6 //2.4ghz channel 6 https://en.wikipedia.org/wiki/List_of_WLAN_channels#2.4_GHz_(802.11b/g/n/ax)


const IPAddress localIP(4, 3, 2, 1); // the IP address the web server, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1); // IP address of the network should be the same as the local IP for captive portals
const IPAddress subnetMask(255,255,255,0); //no need to change: https://avinetworks.com/glossary/subnet-mask/

const String localIPURL = "http://4.3.2.1"; //a string version of the local IP with http, used for redirecting clients to your webpage

uint8_t MAC_ADDRESS[6];
uint64_t UNIQUE_ID;

//WARNING IOS (and maybe macos) WILL NOT POP UP IF IT CONTAINS THE WORD "Success" https://www.esp8266.com/viewtopic.php?f=34&t=4398
//SAFARI (IOS) there is a 128KB limit to the size of the HTML. The HTML can reference external resources/images that bring the total over 128KB
//SAFARI (IOS) popup browser has some severe limitations (javascript disabled, cookies disabled, no .gz extension (even though gzip files are supported)) 
const char indexHtml[] PROGMEM = R"=====(
  <!DOCTYPE html> <html>
    <head>
      <title>natKit ESP32 Captive Portal</title>
      <style>
        body {background-color:#1B9FD6;}
        h1 {color: #202020;}
        h2 {color: #202020;}
      </style>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
    </head>
    <body>
      <h1>ESP32 Configuration Panel</h1>
      <form action="/action">
        <label for="networkSsid">SSID:</label><br>
        <input type="text" id="networkSsid" name="networkSsid" value="selk"><br>
        <label for="networkPassword">Password:</label><br>
        <input type="password" id="networkPassword" name="networkPassword" value=""><br>
        <label for="natKitServerAddress">natKit Core Server Address:</label><br>
        <input type="text" id="natKitServerAddress" name="natKitServerAddress" value=""><br>
        <label for="natKitServerPort">natKit Core Server Port:</label><br>
        <input type="text" id="natKitServerPort" name="natKitServerPort" value="38082"><br><br>
        <input type="submit" value="Submit">
      </form> 
    </body>
  </html>
)=====";
const char formCompletionResponseHtml[] PROGMEM = R"=====(
  <!DOCTYPE html> <html>
    <head>
      <title>natKit ESP32 Captive Portal</title>
      <style>
        body {background-color:#1B9FD6;}
        h1 {color: #202020;}
        h2 {color: #202020;}
      </style>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
    </head>
    <body>
      <h1>Form Submitted, Please Wait While Configuring...</h1>
    </body>
  </html>
)=====";

DNSServer dnsServer;
AsyncWebServer server(80);

ConnectionConfig connectionConfig{};

WiFiClient wifiClient;
// MQTTClient mqttClient{512, 512};
PubSubClient mqttClient{};
// esp_mqtt_client_config_t mqtt_cfg{};// = {
//     .broker.address.uri = "10.26.0.214",
// };
// esp_mqtt_client_handle_t mqttClient;

bool gConnectedToWifi = false;
bool gCreatedKafkaDataTopic = false;
bool gCreatedKafkaMetaTopic = false;
bool gSendKafkaMetaRecord = false;
enum class NetworkingStage {
  Disconnected,
  RecievedWifiCredentials,
  ConnectedToWifi,
  RecievedKafkaClusterId,
  CreatedKafkaDataTopic,
  CreatedKafkaMetaTopic,
  SentKafkaMetaRecord,
  WriteData
};
NetworkingStage currentNetworkingStage = NetworkingStage::Disconnected;
KafkaTopic* kafkaTopic = nullptr;

ImuReader imuReader{};
// TODO: Create a list of these objects so they can be queued
ImuData imuData{};
int imuCalibration{0};
bool IMU_DUMMY_DATA{false};

TaskHandle_t sendMessageTaskHandle = NULL;

void handleApRequestsTask(void*) {
  const auto delay = 1000 / portTICK_PERIOD_MS; // 1s
  while(true) {
    dnsServer.processNextRequest(); //I call this atleast every 10ms in my other projects (can be higher but I haven't tested it for stability)
    vTaskDelay(delay);
  }

  vTaskDelete( NULL );
}

void sendMessageTask(void*) {
  DEBUG_SERIAL.println("Send Message Task run");
  kafkaTopic->writeBulkDataRecord(connectionConfig, mqttClient);
  vTaskDelete( NULL );
}

void handleNetworkingStagesTask(void*) {
  const auto delay = 10 / portTICK_PERIOD_MS; // 10ms
  int task_delta = 0;
  long last_timestamp = 0;
  long current_timestamp = 0;
  Serial.println("Starting Network!");
  while(true) {
    last_timestamp = esp_timer_get_time();
    switch(currentNetworkingStage) {
      case NetworkingStage::Disconnected:
        break;
      
      case NetworkingStage::RecievedWifiCredentials:
        if (connectionConfig.networkSsid != nullptr && connectionConfig.networkPassword != nullptr) {
          currentNetworkMode = NetworkMode::ConnectingToWifi;
          WiFi.begin(connectionConfig.networkSsid, connectionConfig.networkPassword);
          while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(delay*100);
            DEBUG_SERIAL.println("Connecting to WiFi..");
          }
          gConnectedToWifi = true;
          IPAddress ipAddress{};
          ipAddress.fromString(connectionConfig.natKitServerAddress);
          mqttClient.setClient(wifiClient);
          mqttClient.setServer(ipAddress, 1883);
          // mqttClient.begin(ipAddress, 1883, wifiClient);
          while (!mqttClient.connect("natKit-IMU")) {
              Serial.print(".");
              DEBUG_SERIAL.println("Connecting to mqtt..");
          }

          static std::string ntpServerAddressString = connectionConfig.natKitServerAddress;
          ntpServerAddressString += ":123";
          ntpServer = ntpServerAddressString.c_str();
          //configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
          // mqtt_cfg.broker.address.uri = "10.26.0.214";
          // mqttClient = esp_mqtt_client_init(&mqtt_cfg);
          // esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
          // esp_mqtt_client_start(mqttClient);
          DEBUG_SERIAL.print("ESP32 IP on the WiFi network: ");
          DEBUG_SERIAL.println(WiFi.localIP());
          // request->send_P(200, "text/html", formCompletionResponseHtml);
          //uint8_t size;
          //const auto clusterId = KafkaTopic::getKafkaCluster(connectionConfig, size);
          DEBUG_SERIAL.println("AAAAAAAAA");
          // vTaskDelay(500);
          // if (size > 0) {
            // DEBUG_SERIAL.printf("HI %s\n", clusterId);
            const String boardId{UNIQUE_ID};
            DEBUG_SERIAL.println("BBBBBBBBB");

            // kafkaTopic = KafkaTopic::create(esp_random(), boardId);
            kafkaTopic = KafkaTopic::create(UNIQUE_ID, boardId);
            DEBUG_SERIAL.println("CCCCCCCCC");
            //kafkaTopic->createKafkaStream(connectionConfig);
            vTaskDelay(500);
            DEBUG_SERIAL.println("DDDDDDDDD");
            kafkaTopic->writeMetaRecord(connectionConfig, mqttClient);
            DEBUG_SERIAL.println("EEEEEEEE");
            currentNetworkingStage = NetworkingStage::WriteData;
          // }
        } else {
          DEBUG_SERIAL.println("Error: Either the network SSID or the network password was not set");
          // request->send_P(400, "text/html", formCompletionResponseHtml);
        }
        break;

      case NetworkingStage::WriteData:
        if (connectionConfig.networkSsid != nullptr && connectionConfig.networkPassword != nullptr) {

          DEBUG_SERIAL.println("Before");
          bool isBulkReady = kafkaTopic->writeDataRecord(connectionConfig, imuData, mqttClient);
          DEBUG_SERIAL.println("After");
          if (isBulkReady) {
            //xTaskCreate(sendMessageTask, "sendMessageTask", 2048, NULL, tskIDLE_PRIORITY+2, &sendMessageTaskHandle);
            DEBUG_SERIAL.println("Got signal indicating that bulk message is ready to send");
            sendMessageTask(NULL);
          } else {
            DEBUG_SERIAL.println("As you were");
          }
        }
        break;

    default:
        break;
    }
    current_timestamp = esp_timer_get_time();
    task_delta = (current_timestamp - last_timestamp) / 1000;
    if (task_delta >= delay) {
      // Serial.println("LAGGING!");
      log_i("Lagging!\n");
      continue;
    }
    vTaskDelay(delay - task_delta);
  }

  vTaskDelete( NULL );
}

void handleImuUpdateTask(void*) {
  const auto delay = 10 / portTICK_PERIOD_MS; // 10ms
  int iteration = 0;
  // int task_delta = 0;
  // long last_timestamp = 0;
  // long current_timestamp = 0;
  while(true) {
    if (!IMU_DUMMY_DATA) {
      // if (iteration == 0) {
      //   imuReader.calibrate();
      // }
      imuReader.update();
      imuReader.getImuData(&imuData);
    } else {
      imuData.timestamp = getTime();
      const auto randomNumber = esp_random();
      if (iteration % 600 == 0) {
        imuCalibration = randomNumber % 4;
      }
      for(int i = 0; i < 9; ++i) {
        imuData.data[i] = randomNumber + i;
      }
      imuData.accuracy = imuCalibration;
      iteration = (iteration + 1) % 600;
    }
    vTaskDelay(delay);
  }

  vTaskDelete( NULL );
}

void ntpSyncNotification(struct timeval* tv) {
  // TODO: Something here
}

void handleNetworkingStagesAndImuJoinedTask(void*) {
  const auto delay_len = 10 / portTICK_PERIOD_MS; // 10ms
  const auto delay_len_us = 10000; // 10 ms
  const auto half_delay_len_us = 5000; // 5 ms
  int task_delta = 0;
  long last_timestamp = 0;
  long current_timestamp = 0;
  Serial.println("Starting Network!");
  uint64_t next_expected_reading_timestamp = 0;
  while(true) {
    last_timestamp = esp_timer_get_time();
    switch(currentNetworkingStage) {
      case NetworkingStage::Disconnected:
        break;
      
      case NetworkingStage::RecievedWifiCredentials:
        if (connectionConfig.networkSsid != nullptr && connectionConfig.networkPassword != nullptr) {
          currentNetworkMode = NetworkMode::ConnectingToWifi;
          WiFi.begin(connectionConfig.networkSsid, connectionConfig.networkPassword);
          while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(delay_len*100);
            DEBUG_SERIAL.println("Connecting to WiFi..");
          }
          gConnectedToWifi = true;
          IPAddress ipAddress{};
          ipAddress.fromString(connectionConfig.natKitServerAddress);
          mqttClient.setClient(wifiClient);
          mqttClient.setServer(ipAddress, 1883);
          if (!mqttClient.setBufferSize(6200)) {
            currentNetworkingStage = NetworkingStage::Disconnected;
            digitalWrite(13, HIGH);
            break;
          }
          // mqttClient.begin(ipAddress, 1883, wifiClient);
          while (!mqttClient.connect("natKit-IMU")) {
              Serial.print(".");
              DEBUG_SERIAL.println("Connecting to mqtt..");
          }

          static std::string ntpServerAddressString = connectionConfig.natKitServerAddress;
          ntpServerAddressString += ":123";
          ntpServer = ntpServerAddressString.c_str();
          
          
          // configTime(gmtOffset_sec, daylightOffset_sec, ntpServerAddressString.c_str());
          // esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
          // esp_netif_sntp_init(&config);
          //configTime(gmtOffset_sec, daylightOffset_sec, ntpServer.c_str());
          // mqtt_cfg.broker.address.uri = "10.26.0.214";
          // mqttClient = esp_mqtt_client_init(&mqtt_cfg);
          // esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
          // esp_mqtt_client_start(mqttClient);
          DEBUG_SERIAL.print("ESP32 IP on the WiFi network: ");
          DEBUG_SERIAL.println(WiFi.localIP());
          // request->send_P(200, "text/html", formCompletionResponseHtml);
          //uint8_t size;
          //const auto clusterId = KafkaTopic::getKafkaCluster(connectionConfig, size);
          DEBUG_SERIAL.println("AAAAAAAAA");
          // vTaskDelay(500);
          // if (size > 0) {
            // DEBUG_SERIAL.printf("HI %s\n", clusterId);
            const String boardId{UNIQUE_ID};
            DEBUG_SERIAL.println("BBBBBBBBB");

            // kafkaTopic = KafkaTopic::create(esp_random(), boardId);
            kafkaTopic = KafkaTopic::create(UNIQUE_ID, boardId);
            DEBUG_SERIAL.println("CCCCCCCCC");
            //kafkaTopic->createKafkaStream(connectionConfig);
            vTaskDelay(500);
            DEBUG_SERIAL.println("DDDDDDDDD");
            kafkaTopic->writeMetaRecord(connectionConfig, mqttClient);
            DEBUG_SERIAL.println("EEEEEEEE");

            vTaskDelay(1000 / portTICK_PERIOD_MS);
            currentNetworkingStage = NetworkingStage::WriteData;
            next_expected_reading_timestamp = (((getTime() + delay_len_us) / 1000000) + 1) * 1000000; // The start of the next second
          // }
        } else {
          DEBUG_SERIAL.println("Error: Either the network SSID or the network password was not set");
          // request->send_P(400, "text/html", formCompletionResponseHtml);
        }
        break;

      case NetworkingStage::WriteData:
        
        if (connectionConfig.networkSsid != nullptr && connectionConfig.networkPassword != nullptr) {
          current_timestamp = getTime();
          if (current_timestamp > next_expected_reading_timestamp) {
            DEBUG_SERIAL.println("Taking reading late!");
            digitalWrite(13, HIGH);
          } else {
            while(current_timestamp < (next_expected_reading_timestamp - half_delay_len_us)) {
              //mqttClient.loop();
              DEBUG_SERIAL.printf("%lld < %lld\n", current_timestamp, next_expected_reading_timestamp);
              current_timestamp = getTime();
            }
            digitalWrite(12, HIGH);
            while(current_timestamp < next_expected_reading_timestamp) {
              current_timestamp = getTime();
            }
            digitalWrite(12, LOW);
            digitalWrite(13, LOW);
          }
          next_expected_reading_timestamp += delay_len_us;
          imuReader.update();
          imuReader.getImuData(&imuData);
          // DEBUG_SERIAL.println("Before");
          bool isBulkReady = kafkaTopic->writeDataRecord(connectionConfig, imuData, mqttClient);
          // DEBUG_SERIAL.println("After");
          if (isBulkReady) {
            BaseType_t result = xTaskCreate(sendMessageTask, "sendMessageTask", 16384 + 2048, NULL, tskIDLE_PRIORITY+2, &sendMessageTaskHandle);
            if (result != pdPASS) {
              DEBUG_SERIAL.println("Failed to create the send message task!");
            } else {
              DEBUG_SERIAL.println("--------------------------------------------- Created Task");
            }
            //DEBUG_SERIAL.println("Got signal indicating that bulk message is ready to send");
            //sendMessageTask(NULL);
          }
        }
        break;

    default:
        break;
    }
    // digitalWrite(13, LOW);
    // current_timestamp = getTimeNowAsUs();
    // task_delta = (next_expected_reading_timestamp - current_timestamp);
    // if (current_timestamp > next_expected_reading_timestamp) {
    //   digitalWrite(13, HIGH);
    // } else {
    //   while(current_timestamp < (next_expected_reading_timestamp - half_delay_len_us)) {
    //     mqttClient.loop();
    //     current_timestamp = getTimeNowAsUs();
    //   }
    //   digitalWrite(12, HIGH);
    //   while(current_timestamp < next_expected_reading_timestamp) {
    //     current_timestamp = getTimeNowAsUs();
    //   }
    //   digitalWrite(12, LOW);
    //   do {
    //     current_timestamp = esp_timer_get_time();
    //     task_delta = (current_timestamp - last_timestamp) / 1000;
    //   } while (task_delta < (delay_len / 2));
    //   digitalWrite(12, HIGH);
    //   do {
    //     current_timestamp = esp_timer_get_time();
    //     task_delta = (current_timestamp - last_timestamp) / 1000;
    //   } while (task_delta < delay_len);
    // }
    // digitalWrite(12, LOW);
    // if (task_delta >= delay_len) {
    //   // Serial.println("LAGGING!");
    //   log_i("Lagging!\n");
    //   vTaskDelay(0);
    //   continue;
    // }
    // vTaskDelay(delay_len - task_delta);
  }

  //vTaskDelete( NULL );
}

void handleNtpTask(void*) {
  const auto delay = (1000 / portTICK_PERIOD_MS) * 60 * 5; // 5m
  sntp_set_time_sync_notification_cb(ntpSyncNotification);
  while(true) {
    if (gConnectedToWifi) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    }
    vTaskDelay(delay);
  }

  vTaskDelete( NULL );
}

void handleUpdateImuTask(void*) {
  const auto delayUs = reportIntervalUs;
  // const auto delay = (reportIntervalUs / 1000) / portTICK_PERIOD_MS; // 1s
  while(true) {
    const auto startTimeUs = esp_timer_get_time();
    imuReader.update();
    const auto endTimeUs = esp_timer_get_time();

    const auto delta = endTimeUs - startTimeUs;
    const auto delay = (std::max(0LL, delayUs - delta) / 1000) / portTICK_PERIOD_MS;
    vTaskDelay(delay);
  }

  vTaskDelete( NULL );
}

void WiFiEvent(WiFiEvent_t event)
{
   log_i( "[WiFi-event] event: %d\n", event );
  switch (event) {
        case SYSTEM_EVENT_WIFI_READY:
          log_i("WiFi interface ready");
          break;
        case SYSTEM_EVENT_SCAN_DONE:
          log_i("Completed scan for access points");
          break;
        case SYSTEM_EVENT_STA_START:
          log_i("WiFi client started");
          break;
        case SYSTEM_EVENT_STA_STOP:
          log_i("WiFi clients stopped");
          break;
    case SYSTEM_EVENT_STA_CONNECTED:
      log_i("Connected to access point");
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      log_i("Disconnected from WiFi access point");
      break;
        case SYSTEM_EVENT_STA_AUTHMODE_CHANGE:
          log_i("Authentication mode of access point has changed");
          break;
        case SYSTEM_EVENT_STA_GOT_IP:
          log_i ("Obtained IP address: %s",  WiFi.localIP() );
          break;
        case SYSTEM_EVENT_STA_LOST_IP:
          log_i("Lost IP address and IP address is reset to 0");
          //      vTaskDelay( 5000 );
          //      ESP.restart();
          break;
        case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
          log_i("WiFi Protected Setup (WPS): succeeded in enrollee mode");
          break;
        case SYSTEM_EVENT_STA_WPS_ER_FAILED:
          log_i("WiFi Protected Setup (WPS): failed in enrollee mode");
          //      ESP.restart();
          break;
        case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
          log_i("WiFi Protected Setup (WPS): timeout in enrollee mode");
          break;
        case SYSTEM_EVENT_STA_WPS_ER_PIN:
          log_i("WiFi Protected Setup (WPS): pin code in enrollee mode");
          break;
        case SYSTEM_EVENT_AP_START:
          log_i("WiFi access point started");
          break;
        case SYSTEM_EVENT_AP_STOP:
          log_i("WiFi access point  stopped");
          //      WiFi.mode( WIFI_OFF);
          //      esp_sleep_enable_timer_wakeup( 1000000 * 2 ); // 1 second times how many seconds wanted
          //      esp_deep_sleep_start();
          break;
        case SYSTEM_EVENT_AP_STACONNECTED:
          log_i("Client connected");
          break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
      log_i("WiFi client disconnected");
          break;
        case SYSTEM_EVENT_AP_STAIPASSIGNED:
          log_i("Assigned IP address to client");
          break;
        case SYSTEM_EVENT_AP_PROBEREQRECVED:
          log_i("Received probe request");
          break;
        case SYSTEM_EVENT_GOT_IP6:
          log_i("IPv6 is preferred");
          break;
        case SYSTEM_EVENT_ETH_GOT_IP:
          log_i("Obtained IP address");
          break;
    default: break;
  }
}

void setup(){ //the order of the code is important and it is critical the the android workaround is after the dns and sofAP setup
  pinMode(2, OUTPUT);
  pinMode(12, OUTPUT);
  pinMode(13, OUTPUT);
  pinMode(27, OUTPUT);
  digitalWrite(2, HIGH);
  mqttClient.setBufferSize(16384);
  WiFi.onEvent(WiFiEvent);

  esp_efuse_mac_get_default(MAC_ADDRESS);
  UNIQUE_ID = (static_cast<uint64_t>(MAC_ADDRESS[0]) << 8*5) +
              (static_cast<uint64_t>(MAC_ADDRESS[1]) << 8*4) +
              (static_cast<uint64_t>(MAC_ADDRESS[2]) << 8*3) +
              (static_cast<uint64_t>(MAC_ADDRESS[3]) << 8*2) +
              (static_cast<uint64_t>(MAC_ADDRESS[4]) << 8*1) +
              (MAC_ADDRESS[5]);
  #if USE_SERIAL == true
  Serial.begin(115200);
  while (!Serial);
  Serial.println("\n\nCaptive Test, V0.4.0 compiled " __DATE__ " " __TIME__ " by CD_FER"); //__DATE__ is provided by the platformio ide
  Serial.printf("Unique ID is: %u\n", UNIQUE_ID);
  #endif

  sprintf(ssid, "natKit-ESP32-%lld", UNIQUE_ID);

  if (!IMU_DUMMY_DATA) {
    DEBUG_SERIAL.println("Starting IMU Reader");
    imuReader.start();
    DEBUG_SERIAL.println("Finished starting IMU Reader");
  }

  WiFi.mode(WIFI_AP_STA); //access point mode
  WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
  WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, MAX_CLIENTS);

  dnsServer.setTTL(300); //set 5min client side cache for DNS
  dnsServer.start(53, "*", localIP); //if DNSServer is started with "*" for domain name, it will reply with provided IP to all DNS request

  //ampdu_rx_disable android workaround see https://github.com/espressif/arduino-esp32/issues/4423
  esp_wifi_stop();
  esp_wifi_deinit();

  wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();   //We use the default config ...
  my_config.ampdu_rx_enable = false;                           //... and modify only what we want.

  esp_wifi_init(&my_config); //set the new config
  esp_wifi_start(); //Restart WiFi
  delay(100); //this is necessary don't ask me why

	//Required
	server.on("/connecttest.txt",[](AsyncWebServerRequest *request){request->redirect("http://logout.net");}); //windows 11 captive portal workaround
	server.on("/wpad.dat",[](AsyncWebServerRequest *request){request->send(404);}); //Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)

	//Background responses: Probably not all are Required, but some are. Others might speed things up?
	//A Tier (commonly used by modern systems)
	server.on("/generate_204",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); // android captive portal redirect
	server.on("/redirect",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //microsoft redirect
	server.on("/hotspot-detect.html",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //apple call home
	server.on("/canonical.html",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //firefox captive portal call home
	server.on("/success.txt",[](AsyncWebServerRequest *request){request->send(200);}); //firefox captive portal call home
	server.on("/ncsi.txt",[](AsyncWebServerRequest *request){request->redirect(localIPURL);}); //windows call home

	//B Tier (uncommon)
	// server.on("/chrome-variations/seed",[](AsyncWebServerRequest *request){request->send(200);}); //chrome captive portal call home
	// server.on("/service/update2/json",[](AsyncWebServerRequest *request){request->send(200);}); //firefox?
	// server.on("/chat",[](AsyncWebServerRequest *request){request->send(404);}); //No stop asking Whatsapp, there is no internet connection
	// server.on("/startpage",[](AsyncWebServerRequest *request){request->redirect(localIPURL);});


  //return 404 to webpage icon
  server.on("/favicon.ico",[](AsyncWebServerRequest *request){request->send(404);}); //webpage icon

  //Serve Basic HTML Page
  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", indexHtml);
    response->addHeader("Cache-Control", "public,max-age=31536000"); //save this file to cache for 1 year (unless you refresh)
    request->send(response);
    DEBUG_SERIAL.println("Served Basic HTML Page");
  });

  // Respond to form
  server.on("/action", HTTP_ANY, [](AsyncWebServerRequest *request) {
      DEBUG_SERIAL.println("Rescieved Form Response");

      int params = request->params();
      for (int i = 0; i < params; ++i) {
        connectionConfig.configureSetting(request->getParam(i));
        //DEBUG_SERIAL.printf("POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
      }
      request->send_P(200, "text/html", formCompletionResponseHtml);
      currentNetworkingStage = NetworkingStage::RecievedWifiCredentials;
    }
  );

  //the catch all
  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect(localIPURL);
    DEBUG_SERIAL.print("onNotFound ");
    DEBUG_SERIAL.print(request->host());       //This gives some insight into whatever was being requested on the serial monitor
    DEBUG_SERIAL.print(" ");
    DEBUG_SERIAL.print(request->url());
    DEBUG_SERIAL.print(" sent redirect to " + localIPURL +"\n");
  });

  server.begin();

  DEBUG_SERIAL.print("\n");
  DEBUG_SERIAL.print("Startup Time:"); //should be somewhere between 270-350 for Generic ESP32 (D0WDQ6 chip, can have a higher startup time on first boot)
  DEBUG_SERIAL.println(millis());
  DEBUG_SERIAL.print("\n");

  TaskHandle_t handleApRequestsTaskHandle = NULL;
  xTaskCreate(handleApRequestsTask, "HandleApRequestsTask", 16384, NULL, tskIDLE_PRIORITY, &handleApRequestsTaskHandle);

  // TaskHandle_t handleNetworkingStagesTaskHandle = NULL;
  // xTaskCreate(handleNetworkingStagesTask, "HandleNetworkingStagesTask", 16384, NULL, tskIDLE_PRIORITY+1, &handleNetworkingStagesTaskHandle);

  // TaskHandle_t handleImuUpdateTaskHandle = NULL;
  // xTaskCreate(handleImuUpdateTask, "HandleImuUpdateTask", 20384, NULL, tskIDLE_PRIORITY+2, &handleImuUpdateTaskHandle);

  //TaskHandle_t handleNetworkingStagesAndImuJoinedTaskHandle = NULL;
  //xTaskCreatePinnedToCore(handleNetworkingStagesAndImuJoinedTask, "handleNetworkingStagesAndImuJoinedTask", 20384, NULL, tskIDLE_PRIORITY+2, &handleNetworkingStagesAndImuJoinedTaskHandle, 1);

  TaskHandle_t handleNtpTaskHandle = NULL;
  xTaskCreate(handleNtpTask, "handleNtpTask", 2048, NULL, tskIDLE_PRIORITY+1, &handleNtpTaskHandle);

  

  // TaskHandle_t handleUpdateImuTaskHandle = NULL;
  // xTaskCreate(handleUpdateImuTask, "handleUpdateImuTask", 2048, NULL, tskIDLE_PRIORITY+1, &handleNtpTaskHandle);

}

void loop(){
  //vTaskDelay(500);
  handleNetworkingStagesAndImuJoinedTask(nullptr);
}

// #include <WiFi.h>
// #include <ESP32Ping.h>

// #include "time.h"

// unsigned long epochTime; 

// unsigned long getTime() {
//   time_t now;
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     return 0;
//   }
//   time(&now);
//   return now;
// }
 
// const char* ssid = "selk";
// const char* password =  "filberts";
// unsigned long long start, end;
// int count = 0;
 
// void setup() {
//   Serial.begin(115200);
 
//   WiFi.begin(ssid, password);
   
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.println("Connecting to WiFi...");
//   }

//   Serial.println("Connected");

//   while (count < 100) {
//     start = esp_timer_get_time();
//     bool success = Ping.ping("10.26.0.214", 1);
//     end = esp_timer_get_time();
//     double diff = end - start;

//     if(!success){
//       Serial.println("Ping failed");
//       return;
//     }
  
//     Serial.printf("64 bytes from 10.26.0.214: icmp_seq=%i ttl=64 time=%.3f ms\n", count, Ping.averageTime());
  
//     count += 1;
//   }
// }
 
// void loop() {
  
// }