"""Compile the actual Node 2 sketch against mocked CAN/PWM/relay I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'node2_pcm' / 'node2_pcm.ino').read_text().replace('#include "CanCommon.h"', '').replace('#include <driver/gpio.h>', '')
shim = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "CanRecovery.h"
#include "TractionMode.h"
constexpr int LOW=0, HIGH=1, OUTPUT=1;
uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
void delay(unsigned n) { clockMs+=n; }
struct SerialMock {
  void begin(int) {}
  template<class... T> void printf(const char*, T...) {}
} Serial;
bool canReady=true, txWorks=true, writesWork=true, firstAttach=true, secondAttach=true;
CanRecovery canRecovery;
void canService() {}
void canPrintStatus(const char*) {}
uint8_t canTxCount=0, canRxCount=0;
int pins[40]={}, pwm[40]={}, pinModes[40]={};
void pinMode(int pin,int mode) {
  // The active-low relay input must already be OFF when OUTPUT is enabled.
  if ((pin==26 || pin==33) && mode==OUTPUT) assert(pins[pin]==HIGH);
  pinModes[pin]=mode;
}
using gpio_num_t=int;
int gpio_set_level(gpio_num_t pin,int value) { pins[pin]=value; return 0; }
int digitalRead(int pin) { return pins[pin]; }
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
  // Match ESP32 core 3.x: an unconfigured pin ignores digitalWrite.
  if(pinModes[pin]!=OUTPUT)return;
  if ((pin==26 || pin==33) && pins[pin]!=value) assert(pwm[13]<=62 && pwm[14]<=62);
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
  memset(pins,0,sizeof(pins)); memset(pwm,0,sizeof(pwm)); memset(pinModes,0,sizeof(pinModes));
  acceleratorADC=0; switchMask=0; throttleOut=0; stopReason=3;
  accelSeen=false; switchSeen=false; pwmReady=false;
  pedalArmed=false; reverseOut=false; txFault=false;
  accelAt=switchAt=lastStatus=lastBeat=lastPrint=lastCanAttempt=0;
  invalidFrames=0; canTxCount=canRxCount=0; incoming.clear(); outgoing.clear();
  remoteGate=RemoteDriveGate{}; remotePeerSeen=remoteSwitchSeen=remoteCommandSeen=false;
  tractionMode=TractionMode{};
  remotePeerAt=remoteSwitchAt=remoteCommandAt=lastRemoteStatus=0;remotePwm=0;remoteSequence=0;
  setup(); assert(pwm[13]==(ENABLE_TRACTION_OUTPUTS?62:0) && pwm[14]==pwm[13] && pins[26]==HIGH && pins[33]==HIGH);
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
void assertStopped() { const int idle=ENABLE_TRACTION_OUTPUTS && pwmReady?62:0; assert(throttleDuty==idle && pwm[13]==idle && pwm[14]==idle); }
void assertRemoteIdle() { assert(throttleDuty==62 && pwm[13]==62 && pwm[14]==62); }
void assertReverseStatus(bool reverse) {
  sendPowertrainStatus();const auto output=outgoing.back();
  sendRemoteMotorStatus();const auto gate=outgoing.back();
  assert(output.id==0x201 && output.data[2]==reverse);
  assert(gate.id==0x124 && bool(gate.data[0]&0x80)==reverse);
  assert((gate.data[0]&0x7F)==remoteGate.phase && gate.data[1]==throttleDuty);
}
void full() { reset(); inputs(0,0); keep(THROTTLE_RAMP_UP_MS,4095); assert(throttleOut==100 && pwm[13]==255); }
void resumeFull() { switches(0); pedal(0); keep(clockMs+THROTTLE_RAMP_UP_MS,4095); assert(pwm[13]==255); }
uint16_t remoteTestSeq=0;
void remoteInputs(uint8_t duty,bool rev=false,uint16_t physical=0,uint8_t mask=0) {
  uint8_t p[8]={1,(uint8_t)physical,(uint8_t)(physical>>8),0xD1,99,0,0,0};
  const uint8_t physicalSwitches=SWITCH_REMOTE_MODE|mask;
  receivePowertrain(0x300,1,&physicalSwitches);
  uint8_t s[8]={1,physicalSwitches,0,0xD1,99,0,0,0};
  receivePowertrain(0x121,8,p);receivePowertrain(0x122,8,s);
  ++remoteTestSeq;
  uint8_t gate[8]={1,(uint8_t)(2|rev),99,0,0,0,(uint8_t)remoteTestSeq,(uint8_t)(remoteTestSeq>>8)};
  receivePowertrain(0x120,8,gate);
  uint8_t command[8]={duty,(uint8_t)rev,(uint8_t)remoteTestSeq,(uint8_t)(remoteTestSeq>>8),99,0,0,0};
  receivePowertrain(0x123,8,command);servicePowertrain();
}
void keepRemote(uint32_t until,uint8_t duty,bool rev=false,uint16_t physical=0,uint8_t mask=0) {
  remoteInputs(duty,rev,physical,mask);
  while(clockMs<until) {++clockMs;if(clockMs%50==0)remoteInputs(duty,rev,physical,mask);servicePowertrain();assert(pwm[13]==pwm[14]);}
}
int main() {
  assert(MOTOR_1==13 && MOTOR_2==14 && REVERSE_1==33 && REVERSE_2==26);
  assert(REVERSE_RELAY_ON==LOW && REVERSE_RELAY_OFF==HIGH);
  assert(ACCEL_TIMEOUT_MS==501 && SWITCH_TIMEOUT_MS==3001);
#ifdef TEST_VEHICLE_DEFAULTS
  // Do not override the sketch's defaults: this caught the vehicle no-drive bug.
  assert(ENABLE_TRACTION_OUTPUTS && ENABLE_REVERSE_RELAYS);
#endif
  reset(); assertReverseStatus(false); inputs(0,4095); assertStopped();
  if (!ENABLE_TRACTION_OUTPUTS) {
    assert(stopReason==7); inputs(10,0,SWITCH_REVERSE); keep(2500,0,SWITCH_REVERSE);
    inputs(2550,4095,SWITCH_REVERSE); assertStopped(); assert(pins[26]==HIGH && pins[33]==HIGH);
    sendPowertrainStatus(); assert(outgoing.back().data[3]==0);
    puts("Node 2 bench override passed: PWM and reverse disabled, active-low relays OFF"); return 0;
  }
  assert(stopReason==5); // Booting with a pressed pedal cannot move.
  reset();inputs(0,0);
  for(int adc=0;adc<=4095;++adc) {
    const int originalPercent=adc*100/4095,originalPWM=originalPercent*255/100<62 ? 62 : originalPercent*255/100;
    keep(clockMs+400,adc);
    assert(manualThrottlePercent(adc)==originalPercent && pwm[13]==originalPWM && pwm[14]==originalPWM);
  }
  puts("Working-code compatibility passed: all 4096 physical ADC values match max(62, original PWM), preserving the 0.8 V idle minimum");
  reset();inputs(0,4095);assert(stopReason==5);
  pedal(868); assert(pedalArmed); pedal(4095); assertStopped();
  // One full-scale jump becomes a linear configured-time PWM ramp, independent of
  // how many times the control function is called within one millisecond.
  int previous=62;
  for(uint32_t n=1;n<=THROTTLE_RAMP_UP_MS;++n) {
    ++clockMs; servicePowertrain();
    if(n%50==0) pedal(4095);
    assert(pwm[13]==int(62+n*193/THROTTLE_RAMP_UP_MS));
    assert(pwm[13]>=previous && pwm[13]-previous<=1 && pwm[13]==pwm[14]);
    previous=pwm[13];
    for(int duplicate=0;duplicate<5;++duplicate)servicePowertrain();
    assert(pwm[13]==previous);
    if(n==THROTTLE_RAMP_UP_MS/2) {sendPowertrainStatus();assert(outgoing.back().data[3]==62 && throttleTarget==100);}
  }
  assert(throttleOut==100 && pwm[13]==255);
  pedal(0); assert(pwm[13]==255); // Ordinary release also ramps, without a jump.
  for(uint32_t n=1;n<=THROTTLE_RAMP_DOWN_MS;++n) {
    ++clockMs; servicePowertrain(); if(n%50==0)pedal(0);
    assert(pwm[13]==int(62+(THROTTLE_RAMP_DOWN_MS-n)*193/THROTTLE_RAMP_DOWN_MS));
    assert(pwm[13]<=previous && previous-pwm[13]<=1 && pwm[13]==pwm[14]);
    previous=pwm[13];
    if(n==398) { // Idle carries a nonzero voltage but is not an active traction request.
      sendPowertrainStatus(); assert(outgoing.back().data[0]==0 && outgoing.back().data[3]==24);
    }
  }
  assertStopped(); assert(pedalArmed && stopReason==0);

  // Cruise captures the currently applied output and ignores later pedal changes.
  reset();inputs(0,0);keep(300,2000);const uint8_t cruisePwm=pwm[13];
  switches(SWITCH_CRUISE);assert(cruiseActive && pwm[13]==cruisePwm);
  keep(clockMs+350,4095,SWITCH_CRUISE);assert(cruiseActive && pwm[13]==cruisePwm);
  sendPowertrainStatus();assert(outgoing.back().data[4]==1);
  switches(0);assert(!cruiseActive);keep(clockMs+200,4095);assert(pwm[13]>cruisePwm);

  // Remote mode blocks the physical pedal until a remote owner claims the gate.
  reset();inputs(0,0);switches(SWITCH_REMOTE_MODE);assertStopped() ;assert(stopReason==9);
  switches(SWITCH_REMOTE_MODE|SWITCH_REVERSE);assertStopped();assert(reverseOut && stopReason==9);
  switches(SWITCH_REMOTE_MODE);assertStopped();assert(!reverseOut && stopReason==9);
  pedal(4095);assertStopped();assert(stopReason==9);
  remoteTestSeq=0;remoteInputs(0);assert(remoteGate.phase==1 && stopReason==0);
  keepRemote(clockMs+100,120);const int heldRemotePwm=pwm[13];
  pedal(4095);assert(pwm[13]==heldRemotePwm && stopReason==0);
  switches(0);assertRemoteIdle();assert(remoteGate.phase==2 && stopReason==9);

  reset(); inputs(0,0); keep(THROTTLE_RAMP_UP_MS,2000);
  assert(acceleratorADC==2000 && throttleOut==48 && pwm[13]==122);
  keep(clockMs+100,2000); assert(pwm[13]==122); // Exact working receiver target, without overshoot.
  full(); pedal(2000); assert(pwm[13]==255);
  keep(clockMs+100,2000); assert(pwm[13]==206 && throttleTarget==48);
  pedal(4095); assert(pwm[13]==206); keep(clockMs+THROTTLE_RAMP_UP_MS,4095); assert(pwm[13]==255);
  // Autonomous and horn remain indications; unavailable hill selection blocks drive.
  for(uint8_t bit : {SWITCH_AUTONOMOUS,SWITCH_HORN}) {
    switches(bit); assert(pwm[13]==255 && throttleTarget==100 && !reverseOut);
  }
  keep(clockMs+240,4095,SWITCH_HILL_HOLD|SWITCH_AUTONOMOUS|SWITCH_HORN);
  assertStopped();assert(stopReason==10 && !pedalArmed);
  sendPowertrainStatus(); const auto s=outgoing.back();
  assert(s.id==0x201 && s.len==8 && s.data[0]==0 && s.data[1]==0);
  assert(s.data[2]==0 && s.data[3]==24 && s.data[4]==0 && s.data[5]==0 && s.data[6]==0 && s.data[7]==10);
  keep(clockMs+400,0,SWITCH_HILL_HOLD|SWITCH_AUTONOMOUS|SWITCH_HORN); assertStopped(); // Requests cannot add throttle.

  pedal(4095);switches(0);assertStopped();assert(stopReason==5);
  pedal(0);assert(stopReason==0); // Leaving blocked hill mode still requires release.

  // Irregular 1..9 ms scheduling gives the same ramp as the 1 ms loop.
  reset(); inputs(0,0); pedal(4095);
  const uint32_t spacing[]={1,7,3,9,2,6};
  for(uint32_t n=0;clockMs<THROTTLE_RAMP_UP_MS;++n) {
    uint32_t dt=spacing[n%6]; if(dt>THROTTLE_RAMP_UP_MS-clockMs)dt=THROTTLE_RAMP_UP_MS-clockMs;
    clockMs+=dt; servicePowertrain(); pedal(4095);
    assert(pwm[13]==int(62+clockMs*193/THROTTLE_RAMP_UP_MS));
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
  switches(0x40); assertStopped(); assert(!switchSeen); // Bit 6 is now reserved.
  switches(0x80); assertStopped(); switches(0); pedal(4095); assertStopped();
  keep(clockMs+THROTTLE_RAMP_UP_MS,4095); assert(pwm[13]==255);
  receivePowertrain(0x300,8,old); servicePowertrain(); assertStopped(); assert(invalidFrames==5);

  if (!ENABLE_REVERSE_RELAYS) {
    reset(); inputs(0,0,SWITCH_REVERSE); keep(2100,0,SWITCH_REVERSE); inputs(2150,4095,SWITCH_REVERSE);
    assertStopped(); assert(stopReason==8 && !reverseOut);
    puts("Node 2 reverse-disabled guard passed"); return 0;
  }
  reset(); inputs(0,0,SWITCH_REVERSE);
  assert(clockMs==0 && reverseOut && pins[26]==LOW && pins[33]==LOW); assertStopped();
  assert(remoteGate.phase==0 && !remoteGate.reverse);assertReverseStatus(true);
  assert(pedalArmed);
  keep(THROTTLE_RAMP_UP_MS,4095,SWITCH_REVERSE); assert(throttleOut==100 && pins[26]==LOW && pins[33]==LOW);
  // A CAN failure stops immediately and preserves reverse, including on recovery.
  txWorks=false; sendPowertrainStatus(); assertStopped(); assert(reverseOut && stopReason==1);
  txWorks=true; sendPowertrainStatus(); servicePowertrain(); assertStopped(); assert(stopReason==5);
  pedal(0); keep(clockMs+THROTTLE_RAMP_UP_MS,4095,SWITCH_REVERSE); assert(pwm[13]==255);
  // A direction request at throttle still cuts immediately.
  switches(0); assertStopped(); assert(reverseOut);
  keep(clockMs+1100,4095); assert(reverseOut);
  const uint32_t zeroStart=clockMs; pedal(0);
  assert(clockMs==zeroStart && !reverseOut && pins[26]==HIGH && pins[33]==HIGH); assertStopped();
  assertReverseStatus(false);
  keep(clockMs+THROTTLE_RAMP_UP_MS,4095); assert(pwm[13]==255);
  // A request during ramp-down cuts PWM before writing either relay, without
  // an added timer. digitalWrite's I/O mock asserts that ordering at each pin.
  full(); keep(clockMs+200,0); assert(pwm[13]>0);
  const uint32_t requestedAt=clockMs; switches(SWITCH_REVERSE); assertStopped();
  assert(clockMs==requestedAt && reverseOut && pwm[13]==62 && pwm[14]==62);
  // Include the actual released ADC from the user's log and the idle boundary.
  for(uint16_t idleAdc : {0,795,868,869,930,992}) {
    reset();inputs(0,idleAdc);keep(400,idleAdc);
    const uint32_t shiftAt=clockMs;
    switches(SWITCH_REVERSE);assert(clockMs==shiftAt && reverseOut);assertStopped();
    assertReverseStatus(true);
    keep(clockMs+400,idleAdc,SWITCH_REVERSE);
    assert(pwm[13]==62);
    const uint32_t forwardAt=clockMs;
    switches(0);assert(clockMs==forwardAt && !reverseOut);assertStopped();
    assert(pins[33]==HIGH && pins[26]==HIGH);assertReverseStatus(false);
  }

  full(); writesWork=false; servicePowertrain(); assertStopped(); assert(stopReason==2 && !pwmReady);
  writesWork=true; pedal(0); keep(clockMs+THROTTLE_RAMP_UP_MS,4095); assertStopped(); // Requires reboot.
  reset(); secondAttach=false; setup(); inputs(0,0); keep(THROTTLE_RAMP_UP_MS,4095); assertStopped();

  // Both the ramp clock and input watchdogs work across millis() rollover.
  reset(); inputs(0xfffffff0,0); pedal(4095);
  for(uint32_t n=1;n<=THROTTLE_RAMP_UP_MS;++n) {
    ++clockMs;servicePowertrain();if(n%50==0)pedal(4095);
    assert(pwm[13]==int(62+n*193/THROTTLE_RAMP_UP_MS));
  }
  clockMs+=501; servicePowertrain(); assertStopped(); assert(stopReason==3);
  // Exercise the actual loop's CAN adapter, status and heartbeat schedules.
  reset(); incoming.push_back({0x300,1,{0}}); incoming.push_back({0x301,2,{0,0}});
  loop(); assert(pedalArmed);
  incoming.push_back({0x301,2,{255,15}}); loop(); assert(pwm[13]==62);
  for(uint32_t n=0;n<THROTTLE_RAMP_UP_MS;++n) {
    if(n%50==0)incoming.push_back({0x301,2,{255,15}});
    loop();
  }
  assert(pwm[13]==255);
  clockMs+=1000; incoming.push_back({0x301,2,{0,0}}); loop();
  assert(outgoing.back().id==0x602 && outgoing.back().data[0]==2);
  for(uint16_t idleAdc : {869,930,992,993,1100,1240,1241}) {
    reset();inputs(0,idleAdc);remoteTestSeq=0;remoteInputs(0,false,idleAdc);
    assert((remoteGate.phase==1)==(idleAdc<=1240));
  }
  // Actual joystick protocol: all 256 demand values map above the idle baseline.
  reset();inputs(0,0);remoteTestSeq=0;remoteInputs(0);assert(remoteGate.phase==1 && pedalArmed);
  keepRemote(THROTTLE_RAMP_UP_MS,180);assert(pwm[13]==198 && throttleDuty==198);
  keepRemote(clockMs+THROTTLE_RAMP_DOWN_MS,0);assertRemoteIdle();
  for(int duty=0;duty<=255;++duty){keepRemote(clockMs+(THROTTLE_RAMP_UP_MS+192)/193,duty);assert(pwm[13]==62+(193*duty)/255);}
  keepRemote(clockMs+400,180,false,4095);assert(pwm[13]==198 && remoteGate.phase==1);
  // Physical input frames cannot override either throttle or direction.
  pedal(4095);switches(SWITCH_REMOTE_MODE|SWITCH_REVERSE);assert(pwm[13]==198 && !reverseOut && remoteGate.phase==1);
  keepRemote(clockMs+1000,255,false,4095);assert(pwm[13]==255);
  // Remote direction still requires a zero command, then switches immediately.
  remoteInputs(180,true,4095);assert(!reverseOut && stopReason==6);assertRemoteIdle();
  const uint32_t shiftStart=clockMs;remoteInputs(0,true,4095);
  assert(clockMs==shiftStart && reverseOut && pins[33]==LOW && pins[26]==LOW);
  assertReverseStatus(true);
  keepRemote(clockMs+THROTTLE_RAMP_UP_MS,180,true,4095);assert(pwm[13]==198);
  // Expiry latches the electrical idle signal even with a fully pressed physical pedal.
  clockMs+=350;servicePowertrain();assertRemoteIdle();assert(remoteGate.phase==2 && stopReason==9);
  pedal(4095);switches(0);assertRemoteIdle();
  uint8_t release[8]={3,3,99,0,0,0,1,0};receivePowertrain(0x120,8,release);servicePowertrain();assert(remoteGate.phase==2);
  uint8_t p[8]={0,0,0,0xD1,99,0,0,0},releasedSwitch[8]={0,0,0,0xD1,99,0,0,0};
  receivePowertrain(0x121,8,p);receivePowertrain(0x122,8,releasedSwitch);receivePowertrain(0x120,8,release);
  assert(remoteGate.phase==0);pedal(0);switches(SWITCH_REVERSE);keep(clockMs+THROTTLE_RAMP_UP_MS,4095,SWITCH_REVERSE);assert(pwm[13]==255);
  reset();inputs(0,869);remoteTestSeq=0;remoteInputs(0);keepRemote(400,0);assertRemoteIdle();
  const uint32_t riseAt=clockMs;
  int remotePrevious=pwm[13];
  for(unsigned n=1;n<=THROTTLE_RAMP_UP_MS;++n) {
    keepRemote(riseAt+n,255);
    const int expected=62+(n*193/THROTTLE_RAMP_UP_MS); // Same bounded electrical slew as pedal mode.
    assert(pwm[13]==(expected>255?255:expected));
    assert(pwm[13]>=remotePrevious && pwm[13]-remotePrevious<=1);
    remotePrevious=pwm[13];
  }
  // A latched command cannot return a positive demand when the lease expires.
  clockMs+=350;servicePowertrain();assertRemoteIdle();assert(remoteGate.phase==2);
  txWorks=false;sendPowertrainStatus();assertStopped(); // CAN loss also retains the 0.8 V idle output.
  reset();inputs(0,0);remoteTestSeq=0;remoteInputs(0);keepRemote(100,180);
  remoteInputs(180,false,4095,SWITCH_HILL_HOLD);servicePowertrain();
  assertRemoteIdle();assert(remoteGate.phase==2 && stopReason==10);
  puts("Unavailable hill mode passed: pedal ignored at idle, remote drive cancelled, explicit pedal release needed to resume");
  puts("Joystick motor tests passed: all 256 demand values mapped above idle, physical pedal/switch blocking, HillHold indication, active-low reversal, lease expiry and idle-only handback");
  puts("Node 2 receiver/output tests passed: linear up/down PWM, target changes, loop jitter/stalls, applied-output telemetry, six switch signals, CAN layouts, idle recovery, reverse timing, immediate faults, heartbeat and clock wrap");
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
