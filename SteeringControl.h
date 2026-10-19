#pragma once
#include <stdint.h>

constexpr uint32_t STEERING_COMMAND_ID = 0x110;
constexpr uint32_t STEERING_STATUS_ID = 0x111;
constexpr uint8_t STEERING_PWM = 60;
constexpr uint32_t STEERING_TIMEOUT_MS = 350;
constexpr uint32_t STEERING_HOLD_MS = 5000;
inline uint32_t steeringRead32(const uint8_t *d) {
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}
inline void steeringWrite32(uint8_t *d, uint32_t value) {
  for (uint8_t n=0; n<4; ++n) d[n] = value >> (8*n);
}
// Reasons: 0=normal, 1=CAN failure, 2=command timeout, 3=invalid command, 4=hold limit.
// Tokens change after every stop. Delayed commands cannot restart a stopped motor.
struct SteeringControl {
  uint8_t direction = 0, reason = 0;
  uint32_t token = 1, lastCommandAt = 0, heldAt = 0;
  uint16_t sequence = 0;
  bool sequenced = false;
  void stop(uint8_t why = 0) {
    direction = 0; reason = why; sequenced = false;
    if (++token == 0) ++token;
  }
  bool tick(uint32_t now) {
    if (!direction) return false;
    if (now - lastCommandAt >= STEERING_TIMEOUT_MS) { stop(2); return true; }
    if (now - heldAt >= STEERING_HOLD_MS) { stop(4); return true; }
    return false;
  }
  bool accept(const uint8_t *d, uint8_t len, uint32_t now) {
    tick(now);
    if (len != 8 || d[0] > 2 || (d[0] ? d[1] != STEERING_PWM : d[1] != 0)) { stop(3); return false; }
    if (!d[0]) { stop(); return true; } // STOP is always available.
    const uint16_t next = d[2] | ((uint16_t)d[3] << 8);
    if (steeringRead32(d+4) != token) return false;
    const uint16_t advance = next - sequence;
    if (sequenced && (advance == 0 || advance >= 0x8000)) return false;
    if (direction && direction != d[0]) { stop(3); return false; }
    if (!direction) heldAt = now;
    direction = d[0]; reason = 0; lastCommandAt = now; sequence = next; sequenced = true;
    return true;
  }
  void status(uint8_t *d) const {
    d[0] = direction; d[1] = direction ? STEERING_PWM : 0; d[2] = reason; d[3] = 1;
    steeringWrite32(d+4, token);
  }
};

// Dashboard lease is independent of Node 1's watchdog. One gesture owns one
// token; release, timeout, failed CAN or changed Node 1 boot token invalidates it.
struct SteeringRelay {
  uint32_t token = 1, nodeToken = 0, lastCommandAt = 0;
  uint16_t sequence = 0;
  uint8_t direction = 0;
  void stop() { direction = 0; nodeToken = 0; sequence = 0; if (++token == 0) ++token; }
  bool tick(uint32_t now) {
    if (direction && now-lastCommandAt >= STEERING_TIMEOUT_MS) { stop(); return true; }
    return false;
  }
  bool accept(uint8_t dir, uint16_t seq, uint32_t lease, uint32_t node, bool ready, uint32_t now) {
    tick(now);
    if (!ready || !node || lease != token || dir < 1 || dir > 2 || !seq || seq <= sequence) return false;
    if (direction && (direction != dir || nodeToken != node)) return false;
    direction = dir; sequence = seq; nodeToken = node; lastCommandAt = now;
    return true;
  }
};
