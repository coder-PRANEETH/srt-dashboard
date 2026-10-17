/*
Node 3 CCM - PRODUCTION. Serial diagnostics are at 115200 baud.
Connections:
  Switches close to GND, using ESP32 internal pull-ups.
  GPIO4 cruise, GPIO5 ECO, GPIO13 reverse, GPIO14 hill hold,
  GPIO26 horn, GPIO32 autonomous mode, GPIO33 headlight.
  Open means OFF/HIGH; closed means ON/LOW. Use only dry contacts or
  3.3 V-compatible signals, never a direct 12 V switch feed.
  Release GPIO4/5 switches at boot; those pins affect ESP32 boot mode.
Verification:
  Flip switches individually and watch Node 5 CAN log frame 0x300:
  bytes 0..6 follow the pin order above, 0=OFF and 1=ON.
  State changes settle after 50 ms debounce. Frame 0x300 repeats every
  20 ms and Node 3 heartbeat 0x600 every second.

Shared CAN connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT. Join ESP32, MCP2515 and vehicle grounds.
  Connect CANH/CANL to the same bus; fit 120 ohms at each physical end.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  A successful CAN startup confirms SPI init; RX needs another CAN node.
*/

#include "CanCommon.h"
const uint8_t pins[7] = {4, 5, 13, 14, 26, 32, 33};
bool stable[7] = {}, sample[7] = {};
uint32_t changedAt[7] = {}, lastSend = 0, lastBeat = 0;
void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < 7; ++i) {
    pinMode(pins[i], INPUT_PULLUP); stable[i] = sample[i] = digitalRead(pins[i]) == LOW;
  }
  if (!canBegin()) Serial.println("CAN unavailable");
  Serial.println("Node 3 CCM: active-low switches to GND, 50 ms debounce");
}
void loop() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < 7; ++i) {
    bool current = digitalRead(pins[i]) == LOW;
    if (current != sample[i]) { sample[i] = current; changedAt[i] = now; }
    if (stable[i] != sample[i] && now - changedAt[i] >= 50) stable[i] = sample[i];
  }
  if (now - lastSend >= 20) {
    lastSend = now;
    uint8_t data[8] = {};
    for (uint8_t i = 0; i < 7; ++i) data[i] = stable[i];
    canSend(0x300, data);
  }
  uint32_t id; uint8_t len, data[8]; while (canRead(id, len, data)) {}
  if (now - lastBeat >= 1000) { lastBeat = now; canHeartbeat(3, canReady ? 1 : 0, canReady ? 0 : 1); }
}
