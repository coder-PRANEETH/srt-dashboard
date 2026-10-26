#pragma once
#include <stdint.h>
#include "SwitchSignals.h"

constexpr uint32_t DRIVE_GATE_ID = 0x120;
constexpr uint32_t DRIVE_PEDAL_STATUS_ID = 0x121;
constexpr uint32_t DRIVE_SWITCH_STATUS_ID = 0x122;
constexpr uint32_t DRIVE_PWM_ID = 0x123;
constexpr uint32_t DRIVE_MOTOR_STATUS_ID = 0x124;
constexpr uint32_t DRIVE_LEASE_MS = 350;
// Browser hold renews every 2 s; Node 5 still renews the CAN lease every 50 ms.
constexpr uint32_t DRIVE_HTTP_RENEW_MS = 2000;
constexpr uint32_t DRIVE_HTTP_LEASE_MS = 2500;
constexpr uint32_t DRIVE_SETTLE_MS = 1000;
// Pedal release tolerance is independent of the motor controller's idle level.
constexpr uint16_t DRIVE_IDLE_ADC = 1000UL * 4095 / 3300; // <=1.0 V: ADC 0..1240.
constexpr uint16_t DRIVE_REST_ADC = 800UL * 4095 / 3300; // Keep electrical idle at 0.8 V.
// Nearest 8-bit duty to 0.8 V on a 3.3 V output: 62/255 (~0.802 V).
constexpr uint8_t DRIVE_REST_PWM = (800UL * 255 + 1650) / 3300;
inline uint16_t driveThrottleAdc(uint8_t percent) {
  return DRIVE_REST_ADC + (uint32_t)(4095-DRIVE_REST_ADC)*percent/100;
}
inline uint8_t driveThrottleDuty(uint8_t demand) {
  return DRIVE_REST_PWM + (uint16_t)(255-DRIVE_REST_PWM)*demand/255;
}
inline uint32_t driveRead32(const uint8_t *d) {
  return (uint32_t)d[0] | ((uint32_t)d[1]<<8) | ((uint32_t)d[2]<<16) | ((uint32_t)d[3]<<24);
}
inline void driveWrite32(uint8_t *d, uint32_t n) { for(uint8_t k=0;k<4;++k) d[k]=n>>(8*k); }
inline bool driveStatusValid(const uint8_t *d, uint16_t maxRaw) {
  return d[3]==0xD1 && (d[0]&0x7F)<=2 &&
    (d[1] | ((uint16_t)d[2]<<8))<=maxRaw;
}
inline uint8_t driveSwitchMask(uint8_t physical, bool reverse) {
  // Retain hill/remote-mode/horn requests; clear cruise/autonomous and set direction.
  return (physical & (SWITCH_HILL_HOLD | SWITCH_REMOTE_MODE | SWITCH_HORN)) |
    (reverse ? SWITCH_REVERSE : 0);
}
// 0 normal sender, 1 silent while host owns IDs, 2 latched idle/held direction.
// Op 0 locks; 1 claims/renews; 2 releases switches; 3 releases pedal last.
struct RemoteDriveGate {
  uint8_t phase=0;
  bool reverse=false, sequenced=false;
  uint32_t session=0, lastAt=0;
  uint16_t sequence=0;
  void lock() { if(phase) phase=2; }
  void tick(uint32_t now) {
    if(phase==1 && now-lastAt>=DRIVE_LEASE_MS) lock();
  }
  bool accept(const uint8_t *d, uint8_t len, uint8_t node, bool inputSafe, bool peerReleased, uint32_t now) {
    tick(now);
    // Bit 0 = reverse, bit 1 = direct PWM protocol (joystick).
    if(len!=8 || d[0]>3 || d[1]>3) { lock(); return false; }
    const uint32_t token=driveRead32(d+2);
    const uint16_t seq=d[6] | ((uint16_t)d[7]<<8);
    if(!token) return false;
    if(d[0]==0) { if(phase && token==session) lock(); return true; }
    if(d[0]==1) {
      // Idle is required for entry and return to manual, not for renewals.
      // A physical pedal press cannot change or cancel a remotely owned drive.
      if(phase!=1 && !inputSafe) return false;
      if(token!=session) {
        if(phase==1) return false;
        session=token; sequenced=false;
      } else if(phase!=1) return false; // Expired or released session cannot restart.
      const uint16_t advance=seq-sequence;
      if(sequenced && (!advance || advance>=0x8000)) return false;
      reverse=d[1]&1; sequence=seq; sequenced=true; phase=1; lastAt=now; return true;
    }
    if(token!=session || phase!=2 || !inputSafe) return false;
    if((node==3 && d[0]==2) || (node==1 && d[0]==3 && peerReleased)) { phase=0; return true; }
    return false;
  }
  void status(uint8_t *d, uint16_t raw) const {
    d[0]=phase|(reverse ? 0x80 : 0); d[1]=raw; d[2]=raw>>8; d[3]=0xD1;
    driveWrite32(d+4,session);
  }
};
