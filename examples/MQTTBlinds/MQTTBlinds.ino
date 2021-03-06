/**
 * A proxy that allows you to control AM43 style blind controllers via MQTT.
 * 
 * It will scan for and auto-connect to any AM43 devices in range, then provide
 * MQTT topics to control and get status from them.
 *
 * The following MQTT topis are published to:
 *
 * - am43/<device>/available - Either 'offline' or 'online'
 * - am43/<device>/position  - The current blind position, between 0 and 100
 * - am43/<device>/battery   - The current battery level, between 0 and 100
 * - am43/<device>rssi       - The current RSSI reported by the device.
 * - am43/LWT                - Either 'Online' or 'Offline', MQTT status of this service.
 *
 * The following MQTT topics are subscribed to:
 *
 * - am43/<device>/set          - Set the blind to 'OPEN', 'STOP' or 'CLOSE'
 * - am43/<device>/set_position - Set the blind position, between 0 and 100.
 * - am43/restart               - Reboot this service.
 *
 * <device> is the bluetooth mac address of the device, eg 02:69:32:f0:c5:1d
 *
 * For the position set commands, you can use name 'all' to change all devices.
 * 
 * Arduino OTA update is supported.
 */

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <PubSubClient.h>
#include <AM43Client.h>
#include <BLEDevice.h>
#include "config.h"

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
const char *mqtt_server = MQTT_ADDRESS;
const uint16_t am43Pin = AM43_PIN;

WiFiClient espClient;
PubSubClient pubSubClient(espClient);

FreeRTOS::Semaphore clientListSem = FreeRTOS::Semaphore("clients");

void mqtt_callback(char* top, byte* pay, unsigned int length);

unsigned long lastScan = 0;
boolean scanning = false;

class MyAM43Callbacks: public AM43Callbacks {
  public:
    AM43Client *client;
    WiFiClient wifiClient;
    PubSubClient *mqtt;
    unsigned long nextMqttAttempt;

    BLEAddress rmtAddress() {
      return client->m_Device->getAddress();
    }

    String topic(char *t) {
      char top[64];
#ifdef AM43_USE_NAME_FOR_TOPIC
      sprintf(top, "%s/%s/%s", MQTT_TOPIC_PREFIX, client->m_Name.c_str(), t);
#else
      sprintf(top, "%s/%s/%s", MQTT_TOPIC_PREFIX, client->m_Device->getAddress().toString().c_str(), t);
#endif
      String ret = String(top);
      //ret.replace(":", "");
      return ret;
    }

    void onPosition(uint8_t pos) {
      Serial.printf("[%s] Got position: %d\r\n", rmtAddress().toString().c_str(), pos);
      this->mqtt->publish(topic("position").c_str(), String(pos).c_str(), false);
    }
    void onBatteryLevel(uint8_t level) {
      Serial.printf("[%s] Got battery: %d\r\n", rmtAddress().toString().c_str(), level);
      this->mqtt->publish(topic("battery").c_str(), String(level).c_str(), false);
    }
    void onLightLevel(uint8_t level) {
      Serial.printf("[%s] Got light: %d\r\n", rmtAddress().toString().c_str(), level);
      this->mqtt->publish(topic("light").c_str(), String(level).c_str(), false);
    }
    void onConnect(AM43Client *c) {
      Serial.printf("[%s] Connected\r\n", rmtAddress().toString().c_str());
      this->mqtt = new PubSubClient(this->wifiClient);
      this->nextMqttAttempt = 0;
      this->mqtt->setServer(mqtt_server, 1883);
      this->mqtt->setCallback(mqtt_callback);
      lastScan = millis()-60000; // Trigger a new scan after connection.
    }
    void onDisconnect(AM43Client *c) {
      Serial.printf("[%s] Disconnected\r\n", rmtAddress().toString().c_str());
      if (this->mqtt != nullptr && this->mqtt->connected()) {
        // Publish offline availability as LWT is only for ungraceful disconnect.
        this->mqtt->publish(topic("available").c_str(), "offline", true);
        this->mqtt->loop();
        this->mqtt->disconnect();
      }
      this->mqtt = nullptr;
    }

    void handle() {
      if (this->mqtt == nullptr) return;
      if (this->mqtt->connected()) {
        this->mqtt->loop();
        return;
      }
      if (WiFi.status() != WL_CONNECTED || millis() < this->nextMqttAttempt) return;
      if (!this->mqtt->connect(topic("").c_str(), MQTT_USERNAME, MQTT_PASSWORD, topic("available").c_str(), 0, false, "offline")) {
        Serial.print("MQTT connect failed, rc=");
        Serial.print(this->mqtt->state());
        Serial.println(" retrying in 5s.");
        this->nextMqttAttempt = millis() + 5000;
        return;
      }
      this->mqtt->publish(topic("available").c_str(), "online", true);
      this->mqtt->subscribe(topic("set").c_str());
      this->mqtt->subscribe(topic("set_position").c_str());
      this->mqtt->loop();
    }
};

std::map<std::string, MyAM43Callbacks*> allClients;

std::map<std::string, MyAM43Callbacks*> getClients() {
  std::map<std::string, MyAM43Callbacks*> cls;
  clientListSem.take("clientsAll");
  for (auto const& c : allClients)
    cls.insert({c.first, c.second});
  clientListSem.give();
  return cls;
}


static void notifyCallback(BLERemoteCharacteristic* rChar, uint8_t* pData, size_t length, bool isNotify) {
  auto cls = getClients();
  for (auto const& c : cls) {
    if (c.second->client->m_Char == rChar) {
      c.second->client->myNotifyCallback(rChar, pData, length, isNotify);
      return;
    }
  }
}


void mqtt_callback(char* top, byte* pay, unsigned int length) {
  pay[length] = '\0';
  String payload = String((char *)pay);
  String topic = String(top);
  Serial.printf("MQTT [%s]%d: %s\r\n", top, length, payload.c_str());

  int i1, i2, i3;
  i1 = topic.indexOf('/');
  i2 = topic.indexOf('/', i1+1);
  String address = topic.substring(i1+1, i2);
  String command = topic.substring(i2+1);
  Serial.printf("Addr: %s Cmd: %s\r\n", address.c_str(), command.c_str());

  auto cls = getClients();
  if (address == "restart") {
    for (auto const& c : cls)
      c.second->onDisconnect(c.second->client);
    delay(200);
    ESP.restart();
  }

  for (auto const& c : cls) {
    auto cl = c.second->client;
#ifdef AM43_USE_NAME_FOR_TOPIC
    if (String(cl->m_Name.c_str()) == address || address == "all") {
#else
    if (String(cl->m_Device->getAddress().toString().c_str()) == address || address == "all") {
#endif
      if (command == "set") {
        payload.toLowerCase();
        if (payload == "open") cl->open();
        if (payload == "close") cl->close();
        if (payload == "stop") cl->stop();
      }
      if (command == "set_position") {
        cl->setPosition(payload.toInt());
      }
    }
  }
}

std::vector<BLEAddress> allowList;

void parseAllowList() {
  std::string allowListStr = std::string(DEVICE_ALLOWLIST);
  if (allowListStr.length() == 0) return;
  size_t idx1 = 0;

  for(;;) {
    auto idx = allowListStr.find(',', idx1);
    if (idx == std::string::npos) break;
    allowList.push_back(BLEAddress(allowListStr.substr(idx1, idx-idx1)));
    idx1 = idx+1;
  }
  allowList.push_back(BLEAddress(allowListStr.substr(idx1)));
  Serial.println("AllowList contains the following device(s):");
  for (auto dev : allowList)
    Serial.printf(" Mac: %s \n", dev.toString().c_str());
}

bool isAllowed(BLEAddress address) {
  if (allowList.size() < 1) return true;
  for (auto a : allowList) {
    if (a.equals(address)) return true;
  }
  return false;
}

/**
 * Scan for BLE servers and find any that advertise the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
 /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.printf("BLE Advertised Device found: %s\r\n", advertisedDevice.toString().c_str());

    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      auto cls = getClients();
      for (auto const& c : cls) {
        if (!c.first.compare(advertisedDevice.toString())) {
          Serial.printf("Ignoring advertising device %s, already present\r\n", advertisedDevice.toString());
          return;
        }
      }
      if (!isAllowed(advertisedDevice.getAddress())) {
        Serial.printf("Ignoring device %s, not in allow list", advertisedDevice.toString());
        return;
      }
      AM43Client* newClient = new AM43Client(new BLEAdvertisedDevice(advertisedDevice), am43Pin);
      newClient->m_DoConnect = true;
      newClient->m_Name = advertisedDevice.getName();
      MyAM43Callbacks *cbs = new MyAM43Callbacks();
      cbs->client = newClient;
      newClient->setClientCallbacks(cbs);
      clientListSem.take("clientInsert");
      allClients.insert({advertisedDevice.toString(), cbs});
      clientListSem.give();
      BLEDevice::getScan()->stop();
      scanning = false;
    } // Found our server
  } // onResult
}; // MyAdvertisedDeviceCallbacks

void bleScanComplete(BLEScanResults r) {
  Serial.println("BLE scan complete.");
  scanning = false;
};

void initBLEScan() {
  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
    scanning = true;
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setInterval(500);
    pBLEScan->setWindow(50);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(10, bleScanComplete, false);
  
}

unsigned int wifiDownSince = 0;

void WiFiEvent(WiFiEvent_t event) {
  Serial.printf("[WiFi-event] event: %d\r\n", event);

  switch(event) {
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println("WiFi connected");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      wifiDownSince = millis();
      break;
  }
}

void setup_wifi() {
  WiFi.disconnect(true);
  delay(1000);
  Serial.printf("Wifi connecting to: %s ... \r\n", ssid);
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  wifiDownSince = 0;
}

unsigned long nextMqttAttempt = 0;

void reconnect_mqtt() {
  if (WiFi.status() == WL_CONNECTED && millis() > nextMqttAttempt) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (pubSubClient.connect("am43-gateway", MQTT_USERNAME, MQTT_PASSWORD, "am43/LWT", 0, false, "Offline")) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      pubSubClient.publish("am43/LWT", "Online", true);
      pubSubClient.subscribe("am43/restart");
      pubSubClient.subscribe("am43/all/set");
      pubSubClient.subscribe("am43/all/set_position");
      pubSubClient.loop();
    } else {
      Serial.print("failed, rc=");
      Serial.print(pubSubClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      nextMqttAttempt = millis() + 5000;
    }
  }
}

bool otaUpdating = false;

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Arduino BLE Client application...");
  setup_wifi();
  pubSubClient.setServer(mqtt_server, 1883);
  pubSubClient.setCallback(mqtt_callback);

  parseAllowList();
  
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
      // Stop any active BLEScan during OTA - improves stability.
      otaUpdating = true;
      BLEDevice::getScan()->stop();
    })
    .onEnd([]() {
      Serial.println("\nEnd");
      pubSubClient.disconnect();
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
      otaUpdating = false;
    });

  otaUpdating = false;
  ArduinoOTA.begin();
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  BLEDevice::init("");
  initBLEScan();
} // End of setup.

unsigned long lastAM43update = 0;

// This is the Arduino main loop function.
void loop() {

  if (WiFi.status() != WL_CONNECTED && wifiDownSince > 0 && millis()-wifiDownSince > 20000) {
    setup_wifi();
  }
  if (!pubSubClient.connected()) {
    reconnect_mqtt();
  }
  pubSubClient.loop();
  
  if (millis()-lastAM43update > 500) { // Only process this every 500ms.
    std::vector<std::string> removeList; // Clients will be added to this as they are disconnected.
    auto cls = getClients();
    // Iterate through connected devices, perform any connect/update/etc actions.
    for (auto const &c : cls) {
      if (c.second->client->m_DoConnect && !scanning) {
        c.second->client->connectToServer(notifyCallback);
        break;  // Connect takes some time, so break out to allow other processing.
      }
      if (c.second->client->m_Connected) {
        c.second->client->update();
        c.second->handle();
      }
      if (c.second->client->m_Disconnected) removeList.push_back(c.first);
    }
    // Remove any clients that have been disconnected.
    for (auto i : removeList) {
      clientListSem.take("clientRemove");
      allClients.erase(i);
      clientListSem.give();
    }

    lastAM43update = millis();
  }
  // Start a new scan every 60s.
  if (millis() - lastScan > 60000 && !otaUpdating) {
    initBLEScan();
    lastScan = millis();
  }

  ArduinoOTA.handle();

} // End of loop
