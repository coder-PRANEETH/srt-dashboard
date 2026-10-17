/*
Node 1 BCM - BENCH TEST. Open Serial Monitor at 115200 baud.
Connections:
  GPIO34 <- throttle/pedal divider output, limited to 0-3.3 V. Never
  connect a 12 V pedal/switch wire directly to an ESP32 GPIO.
  For an isolated UART check, jumper ESP32 TX2 GPIO17 -> RX2 GPIO16.
  Production can instead cross these to Pi 5 RX/TX at 3.3 V TTL.
  GPIO13 -> active-high 3.3 V horn relay input; relay switches 12 V load.
  BTS7960 VCC, R_EN and L_EN -> regulated 5 V; GND -> shared GND.
  GPIO32 -> BTS7960 LPWM; GPIO33 -> RPWM. Its motor supply is 12 V,
  wired to the power terminals, separate from the ESP32 logic supply.
  Add external pull-downs to LPWM/RPWM so boot/reset leaves the motor off.
  Steering MPU6050 is on a separate ESP32. This sketch only initializes
  this board's ESP-NOW radio; no wireless sender is required.
Verification:
  Confirm printed Node 1 MAC is 30:76:F5:94:6A:60, the sender target.
  Pedal ADC should change smoothly across the safe divider output range.
  With the UART jumper, this sketch sends 'T' every second and prints it
  arriving at RX2. CAN local loopback sent/pass counts should both rise.
  On a clear bench with a fused 12 V motor supply, type A for LEFT (GPIO32
  LPWM), D for RIGHT (GPIO33 RPWM), or S to ramp to STOP. PWM is limited to
  60/255, as in the working motor sketch. It changes by 2 counts every 15 ms
  (about 450 ms from 0 to 60), and waits 100 ms at zero before reversing.
  A/D motion expires after 2 s unless the command is sent again. X cuts
  both PWM pins to zero immediately for an emergency stop; use S normally.
  Type m then h within 5 s to pulse the horn relay for 100 ms; meter first.
  No other vehicle node or wireless sender is needed for this test.

Local CAN controller connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT; MCP2515 GND -> ESP32 GND.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  Internal MCP2515 loopback needs no CANH/CANL bus, termination or peer.
  It tests SPI/controller TX/RX, not the external transceiver or bus.
*/

#include "CanCommon.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

constexpr uint8_t PEDAL_PIN = 34;
constexpr uint8_t HORN_PIN = 13;
constexpr uint8_t LPWM_PIN = 32;
constexpr uint8_t RPWM_PIN = 33;
constexpr uint32_t LOCAL_CAN_ID = 0x7A1;
constexpr int16_t MOTOR_DUTY = 60;  // Working BTS7960 value, 8-bit PWM (0-255).
constexpr int16_t MOTOR_STEP = 2;
constexpr uint32_t MOTOR_STEP_MS = 15;
constexpr uint32_t MOTOR_ZERO_PAUSE_MS = 100;
constexpr uint32_t MOTOR_COMMAND_TIMEOUT_MS = 2000;

HardwareSerial piSerial(2);
bool pwmReady = false, armed = false;
uint32_t armUntil = 0, hornUntil = 0;
uint32_t nextMotorStep = 0, motorCommandAt = 0, zeroAt = 0;
int16_t motorDuty = 0, motorTarget = 0; // Positive=LPWM/LEFT; negative=RPWM/RIGHT.
uint32_t lastReport = 0, canSent = 0, canPassed = 0;
uint8_t canSequence = 0;

void hardStopMotor() {
  motorTarget = motorDuty = 0;
  motorCommandAt = 0;
  zeroAt = millis();
  if (pwmReady) {
    ledcWrite(LPWM_PIN, 0);
    ledcWrite(RPWM_PIN, 0);
  }
}

void commandMotor(int16_t target) {
  if (!pwmReady) {
    Serial.println("PWM initialization failed; motor command ignored");
    return;
  }
  motorTarget = target;
  motorCommandAt = millis();
  Serial.printf("%s requested: ramp toward PWM %d; resend within 2 s to continue\n",
                target > 0 ? "LEFT GPIO32" : "RIGHT GPIO33", MOTOR_DUTY);
}

void updateMotor(uint32_t now) {
  if (motorCommandAt && now - motorCommandAt >= MOTOR_COMMAND_TIMEOUT_MS) {
    motorCommandAt = 0;
    motorTarget = 0;
    Serial.println("Motor command timed out; ramping to STOP");
  }
  if (!pwmReady || (int32_t)(now - nextMotorStep) < 0) return;
  nextMotorStep = now + MOTOR_STEP_MS;

  int16_t next = motorDuty;
  // Any direction change first ramps the active PWM to zero. Never drive
  // LPWM and RPWM together, and keep both off during the reversal pause.
  if (motorDuty > 0 && motorTarget < 0) next = max(0, motorDuty - MOTOR_STEP);
  else if (motorDuty < 0 && motorTarget > 0) next = min(0, motorDuty + MOTOR_STEP);
  else if (motorDuty < motorTarget) {
    if (motorDuty == 0 && now - zeroAt < MOTOR_ZERO_PAUSE_MS) return;
    next = min(motorTarget, motorDuty + MOTOR_STEP);
  } else if (motorDuty > motorTarget) {
    if (motorDuty == 0 && now - zeroAt < MOTOR_ZERO_PAUSE_MS) return;
    next = max(motorTarget, motorDuty - MOTOR_STEP);
  }
  if (next == motorDuty) return;
  motorDuty = next;
  if (motorDuty == 0) zeroAt = now;
  ledcWrite(LPWM_PIN, motorDuty > 0 ? motorDuty : 0);
  ledcWrite(RPWM_PIN, motorDuty < 0 ? -motorDuty : 0);
}

void pulseHorn() {
  if (!armed) {
    Serial.println("Arm with m first");
    return;
  }
  armed = false;
  digitalWrite(HORN_PIN, HIGH);
  hornUntil = millis() + 100;
  Serial.println("Horn relay GPIO13 HIGH for 100 ms, then automatic stop");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  piSerial.begin(115200, SERIAL_8N1, 16, 17);
  digitalWrite(LPWM_PIN, LOW);
  digitalWrite(RPWM_PIN, LOW);
  pinMode(LPWM_PIN, OUTPUT);
  pinMode(RPWM_PIN, OUTPUT);
  pinMode(HORN_PIN, OUTPUT);
  digitalWrite(HORN_PIN, LOW);
  const bool leftReady = ledcAttach(LPWM_PIN, 1000, 8);
  const bool rightReady = ledcAttach(RPWM_PIN, 1000, 8);
  pwmReady = leftReady && rightReady;
  hardStopMotor();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  const bool channelReady = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) == ESP_OK;
  uint8_t mac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    Serial.printf("Node 1 MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  const bool radioReady = channelReady && esp_now_init() == ESP_OK;
  Serial.printf("Local ESP-NOW radio/channel 1: %s (no peer needed)\n",
                radioReady ? "READY" : "FAILED");
  Serial.printf("Local MCP2515 CAN loopback: %s\n", canBegin(true) ? "READY" : "FAILED");
  Serial.printf("PWM outputs: %s\n", pwmReady ? "READY" : "FAILED");
  Serial.println("Commands: A=LEFT, D=RIGHT, S=smooth STOP, X=immediate cutoff, m then h=horn pulse");
}

void loop() {
  const uint32_t now = millis();
  if (armed && (int32_t)(now - armUntil) >= 0) {
    armed = false;
    Serial.println("Arm expired");
  }
  if (hornUntil && (int32_t)(now - hornUntil) >= 0) {
    digitalWrite(HORN_PIN, LOW);
    hornUntil = 0;
    Serial.println("Horn relay stopped");
  }

  while (Serial.available()) {
    const char command = Serial.read();
    if (command == 'm') {
      armed = true;
      armUntil = millis() + 5000;
      Serial.println("Horn armed for one pulse; choose h within 5 s");
    } else if (command == 'a' || command == 'A') commandMotor(MOTOR_DUTY);
    else if (command == 'd' || command == 'D') commandMotor(-MOTOR_DUTY);
    else if (command == 'h') pulseHorn();
    else if (command == 's' || command == 'S') {
      armed = false;
      motorTarget = 0;
      motorCommandAt = 0;
      digitalWrite(HORN_PIN, LOW);
      hornUntil = 0;
      Serial.println("Ramping motor to STOP; horn relay off");
    } else if (command == 'x' || command == 'X') {
      armed = false;
      hardStopMotor();
      digitalWrite(HORN_PIN, LOW);
      hornUntil = 0;
      Serial.println("Emergency cutoff: both motor PWM pins and horn off");
    }
  }
  updateMotor(now);

  while (piSerial.available())
    Serial.printf("UART RX GPIO16: 0x%02X\n", piSerial.read());

  uint32_t id;
  uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == LOCAL_CAN_ID && len == 8 && data[0] == 1 && data[2] == 0xA5)
      ++canPassed;
  }
  if (now - lastReport >= 1000) {
    lastReport = now;
    const uint8_t probe[8] = {1, canSequence++, 0xA5, 0, 0, 0, 0, 0};
    if (canSend(LOCAL_CAN_ID, probe)) ++canSent;
    piSerial.write('T');
    Serial.printf("Pedal GPIO34 ADC=%d | motor PWM signed=%d target=%d | CAN local loopback sent=%lu pass=%lu | UART TX='T'\n",
                  analogRead(PEDAL_PIN), motorDuty, motorTarget,
                  (unsigned long)canSent, (unsigned long)canPassed);
  }
}
