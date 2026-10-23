#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>
#include "SwitchSignals.h"

#ifndef CAN_FRAME_SERIAL_LOG
#define CAN_FRAME_SERIAL_LOG 1
#endif

// Classic ESP32 + MCP2515. Match MCP_8MHZ to the oscillator actually fitted.
constexpr uint8_t CAN_CS_PIN = 25;
constexpr uint8_t CAN_INT_PIN = 27;
static MCP_CAN canBus(CAN_CS_PIN);
static bool canReady = false;
static uint8_t canTxCount = 0, canRxCount = 0;
static uint32_t canTxTotal = 0, canRxTotal = 0, canTxFailed = 0, canLastRxAt = 0;

inline bool canBegin(bool loopback = false) {
  canReady = false;
  Serial.printf("CAN setup: %s, 500 kbps, MCP2515 8 MHz | SCK=18 MISO=19 MOSI=23 CS=25 INT=27\n",
                loopback ? "INTERNAL LOOPBACK" : "PHYSICAL BUS");
  pinMode(CAN_INT_PIN, INPUT);
  SPI.begin(18, 19, 23, CAN_CS_PIN);
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    if (canBus.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK &&
        canBus.setMode(loopback ? MCP_LOOPBACK : MCP_NORMAL) == CAN_OK) {
      canReady = true;
      Serial.println("CAN controller READY");
      return true;
    }
    Serial.printf("CAN init attempt %u/5 FAILED\n", attempt + 1);
    delay(200);
  }
  Serial.println("CAN unavailable: check power, SPI pins and MCP2515 crystal");
  return false;
}
inline void canPrintStatus(const char *node) {
  Serial.printf("[%lu ms] %s | CAN=%s TX=%lu RX=%lu TX failed=%lu | last RX=",
                (unsigned long)millis(), node, canReady ? "READY" : "UNAVAILABLE",
                (unsigned long)canTxTotal, (unsigned long)canRxTotal, (unsigned long)canTxFailed);
  if (canRxTotal) Serial.printf("%lu ms ago\n", (unsigned long)(millis() - canLastRxAt));
  else Serial.println("none yet");
}
inline void canReportTxFailure(uint32_t id) {
  ++canTxFailed;
  static uint32_t lastReport = 0;
  static bool reported = false;
  if (!reported || millis() - lastReport >= 1000) {
    reported = true; lastReport = millis();
    Serial.printf("[%lu ms] CAN TX FAILED ID=0x%03lX | failures=%lu | %s\n",
                  (unsigned long)millis(), (unsigned long)id, (unsigned long)canTxFailed,
                  canReady ? "check CANH/CANL, termination and an active peer for ACK" : "controller not initialized");
  }
}
// Decode the payload that was actually sent, including its CAN precision.
// Keep copies identical across the standalone Arduino sketch folders.
inline uint16_t canLogU16(const uint8_t *b) { return ((uint16_t)b[0] << 8) | b[1]; }
inline const char *canLogOn(uint8_t value) { return value ? "ON" : "OFF"; }
inline void canPrintFrame(uint32_t id, const uint8_t (&d)[8], uint8_t len, const char *direction) {
  if (!CAN_FRAME_SERIAL_LOG) return;
  // Serial is slower than the bus. Limit repetitive console lines, not CAN
  // traffic/counters or Node 5's complete bounded web log.
  const uint32_t fastIds[]={0x100,0x110,0x111,0x120,0x121,0x122,0x123,0x124,0x201,0x300,0x301};
  static uint32_t printedAt[11][2]={}; static bool printed[11][2]={};
  const uint8_t side=direction[0]=='T'?1:0;
  for(uint8_t n=0;n<11;++n) if(id==fastIds[n]) {
    if(printed[n][side] && millis()-printedAt[n][side]<250) return;
    printed[n][side]=true;printedAt[n][side]=millis();break;
  }
  Serial.printf("[%lu ms] CAN %s ID=0x%03lX DLC=%u | ", (unsigned long)millis(), direction, (unsigned long)id, len);
  if (id == 0x300 && len == 1) {
    const char *names[] = {"cruise", "hillHold", "autonomous", "remoteMode", "horn", "reverse"};
    for (uint8_t bit = 0; bit < SWITCH_COUNT; ++bit)
      Serial.printf("%s%s=%s", bit ? " " : "", names[bit], canLogOn(d[0] & (1U << bit)));
    Serial.println(); return;
  }
  if (id == 0x301 && len == 2) {
    const uint16_t adc = d[0] | ((uint16_t)d[1] << 8);
    Serial.printf("accelerator ADC=%u/4095 raw pin voltage estimate=%.3f V\n", adc, adc * (3.3f / 4095.0f));
    return;
  }
  if (id == 0x300 || id == 0x301 || len != 8) {
    Serial.println("unexpected payload length"); return;
  }
  switch (id) {
    case 0x110:
    case 0x111:
      Serial.printf("steering direction=%u PWM=%u/255%s\n",d[0],d[1],id==0x111 ? " (applied command)" : "");
      break;
    case 0x123:
      Serial.printf("joystick traction demand=%u/255 (above idle) reverse=%s sequence=%u\n",d[0],canLogOn(d[1]),d[2] | ((uint16_t)d[3]<<8));
      break;
    case 0x124:
      Serial.printf("motor ownership=%u reverse=%s applied PWM=%u/255\n",d[0]&127,canLogOn(d[0]&128),d[1] | ((uint16_t)d[2]<<8));
      break;
    case 0x100:
      if (d[3]) Serial.printf("steering angle=%.1f deg source age=%u ms link=LIVE\n", (int16_t)canLogU16(d) / 10.0, d[2]);
      else Serial.println("steering angle=unavailable link=STALE/MISSING");
      break;
    case 0x201:
      Serial.printf("motor1=%s motor2=%s reverse=%s throttle=%u%% cruise=%s ECO=%s hill=%s stop reason=%u\n",
                    canLogOn(d[0]), canLogOn(d[1]), canLogOn(d[2]), d[3],
                    canLogOn(d[4]), canLogOn(d[5]), canLogOn(d[6]), d[7]);
      break;
    case 0x7A3:
      Serial.printf("switches cruise=%s ECO=%s reverse=%s hill=%s horn=%s auto=%s lights=%s",
                    canLogOn(d[0]), canLogOn(d[1]), canLogOn(d[2]), canLogOn(d[3]),
                    canLogOn(d[4]), canLogOn(d[5]), canLogOn(d[6]));
      if (id == 0x7A3) Serial.printf(" loopback marker=%s", d[7] == 0xA5 ? "OK" : "INVALID");
      Serial.println();
      break;
    case 0x400:
      if (d[6]) Serial.printf("GPS latitude=%.6f deg longitude=%.6f deg fix=YES satellites=%u\n",
                              d[0] + canLogU16(d + 1) / 6000.0,
                              d[3] + canLogU16(d + 4) / 6000.0, d[7]);
      else Serial.printf("GPS fix=NO latitude=unavailable longitude=unavailable satellites=%u\n", d[7]);
      break;
    case 0x401:
      Serial.printf("ignition=%s on-time=%u s\n", canLogOn(d[0]), canLogU16(d + 1));
      break;
    case 0x500:
      Serial.printf("hill request=%s hold=%u%% pitch=", canLogOn(d[0]), d[2]);
      if (d[3]) Serial.printf("%d deg valid=YES\n", (int)d[1] - 90);
      else Serial.println("unavailable valid=NO");
      break;
    case 0x600:
      Serial.printf("node=%u status=%s uptime=%u s (mod 256) error=%s TX=%u RX=%u (mod 256)\n",
                    d[0], d[1] ? "OK" : "ERROR", d[2],
                    d[3] == 0 ? "none" : d[3] == 1 ? "CAN unavailable" :
                    d[0] == 4 && d[3] == 2 ? "MPU6050 missing" : "unknown",
                    d[4], d[5]);
      break;
    case 0x7A1:
    case 0x7A2:
    case 0x7A4:
    case 0x7A5:
      Serial.printf("loopback node=%u sequence=%u marker=%s\n",
                    d[0], d[1], d[2] == 0xA5 ? "OK" : "INVALID");
      break;
    default:
      Serial.println("no signal definition for this ID");
      break;
  }
}

template <size_t N>
inline bool canSend(uint32_t id, const uint8_t (&data)[N]) {
  static_assert(N >= 1 && N <= 8, "Classic CAN payload must be 1..8 bytes");
  if (!canReady) { canReportTxFailure(id); return false; }
  uint8_t copy[8] = {};
  memcpy(copy, data, N);
  if (canBus.sendMsgBuf(id, 0, N, copy) != CAN_OK) { canReportTxFailure(id); return false; }
  ++canTxCount;
  ++canTxTotal;
  canPrintFrame(id, copy, N, "TX");
  return true;
}
inline bool canRead(uint32_t &id, uint8_t &len, uint8_t (&data)[8]) {
  if (!canReady || canBus.checkReceive() != CAN_MSGAVAIL) return false;
  INT32U raw;
  INT8U size;
  INT8U bytes[8] = {};
  if (canBus.readMsgBuf(&raw, &size, bytes) != CAN_OK) return false;
  id = raw & 0x1FFFFFFFUL;
  len = min((uint8_t)size, (uint8_t)8);
  memcpy(data, bytes, 8);
  ++canRxCount; ++canRxTotal; canLastRxAt = millis();
  canPrintFrame(id, data, len, "RX");
  return true;
}
inline void canHeartbeat(uint8_t node, uint8_t status = 1, uint8_t error = 0) {
  uint8_t data[8] = {node, status, (uint8_t)(millis() / 1000), error,
                     canTxCount, canRxCount, 0, 0};
  canSend(0x600, data);
}
