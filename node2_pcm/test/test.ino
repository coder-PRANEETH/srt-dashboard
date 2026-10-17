/*
Node 2 PCM - BENCH TEST. Open Serial Monitor at 115200 baud.
Connections:
  GPIO13 -> motor/controller PWM 1; GPIO14 -> PWM 2.
  GPIO26 -> reverse relay 1 input; GPIO33 -> reverse relay 2 input.
  Relay inputs use active-high 3.3 V control; their 12 V contacts/load
  supply must remain electrically separate from ESP32 GPIO.
  Share logic ground with the controller/relay inputs as required.
  There is NO brake input on GPIO22 or any other Node 2 GPIO.
Verification:
  Local CAN loopback sent/pass counts should rise without another node.
  Type m to arm for 5 s; 1/2 give 25% PWM on GPIO13/14 for 250 ms, and
  3/4 give 100 ms HIGH on relay inputs GPIO26/33. Each pulse stops
  automatically and consumes the arm. Type s for immediate STOP.
  Start with a meter/scope and isolated loads before trying motor hardware.
  No brake input is checked because its pin was removed.

Local CAN controller connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT; MCP2515 GND -> ESP32 GND.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  Internal MCP2515 loopback needs no CANH/CANL bus, termination or peer.
  It tests SPI/controller TX/RX, not the external transceiver or bus.
*/

#include "CanCommon.h"

constexpr uint8_t MOTOR_1 = 13, MOTOR_2 = 14;
constexpr uint8_t REVERSE_1 = 26, REVERSE_2 = 33;
constexpr uint32_t LOCAL_CAN_ID = 0x7A2;
constexpr uint8_t MOTOR_DUTY = 64; // 25% at 1 kHz, 8-bit PWM.

bool pwmReady = false, armed = false;
uint32_t armUntil = 0, outputUntil = 0, lastReport = 0;
uint32_t canSent = 0, canPassed = 0;
uint8_t canSequence = 0;

void stopOutputs() {
  if (pwmReady) {
    ledcWrite(MOTOR_1, 0);
    ledcWrite(MOTOR_2, 0);
  }
  digitalWrite(REVERSE_1, LOW);
  digitalWrite(REVERSE_2, LOW);
  outputUntil = 0;
}

void pulseOutput(char command) {
  if (!armed) {
    Serial.println("Arm with m first");
    return;
  }
  if ((command == '1' || command == '2') && !pwmReady) {
    Serial.println("PWM initialization failed");
    return;
  }
  stopOutputs();
  armed = false;
  const uint8_t pin = command == '1' ? MOTOR_1 : command == '2' ? MOTOR_2 :
                      command == '3' ? REVERSE_1 : REVERSE_2;
  const bool motor = command == '1' || command == '2';
  if (motor) ledcWrite(pin, MOTOR_DUTY);
  else digitalWrite(pin, HIGH);
  outputUntil = millis() + (motor ? 250 : 100);
  Serial.printf("GPIO%u %s pulse started; automatic stop in %u ms\n",
                pin, motor ? "25% PWM" : "HIGH", motor ? 250 : 100);
}

void setup() {
  Serial.begin(115200);
  digitalWrite(REVERSE_1, LOW);
  digitalWrite(REVERSE_2, LOW);
  pinMode(REVERSE_1, OUTPUT);
  pinMode(REVERSE_2, OUTPUT);
  const bool firstReady = ledcAttach(MOTOR_1, 1000, 8);
  const bool secondReady = ledcAttach(MOTOR_2, 1000, 8);
  pwmReady = firstReady && secondReady;
  stopOutputs();
  Serial.printf("Node 2 local MCP2515 CAN loopback: %s\n",
                canBegin(true) ? "READY" : "FAILED");
  Serial.printf("Motor PWM outputs: %s\n", pwmReady ? "READY" : "FAILED");
  Serial.println("Commands: m=arm 5 s, 1/2=PWM pins 13/14, 3/4=relay pins 26/33, s=STOP");
}

void loop() {
  const uint32_t now = millis();
  if (armed && (int32_t)(now - armUntil) >= 0) {
    armed = false;
    Serial.println("Arm expired");
  }
  if (outputUntil && (int32_t)(now - outputUntil) >= 0) {
    stopOutputs();
    Serial.println("Output stopped");
  }
  while (Serial.available()) {
    const char command = Serial.read();
    if (command == 'm') {
      armed = true;
      armUntil = millis() + 5000;
      Serial.println("Armed for one pulse; choose 1, 2, 3 or 4 within 5 s");
    } else if (command >= '1' && command <= '4') pulseOutput(command);
    else if (command == 's') {
      armed = false;
      stopOutputs();
      Serial.println("STOP");
    }
  }

  uint32_t id;
  uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == LOCAL_CAN_ID && len == 8 && data[0] == 2 && data[2] == 0xA5)
      ++canPassed;
  }
  if (now - lastReport >= 1000) {
    lastReport = now;
    const uint8_t probe[8] = {2, canSequence++, 0xA5, 0, 0, 0, 0, 0};
    if (canSend(LOCAL_CAN_ID, probe)) ++canSent;
    Serial.printf("Node 2 CAN local loopback sent=%lu pass=%lu\n",
                  (unsigned long)canSent, (unsigned long)canPassed);
  }
}
