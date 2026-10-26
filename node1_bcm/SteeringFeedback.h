#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

constexpr uint32_t STEERING_ANGLE_ID = 0x100;
constexpr uint32_t WIRELESS_HORN_STATUS_ID = 0x103;
// Wheel-mounted pitch is display telemetry only, never a chassis incline input.
constexpr uint32_t WHEEL_PITCH_ID = 0x102;
constexpr uint32_t STEERING_FEEDBACK_TIMEOUT_MS = 250;
// Match the supplied IMU sender's constrained angle range.
constexpr int16_t STEERING_MAX_ANGLE_DEG = 270;

// Exact wire format sent by the steering-wheel ESP32 (ESP-NOW, little-endian).
struct __attribute__((packed)) IMUData {
  float steeringAngle;
  uint32_t sequence;
};
static_assert(sizeof(float) == 4 && sizeof(IMUData) == 8, "Steering IMU packet must be 8 bytes");
// New sender appends one packed bool. Read it as a byte to validate its wire
// representation before converting to bool; keep the old 8-byte sender working.
struct __attribute__((packed)) IMUDataWithHorn {
  float steeringAngle;
  uint32_t sequence;
  uint8_t hornPressed;
};
static_assert(sizeof(IMUDataWithHorn) == 9, "Steering IMU + horn packet must be 9 bytes");
// The supplied 9-byte packet plus an explicit angle-valid byte. Used while
// calibrating or after I2C failure so horn traffic cannot freshen an old angle.
struct __attribute__((packed)) IMUDataWithHornValidity {
  float steeringAngle;
  uint32_t sequence;
  uint8_t hornPressed;
  uint8_t angleValid;
};
static_assert(sizeof(IMUDataWithHornValidity) == 10, "Horn + angle validity packet must be 10 bytes");
struct __attribute__((packed)) IMUDataWithPitch {
  float steeringAngle;
  uint32_t sequence;
  uint8_t hornPressed;
  float pitchAngle;
  uint8_t pitchValid;
};
static_assert(sizeof(IMUDataWithPitch) == 14, "Steering IMU + horn + pitch packet must be 14 bytes");

inline int16_t steeringAngleTenths(const uint8_t *d) {
  return (int16_t)(((uint16_t)d[0] << 8) | d[1]);
}
inline uint32_t steeringAngleSequence(const uint8_t *d) {
  return (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
}
inline bool steeringAngleFrameValid(const uint8_t *d) {
  const int16_t angle = steeringAngleTenths(d);
  return angle >= -STEERING_MAX_ANGLE_DEG * 10 && angle <= STEERING_MAX_ANGLE_DEG * 10 && d[3] <= 1 &&
    (!d[3] || d[2] < STEERING_FEEDBACK_TIMEOUT_MS);
}
inline bool steeringAngleFrameFresh(const uint8_t *d, uint32_t canAgeMs) {
  return d[3] == 1 && canAgeMs < STEERING_FEEDBACK_TIMEOUT_MS &&
    d[2] < STEERING_FEEDBACK_TIMEOUT_MS - canAgeMs;
}
inline bool wheelPitchFrameValid(const uint8_t *d) {
  return steeringAngleFrameValid(d) && steeringAngleTenths(d)>=-900 && steeringAngleTenths(d)<=900;
}
// Node 1 GPIO command, not relay-contact feedback. Version 2:
// d0=combined relay; d1 bits=wireless known, wireless pressed, website held;
// d2=wireless age; d3=2; d4..7=website command token LE32.
inline bool wirelessHornFrameValid(const uint8_t *d) {
  return d[0]<=1 && d[1]<=7 && d[3]==2 &&
    d[0]==((d[1]&6)?1:0) && (!(d[1]&2) || (d[1]&1)) &&
    (!(d[1]&1) || d[2]<STEERING_FEEDBACK_TIMEOUT_MS);
}
inline bool wirelessHornInputFresh(const uint8_t *d,uint32_t canAgeMs) {
  return (d[1]&1) && canAgeMs<STEERING_FEEDBACK_TIMEOUT_MS &&
    d[2]<STEERING_FEEDBACK_TIMEOUT_MS-canAgeMs;
}

struct SteeringFeedback {
  float angle = 0;
  uint32_t sequence = 0, receivedAt = 0, accepted = 0, invalid = 0, replayed = 0;
  uint32_t outages = 0;
  bool seen = false, angleAvailable = false;
  bool hornAvailable = false, hornPressed = false;
  float pitch = 0;
  bool pitchAvailable = false;

  bool packetFresh(uint32_t now) const { return seen && now - receivedAt < STEERING_FEEDBACK_TIMEOUT_MS; }
  bool fresh(uint32_t now) const { return angleAvailable && packetFresh(now); }
  bool hornActive(uint32_t now) const { return packetFresh(now) && hornAvailable && hornPressed; }

  bool accept(const uint8_t *bytes, int len, uint32_t now) {
    if (!bytes || (len != (int)sizeof(IMUData) && len != (int)sizeof(IMUDataWithHorn) && len != (int)sizeof(IMUDataWithHornValidity) && len != (int)sizeof(IMUDataWithPitch))) { ++invalid; return false; }
    const bool hasValidity = len == (int)sizeof(IMUDataWithHornValidity);
    if (hasValidity && bytes[9] > 1) { ++invalid; return false; }
    const bool hasPitch = len == (int)sizeof(IMUDataWithPitch);
    const bool hasHorn = len >= (int)sizeof(IMUDataWithHorn);
    if (hasHorn && bytes[8] > 1) { ++invalid; return false; }
    IMUDataWithPitch extended = {};
    if (hasPitch) {
      memcpy(&extended, bytes, sizeof(extended));
      if (extended.pitchValid>1 || !isfinite(extended.pitchAngle) || fabsf(extended.pitchAngle)>90) { ++invalid; return false; }
    }
    IMUData packet;
    memcpy(&packet, bytes, sizeof(packet));
    if (!isfinite(packet.steeringAngle) || packet.steeringAngle < -STEERING_MAX_ANGLE_DEG || packet.steeringAngle > STEERING_MAX_ANGLE_DEG) {
      ++invalid; return false;
    }
    const uint32_t advance = packet.sequence - sequence;
    // A stationary angle still needs advancing sequences. Allow a transmitter
    // reboot/reset after the link timeout, when the motor is already stopped.
    if (seen && (advance == 0 || (packetFresh(now) && advance >= 0x80000000UL))) {
      ++replayed; return false;
    }
    if (seen && !packetFresh(now)) ++outages;
    angleAvailable = !hasValidity || bytes[9] == 1;
    angle = packet.steeringAngle; sequence = packet.sequence; receivedAt = now;
    hornAvailable = hasHorn; hornPressed = hasHorn && bytes[8] == 1;
    pitchAvailable = hasPitch && extended.pitchValid;
    pitch = pitchAvailable ? extended.pitchAngle : 0;
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
  void pitchStatus(uint8_t *d, uint32_t now) const {
    status(d, now);
    const uint16_t encoded = (uint16_t)(int16_t)lroundf(pitch*10);
    d[0]=encoded>>8; d[1]=encoded;
    d[3]=fresh(now) && pitchAvailable ? 1 : 0;
  }
  void hornStatus(uint8_t *d,uint32_t now) const {
    status(d,now);
    d[0]=hornActive(now)?1:0;
    d[1]=packetFresh(now) && hornAvailable?1:0;
    d[1]|=hornActive(now)?2:0;
    d[3]=2;
  }
};
