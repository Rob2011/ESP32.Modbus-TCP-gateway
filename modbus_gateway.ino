/*
 * ESP32 Modbus RTU to TCP/IP Gateway with Web Portal and Captive Portal
 * Complete working version with WiFi Configuration AP and mDNS hostname configuration
 * By Rob Hagemann - Dec. 2025
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
#include <time.h>

// Configuration
String wifi_ssid = "";
String wifi_password = "";
String hostname = "modbus";     // address will be: http://modbus.local
int modbus_baud = 9600;
int rx_pin = 16;
int tx_pin = 17;
String version = "3.5";

// Access Point Configuration
const char* ap_ssid = "ModbusGateway-Config";
const char* ap_password = "modbus123";
bool apMode = false;
DNSServer dnsServer;
const byte DNS_PORT = 53;

// time not used yet in this version
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;      // Adjust for your timezone
const int daylightOffset_sec = 3600;  // Adjust for DST

#define MODBUS_SERIAL Serial2
#define MAX_clientS 3

struct clientConfig {
  uint8_t id;
  bool enabled;
  uint16_t startAddress;
  uint16_t registerCount;
  uint16_t pollInterval;
  unsigned long lastPoll;
  uint16_t errorCount;
  uint16_t successCount;
  bool isGrowatt;
  bool isEastron;
};

clientConfig clients[MAX_clientS];
int activeclients = 0;

WebServer server(80);
ModbusTCP mbTCP;
ModbusRTU mbRTU;
Preferences prefs;

unsigned long tcpRequests = 0;
unsigned long rtuRequests = 0;
unsigned long errors = 0;
unsigned long systemErrors = 0;
unsigned long wifiReconnects = 0;

TaskHandle_t modbusTaskHandle;
TaskHandle_t webTaskHandle;
SemaphoreHandle_t dataMutex;

uint16_t holdingRegs[MAX_clientS][100];
uint16_t inputRegs[MAX_clientS][100];

#define WIFI_RECONNECT_INTERVAL 30000
#define MAX_MODBUS_RETRIES 3
#define WIFI_CONNECT_TIMEOUT 20000
unsigned long lastWifiCheck = 0;

// Forward declarations
String getHTMLHeader(String title);
String getHTMLFooter();
String getSDM630Description(uint16_t regAddr);
String getGrowattRegDescription(uint16_t regAddr);
void setupWebServer();
void startAPMode();
void handleCaptivePortal();
void handleWiFiConfig();
void handleWiFiSave();

// ########## SETUP ##########
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32 Modbus Gateway v" + version);

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  dataMutex = xSemaphoreCreateMutex();
  loadConfig();

  // Initialize Modbus RTU
  MODBUS_SERIAL.begin(modbus_baud, SERIAL_8N1, rx_pin, tx_pin);
  mbRTU.begin(&MODBUS_SERIAL);
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
  server.begin();
  Serial.println("Web server started on port 80");

  xTaskCreatePinnedToCore(modbusTask, "Modbus", 10000, NULL, 1, &modbusTaskHandle, 0);
  xTaskCreatePinnedToCore(webTask, "Web", 10000, NULL, 1, &webTaskHandle, 1);

  Serial.println("Ready!");
}

// ########## LOOP ##########
void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

void startAPMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);

  delay(100);

  IPAddress IP = WiFi.softAPIP();
  Serial.println("AP Mode Started");
  Serial.println("SSID: " + String(ap_ssid));
  Serial.println("Password: " + String(ap_password));
  Serial.println("AP IP address: " + IP.toString());

  dnsServer.start(DNS_PORT, "*", IP);
  Serial.println("DNS server started for captive portal");
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname.c_str());
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.println("\nWiFi Connected!");
    Serial.println("IP: " + WiFi.localIP().toString());
    MDNS.begin(hostname.c_str());
    Serial.println("mDNS started: http://" + hostname + ".local");
    return true;
  }

  Serial.println("\nWiFi Connection Failed!");
  return false;
}

void modbusTask(void* parameter) {
  for (;;) {
    mbTCP.task();
    mbRTU.task();

    for (int i = 0; i < activeclients; i++) {
      if (clients[i].enabled && (millis() - clients[i].lastPoll >= clients[i].pollInterval)) {
        clients[i].lastPoll = millis();

        bool success = false;
        for (int retry = 0; retry < MAX_MODBUS_RETRIES && !success; retry++) {
          if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {

          // GROWATT: Use readHreg (Holding Registers) for Growatt, readIreg for others
            if (clients[i].isGrowatt) {
              // Growatt uses Holding Registers (Function 03) for input data
              success = mbRTU.readHreg(clients[i].id, clients[i].startAddress, holdingRegs[i], clients[i].registerCount, nullptr);
              
              if (success) {
                // Copy to Modbus TCP holding registers
                for (int j = 0; j < clients[i].registerCount; j++) {
                  mbTCP.Hreg(clients[i].startAddress + j, holdingRegs[i][j]);
                }
              }
            } else {
              // Eastron and other devices use Input Registers (Function 04)
              success = mbRTU.readIreg(clients[i].id, clients[i].startAddress, inputRegs[i], clients[i].registerCount, nullptr);
              
              if (success) {
                // Copy to Modbus TCP input registers
                for (int j = 0; j < clients[i].registerCount; j++) {
                  mbTCP.Ireg(clients[i].startAddress + j, inputRegs[i][j]);
                }
              }
            }
            //success = mbRTU.readIreg(clients[i].id, clients[i].startAddress, inputRegs[i], clients[i].registerCount, nullptr);

           // if (success) {
            //  for (int j = 0; j < clients[i].registerCount; j++) {
            //    mbTCP.Ireg(clients[i].startAddress + j, inputRegs[i][j]);
            //  }
           // }

            xSemaphoreGive(dataMutex);

            if (success) {
              clients[i].successCount++;
              rtuRequests++;
              tcpRequests++;
            } else if (retry == MAX_MODBUS_RETRIES - 1) {
              clients[i].errorCount++;
              errors++;
            } else {
              vTaskDelay(100 / portTICK_PERIOD_MS);
            }
          }
        }
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void webTask(void* parameter) {
  for (;;) {
    if (apMode) {
      dnsServer.processNextRequest();
    }

    server.handleClient();

    if (!apMode && millis() - lastWifiCheck > WIFI_RECONNECT_INTERVAL) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        wifiReconnects++;
        if (!connectWiFi()) {
          Serial.println("Reconnection failed. Starting AP mode...");
          startAPMode();
        }
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void loadConfig() {
  prefs.begin("modbus-gw", false);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_password = prefs.getString("password", "");
  hostname = prefs.getString("hostname", hostname);
  modbus_baud = prefs.getInt("baud", modbus_baud);
  rx_pin = prefs.getInt("rx_pin", rx_pin);
  tx_pin = prefs.getInt("tx_pin", tx_pin);
  activeclients = prefs.getInt("active_clients", 0);

  for (int i = 0; i < activeclients && i < MAX_clientS; i++) {
    String prefix = "client" + String(i) + "_";
    clients[i].id = prefs.getUChar((prefix + "id").c_str(), 1);
    clients[i].enabled = prefs.getBool((prefix + "en").c_str(), true);
    clients[i].startAddress = prefs.getUShort((prefix + "addr").c_str(), 0);
    clients[i].registerCount = prefs.getUShort((prefix + "count").c_str(), 10);
    clients[i].pollInterval = prefs.getUShort((prefix + "poll").c_str(), 1000);
    clients[i].isGrowatt = prefs.getBool((prefix + "growatt").c_str(), false);
    clients[i].isEastron = prefs.getBool((prefix + "eastron").c_str(), false);
    clients[i].lastPoll = 0;
    clients[i].errorCount = 0;
    clients[i].successCount = 0;
  }

  if (activeclients == 0) {
    clients[0].id = 1;
    clients[0].enabled = true;
    clients[0].startAddress = 0;
    clients[0].registerCount = 10;
    clients[0].pollInterval = 1000;
    clients[0].isGrowatt = false;
    clients[0].isEastron = false;
    clients[0].lastPoll = 0;
    clients[0].errorCount = 0;
    clients[0].successCount = 0;
    activeclients = 1;
  }
  prefs.end();
}

void saveConfig() {
  prefs.begin("modbus-gw", false);
  prefs.putString("ssid", wifi_ssid);
  prefs.putString("password", wifi_password);
  prefs.putString("hostname", hostname);
  prefs.putInt("baud", modbus_baud);
  prefs.putInt("rx_pin", rx_pin);
  prefs.putInt("tx_pin", tx_pin);
  prefs.putInt("active_clients", activeclients);

  for (int i = 0; i < activeclients && i < MAX_clientS; i++) {
    String prefix = "client" + String(i) + "_";
    prefs.putUChar((prefix + "id").c_str(), clients[i].id);
    prefs.putBool((prefix + "en").c_str(), clients[i].enabled);
    prefs.putUShort((prefix + "addr").c_str(), clients[i].startAddress);
    prefs.putUShort((prefix + "count").c_str(), clients[i].registerCount);
    prefs.putUShort((prefix + "poll").c_str(), clients[i].pollInterval);
    prefs.putBool((prefix + "growatt").c_str(), clients[i].isGrowatt);
    prefs.putBool((prefix + "eastron").c_str(), clients[i].isEastron);
  }
  prefs.end();
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
  html += ".form-group input,.form-group select{width:100%;padding:12px;border:2px solid #ddd;border-radius:8px;font-size:16px;box-sizing:border-box;transition:border 0.3s;}";
  html += ".form-group input:focus,.form-group select:focus{outline:none;border-color:#667eea;}";
  html += ".form-group small{display:block;margin-top:5px;color:#666;font-size:13px;}";
  html += ".btn{width:100%;padding:15px;background:#667eea;color:#fff;border:none;border-radius:8px;font-size:18px;font-weight:bold;cursor:pointer;transition:background 0.3s;}";
  html += ".btn:hover{background:#5568d3;}";
  html += ".info-box{background:#f0f4ff;padding:15px;border-radius:8px;margin:20px 0;border-left:4px solid #667eea;}";
  html += ".info-box strong{color:#667eea;}";
  html += ".network-list{background:#f8f9fa;padding:15px;border-radius:8px;margin:15px 0;max-height:200px;overflow-y:auto;}";
  html += ".network-item{padding:10px;margin:5px 0;background:#fff;border-radius:5px;cursor:pointer;transition:background 0.3s;}";
  html += ".network-item:hover{background:#e9ecef;}";
  html += ".signal{float:right;color:#667eea;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>WiFi Configuration</h1>";
  html += "<div class='subtitle'>Modbus Gateway Setup</div>";

  html += "<div class='info-box'>";
  html += "<strong>Welcome!</strong><br>";
  html += "Connect this device to your WiFi network to enable remote access and monitoring.";
  html += "</div>";

  html += "<div class='network-list'>";
  html += "<strong>Available Networks:</strong><br>";
  int n = WiFi.scanNetworks();
  if (n > 0) {
    for (int i = 0; i < n && i < 10; i++) {
      String strength = "";
      int rssi = WiFi.RSSI(i);
      if (rssi > -50) strength = "****";
      else if (rssi > -60) strength = "***";
      else if (rssi > -70) strength = "**";
      else strength = "*";

      html += "<div class='network-item' onclick='selectNetwork(\"" + WiFi.SSID(i) + "\")'>";
      html += WiFi.SSID(i);
      html += "<span class='signal'>" + strength + "</span>";
      html += "</div>";
    }
  } else {
    html += "<div style='color:#666;padding:10px;'>No networks found</div>";
  }
  html += "</div>";

  html += "<form method='POST' action='/wifisave'>";
  html += "<div class='form-group'>";
  html += "<label>Network Name (SSID):</label>";
  html += "<input type='text' id='ssid' name='ssid' placeholder='Enter WiFi SSID' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label>Password:</label>";
  html += "<input type='password' name='password' placeholder='Enter WiFi password' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label>Device Hostname (mDNS):</label>";
  html += "<input type='text' name='hostname' value='" + hostname + "' placeholder='modbus-gateway' pattern='[a-zA-Z0-9-]+' required>";
  html += "<small>Only letters, numbers, and hyphens. Access via http://hostname.local</small>";
  html += "</div>";
  html += "<button type='submit' class='btn'>Connect to WiFi</button>";
  html += "</form>";

  html += "</div>";
  html += "<script>";
  html += "function selectNetwork(ssid) {";
  html += "  document.getElementById('ssid').value = ssid;";
  html += "}";
  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleWiFiSave() {
  if (!server.hasArg("ssid") || !server.hasArg("password") || !server.hasArg("hostname")) {
    server.send(400, "text/html", "<h1>Error: Missing credentials</h1>");
    return;
  }

  wifi_ssid = server.arg("ssid");
  wifi_password = server.arg("password");
  hostname = server.arg("hostname");

  hostname.replace(" ", "-");
  hostname.toLowerCase();

  saveConfig();

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
  html += ".access-info{background:#f0f4ff;padding:15px;border-radius:8px;margin:20px 0;border-left:4px solid #667eea;text-align:left;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Connecting to WiFi</h1>";
  html += "<div class='spinner'></div>";
  html += "<div class='info'>";
  html += "<strong>SSID:</strong> " + wifi_ssid + "<br>";
  html += "<strong>Hostname:</strong> " + hostname + "<br><br>";
  html += "Please wait while the device connects to your network...<br>";
  html += "This page will redirect automatically.<br><br>";
  html += "</div>";
  html += "<div class='access-info'>";
  html += "<strong>Once connected, access via:</strong><br>";
  html += "http://" + hostname + ".local<br>";
  html += "Or check your router for the assigned IP address";
  html += "</div>";
  html += "<div class='info' style='font-size:14px;'>";
  html += "If connection fails, the device will restart in AP mode.";
  html += "</div>";
  html += "</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);

  delay(2000);

  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  if (connectWiFi()) {
    Serial.println("Successfully connected to WiFi!");
    Serial.println("Access device at: http://" + hostname + ".local");
  } else {
    Serial.println("Failed to connect. Restarting...");
    delay(3000);
    ESP.restart();
  }
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
    //html += "<h2>Version " + version + "</h2>";

    if (!apMode) {
      html += "<div class='info-box' style='background:#3d3d3d;border-color:#28a745;margin:20px 0;padding:15px;border-radius:5px;'>";
      html += "<strong style='color:#28a745;'>WiFi Connected</strong><br>";
      html += "Network: " + wifi_ssid + "<br>";
      html += "IP Address: " + WiFi.localIP().toString() + "<br>";
      html += "Hostname: http://" + hostname + ".local";
      html += "</div>";
    }

    html += "<a href='/status' class='btn btn-primary'>Status</a>";
    html += "<a href='/config' class='btn btn-primary'>Configuration</a>";
    html += "<a href='/clients' class='btn btn-primary'>Manage Clients</a>";
    html += "<a href='/debug' class='btn btn-primary'>Debug</a>";
    html += "<a href='/firmware' class='btn btn-primary'>Firmware Update</a>";
   // html += "<a href='/wificonfig' class='btn btn-primary'>WiFi Setup</a>";
    html += "<button onclick='reboot()' class='btn btn-danger'>Reboot</button>";
    html += "<script>";
    html += "function reboot(){if(confirm('Reboot device now?')){fetch('/reboot',{method:'POST'}).then(()=>alert('Rebooting...'));}}";
    html += "</script>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/wificonfig", HTTP_GET, handleCaptivePortal);
  server.on("/wifisave", HTTP_POST, handleWiFiSave);

  server.on("/status", HTTP_GET, []() {
    String html = getHTMLHeader("Gateway Status");
    html += "<div class='container'>";
    html += "<h1>Gateway Status</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";

    html += "<div class='info-box'>";
    html += "<h3>System Information</h3>";
    html += "Hostname: " + hostname + "<br>";
    html += "Version: " + version + "<br>";
    html += "WiFi SSID: " + wifi_ssid + "<br>";
    html += "IP Address: " + WiFi.localIP().toString() + "<br>";
    html += "mDNS: http://" + hostname + ".local<br>";
    html += "Uptime: " + String(millis() / 1000) + " seconds<br>";
    html += "</div>";

    html += "<div class='info-box'>";
    html += "<h3>Statistics</h3>";
    html += "TCP Requests: " + String(tcpRequests) + "<br>";
    html += "RTU Requests: " + String(rtuRequests) + "<br>";
    html += "Errors: " + String(errors) + "<br>";
    html += "System Errors: " + String(systemErrors) + "<br>";
    html += "WiFi Reconnects: " + String(wifiReconnects) + "<br>";
    html += "</div>";

    html += "<div class='info-box'>";
    html += "<h3>Active Clients</h3>";
    for (int i = 0; i < activeclients; i++) {
      html += "Client " + String(i) + " (ID: " + String(clients[i].id) + ")<br>";
      html += "&nbsp;&nbsp;Enabled: " + String(clients[i].enabled ? "Yes" : "No") + "<br>";
      html += "&nbsp;&nbsp;Success: " + String(clients[i].successCount) + "<br>";
      html += "&nbsp;&nbsp;Errors: " + String(clients[i].errorCount) + "<br><br>";
    }
    html += "</div>";

    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/config", HTTP_GET, []() {
    String html = getHTMLHeader("Configuration");
    html += "<div class='container'>";
    html += "<h1>System Configuration</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";

    html += "<form method='POST' action='/config'>";
    html += "<div class='form-group'>";
    html += "<label>Hostname:</label>";
    html += "<input type='text' name='hostname' value='" + hostname + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>Modbus Baud Rate:</label>";
    html += "<input type='number' name='baud' value='" + String(modbus_baud) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>RX Pin:</label>";
    html += "<input type='number' name='rx_pin' value='" + String(rx_pin) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label>TX Pin:</label>";
    html += "<input type='number' name='tx_pin' value='" + String(tx_pin) + "'>";
    html += "</div>";
    html += "<button type='submit' class='btn btn-primary'>Save Configuration</button>";
    html += "</form>";

    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/config", HTTP_POST, []() {
    if (server.hasArg("hostname")) hostname = server.arg("hostname");
    if (server.hasArg("baud")) modbus_baud = server.arg("baud").toInt();
    if (server.hasArg("rx_pin")) rx_pin = server.arg("rx_pin").toInt();
    if (server.hasArg("tx_pin")) tx_pin = server.arg("tx_pin").toInt();

    saveConfig();

    server.sendHeader("Location", "/config");
    server.send(303, "text/plain", "Configuration saved");
  });

  server.on("/clients", HTTP_GET, []() {
    String html = getHTMLHeader("Manage Clients");
    html += "<div class='container'>";
    html += "<h1>Manage Clients</h1>";
        html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<form method='POST' action='/clients'>";

    for (int i = 0; i < MAX_clientS; i++) {
      html += "<div style='background:#3d3d3d;padding:20px;border-radius:5px;margin:15px 0;'>";
      html += "<h3>Client " + String(i + 1) + "</h3>";
      html += "<div class='form-group'>";
      html += "<label class='checkbox-label'><input type='checkbox' name='en" + String(i) + "' " + String(i < activeclients && clients[i].enabled ? "checked" : "") + "> Enabled</label>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label class='checkbox-label'><input type='checkbox' name='growatt" + String(i) + "' " + String(i < activeclients && clients[i].isGrowatt ? "checked" : "") + "> Growatt Inverter</label>";
      html += "<label class='checkbox-label'><input type='checkbox' name='eastron" + String(i) + "' " + String(i < activeclients && clients[i].isEastron ? "checked" : "") + "> Eastron Energy Meter</label>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Client ID:</label>";
      html += "<input type='number' name='id" + String(i) + "' value='" + String(i < activeclients ? clients[i].id : i + 1) + "' min='1' max='247'>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Start Address:</label>";
      html += "<input type='number' name='addr" + String(i) + "' value='" + String(i < activeclients ? clients[i].startAddress : 0) + "' min='0' max='65535'>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Register Count:</label>";
      html += "<input type='number' name='count" + String(i) + "' value='" + String(i < activeclients ? clients[i].registerCount : 10) + "' min='1' max='100'>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label>Poll Interval (ms):</label>";
      html += "<input type='number' name='poll" + String(i) + "' value='" + String(i < activeclients ? clients[i].pollInterval : 1000) + "' min='100' max='60000'>";
      html += "</div>";
      html += "</div>";
    }

    html += "<button type='submit' class='btn btn-primary'>Save</button>";
    html += "</form>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/clients", HTTP_POST, []() {
    activeclients = 0;

    for (int i = 0; i < MAX_clientS; i++) {
      if (server.hasArg("en" + String(i)) || (i == 0 && server.hasArg("id" + String(i)))) {
        clients[activeclients].enabled = server.hasArg("en" + String(i));
        clients[activeclients].isGrowatt = server.hasArg("growatt" + String(i));  // GROWATT: Read checkbox
        clients[activeclients].isEastron = server.hasArg("eastron" + String(i));
        clients[activeclients].id = server.arg("id" + String(i)).toInt();
        clients[activeclients].startAddress = server.arg("addr" + String(i)).toInt();
        clients[activeclients].registerCount = server.arg("count" + String(i)).toInt();
        clients[activeclients].pollInterval = server.arg("poll" + String(i)).toInt();
        clients[activeclients].lastPoll = 0;
        clients[activeclients].errorCount = 0;
        clients[activeclients].successCount = 0;
        activeclients++;
      }
    }

    if (activeclients == 0) {
      clients[0].id = 1;
      clients[0].enabled = true;
      clients[0].startAddress = 0;
      clients[0].registerCount = 10;
      clients[0].pollInterval = 1000;
      clients[0].isGrowatt = false;  // GROWATT: Initialize
      clients[0].isEastron = false;
      clients[0].lastPoll = 0;
      clients[0].errorCount = 0;
      clients[0].successCount = 0;
      activeclients = 1;
    }

    saveConfig();

    String html = getHTMLHeader("Saved");
    html += "<div class='container'>";
    html += "<h1>Clients Saved</h1>";
    html += "<p>" + String(activeclients) + " client(s) configured.</p>";
    html += "<a href='/clients' class='btn btn-primary'>Back to Clients</a>";
    html += "<a href='/' class='btn btn-primary'>Back to Main</a>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/debug", HTTP_GET, []() {
    String html = getHTMLHeader("Debug");
    html += "<div class='container'>";

    html += "<h1>Debug Information</h1>";
    html += "<h3>Modbus Data (Live) <button onclick='location.reload()' class='btn btn-primary' style='font-size:12px;padding:5px 10px;'>Refresh</button></h3>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<div id='modbus-data'>";

    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < activeclients; i++) {
        if (clients[i].enabled) {
          html += "<h4> " ;
          html += "<h4>Client " + String(clients[i].id);

          if (clients[i].isGrowatt) html += " (Growatt Inverter)";
          if (clients[i].isEastron) html += " (Eastron Energy Meter)";
          html += "</h4>";
          html += "<table style='width:100%;border-collapse:collapse;font-size:12px;'>";
          html += "<tr style='background:#4d4d4d;'>";
          html += "<th style='padding:6px;border:1px solid #555;'>Reg</th>";
          html += "<th style='padding:6px;border:1px solid #555;'>Hex</th>";
          html += "<th style='padding:6px;border:1px solid #555;'>Value</th>";
          html += "<th style='padding:6px;border:1px solid #555;'>Description</th>";
          html += "</tr>";

          // Show data differently based on device type
          if (clients[i].isGrowatt) {
            // Growatt: show individual registers
            for (int j = 0; j < clients[i].registerCount; j++) {
              uint16_t reg1 = holdingRegs[i][j];
              uint16_t reg2 = holdingRegs[i][j + 1];

              uint32_t combined;
              uint8_t* bytes = (uint8_t*)&combined;
              bytes[3] = (reg1 >> 8) & 0xFF;
              bytes[2] = reg1 & 0xFF;
              bytes[1] = (reg2 >> 8) & 0xFF;
              bytes[0] = reg2 & 0xFF;

              float value;
              memcpy(&value, &combined, sizeof(float));

              char hexStr[20];
              sprintf(hexStr, "0x%04X 0x%04X", reg1, reg2);

              html += "<tr style='background:#3d3d3d;'>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(clients[i].startAddress + j) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;font-family:monospace;'>" + String(hexStr) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(value, 2) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;'>" + getGrowattRegDescription(clients[i].startAddress + j) + "</td>";
              html += "</tr>";
            }
          } else if (clients[i].isEastron) {

            // Generic device: show as float pairs (SDM630 style)
            for (int j = 0; j < clients[i].registerCount - 1; j += 2) {
              uint16_t reg1 = inputRegs[i][j];
              uint16_t reg2 = inputRegs[i][j + 1];

              uint32_t combined;
              uint8_t* bytes = (uint8_t*)&combined;
              bytes[3] = (reg1 >> 8) & 0xFF;
              bytes[2] = reg1 & 0xFF;
              bytes[1] = (reg2 >> 8) & 0xFF;
              bytes[0] = reg2 & 0xFF;

              float value;
              memcpy(&value, &combined, sizeof(float));

              String desc = getSDM630Description(clients[i].startAddress + j);
              char hexStr[20];
              sprintf(hexStr, "0x%04X 0x%04X", reg1, reg2);

              html += "<tr style='background:#3d3d3d;'>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(clients[i].startAddress + j) + "-" + String(clients[i].startAddress + j + 1) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;font-family:monospace;'>" + String(hexStr) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(value, 2) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;'>" + desc + "</td>";
              html += "</tr>";
            }

          } else {

            // Generic device: show as float pairs (SDM630 style)
            for (int j = 0; j < clients[i].registerCount - 1; j += 2) {
              uint16_t reg1 = inputRegs[i][j];
              uint16_t reg2 = inputRegs[i][j + 1];

              uint32_t combined;
              uint8_t* bytes = (uint8_t*)&combined;
              bytes[3] = (reg1 >> 8) & 0xFF;
              bytes[2] = reg1 & 0xFF;
              bytes[1] = (reg2 >> 8) & 0xFF;
              bytes[0] = reg2 & 0xFF;

              float value;
              memcpy(&value, &combined, sizeof(float));

              String desc = "Register " + String(clients[i].startAddress + j);
              char hexStr[20];
              sprintf(hexStr, "0x%04X 0x%04X", reg1, reg2);

              html += "<tr style='background:#3d3d3d;'>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(clients[i].startAddress + j) + "-" + String(clients[i].startAddress + j + 1) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;font-family:monospace;'>" + String(hexStr) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;text-align:center;'>" + String(value, 2) + "</td>";
              html += "<td style='padding:6px;border:1px solid #555;'>" + desc + "</td>";
              html += "</tr>";
            }
          }
          html += "</table>";
        }
      }
      xSemaphoreGive(dataMutex);
    }

    html += "<br><a href='/' class='btn btn-secondary'>Back</a>";
    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on("/firmware", HTTP_GET, []() {
    String html = getHTMLHeader("Firmware Update");
    html += "<div class='container'>";
    html += "<h1>Firmware Update</h1>";
    html += "<a href='/' class='btn btn-primary'>Back</a>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<div class='form-group'>";
    html += "<label>Select Firmware File (.bin):</label>";
    html += "<input type='file' name='update' accept='.bin' required>";
    html += "</div>";
    html += "<button type='submit' class='btn btn-primary'>Upload Firmware</button>";
    html += "</form>";

    html += "<div class='info-box' style='background:#3d3d3d;border-color:#ffc107;margin-top:20px;'>";
    html += "<strong style='color:#856404;'>Warning:</strong><br>";
    html += "Do not power off the device during firmware update!";
    html += "</div>";

    html += "</div>";
    html += getHTMLFooter();
    server.send(200, "text/html", html);
  });

  server.on(
    "/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
      }
    });

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Rebooting...");
    saveConfig();
    delay(1000);
    ESP.restart();
  });

  server.onNotFound([]() {
    if (apMode) {
      handleCaptivePortal();
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);
}

// GROWATT: Helper function for register descriptions
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


String getHTMLHeader(String title) {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + title + "</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#1a1a1a;color:#fff;}";
  html += ".container{max-width:900px;margin:0 auto;background:#2d2d2d;padding:30px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.3);}";
  html += "h1{color:#4CAF50;margin-top:0;}";
  html += "h2{color:#4CAF50;}";
  html += "h3{color:#4CAF50;}";
  html += "h4{color:#4CAF50;margin-top:0;}";
  html += ".btn{display:inline-block;padding:15px 30px;margin:10px 5px;text-decoration:none;border-radius:5px;font-size:16px;border:none;cursor:pointer;transition:all 0.3s;}";
  html += ".btn-primary{background:#2196F3;color:#fff;}";
  html += ".btn-danger{background:#f44336;color:#fff;}";
  html += ".info-box{background:#3d3d3d;padding:15px;border-radius:5px;margin:15px 0;}";
  html += ".form-group{margin:15px 0;}";
  html += ".form-group label{display:block;margin-bottom:5px;color:#4CAF50;font-weight:bold;}";
  html += ".form-group input,.form-group select{width:100%;padding:10px;border:1px solid #555;background:#3d3d3d;color:#fff;border-radius:5px;box-sizing:border-box;}";
  html += ".form-group input:focus,.form-group select:focus{outline:none;border-color:#4CAF50;}";
  html += ".form-group input[type='checkbox']{width:auto;margin-right:8px;cursor:pointer;}";
  html += ".checkbox-label{display:inline-flex;align-items:center;color:#fff;font-weight:normal;margin-right:20px;cursor:pointer;}";
  html += "</style>";
  html += "</head><body>";
  return html;
}

String getHTMLFooter() {
  return "</body></html>";
}


float getSDM630Float(int clientIndex, uint16_t registerAddress) {
  if (clientIndex >= activeclients) return 0.0;

  if (registerAddress < clients[clientIndex].startAddress || registerAddress + 1 >= clients[clientIndex].startAddress + clients[clientIndex].registerCount) {
    return 0.0;
  }

  int regIndex = registerAddress - clients[clientIndex].startAddress;

  uint16_t reg1 = inputRegs[clientIndex][regIndex];
  uint16_t reg2 = inputRegs[clientIndex][regIndex + 1];

  uint32_t combined;
  uint8_t* bytes = (uint8_t*)&combined;
  bytes[3] = (reg1 >> 8) & 0xFF;
  bytes[2] = reg1 & 0xFF;
  bytes[1] = (reg2 >> 8) & 0xFF;
  bytes[0] = reg2 & 0xFF;

  float value;
  memcpy(&value, &combined, sizeof(float));

  return value;
}

float getGrowattFloat(int clientIndex, uint16_t registerAddress) {
  int regIndex = registerAddress - clients[clientIndex].startAddress;

  uint16_t reg1 = holdingRegs[clientIndex][regIndex];
  uint16_t reg2 = holdingRegs[clientIndex][regIndex + 1];

  uint32_t combined;
  uint8_t* bytes = (uint8_t*)&combined;
  bytes[3] = (reg1 >> 8) & 0xFF;
  bytes[2] = reg1 & 0xFF;
  bytes[1] = (reg2 >> 8) & 0xFF;
  bytes[0] = reg2 & 0xFF;

  float value;
  memcpy(&value, &combined, sizeof(float));

  return value;
}
