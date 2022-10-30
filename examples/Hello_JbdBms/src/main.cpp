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
  else {
    Serial.println("jbdbms.getStatus() failed");
  }

  delay(10000);
}
