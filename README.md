DIY Project "ESP32 Modbus TCP gateway" to connect Modbus devices like Energy Meters 
or Solar Inverters to network devices (home servers) over TCP/IP.

**Software:**
- Arduino IDE code for Espessife devices (in this repositorie)

**Features:**
- Supported & tested devices (at the moment): Eastron SDM630
- Start with Access Point for easy configuraion of WiFi settings and connection to own network.
- Load balancing on dual core CPU (for responsive webportal and uninterrupted modbus communication)
- Support mNDS for easy to find on local network with "http://modbus.local"
- Remote firmware update
- Configurable multiple Modbus clients
- Debug page for easy data check
- Error Recovery
  - Save settings for easy recovery
  - WiFi Auto-Reconnect: Checks every 30 seconds and reconnects automatically
  - Modbus Retry Logic: 3 retries with 100ms delay for failed operations
  - Statistics Tracking: Monitors WiFi reconnects, system errors, per-slave errors

  ![Schema](images/main.png)
  ![Schema](images/configuration.png)
  ![Schema](images/debug.png)

**Hardware:**
- ESP32 C dev. board (or similar)
- MAX438 board (or similar board for Modbus to serial communication)

![Schema](images/schema.png)

**How to build:**
1. connect the 4 points from MAX438 board to ESP32 hardware according the drawing.
- Vcc - Vcc
- Gnd - Gnd
- Tx - Tx (pin 17)
- Rx - Rx (pin 16)

2. Connect A and B from the modbus device 
   (ground or resistor not required for short distance cable)

3. Connect USB with computer and download the programm

4. Test / use the modbus gateway :-)
   
