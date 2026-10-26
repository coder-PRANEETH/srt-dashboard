/* Steering-wheel transmitter based on the supplied 9-byte sketch.
 * MPU6050: SDA21/SCL22. Horn: GPIO27 switched to GND (INPUT_PULLUP).
 * Node 1: 68:09:47:DF:8E:E8, channel 1. Serial Monitor: 115200 baud.
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Wire.h>
#include <math.h>
#include "SteeringFeedback.h"

#define MPU 0x68
#define HORN_PIN 27
constexpr uint8_t RADIO_CHANNEL = 1;
constexpr uint32_t HORN_DEBOUNCE_MS = 15, SEND_INTERVAL_MS = 10;
uint8_t receiverMAC[] = {0x68, 0x09, 0x47, 0xDF, 0x8E, 0xE8};
// Same packed float + uint32_t + horn byte as the user's bool packet.
IMUDataWithHorn data = {};
static_assert(sizeof(data) == 9, "Node 1 expects a packed 9-byte horn packet");
float gzOffset = 0.0f, steeringAngle = 0.0f;
uint32_t lastTime = 0, sequence = 0, lastSend = 0, lastPrint = 0;
uint32_t rawChangedAt = 0;
bool rawHorn = false, hornPressed = false, hornChanged = false;
bool gyroConfigured = false, gyroCalibrated = false, angleValid = false;
bool radioReady = false, sentOnce = false;
uint32_t i2cFailures = 0, queueFailures = 0;
esp_err_t lastQueueResult = ESP_OK;
portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
bool sendPending = false;
uint32_t delivered = 0, deliveryFailed = 0;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSent(const esp_now_send_info_t *, esp_now_send_status_t status) {
#else
void onSent(const uint8_t *, esp_now_send_status_t status) {
#endif
  // Callback runs in the Wi-Fi task: copy counters only, no Serial or GPIO work.
  portENTER_CRITICAL(&radioMux);
  sendPending = false;
  if (status == ESP_NOW_SEND_SUCCESS) ++delivered;
  else ++deliveryFailed;
  portEXIT_CRITICAL(&radioMux);
}

bool readGyro(int16_t &gz) {
  Wire.beginTransmission(MPU);
  Wire.write(0x47);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(MPU, (uint8_t)2, (uint8_t)true) != 2) return false;
  const uint8_t hi = Wire.read(), lo = Wire.read();
  gz = (int16_t)(((uint16_t)hi << 8) | lo);
  return true;
}
bool configureGyro() {
  // Explicit +/-250 deg/s range matches the /131 conversion below.
  for (const uint8_t reg : {0x6B, 0x1B}) {
    Wire.beginTransmission(MPU); Wire.write(reg); Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
  }
  return true;
}
void serviceHorn() {
  const uint32_t now = millis();
  const bool pressed = digitalRead(HORN_PIN) == LOW;
  if (pressed != rawHorn) { rawHorn = pressed; rawChangedAt = now; }
  if (hornPressed != rawHorn && now - rawChangedAt >= HORN_DEBOUNCE_MS) {
    hornPressed = rawHorn; hornChanged = true;
    Serial.printf("HORN GPIO27=%s -> %s\n", rawHorn ? "LOW" : "HIGH", hornPressed ? "PRESSED" : "RELEASED");
  }
}
void sendControls() {
  if (!radioReady || (!hornChanged && sentOnce && millis() - lastSend < SEND_INTERVAL_MS)) return;
  portENTER_CRITICAL(&radioMux);
  const bool pending = sendPending;
  if (!pending) sendPending = true;
  portEXIT_CRITICAL(&radioMux);
  if (pending) return; // Do not build up a queue of old press/release packets.
  data.steeringAngle = steeringAngle;
  data.sequence = sequence++;
  data.hornPressed = hornPressed ? 1 : 0;
  if (angleValid) {
    lastQueueResult = esp_now_send(receiverMAC, (const uint8_t *)&data, sizeof(data));
  } else {
    // On I2C failure, keep the horn live without claiming an old angle is fresh.
    IMUDataWithHornValidity unavailable = {steeringAngle, data.sequence, data.hornPressed, 0};
    lastQueueResult = esp_now_send(receiverMAC, (const uint8_t *)&unavailable, sizeof(unavailable));
  }
  lastSend = millis(); sentOnce = true;
  if (lastQueueResult == ESP_OK) hornChanged = false;
  else {
    ++queueFailures;
    portENTER_CRITICAL(&radioMux); sendPending = false; portEXIT_CRITICAL(&radioMux);
  }
}
void printStatus() {
  if (millis() - lastPrint < 250) return;
  lastPrint = millis();
  portENTER_CRITICAL(&radioMux);
  const uint32_t ok = delivered, failed = deliveryFailed;
  const bool pending = sendPending;
  portEXIT_CRITICAL(&radioMux);
  Serial.printf("GPIO27=%s | Horn=%s | Steering=%.2f deg %s | IMU errors=%lu | radio ACK=%lu FAIL=%lu pending=%u queueErrors=%lu lastQueue=%d\n",
    rawHorn ? "LOW" : "HIGH", hornPressed ? "PRESSED" : "RELEASED", steeringAngle,
    angleValid ? "LIVE" : "UNAVAILABLE", (unsigned long)i2cFailures,
    (unsigned long)ok, (unsigned long)failed, pending, (unsigned long)queueFailures, (int)lastQueueResult);
}
void setup() {
  Serial.begin(115200);
  pinMode(HORN_PIN, INPUT_PULLUP);
  rawHorn = digitalRead(HORN_PIN) == LOW; rawChangedAt = millis();
  Serial.println("Horn wiring: GPIO27 -> button -> GND. Press must read LOW; release must read HIGH.");

  if (!WiFi.mode(WIFI_STA)) { Serial.println("WiFi startup failed"); return; }
  WiFi.setSleep(false); WiFi.setAutoReconnect(false);
  Serial.print("Transmitter STA MAC: "); Serial.println(WiFi.macAddress());
  Serial.println("Node 1 receiver STA MAC: 68:09:47:DF:8E:E8 | channel 1");
  if (esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK || esp_now_init() != ESP_OK ||
      esp_now_register_send_cb(onSent) != ESP_OK) { Serial.println("ESP-NOW startup failed"); return; }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, receiverMAC, 6); peer.channel = RADIO_CHANNEL; peer.ifidx = WIFI_IF_STA; peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) { Serial.println("Failed to add Node 1 peer"); return; }
  radioReady = true;

  Wire.begin(21, 22); Wire.setTimeOut(10);
  gyroConfigured = configureGyro();
  if (gyroConfigured) {
    Serial.println("Keep IMU STILL: calibrating gyro; horn remains active...");
    int64_t total = 0; unsigned samples = 0;
    for (unsigned i = 0; i < 1000; ++i) {
      serviceHorn(); sendControls();
      int16_t gz;
      if (readGyro(gz)) { total += gz; ++samples; } else ++i2cFailures;
      delay(3);
    }
    if (samples >= 900) { gzOffset = total / (float)samples / 131.0f; gyroCalibrated = true; }
    Serial.printf("Gyro samples=%u/1000 | offset=%.4f | %s\n", samples, gzOffset,
                  gyroCalibrated ? "calibrated" : "calibration failed; horn only, restart after repairing IMU");
  } else Serial.println("MPU6050 not detected; horn remains active. Repair I2C and restart to calibrate steering.");
  lastTime = micros();
  Serial.println("Transmitter ready. ACK counters indicate radio delivery, not relay contact feedback.");
}
void loop() {
  serviceHorn(); // Read the button before I2C; I2C failure never skips the horn.
  if (gyroCalibrated) {
    int16_t gz;
    const uint32_t now = micros();
    const float dt = (uint32_t)(now - lastTime) / 1000000.0f;
    lastTime = now;
    angleValid = readGyro(gz);
    if (angleValid && dt > 0 && dt < 0.1f) {
      float rateZ = gz / 131.0f - gzOffset;
      if (fabsf(rateZ) < 0.8f) rateZ = 0;
      steeringAngle = constrain(steeringAngle + rateZ * dt, -270.0f, 270.0f);
    } else if (!angleValid) ++i2cFailures;
  }
  serviceHorn(); sendControls(); printStatus();
  delay(2);
}
