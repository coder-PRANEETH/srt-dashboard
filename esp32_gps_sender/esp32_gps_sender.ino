#include <TinyGPSPlus.h>
#include <HardwareSerial.h>

TinyGPSPlus gps;

HardwareSerial GPS(2);   // GPS UART
HardwareSerial LINK(1);  // ESP32 #1 communication

#define GPS_RX 16
#define LINK_TX 17

/* Match the dashboard's high-rate GPS transport. */
#define GPS_PUBLISH_INTERVAL_MS 5

void setup() {
  Serial.begin(115200);

  // GPS: only RX, no TX
  GPS.begin(9600, SERIAL_8N1, GPS_RX, -1);

  // Send GPS information to ESP32 #1 through GPIO17
  LINK.begin(115200, SERIAL_8N1, -1, LINK_TX);

  Serial.println("GPS transmitter started");
}

void loop() {

  // Read GPS data
  while (GPS.available()) {
    char c = GPS.read();

    Serial.write(c);
    gps.encode(c);
  }

  // Transmit the most recent parsed GPS data every 5 ms.
  static unsigned long lastSend = 0;

  if (millis() - lastSend >= GPS_PUBLISH_INTERVAL_MS) {
    lastSend = millis();

    bool fix = gps.location.isValid();

    LINK.print("{\"lat\":");
    LINK.print(fix ? gps.location.lat() : 0.0, 6);

    LINK.print(",\"lon\":");
    LINK.print(fix ? gps.location.lng() : 0.0, 6);

    LINK.print(",\"sats\":");
    LINK.print(
      gps.satellites.isValid()
      ? gps.satellites.value()
      : 0
    );

    LINK.print(",\"fix\":");
    LINK.print(fix ? "true" : "false");

    LINK.println("}");
  }
}
