#include <HardwareSerial.h>

HardwareSerial LINK(1);

#define LINK_RX 16
#define LINK_TX 17

void setup() {
  Serial.begin(115200);

  // RX = GPIO16, TX unused
  LINK.begin(115200, SERIAL_8N1, LINK_RX, -1);

  Serial.println("ESP32 #1 GPS Receiver Started");
}

void loop() {
  while (LINK.available()) {
    char c = LINK.read();

    Serial.write(c);
  }
}