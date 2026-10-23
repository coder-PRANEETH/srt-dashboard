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
#include <deque>
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
struct SerialMock {
  std::deque<char> input;
  int available(){return input.size();}
  int read(){const auto c=input.front();input.pop_front();return c;}
  template<class... T> void printf(const char*,T...){}
  void println(const char*){}
} Serial;
int pins[40] = {};
void analogWrite(uint8_t pin,int value){
  assert(pin==32 || pin==33); assert(value>=0 && value<=255);
  pins[pin]=value; assert(!(pins[32] && pins[33]));
}
'''
scenario = '''
void serialKeys(const char *keys){
  while(*keys)Serial.input.push_back(*keys++);
  serviceSerialSteering();serviceSteering();
}
int main(){
  applySteering(); assert(pins[32]==0 && pins[33]==0);
  uint8_t d[8]={1,60,1,0,0,0,0,0}; steeringWrite32(d+4,steering.token);
  assert(steering.accept(d,8,clockMs)); applySteering(); assert(pins[32]==60 && !pins[33]);
  steeringRadioReady=true;
  IMUData packet={25.5f,10};
  onSteeringAngle(nullptr,(const uint8_t*)&packet,sizeof(packet)); serviceSteering();
  assert(steeringFeedback.angle==25.5f && steering.feedbackAvailable);
  steeringWrite32(d+4,steering.token);
  steering.accept(d,8,clockMs); applySteering(); assert(pins[32]==60 && pins[33]==0);
  for(int pwm=1;pwm<=255;++pwm){d[1]=pwm;d[2]=pwm+1;d[3]=(pwm+1)>>8;assert(steering.accept(d,8,clockMs));applySteering();assert(pins[32]==pwm && !pins[33]);}
  d[0]=0; d[1]=0; steering.accept(d,8,clockMs); applySteering(); assert(!pins[32] && !pins[33]);
  d[0]=2; d[1]=60; steeringWrite32(d+4,steering.token);
  steering.accept(d,8,clockMs); applySteering(); assert(pins[32]==0 && pins[33]==60);
  clockMs+=249; serviceSteering(); assert(pins[33]==60);
  ++clockMs; serviceSteering(); assert(!pins[32] && pins[33]==60 && !steering.feedbackAvailable);
  clockMs+=100; serviceSteering(); assert(!pins[32] && !pins[33] && steering.reason==2);
  const auto stoppedToken=steering.token;
  packet.sequence=0; onSteeringAngle(nullptr,(const uint8_t*)&packet,8); serviceSteering();
  assert(steering.feedbackAvailable && !steering.direction && steering.token==stoppedToken);
  assert(!steering.accept(d,8,clockMs)); // Old motion cannot resume after feedback returns.
  steeringWrite32(d+4,steering.token); assert(steering.accept(d,8,clockMs)); applySteering();
  clockMs+=250; ++packet.sequence;
  onSteeringAngle(nullptr,(const uint8_t*)&packet,8); serviceSteering();
  assert(!pins[32] && pins[33]==60); // Angle dropout does not cancel motor speed.
  ++d[2]; steeringWrite32(d+4,steering.token); assert(steering.accept(d,8,clockMs)); applySteering();
  for(int n=0;n<3;++n){clockMs+=100;++packet.sequence;onSteeringAngle(nullptr,(const uint8_t*)&packet,8);serviceSteering();}
  clockMs+=50; ++packet.sequence; onSteeringAngle(nullptr,(const uint8_t*)&packet,8); serviceSteering();
  assert(!pins[32] && !pins[33] && steering.reason==2); // Command watchdog is independent.
  puts("Node 1 output tests passed: ESP-NOW callback, LPWM32/RPWM33, all PWM values 1..255, mutual exclusion, feedback loss/recovery and command timeout");
  // The local check operates without wireless feedback and uses the same GPIO
  // adapter, whose analogWrite mock rejects simultaneous left/right outputs.
  steeringRadioReady=false;clockMs+=500;serviceSteering();
  serialKeys("a\\r\\n");assert(serialSteeringActive && pins[32]==60 && !pins[33]);
  clockMs+=350;serviceSteering();assert(pins[32]==60); // Not the remote 350 ms lease.
  serialKeys("+");assert(serialSteeringPWM==70 && pins[32]==70);
  serialKeys("d");assert(!pins[32] && pins[33]==70);
  serialKeys("-");assert(pins[33]==60);
  for(int n=0;n<30;++n)serialKeys("+");
  assert(serialSteeringPWM==255 && pins[33]==255);
  for(int n=0;n<30;++n)serialKeys("-");
  assert(!serialSteeringActive && serialSteeringPWM==0 && !pins[32] && !pins[33]);
  serialKeys("a");assert(!serialSteeringActive);
  serialKeys("+");assert(serialSteeringPWM==10 && !pins[32] && !pins[33]);
  serialKeys("A");assert(pins[32]==10);const auto runAt=clockMs;
  clockMs=runAt+4999;serialKeys("+");assert(pins[32]==20);
  ++clockMs;serviceSteering();assert(!serialSteeringActive && !pins[32] && !pins[33] && steering.reason==4);
  serialKeys("+");assert(!serialSteeringActive && !steering.direction); // No implicit restart.
  serialKeys("D");assert(pins[33]==30);serialKeys("s");assert(!pins[32] && !pins[33]);
  serialKeys("a ");assert(!serialSteeringActive && !pins[32] && !pins[33]);
  clockMs=0xFFFFFFF0;serialKeys("d");clockMs+=5000;serviceSteering();assert(!serialSteeringActive && !pins[33]);
  steeringWrite32(d+4,steering.token);
  assert(steering.accept(d,8,clockMs)); // Normal CAN works without angle feedback.
  steeringRadioReady=true;++packet.sequence;onSteeringAngle(nullptr,(const uint8_t*)&packet,8);serviceSteering();
  ++d[2];steeringWrite32(d+4,steering.token);assert(steering.accept(d,8,clockMs));applySteering();assert(pins[33]==60);
  serialKeys("s");assert(!pins[32] && !pins[33]);
  puts("Serial steering tests passed: a/d, live speed changes, 0..255 limits, zero/stop, CRLF, no-feedback operation, five-second timeout, no restart and clock wrap");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, cpp in [('state', root / 'dashboard/tools/test-steering.cpp'), ('outputs', tmp / 'outputs.cpp')]:
        if name == 'outputs': cpp.write_text(shim + receiver + adapter + scenario)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(root / 'node1_bcm'),str(cpp),'-o',str(tmp/name)],check=True)
        subprocess.run([str(tmp/name)],check=True)
