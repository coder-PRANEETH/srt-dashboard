/*
Node 1 BCM - PRODUCTION. Serial diagnostics are at 115200 baud.
Connections:
  GPIO34 <- throttle/pedal divider output, 0-3.3 V maximum.
  ESP32 RX2 GPIO16 <- Pi 5 TX; ESP32 TX2 GPIO17 -> Pi 5 RX; common GND,
  3.3 V TTL, 115200 baud. Never apply 5 V UART or 12 V to ESP GPIO.
  GPIO13 -> active-high 3.3 V input of the 12 V horn relay.
  BTS7960 VCC, R_EN and L_EN -> regulated 5 V; GND -> shared GND.
  GPIO32 -> LPWM; GPIO33 -> RPWM. Motor supply goes to the driver's
  separate 12 V power terminals. Add external LPWM/RPWM pull-downs.
  The steering MPU6050 is on the separate ESP32, linked by ESP-NOW on
  WiFi channel 1; sender targets MAC 30:76:F5:94:6A:60.
Verification:
  Confirm this board prints the sender's target MAC on boot.
  Node 5 CAN log should show 0x301 pedal, 0x100 steering, 0x101 horn,
  and Node 1 heartbeat 0x600. Angle/link becomes invalid after 500 ms
  without wireless packets. GPIO13 follows a fresh 0x300 horn request.
  GPIO32/33 stay LOW in production; steering motor drive is inactive.
  Headlight output is unassigned. Check pedal calibration on the bench.

Shared CAN connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT. Join ESP32, MCP2515 and vehicle grounds.
  Connect CANH/CANL to the same bus; fit 120 ohms at each physical end.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  A successful CAN startup confirms SPI init; RX needs another CAN node.
*/

#include "CanCommon.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>

// Confirm relay polarity, motor limits and pedal calibration before enabling outputs.
constexpr bool ENABLE_HORN_OUTPUT = true; // Active-high 3.3 V input to the 12 V horn relay.
constexpr uint8_t HORN_PIN = 13, PEDAL_PIN = 34;
constexpr uint8_t STEER_LPWM_PIN = 32, STEER_RPWM_PIN = 33;
constexpr int PEDAL_MIN = 0, PEDAL_MAX = 4095;
constexpr uint8_t WIFI_CHANNEL = 1;
constexpr uint32_t STEERING_TIMEOUT_MS = 500;
#pragma pack(push, 1)
struct SteeringData { uint32_t sequence; float steeringAngle; };
#pragma pack(pop)
QueueHandle_t steeringQueue = nullptr;
float steeringAngle = 0;
uint32_t steeringAt = 0, steeringSequence = 0;
bool steeringSeen = false;
uint32_t switchAt = 0, lastAccel = 0, lastSteer = 0, lastLamps = 0, lastBeat = 0;
bool hornRequest = false, headlightRequest = false;

void onSteering(const esp_now_recv_info_t *, const uint8_t *bytes, int len) {
  if (len != sizeof(SteeringData)) return;
  SteeringData packet;
  memcpy(&packet, bytes, sizeof(packet));
  if (!isfinite(packet.steeringAngle)) return;
  if (steeringQueue != nullptr && xQueueSend(steeringQueue, &packet, 0) != pdTRUE) {
    SteeringData discard;
    xQueueReceive(steeringQueue, &discard, 0);
    xQueueSend(steeringQueue, &packet, 0);
  }
}
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  steeringQueue = xQueueCreate(20, sizeof(SteeringData));
  if (steeringQueue == nullptr) Serial.println("Steering ESP-NOW queue allocation failed");
  // EN pins are tied high at the driver. External pulldowns must hold these
  // inputs low during ESP32 reset and boot, before this code runs.
  digitalWrite(STEER_LPWM_PIN, LOW);
  digitalWrite(STEER_RPWM_PIN, LOW);
  pinMode(STEER_LPWM_PIN, OUTPUT);
  pinMode(STEER_RPWM_PIN, OUTPUT);
  pinMode(HORN_PIN, OUTPUT);
  digitalWrite(HORN_PIN, LOW);
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); delay(200);
  uint8_t mac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    Serial.printf("Node 1 MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK)
    Serial.println("ESP-NOW channel setup failed");
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onSteering);
  else Serial.println("ESP-NOW initialization failed");
  if (!canBegin()) Serial.println("CAN unavailable: check MCP2515 wiring/crystal");
  Serial.println("Node 1 BCM: ESP-NOW channel 1 telemetry active; high-power steering and unassigned headlight output inactive");
}
void loop() {
  const uint32_t now = millis();
  SteeringData packet;
  while (steeringQueue != nullptr && xQueueReceive(steeringQueue, &packet, 0) == pdTRUE) {
    if (steeringSeen && now - steeringAt < STEERING_TIMEOUT_MS &&
        (int32_t)(packet.sequence - steeringSequence) <= 0) continue;
    steeringAngle = packet.steeringAngle;
    steeringSequence = packet.sequence;
    steeringAt = now;
    steeringSeen = true;
  }
  uint32_t id; uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == 0x300 && len == 8) {
      hornRequest = data[4] != 0;
      headlightRequest = data[6] != 0;
      switchAt = now;
    }
    // 0x200 and 0x501 are intentionally ignored: the matrix has no secure
    // authentication or validated steering motor control specification.
  }
  const bool switchesFresh = switchAt && now - switchAt < 200;
  // No calibrated target, end stop or closed-loop steering controller yet.
  digitalWrite(STEER_LPWM_PIN, LOW);
  digitalWrite(STEER_RPWM_PIN, LOW);
  digitalWrite(HORN_PIN, ENABLE_HORN_OUTPUT && switchesFresh && hornRequest ? HIGH : LOW);
  if (now - lastAccel >= 20) {
    lastAccel = now;
    const int raw = analogRead(PEDAL_PIN);
    const int pct = constrain(map(raw, PEDAL_MIN, PEDAL_MAX, 0, 100), 0, 100);
    uint8_t accel[8] = {(uint8_t)(raw >> 8), (uint8_t)raw,
                        (uint8_t)pct, 0, 0, 0, 0, 0};
    canSend(0x301, accel);
  }
  if (now - lastSteer >= 20) {
    lastSteer = now;
    const bool fresh = steeringSeen && now - steeringAt < STEERING_TIMEOUT_MS;
    const int16_t angle10 = fresh ? (int16_t)constrain(lroundf(steeringAngle * 10), -32768L, 32767L) : 0;
    const uint16_t encoded = (uint16_t)angle10;
    uint8_t steer[8] = {(uint8_t)(encoded >> 8), (uint8_t)encoded,
                        (uint8_t)(steeringAngle >= 0), (uint8_t)fresh, 0, 0, 0, 0};
    canSend(0x100, steer);
  }
  if (now - lastLamps >= 100) {
    lastLamps = now;
    uint8_t lamps[8] = {(uint8_t)(ENABLE_HORN_OUTPUT && switchesFresh && hornRequest),
                        0, 0, 0, 0, 0, 0, 0};
    canSend(0x101, lamps);
  }
  if (now - lastBeat >= 1000) { lastBeat = now; canHeartbeat(1, canReady ? 1 : 0, canReady ? 0 : 1); }
}
