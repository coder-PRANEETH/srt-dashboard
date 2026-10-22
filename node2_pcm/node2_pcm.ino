/*
 * NODE 2 PCM - CAN throttle receiver and reverse outputs.
 * Matches the supplied running vehicle: PWM GPIO13/14 (1 kHz, 8 bit),
 * active-LOW reverse relay inputs GPIO33/26. No local pedal/brake input.
 * MCP2515: SCK18 MISO19 MOSI23 CS25 INT27, 500 kbps, 8 MHz crystal.
 * Copy CanCommon.h and ThrottleCalibration.h into this sketch's folder.
 *
 * RX 0x301 DLC2: raw 12-bit pedal ADC, little-endian.
 * RX 0x300 DLC1: cruise/ECO/reverse/hill/horn/headlight/autonomous bits 0..6.
 * TX 0x201 DLC8: motor1, motor2, applied reverse, throttle %, cruise active,
 *               ECO selected, hill active, stop reason (see pcmReason()).
 * TX 0x600 DLC8: Node 2 heartbeat, once per second.
 *
 * Traction and reverse outputs are enabled for the supplied vehicle wiring.
 * Override either enable to 0 for a bench build without that output.
 * Cruise, hill and autonomous requests never synthesize extra throttle.
 * Direction changes require a zero pedal command for 1 s, then 1 s settling.
 * Normal PWM changes ramp linearly in either direction; faults cut immediately.
 * This checks commanded throttle, not vehicle speed or braking.
 */

#include "CanCommon.h"
#include "ThrottleCalibration.h"

#ifndef NODE2_ENABLE_TRACTION_OUTPUTS
#define NODE2_ENABLE_TRACTION_OUTPUTS 1
#endif
#ifndef NODE2_ENABLE_REVERSE_RELAYS
#define NODE2_ENABLE_REVERSE_RELAYS 1
#endif
constexpr bool ENABLE_TRACTION_OUTPUTS = NODE2_ENABLE_TRACTION_OUTPUTS;
constexpr bool ENABLE_REVERSE_RELAYS = NODE2_ENABLE_REVERSE_RELAYS;
constexpr uint8_t MOTOR_1 = 13, MOTOR_2 = 14, REVERSE_1 = 33, REVERSE_2 = 26;
constexpr uint8_t REVERSE_RELAY_ON = LOW, REVERSE_RELAY_OFF = HIGH;
// Match the running receiver: valid through 500/3000 ms; expire after that.
constexpr uint32_t ACCEL_TIMEOUT_MS = 501, SWITCH_TIMEOUT_MS = 3001;
constexpr uint32_t SHIFT_ZERO_MS = 1000, SHIFT_SETTLE_MS = 1000;
constexpr uint8_t ECO_LIMIT_PERCENT = 40;
// Full-scale ramp time. A 25% change takes 100 ms with these defaults.
constexpr uint32_t THROTTLE_RAMP_UP_MS = 400, THROTTLE_RAMP_DOWN_MS = 400;
// Discard excess elapsed time after a delayed loop instead of jumping the PWM.
constexpr uint32_t THROTTLE_RAMP_MAX_STEP_MS = 10;
static_assert(THROTTLE_RAMP_UP_MS > 0 && THROTTLE_RAMP_DOWN_MS > 0, "Throttle ramp times must be nonzero");
constexpr uint32_t THROTTLE_DUTY_SCALE = 1000, THROTTLE_FULL_DUTY = 255 * THROTTLE_DUTY_SCALE;

uint16_t acceleratorADC = 0;
uint8_t switchMask = 0, throttleOut = 0, stopReason = 3;
bool accelSeen = false, switchSeen = false, pwmReady = false;
bool pedalArmed = false, reverseOut = false, zeroTiming = false, shifting = false;
bool txFault = false;
uint32_t accelAt = 0, switchAt = 0, zeroAt = 0, shiftedAt = 0;
uint32_t lastStatus = 0, lastBeat = 0, lastPrint = 0, lastCanAttempt = 0;
uint32_t invalidFrames = 0;
uint32_t throttleDutyScaled = 0, throttleRampAt = 0, throttleRampRemainder = 0;
uint8_t throttleDuty = 0, throttleTarget = 0, throttleRampDirection = 0;

const char *pcmReason() {
  switch (stopReason) {
    case 0: return "ready";
    case 1: return "CAN unavailable/TX failure";
    case 2: return "PWM setup/write failure";
    case 3: return "pedal missing/stale/invalid";
    case 4: return "switches missing/stale/invalid";
    case 5: return "release pedal to resume";
    case 6: return "direction change/settling";
    case 7: return "traction outputs disabled";
    case 8: return "reverse relays disabled";
    default: return "unknown";
  }
}

void stopThrottle() {
  throttleOut = throttleDuty = throttleTarget = throttleRampDirection = 0;
  throttleDutyScaled = throttleRampRemainder = 0;
  throttleRampAt = millis();
  // Also clear a successfully attached channel if the other failed setup.
  ledcWrite(MOTOR_1, 0);
  ledcWrite(MOTOR_2, 0);
}

void rampThrottle(uint8_t target, uint32_t now) {
  throttleTarget = target;
  const uint32_t targetDuty = (uint32_t)target * THROTTLE_FULL_DUTY / 100;
  uint32_t elapsed = now - throttleRampAt;
  throttleRampAt = now;
  if (elapsed > THROTTLE_RAMP_MAX_STEP_MS) elapsed = THROTTLE_RAMP_MAX_STEP_MS;
  const uint8_t direction = targetDuty > throttleDutyScaled ? 1 : targetDuty < throttleDutyScaled ? 2 : 0;
  if (direction != throttleRampDirection) throttleRampRemainder = 0;
  throttleRampDirection = direction;
  if (direction && elapsed) {
    const uint32_t duration = direction == 1 ? THROTTLE_RAMP_UP_MS : THROTTLE_RAMP_DOWN_MS;
    const uint32_t numerator = THROTTLE_FULL_DUTY * elapsed + throttleRampRemainder;
    const uint32_t step = numerator / duration;
    throttleRampRemainder = numerator % duration;
    const uint32_t distance = direction == 1 ? targetDuty - throttleDutyScaled : throttleDutyScaled - targetDuty;
    if (step >= distance) {
      throttleDutyScaled = targetDuty;
      throttleRampRemainder = 0;
    } else if (direction == 1) throttleDutyScaled += step;
    else throttleDutyScaled -= step;
  }
  throttleDuty = throttleDutyScaled / THROTTLE_DUTY_SCALE;
  // CAN reports the applied duty rounded to whole percent, not the target.
  throttleOut = ((uint16_t)throttleDuty * 100 + 127) / 255;
}

void servicePowertrain() {
  const uint32_t now = millis();
  if (!canReady || txFault) stopReason = 1;
  else if (!pwmReady) stopReason = 2;
  else if (!accelSeen || now - accelAt >= ACCEL_TIMEOUT_MS) stopReason = 3;
  else if (!switchSeen || now - switchAt >= SWITCH_TIMEOUT_MS) stopReason = 4;
  else if (!ENABLE_TRACTION_OUTPUTS) stopReason = 7;
  else stopReason = 0;

  if (stopReason) {
    pedalArmed = false;
    zeroTiming = false;
    stopThrottle();
    // Hold the last applied direction on a fault; dropping reverse while
    // coasting would itself be an unrequested direction change.
    return;
  }

  const uint8_t pedal = throttlePercent(acceleratorADC);
  if (pedal) zeroTiming = false;
  else if (!zeroTiming) { zeroTiming = true; zeroAt = now; }

  const bool requestedReverse = (switchMask & (1U << 2)) != 0;
  if (requestedReverse != reverseOut) {
    pedalArmed = false;
    stopThrottle();
    stopReason = ENABLE_REVERSE_RELAYS ? 6 : 8;
    if (!ENABLE_REVERSE_RELAYS || !zeroTiming || now - zeroAt < SHIFT_ZERO_MS ||
        (shifting && now - shiftedAt < SHIFT_SETTLE_MS)) return;
    digitalWrite(REVERSE_1, requestedReverse ? REVERSE_RELAY_ON : REVERSE_RELAY_OFF);
    digitalWrite(REVERSE_2, requestedReverse ? REVERSE_RELAY_ON : REVERSE_RELAY_OFF);
    reverseOut = requestedReverse;
    shiftedAt = now;
    shifting = true;
  }
  if (shifting) {
    if (now - shiftedAt < SHIFT_SETTLE_MS) {
      stopReason = 6;
      stopThrottle();
      return;
    }
    shifting = false;
  }

  if (!pedal) pedalArmed = true;
  if (!pedalArmed) { stopReason = 5; stopThrottle(); return; }
  const uint8_t target = (switchMask & (1U << 1)) && pedal > ECO_LIMIT_PERCENT ? ECO_LIMIT_PERCENT : pedal;
  rampThrottle(target, now);
  // Evaluate both writes, even if one fails, so the other can be stopped.
  const bool firstOK = ledcWrite(MOTOR_1, throttleDuty);
  const bool secondOK = ledcWrite(MOTOR_2, throttleDuty);
  if (!firstOK || !secondOK) {
    pwmReady = false;
    pedalArmed = false;
    stopReason = 2;
    stopThrottle();
  }
}

void receivePowertrain(uint32_t id, uint8_t len, const uint8_t *data) {
  const uint32_t now = millis();
  if (id == 0x301) {
    const uint16_t adc = len == 2 ? data[0] | ((uint16_t)data[1] << 8) : 0;
    if (len != 2 || adc > 4095) { accelSeen = false; ++invalidFrames; return; }
    acceleratorADC = adc;
    accelAt = now;
    accelSeen = true;
  } else if (id == 0x300) {
    if (len != 1 || (data[0] & 0x80)) { switchSeen = false; ++invalidFrames; return; }
    switchMask = data[0];
    switchAt = now;
    switchSeen = true;
  }
}

void sendPowertrainStatus() {
  const uint8_t data[8] = {
    (uint8_t)(throttleDuty > 0), (uint8_t)(throttleDuty > 0),
    (uint8_t)reverseOut, throttleOut, 0,
    (uint8_t)(switchSeen && millis() - switchAt < SWITCH_TIMEOUT_MS && (switchMask & 2)),
    0, stopReason
  };
  txFault = !canSend(0x201, data);
  if (txFault) servicePowertrain();
}

void setup() {
  Serial.begin(115200);
  // Preload OFF before enabling either output, as in the running receiver.
  digitalWrite(REVERSE_1, REVERSE_RELAY_OFF); digitalWrite(REVERSE_2, REVERSE_RELAY_OFF);
  pinMode(REVERSE_1, OUTPUT); pinMode(REVERSE_2, OUTPUT);
  pinMode(MOTOR_1, OUTPUT); pinMode(MOTOR_2, OUTPUT);
  digitalWrite(MOTOR_1, LOW); digitalWrite(MOTOR_2, LOW);
  const bool firstReady = ledcAttach(MOTOR_1, 1000, 8);
  const bool secondReady = ledcAttach(MOTOR_2, 1000, 8);
  pwmReady = firstReady && secondReady;
  stopThrottle();
  canBegin(); lastCanAttempt = millis();
  Serial.printf("Node 2 PCM | PWM13/14=%s reverse33/26=%s (LOW=reverse, HIGH=forward) | PWM setup=%s\n",
                ENABLE_TRACTION_OUTPUTS ? "ENABLED" : "DISABLED",
                ENABLE_REVERSE_RELAYS ? "ENABLED" : "DISABLED", pwmReady ? "OK" : "FAILED");
}

void loop() {
  servicePowertrain();
  uint32_t id; uint8_t len, data[8];
  // Bound RX work and enforce expiry between frames on a busy bus.
  for (uint8_t n = 0; n < 8 && canRead(id, len, data); ++n) {
    servicePowertrain();
    receivePowertrain(id, len, data);
    servicePowertrain();
  }
  const uint32_t now = millis();
  if (!canReady && now - lastCanAttempt >= 1000) {
    stopThrottle();
    canBegin(); lastCanAttempt = millis();
  }
  if (canReady && now - lastStatus >= 50) {
    lastStatus = now;
    sendPowertrainStatus();
  }
  servicePowertrain();
  if (canReady && now - lastBeat >= 1000) {
    lastBeat = now;
    const uint8_t beat[8] = {2, (uint8_t)(stopReason == 0), (uint8_t)(now / 1000),
                           stopReason, canTxCount, canRxCount, 0, 0};
    if (!canSend(0x600, beat)) { txFault = true; servicePowertrain(); }
  }
  if (now - lastPrint >= 250) {
    lastPrint = now;
    Serial.printf("Node 2 | ADC=%u/4095 corrected=%.3f V pedal=%u%% target=%u%% output=%u%% PWM=%u/255 reverse=%s | %s | invalid=%lu\n",
                  acceleratorADC, throttleVoltage(acceleratorADC), throttlePercent(acceleratorADC),
                  throttleTarget, throttleOut, throttleDuty, reverseOut ? "ON" : "OFF", pcmReason(), (unsigned long)invalidFrames);
    Serial.printf("Requests: cruise=%u ECO=%u reverse=%u hill=%u horn=%u headlight=%u autonomous=%u\n",
                  !!(switchMask & 1), !!(switchMask & 2), !!(switchMask & 4), !!(switchMask & 8),
                  !!(switchMask & 16), !!(switchMask & 32), !!(switchMask & 64));
  }
  servicePowertrain();
  delay(1);
}
