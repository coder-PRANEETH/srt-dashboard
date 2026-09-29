/* GPS-only diagnostic for ESP32 #2. Use the same UART settings as the sender.
 * GPS TX -> GPIO16, GPS GND -> ESP32 GND; Serial Monitor: 115200 baud. */
#include <TinyGPSPlus.h>

#define GPS_RX 16
#define GPS_TX -1
#define GPS_BAUD 9600

HardwareSerial GPS(2);
TinyGPSPlus gps;
unsigned long lastReport = 0;

void setup() {
  Serial.begin(115200);
  GPS.setRxBufferSize(2048);
  GPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("ESP32 GPS diagnostic started");
}

void loop() {
  while (GPS.available()) gps.encode(GPS.read());

  if (millis() - lastReport < 1000) return;
  lastReport = millis();

  Serial.print("chars: ");
  Serial.print(gps.charsProcessed());
  Serial.print("  checksum errors: ");
  Serial.print(gps.failedChecksum());

  if (!gps.charsProcessed()) {
    Serial.println("  NO DATA (check TX/GND and GPS baud)");
    return;
  }

  const bool fix = gps.location.isValid() && gps.location.age() < 5000;
  Serial.print("  FIX: ");
  Serial.print(fix ? "YES" : "NO");
  Serial.print("  satellites: ");
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);

  if (fix) {
    Serial.print("  lat: ");
    Serial.print(gps.location.lat(), 6);
    Serial.print("  lon: ");
    Serial.print(gps.location.lng(), 6);
    Serial.print("  speed km/h: ");
    Serial.print(gps.speed.kmph(), 1);
    Serial.print("  HDOP: ");
    Serial.print(gps.hdop.hdop(), 2);
  }
  Serial.println();
}
