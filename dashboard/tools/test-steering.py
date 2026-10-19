"""Test the steering lease and the actual Node 1 GPIO-output adapter without hardware."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'node1_bcm.ino').read_text()
adapter = source[source.index('#define MOTOR_LPWM'):source.index('#define CAN_SCK')]
shim = '''
#include <cassert>
#include <cstdio>
#include "SteeringControl.h"
uint32_t clockMs=100;
uint32_t millis(){return clockMs;}
int pins[40] = {};
void analogWrite(uint8_t pin,int value){
  assert(pin==32 || pin==33); assert(value==0 || value==60);
  pins[pin]=value; assert(!(pins[32] && pins[33]));
}
'''
scenario = '''
int main(){
  applySteering(); assert(pins[32]==0 && pins[33]==0);
  uint8_t d[8]={1,60,1,0,0,0,0,0}; steeringWrite32(d+4,steering.token);
  steering.accept(d,8,clockMs); applySteering(); assert(pins[32]==60 && pins[33]==0);
  d[0]=0; d[1]=0; steering.accept(d,8,clockMs); applySteering(); assert(!pins[32] && !pins[33]);
  d[0]=2; d[1]=60; steeringWrite32(d+4,steering.token);
  steering.accept(d,8,clockMs); applySteering(); assert(pins[32]==0 && pins[33]==60);
  clockMs+=350; serviceSteering(); assert(!pins[32] && !pins[33]);
  puts("Node 1 output tests passed: LPWM32/RPWM33, PWM60, mutual exclusion, local timeout");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, cpp in [('state', root / 'dashboard/tools/test-steering.cpp'), ('outputs', tmp / 'outputs.cpp')]:
        if name == 'outputs': cpp.write_text(shim + adapter + scenario)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(root),str(cpp),'-o',str(tmp/name)],check=True)
        subprocess.run([str(tmp/name)],check=True)
