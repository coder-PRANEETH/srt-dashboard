/*
Node 2 PCM - PRODUCTION. Serial diagnostics are at 115200 baud.
Connections:
  GPIO13 -> motor/controller PWM 1; GPIO14 -> PWM 2 (1 kHz, 8-bit here).
  GPIO26 -> reverse relay 1 input; GPIO33 -> reverse relay 2 input.
  Reverse relay inputs are active-high at 3.3 V; the 12 V power path is
  separate. Join control grounds and keep 12 V off all ESP32 GPIO.
  No brake GPIO is connected or read by this sketch.
Verification:
  Supply 0x301 pedal and 0x300 switches from Nodes 1 and 3; observe
  Node 2 status 0x201 and heartbeat 0x600 in Node 5 CAN log.
  ENABLE_TRACTION_OUTPUTS and ENABLE_REVERSE_RELAYS are FALSE by default;
  expected bench measurement is zero PWM and LOW reverse outputs.
  Validate motor/controller PWM requirements and relay polarity with loads
  isolated before changing either enable constant. Stale CAN input clears
  throttle output within 250 ms.

Shared CAN connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT. Join ESP32, MCP2515 and vehicle grounds.
  Connect CANH/CANL to the same bus; fit 120 ohms at each physical end.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  A successful CAN startup confirms SPI init; RX needs another CAN node.
*/

#include "CanCommon.h"
// Set true only after isolated motor/relay bench validation and polarity checks.
constexpr bool ENABLE_TRACTION_OUTPUTS = false;
constexpr bool ENABLE_REVERSE_RELAYS = false;
constexpr uint8_t MOTOR_1 = 13, MOTOR_2 = 14, REVERSE_1 = 26, REVERSE_2 = 33;
uint8_t pedal = 0, throttleOut = 0;
bool blocked = true, cruise = false, eco = false, reverse = false, hill = false;
uint32_t accelAt = 0, switchAt = 0, lastStatus = 0, lastBeat = 0;
void safeOutputs() {
  ledcWrite(MOTOR_1, 0); ledcWrite(MOTOR_2, 0);
  digitalWrite(REVERSE_1, LOW); digitalWrite(REVERSE_2, LOW);
}
void setup() {
  Serial.begin(115200);
  pinMode(REVERSE_1, OUTPUT); pinMode(REVERSE_2, OUTPUT);
  digitalWrite(REVERSE_1, LOW); digitalWrite(REVERSE_2, LOW);
  ledcAttach(MOTOR_1, 1000, 8); ledcAttach(MOTOR_2, 1000, 8);
  safeOutputs();
  if (!canBegin()) Serial.println("CAN unavailable");
  Serial.println("Node 2 PCM: telemetry active; traction outputs require explicit bench arming");
}
void loop() {
  const uint32_t now = millis();
  uint32_t id; uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == 0x301 && len == 8) {
      accelAt = now; pedal = min(data[2], (uint8_t)100); blocked = data[3] != 0;
    } else if (id == 0x300 && len == 8) {
      switchAt = now; cruise = data[0]; eco = data[1]; reverse = data[2]; hill = data[3];
    }
  }
  const bool fresh = accelAt && switchAt && now - accelAt < 250 && now - switchAt < 250;
  const bool permitted = fresh && !blocked && ENABLE_TRACTION_OUTPUTS;
  // Cruise/hill hold need independent vehicle validation, so never raise throttle.
  throttleOut = permitted ? (eco ? min(pedal, (uint8_t)40) : pedal) : 0;
  const uint8_t duty = map(throttleOut, 0, 100, 0, 255);
  ledcWrite(MOTOR_1, duty); ledcWrite(MOTOR_2, duty);
  const bool reverseOut = permitted && ENABLE_REVERSE_RELAYS && reverse && throttleOut == 0;
  digitalWrite(REVERSE_1, reverseOut ? HIGH : LOW);
  digitalWrite(REVERSE_2, reverseOut ? HIGH : LOW);
  if (now - lastStatus >= 50) {
    lastStatus = now;
    uint8_t s[8] = {(uint8_t)(throttleOut > 0), (uint8_t)(throttleOut > 0),
                    (uint8_t)reverseOut, throttleOut, (uint8_t)cruise,
                    (uint8_t)eco, 0, 0};
    canSend(0x201, s);
  }
  if (now - lastBeat >= 1000) { lastBeat = now; canHeartbeat(2, canReady ? 1 : 0, canReady ? 0 : 1); }
}
