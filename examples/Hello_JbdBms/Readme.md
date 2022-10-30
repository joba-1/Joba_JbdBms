# ESP32 Arduino Firmware to get the status of a Jaibaida BMS with the Joba_JbdBms Library

Basic "hello world" example for the JBD BMS

# Installation
There are many options to compile and install an ESP32 Arduino firmware. I use this one on linux:
* Install MS Code
 * Install PlatformIO as MS Code extension
* Add this folder to the MS Code workspace
* Edit platformio.ini in this folder so the usb device name for upload matches your environment
* Select build and upload the firmware

# Connection
See library readme for wiring details. 

Required hardware:
* A MAX485 board (or similar) to convert RS485 signals to serial.
* A level shifter from 5V to 3.3V (at least 3 channels) if the MAX485 board does not already have one
* An ESP32 (with USB connection or an additional serial-to-USB adapter for flashing)
* JBD-SP04S010A (or similar) battery management controller
* LiFePO battery with number of cells as supported by the BMS
* Some charger (matching battery voltage)

# Result
The battery voltage should be printed on serial every 10s.


Comments welcome

Joachim Banzhaf
