/*
Node 4 TCU - PRODUCTION. Serial diagnostics are at 115200 baud.
Connections:
  GPS TX -> ESP32 RX2 GPIO16, 9600 baud, 3.3 V TTL; common GND.
  GPS RX is unused. Power GPS according to its module specification.
  Condition 12 V ignition through a divider/isolator into GPIO34 at
  0-3.3 V; external pull-down is needed for a defined off state.
  MPU6050 SDA -> GPIO21, SCL -> GPIO22, compatible 3.3 V and GND.
  Mount its X axis longitudinally for the pitch calculation used here.
Verification:
  Node 5 CAN log should show 0x400 GPS, 0x401 ignition, 0x500 hill/pitch,
  and Node 4 heartbeat 0x600. Tilt the sensor and check pitch direction.
  GPIO34 HIGH means ignition ON; check this after input conditioning.
  A 0x300 hill switch request changes 0x500 request byte, but hold
  throttle stays zero. GPS 0x400 has no sign bits: this sketch marks a
  fix valid only for nonnegative latitude AND longitude.

Shared CAN connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT. Join ESP32, MCP2515 and vehicle grounds.
  Connect CANH/CANL to the same bus; fit 120 ohms at each physical end.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  A successful CAN startup confirms SPI init; RX needs another CAN node.
*/

#include "CanCommon.h"
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
Adafruit_MPU6050 imu;
bool imuReady = false;
uint32_t lastImuRetry = 0;
constexpr uint8_t GPS_RX = 16, IGN_PIN = 34;
constexpr uint8_t IGN_ACTIVE_LEVEL = HIGH; // External 12 V ignition requires level shifting.
uint32_t ignOnAt = 0, lastGps = 0, lastIgn = 0, lastHill = 0, lastBeat = 0, switchAt = 0;
bool lastIgnState = false, hillRequest = false, switchSeen = false;
void encodeGpsCoordinate(double degrees, uint8_t *bytes) {
  // Round the complete coordinate before splitting it. Rounding fractional
  // minutes alone can emit 6000 near a degree boundary, which receivers reject.
  const uint32_t units = (uint32_t)lround(degrees * 6000.0);
  const uint16_t minutes = units % 6000;
  bytes[0] = units / 6000; bytes[1] = minutes >> 8; bytes[2] = minutes;
}
bool startImu() {
  for (const uint8_t address : {uint8_t(0x68), uint8_t(0x69)}) {
    if (!imu.begin(address)) continue;
    imu.setAccelerometerRange(MPU6050_RANGE_8_G);
    imu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.printf("MPU6050 ready at I2C 0x%02X\n", address);
    return true;
  }
  return false;
}
void setup() {
  Serial.begin(115200);
  pinMode(IGN_PIN, INPUT);
  gpsSerial.setRxBufferSize(1024);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, -1);
  Wire.begin(21, 22);
  Wire.setTimeOut(50);
  imuReady = startImu();
  if (!imuReady) Serial.println("MPU6050 not detected at I2C 0x68/0x69 on GPIO21/22; pitch invalid");
  if (!canBegin()) Serial.println("CAN unavailable");
  Serial.println("Node 4 TCU: GPS, ignition and MPU6050 pitch telemetry");
}
void loop() {
  const uint32_t now = millis();
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  uint32_t id; uint8_t len, data[8];
  for (uint8_t n=0; n<8 && canRead(id, len, data); ++n) if (id == 0x300) {
    if (len != 1 || (data[0] & 0x80)) { switchSeen = false; continue; }
    hillRequest = (data[0] & (1U << 3)) != 0; switchAt = now; switchSeen = true;
  }
  const bool hillEnabled = switchSeen && now - switchAt <= 3000 && hillRequest;
  if (now - lastGps >= 1000) {
    lastGps = now;
    uint8_t p[8] = {};
    const bool fix = gps.location.isValid() && gps.location.age() < 3000 &&
                     isfinite(gps.location.lat()) && isfinite(gps.location.lng()) &&
                     gps.location.lat() >= 0 && gps.location.lat() <= 90 &&
                     gps.location.lng() >= 0 && gps.location.lng() <= 180;
    if (fix) {
      const double lat = gps.location.lat(), lon = gps.location.lng();
      encodeGpsCoordinate(lat, p); encodeGpsCoordinate(lon, p+3);
      p[6] = 1; p[7] = min((uint32_t)12, gps.satellites.isValid() ? gps.satellites.value() : 0U);
    }
    canSend(0x400, p);
  }
  const bool ign = digitalRead(IGN_PIN) == IGN_ACTIVE_LEVEL;
  if (ign && !lastIgnState) ignOnAt = now;
  lastIgnState = ign;
  if (now - lastIgn >= 1000) {
    lastIgn = now;
    const uint16_t seconds = ign ? (uint16_t)((now - ignOnAt) / 1000) : 0;
    uint8_t d[8] = {(uint8_t)ign, (uint8_t)(seconds >> 8), (uint8_t)seconds, 0, 0, 0, 0, 0};
    canSend(0x401, d);
  }
  if (!imuReady && now - lastImuRetry >= 5000) {
    lastImuRetry = now;
    imuReady = startImu();
  }
  if (now - lastHill >= (hillEnabled ? 50U : 500U)) {
    lastHill = now;
    float pitchDeg = 0;
    bool pitchValid = false;
    if (imuReady) {
      sensors_event_t accel, gyro, temp;
      if (imu.getEvent(&accel, &gyro, &temp)) {
        const float transverse = sqrtf(accel.acceleration.y * accel.acceleration.y +
                                       accel.acceleration.z * accel.acceleration.z);
        pitchDeg = atan2f(-accel.acceleration.x, transverse) * 180.0f / PI;
        pitchValid = isfinite(pitchDeg);
      } else imuReady = false;
    }
    // IMU X-axis is assumed longitudinal; verify mounting sign on the bench.
    const int encoded = pitchValid ? constrain((int)lroundf(pitchDeg + 90.0f), 0, 180) : 90;
    uint8_t h[8] = {(uint8_t)hillEnabled, (uint8_t)encoded, 0,
                    (uint8_t)pitchValid, 0, 0, 0, 0};
    canSend(0x500, h);
  }
  if (now - lastBeat >= 1000) { lastBeat = now; canPrintStatus("Node 4 GPS/ignition/IMU"); canHeartbeat(4, canReady && imuReady ? 1 : 0, !canReady ? 1 : (!imuReady ? 2 : 0)); }
}
