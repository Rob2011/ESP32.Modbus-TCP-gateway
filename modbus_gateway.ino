/*
 * ESP32 Modbus RTU to TCP/IP Gateway with Web Portal and REST API
 * Enhanced Version 5.5 - Fixed configuration saving
 * By Rob Hagemann - Dec. 2025
 * Fixed: WiFi credentials not saving, config validation, and memory management
 */

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ModbusRTU.h>
#include <ModbusTCP.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// ########## CONFIGURATION DEFINES ##########
#define VERSION "5.5"
#define MAX_CLIENTS 3
#define MAX_REGISTERS 122
#define DNS_PORT 53
#define MODBUS_BAUD_DEFAULT 9600
#define RX_PIN_DEFAULT 16
#define TX_PIN_DEFAULT 17
#define GROWATT_BAUD 115200
#define POLL_INTERVAL 1000
#define WIFI_RECONNECT_INTERVAL 30000
#define WIFI_CONNECT_TIMEOUT 20000
#define MAX_MODBUS_RETRIES 3
#define MODBUS_READ_TIMEOUT 2000
#define HOSTNAME_DEFAULT "modbus2"
#define HOSTNAME_MAX_LEN 32
#define BAUD_RATE_MIN 1200
#define BAUD_RATE_MAX 115200
#define REGISTER_COUNT_MIN 1
#define REGISTER_COUNT_MAX 122
#define POLL_INTERVAL_MIN 100
#define POLL_INTERVAL_MAX 60000
#define CLIENT_ID_MIN 1
#define CLIENT_ID_MAX 247
#define TASK_STACK_SIZE 16000
#define TASK_TIMEOUT 5000
#define WATCHDOG_TIMEOUT 30000
#define AP_RECONNECT_INTERVAL 60000
#define MAX_RECONNECT_ATTEMPTS 3

// ########## DEVICE PROFILE SYSTEM ##########
enum DeviceType {
  DEVICE_GENERIC = 0,
  DEVICE_GROWATT_INVERTER = 1,
  DEVICE_EASTRON_SDM630 = 2,
  DEVICE_SOLAREDGE_INVERTER = 3,
  DEVICE_SMA_INVERTER = 4,
  DEVICE_FRONIUS_INVERTER = 5,
  DEVICE_HUAWEI_INVERTER = 6
};

enum ModbusFunction {
  FUNC_READ_COILS = 1,
  FUNC_READ_DISCRETE = 2,
  FUNC_READ_HOLDING = 3,
  FUNC_READ_INPUT = 4
};

enum DataType {
  TYPE_UINT16,
  TYPE_INT16,
  TYPE_UINT32,
  TYPE_INT32,
  TYPE_FLOAT32
};

struct RegisterPreset {
  uint16_t startAddr;
  uint16_t count;
  String description;
};

struct DeviceProfile {
  String name;
  ModbusFunction readFunction;
  bool useFloatDecoding;
  bool swapBytes;
  bool swapWords;
  uint16_t recommendedPollInterval;
  String (*getRegDescription)(uint16_t);
  const RegisterPreset* presets;
  int presetCount;
};

struct clientConfig {
  uint8_t id;
  bool enabled;
  uint16_t startAddress;
  uint16_t registerCount;
  uint16_t pollInterval;
  unsigned long lastPoll;
  uint16_t errorCount;
  uint16_t successCount;
  DeviceType deviceType;
};

// ########## GLOBAL VARIABLES ##########
uint16_t holdingRegs[MAX_CLIENTS][MAX_REGISTERS];
uint16_t inputRegs[MAX_CLIENTS][MAX_REGISTERS];

unsigned long lastWifiCheck = 0;
unsigned long tcpRequests = 0;
unsigned long rtuRequests = 0;
unsigned long errors = 0;
unsigned long systemErrors = 0;
unsigned long wifiReconnects = 0;
unsigned long lastWatchdog = 0;
unsigned long lastAPReconnectAttempt = 0;
int reconnectAttempts = 0;

TaskHandle_t modbusTaskHandle = NULL;
TaskHandle_t webTaskHandle = NULL;
SemaphoreHandle_t dataMutex = NULL;
SemaphoreHandle_t configMutex = NULL;

// Register Presets
const RegisterPreset growattPresets[] = {
  { 0, 120, "Complete Status (0-119)" },
  { 1, 10, "PV Input Data (1-10)" },
  { 35, 20, "AC Output Data (35-54)" },
  { 53, 4, "Energy Counters (53-56)" }
};

const RegisterPreset eastronPresets[] = {
  { 0, 80, "All Phase Data (0-79)" },
  { 0, 42, "Voltage & Current (0-41)" },
  { 52, 30, "Power & Energy (52-81)" }
};

const RegisterPreset solarEdgePresets[] = {
  { 40000, 50, "Inverter Status & Power" },
  { 40069, 40, "AC Measurements" },
  { 40100, 20, "DC Measurements" }
};

const RegisterPreset smaPresets[] = {
  { 30051, 2, "Total Power" },
  { 30775, 2, "Daily Yield" },
  { 30529, 2, "Total Yield" }
};

const RegisterPreset froniusPresets[] = {
  { 40001, 20, "Common Model Block" },
  { 40069, 50, "Inverter Model Block" }
};

const RegisterPreset huaweiPresets[] = {
  { 30000, 50, "Device Information" },
  { 32000, 40, "Running Status" },
  { 32016, 30, "Grid Measurements" }
};

// Configuration
String wifi_ssid = "";
String wifi_password = "";
String hostname = HOSTNAME_DEFAULT;
int modbus_baud = MODBUS_BAUD_DEFAULT;
int rx_pin = RX_PIN_DEFAULT;
int tx_pin = TX_PIN_DEFAULT;
const char* version = VERSION;

// Access Point Configuration
const char* ap_ssid = "ModbusGateway-Config";
const char* ap_password = "modbus123";
bool apMode = false;
DNSServer dnsServer;

#define MODBUS_RS485_SERIAL Serial2

// Forward declarations for register descriptions
String getGenericRegDescription(uint16_t regAddr);
String getGrowattRegDescription(uint16_t regAddr);
String getSDM630Description(uint16_t regAddr);
String getSolarEdgeRegDescription(uint16_t regAddr);
String getSMARegDescription(uint16_t regAddr);
String getFroniusRegDescription(uint16_t regAddr);
String getHuaweiRegDescription(uint16_t regAddr);

// Additional forward declarations
void loadConfig();
void saveConfig();
bool validateConfig();
bool validateBaudRate(int baud);
bool validateHostname(String host);
bool validateRegisterCount(int count);
void setupWebServer();
void startAPMode();
void handleCaptivePortal();
void handleWiFiSave();
void setupRESTAPI();
bool connectWiFi();
void modbusTask(void* parameter);
void webTask(void* parameter);
String getHTMLHeader(String title);
String getHTMLFooter();
String getPresetsJSON();
void handleClientData(int clientIndex);
void handleDevicePresets(int deviceId);

// Device Profile Registry
const DeviceProfile deviceProfiles[] = {
  { "Generic Device", FUNC_READ_INPUT, true, false, false, 1000, getGenericRegDescription, nullptr, 0 },
  { "Growatt Inverter", FUNC_READ_INPUT, true, true, true, 1000, getGrowattRegDescription, growattPresets, 4 },
  { "Eastron SDM630", FUNC_READ_INPUT, true, false, true, 1000, getSDM630Description, eastronPresets, 3 },
  { "SolarEdge Inverter", FUNC_READ_HOLDING, true, true, false, 1000, getSolarEdgeRegDescription, solarEdgePresets, 3 },
  { "SMA Inverter", FUNC_READ_HOLDING, true, true, true, 1000, getSMARegDescription, smaPresets, 3 },
  { "Fronius Inverter", FUNC_READ_HOLDING, true, false, false, 1000, getFroniusRegDescription, froniusPresets, 2 },
  { "Huawei Inverter", FUNC_READ_HOLDING, true, true, false, 1000, getHuaweiRegDescription, huaweiPresets, 3 }
};

const int DEVICE_PROFILE_COUNT = sizeof(deviceProfiles) / sizeof(DeviceProfile);

clientConfig clients[MAX_CLIENTS];
int activeclients = 0;

WebServer server(80);
ModbusTCP mbTCP;
ModbusRTU mbRTU;
Preferences prefs;

// ########## SETUP ##########
void setup() {
  Serial.begin(GROWATT_BAUD);
  delay(1000);
  Serial.println("\n\n=== ESP32 Modbus Gateway v" + String(version) + " ===");

  // Create mutexes
  dataMutex = xSemaphoreCreateMutex();
  configMutex = xSemaphoreCreateMutex();

  if (!dataMutex || !configMutex) {
    Serial.println("ERROR: Failed to create semaphores!");
    delay(1000);
    ESP.restart();
  }

  // Load and validate configuration
  loadConfig();
  if (!validateConfig()) {
    Serial.println("WARNING: Configuration validation failed. Using defaults.");
    systemErrors++;
  }

  // Initialize Modbus Serial
  Serial2.begin(modbus_baud, SERIAL_8N1, rx_pin, tx_pin);
  mbRTU.begin(&Serial2);
  Serial.println("Modbus source: RS-485");
  mbRTU.master();

  // Try to connect to WiFi if credentials exist
  if (wifi_ssid.length() > 0) {
    Serial.println("Attempting to connect to WiFi: " + wifi_ssid);
    if (!connectWiFi()) {
      Serial.println("Failed to connect to WiFi. Starting AP mode...");
      startAPMode();
    }
  } else {
    Serial.println("No WiFi credentials found. Starting AP mode...");
    startAPMode();
  }

  // Initialize Modbus TCP server
  mbTCP.server();
  mbTCP.addHreg(0, 0, 1000);
  mbTCP.addIreg(0, 0, 1000);
  mbTCP.addCoil(0, 0, 1000);
  mbTCP.addIsts(0, 0, 1000);
  Serial.println("Modbus TCP server started on port 502");

  setupWebServer();
  setupRESTAPI();
  server.begin();
  Serial.println("Web server started on port 80");

  // Create tasks
  if (!xTaskCreatePinnedToCore(modbusTask, "Modbus", TASK_STACK_SIZE, NULL, 1, &modbusTaskHandle, 0)) {
    Serial.println("ERROR: Failed to create Modbus task!");
    systemErrors++;
  }

  if (!xTaskCreatePinnedToCore(webTask, "Web", TASK_STACK_SIZE, NULL, 1, &webTaskHandle, 1)) {
    Serial.println("ERROR: Failed to create Web task!");
    systemErrors++;
  }

  Serial.println("=== System Ready ===\n");
  lastWatchdog = millis();
}

// ########## LOOP ##########
void loop() {
  unsigned long now = millis();
  if (now - lastWatchdog > WATCHDOG_TIMEOUT) {
    Serial.println("WARNING: Watchdog timeout - tasks may be hung!");
    systemErrors++;
    lastWatchdog = now;
  }
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

bool connectWiFi() {
  Serial.println("Connecting to WiFi...");
  Serial.println("  SSID: " + wifi_ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname.c_str());
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

  unsigned long startAttempt = millis();
  int dots = 0;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
    dots++;
    if (dots > 40) {
      Serial.println();
      dots = 0;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    reconnectAttempts = 0;
    Serial.println("\nWiFi Connected!");
    Serial.println("  IP: " + WiFi.localIP().toString());

    dnsServer.stop();
    WiFi.softAPdisconnect(true);

    if (!MDNS.begin(hostname.c_str())) {
      Serial.println("WARNING: mDNS initialization failed!");
      systemErrors++;
    } else {
      Serial.println("  mDNS: http://" + hostname + ".local");
    }
    return true;
  }

  Serial.println("\nWiFi Connection Failed!");
  wifiReconnects++;
  reconnectAttempts++;
  return false;
}

float decodeFloat32(uint16_t reg1, uint16_t reg2, bool swapBytes, bool swapWords) {
  uint32_t combined;
  uint8_t* bytes = (uint8_t*)&combined;

  if (swapWords) {
    uint16_t temp = reg1;
    reg1 = reg2;
    reg2 = temp;
  }

  if (swapBytes) {
    bytes[3] = (reg1 >> 8) & 0xFF;
    bytes[2] = reg1 & 0xFF;
    bytes[1] = (reg2 >> 8) & 0xFF;
    bytes[0] = reg2 & 0xFF;
  } else {
    bytes[1] = (reg1 >> 8) & 0xFF;
    bytes[0] = reg1 & 0xFF;
    bytes[3] = (reg2 >> 8) & 0xFF;
    bytes[2] = reg2 & 0xFF;
  }

  float value;
  memcpy(&value, &combined, sizeof(float));
  return value;
}

void modbusTask(void* parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    lastWatchdog = millis();

    mbTCP.task();
    mbRTU.task();

    if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      int clientCount = activeclients;
      xSemaphoreGive(configMutex);

      for (int i = 0; i < clientCount; i++) {
        if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
          continue;
        }

        if (!clients[i].enabled) {
          xSemaphoreGive(configMutex);
          continue;
        }

        uint16_t pollInterval = clients[i].pollInterval;
        unsigned long lastPoll = clients[i].lastPoll;
        uint8_t clientId = clients[i].id;
        uint16_t startAddr = clients[i].startAddress;
        uint16_t regCount = clients[i].registerCount;
        DeviceType deviceType = clients[i].deviceType;

        xSemaphoreGive(configMutex);

        if (millis() - lastPoll < pollInterval) {
          continue;
        }

        const DeviceProfile& profile = deviceProfiles[deviceType];

        bool success = false;
        for (int retry = 0; retry < MAX_MODBUS_RETRIES && !success; retry++) {
          if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(TASK_TIMEOUT)) == pdTRUE) {

            switch (profile.readFunction) {
              case FUNC_READ_HOLDING:
                success = mbRTU.readHreg(clientId, startAddr, holdingRegs[i], regCount, nullptr);
                if (success) {
                  for (int j = 0; j < regCount; j++) {
                    mbTCP.Hreg(startAddr + j, holdingRegs[i][j]);
                  }
                }
                break;

              case FUNC_READ_INPUT:
                success = mbRTU.readIreg(clientId, startAddr, inputRegs[i], regCount, nullptr);
                if (success) {
                  for (int j = 0; j < regCount; j++) {
                    mbTCP.Ireg(startAddr + j, inputRegs[i][j]);
                  }
                }
                break;

              default:
                success = mbRTU.readIreg(clientId, startAddr, inputRegs[i], regCount, nullptr);
                if (success) {
                  for (int j = 0; j < regCount; j++) {
                    mbTCP.Ireg(startAddr + j, inputRegs[i][j]);
                  }
                }
                break;
            }

            xSemaphoreGive(dataMutex);

            if (success) {
              if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                clients[i].successCount++;
                clients[i].lastPoll = millis();
                xSemaphoreGive(configMutex);
              }
              rtuRequests++;
              tcpRequests++;
            } else if (retry < MAX_MODBUS_RETRIES - 1) {
              vTaskDelay(pdMS_TO_TICKS(100));
            } else {
              if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                clients[i].errorCount++;
                clients[i].lastPoll = millis();
                xSemaphoreGive(configMutex);
              }
              errors++;
            }
          } else {
            systemErrors++;
            vTaskDelay(pdMS_TO_TICKS(50));
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void webTask(void* parameter) {
  for (;;) {
    lastWatchdog = millis();

    if (apMode) {
      dnsServer.processNextRequest();

      if (wifi_ssid.length() > 0) {
        unsigned long now = millis();
        unsigned long reconnectDelay = AP_RECONNECT_INTERVAL;

        if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
          reconnectDelay = AP_RECONNECT_INTERVAL * 2;
        }

        if (now - lastAPReconnectAttempt > reconnectDelay) {
          lastAPReconnectAttempt = now;
          Serial.println("AP Mode: Attempting to reconnect to saved WiFi network...");
          Serial.println("  SSID: " + wifi_ssid);
          Serial.println("  Attempt: " + String(reconnectAttempts + 1));

          if (connectWiFi()) {
            Serial.println("Successfully reconnected! Switching from AP to STA mode.");
          } else {
            Serial.println("Reconnection failed. Will try again in " + String(reconnectDelay / 1000) + " seconds.");
            if (!WiFi.softAPgetStationNum()) {
              WiFi.softAP(ap_ssid, ap_password);
              IPAddress IP = WiFi.softAPIP();
              dnsServer.start(DNS_PORT, "*", IP);
            }
          }
        }
      }
    } else {
      if (millis() - lastWifiCheck > WIFI_RECONNECT_INTERVAL) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
          wifiReconnects++;
          Serial.println("WiFi connection lost. Attempting reconnect...");
          if (!connectWiFi()) {
            Serial.println("Reconnection failed. Starting AP mode...");
            startAPMode();
            lastAPReconnectAttempt = millis();
          }
        }
      }
    }

    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startAPMode() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  delay(100);

  IPAddress IP = WiFi.softAPIP();
  Serial.println("AP Mode Started");
  Serial.println("  SSID: " + String(ap_ssid));
  Serial.println("  Password: " + String(ap_password));
  Serial.println("  IP: " + IP.toString());

  if (wifi_ssid.length() > 0) {
    Serial.println("  Will attempt reconnection to '" + wifi_ssid + "' every 60 seconds");
  }

  dnsServer.start(DNS_PORT, "*", IP);
  Serial.println("DNS server started for captive portal");

  lastAPReconnectAttempt = millis();
  reconnectAttempts = 0;
}

// ########## CONFIG VALIDATION ##########
bool validateConfig() {
  bool valid = true;

  if (!validateHostname(hostname)) {
    Serial.println("WARNING: Invalid hostname. Using default.");
    hostname = HOSTNAME_DEFAULT;
    valid = false;
  }

  if (!validateBaudRate(modbus_baud)) {
    Serial.println("WARNING: Invalid baud rate. Using default.");
    modbus_baud = MODBUS_BAUD_DEFAULT;
    valid = false;
  }

  if (activeclients < 0 || activeclients > MAX_CLIENTS) {
    Serial.println("WARNING: Invalid client count. Using default.");
    activeclients = 0;
    valid = false;
  }

  for (int i = 0; i < activeclients; i++) {
    if (clients[i].id < CLIENT_ID_MIN || clients[i].id > CLIENT_ID_MAX) {
      clients[i].id = i + 1;
      valid = false;
    }
    if (!validateRegisterCount(clients[i].registerCount)) {
      clients[i].registerCount = 10;
      valid = false;
    }
    if (clients[i].pollInterval < POLL_INTERVAL_MIN || clients[i].pollInterval > POLL_INTERVAL_MAX) {
      clients[i].pollInterval = 1000;
      valid = false;
    }
    if (clients[i].deviceType < 0 || clients[i].deviceType >= DEVICE_PROFILE_COUNT) {
      clients[i].deviceType = DEVICE_GENERIC;
      valid = false;
    }
  }

  return valid;
}

bool validateHostname(String host) {
  if (host.length() == 0 || host.length() > HOSTNAME_MAX_LEN) return false;
  for (unsigned int i = 0; i < host.length(); i++) {
    char c = host.charAt(i);
    if (!isalnum(c) && c != '-') return false;
  }
  return true;
}

bool validateBaudRate(int baud) {
  return (baud >= BAUD_RATE_MIN && baud <= BAUD_RATE_MAX);
}

bool validateRegisterCount(int count) {
  return (count >= REGISTER_COUNT_MIN && count <= REGISTER_COUNT_MAX);
}

void loadConfig() {
  prefs.begin("modbus-gw", false);

  wifi_ssid = prefs.getString("ssid", "");
  wifi_password = prefs.getString("password", "");
  hostname = prefs.getString("hostname", HOSTNAME_DEFAULT);
  modbus_baud = prefs.getInt("baud", MODBUS_BAUD_DEFAULT);
  rx_pin = prefs.getInt("rx_pin", RX_PIN_DEFAULT);
  tx_pin = prefs.getInt("tx_pin", TX_PIN_DEFAULT);
  activeclients = prefs.getInt("active_clients", 0);

  Serial.println("Loaded configuration:");
  Serial.println("  SSID: " + (wifi_ssid.length() > 0 ? wifi_ssid : "(empty)"));
  Serial.println("  Hostname: " + hostname);
  Serial.println("  Active clients: " + String(activeclients));

  for (int i = 0; i < activeclients && i < MAX_CLIENTS; i++) {
    String prefix = "client" + String(i) + "_";
    clients[i].id = prefs.getUChar((prefix + "id").c_str(), i + 1);
    clients[i].enabled = prefs.getBool((prefix + "en").c_str(), true);
    clients[i].startAddress = prefs.getUShort((prefix + "addr").c_str(), 0);
    clients[i].registerCount = prefs.getUShort((prefix + "count").c_str(), 10);
    clients[i].pollInterval = prefs.getUShort((prefix + "poll").c_str(), 1000);
    clients[i].deviceType = (DeviceType)prefs.getUChar((prefix + "type").c_str(), DEVICE_GENERIC);
    clients[i].lastPoll = 0;
    clients[i].errorCount = 0;
    clients[i].successCount = 0;
  }

  if (activeclients == 0) {
    Serial.println("No clients configured. Creating default client.");
    clients[0].id = 1;
    clients[0].enabled = true;
    clients[0].startAddress = 0;
    clients[0].registerCount = 10;
    clients[0].pollInterval = 1000;
    clients[0].deviceType = DEVICE_GENERIC;
    clients[0].lastPoll = 0;
    clients[0].errorCount = 0;
    clients[0].successCount = 0;
    activeclients = 1;
  }

  prefs.end();
}

void saveConfig() {
  Serial.println("Saving configuration...");

  if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(TASK_TIMEOUT)) != pdTRUE) {
    Serial.println("ERROR: Could not acquire config mutex for save!");
    systemErrors++;
    return;
  }

  prefs.begin("modbus-gw", false);

  // Save WiFi credentials
  prefs.putString("ssid", wifi_ssid);
  prefs.putString("password", wifi_password);
  //Serial.println("  Saved SSID: " + (wifi_ssid.length() > 0 ? wifi_ssid : "(empty)"));
  //Serial.println("  Saved Password: " + (wifi_password.length() > 0 ? "***" : "(empty)"));

  // Save system config
  prefs.putString("hostname", hostname);
  prefs.putInt("baud", modbus_baud);
  prefs.putInt("rx_pin", rx_pin);
  prefs.putInt("tx_pin", tx_pin);
  prefs.putInt("active_clients", activeclients);
  Serial.println("  Saved active_clients: " + String(activeclients));

  // Save client configurations
  for (int i = 0; i < activeclients && i < MAX_CLIENTS; i++) {
    String prefix = "client" + String(i) + "_";
    prefs.putUChar((prefix + "id").c_str(), clients[i].id);
    prefs.putBool((prefix + "en").c_str(), clients[i].enabled);
    prefs.putUShort((prefix + "addr").c_str(), clients[i].startAddress);
    prefs.putUShort((prefix + "count").c_str(), clients[i].registerCount);
    prefs.putUShort((prefix + "poll").c_str(), clients[i].pollInterval);
    prefs.putUChar((prefix + "type").c_str(), (uint8_t)clients[i].deviceType);
  }

  prefs.end();
  xSemaphoreGive(configMutex);

  Serial.println("Configuration saved successfully!");
}

// ########## REST API ##########
void setupRESTAPI() {
  // Test Modbus connection
  server.on("/api/test", HTTP_GET, []() {
    if (!server.hasArg("id") || !server.hasArg("addr")) {
      server.send(400, "application/json", "{\"error\":\"Missing id or addr parameter\"}");
      return;
    }

    uint8_t clientId = server.arg("id").toInt();
    uint16_t addr = server.arg("addr").toInt();
    uint16_t count = server.hasArg("count") ? server.arg("count").toInt() : 1;

    uint16_t testRegs[10];
    bool success = mbRTU.readIreg(clientId, addr, testRegs, count, nullptr);

    DynamicJsonDocument doc(512);
    doc["success"] = success;
    doc["client_id"] = clientId;
    doc["address"] = addr;
    doc["count"] = count;

    if (success) {
      JsonArray values = doc.createNestedArray("values");
      for (int i = 0; i < count && i < 10; i++) {
        values.add(testRegs[i]);
      }
    }

    String response;
    serializeJson(doc, response);
    server.send(success ? 200 : 500, "application/json", response);
  });

  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<1024> doc;
    doc["version"] = version;
    doc["hostname"] = hostname;
    doc["uptime"] = millis() / 1000;
    doc["wifi_ssid"] = wifi_ssid;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip_address"] = WiFi.localIP().toString();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["tcp_requests"] = tcpRequests;
    doc["rtu_requests"] = rtuRequests;
    doc["errors"] = errors;
    doc["system_errors"] = systemErrors;
    doc["wifi_reconnects"] = wifiReconnects;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // GET /api/clients - List all clients
  server.on("/api/clients", HTTP_GET, []() {
    DynamicJsonDocument doc(2048);
    JsonArray clientsArray = doc.createNestedArray("clients");

    if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      for (int i = 0; i < activeclients; i++) {
        JsonObject client = clientsArray.createNestedObject();
        client["index"] = i;
        client["id"] = clients[i].id;
        client["enabled"] = clients[i].enabled;
        client["device_type"] = clients[i].deviceType;
        client["device_name"] = deviceProfiles[clients[i].deviceType].name;
        client["start_address"] = clients[i].startAddress;
        client["register_count"] = clients[i].registerCount;
        client["poll_interval"] = clients[i].pollInterval;
        client["success_count"] = clients[i].successCount;
        client["error_count"] = clients[i].errorCount;
      }
      xSemaphoreGive(configMutex);
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // GET /api/client/{id}/data - Get client data
  server.on("/api/client/0/data", HTTP_GET, []() {
    handleClientData(0);
  });
  server.on("/api/client/1/data", HTTP_GET, []() {
    handleClientData(1);
  });
  server.on("/api/client/2/data", HTTP_GET, []() {
    handleClientData(2);
  });

  // GET /api/devices - List available device profiles
  server.on("/api/devices", HTTP_GET, []() {
    DynamicJsonDocument doc(2048);
    JsonArray devicesArray = doc.createNestedArray("devices");

    for (int i = 0; i < DEVICE_PROFILE_COUNT; i++) {
      JsonObject device = devicesArray.createNestedObject();
      device["id"] = i;
      device["name"] = deviceProfiles[i].name;
      device["function"] = deviceProfiles[i].readFunction;
      device["recommended_poll"] = deviceProfiles[i].recommendedPollInterval;
      device["preset_count"] = deviceProfiles[i].presetCount;
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // GET /api/device/{id}/presets - Get device presets
  server.on("/api/device/0/presets", HTTP_GET, []() {
    handleDevicePresets(0);
  });
  server.on("/api/device/1/presets", HTTP_GET, []() {
    handleDevicePresets(1);
  });
  server.on("/api/device/2/presets", HTTP_GET, []() {
    handleDevicePresets(2);
  });
  server.on("/api/device/3/presets", HTTP_GET, []() {
    handleDevicePresets(3);
  });
  server.on("/api/device/4/presets", HTTP_GET, []() {
    handleDevicePresets(4);
  });
  server.on("/api/device/5/presets", HTTP_GET, []() {
    handleDevicePresets(5);
  });
  server.on("/api/device/6/presets", HTTP_GET, []() {
    handleDevicePresets(6);
  });

  // GET /api/registers/{start}/{count} - Read specific registers
  server.on("/api/registers", HTTP_GET, []() {
    if (!server.hasArg("start") || !server.hasArg("count")) {
      server.send(400, "application/json", "{\"error\":\"Missing start or count parameter\"}");
      return;
    }

    int start = server.arg("start").toInt();
    int count = server.arg("count").toInt();

    if (count < 1 || count > 122) {
      server.send(400, "application/json", "{\"error\":\"Invalid count (1-122)\"}");
      return;
    }

    DynamicJsonDocument doc(4096);
    JsonArray registers = doc.createNestedArray("registers");

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      for (int i = 0; i < count && i < MAX_REGISTERS; i++) {
        JsonObject reg = registers.createNestedObject();
        reg["address"] = start + i;
        reg["value"] = inputRegs[0][i];
        reg["hex"] = "0x" + String(inputRegs[0][i], HEX);
      }
      xSemaphoreGive(dataMutex);
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
}

void handleClientData(int clientIndex) {
  if (clientIndex >= activeclients) {
    server.send(404, "application/json", "{\"error\":\"Client not found\"}");
    return;
  }

  DynamicJsonDocument doc(4096);

  if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    doc["client_id"] = clients[clientIndex].id;
    doc["device_type"] = clients[clientIndex].deviceType;
    doc["device_name"] = deviceProfiles[clients[clientIndex].deviceType].name;
    doc["start_address"] = clients[clientIndex].startAddress;
    doc["register_count"] = clients[clientIndex].registerCount;
    xSemaphoreGive(configMutex);
  }

  JsonArray registers = doc.createNestedArray("registers");

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    const DeviceProfile& profile = deviceProfiles[clients[clientIndex].deviceType];

    if (profile.useFloatDecoding) {
      for (int j = 0; j < clients[clientIndex].registerCount - 1; j += 2) {
        if (j + 1 >= MAX_REGISTERS) break;

        JsonObject reg = registers.createNestedObject();
        reg["address"] = clients[clientIndex].startAddress + j;

        uint16_t* regs = (profile.readFunction == FUNC_READ_HOLDING) ? holdingRegs[clientIndex] : inputRegs[clientIndex];

        float value = decodeFloat32(regs[j], regs[j + 1], profile.swapBytes, profile.swapWords);
        reg["value"] = value;
        reg["raw_high"] = regs[j];
        reg["raw_low"] = regs[j + 1];

        if (profile.getRegDescription) {
          reg["description"] = profile.getRegDescription(clients[clientIndex].startAddress + j);
        }
      }
    } else {
      for (int j = 0; j < clients[clientIndex].registerCount; j++) {
        if (j >= MAX_REGISTERS) break;

        JsonObject reg = registers.createNestedObject();
        reg["address"] = clients[clientIndex].startAddress + j;

        uint16_t* regs = (profile.readFunction == FUNC_READ_HOLDING) ? holdingRegs[clientIndex] : inputRegs[clientIndex];
        reg["value"] = regs[j];
      }
    }

    xSemaphoreGive(dataMutex);
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleDevicePresets(int deviceId) {
  if (deviceId >= DEVICE_PROFILE_COUNT) {
    server.send(404, "application/json", "{\"error\":\"Device not found\"}");
    return;
  }

  const DeviceProfile& profile = deviceProfiles[deviceId];

  DynamicJsonDocument doc(2048);
  doc["device_id"] = deviceId;
  doc["device_name"] = profile.name;

  JsonArray presetsArray = doc.createNestedArray("presets");

  if (profile.presets && profile.presetCount > 0) {
    for (int i = 0; i < profile.presetCount; i++) {
      JsonObject preset = presetsArray.createNestedObject();
      preset["start_address"] = profile.presets[i].startAddr;
      preset["count"] = profile.presets[i].count;
      preset["description"] = profile.presets[i].description;
    }
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleCaptivePortal() {
  if (!apMode) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>WiFi Setup - Modbus Gateway</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;}";
  html += ".container{max-width:500px;margin:0 auto;background:#fff;padding:30px;border-radius:15px;box-shadow:0 10px 40px rgba(0,0,0,0.3);}";
  html += "h1{color:#667eea;text-align:center;margin:0 0 10px 0;}";
  html += ".subtitle{text-align:center;color:#666;margin-bottom:30px;}";
  html += ".form-group{margin:20px 0;}";
  html += ".form-group label{display:block;margin-bottom:8px;color:#333;font-weight:bold;}";
  html += ".form-group input{width:100%;padding:12px;border:2px solid #ddd;border-radius:8px;font-size:16px;box-sizing:border-box;transition:border 0.3s;}";
  html += ".form-group input:focus{outline:none;border-color:#667eea;}";
  html += ".form-group small{display:block;margin-top:5px;color:#666;font-size:13px;}";
  html += ".btn{width:100%;padding:15px;background:#667eea;color:#fff;border:none;border-radius:8px;font-size:18px;font-weight:bold;cursor:pointer;transition:background 0.3s;}";
  html += ".btn:hover{background:#5568d3;}";
  html += ".info-box{background:#f0f4ff;padding:15px;border-radius:8px;margin:20px 0;border-left:4px solid #667eea;}";
  html += ".network-list{background:#f8f9fa;padding:15px;border-radius:8px;margin:15px 0;max-height:200px;overflow-y:auto;}";
  html += ".network-item{padding:10px;margin:5px 0;background:#fff;border-radius:5px;cursor:pointer;transition:background 0.3s;}";
  html += ".network-item:hover{background:#e9ecef;}";
  html += ".signal{float:right;color:#667eea;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>WiFi Configuration</h1>";
  html += "<div class='subtitle'>Modbus Gateway Setup</div>";
  html += "<div class='info-box'><strong>Welcome!</strong><br>Connect this device to your WiFi network.</div>";
  html += "<div class='network-list'>";
  html += "<strong>Available Networks:</strong><br>";

  int n = WiFi.scanNetworks();
  if (n > 0) {
    for (int i = 0; i < n && i < 10; i++) {
      int rssi = WiFi.RSSI(i);
      String strength = (rssi > -50) ? "****" : (rssi > -60) ? "***"
                                              : (rssi > -70) ? "**"
                                                             : "*";
      html += "<div class='network-item' onclick='selectNetwork(\"" + WiFi.SSID(i) + "\")'>";
      html += WiFi.SSID(i);
      html += "<span class='signal'>" + strength + "</span></div>";
    }
  } else {
    html += "<div style='color:#666;padding:10px;'>No networks found</div>";
  }
  html += "</div>";
  html += "<form method='POST' action='/wifisave'>";
  html += "<div class='form-group'><label>Network Name (SSID):</label>";
  html += "<input type='text' id='ssid' name='ssid' placeholder='Enter WiFi SSID' required></div>";
  html += "<div class='form-group'><label>Password:</label>";
  html += "<input type='password' name='password' placeholder='Enter WiFi password' required></div>";
  html += "<div class='form-group'><label>Device Hostname (mDNS):</label>";
  html += "<input type='text' name='hostname' value='" + hostname + "' placeholder='modbus-gateway' pattern='[a-zA-Z0-9-]{1,32}' required>";
  html += "<small>Letters, numbers, and hyphens only. Max 32 characters.</small></div>";
  html += "<button type='submit' class='btn'>Connect to WiFi</button></form></div>";
  html += "<script>function selectNetwork(ssid){document.getElementById('ssid').value=ssid;}</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleWiFiSave() {
  Serial.println("\n=== WiFi Save Request ===");

  if (!server.hasArg("ssid") || !server.hasArg("password") || !server.hasArg("hostname")) {
    Serial.println("ERROR: Missing required fields");
    server.send(400, "text/html", "<h1>Error: Missing credentials</h1>");
    return;
  }

  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  String newHostname = server.arg("hostname");

  Serial.println("Received credentials:");
  Serial.println("  SSID: " + newSsid);
  Serial.println("  Password length: " + String(newPassword.length()));
  Serial.println("  Hostname: " + newHostname);

  if (newSsid.length() == 0 || newSsid.length() > 32) {
    Serial.println("ERROR: Invalid SSID length");
    server.send(400, "text/html", "<h1>Error: Invalid SSID</h1>");
    return;
  }

  if (newPassword.length() < 8 || newPassword.length() > 63) {
    Serial.println("ERROR: Invalid password length");
    server.send(400, "text/html", "<h1>Error: Password must be 8-63 characters</h1>");
    return;
  }

  if (!validateHostname(newHostname)) {
    Serial.println("ERROR: Invalid hostname");
    server.send(400, "text/html", "<h1>Error: Invalid hostname</h1>");
    return;
  }

  // Update global variables
  wifi_ssid = newSsid;
  wifi_password = newPassword;
  hostname = newHostname;
  hostname.replace(" ", "-");
  hostname.toLowerCase();

  // CRITICAL: Save configuration BEFORE attempting to connect
  Serial.println("Updating configuration...");
  saveConfig();
  Serial.println("Configuration saved!");

  // Send response to user
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='15;url=/'>";
  html += "<title>Connecting...</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;}";
  html += ".container{max-width:500px;background:#fff;padding:40px;border-radius:15px;box-shadow:0 10px 40px rgba(0,0,0,0.3);text-align:center;}";
  html += "h1{color:#667eea;margin:0 0 20px 0;}";
  html += ".spinner{border:4px solid #f3f3f3;border-top:4px solid #667eea;border-radius:50%;width:60px;height:60px;animation:spin 1s linear infinite;margin:30px auto;}";
  html += "@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}";
  html += ".info{color:#666;margin:20px 0;line-height:1.6;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Connecting to WiFi</h1>";
  html += "<div class='spinner'></div>";
  html += "<div class='info'>Please wait...</div>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
  delay(2000);

  Serial.println("Stopping AP mode and attempting WiFi connection...");
  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  if (connectWiFi()) {
    Serial.println("Successfully connected to WiFi!");
  } else {
    Serial.println("Failed to connect. Restarting...");
    delay(3000);
    ESP.restart();
  }
}

String getLastErrors() {
  // This is a simple implementation - you could expand this with a circular buffer
  String errorLog = "";
  if (systemErrors > 0) errorLog += "System errors: " + String(systemErrors) + "\n";
  if (errors > 0) errorLog += "Modbus errors: " + String(errors) + "\n";
  if (wifiReconnects > 0) errorLog += "WiFi reconnects: " + String(wifiReconnects) + "\n";
  return errorLog.length() > 0 ? errorLog : "No errors logged";
}


void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    if (apMode) {
      handleCaptivePortal();
      return;
    }

    String html = getHTMLHeader("Modbus Gateway");
    html += "<div class='container'>";
    html += "<h1>ESP32 Modbus Gateway</h1>";
    html += "<a href='/status' class='btn btn-primary'>Status</a>";
    html += "<a href='/config' class='btn btn-primary'>Configuration</a>";
    html += "<a href='/clients' class='btn btn-primary'>Manage Clients</a>";
    html += "<a href='/debug' class='btn btn-primary'>Preview</a>";
    html += "<a href='/firmware' class='btn btn-primary'>Firmware</a>";
    html += "<button onclick='reboot()' class='btn btn-danger'>Reboot</button>";
    html += "<div class='info-box' style='margin-top:20px;'>";
    html += "<strong>MODBUS TCP:</strong><br>";
    html += "IP: " + WiFi.localIP().toString() + "<br>";
    html += "Poort: 502<br>";  // use field
    html += "Hostname: http://" + hostname + ".local";
    html += "<br><br>";
    html += "<strong>REST API Endpoints:</strong><br>";
    html += "GET /api/status<br>";
    html += "GET /api/clients<br>";
    html += "GET /api/client/{id}/data<br>";
    html += "GET /api/devices<br>";
    html += "GET /api/device/{id}/presets";
    html += "</div>";
    html += "<script>";
    html += "function reboot(){if(confirm('Reboot?')){fetch('/reboot',{method:'POST'}).then(()=>alert('Rebooting...'));}}";
    html += "</script>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/wificonfig", HTTP_GET, handleCaptivePortal);
  server.on("/wifisave", HTTP_POST, handleWiFiSave);

  server.on("/status", HTTP_GET, []() {
    String html = getHTMLHeader("Status");
    html += "<div class='container'>";
    html += "<h1>Gateway Status</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<button onclick='location.reload()' class='btn btn-primary'>Refresh</button>";

    html += "<div class='info-box'>";
    html += "<h3>System</h3>";
    html += "Version: " + String(version) + "<br>";
    html += "Uptime: " + String(millis() / 1000) + "s (" + String(millis() / 3600000) + "h " + String((millis() % 3600000) / 60000) + "m)<br>";
    html += "Free Heap: " + String(ESP.getFreeHeap() / 1024) + " KB<br>";
    html += "CPU Frequency: " + String(ESP.getCpuFreqMHz()) + " MHz<br>";
    html += "</div>";

    html += "<div class='info-box'>";
    html += "<h3>Network</h3>";
    if (!apMode) {
      html += "Mode: Station (Connected)<br>";
      html += "SSID: " + wifi_ssid + "<br>";
      html += "IP: " + WiFi.localIP().toString() + "<br>";
      html += "Gateway: " + WiFi.gatewayIP().toString() + "<br>";
      html += "Subnet: " + WiFi.subnetMask().toString() + "<br>";
      html += "DNS: " + WiFi.dnsIP().toString() + "<br>";
      html += "Hostname: http://" + hostname + ".local<br>";

      int rssi = WiFi.RSSI();
      String signal = (rssi > -50) ? "Excellent (****)" : (rssi > -60) ? "Good (***)"
                                                        : (rssi > -70) ? "Fair (**)"
                                                                       : "Weak (*)";
      html += "RSSI: " + String(rssi) + " dBm (" + signal + ")<br>";
      html += "MAC: " + WiFi.macAddress() + "<br>";

      html += "WiFi Reconnects: " + String(wifiReconnects) + "<br>";
      html += "<br><button onclick='resetStats()' class='btn btn-danger' style='padding:8px 15px;font-size:14px;'>Reset Statistics</button>";
      //html += "</div>";

    } else {
      html += "Mode: Access Point<br>";
      html += "AP SSID: " + String(ap_ssid) + "<br>";
      html += "AP IP: " + WiFi.softAPIP().toString() + "<br>";
      html += "Connected Clients: " + String(WiFi.softAPgetStationNum()) + "<br>";
    }
    html += "</div>";

    html += "<div class='info-box'>";
    html += "<h3>Modbus Statistics</h3>";
    html += "TCP Requests: " + String(tcpRequests) + "<br>";
    html += "RTU Requests: " + String(rtuRequests) + "<br>";
    html += "Errors: " + String(errors) + "<br>";
    html += "System Errors: " + String(systemErrors) + "<br>";
    html += "WiFi Reconnects: " + String(wifiReconnects) + "<br>";
    html += "</div>";

    html += "<div class='info-box'>";
    html += "<h3>Active Clients</h3>";
    if (activeclients > 0) {
      html += "<table style='width:100%;'>";
      html += "<tr><th>ID</th><th>Device</th><th>Success</th><th>Errors</th><th>Success Rate</th></tr>";
      for (int i = 0; i < activeclients; i++) {
        if (clients[i].enabled) {
          unsigned long total = clients[i].successCount + clients[i].errorCount;
          float successRate = (total > 0) ? (clients[i].successCount * 100.0 / total) : 0;
          html += "<tr>";
          html += "<td>" + String(clients[i].id) + "</td>";
          html += "<td>" + deviceProfiles[clients[i].deviceType].name + "</td>";
          html += "<td>" + String(clients[i].successCount) + "</td>";
          html += "<td>" + String(clients[i].errorCount) + "</td>";
          html += "<td>" + String(successRate, 1) + "%</td>";
          html += "</tr>";
        }
      }
      html += "</table>";
    } else {
      html += "No active clients configured.";
    }
    html += "</div>";

    html += "<script>setTimeout(function(){location.reload();}, 30000);</script>";  // Auto-refresh every 30s
    html += "<script>";
    html += "function resetStats(){if(confirm('Reset all statistics?')){fetch('/reset-stats',{method:'POST'}).then(()=>{alert('Statistics reset!');location.reload();});}}";
    html += "</script>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/errors", HTTP_GET, []() {
    String html = getHTMLHeader("Error Log");
    html += "<div class='container'>";
    html += "<h1>Error Log</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<div class='info-box'>";
    html += "<pre>" + getLastErrors() + "</pre>";
    html += "</div>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/reset-stats", HTTP_POST, []() {
    Serial.println("Resetting statistics...");

    tcpRequests = 0;
    rtuRequests = 0;
    errors = 0;
    systemErrors = 0;
    wifiReconnects = 0;

    for (int i = 0; i < activeclients; i++) {
      clients[i].successCount = 0;
      clients[i].errorCount = 0;
    }

    server.send(200, "text/plain", "Statistics reset successfully");
  });

  server.on("/reconnect", HTTP_POST, []() {
    if (!apMode || wifi_ssid.length() == 0) {
      server.send(400, "text/plain", "Not in AP mode or no saved network");
      return;
    }

    server.send(200, "text/plain", "Attempting reconnection...");
    delay(100);

    Serial.println("Manual reconnection requested via web interface");
    if (connectWiFi()) {
      Serial.println("Manual reconnection successful!");
    } else {
      Serial.println("Manual reconnection failed");
      // Ensure AP mode is still running
      WiFi.softAP(ap_ssid, ap_password);
      IPAddress IP = WiFi.softAPIP();
      dnsServer.start(DNS_PORT, "*", IP);
    }
  });

  server.on("/config", HTTP_GET, []() {
    String html = getHTMLHeader("Configuration");
    html += "<div class='container'>";
    html += "<h1>System Configuration</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<form method='POST' action='/config'>";
    html += "<div class='form-group'>";
    html += "<label>WiFi SSID:</label>";
    html += "<input type='text' name='ssid' value='" + wifi_ssid + "' required>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>WiFi Password:</label>";
    html += "<input type='password' name='password' value='" + wifi_password + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>Hostname:</label>";
    html += "<input type='text' name='hostname' value='" + hostname + "' required>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>Modbus Baud Rate:</label>";
    html += "<select name='baud'>";
    int bauds[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
    for (int i = 0; i < 8; i++) {
      html += "<option value='" + String(bauds[i]) + "'" + (modbus_baud == bauds[i] ? " selected" : "") + ">" + String(bauds[i]) + "</option>";
    }
    html += "</select>";
    html += "</div>";

    html += "<div class='form-group'>";
    html += "<label>RX Pin:</label>";
    html += "<input type='number' name='rx_pin' value='" + String(rx_pin) + "' min='0' max='39' required>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>TX Pin:</label>";
    html += "<input type='number' name='tx_pin' value='" + String(tx_pin) + "' min='0' max='39' required>";
    html += "</div>";

    html += "<button type='submit' class='btn btn-primary'>Save Configuration</button>";
    html += "</form>";

    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/config", HTTP_POST, []() {
    Serial.println("\n=== Config Save Request ===");

    // Save WiFi credentials
    if (server.hasArg("ssid")) {
      wifi_ssid = server.arg("ssid");
      Serial.println("  Updated SSID: " + wifi_ssid);
    }
    if (server.hasArg("password")) {
      wifi_password = server.arg("password");
      //Serial.println("  Updated Password: " + (wifi_password.length() > 0 ? "***" : "(empty)"));
    }

    // Save hostname
    if (server.hasArg("hostname") && validateHostname(server.arg("hostname"))) {
      hostname = server.arg("hostname");
      Serial.println("  Updated Hostname: " + hostname);
    }

    // Save baud rate
    if (server.hasArg("baud") && validateBaudRate(server.arg("baud").toInt())) {
      modbus_baud = server.arg("baud").toInt();
      Serial.println("  Updated Baud: " + String(modbus_baud));
    }

    // Save RX/TX pins
    if (server.hasArg("rx_pin")) {
      rx_pin = server.arg("rx_pin").toInt();
      Serial.println("  Updated RX Pin: " + String(rx_pin));
    }
    if (server.hasArg("tx_pin")) {
      tx_pin = server.arg("tx_pin").toInt();
      Serial.println("  Updated TX Pin: " + String(tx_pin));
    }

    // Save to flash
    saveConfig();

    String html = getHTMLHeader("Configuration Saved");
    html += "<div class='container'>";
    html += "<h1>Configuration Saved</h1>";
    html += "<div class='info-box'>";
    html += "<p>Configuration has been saved successfully!</p>";
    html += "<p><strong>Note:</strong> If you changed WiFi settings, the device will attempt to reconnect.</p>";
    html += "</div>";
    html += "<a href='/config' class='btn btn-primary'>Back to Config</a>";
    html += "<a href='/' class='btn btn-secondary'>Home</a>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);

    // If WiFi credentials changed, attempt reconnect
    if (server.hasArg("ssid") || server.hasArg("password")) {
      delay(2000);
      Serial.println("WiFi credentials changed, attempting reconnect...");
      if (connectWiFi()) {
        Serial.println("Reconnected successfully!");
      } else {
        Serial.println("Reconnection failed, starting AP mode...");
        startAPMode();
      }
    }
  });

  server.on("/clients", HTTP_GET, []() {
    String html = getHTMLHeader("Manage Clients");
    html += "<div class='container'>";
    html += "<h1>Manage Clients</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<form method='POST' action='/clients'>";

    for (int i = 0; i < MAX_CLIENTS; i++) {
      html += "<div style='background:#3d3d3d;padding:20px;border-radius:5px;margin:15px 0;'>";
      html += "<h3>Client " + String(i + 1) + "</h3>";
      html += "<div class='form-group'>";
      html += "<label class='checkbox-label'><input type='checkbox' name='en" + String(i) + "' " + String(i < activeclients && clients[i].enabled ? "checked" : "") + "> Enabled</label>";
      html += "</div>";

      html += "<div class='form-group'>";
      html += "<label>Device Type:</label>";
      html += "<select name='type" + String(i) + "' id='type" + String(i) + "' onchange='updatePresets(" + String(i) + ")'>";
      for (int d = 0; d < DEVICE_PROFILE_COUNT; d++) {
        html += "<option value='" + String(d) + "' " + String(i < activeclients && clients[i].deviceType == d ? "selected" : "") + ">" + deviceProfiles[d].name + "</option>";
      }
      html += "</select></div>";

      html += "<div class='form-group' id='presets" + String(i) + "' style='background:#2d2d2d;padding:10px;border-radius:5px;'>";
      html += "<label>Register Presets:</label>";
      html += "<div id='preset_list" + String(i) + "'>Select device type to see presets</div>";
      html += "</div>";

      html += "<div class='form-group'>";
      html += "<label>Client ID (1-247):</label>";
      html += "<input type='number' name='id" + String(i) + "' value='" + String(i < activeclients ? clients[i].id : i + 1) + "' min='1' max='247'>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Start Address:</label>";
      html += "<input type='number' name='addr" + String(i) + "' value='" + String(i < activeclients ? clients[i].startAddress : 0) + "' min='0' max='65535'>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Register Count:</label>";
      html += "<input type='number' name='count" + String(i) + "' value='" + String(i < activeclients ? clients[i].registerCount : 10) + "' min='1' max='120'>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Poll Interval (ms):</label>";
      html += "<input type='number' name='poll" + String(i) + "' value='" + String(i < activeclients ? clients[i].pollInterval : 1000) + "' min='100' max='60000'>";
      html += "</div>";
      html += "</div>";
    }

    html += "<button type='submit' class='btn btn-primary'>Save All Clients</button>";
    html += "</form>";

    html += "<script>";
    html += "const presets = " + getPresetsJSON() + ";";
    html += "function updatePresets(idx){";
    html += "const type=document.getElementById('type'+idx).value;";
    html += "const list=document.getElementById('preset_list'+idx);";
    html += "if(presets[type]&&presets[type].length>0){";
    html += "let html='';";
    html += "presets[type].forEach((p,i)=>{";
    html += "html+='<div style=\"margin:5px 0;padding:8px;background:#3d3d3d;border-radius:3px;cursor:pointer;\" onclick=\"applyPreset('+idx+','+p.addr+','+p.count+')\">'+p.desc+'</div>';";
    html += "});";
    html += "list.innerHTML=html;";
    html += "}else{list.innerHTML='No presets available';}}";
    html += "function applyPreset(idx,addr,count){";
    html += "document.getElementsByName('addr'+idx)[0].value=addr;";
    html += "document.getElementsByName('count'+idx)[0].value=count;}";
    html += "</script>";

    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/clients", HTTP_POST, []() {
    activeclients = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (server.hasArg("en" + String(i)) || (i == 0 && server.hasArg("id" + String(i)))) {
        clients[activeclients].enabled = server.hasArg("en" + String(i));
        clients[activeclients].id = server.arg("id" + String(i)).toInt();
        clients[activeclients].deviceType = (DeviceType)server.arg("type" + String(i)).toInt();
        clients[activeclients].startAddress = server.arg("addr" + String(i)).toInt();
        clients[activeclients].registerCount = server.arg("count" + String(i)).toInt();
        clients[activeclients].pollInterval = server.arg("poll" + String(i)).toInt();
        clients[activeclients].lastPoll = 0;
        clients[activeclients].errorCount = 0;
        clients[activeclients].successCount = 0;
        activeclients++;
      }
    }
    if (activeclients == 0) activeclients = 1;
    saveConfig();
    server.sendHeader("Location", "/clients");
    server.send(303);
  });

  // preview data
  server.on("/debug", HTTP_GET, []() {
    String html = getHTMLHeader("Debug");
    html += "<div class='container'>";
    html += "<h1>Preview Data</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<button onclick='location.reload()' class='btn btn-primary'>Refresh</button>";

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      for (int i = 0; i < activeclients; i++) {
        if (clients[i].enabled) {
          const DeviceProfile& profile = deviceProfiles[clients[i].deviceType];
          html += "<h4>" + profile.name + " (ID: " + String(clients[i].id) + ")</h4>";
          html += "<table style='width:100%;border-collapse:collapse;font-size:12px;'>";
          html += "<tr style='background:#4d4d4d;'>";
          html += "<th style='padding:6px;border:1px solid #555;'>Addr</th>";
          html += "<th style='padding:6px;border:1px solid #555;'>Value</th>";
          html += "<th style='padding:6px;border:1px solid #555;'>Description</th>";
          html += "</tr>";

          uint16_t* regs = (profile.readFunction == FUNC_READ_HOLDING) ? holdingRegs[i] : inputRegs[i];

          if (profile.useFloatDecoding) {
            for (int j = 0; j < clients[i].registerCount - 1; j += 2) {
              if (j + 1 >= MAX_REGISTERS) break;
              float value = decodeFloat32(regs[j], regs[j + 1], profile.swapBytes, profile.swapWords);
              html += "<tr style='background:#3d3d3d;'>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(clients[i].startAddress + j) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(value, 2) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;'>";
              if (profile.getRegDescription) {
                html += profile.getRegDescription(clients[i].startAddress + j);
              } else {
                html += "Register " + String(clients[i].startAddress + j);
              }
              html += "</td></tr>";
            }
          }
          html += "</table>";
        }
      }
      xSemaphoreGive(dataMutex);
    }

    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/firmware", HTTP_GET, []() {
    String html = getHTMLHeader("Firmware Update");
    html += "<div class='container'>";
    html += "<h1>Firmware Update</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<div style='background:#3d3d3d;padding:15px;border-radius:5px;margin:20px 0;'>";
    html += "<h3>Current Firmware Info</h3>";
    html += "<p><strong>Version:</strong> " + String(version) + "</p>";
    html += "<p><strong>Free Space:</strong> " + String(ESP.getFreeSketchSpace() / 1024) + " KB</p>";
    html += "<p><strong>Current Size:</strong> " + String(ESP.getSketchSize() / 1024) + " KB</p>";
    html += "<p><strong>Flash Size:</strong> " + String(ESP.getFlashChipSize() / 1024) + " KB</p>";
    html += "</div>";
    html += "<p>Select a firmware file (.bin) compiled for ESP32:</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data' onsubmit='return validateFile()'>";
    html += "<input type='file' id='fileInput' name='update' accept='.bin' required>";
    html += "<p id='fileInfo' style='color:#aaa;margin:10px 0;'></p>";
    html += "<br>";
    html += "<div style='background:#4d2020;padding:15px;border-radius:5px;margin:20px 0;'>";
    html += "<strong>Warning:</strong> do not disconnect power or close browser during update!";
    html += "</div><br>";
    html += "<button type='submit' class='btn btn-primary'>Upload Firmware</button>";
    html += "</form>";
    html += "<div id='uploadProgress' style='display:none;margin-top:20px;'>";
    html += "<p>Uploading firmware... Please wait.</p>";
    html += "<p style='color:#aaa;'>This may take 1-2 minutes. Do NOT close this window or disconnect power!</p>";
    html += "</div>";
    html += "</div>";
    html += "<script>";
    html += "document.getElementById('fileInput').addEventListener('change', function(e) {";
    html += "  var file = e.target.files[0];";
    html += "  if (file) {";
    html += "    var sizeMB = (file.size / 1024 / 1024).toFixed(2);";
    html += "    document.getElementById('fileInfo').innerHTML = 'File: ' + file.name + ' (' + sizeMB + ' MB)';";
    html += "  }";
    html += "});";
    html += "function validateFile() {";
    html += "  var file = document.getElementById('fileInput').files[0];";
    html += "  if (!file) { alert('Please select a file'); return false; }";
    html += "  if (!file.name.endsWith('.bin')) { alert('Please select a .bin file'); return false; }";
    html += "  if (file.size > " + String(ESP.getFreeSketchSpace()) + ") {";
    html += "    alert('File too large! Max size: " + String(ESP.getFreeSketchSpace() / 1024) + " KB');";
    html += "    return false;";
    html += "  }";
    html += "  if (!confirm('Update firmware?\\n\\nFile: ' + file.name + '\\nSize: ' + (file.size/1024).toFixed(1) + ' KB\\n\\n Do NOT disconnect power!')) {";
    html += "    return false;";
    html += "  }";
    html += "  document.getElementById('uploadProgress').style.display = 'block';";
    html += "  return true;";
    html += "}";
    html += "</script>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on(
    "/update", HTTP_POST, []() {
      String html = getHTMLHeader("Update Complete");
      html += "<div class='container'>";
      if (Update.hasError()) {
        html += "<h1>Update Failed</h1>";
        html += "<p style='color:#f44336;'>Error Code: " + String(Update.getError()) + "</p>";
        html += "<p>Possible causes:</p>";
        html += "<ul style='text-align:left;'>";
        html += "<li>Incorrect firmware file</li>";
        html += "<li>Not enough space</li>";
        html += "<li>Corrupted file</li>";
        html += "<li>Connection interrupted</li>";
        html += "</ul>";
        html += "<a href='/firmware' class='btn btn-primary'>Try Again</a>";
        html += "<a href='/' class='btn btn-secondary'>Back</a>";
      } else {
        html += "<h1>Update Successful!</h1>";
        html += "<p>Firmware updated successfully. Device will reboot in 3 seconds...</p>";
        html += "<p>After reboot, refresh this page.</p>";
        html += "<script>setTimeout(()=>{window.location='/';},10000);</script>";  // 5000 old value
      }
      html += "</div>";
      html += getHTMLFooter();
      server.send(200, "text/html", html);

      if (!Update.hasError()) {
        delay(2000);
        ESP.restart();
      }
    },
    []() {
      HTTPUpload& upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (modbusTaskHandle != NULL) {
          vTaskSuspend(modbusTaskHandle);
        }

        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace)) {
          Update.printError(Serial);
          if (modbusTaskHandle != NULL) {
            vTaskResume(modbusTaskHandle);
          }
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.printf("Update Success: %u bytes\n", upload.totalSize);
        } else {
          Update.printError(Serial);
          if (modbusTaskHandle != NULL) {
            vTaskResume(modbusTaskHandle);
          }
        }
      } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        if (modbusTaskHandle != NULL) {
          vTaskResume(modbusTaskHandle);
        }
      }
    });

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });

  server.onNotFound([]() {
    if (apMode) handleCaptivePortal();
    else server.send(404, "text/plain", "Not Found");
  });

  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);
}

String getPresetsJSON() {
  String json = "{";
  for (int d = 0; d < DEVICE_PROFILE_COUNT; d++) {
    json += "\"" + String(d) + "\":[";
    if (deviceProfiles[d].presets && deviceProfiles[d].presetCount > 0) {
      for (int p = 0; p < deviceProfiles[d].presetCount; p++) {
        if (p > 0) json += ",";
        json += "{\"addr\":" + String(deviceProfiles[d].presets[p].startAddr);
        json += ",\"count\":" + String(deviceProfiles[d].presets[p].count);
        json += ",\"desc\":\"" + deviceProfiles[d].presets[p].description + "\"}";
      }
    }
    json += "]";
    if (d < DEVICE_PROFILE_COUNT - 1) json += ",";
  }
  json += "}";
  return json;
}

// ########## REGISTER DESCRIPTIONS ##########
String getGenericRegDescription(uint16_t regAddr) {
  return "Register " + String(regAddr);
}

String getGrowattRegDescription(uint16_t regAddr) {
  switch (regAddr) {
    case 0: return "Inverter Status";
    case 1: return "PV Total Power H";
    case 2: return "PV Total Power L";
    case 3: return "PV1 Voltage";
    case 4: return "PV1 Current";
    case 5: return "PV1 Power H";
    case 6: return "PV1 Power L";
    case 7: return "PV2 Voltage";
    case 8: return "PV2 Current";
    case 9: return "PV2 Power H";
    case 10: return "PV2 Power L";
    case 35: return "Output Power H";
    case 36: return "Output Power L";
    case 37: return "Grid Frequency";
    case 38: return "Phase 1 Voltage";
    case 39: return "Phase 1 Current";
    case 40: return "Phase 1 Power H";
    case 41: return "Phase 1 Power L";
    case 42: return "Phase 2 Voltage";
    case 43: return "Phase 2 Current";
    case 44: return "Phase 2 Power H";
    case 45: return "Phase 2 Power L";
    case 46: return "Phase 3 Voltage";
    case 47: return "Phase 3 Current";
    case 48: return "Phase 3 Power H";
    case 49: return "Phase 3 Power L";
    case 53: return "Energy Today H";
    case 54: return "Energy Today L";
    case 55: return "Energy Total H";
    case 56: return "Energy Total L";
    case 93: return "Inverter Temperature";
    case 94: return "IPM Temperature";
    case 95: return "Boost Temperature";
    case 98: return "P Bus Voltage";
    case 99: return "N Bus Voltage";
    case 105: return "Fault Code";
    case 106: return "Fault Bit Code H";
    case 107: return "Fault Bit Code L";
    default: return "Register " + String(regAddr);
  }
}

String getSDM630Description(uint16_t regAddr) {
  switch (regAddr) {
    case 0: return "Phase 1 Voltage";
    case 2: return "Phase 2 Voltage";
    case 4: return "Phase 3 Voltage";
    case 6: return "Phase 1 Current";
    case 8: return "Phase 2 Current";
    case 10: return "Phase 3 Current";
    case 12: return "Phase 1 Power";
    case 14: return "Phase 2 Power";
    case 16: return "Phase 3 Power";
    case 18: return "Phase 1 Apparent Power";
    case 20: return "Phase 2 Apparent Power";
    case 22: return "Phase 3 Apparent Power";
    case 24: return "Phase 1 Reactive Power";
    case 26: return "Phase 2 Reactive Power";
    case 28: return "Phase 3 Reactive Power";
    case 30: return "Phase 1 Power Factor";
    case 32: return "Phase 2 Power Factor";
    case 34: return "Phase 3 Power Factor";
    case 36: return "Phase 1 Phase Angle";
    case 38: return "Phase 2 Phase Angle";
    case 40: return "Phase 3 Phase Angle";
    case 42: return "Average L-N Voltage";
    case 46: return "Average Line Current";
    case 48: return "Sum of Line Currents";
    case 52: return "Total System Power";
    case 56: return "Total System Apparent Power";
    case 60: return "Total System Reactive Power";
    case 62: return "Total Power Factor";
    case 66: return "Total Phase Angle";
    case 70: return "Frequency";
    case 72: return "Total Import Energy";
    case 74: return "Total Export Energy";
    case 76: return "Total Import Reactive Energy";
    case 78: return "Total Export Reactive Energy";
    case 80: return "Total Apparent Energy";
    case 82: return "Total Current";
    case 84: return "Total System Power Demand";
    case 86: return "Maximum Total System Power Demand";
    case 200: return "Line 1 to Line 2 Voltage";
    case 202: return "Line 2 to Line 3 Voltage";
    case 204: return "Line 3 to Line 1 Voltage";
    case 206: return "Average L-L Voltage";
    case 224: return "Neutral Current";
    case 342: return "Total kwh";
    case 344: return "Total kvarh(3)";
    case 346: return "L1 import kwh ";
    case 348: return "L2 import kwh ";
    case 350: return "L3 import kWh ";
    case 352: return "L1 export kWh ";
    case 354: return "L2 export kwh ";
    case 356: return "L3 export kWh ";
    case 358: return "L1 total kwh(3) ";
    case 360: return "L2 total kWh(3) ";
    case 362: return "L3 total kwh(3)";


    default: return "Register " + String(regAddr);
  }
}

String getSolarEdgeRegDescription(uint16_t regAddr) {
  switch (regAddr) {
    case 40000: return "SunSpec ID";
    case 40001: return "SunSpec DID";
    case 40003: return "AC Current (A)";
    case 40004: return "AC Current Phase A";
    case 40005: return "AC Current Phase B";
    case 40006: return "AC Current Phase C";
    case 40007: return "AC Voltage Phase AB";
    case 40012: return "AC Frequency (Hz)";
    case 40083: return "AC Power (W)";
    case 40084: return "AC Lifetime Energy (Wh)";
    case 40092: return "DC Current (A)";
    case 40093: return "DC Voltage (V)";
    case 40094: return "DC Power (W)";
    case 40103: return "Heat Sink Temperature (C)";
    case 40107: return "Operating State";
    default: return "Register " + String(regAddr);
  }
}

String getSMARegDescription(uint16_t regAddr) {
  switch (regAddr) {
    case 30051: return "Total Power (W)";
    case 30057: return "Grid Frequency (Hz)";
    case 30201: return "Operating Status";
    case 30513: return "AC Voltage Phase 1 (V)";
    case 30515: return "AC Voltage Phase 2 (V)";
    case 30517: return "AC Voltage Phase 3 (V)";
    case 30521: return "AC Current Phase 1 (A)";
    case 30523: return "AC Current Phase 2 (A)";
    case 30525: return "AC Current Phase 3 (A)";
    case 30529: return "Total Yield (Wh)";
    case 30535: return "Daily Yield (Wh)";
    case 30775: return "Today's Yield (Wh)";
    case 30953: return "Device Temperature (C)";
    default: return "Register " + String(regAddr);
  }
}

String getFroniusRegDescription(uint16_t regAddr) {
  switch (regAddr) {
    case 40001: return "SunSpec Common Model";
    case 40069: return "Inverter Model ID";
    case 40071: return "AC Current (A)";
    case 40072: return "AC Current Phase A";
    case 40073: return "AC Current Phase B";
    case 40074: return "AC Current Phase C";
    case 40079: return "AC Voltage AB (V)";
    case 40080: return "AC Voltage BC (V)";
    case 40081: return "AC Voltage CA (V)";
    case 40083: return "AC Power (W)";
    case 40084: return "AC Frequency (Hz)";
    case 40085: return "AC Apparent Power (VA)";
    case 40091: return "DC Current (A)";
    case 40092: return "DC Voltage (V)";
    case 40093: return "DC Power (W)";
    case 40103: return "Cabinet Temperature (C)";
    case 40107: return "Operating State";
    case 40118: return "Lifetime Energy (Wh)";
    default: return "Register " + String(regAddr);
  }
}

String getHuaweiRegDescription(uint16_t regAddr) {
  switch (regAddr) {
    case 30000: return "Model";
    case 30003: return "Serial Number";
    case 30015: return "Rated Power (W)";
    case 32000: return "State 1";
    case 32002: return "State 2";
    case 32003: return "State 3";
    case 32008: return "Alarm 1";
    case 32009: return "Alarm 2";
    case 32010: return "Alarm 3";
    case 32016: return "Input Power (kW)";
    case 32064: return "Line Voltage AB (V)";
    case 32066: return "Line Voltage BC (V)";
    case 32068: return "Line Voltage CA (V)";
    case 32070: return "Phase A Voltage (V)";
    case 32072: return "Phase B Voltage (V)";
    case 32074: return "Phase C Voltage (V)";
    case 32076: return "Phase A Current (A)";
    case 32078: return "Phase B Current (A)";
    case 32080: return "Phase C Current (A)";
    //case 32080: return "Peak Active Power (kW)";
    case 32082: return "Active Power (kW)";
    case 32084: return "Reactive Power (kVar)";
    case 32086: return "Power Factor";
    case 32088: return "Grid Frequency (Hz)";
    case 32106: return "Internal Temperature (C)";
    case 32114: return "Insulation Resistance (MOhm)";
    //case 32106: return "Device Status";
    case 37113: return "Daily Energy (kWh)";
    case 37119: return "Total Energy (MWh)";
    default: return "Register " + String(regAddr);
  }
}

String getHTMLHeader(String title) {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + title + "</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#1a1a1a;color:#fff;}";
  html += ".container{max-width:900px;margin:0 auto;background:#2d2d2d;padding:30px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.3);}";
  html += "h1{color:#4CAF50;margin-top:0;}";
  html += "h2,h3,h4{color:#4CAF50;}";
  html += ".btn{display:inline-block;padding:15px 30px;margin:10px 5px;text-decoration:none;border-radius:5px;font-size:16px;border:none;cursor:pointer;transition:all 0.3s;}";
  html += ".btn-primary{background:#2196F3;color:#fff;}";
  html += ".btn-secondary{background:#666;color:#fff;}";
  html += ".btn-danger{background:#f44336;color:#fff;}";
  html += ".btn:hover{opacity:0.8;}";
  html += ".info-box{background:#3d3d3d;padding:15px;border-radius:5px;margin:15px 0;border-left:4px solid #4CAF50;}";
  html += ".form-group{margin:15px 0;}";
  html += ".form-group label{display:block;margin-bottom:5px;color:#4CAF50;font-weight:bold;}";
  html += ".form-group input,.form-group select{width:100%;padding:10px;border:1px solid #555;background:#3d3d3d;color:#fff;border-radius:5px;box-sizing:border-box;}";
  html += ".form-group input:focus,.form-group select:focus{outline:none;border-color:#4CAF50;}";
  html += ".form-group input[type='checkbox']{width:auto;margin-right:8px;}";
  html += ".checkbox-label{display:inline-flex;align-items:center;color:#fff;font-weight:normal;margin-right:20px;cursor:pointer;}";
  html += "table{width:100%;border-collapse:collapse;margin:15px 0;}";
  html += "th,td{padding:10px;border:1px solid #555;text-align:left;}";
  html += "th{background:#4d4d4d;color:#4CAF50;}";
  html += "tr:nth-child(even){background:#3d3d3d;}";
  html += "</style>";
  html += "</head><body>";
  return html;
}

String getHTMLFooter() {
  String footer = "<div style='text-align:center;margin-top:20px;color:#888;'>";
  footer += "ESP32 Modbus Gateway v" + String(version) + "";
  footer += "</div>";
  footer += "</body></html>";
  return footer;
}
