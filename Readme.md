# Joba_JbdBms

Talk to a Jiabaida Battery Management System over RS485. Tested with an ESP32 and a JBD-SP04S010A. Might also work with UART.

* See more on JBD commands and wiring in include/jbdbms.h
* See usage in examples/ directory
    * Hello: print battery voltage to demonstrate JbdBms usage
    * Test: uses all functions and prints results to check functionality
    * Monitor: regularly check values of the device and report changes (on serial, syslog and influx db). 
      Also provide values as json and allow toggling mosfet status for charging and discharging on a simple web interface. 
      ```
      > influx -precision rfc3339 --database JbdBms --execute "select * from Status where time > '2022-10-24T07:32:26Z' and time < '2022-10-24T07:33:55Z' order by time"
      name: Status
      TODO provide real example...
      ```
* Init the serial port with 9600 baud (TODO check 8N1) before calling JbdBms class methods.
    * ESP32 HardwareSerial, default pins
      ```c
      Serial2.begin(9600, SERIAL_8N1);
      ```
    * ESP8266 SoftwareSerial
      ```c
      SoftwareSerial port;
      ... 
      port.begin(9600, SWSERIAL_8N1, rx_pin, tx_pin);
      ```
    * ESP8266 hardware serial with alternate rx/tx pins 13/15 (not tested)
      ```c
      Serial.begin(9600); Serial.swap();
      ```
* Complete example
   * Get Status
   ```c
   #include <Arduino.h>
   #include <jbdbms.h>

   #define RS485_DIR_PIN 22             // Explicit DE/!RE pin, -1 if board does automatic direction

   JbdBms jbdbms(Serial2);              // Uses ESP32 2nd serial port to communicate with RS485 (or UART?) adapter

   void setup() {
      Serial.begin(115200);
      Serial2.begin(9600, SERIAL_8N1);  // Init serial port with default pins 16 and 17 for RX and TX
      jbdbms.begin(RS485_DIR_PIN);      // Init RS485 communication
   }

   void loop() {
      JbdBms::Status_t status;
      
      if (jbdbms.getStatus(status)) {   // Get current global status
         Serial.printf("Voltage: %u\n", status.voltage);
      }

      delay(10000);
   }
   ```

Thank you Jaibaida for providing the relevant protocol information!

Comments welcome,
Joachim Banzhaf
