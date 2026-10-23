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
  Serial.printf("Steering ESP-NOW listening on channel %u | angle + sequence (8 bytes), optional horn (9 bytes) | range +/-%d deg\n", STEERING_WIFI_CHANNEL, STEERING_MAX_ANGLE_DEG);
}

#define ACCELERATOR_PIN 34
#define MOTOR_LPWM 32
#define MOTOR_RPWM 33
const int MOTOR_SPEED = 60; // User-tested 8-bit analogWrite duty, not percent.
static_assert(MOTOR_SPEED == STEERING_PWM, "Steering protocol PWM must match motor configuration");
SteeringControl steering;
uint8_t appliedDirection = 255;
uint8_t appliedPWM = 0;
uint32_t lastSteeringStatus = 0;
bool steeringStatusPending = true;

// Local Serial Monitor motor check; normal CAN steering keeps its own rules.
constexpr uint8_t SERIAL_STEERING_STEP = 10;
constexpr uint32_t SERIAL_STEERING_RUN_MS = 5000;
uint8_t serialSteeringPWM = MOTOR_SPEED;
bool serialSteeringActive = false;
uint32_t serialSteeringStartedAt = 0;

void stopMotor() {
  analogWrite(MOTOR_LPWM, 0);
  analogWrite(MOTOR_RPWM, 0);
}
void applySteering() {
  if (appliedDirection == steering.direction && appliedPWM == steering.pwm) return;
  stopMotor();
  if (steering.direction == 1) analogWrite(MOTOR_LPWM, steering.pwm);
  else if (steering.direction == 2) analogWrite(MOTOR_RPWM, steering.pwm);
  appliedDirection = steering.direction;
  appliedPWM = steering.pwm;
  steeringStatusPending = true;
}

void stopSerialSteering(uint8_t reason = 0) {
  serialSteeringActive = false;
  steering.stop(reason); // Invalidate old CAN commands before returning control.
  steeringStatusPending = true;
  applySteering();
}
void printSerialSteering() {
  Serial.printf("Serial steering | %s | selected PWM=%u/255 | output PWM=%u/255\n",
                serialSteeringActive ? (steering.direction == 1 ? "LEFT" : "RIGHT") : "STOP",
                serialSteeringPWM, steering.pwm);
}
void serviceSerialSteering() {
  // Character input only: no blocking readString/parseInt during live CAN work.
  for (uint8_t n=0; n<16 && Serial.available(); ++n) {
    const char key = (char)Serial.read();
    if (key == '\r' || key == '\n') continue;
    if (key == 's' || key == 'S' || key == ' ') {
      stopSerialSteering(); printSerialSteering();
    } else if (key == '+' || key == '-') {
      const int next = (int)serialSteeringPWM + (key == '+' ? SERIAL_STEERING_STEP : -SERIAL_STEERING_STEP);
      serialSteeringPWM = next < 0 ? 0 : next > 255 ? 255 : next;
      if (serialSteeringActive) {
        if (!serialSteeringPWM) stopSerialSteering();
        else { steering.pwm = serialSteeringPWM; applySteering(); }
      }
      printSerialSteering();
    } else if (key == 'a' || key == 'A' || key == 'd' || key == 'D') {
      stopSerialSteering();
      if (serialSteeringPWM) {
        serialSteeringActive = true;
        serialSteeringStartedAt = millis();
        steering.direction = (key == 'a' || key == 'A') ? 1 : 2;
        steering.pwm = serialSteeringPWM;
        applySteering();
      }
      printSerialSteering();
    }
  }
}
void serviceSteering() {
  portENTER_CRITICAL(&steeringFeedbackMux);
  steeringFeedback = wirelessSteeringFeedback;
  portEXIT_CRITICAL(&steeringFeedbackMux);
  // A local motor check is independent of the wireless angle sensor. Existing
  // CAN-send fault handlers still stop it, and its run time is bounded here.
  if (serialSteeringActive) {
    if (!steering.direction) serialSteeringActive = false;
    else if (millis() - serialSteeringStartedAt >= SERIAL_STEERING_RUN_MS) {
      stopSerialSteering(4);
      Serial.println("Serial steering STOP: 5 s limit; send a/d to run again.");
    } else { applySteering(); return; }
  }
  // Angle availability is reported separately from motor-speed control.
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
unsigned long lastCanAttempt = 0;
uint32_t remoteSteeringReceived=0, remoteSteeringAccepted=0, remoteSteeringRejected=0;
bool lastSteeringStatusOK=false;

void initializeCAN() {
  lastCanAttempt = millis();
  canReady = CAN.begin(MCP_ANY, CAN_SPEED, CAN_CRYSTAL) == CAN_OK &&
             CAN.setMode(MCP_NORMAL) == CAN_OK;
  if (canReady) {
    steeringStatusPending = true;
    Serial.println("CAN initialized | remote steering RX=0x110 TX=0x111 | angle optional");
  } else {
    Serial.println("ERROR: CAN initialization failed; retrying in 1 s. Serial steering is local only.");
  }
}

void receiveSteering() {
  // Bound receive work so a busy bus cannot starve the motor timeout.
  for (uint8_t n=0; n<4 && canReady && CAN.checkReceive() == CAN_MSGAVAIL; ++n) {
    unsigned long id = 0; byte len = 0, data[8] = {};
    if (CAN.readMsgBuf(&id, &len, data) != CAN_OK) break;
    if(id==DRIVE_SWITCH_STATUS_ID && len==8 && driveStatusValid(data,SWITCH_VALID_MASK)) {
      drivePeerNormal=(data[0]&0x7F)==0; drivePeerSession=driveRead32(data+4);
      drivePeerAt=millis(); drivePeerSeen=true;
    }
    if(id==DRIVE_GATE_ID) {
      driveGate.accept(data,len,1,acceleratorADC<=DRIVE_IDLE_ADC,
        drivePeerSeen && drivePeerNormal && drivePeerSession==driveGate.session && millis()-drivePeerAt<350,millis());
    }
    if (id == STEERING_COMMAND_ID) {
      ++remoteSteeringReceived;
      serviceSteering();
      if (serialSteeringActive) {
        // Local test owns direction/speed; a valid CAN STOP still takes effect.
        if (len == 8 && data[0] == 0 && data[1] == 0) { stopSerialSteering(); ++remoteSteeringAccepted; }
        else ++remoteSteeringRejected;
      } else {
        const uint32_t before = steering.token;
        if (steering.accept(data, len, millis())) ++remoteSteeringAccepted;
        else ++remoteSteeringRejected;
        if (before != steering.token) steeringStatusPending = true;
        applySteering();
      }
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
  lastSteeringStatusOK = CAN.sendMsgBuf(STEERING_STATUS_ID, 0, 8, data) == CAN_OK;
  if (lastSteeringStatusOK) {
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
  Serial.println("Steering serial test: a=LEFT d=RIGHT +=PWM+10 -=PWM-10 s/space=STOP; PWM 0..255, starts at 60, 5 s per a/d. Angle feedback not required for local test.");
  setupSteeringReceiver();
  Serial.println("Serial 115200 | CAN 500 kbps / MCP2515 8 MHz | SPI SCK=18 MISO=19 MOSI=23 CS=25");
  analogReadResolution(12);
  pinMode(ACCELERATOR_PIN, INPUT);

  Serial.println("\n=== NODE 1: ACCELERATOR SENDER ===");
  Serial.println("GPIO 34 | CAN ID 0x301 | DLC 2");

  SPI.begin(CAN_SCK, CAN_MISO, CAN_MOSI, CAN_CS);

  initializeCAN();
}

void loop() {
  if (!canReady && millis()-lastCanAttempt >= 1000) initializeCAN();
  acceleratorADC = analogRead(ACCELERATOR_PIN);
  driveGate.tick(millis());
  serviceSteering(); receiveSteering(); serviceSteering();
  serviceSerialSteering(); serviceSteering();
  unsigned long now = millis();

  if (canReady && driveGate.phase!=1 && now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    byte data[2];
    const uint16_t transmittedADC=driveGate.phase ? DRIVE_REST_ADC : acceleratorADC;
    data[0] = transmittedADC & 0xFF;
    data[1] = (transmittedADC >> 8) & 0xFF;

    lastSendOK =
      (CAN.sendMsgBuf(ACCEL_CAN_ID, 0, 2, data) == CAN_OK);
    if (lastSendOK) { ++txOK; lastTxAt = millis(); }
    else { ++txFailed; if (steering.direction) { steering.stop(1); applySteering(); } }
    if (lastSendOK && now-lastPrint>=PRINT_INTERVAL_MS) Serial.printf("[%lu ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=%d/4095 | raw pin voltage estimate=%.3f V\n",
                                  millis(), transmittedADC, throttleRawVoltage(transmittedADC));
  }

  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;
    Serial.printf("Remote steering | RX110=%lu accepted=%lu rejected=%lu | TX111=%s | dir=%u PWM=%u token=%lu owner=%s\n",
      (unsigned long)remoteSteeringReceived,(unsigned long)remoteSteeringAccepted,(unsigned long)remoteSteeringRejected,
      lastSteeringStatusOK ? "OK" : "FAILED/NOT SENT",steering.direction,steering.pwm,(unsigned long)steering.token,
      serialSteeringActive ? "SERIAL" : "CAN");

    Serial.printf("[%lu ms] Node 1 | TX OK=%lu failed=%lu | ", now, txOK, txFailed);
    if (steeringFeedback.fresh(now)) Serial.printf("Steering=%.2f deg seq=%lu age=%lu ms | ", steeringFeedback.angle, (unsigned long)steeringFeedback.sequence, (unsigned long)(now-steeringFeedback.receivedAt));
    else Serial.printf("Steering angle UNAVAILABLE (%s) | ", steeringRadioReady ? "waiting/stale" : "radio failed");
    Serial.printf("IMU accepted=%lu invalid=%lu replayed=%lu | ", (unsigned long)steeringFeedback.accepted, (unsigned long)steeringFeedback.invalid, (unsigned long)steeringFeedback.replayed);
    Serial.printf("IMU horn=%s | ", steeringFeedback.fresh(now) && steeringFeedback.hornAvailable ?
      (steeringFeedback.hornPressed ? "PRESSED" : "RELEASED") : "UNAVAILABLE");
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

  driveGate.tick(millis());
  sendDriveGateStatus();
  serviceSteering(); sendSteeringAngle(); serviceSteering(); sendSteeringStatus(); serviceSteering();
  delay(1);
}
