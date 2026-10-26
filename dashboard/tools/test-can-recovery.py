"""Exercise the actual shared CAN adapter and recovery against a stalled driver."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = re.sub(r'^#include .*$', '', (root / 'CanCommon.h').read_text(), flags=re.M)
source = source.replace('#pragma once', '')
shim = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include "CanRecovery.h"
#include "SwitchSignals.h"
using std::min;
using INT32U=unsigned long;
using INT8U=uint8_t;
constexpr int INPUT=0,CAN_OK=0,CAN_MSGAVAIL=1,MCP_ANY=0,CAN_500KBPS=500,MCP_8MHZ=8,MCP_NORMAL=0,MCP_LOOPBACK=2;
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
void pinMode(int,int){}
struct SerialMock {
  template<class... T> void printf(const char*,T...){}
  template<class T> void print(T){}
  template<class T> void println(T){}
  void println(){}
} Serial;
struct SpiMock {void begin(int,int,int,int){}} SPI;
struct Frame {uint32_t id;uint8_t len,data[8];};
struct MCP_CAN {
  unsigned starts=0,sends=0,aborts=0;
  bool initWorks=true,modeWorks=true,txWorks=true,queued=false,aborted=false;
  uint8_t flags=0,tec=0,rec=0;
  uint32_t lastId=0;
  std::deque<Frame> incoming;
  explicit MCP_CAN(int){}
  int begin(int,int,int){++starts;if(!initWorks)return 2;queued=aborted=false;flags=tec=rec=0;return 0;}
  int setMode(int){return modeWorks?0:2;}
  int getError(){return flags;} int errorCountTX(){return tec;} int errorCountRX(){return rec;}
  int abortTX(){++aborts;queued=false;aborted=true;return 0;}
  int sendMsgBuf(unsigned long id,int,int,uint8_t*){
    assert(!aborted);++sends;lastId=id;
    if(!txWorks){queued=true;return 2;}
    return 0;
  }
  int checkReceive(){return incoming.empty()?0:CAN_MSGAVAIL;}
  int readMsgBuf(unsigned long *id,uint8_t *len,uint8_t *data){
    auto f=incoming.front();incoming.pop_front();*id=f.id;*len=f.len;memcpy(data,f.data,8);return 0;
  }
};
'''
scenarios = r'''
int main(){
  assert(canBegin());const uint8_t cmd[8]={1,1};
  assert(canSend(0x112,cmd));canHeartbeat(2);assert(canBus.lastId==0x602);
  canHeartbeat(5);assert(canBus.lastId==0x605);
  // A driver timeout can leave a positive command queued. Abort it immediately
  // and refuse every further TX until reset clears the driver's latched ABAT.
  canBus.txWorks=false;canBus.tec=128;clockMs=50;
  assert(!canSend(0x112,cmd) && !canReady && !canBus.queued && canBus.aborted);
  assert(canRecovery.reason==1 && canRecovery.txErrors==128 && canRecovery.faults==1);
  const auto attempts=canBus.sends;
  assert(!canSend(0x123,cmd) && canBus.sends==attempts);
  assert(!canRecovery.retryDue(1049,0));assert(canRecovery.retryDue(1050,0));
  clockMs=1050;canBus.txWorks=true;assert(canBegin());assert(canRecovery.restarts==1 && !canBus.aborted);
  assert(canSend(0x112,cmd));
  // Bus-off after an apparently healthy initialization triggers recovery.
  clockMs+=100;canBus.flags=0x20;canBus.tec=255;canService();
  assert(!canReady && canRecovery.reason==2 && canRecovery.errorFlags==0x20);
  clockMs+=1000;assert(canBegin());
  // Short passive transients and old overflow flags do not reset healthy links.
  canBus.flags=0x10;clockMs+=100;canService();assert(canReady);
  clockMs+=200;canService();assert(canReady);
  canBus.flags=0xC0;clockMs+=100;canService();assert(canReady);
  canBus.flags=0x08;clockMs+=100;canService();
  clockMs+=300;canService();assert(!canReady && canRecovery.reason==3);
  clockMs+=1000;assert(canBegin());
  // A frozen receive path is detected even when TX falsely reports success.
  canBus.incoming.push_back({0x300,1,{0}});uint32_t id;uint8_t len,data[8];
  assert(canRead(id,len,data));clockMs+=1499;canService();assert(canReady);
  clockMs+=100;canService();assert(!canReady && canRecovery.reason==4);
  clockMs+=1000;assert(canBegin());
  canBus.flags=0xFF;clockMs+=100;canService();assert(!canReady && canRecovery.reason==5);
  clockMs+=1000;canBus.initWorks=false;const auto before=canBus.starts;
  assert(!canBegin() && canBus.starts==before+1); // One bounded attempt, no delay/retry loop.
  canBus.initWorks=true;clockMs=0xFFFFFF00;assert(canBegin());
  canRecovery.received(clockMs);clockMs+=1600;canService();assert(!canReady && canRecovery.reason==4);
  assert(!canRecovery.retryDue(clockMs+999,clockMs));assert(canRecovery.retryDue(clockMs+1000,clockMs));
  puts("Actual CAN adapter recovery passed: unique heartbeat IDs, stalled TX abort, no queued replay, bus-off/passive/SPI faults, RX silence, bounded retry and millis wrap");
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'recovery.cpp'
    cpp.write_text(shim + source + scenarios)
    binary = Path(directory) / 'recovery'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
