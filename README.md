DIY Project "ESP32 Modbus TCP gateway" is to connect Modbus devices like energy meters 
or Solar Inverters to network devices (home servers) over TCP/IP.



**Software:**
- Arduino IDE code for Espessife devices (in this repositorie)

**Hardware:**
- ESP32 C dev. board (or similar)
- MAX438 board (or similar board for Modbus to serial communication)

**How to build:**
1. connect the 4 points from MAX438 board to ESP32 hardware according the drawing.
   Vcc - Vcc
   Gnd - Gnd
   Tx - Tx (pin 17)
   Rx - Rx (pin 16)

2. Connect A and B from the modbus device 
   (ground or resistor not required for short distance cable)

3. Connect USB with computer and download the programm

4. Test / use the modbus gateway :-)
   
