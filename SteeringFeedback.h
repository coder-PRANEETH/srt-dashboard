#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

constexpr uint32_t STEERING_ANGLE_ID = 0x100;
constexpr uint32_t STEERING_FEEDBACK_TIMEOUT_MS = 250;

// Exact wire format sent by the steering-wheel ESP32 (ESP-NOW, little-endian).
struct __attribute__((packed)) IMUData {
  float steeringAngle;
  uint32_t sequence;
};
static_assert(sizeof(float) == 4 && sizeof(IMUData) == 8, "Steering IMU packet must be 8 bytes");

inline int16_t steeringAngleTenths(const uint8_t *d) {
  return (int16_t)(((uint16_t)d[0] << 8) | d[1]);
}
inline uint32_t steeringAngleSequence(const uint8_t *d) {
  return (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
}
inline bool steeringAngleFrameValid(const uint8_t *d) {
  const int16_t angle = steeringAngleTenths(d);
  return angle >= -1800 && angle <= 1800 && d[3] <= 1 &&
    (!d[3] || d[2] < STEERING_FEEDBACK_TIMEOUT_MS);
}
inline bool steeringAngleFrameFresh(const uint8_t *d, uint32_t canAgeMs) {
  return d[3] == 1 && canAgeMs < STEERING_FEEDBACK_TIMEOUT_MS &&
    d[2] < STEERING_FEEDBACK_TIMEOUT_MS - canAgeMs;
}

struct SteeringFeedback {
  float angle = 0;
  uint32_t sequence = 0, receivedAt = 0, accepted = 0, invalid = 0, replayed = 0;
  uint32_t outages = 0;
  bool seen = false;

  bool fresh(uint32_t now) const { return seen && now - receivedAt < STEERING_FEEDBACK_TIMEOUT_MS; }

  bool accept(const uint8_t *bytes, int len, uint32_t now) {
    if (!bytes || len != (int)sizeof(IMUData)) { ++invalid; return false; }
    IMUData packet;
    memcpy(&packet, bytes, sizeof(packet));
    if (!isfinite(packet.steeringAngle) || packet.steeringAngle < -180 || packet.steeringAngle > 180) {
      ++invalid; return false;
    }
    const uint32_t advance = packet.sequence - sequence;
    // A stationary angle still needs advancing sequences. Allow a transmitter
    // reboot/reset after the link timeout, when the motor is already stopped.
    if (seen && (advance == 0 || (fresh(now) && advance >= 0x80000000UL))) {
      ++replayed; return false;
    }
    if (seen && !fresh(now)) ++outages;
    angle = packet.steeringAngle; sequence = packet.sequence; receivedAt = now;
    seen = true; ++accepted; return true;
  }

  // CAN 0x100: signed BE16 tenths of a degree, source age ms (255=unknown/
  // >=255), live flag, ESP-NOW sequence LE32. Never send stale data as live.
  void status(uint8_t *d, uint32_t now) const {
    const int16_t tenths = (int16_t)lroundf(angle * 10);
    const uint16_t encoded = (uint16_t)tenths;
    d[0] = encoded >> 8; d[1] = encoded;
    const uint32_t age = now - receivedAt;
    d[2] = !seen || age > 255 ? 255 : age;
    d[3] = fresh(now) ? 1 : 0;
    for (uint8_t n = 0; n < 4; ++n) d[4+n] = sequence >> (8*n);
  }
};
