/*
 * NODE 1 - ACCELERATOR + REMOTE STEERING + WIRELESS ANGLE FEEDBACK
 * GPIO 34: Accelerator signal input
 * CAN ID: 0x301
 * Payload: 2 bytes containing ADC value (0-4095)
 */

#include <SPI.h>
#include <mcp_can.h>
#include "ThrottleCalibration.h"
#include "SteeringControl.h"
#include "SteeringFeedback.h"
#include "RemoteDrive.h"
#include <esp_system.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>

// Match the steering-wheel transmitter's current WiFi channel. No TCP/UDP
// port, access point connection or send peer is needed for ESP-NOW reception.
constexpr uint8_t STEERING_WIFI_CHANNEL = 1;
portMUX_TYPE steeringFeedbackMux = portMUX_INITIALIZER_UNLOCKED;
SteeringFeedback wirelessSteeringFeedback;
SteeringFeedback steeringFeedback;
bool steeringRadioReady = false;
uint32_t lastSteeringAngleStatus = 0;
bool steeringAngleWasFresh = false;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onSteeringAngle(const esp_now_recv_info_t *, const uint8_t *bytes, int len) {
#else
void onSteeringAngle(const uint8_t *, const uint8_t *bytes, int len) {
#endif
  // The WiFi task only updates a protected snapshot; GPIO/CAN stay in loop().
  const uint32_t now = millis();
  portENTER_CRITICAL(&steeringFeedbackMux);
  wirelessSteeringFeedback.accept(bytes, len, now);
  portEXIT_CRITICAL(&steeringFeedbackMux);
}

void setupSteeringReceiver() {
  if (!WiFi.mode(WIFI_STA)) { Serial.println("ERROR: steering WiFi startup failed"); return; }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  Serial.print("Node 1 steering receiver STA MAC: "); Serial.println(WiFi.macAddress());
  if (esp_wifi_set_channel(STEERING_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
      esp_now_init() != ESP_OK) {
    Serial.println("ERROR: steering ESP-NOW startup failed"); return;
  }
  if (esp_now_register_recv_cb(onSteeringAngle) != ESP_OK) {
    esp_now_deinit(); Serial.println("ERROR: steering receive callback failed"); return;
  }
  steeringRadioReady = true;
  Serial.printf("Steering ESP-NOW listening on channel %u | float angle + uint32 sequence (8 bytes)\n", STEERING_WIFI_CHANNEL);
}

#define ACCELERATOR_PIN 34
#define MOTOR_LPWM 32
#define MOTOR_RPWM 33
const int MOTOR_SPEED = 60; // User-tested 8-bit analogWrite duty, not percent.
static_assert(MOTOR_SPEED == STEERING_PWM, "Steering protocol PWM must match motor configuration");
SteeringControl steering;
uint8_t appliedDirection = 255;
uint32_t lastSteeringStatus = 0;
bool steeringStatusPending = true;

void stopMotor() {
  analogWrite(MOTOR_LPWM, 0);
  analogWrite(MOTOR_RPWM, 0);
}
void applySteering() {
  if (appliedDirection == steering.direction) return;
  stopMotor();
  if (steering.direction == 1) analogWrite(MOTOR_LPWM, MOTOR_SPEED);
  else if (steering.direction == 2) analogWrite(MOTOR_RPWM, MOTOR_SPEED);
  appliedDirection = steering.direction;
  steeringStatusPending = true;
}
void serviceSteering() {
  const uint32_t previousOutages = steeringFeedback.outages;
  portENTER_CRITICAL(&steeringFeedbackMux);
  steeringFeedback = wirelessSteeringFeedback;
  portEXIT_CRITICAL(&steeringFeedbackMux);
  // A new packet must not hide a timed-out link from a delayed loop iteration.
  if (steering.direction && previousOutages != steeringFeedback.outages) {
    steering.stop(5); steeringStatusPending = true;
  }
  if (steering.setFeedbackAvailable(steeringRadioReady && steeringFeedback.fresh(millis()))) steeringStatusPending = true;
  if (steering.tick(millis())) steeringStatusPending = true;
  applySteering();
}

#define CAN_SCK  18
#define CAN_MISO 19
#define CAN_MOSI 23
#define CAN_CS   25

#define CAN_SPEED   CAN_500KBPS
#define CAN_CRYSTAL MCP_8MHZ  // Match actual module crystal

const unsigned long ACCEL_CAN_ID = 0x301;
const unsigned long SEND_INTERVAL_MS = 50;
const unsigned long PRINT_INTERVAL_MS = 250;

MCP_CAN CAN(CAN_CS);

unsigned long lastSend = 0;
unsigned long lastPrint = 0;
bool canReady = false;
unsigned long txOK = 0, txFailed = 0, lastTxAt = 0;

RemoteDriveGate driveGate;
uint32_t driveStatusAt=0, drivePeerAt=0, drivePeerSession=0;
bool drivePeerNormal=false, drivePeerSeen=false;
int acceleratorADC = 0;
bool lastSendOK = false;

void receiveSteering() {
  // Bound receive work so a busy bus cannot starve the motor timeout.
  for (uint8_t n=0; n<4 && canReady && CAN.checkReceive() == CAN_MSGAVAIL; ++n) {
    unsigned long id = 0; byte len = 0, data[8] = {};
    if (CAN.readMsgBuf(&id, &len, data) != CAN_OK) break;
    if(id==DRIVE_SWITCH_STATUS_ID && len==8 && driveStatusValid(data,127)) {
      drivePeerNormal=(data[0]&0x7F)==0; drivePeerSession=driveRead32(data+4);
      drivePeerAt=millis(); drivePeerSeen=true;
    }
    if(id==DRIVE_GATE_ID) {
      driveGate.accept(data,len,1,acceleratorADC<=DRIVE_IDLE_ADC,
        drivePeerSeen && drivePeerNormal && drivePeerSession==driveGate.session && millis()-drivePeerAt<350,millis());
    }
    if (id == STEERING_COMMAND_ID) {
      serviceSteering();
      const uint32_t before = steering.token;
      steering.accept(data, len, millis());
      if (before != steering.token) steeringStatusPending = true;
      applySteering();
    }
    serviceSteering();
  }
}
void sendDriveGateStatus() {
  if(!canReady || millis()-driveStatusAt<100) return;
  byte data[8]; driveGate.status(data,acceleratorADC); driveStatusAt=millis();
  if(CAN.sendMsgBuf(DRIVE_PEDAL_STATUS_ID,0,8,data)==CAN_OK) { ++txOK; lastTxAt=millis(); }
  else {
    ++txFailed; driveGate.lock();
    if (steering.direction) { steering.stop(1); applySteering(); }
  }
}
void sendSteeringStatus() {
  if (!canReady || (!steeringStatusPending && millis()-lastSteeringStatus < 100)) return;
  byte data[8]; steering.status(data);
  lastSteeringStatus = millis();
  if (CAN.sendMsgBuf(STEERING_STATUS_ID, 0, 8, data) == CAN_OK) {
    steeringStatusPending = false; ++txOK; lastTxAt=millis();
  } else {
    ++txFailed;
    if (steering.direction) { steering.stop(1); applySteering(); }
  }
}

void sendSteeringAngle() {
  const uint32_t now = millis();
  const bool live = steeringFeedback.fresh(now);
  if (!canReady || (live == steeringAngleWasFresh && now-lastSteeringAngleStatus < 50)) return;
  byte data[8]; steeringFeedback.status(data, now);
  lastSteeringAngleStatus = now; steeringAngleWasFresh = live;
  if (CAN.sendMsgBuf(STEERING_ANGLE_ID, 0, 8, data) == CAN_OK) { ++txOK; lastTxAt=millis(); }
  else {
    ++txFailed;
    if (steering.direction) { steering.stop(1); applySteering(); }
  }
}

void setup() {
  pinMode(MOTOR_LPWM, OUTPUT); pinMode(MOTOR_RPWM, OUTPUT);
  stopMotor();
  steering.token = esp_random(); if (!steering.token) steering.token = 1;
  applySteering();
  Serial.begin(115200);
  setupSteeringReceiver();
  Serial.println("Serial 115200 | CAN 500 kbps / MCP2515 8 MHz | SPI SCK=18 MISO=19 MOSI=23 CS=25");
  analogReadResolution(12);
  pinMode(ACCELERATOR_PIN, INPUT);

  Serial.println("\n=== NODE 1: ACCELERATOR SENDER ===");
  Serial.println("GPIO 34 | CAN ID 0x301 | DLC 2");

  SPI.begin(CAN_SCK, CAN_MISO, CAN_MOSI, CAN_CS);

  if (CAN.begin(MCP_ANY, CAN_SPEED, CAN_CRYSTAL) == CAN_OK &&
      CAN.setMode(MCP_NORMAL) == CAN_OK) {
    canReady = true;
    Serial.println("CAN initialized. Waiting for accelerator input.");
  } else {
    Serial.println("ERROR: CAN initialization failed.");
  }
}

void loop() {
  acceleratorADC = analogRead(ACCELERATOR_PIN);
  driveGate.tick(millis(),acceleratorADC<=DRIVE_IDLE_ADC);
  serviceSteering(); receiveSteering(); serviceSteering();
  unsigned long now = millis();

  if (canReady && driveGate.phase!=1 && now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    byte data[2];
    const uint16_t transmittedADC=driveGate.phase ? 0 : acceleratorADC;
    data[0] = transmittedADC & 0xFF;
    data[1] = (transmittedADC >> 8) & 0xFF;

    lastSendOK =
      (CAN.sendMsgBuf(ACCEL_CAN_ID, 0, 2, data) == CAN_OK);
    if (lastSendOK) { ++txOK; lastTxAt = millis(); }
    else { ++txFailed; if (steering.direction) { steering.stop(1); applySteering(); } }
    if (lastSendOK) Serial.printf("[%lu ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=%d/4095 | raw pin voltage estimate=%.3f V\n",
                                  millis(), transmittedADC, throttleRawVoltage(transmittedADC));
  }

  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;

    Serial.printf("[%lu ms] Node 1 | TX OK=%lu failed=%lu | ", now, txOK, txFailed);
    if (steeringFeedback.fresh(now)) Serial.printf("Steering=%.2f deg seq=%lu age=%lu ms | ", steeringFeedback.angle, (unsigned long)steeringFeedback.sequence, (unsigned long)(now-steeringFeedback.receivedAt));
    else Serial.printf("Steering angle UNAVAILABLE (%s) | ", steeringRadioReady ? "waiting/stale" : "radio failed");
    Serial.printf("IMU accepted=%lu invalid=%lu replayed=%lu | ", (unsigned long)steeringFeedback.accepted, (unsigned long)steeringFeedback.invalid, (unsigned long)steeringFeedback.replayed);
    float voltage = throttleRawVoltage(acceleratorADC);

    Serial.print("Accelerator ADC: ");
    Serial.print(acceleratorADC);
    Serial.print("/4095 | Raw pin voltage estimate: ");
    Serial.print(voltage, 3);
    Serial.print(" V | CAN: ");

    if (!canReady) {
      Serial.println("NOT INITIALIZED");
    } else {
      Serial.println(lastSendOK ? "TX OK" : "TX FAILED - check CAN wiring, termination and peer ACK");
      if (txOK) Serial.printf("Last successful send: %lu ms ago\n", millis() - lastTxAt);
    }
  }

  driveGate.tick(millis(),acceleratorADC<=DRIVE_IDLE_ADC);
  sendDriveGateStatus();
  serviceSteering(); sendSteeringAngle(); serviceSteering(); sendSteeringStatus(); serviceSteering();
  delay(1);
}
