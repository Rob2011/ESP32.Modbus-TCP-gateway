# ESP32 Modbus RTU to TCP/IP Gateway

A professional-grade Modbus gateway that bridges RS-485 Modbus RTU devices to Ethernet/WiFi Modbus TCP networks. Features a comprehensive web interface, REST API, and support for multiple device profiles including solar inverters and energy meters.

![Version](https://img.shields.io/badge/version-5.5-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)

## 🌟 Features

### Core Functionality
- **Bidirectional Protocol Conversion**: Seamless translation between Modbus RTU (RS-485) and Modbus TCP/IP
- **Multi-Client Support**: Connect up to 3 Modbus RTU devices simultaneously
- **Real-time Data Polling**: Configurable polling intervals (100ms - 60s) per device
- **Thread-Safe Operation**: Dual-core task management with mutex protection

### Device Profiles
Pre-configured profiles with optimized register mappings:
- **Growatt Inverters** - Complete status monitoring with float decoding
- **Eastron SDM630** - Three-phase energy meter support
- **SolarEdge Inverters** - SunSpec compliant register layout
- **SMA Inverters** - Power and yield monitoring
- **Fronius Inverters** - Comprehensive AC/DC measurements
- **Huawei Inverters** - Grid measurements and status monitoring
- **Generic Device** - Customizable for any Modbus device

### Web Interface
- **Responsive Design**: Mobile-friendly interface with dark theme
- **Live Dashboard**: Real-time status monitoring with auto-refresh
- **Configuration Management**: Easy setup of WiFi, Modbus, and client parameters
- **Data Preview**: Live register values with human-readable descriptions
- **Statistics**: Success rates, error counts, and performance metrics
- **OTA Updates**: Over-the-air firmware updates via web interface

### REST API
Complete JSON API for integration and automation:
```
GET  /api/status              - System status and statistics
GET  /api/clients             - List all configured clients
GET  /api/client/{id}/data    - Client data with decoded values
GET  /api/devices             - Available device profiles
GET  /api/device/{id}/presets - Register presets for device
GET  /api/registers?start=0&count=10 - Read specific registers
GET  /api/test?id=1&addr=0    - Test Modbus connection
```

### Network Features
- **WiFi Station Mode**: Connect to existing WiFi networks
- **Access Point Mode**: Built-in configuration portal with captive portal
- **Auto-Reconnect**: Automatic WiFi recovery with exponential backoff
- **mDNS Support**: Access via `http://modbus.local` (configurable hostname)
- **Static Configuration**: Persistent settings stored in flash memory

## 📋 Requirements

### Hardware
- **ESP32 Development Board** (ESP32-WROOM-32 or compatible)
- **RS-485 to TTL Converter** (MAX485, MAX3485, or similar)
- **Power Supply**: 5V USB or external power
- **Modbus RTU Devices**: Any standard Modbus RTU slave device(s)

<p align="center"><img width="800px" alt="Hardware setup" class="recess" src="./images/schema.png" /></p>


### Wiring Diagram
```
ESP32          RS-485 Module      Modbus Device
GPIO16 (RX) -> RO                       
GPIO17 (TX) -> DI                       
GND         -> GND         ->    GND
            -> A           ->    A/D+
            -> B           ->    B/D-
```

### Software
- **Arduino IDE** 1.8.x or 2.x
- **ESP32 Board Package** (via Board Manager)

### Required Libraries
Install via Arduino Library Manager:
- `WiFi` (built-in)
- `WebServer` (built-in)
- `DNSServer` (built-in)
- `Preferences` (built-in)
- `Update` (built-in)
- `ESPmDNS` (built-in)
- `ModbusRTU` by Alexander Emelianov
- `ModbusTCP` by Alexander Emelianov
- `ArduinoJson` by Benoit Blanchon (v6.x)

## 🚀 Quick Start

### 1. Hardware Setup
1. Connect RS-485 module to ESP32 as shown in wiring diagram
2. Connect RS-485 A/B lines to your Modbus device(s)
3. Ensure proper termination resistors (120Ω) on RS-485 bus if required
4. Power the ESP32 via USB

### 2. Software Installation
1. Install Arduino IDE and ESP32 board package
2. Install required libraries listed above
3. Download `modbus_gateway.ino` from this repository
4. Open in Arduino IDE
5. Configure settings if needed:
   ```cpp
   #define HOSTNAME_DEFAULT "modbus2"  // Change default hostname
   #define RX_PIN_DEFAULT 16            // Change RX pin if needed
   #define TX_PIN_DEFAULT 17            // Change TX pin if needed
   ```
6. Select your ESP32 board and port
7. Click Upload

### 3. Initial Configuration
1. After first boot, ESP32 creates WiFi access point: `ModbusGateway-Config`
2. Connect to this network (password: `modbus123`)
3. Web portal opens automatically (or navigate to `192.168.4.1`)
4. Enter your WiFi credentials and hostname
5. Device connects to WiFi and shows IP address in Serial Monitor

### 4. Configure Modbus Clients
1. Access web interface at device IP or `http://modbus.local`
2. Go to **Manage Clients**
3. For each Modbus device:
   - Enable the client
   - Select device type from profiles
   - Set Modbus slave ID (1-247)
   - Choose register preset or enter custom start address
   - Set register count
   - Configure poll interval (recommended: 1000ms)
4. Click **Save All Clients**

### 5. Verify Operation
1. Go to **Preview** page to see live register values
2. Check **Status** page for connection statistics
3. Use REST API for programmatic access

## 📖 Configuration Guide

### WiFi Settings
- **SSID**: Your WiFi network name
- **Password**: WiFi password (8-63 characters)
- **Hostname**: mDNS hostname (letters, numbers, hyphens only)

### Modbus Settings
- **Baud Rate**: 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
- **Default**: 9600 (most common for Modbus RTU)
- **RX Pin**: GPIO pin for RS-485 receive (default: 16)
- **TX Pin**: GPIO pin for RS-485 transmit (default: 17)

### Client Configuration
For each Modbus slave device:

| Parameter | Description | Range |
|-----------|-------------|-------|
| Client ID | Modbus slave address | 1-247 |
| Device Type | Pre-configured profile | See device profiles |
| Start Address | First register to read | 0-65535 |
| Register Count | Number of registers | 1-122 |
| Poll Interval | Milliseconds between reads | 100-60000 |

### Device Profile Selection
Choose the appropriate profile for optimal register decoding:

- **Growatt Inverter**: For Growatt solar inverters (byte/word swapping enabled)
- **Eastron SDM630**: For SDM630 energy meters (word swapping enabled)
- **SolarEdge**: For SolarEdge inverters (SunSpec protocol)
- **Generic Device**: For any Modbus device (basic 16-bit registers)

Each profile includes:
- Optimal byte order configuration
- Float32 decoding where applicable
- Human-readable register descriptions
- Pre-configured register presets

## 🔧 Advanced Features

### Register Presets
Device profiles include common register ranges:
```cpp
// Example: Growatt Inverter presets
- Complete Status (0-119): All inverter data
- PV Input Data (1-10): Solar panel information
- AC Output Data (35-54): Grid connection data
- Energy Counters (53-56): Production statistics
```

### Float32 Decoding
The gateway automatically decodes 32-bit float values from register pairs:
- Configurable byte order (big/little endian)
- Configurable word order
- Automatic conversion for device profiles

### Statistics & Monitoring
Track gateway performance:
- **Success Rate**: Percentage of successful Modbus reads
- **Error Count**: Failed communication attempts
- **Request Counters**: TCP and RTU request statistics
- **WiFi Reconnects**: Network stability indicator

### OTA Firmware Updates
Update firmware without USB cable:
1. Go to **Firmware** page
2. Select `.bin` file compiled for ESP32
3. Upload and wait for automatic reboot
4. ⚠️ **Never disconnect power during update!**

## 📡 REST API Examples

### Get System Status
```bash
curl http://modbus.local/api/status
```
Response:
```json
{
  "version": "5.5",
  "hostname": "modbus2",
  "uptime": 3600,
  "wifi_ssid": "MyNetwork",
  "wifi_rssi": -45,
  "ip_address": "192.168.1.100",
  "free_heap": 234560,
  "tcp_requests": 1250,
  "rtu_requests": 1250,
  "errors": 3
}
```

### Get Client Data
```bash
curl http://modbus.local/api/client/0/data
```
Response:
```json
{
  "client_id": 1,
  "device_type": 1,
  "device_name": "Growatt Inverter",
  "registers": [
    {
      "address": 0,
      "value": 245.50,
      "description": "PV1 Voltage"
    }
  ]
}
```

### Test Modbus Connection
```bash
curl "http://modbus.local/api/test?id=1&addr=0&count=5"
```

## 🛠️ Troubleshooting

### WiFi Connection Issues
**Problem**: Device doesn't connect to WiFi
- ✅ Verify SSID and password are correct
- ✅ Check WiFi signal strength (stay within range)
- ✅ Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
- ✅ Check Serial Monitor for connection attempts
- ✅ Reset to AP mode via web interface if needed

### Modbus Communication Errors
**Problem**: High error count or no data
- ✅ Verify RS-485 wiring (A to A, B to B)
- ✅ Check baud rate matches slave device
- ✅ Confirm correct Modbus slave ID
- ✅ Test with lower poll interval (2000-5000ms)
- ✅ Ensure only one master on RS-485 bus
- ✅ Add 120Ω termination resistors if needed
- ✅ Check register addresses are valid for device

### Register Values Show Zero
**Problem**: Reads succeed but values are zero
- ✅ Try different register start address
- ✅ Switch between holding/input registers
- ✅ Disable float decoding for raw values
- ✅ Consult device's Modbus register map

### Web Interface Not Loading
**Problem**: Can't access web interface
- ✅ Check IP address in Serial Monitor
- ✅ Try `http://[ip-address]` instead of mDNS
- ✅ Verify device and computer on same network
- ✅ Clear browser cache
- ✅ Try different browser

## 🔍 Serial Monitor Output
Connect at **115200 baud** to see diagnostic information:
```
=== ESP32 Modbus Gateway v5.5 ===
Loaded configuration:
  SSID: MyNetwork
  Hostname: modbus2
  Active clients: 1
Connecting to WiFi...
WiFi Connected!
  IP: 192.168.1.100
  mDNS: http://modbus2.local
Modbus TCP server started on port 502
Web server started on port 80
=== System Ready ===
```

## 📊 Performance Specifications

| Metric | Value |
|--------|-------|
| Maximum Clients | 3 simultaneous |
| Maximum Registers per Client | 122 |
| Minimum Poll Interval | 100ms |
| Maximum Poll Interval | 60s |
| Modbus Response Timeout | 2s |
| Maximum Retries | 3 attempts |
| WiFi Reconnect Interval | 30s |
| Supported Baud Rates | 1200-115200 |

## 🤝 Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for:
- Additional device profiles
- Bug fixes
- Feature enhancements
- Documentation improvements

## 📝 License

This project is licensed under the MIT License - see below for details:

```
MIT License

Copyright (c) 2025 Rob Hagemann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 🙏 Acknowledgments

- **ModbusRTU/TCP Libraries** by Alexander Emelianov
- **ArduinoJson** by Benoit Blanchon
- **ESP32 Arduino Core** by Espressif Systems
- Community contributors and testers

## 📞 Support

- **Issues**: Use GitHub Issues for bug reports
- **Discussions**: Use GitHub Discussions for questions
- **Email**: Create an issue for contact information

## 🔄 Changelog

### Version 5.5 (Current)
- ✨ Added enhanced status page with detailed statistics
- ✨ Added statistics reset functionality
- ✨ Added error logging page
- ✨ Added Modbus connection test API
- ✨ Improved WiFi configuration saving
- ✨ Enhanced status monitoring with success rates
- 🐛 Fixed configuration persistence issues
- 🐛 Fixed WiFi reconnection logic
- 📚 Improved documentation

### Version 5.4
- ✨ Added device profile system
- ✨ Added register presets
- ✨ Added REST API
- 🐛 Fixed configuration validation


**Made with ❤️ for the DIY community**

⭐ Star this repo if you find it useful!
