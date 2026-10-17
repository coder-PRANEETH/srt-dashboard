/*
Steering IMU ESP32 - BENCH TEST. Serial Monitor: 115200 baud.
Connections:
  MPU6050 SDA -> GPIO21, SCL -> GPIO22, VCC -> compatible 3.3 V, GND.
  This ESP32 has NO CAN wiring. The test initializes its local WiFi/
  ESP-NOW radio on channel 1 but sends no packet and needs no Node 1.
Verification:
  Keep wheel still during the startup gyro calibration (about 2 seconds).
  Local radio READY and MPU6050 ready should print. Every 200 ms,
  gyro, acceleration, temperature and integrated angle are reported.
  Rotate the sensor: angle and acceleration should respond; keep still
  to observe drift. Wireless delivery is tested later with production
  firmware on both boards, since it cannot be checked on one board alone.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>

Adafruit_MPU6050 mpu;
float gyroOffsetZ = 0, angle = 0;
uint32_t previousMicros = 0, lastReport = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  const bool channelReady = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) == ESP_OK;
  uint8_t mac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    Serial.printf("Steering IMU MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  const bool radioReady = channelReady && esp_now_init() == ESP_OK;
  Serial.printf("Local ESP-NOW radio/channel 1: %s (no peer or packets)\n",
                radioReady ? "READY" : "FAILED");

  Wire.begin(21, 22);
  Wire.setClock(400000);
  if (!mpu.begin()) {
    Serial.println("FAIL: MPU6050 not found on GPIO21/22");
    while (true) delay(1000);
  }
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("Keep steering still: calibrating gyro offset...");
  delay(1000);
  double sum = 0;
  for (int n = 0; n < 300; ++n) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    sum += g.gyro.z;
    delay(3);
  }
  gyroOffsetZ = sum / 300.0;
  previousMicros = micros();
  Serial.println("MPU6050 ready; rotate sensor and watch angle and acceleration");
}

void loop() {
  sensors_event_t a, g, t;
  if (!mpu.getEvent(&a, &g, &t)) {
    if (millis() - lastReport >= 200) {
      lastReport = millis();
      Serial.println("FAIL: MPU6050 read error");
    }
    return;
  }
  const uint32_t nowMicros = micros();
  const float dt = (uint32_t)(nowMicros - previousMicros) / 1000000.0f;
  previousMicros = nowMicros;
  if (dt > 0 && dt < 0.1f)
    angle += (g.gyro.z - gyroOffsetZ) * 180.0f / PI * dt;
  if (millis() - lastReport >= 200) {
    lastReport = millis();
    Serial.printf("Angle=%.2f deg | gyroZ=%.3f rad/s | accel=(%.2f, %.2f, %.2f) m/s2 | temp=%.1f C\n",
                  angle, g.gyro.z, a.acceleration.x, a.acceleration.y,
                  a.acceleration.z, t.temperature);
  }
}
