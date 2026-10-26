"""Connect the actual Node 3 sender to the actual Node 2 receiver on a fake bus.

GPIOs belong to separate boards. Only Arduino/CAN I/O is mocked; debounce,
payload generation, ownership, watchdogs and relay control run unchanged.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def sketch(name):
    source = (root / name / f'{name}.ino').read_text()
    return re.sub(r'^#include .*$', '', source, flags=re.M)


shim = r'''
#include "node3_ccm/RemoteDrive.h"
#include "node2_pcm/ThrottleCalibration.h"
#include "CanRecovery.h"
#include "TractionMode.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
using byte=uint8_t;
constexpr int LOW=0,HIGH=1,OUTPUT=1,INPUT_PULLUP=2;
constexpr int CAN_OK=0,CAN_MSGAVAIL=1,MCP_ANY=0,MCP_NORMAL=0,CAN_500KBPS=500,MCP_8MHZ=8;
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
void delay(unsigned n){clockMs+=n;}
struct SerialMock {
  void begin(int){}
  template<class... T> void printf(const char*,T...){}
  template<class T> void print(T){}
  template<class T> void println(T){}
  void println(){}
} Serial;
struct SpiMock {void begin(int,int,int,int){}} SPI;
struct Frame {uint32_t id;uint8_t len,data[8]={};};
std::vector<Frame> switchTx,motorTx;
std::deque<Frame> switchRx;
int inputs[40]={};
void pinMode(int,int){}
int digitalRead(int pin){return inputs[pin];}
struct MCP_CAN {
  explicit MCP_CAN(int cs){assert(cs==25);}
  int begin(int mode,int speed,int crystal){
    assert(mode==MCP_ANY && speed==CAN_500KBPS && crystal==MCP_8MHZ);return CAN_OK;
  }
  int setMode(int mode){assert(mode==MCP_NORMAL);return CAN_OK;}
  int getError(){return 0;} int errorCountTX(){return 0;} int errorCountRX(){return 0;} int abortTX(){return 0;}
  int checkReceive(){return switchRx.empty()?0:CAN_MSGAVAIL;}
  int readMsgBuf(unsigned long *id,byte *len,byte *data){
    auto f=switchRx.front();switchRx.pop_front();*id=f.id;*len=f.len;memcpy(data,f.data,8);return CAN_OK;
  }
  int sendMsgBuf(unsigned long id,byte extended,byte len,byte *data){
    assert(!extended && len<=8);Frame f{};f.id=id;f.len=len;memcpy(f.data,data,len);switchTx.push_back(f);return CAN_OK;
  }
};
namespace pcm {
bool canReady=true;
CanRecovery canRecovery;
void canService() {}
void canPrintStatus(const char*) {}
uint8_t canTxCount=0,canRxCount=0;
int outputs[40]={},pwm[40]={},pinModes[40]={};
void pinMode(int pin,int mode){
  if((pin==26 || pin==33) && mode==OUTPUT)assert(outputs[pin]==HIGH);
  pinModes[pin]=mode;
}
using gpio_num_t=int;
int gpio_set_level(gpio_num_t pin,int value){outputs[pin]=value;return 0;}
int digitalRead(int pin){return outputs[pin];}
void digitalWrite(int pin,int value){
  if(pinModes[pin]!=OUTPUT)return;
  if((pin==26 || pin==33) && outputs[pin]!=value)assert(pwm[13]<=62 && pwm[14]<=62);
  outputs[pin]=value;
}
bool ledcAttach(int pin,int hz,int bits){assert((pin==13 || pin==14) && hz==1000 && bits==8);return true;}
bool ledcWrite(int pin,int duty){assert(duty>=0 && duty<=255);pwm[pin]=duty;return true;}
bool canBegin(){return true;}
bool canRead(uint32_t&,uint8_t&,uint8_t(&)[8]){return false;}
template<size_t N>bool canSend(uint32_t id,const uint8_t(&data)[N]){
  Frame f{};f.id=id;f.len=N;memcpy(f.data,data,N);motorTx.push_back(f);++canTxCount;return true;
}
'''
adapter = r'''
}
namespace ccm {
'''
scenarios = r'''
}
size_t delivered=0;
uint16_t physicalAdc=0,remoteSeq=0;
bool pedalOnline=true,remoteRenew=false;
void deliverSwitches(){
  while(delivered<switchTx.size()){
    const auto f=switchTx[delivered++];
    pcm::receivePowertrain(f.id,f.len,f.data);pcm::servicePowertrain();
  }
}
void gate(uint8_t op){
  const uint16_t seq=++remoteSeq;
  Frame f{0x120,8,{op,2,99,0,0,0,(uint8_t)seq,(uint8_t)(seq>>8)}};
  switchRx.push_back(f);pcm::receivePowertrain(f.id,f.len,f.data);
}
void advance(uint32_t duration){
  const uint32_t end=clockMs+duration;
  while(clockMs<end){
    if(pedalOnline && clockMs%50==0){
      const uint8_t p[2]={(uint8_t)physicalAdc,(uint8_t)(physicalAdc>>8)};
      pcm::receivePowertrain(0x301,2,p);
      uint8_t status[8]={(uint8_t)(remoteSeq?(remoteRenew?1:2):0),(uint8_t)physicalAdc,(uint8_t)(physicalAdc>>8),0xD1,99,0,0,0};
      pcm::receivePowertrain(0x121,8,status);
    }
    if(remoteRenew && clockMs%50==0){
      gate(1);
      uint8_t command[8]={0,0,(uint8_t)remoteSeq,(uint8_t)(remoteSeq>>8),99,0,0,0};
      pcm::receivePowertrain(0x123,8,command);
    }
    pcm::servicePowertrain();ccm::loop();deliverSwitches();pcm::servicePowertrain();
    assert(pcm::pwm[13]==pcm::pwm[14]);
  }
}
void expectRelays(bool on){
  assert(pcm::reverseOut==on);
  assert(pcm::outputs[33]==(on?LOW:HIGH) && pcm::outputs[26]==(on?LOW:HIGH));
  pcm::sendPowertrainStatus();assert(motorTx.back().data[2]==on);
  pcm::sendRemoteMotorStatus();assert(bool(motorTx.back().data[0]&0x80)==on);
}
void selectSwitches(uint8_t mask){
  const int wiring[]={4,5,13,14,26,33};
  for(int bit=0;bit<6;++bit)inputs[wiring[bit]]=(mask&(1U<<bit))?LOW:HIGH;
}
int main(){
  std::fill(inputs,inputs+40,HIGH);ccm::setup();pcm::setup();expectRelays(false);
  // The actual sender must send 0x00 at startup, 0x20 for GPIO33 alone.
  advance(10);assert(ccm::buildPayload()==0);expectRelays(false);
  inputs[33]=LOW;advance(25);assert(ccm::buildPayload()==0);expectRelays(false);
  advance(2);assert(ccm::buildPayload()==0x20 && pcm::switchMask==0x20);
  expectRelays(true);assert(pcm::pwm[13]==62 && pcm::stopReason==0);
  inputs[33]=HIGH;advance(27);assert(pcm::switchMask==0);expectRelays(false);
  // No other switch combination may change the direction interpretation.
  bool expectedReverse=false;
  for(uint8_t mask=0;mask<64;++mask){
    selectSwitches(mask);advance(30);assert(pcm::switchMask==mask);
    expectedReverse=mask&SWITCH_REVERSE;
    expectRelays(expectedReverse);
  }
  selectSwitches(0);advance(30);expectRelays(false);
  // The working raw scale has nonzero PWM even at a released 0.7 V pedal.
  physicalAdc=868;advance(500);assert(pcm::pwm[13]==62);
  // Requests shorter than one second now switch as soon as debounce completes.
  for(const auto pulse:{324,945}){
    selectSwitches(0x20);advance(27);expectRelays(true);advance(pulse-27);
    selectSwitches(0);advance(27);expectRelays(false);advance(400);
  }
  selectSwitches(0x20);advance(26);assert(pcm::pwm[13]==62);expectRelays(true);
  selectSwitches(0);advance(27);expectRelays(false);
  puts("Node 3 -> Node 2 relay link passed: boot OFF, GPIO33 ON/OFF within 27 ms of physical input in simulation (25 ms debounce), all 64 switch combinations, nonzero idle, short pulses and both active-LOW relay pins");
  // Expose existing blocks without bypassing them: stale/missing pedal and
  // readings above the current idle threshold cannot satisfy the interlock.
  physicalAdc=0;advance(500);pedalOnline=false;advance(501);
  selectSwitches(0x20);advance(2130);expectRelays(false);assert(pcm::stopReason==3);
  pedalOnline=true;physicalAdc=1241;advance(2130);expectRelays(false);assert(pcm::stopReason==6);
  physicalAdc=0;advance(2130);expectRelays(true);
  puts("Reverse blocks reproduced: missing/stale pedal (reason 3), released reading above ADC 1240 / 1 V (reason 6); valid idle recovers");
  // Actual Node 3 suppression and actual Node 2 direct ownership must ignore
  // physical direction changes until a safe, ordered return to manual.
  selectSwitches(SWITCH_REMOTE_MODE);advance(2130);expectRelays(false);
  remoteRenew=true;advance(250);assert(ccm::driveGate.phase==1 && pcm::remoteGate.phase==1);
  const auto before=switchTx.size();selectSwitches(SWITCH_REMOTE_MODE|SWITCH_REVERSE);advance(1200);expectRelays(false);
  assert(ccm::buildPayload()==(SWITCH_REMOTE_MODE|SWITCH_REVERSE) && pcm::remoteGate.phase==1);
  for(size_t n=before;n<switchTx.size();++n)assert(switchTx[n].id!=0x300);
  remoteRenew=false;advance(350);assert(ccm::driveGate.phase==2 && pcm::remoteGate.phase==2);
  expectRelays(false);gate(2);advance(110);assert(ccm::driveGate.phase==2); // switch mismatch
  selectSwitches(0);advance(30);gate(2);advance(110);assert(ccm::driveGate.phase==0);
  gate(3);advance(110);assert(pcm::remoteGate.phase==0);
  selectSwitches(0x20);advance(2130);expectRelays(true);
  selectSwitches(0);advance(2130);expectRelays(false);
  puts("Reverse ownership link passed: physical switch suppressed during remote control, expiry holds direction, mismatch blocks handback, manual ON/OFF restored after release");
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    cpp = path / 'reverse-link.cpp'
    cpp.write_text(shim + sketch('node2_pcm') + adapter + sketch('node3_ccm') + scenarios)
    binary = path / 'reverse-link'
    subprocess.run([
        'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root),
        str(cpp), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
