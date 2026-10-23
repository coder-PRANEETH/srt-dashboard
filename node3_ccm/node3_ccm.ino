/*
 * NODE 3 - SWITCH CAN SENDER
 * CAN ID: 0x300 | DLC: 1
 *
 * Bit 0: Cruise
 * Bit 1: HillHold
 * Bit 2: Autonomous
 * Bit 3: Remote mode
 * Bit 4: Horn
 * Bit 5: Reverse
 * Bits 6-7: Reserved (zero)
 *
 * Switch ON = GPIO connected to GND.
 */

#include <SPI.h>
#include <mcp_can.h>
#include "RemoteDrive.h"

#define DPDT1_A  4
#define DPDT1_B  5
#define DPDT2_A  13
#define DPDT2_B  14
#define PUSH_BTN 26
#define SPST1    33

#define CAN_SCK  18
#define CAN_MISO 19
#define CAN_MOSI 23
#define CAN_CS   25

#define CAN_SPEED   CAN_500KBPS
#define CAN_CRYSTAL MCP_8MHZ  // Match actual module crystal

const unsigned long SWITCH_CAN_ID = 0x300;
const unsigned long DEBOUNCE_MS = 25;
const unsigned long SEND_INTERVAL_MS = 1000;
const unsigned long PRINT_INTERVAL_MS = 250;

const byte switchPins[SWITCH_COUNT] = {
  DPDT1_A, DPDT1_B, DPDT2_A, DPDT2_B,
  PUSH_BTN, SPST1
};

const char* switchNames[SWITCH_COUNT] = {
  "Cruise", "HillHold", "Autonomous", "Remote mode",
  "Horn", "Reverse"
};

bool rawState[SWITCH_COUNT];
bool stableState[SWITCH_COUNT];
unsigned long rawChangedAt[SWITCH_COUNT];

MCP_CAN CAN(CAN_CS);
RemoteDriveGate driveGate;
uint32_t driveStatusAt=0;

bool canReady = false;
unsigned long txOK = 0, txFailed = 0, lastTxAt = 0;
bool sendPending = true;
unsigned long lastSend = 0;
unsigned long lastPrint = 0;
unsigned long lastAttempt = 0;

byte buildPayload() {
  byte payload = 0;

  for (byte i = 0; i < SWITCH_COUNT; i++) {
    if (stableState[i]) {
      payload |= (1U << i);
    }
  }

  return payload;
}

void receiveDriveGate() {
  for(uint8_t n=0;n<4 && CAN.checkReceive()==CAN_MSGAVAIL;++n) {
    unsigned long id=0; byte len=0,data[8]={};
    if(CAN.readMsgBuf(&id,&len,data)!=CAN_OK) break;
    if(id==DRIVE_GATE_ID) {
      const bool releaseMatches=bool(buildPayload()&SWITCH_REVERSE)==driveGate.reverse;
      driveGate.accept(data,len,3,data[0]==1 || releaseMatches,false,millis());
    }
  }
  driveGate.tick(millis());
}
void sendDriveGateStatus() {
  if(millis()-driveStatusAt<100) return;
  byte data[8]; driveGate.status(data,buildPayload()); driveStatusAt=millis();
  if(CAN.sendMsgBuf(DRIVE_SWITCH_STATUS_ID,0,8,data)==CAN_OK) { ++txOK; lastTxAt=millis(); }
  else { ++txFailed; driveGate.lock(); }
}
void printSwitchStates(byte payload) {
  for (byte i = 0; i < SWITCH_COUNT; ++i) {
    if (i) Serial.print(" | ");
    Serial.print(switchNames[i]); Serial.print(": ");
    Serial.print((payload & (1U << i)) ? "ON" : "OFF");
  }
  Serial.println();
}

void initializeCAN() {
  lastAttempt = millis();

  if (CAN.begin(MCP_ANY, CAN_SPEED, CAN_CRYSTAL) == CAN_OK &&
      CAN.setMode(MCP_NORMAL) == CAN_OK) {
    canReady = true;
    sendPending = true;
    Serial.println("CAN initialized: ID 0x300, DLC 1");
  } else {
    canReady = false;
    Serial.println("CAN initialization failed; will retry.");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Serial 115200 | CAN 500 kbps / MCP2515 8 MHz | SPI SCK=18 MISO=19 MOSI=23 CS=25");

  Serial.println("\n=== NODE 3: SWITCH SENDER ===");

  Serial.println("Switch GPIOs: cruise=4 hillHold=5 autonomous=13 remoteMode=14 horn=26 reverse=33; LOW=ON");
  unsigned long now = millis();

  for (byte i = 0; i < SWITCH_COUNT; i++) {
    pinMode(switchPins[i], INPUT_PULLUP);
    stableState[i] = (digitalRead(switchPins[i]) == LOW);
    rawState[i] = stableState[i];
    rawChangedAt[i] = now;
  }

  SPI.begin(CAN_SCK, CAN_MISO, CAN_MOSI, CAN_CS);
  initializeCAN();
}

void loop() {
  unsigned long now = millis();

  for (byte i = 0; i < SWITCH_COUNT; i++) {
    bool current = (digitalRead(switchPins[i]) == LOW);

    if (current != rawState[i]) {
      rawState[i] = current;
      rawChangedAt[i] = now;
    }

    if (rawState[i] != stableState[i] &&
        now - rawChangedAt[i] >= DEBOUNCE_MS) {
      stableState[i] = rawState[i];
      sendPending = true;

      Serial.print(switchNames[i]);
      Serial.print(": ");
      Serial.println(stableState[i] ? "ON" : "OFF");
    }
  }

  if (!canReady) {
    if (now - lastAttempt >= 1000) {
      initializeCAN();
    }
    delay(1);
    return;
  }

  receiveDriveGate(); sendDriveGateStatus();
  if (driveGate.phase!=1 && (sendPending || now - lastSend >= (driveGate.phase ? 100 : SEND_INTERVAL_MS))) {
    byte payload = driveGate.phase ? driveSwitchMask(buildPayload(),driveGate.reverse) : buildPayload();

    if (CAN.sendMsgBuf(SWITCH_CAN_ID, 0, 1, &payload) == CAN_OK) {
      ++txOK; lastTxAt = millis();
      Serial.printf("[%lu ms] CAN TX ID=0x300 DLC=1 | ", millis());
      printSwitchStates(payload);
      sendPending = false;
      lastSend = millis();
    } else {
      ++txFailed;
      sendPending = true;
      // Retry on the next loop without repeatedly reinitializing CAN.
    }
  }

  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;
    byte payload = buildPayload();

    Serial.printf("[%lu ms] Node 3 | TX OK=%lu failed=%lu pending=%s | ", now, txOK, txFailed, sendPending ? "YES" : "NO");
    Serial.print("Switch inputs | ");
    printSwitchStates(payload);
    Serial.println(sendPending ? "CAN TX pending/failed: check bus wiring, termination and peer ACK" : "CAN TX OK");
    if (txOK) Serial.printf("Last successful send: %lu ms ago\n", millis() - lastTxAt);
  }

  delay(1);
}
