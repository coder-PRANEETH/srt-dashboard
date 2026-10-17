#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>

// Classic ESP32 + MCP2515. Match MCP_8MHZ to the oscillator actually fitted.
constexpr uint8_t CAN_CS_PIN = 25;
constexpr uint8_t CAN_INT_PIN = 27;
static MCP_CAN canBus(CAN_CS_PIN);
static bool canReady = false;
static uint8_t canTxCount = 0, canRxCount = 0;

inline bool canBegin(bool loopback = false) {
  pinMode(CAN_INT_PIN, INPUT);
  SPI.begin(18, 19, 23, CAN_CS_PIN);
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    if (canBus.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK) {
      canBus.setMode(loopback ? MCP_LOOPBACK : MCP_NORMAL);
      canReady = true;
      return true;
    }
    delay(200);
  }
  return false;
}
inline bool canSend(uint32_t id, const uint8_t (&data)[8]) {
  if (!canReady) return false;
  uint8_t copy[8];
  memcpy(copy, data, 8);
  if (canBus.sendMsgBuf(id, 0, 8, copy) != CAN_OK) return false;
  ++canTxCount;
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
  ++canRxCount;
  return true;
}
inline void canHeartbeat(uint8_t node, uint8_t status = 1, uint8_t error = 0) {
  uint8_t data[8] = {node, status, (uint8_t)(millis() / 1000), error,
                     canTxCount, canRxCount, 0, 0};
  canSend(0x600, data);
}
