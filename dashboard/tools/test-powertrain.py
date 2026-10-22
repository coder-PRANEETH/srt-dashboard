"""Compile the actual Node 2 sketch against mocked CAN/PWM/relay I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'node2_pcm' / 'node2_pcm.ino').read_text().replace('#include "CanCommon.h"', '')
shim = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
constexpr int LOW=0, HIGH=1, OUTPUT=1;
uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
void delay(unsigned n) { clockMs+=n; }
struct SerialMock {
  void begin(int) {}
  template<class... T> void printf(const char*, T...) {}
} Serial;
bool canReady=true, txWorks=true, writesWork=true, firstAttach=true, secondAttach=true;
uint8_t canTxCount=0, canRxCount=0;
int pins[40]={}, pwm[40]={};
void pinMode(int pin,int mode) {
  // The active-low relay input must already be OFF when OUTPUT is enabled.
  if ((pin==26 || pin==33) && mode==OUTPUT) assert(pins[pin]==HIGH);
}
bool ledcAttach(int pin,int hz,int bits) {
  assert((pin==13 || pin==14) && hz==1000 && bits==8);
  return pin==13 ? firstAttach : secondAttach;
}
bool ledcWrite(int pin,int duty) {
  assert((pin==13 || pin==14) && duty>=0 && duty<=255);
  if (!writesWork && duty) return false;
  pwm[pin]=duty; return true;
}
void digitalWrite(int pin,int value) {
  if ((pin==26 || pin==33) && pins[pin]!=value) assert(!pwm[13] && !pwm[14]);
  pins[pin]=value;
}
bool canBegin() { return canReady=true; }
struct Frame { uint32_t id; uint8_t len, data[8]={}; };
std::deque<Frame> incoming;
std::vector<Frame> outgoing;
bool canRead(uint32_t &id,uint8_t &len,uint8_t (&data)[8]) {
  if (!canReady || incoming.empty()) return false;
  auto f=incoming.front(); incoming.pop_front();
  id=f.id; len=f.len; memcpy(data,f.data,8); ++canRxCount; return true;
}
template<size_t N> bool canSend(uint32_t id,const uint8_t (&data)[N]) {
  Frame f{}; f.id=id; f.len=N; memcpy(f.data,data,N); outgoing.push_back(f);
  if (txWorks) ++canTxCount;
  return txWorks;
}
'''
scenarios = r'''
void reset() {
  clockMs=0; canReady=true; txWorks=true; writesWork=true;
  firstAttach=true; secondAttach=true;
  memset(pins,0,sizeof(pins)); memset(pwm,0,sizeof(pwm));
  acceleratorADC=0; switchMask=0; throttleOut=0; stopReason=3;
  accelSeen=false; switchSeen=false; pwmReady=false;
  pedalArmed=false; reverseOut=false; zeroTiming=false; shifting=false; txFault=false;
  accelAt=switchAt=zeroAt=shiftedAt=lastStatus=lastBeat=lastPrint=lastCanAttempt=0;
  invalidFrames=0; canTxCount=canRxCount=0; incoming.clear(); outgoing.clear();
  setup(); assert(!pwm[13] && !pwm[14] && pins[26]==HIGH && pins[33]==HIGH);
}
void pedal(uint16_t adc) {
  servicePowertrain();
  const uint8_t d[2]={uint8_t(adc),uint8_t(adc>>8)};
  receivePowertrain(0x301,2,d); servicePowertrain();
}
void switches(uint8_t mask) { servicePowertrain(); receivePowertrain(0x300,1,&mask); servicePowertrain(); }
void inputs(uint32_t now,uint16_t adc,uint8_t mask=0) {
  clockMs=now; servicePowertrain(); switches(mask); pedal(adc);
}
void keep(uint32_t until,uint16_t adc,uint8_t mask=0) {
  switches(mask); pedal(adc);
  const uint32_t duration=until-clockMs; assert(duration<10000);
  for(uint32_t n=1;n<=duration;++n) {
    ++clockMs; servicePowertrain();
    if(n%50==0) { switches(mask); pedal(adc); }
    assert(pwm[13]==pwm[14]);
  }
}
void assertStopped() { assert(!throttleOut && !throttleDuty && !pwm[13] && !pwm[14]); }
void full() { reset(); inputs(0,0); keep(THROTTLE_RAMP_UP_MS,4095); assert(throttleOut==100 && pwm[13]==255); }
void resumeFull() { switches(0); pedal(0); keep(clockMs+THROTTLE_RAMP_UP_MS,4095); assert(pwm[13]==255); }
int main() {
  assert(MOTOR_1==13 && MOTOR_2==14 && REVERSE_1==33 && REVERSE_2==26);
  assert(REVERSE_RELAY_ON==LOW && REVERSE_RELAY_OFF==HIGH);
  assert(ACCEL_TIMEOUT_MS==501 && SWITCH_TIMEOUT_MS==3001);
#ifdef TEST_VEHICLE_DEFAULTS
  // Do not override the sketch's defaults: this caught the vehicle no-drive bug.
  assert(ENABLE_TRACTION_OUTPUTS && ENABLE_REVERSE_RELAYS);
#endif
  reset(); inputs(0,4095); assertStopped();
  if (!ENABLE_TRACTION_OUTPUTS) {
    assert(stopReason==7); inputs(10,0,4); keep(2500,0,4);
    inputs(2550,4095,4); assertStopped(); assert(pins[26]==HIGH && pins[33]==HIGH);
    sendPowertrainStatus(); assert(outgoing.back().data[3]==0);
    puts("Node 2 bench override passed: PWM and reverse disabled, active-low relays OFF"); return 0;
  }
  assert(stopReason==5); // Booting with a pressed pedal cannot move.
  pedal(868); assert(pedalArmed); pedal(4095); assertStopped();
  // One full-scale jump becomes a linear 400 ms PWM ramp, independent of
  // how many times the control function is called within one millisecond.
  int previous=0;
  for(uint32_t n=1;n<=THROTTLE_RAMP_UP_MS;++n) {
    ++clockMs; servicePowertrain();
    if(n%50==0) pedal(4095);
    assert(pwm[13]==int(n*255/THROTTLE_RAMP_UP_MS));
    assert(pwm[13]>=previous && pwm[13]-previous<=1 && pwm[13]==pwm[14]);
    previous=pwm[13];
    for(int duplicate=0;duplicate<5;++duplicate)servicePowertrain();
    assert(pwm[13]==previous);
    if(n==200) {sendPowertrainStatus();assert(outgoing.back().data[3]==50 && throttleTarget==100);}
  }
  assert(throttleOut==100 && pwm[13]==255);
  pedal(0); assert(pwm[13]==255); // Ordinary release also ramps, without a jump.
  for(uint32_t n=1;n<=THROTTLE_RAMP_DOWN_MS;++n) {
    ++clockMs; servicePowertrain(); if(n%50==0)pedal(0);
    assert(pwm[13]==int((THROTTLE_RAMP_DOWN_MS-n)*255/THROTTLE_RAMP_DOWN_MS));
    assert(pwm[13]<=previous && previous-pwm[13]<=1 && pwm[13]==pwm[14]);
    previous=pwm[13];
    if(n==398) { // One nonzero PWM count rounds to 0%, but the motor-on flag is true.
      sendPowertrainStatus(); assert(outgoing.back().data[0]==1 && outgoing.back().data[3]==0);
    }
  }
  assertStopped(); assert(pedalArmed && stopReason==0);

  reset(); inputs(0,0); keep(140,2000);
  assert(acceleratorADC==2000 && throttleOut==35 && pwm[13]==89);
  keep(300,2000); assert(pwm[13]==89); // No overshoot after reaching a small target.
  full(); pedal(2000); assert(pwm[13]==255);
  keep(clockMs+100,2000); assert(pwm[13]==191 && throttleTarget==35);
  pedal(4095); assert(pwm[13]==191); keep(clockMs+100,4095); assert(pwm[13]==255);
  // ECO changes the target smoothly rather than stepping the applied PWM.
  switches(0x7b); assert(pwm[13]==255 && throttleTarget==40);
  keep(clockMs+240,4095,0x7b); assert(throttleOut==40 && pwm[13]==102);
  sendPowertrainStatus(); const auto s=outgoing.back();
  assert(s.id==0x201 && s.len==8 && s.data[0]==1 && s.data[1]==1);
  assert(s.data[2]==0 && s.data[3]==40 && s.data[4]==0 && s.data[5]==1 && s.data[6]==0);
  keep(clockMs+400,0,0x7b); assertStopped(); // Cruise/hill/autonomous cannot add throttle.

  // Irregular 1..9 ms scheduling gives the same ramp as the 1 ms loop.
  reset(); inputs(0,0); pedal(4095);
  const uint32_t spacing[]={1,7,3,9,2,6};
  for(uint32_t n=0;clockMs<THROTTLE_RAMP_UP_MS;++n) {
    uint32_t dt=spacing[n%6]; if(dt>THROTTLE_RAMP_UP_MS-clockMs)dt=THROTTLE_RAMP_UP_MS-clockMs;
    clockMs+=dt; servicePowertrain(); pedal(4095);
    assert(pwm[13]==int(clockMs*255/THROTTLE_RAMP_UP_MS));
  }
  assert(pwm[13]==255);
  pedal(0); clockMs+=100; servicePowertrain();
  assert(pwm[13]>=248 && pwm[13]<255); // A stalled loop cannot catch up in a large jump.
  previous=pwm[13]; servicePowertrain(); assert(pwm[13]==previous);

  // Physical switches broadcast once a second; pedal every 50 ms.
  reset(); inputs(0,0); pedal(4095);
  for(uint32_t t=1;t<=2500;++t) {
    clockMs=t;servicePowertrain();if(t%1000==0)switches(0);if(t%50==0)pedal(4095);
    if(t>=THROTTLE_RAMP_UP_MS)assert(throttleOut==100);
  }
  // The supplied receiver tolerates 500 ms, then stops on millisecond 501.
  clockMs+=500; servicePowertrain(); assert(pwm[13]==255 && stopReason==0);
  ++clockMs; servicePowertrain(); assertStopped(); assert(stopReason==3);
  pedal(4095); assertStopped(); assert(stopReason==5);
  resumeFull();
  // A fresh pedal must not hide an expired switch frame.
  const uint32_t switchStart=clockMs;
  for(uint32_t t=1;t<=3001;++t) {
    ++clockMs; servicePowertrain(); if(t%50==0)pedal(4095);
  }
  assert(clockMs==switchStart+3001); assertStopped(); assert(stopReason==4);
  switches(0); assertStopped(); assert(stopReason==5); resumeFull();

  // Faults bypass the comfort ramp and erase its state before idle recovery.
  const uint8_t old[8]={}; receivePowertrain(0x301,8,old); servicePowertrain(); assertStopped();
  pedal(4095); assertStopped(); resumeFull();
  pedal(4096); assertStopped(); pedal(0);
  switches(0x80); assertStopped(); switches(0); pedal(4095); assertStopped();
  keep(clockMs+400,4095); assert(pwm[13]==255);
  receivePowertrain(0x300,8,old); servicePowertrain(); assertStopped(); assert(invalidFrames==4);

  if (!ENABLE_REVERSE_RELAYS) {
    reset(); inputs(0,0,4); keep(2100,0,4); inputs(2150,4095,4);
    assertStopped(); assert(stopReason==8 && !reverseOut);
    puts("Node 2 reverse-disabled guard passed"); return 0;
  }
  reset(); inputs(0,0,4); keep(950,0,4); assert(!reverseOut);
  keep(1000,0,4); assert(reverseOut && pins[26]==LOW && pins[33]==LOW); assertStopped();
  keep(1950,0,4); assertStopped(); keep(2000,0,4); assert(pedalArmed);
  keep(2400,4095,4); assert(throttleOut==100 && pins[26]==LOW && pins[33]==LOW);
  // A CAN failure stops immediately and preserves reverse, including on recovery.
  txWorks=false; sendPowertrainStatus(); assertStopped(); assert(reverseOut && stopReason==1);
  txWorks=true; sendPowertrainStatus(); servicePowertrain(); assertStopped(); assert(stopReason==5);
  pedal(0); keep(clockMs+400,4095,4); assert(pwm[13]==255);
  // A direction request at throttle still cuts immediately.
  switches(0); assertStopped(); assert(reverseOut);
  keep(clockMs+1100,4095); assert(reverseOut);
  const uint32_t zeroStart=clockMs; pedal(0);
  keep(zeroStart+999,0); assert(reverseOut);
  keep(zeroStart+1000,0); assert(!reverseOut && pins[26]==HIGH && pins[33]==HIGH); assertStopped();
  keep(zeroStart+1999,4095); assertStopped();
  keep(zeroStart+2000,4095); assertStopped(); assert(stopReason==5);
  pedal(0); keep(clockMs+400,4095); assert(pwm[13]==255);
  // Keep the existing one-second zero-command contract used by Node 5.
  // A reverse request halfway through a ramp still forces PWM to zero before
  // either relay can change; the pending command timer must not be shortened.
  full(); keep(clockMs+200,0); assert(pwm[13]>0);
  const uint32_t requestedAt=clockMs; switches(4); assertStopped();
  keep(requestedAt+799,0,4); assert(!reverseOut);
  keep(requestedAt+800,0,4); assert(reverseOut && !pwm[13] && !pwm[14]);

  full(); writesWork=false; servicePowertrain(); assertStopped(); assert(stopReason==2 && !pwmReady);
  writesWork=true; pedal(0); keep(clockMs+400,4095); assertStopped(); // Requires reboot.
  reset(); secondAttach=false; setup(); inputs(0,0); keep(400,4095); assertStopped();

  // Both the ramp clock and input watchdogs work across millis() rollover.
  reset(); inputs(0xfffffff0,0); pedal(4095);
  for(uint32_t n=1;n<=400;++n) {
    ++clockMs;servicePowertrain();if(n%50==0)pedal(4095);
    assert(pwm[13]==int(n*255/400));
  }
  clockMs+=501; servicePowertrain(); assertStopped(); assert(stopReason==3);
  // Exercise the actual loop's CAN adapter, status and heartbeat schedules.
  reset(); incoming.push_back({0x300,1,{0}}); incoming.push_back({0x301,2,{0,0}});
  loop(); assert(pedalArmed);
  incoming.push_back({0x301,2,{255,15}}); loop(); assert(pwm[13]==0);
  for(uint32_t n=0;n<400;++n) {
    if(n%50==0)incoming.push_back({0x301,2,{255,15}});
    loop();
  }
  assert(pwm[13]==255);
  clockMs=1000; incoming.push_back({0x301,2,{0,0}}); loop();
  assert(outgoing.back().id==0x600 && outgoing.back().data[0]==2);
  puts("Node 2 receiver/output tests passed: linear up/down PWM, target changes, loop jitter/stalls, applied-output telemetry, ECO ramp, CAN layouts, idle recovery, reverse timing, immediate faults, heartbeat and clock wrap");
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    cpp = path / 'powertrain.cpp'
    cpp.write_text(shim + source + scenarios)
    for label, flags in [
        ('vehicle-defaults', ['-DTEST_VEHICLE_DEFAULTS']),
        ('bench', ['-DNODE2_ENABLE_TRACTION_OUTPUTS=0', '-DNODE2_ENABLE_REVERSE_RELAYS=0']),
        ('forward-only', ['-DNODE2_ENABLE_TRACTION_OUTPUTS=1', '-DNODE2_ENABLE_REVERSE_RELAYS=0']),
    ]:
        binary = path / f'powertrain-{label}'
        subprocess.run([
            'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'node2_pcm'),
            *flags, str(cpp), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
