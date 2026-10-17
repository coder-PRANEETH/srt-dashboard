/*
Node 4 TCU - BENCH TEST. Open Serial Monitor at 115200 baud.
Connections:
  GPS TX -> ESP32 RX2 GPIO16 (9600 baud, 3.3 V TTL); common GND.
  GPS RX is not used; check GPS module power against its own rating.
  Ignition sense -> GPIO34 only THROUGH a divider/isolator that limits
  the input to 0-3.3 V. GPIO34 has no internal pull-up/down; give it a
  defined external LOW when ignition is off. Never connect 12 V directly.
  MPU6050 SDA -> GPIO21, SCL -> GPIO22, VCC -> compatible 3.3 V, GND.
Verification:
  GPS bytes should rise with a powered GPS; valid fix and satellite count
  may require sky view. Check checksum failures if bytes rise without fix.
  Toggle conditioned ignition: printed GPIO34 reading should change 0/1.
  Tilt MPU6050 longitudinally: ax and pitch should change; verify that
  positive/negative pitch matches the module's physical mounting.
  Local CAN loopback sent/pass counts should rise without another node.
  This checks MCP2515 SPI/RX/TX locally, not the external CAN bus wiring.

Local CAN controller connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT; MCP2515 GND -> ESP32 GND.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  Internal MCP2515 loopback needs no CANH/CANL bus, termination or peer.
  It tests SPI/controller TX/RX, not the external transceiver or bus.
*/

#include "CanCommon.h"
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>

constexpr uint32_t LOCAL_CAN_ID = 0x7A4;
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
Adafruit_MPU6050 imu;
bool imuReady = false;
uint32_t gpsBytes = 0, lastReport = 0, canSent = 0, canPassed = 0;
uint8_t canSequence = 0;

void setup() {
  Serial.begin(115200);
  pinMode(34, INPUT);
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);
  Wire.begin(21, 22);
  imuReady = imu.begin();
  if (imuReady) {
    imu.setAccelerometerRange(MPU6050_RANGE_8_G);
    imu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }
  Serial.printf("Node 4 local MCP2515 CAN loopback: %s\n",
                canBegin(true) ? "READY" : "FAILED");
  Serial.printf("MPU6050 on GPIO21/22: %s\n", imuReady ? "READY" : "FAILED");
  Serial.println("GPS TX -> GPIO16; ignition logic -> GPIO34; no other CAN node needed");
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
    ++gpsBytes;
  }
  uint32_t id;
  uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == LOCAL_CAN_ID && len == 8 && data[0] == 4 && data[2] == 0xA5)
      ++canPassed;
  }
  if (millis() - lastReport >= 1000) {
    lastReport = millis();
    const uint8_t probe[8] = {4, canSequence++, 0xA5, 0, 0, 0, 0, 0};
    if (canSend(LOCAL_CAN_ID, probe)) ++canSent;
    Serial.printf("GPS bytes=%lu checksum errors=%lu sats=%lu fix=%u age=%lu | ignition GPIO34=%d\n",
                  (unsigned long)gpsBytes, (unsigned long)gps.failedChecksum(),
                  (unsigned long)(gps.satellites.isValid() ? gps.satellites.value() : 0),
                  (unsigned)gps.location.isValid(), (unsigned long)gps.location.age(), digitalRead(34));
    if (!imuReady) {
      Serial.println("MPU6050 missing; check GPIO21 SDA, GPIO22 SCL, power and GND");
      imuReady = imu.begin();
    } else {
      sensors_event_t a, g, t;
      if (imu.getEvent(&a, &g, &t)) {
        const float transverse = sqrtf(a.acceleration.y * a.acceleration.y +
                                       a.acceleration.z * a.acceleration.z);
        const float pitch = atan2f(-a.acceleration.x, transverse) * 180.0f / PI;
        Serial.printf("MPU6050 ax=%.2f ay=%.2f az=%.2f pitch=%.1f deg\n",
                      a.acceleration.x, a.acceleration.y, a.acceleration.z, pitch);
      } else {
        Serial.println("MPU6050 read failed");
        imuReady = false;
      }
    }
    Serial.printf("CAN local loopback sent=%lu pass=%lu\n",
                  (unsigned long)canSent, (unsigned long)canPassed);
  }
}
