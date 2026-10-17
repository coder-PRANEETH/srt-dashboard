/*
Node 3 CCM - BENCH TEST. Open Serial Monitor at 115200 baud.
Connections:
  Each switch closes its GPIO to GND; internal pull-up is used.
  GPIO4 cruise, GPIO5 ECO, GPIO13 reverse, GPIO14 hill hold,
  GPIO26 horn pushbutton, GPIO32 autonomous mode, GPIO33 headlight.
  Switch open reads OFF/HIGH; switch closed to GND reads ON/LOW.
  Do not apply 12 V or 5 V to the switch inputs. Release GPIO4/5 switches
  during ESP32 boot because they are boot-strapping pins.
Verification:
  Close/release one switch at a time; Serial should print the correct
  GPIO/name and ON/OFF after 50 ms debounce. Local CAN loopback sends
  the seven switch states in test frame 0x7A3; sent/pass counts should
  rise without another node. No production 0x300 frame is transmitted.

Local CAN controller connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT; MCP2515 GND -> ESP32 GND.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  Internal MCP2515 loopback needs no CANH/CANL bus, termination or peer.
  It tests SPI/controller TX/RX, not the external transceiver or bus.
*/

#include "CanCommon.h"

constexpr uint8_t SWITCH_COUNT = 7;
constexpr uint8_t pins[SWITCH_COUNT] = {4, 5, 13, 14, 26, 32, 33};
const char *names[SWITCH_COUNT] = {
  "cruise", "ECO", "reverse", "hill hold", "horn", "autonomous", "headlight"
};
constexpr uint32_t LOCAL_CAN_ID = 0x7A3;

bool stable[SWITCH_COUNT] = {}, sample[SWITCH_COUNT] = {};
uint32_t changedAt[SWITCH_COUNT] = {};
uint32_t lastReport = 0, canSent = 0, canPassed = 0;
uint8_t lastProbe[8] = {};

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < SWITCH_COUNT; ++i) {
    pinMode(pins[i], INPUT_PULLUP);
    stable[i] = sample[i] = digitalRead(pins[i]) == LOW;
    Serial.printf("GPIO%u %-11s initial %s\n", pins[i], names[i], stable[i] ? "ON" : "OFF");
  }
  Serial.printf("Node 3 local MCP2515 CAN loopback: %s\n",
                canBegin(true) ? "READY" : "FAILED");
  Serial.println("Close each switch to GND, one at a time; no other CAN node is needed");
}

void loop() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < SWITCH_COUNT; ++i) {
    const bool current = digitalRead(pins[i]) == LOW;
    if (current != sample[i]) {
      sample[i] = current;
      changedAt[i] = now;
    }
    if (stable[i] != sample[i] && now - changedAt[i] >= 50) {
      stable[i] = sample[i];
      Serial.printf("GPIO%u %s = %s\n", pins[i], names[i], stable[i] ? "ON" : "OFF");
    }
  }

  uint32_t id;
  uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == LOCAL_CAN_ID && len == 8 && memcmp(data, lastProbe, 8) == 0)
      ++canPassed;
  }
  if (now - lastReport >= 1000) {
    lastReport = now;
    for (uint8_t i = 0; i < SWITCH_COUNT; ++i) lastProbe[i] = stable[i];
    lastProbe[7] = 0xA5;
    if (canSend(LOCAL_CAN_ID, lastProbe)) ++canSent;
    Serial.printf("Switches %u%u%u%u%u%u%u | CAN local loopback sent=%lu pass=%lu\n",
                  stable[0], stable[1], stable[2], stable[3],
                  stable[4], stable[5], stable[6],
                  (unsigned long)canSent, (unsigned long)canPassed);
  }
}
