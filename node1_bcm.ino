/*
 * NODE 1 - ACCELERATOR + REMOTE STEERING
 * GPIO 34: Accelerator signal input
 * CAN ID: 0x301
 * Payload: 2 bytes containing ADC value (0-4095)
 */

#include <SPI.h>
#include <mcp_can.h>
#include "ThrottleCalibration.h"
#include "SteeringControl.h"
#include "RemoteDrive.h"
#include <esp_system.h>

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
    if(id==DRIVE_SWITCH_STATUS_ID && len==8 && data[3]==0xD1) {
      drivePeerNormal=(data[0]&3)==0; drivePeerSession=driveRead32(data+4);
      drivePeerAt=millis(); drivePeerSeen=true;
    }
    if(id==DRIVE_GATE_ID) {
      driveGate.accept(data,len,1,acceleratorADC<=DRIVE_IDLE_ADC,
        drivePeerSeen && drivePeerNormal && drivePeerSession==driveGate.session && millis()-drivePeerAt<350,millis());
    }
    if (id == STEERING_COMMAND_ID) {
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
  if(CAN.sendMsgBuf(DRIVE_PEDAL_STATUS_ID,0,8,data)!=CAN_OK) driveGate.lock();
}
void sendSteeringStatus() {
  if (!canReady || (!steeringStatusPending && millis()-lastSteeringStatus < 100)) return;
  byte data[8]; steering.status(data);
  lastSteeringStatus = millis();
  if (CAN.sendMsgBuf(STEERING_STATUS_ID, 0, 8, data) == CAN_OK) {
    steeringStatusPending = false; ++txOK;
  } else {
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
    if (lastSendOK) Serial.printf("[%lu ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=%d/4095 | corrected voltage estimate=%.3f V\n",
                                  millis(), transmittedADC, throttleVoltage(transmittedADC));
  }

  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;

    Serial.printf("[%lu ms] Node 1 | TX OK=%lu failed=%lu | ", now, txOK, txFailed);
    float voltage = throttleVoltage(acceleratorADC);

    Serial.print("Accelerator ADC: ");
    Serial.print(acceleratorADC);
    Serial.print("/4095 | Corrected voltage estimate: ");
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
  serviceSteering(); sendSteeringStatus(); serviceSteering();
  delay(1);
}
