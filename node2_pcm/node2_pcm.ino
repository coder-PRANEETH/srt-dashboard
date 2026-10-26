/*
 * NODE 2 PCM - CAN throttle receiver and reverse outputs.
 * Matches the supplied running vehicle: PWM GPIO13/14 (1 kHz, 8 bit),
 * active-LOW reverse relay inputs GPIO33/26. No local pedal/brake input.
 * MCP2515: SCK18 MISO19 MOSI23 CS25 INT27, 500 kbps, 8 MHz crystal.
 * Keep CanCommon.h, ThrottleCalibration.h, RemoteDrive.h and SwitchSignals.h in this folder.
 *
 * RX 0x301 DLC2: raw 12-bit pedal ADC, little-endian.
 * RX 0x300 DLC1: cruise/hillHold/autonomous/remoteMode/horn/reverse bits 0..5.
 * RX 0x120/0x121/0x122: remote ownership and physical-input status.
 * RX 0x123 DLC8: joystick demand 0..255 above idle, reverse, LE16 sequence, LE32 session.
 * TX 0x124 DLC8: remote ownership, applied reverse and PWM, every 100 ms.
 * TX 0x201 DLC8: motor1, motor2, applied reverse, throttle %, cruise active,
 *               reserved=0, hill active, stop reason (see pcmReason()).
 * TX 0x602 DLC8: Node 2 heartbeat, once per second (unique sender ID).
 *
 * Traction and reverse outputs are enabled for the supplied vehicle wiring.
 * Override either enable to 0 for a bench build without that output.
 * Cruise holds applied output. Hill selection locks throttle at idle because the
 * available pitch sensor rotates with the wheel; autonomous remains a request.
 * Direction changes require an idle pedal command; PWM returns to idle before the
 * relays switch immediately, with no added pre-switch or settling delay.
 * Normal PWM changes ramp linearly; STOP/watchdogs return immediately to the
 * 0.8 V-equivalent idle signal. Disabled or failed PWM hardware is cleared to 0.
 * This checks commanded throttle, not vehicle speed or braking.
 */

#include "CanCommon.h"
#include "ThrottleCalibration.h"
#include "RemoteDrive.h"
#include "TractionMode.h"
#include <driver/gpio.h>

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
// Physical transfer from the supplied receiver, with a 0.8 V minimum applied
// at the output. The dashboard's corrected gauge is independent.
constexpr uint16_t ACCEL_MIN_ADC=0, ACCEL_MAX_ADC=4095;
static_assert(ACCEL_MIN_ADC<ACCEL_MAX_ADC && ACCEL_MAX_ADC<=4095,"Invalid physical pedal calibration");
uint8_t manualThrottlePercent(uint16_t adc) {
  const uint16_t bounded=adc<ACCEL_MIN_ADC?ACCEL_MIN_ADC:adc>ACCEL_MAX_ADC?ACCEL_MAX_ADC:adc;
  return (uint32_t)(bounded-ACCEL_MIN_ADC)*100/(ACCEL_MAX_ADC-ACCEL_MIN_ADC);
}
// Slow the torque rise to soften initial acceleration; keep release response quick.
constexpr uint32_t THROTTLE_RAMP_UP_MS = 2500, THROTTLE_RAMP_DOWN_MS = 400;
// Discard excess elapsed time after a delayed loop instead of jumping the PWM.
constexpr uint32_t THROTTLE_RAMP_MAX_STEP_MS = 10;
static_assert(THROTTLE_RAMP_UP_MS > 0 && THROTTLE_RAMP_DOWN_MS > 0, "Throttle ramp times must be nonzero");
constexpr uint32_t THROTTLE_DUTY_SCALE = 1000, THROTTLE_FULL_DUTY = 255 * THROTTLE_DUTY_SCALE;
constexpr uint32_t THROTTLE_IDLE_DUTY = DRIVE_REST_PWM * THROTTLE_DUTY_SCALE;
constexpr uint32_t THROTTLE_SLEW_DUTY = THROTTLE_FULL_DUTY - THROTTLE_IDLE_DUTY;

uint16_t acceleratorADC = 0;
uint8_t switchMask = 0, throttleOut = 0, stopReason = 3;
bool accelSeen = false, switchSeen = false, pwmReady = false;
bool pedalArmed = false, reverseOut = false;
bool txFault = false;
uint32_t accelAt = 0, switchAt = 0;
uint32_t lastStatus = 0, lastBeat = 0, lastPrint = 0, lastCanAttempt = 0;
uint32_t invalidFrames = 0;
uint32_t throttleDutyScaled = 0, throttleRampAt = 0, throttleRampRemainder = 0;
uint8_t throttleDuty = 0, throttleTarget = 0, throttleRampDirection = 0;
uint32_t cruiseDutyScaled = 0;
bool cruiseActive = false;
RemoteDriveGate remoteGate;
TractionMode tractionMode;
uint32_t remotePeerAt=0, remoteSwitchAt=0, remoteCommandAt=0, lastRemoteStatus=0;
uint32_t remoteSwitchSession=0;
uint16_t remotePhysicalAdc=0, remoteSequence=0;
uint8_t remoteSwitchMask=0, remotePwm=0;
bool remotePeerSeen=false, remoteSwitchSeen=false, remoteSwitchNormal=false;
bool remoteCommandSeen=false, remoteReverse=false;

const char *pcmReason() {
  switch (stopReason) {
    case 0: return "ready";
    case 1: return "CAN unavailable/TX failure";
    case 2: return "PWM setup/write failure";
    case 3: return "pedal missing/stale/invalid";
    case 4: return "switches missing/stale/invalid";
    case 5: return "release pedal to resume";
    case 6: return "release pedal to change direction";
    case 7: return "traction outputs disabled";
    case 8: return "reverse relays disabled";
    case 9: return "remote command missing/expired";
    case 10: return "hill assist unavailable: chassis pitch required";
    case 11: return "IMMOBILE: throttle held at idle";
    default: return "unknown";
  }
}

void setReverseRelays(bool enable) {
  digitalWrite(REVERSE_1, enable ? REVERSE_RELAY_ON : REVERSE_RELAY_OFF);
  digitalWrite(REVERSE_2, enable ? REVERSE_RELAY_ON : REVERSE_RELAY_OFF);
  reverseOut = enable;
  Serial.printf("[%lu ms] Reverse relays command=%s | GPIO33=%s GPIO26=%s (LOW=ON, HIGH=OFF)\n",
                (unsigned long)millis(), enable ? "ON" : "OFF",
                digitalRead(REVERSE_1) == HIGH ? "HIGH" : "LOW",
                digitalRead(REVERSE_2) == HIGH ? "HIGH" : "LOW");
}

void stopThrottle() {
  // Zero demand is the valid electrical idle level, including at startup and
  // on missing commands/CAN faults. It must not become a 0 V throttle fault.
  throttleDuty = ENABLE_TRACTION_OUTPUTS && pwmReady ? DRIVE_REST_PWM : 0;
  throttleOut = throttleTarget = ((uint16_t)throttleDuty * 100 + 127) / 255;
  throttleRampDirection = 0;
  throttleDutyScaled = (uint32_t)throttleDuty * THROTTLE_DUTY_SCALE;
  throttleRampRemainder = 0;
  cruiseActive = false;
  throttleRampAt = millis();
  // Also clear a successfully attached channel if the other failed setup.
  const bool firstOK = ledcWrite(MOTOR_1, throttleDuty);
  const bool secondOK = ledcWrite(MOTOR_2, throttleDuty);
  if ((!firstOK || !secondOK) && throttleDuty) {
    pwmReady = false;
    stopReason = 2;
    stopThrottle();
  }
}

void rampThrottleDuty(uint32_t targetDuty, uint32_t now) {
  if (targetDuty < THROTTLE_IDLE_DUTY) targetDuty = THROTTLE_IDLE_DUTY;
  throttleTarget = (targetDuty * 100 + THROTTLE_FULL_DUTY/2) / THROTTLE_FULL_DUTY;
  uint32_t elapsed = now - throttleRampAt;
  throttleRampAt = now;
  if (elapsed > THROTTLE_RAMP_MAX_STEP_MS) elapsed = THROTTLE_RAMP_MAX_STEP_MS;
  const uint8_t direction = targetDuty > throttleDutyScaled ? 1 : targetDuty < throttleDutyScaled ? 2 : 0;
  if (direction != throttleRampDirection) throttleRampRemainder = 0;
  throttleRampDirection = direction;
  if (direction && elapsed) {
    const uint32_t duration = direction == 1 ? THROTTLE_RAMP_UP_MS : THROTTLE_RAMP_DOWN_MS;
    const uint32_t numerator = THROTTLE_SLEW_DUTY * elapsed + throttleRampRemainder;
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
void rampThrottle(uint8_t target, uint32_t now) {
  rampThrottleDuty((uint32_t)target * THROTTLE_FULL_DUTY / 100, now);
}

void servicePowertrain() {
  const uint32_t now = millis();
  remoteGate.tick(now);
  const bool remote = remoteGate.phase != 0;
  const bool remoteMode = (switchMask & SWITCH_REMOTE_MODE) != 0;
  const uint8_t physicalSwitches=remoteSwitchSeen && now-remoteSwitchAt<DRIVE_LEASE_MS ? remoteSwitchMask : switchMask;
  const bool hillSelected=(physicalSwitches&SWITCH_HILL_HOLD)!=0;
  if (!canReady || txFault) stopReason = 1;
  else if (!pwmReady) stopReason = 2;
  else if (hillSelected) stopReason = 10;
  else if (remoteGate.phase==2 || remoteMode!=(remoteGate.phase==1) ||
      (remote && (!remoteCommandSeen || now-remoteCommandAt>=DRIVE_LEASE_MS ||
       !remotePeerSeen || now-remotePeerAt>=DRIVE_LEASE_MS || !remoteSwitchSeen || now-remoteSwitchAt>=DRIVE_LEASE_MS))) stopReason = 9;
  else if (!remote && (!accelSeen || now - accelAt >= ACCEL_TIMEOUT_MS)) stopReason = 3;
  else if (!remote && (!switchSeen || now - switchAt >= SWITCH_TIMEOUT_MS)) stopReason = 4;
  else if (!ENABLE_TRACTION_OUTPUTS) stopReason = 7;
  else stopReason = 0;

  if (stopReason) {
    // A fault after the first remote command ends ownership. Recovery needs a
    // new gesture/session; the host's continuing positive PWM cannot restart it.
    if (remoteGate.phase==1 && remoteCommandSeen) remoteGate.lock();
    const bool modeSelectionIdle=((stopReason==9 && remoteMode && remoteGate.phase==0) || stopReason==10) &&
      accelSeen && now-accelAt<ACCEL_TIMEOUT_MS && switchSeen && now-switchAt<SWITCH_TIMEOUT_MS &&
      acceleratorADC<=DRIVE_IDLE_ADC;
    pedalArmed = false;
    stopThrottle();
    // Remote mode blocks pedal throttle while idle, but still permits a
    // stationary physical direction switch when the pedal is released.
    if(modeSelectionIdle) {
      const bool requestedReverse=(switchMask&SWITCH_REVERSE)!=0;
      if(requestedReverse!=reverseOut) setReverseRelays(requestedReverse);
    }
    // Hold the last applied direction on a fault; dropping reverse while
    // coasting would itself be an unrequested direction change.
    return;
  }

  const uint8_t pedal = remote ? remotePwm : manualThrottlePercent(acceleratorADC);
  // The real sensor's released-pedal voltage is nonzero. Use the measured idle
  // threshold for arming/shifting while retaining the working raw output scale.
  const bool pedalIdle = remote ? remotePwm==0 : acceleratorADC<=DRIVE_IDLE_ADC;
  const bool requestedReverse = remote ? remoteReverse : (switchMask & SWITCH_REVERSE) != 0;
  if (requestedReverse != reverseOut) {
    // Return to the controller's idle signal, then write both relay pins.
    // Keep idle permission, but do not delay an accepted direction change.
    pedalArmed = false;
    stopThrottle();
    stopReason = ENABLE_REVERSE_RELAYS ? 6 : 8;
    if (!ENABLE_REVERSE_RELAYS || !pedalIdle) return;
    setReverseRelays(requestedReverse);
    stopReason = 0;
  }

  if(!tractionMode.mobile) {
    pedalArmed=false;stopReason=11;stopThrottle();return;
  }
  if (pedalIdle) pedalArmed = true;
  if (!pedalArmed) { stopReason = 5; stopThrottle(); return; }
  if (remote) {
    rampThrottleDuty((uint32_t)driveThrottleDuty(remotePwm) * THROTTLE_DUTY_SCALE, now);
  } else if (switchMask & SWITCH_CRUISE) {
    if (!cruiseActive) {
      cruiseDutyScaled = throttleDutyScaled;
      cruiseActive = true;
    }
    rampThrottleDuty(cruiseDutyScaled, now);
  } else {
    cruiseActive = false;
    rampThrottle(pedal, now);
  }
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
  if(id==TRACTION_MODE_COMMAND_ID) {
    if(!tractionMode.accept(data,len)) {++invalidFrames;return;}
    // Neither MOBILE nor an old demand may restore pre-toggle throttle.
    remotePwm=0;pedalArmed=false;stopThrottle();return;
  }
  if (id==DRIVE_PEDAL_STATUS_ID && len==8 && driveStatusValid(data,4095)) {
    remotePhysicalAdc=data[1] | ((uint16_t)data[2]<<8);
    remotePeerSeen=true; remotePeerAt=now;
  } else if (id==DRIVE_SWITCH_STATUS_ID && len==8 && driveStatusValid(data,SWITCH_VALID_MASK)) {
    remoteSwitchMask=data[1]; remoteSwitchNormal=(data[0]&127)==0;
    remoteSwitchSession=driveRead32(data+4); remoteSwitchSeen=true; remoteSwitchAt=now;
  } else if (id==DRIVE_GATE_ID) {
    // Only the joystick's PWM claim owns Node 2. Legacy ADC controls still work.
    if (len!=8) { remoteGate.lock(); return; }
    if (!(data[1]&2) && data[0]==1) return;
    const uint32_t previous=remoteGate.session;
    const bool idle=remotePeerSeen && now-remotePeerAt<250 && remotePhysicalAdc<=DRIVE_IDLE_ADC;
    const bool modeAllowsClaim=data[0]!=1 || (switchMask&SWITCH_REMOTE_MODE);
    const bool released=remoteSwitchSeen && now-remoteSwitchAt<250 && remoteSwitchNormal && remoteSwitchSession==remoteGate.session;
    if (remoteGate.accept(data,len,1,idle&&modeAllowsClaim,released,now) && remoteGate.session!=previous) {
      remoteCommandSeen=false; remotePwm=0; remoteSequence=0; pedalArmed=false;
    }
  } else if (id==DRIVE_PWM_ID) {
    if (remoteGate.phase!=1) return;
    const uint16_t next=len==8 ? data[2] | ((uint16_t)data[3]<<8) : 0;
    const uint16_t advance=next-remoteSequence;
    if (len!=8 || data[1]>1 || driveRead32(data+4)!=remoteGate.session ||
        !next || (remoteCommandSeen && (!advance || advance>=0x8000))) {
      remoteGate.lock(); remoteCommandSeen=false; return;
    }
    remotePwm=tractionMode.mobile?data[0]:0; remoteReverse=data[1]; remoteSequence=next;
    remoteCommandAt=now; remoteCommandSeen=true;
  }
  if (id == 0x301) {
    const uint16_t adc = len == 2 ? data[0] | ((uint16_t)data[1] << 8) : 0;
    if (len != 2 || adc > 4095) { accelSeen = false; ++invalidFrames; return; }
    acceleratorADC = adc;
    accelAt = now;
    accelSeen = true;
  } else if (id == 0x300) {
    if (len != 1 || (data[0] & ~SWITCH_VALID_MASK)) { switchSeen = false; ++invalidFrames; return; }
    switchMask = data[0];
    switchAt = now;
    switchSeen = true;
  }
}

void sendPowertrainStatus() {
  const uint8_t data[8] = {
    (uint8_t)(throttleDuty > DRIVE_REST_PWM), (uint8_t)(throttleDuty > DRIVE_REST_PWM),
    (uint8_t)reverseOut, throttleOut, (uint8_t)cruiseActive, 0, 0, stopReason
  };
  txFault = !canSend(0x201, data);
  if (txFault) servicePowertrain();
}

void sendRemoteMotorStatus() {
  uint8_t status[8];tractionMode.status(status,throttleDuty,stopReason);
  if (!canSend(TRACTION_MODE_STATUS_ID,status)) { txFault=true; servicePowertrain(); }
  remoteGate.status(status,throttleDuty);
  // The gate's direction is a requested remote direction, including while
  // manual mode is active. Report the applied relay command on both status IDs.
  status[0] = (status[0] & 0x7F) | (reverseOut ? 0x80 : 0);
  if (!canSend(DRIVE_MOTOR_STATUS_ID,status)) { txFault=true; servicePowertrain(); }
}

void setup() {
  Serial.begin(115200);
  // ESP32 Arduino 3.x ignores digitalWrite before pinMode registers a GPIO.
  // Preload the hardware latch directly so enabling OUTPUT cannot pulse LOW.
  gpio_set_level((gpio_num_t)REVERSE_1, REVERSE_RELAY_OFF);
  gpio_set_level((gpio_num_t)REVERSE_2, REVERSE_RELAY_OFF);
  pinMode(REVERSE_1, OUTPUT); pinMode(REVERSE_2, OUTPUT);
  setReverseRelays(false);
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
  canService();
  if(!canReady) {
    remoteGate.lock(); accelSeen=false; switchSeen=false; txFault=true;
    servicePowertrain();
  }
  if (!canReady && canRecovery.retryDue(now,lastCanAttempt)) {
    stopThrottle();
    canBegin(); lastCanAttempt = millis();
  }
  if (canReady && now - lastStatus >= 50) {
    lastStatus = now;
    sendPowertrainStatus();
  }
  if (canReady && now-lastRemoteStatus>=100) {
    lastRemoteStatus=now;
    sendRemoteMotorStatus();
  }
  servicePowertrain();
  if (canReady && now - lastBeat >= 1000) {
    lastBeat = now;
    const uint8_t beat[8] = {2, (uint8_t)(stopReason == 0), (uint8_t)(now / 1000),
                           stopReason, canTxCount, canRxCount, 0, 0};
    if (!canSend(0x602, beat)) { txFault = true; servicePowertrain(); }
  }
  if (now - lastPrint >= 250) {
    lastPrint = now;
    if(!canReady) canPrintStatus("Node 2 recovery");
    Serial.printf("Node 2 | ADC=%u/4095 corrected=%.3f V pedal=%u%% target=%u%% output=%u%% PWM=%u/255 reverse=%s GPIO33=%s GPIO26=%s | %s | invalid=%lu\n",
                  acceleratorADC, throttleVoltage(acceleratorADC), manualThrottlePercent(acceleratorADC),
                  throttleTarget, throttleOut, throttleDuty, reverseOut ? "ON" : "OFF",
                  digitalRead(REVERSE_1) == HIGH ? "HIGH" : "LOW",
                  digitalRead(REVERSE_2) == HIGH ? "HIGH" : "LOW", pcmReason(), (unsigned long)invalidFrames);
    Serial.printf("Requests: cruise=%u hillHold=%u autonomous=%u remoteMode=%u horn=%u reverse=%u\n",
                  !!(switchMask & SWITCH_CRUISE), !!(switchMask & SWITCH_HILL_HOLD),
                  !!(switchMask & SWITCH_AUTONOMOUS), !!(switchMask & SWITCH_REMOTE_MODE),
                  !!(switchMask & SWITCH_HORN), !!(switchMask & SWITCH_REVERSE));
  }
  servicePowertrain();
  delay(1);
}
