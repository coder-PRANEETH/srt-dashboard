"""Test the steering lease and the actual Node 1 GPIO-output adapter without hardware."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'node1_bcm' / 'node1_bcm.ino').read_text()
adapter = source[source.index('#define MOTOR_LPWM'):source.index('#define CAN_SCK')]
receiver = source[source.index('constexpr uint8_t STEERING_WIFI_CHANNEL'):source.index('void setupSteeringReceiver()')]
shim = '''
#include <cassert>
#include <cstdio>
#include "SteeringControl.h"
#include "SteeringFeedback.h"
#define ESP_ARDUINO_VERSION_MAJOR 3
struct esp_now_recv_info_t {};
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*){}
void portEXIT_CRITICAL(portMUX_TYPE*){}
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
  assert(!steering.accept(d,8,clockMs)); applySteering(); assert(!pins[32] && !pins[33]);
  steeringRadioReady=true;
  IMUData packet={25.5f,10};
  onSteeringAngle(nullptr,(const uint8_t*)&packet,sizeof(packet)); serviceSteering();
  assert(steeringFeedback.angle==25.5f && steering.feedbackAvailable);
  steeringWrite32(d+4,steering.token);
  steering.accept(d,8,clockMs); applySteering(); assert(pins[32]==60 && pins[33]==0);
  d[0]=0; d[1]=0; steering.accept(d,8,clockMs); applySteering(); assert(!pins[32] && !pins[33]);
  d[0]=2; d[1]=60; steeringWrite32(d+4,steering.token);
  steering.accept(d,8,clockMs); applySteering(); assert(pins[32]==0 && pins[33]==60);
  clockMs+=249; serviceSteering(); assert(pins[33]==60);
  ++clockMs; serviceSteering(); assert(!pins[32] && !pins[33] && steering.reason==5);
  const auto stoppedToken=steering.token;
  packet.sequence=0; onSteeringAngle(nullptr,(const uint8_t*)&packet,8); serviceSteering();
  assert(steering.feedbackAvailable && !steering.direction && steering.token==stoppedToken);
  assert(!steering.accept(d,8,clockMs)); // Old motion cannot resume after feedback returns.
  steeringWrite32(d+4,steering.token); assert(steering.accept(d,8,clockMs)); applySteering();
  clockMs+=250; ++packet.sequence;
  onSteeringAngle(nullptr,(const uint8_t*)&packet,8); serviceSteering();
  assert(!pins[32] && !pins[33] && steering.reason==5); // Recovery packet cannot hide a gap.
  steeringWrite32(d+4,steering.token); assert(steering.accept(d,8,clockMs)); applySteering();
  for(int n=0;n<3;++n){clockMs+=100;++packet.sequence;onSteeringAngle(nullptr,(const uint8_t*)&packet,8);serviceSteering();}
  clockMs+=50; ++packet.sequence; onSteeringAngle(nullptr,(const uint8_t*)&packet,8); serviceSteering();
  assert(!pins[32] && !pins[33] && steering.reason==2); // Command watchdog is independent.
  puts("Node 1 output tests passed: ESP-NOW callback, LPWM32/RPWM33, PWM60, mutual exclusion, feedback loss/recovery and command timeout");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, cpp in [('state', root / 'dashboard/tools/test-steering.cpp'), ('outputs', tmp / 'outputs.cpp')]:
        if name == 'outputs': cpp.write_text(shim + receiver + adapter + scenario)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(root / 'node1_bcm'),str(cpp),'-o',str(tmp/name)],check=True)
        subprocess.run([str(tmp/name)],check=True)
