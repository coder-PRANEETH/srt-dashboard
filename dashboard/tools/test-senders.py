"""Run the actual Node 1/3 sketches with mocked pins, CAN and radio."""
from pathlib import Path
import json
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
common = r'''
#include <algorithm>
#include "SwitchSignals.h"
#include "CanRecovery.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
using byte=uint8_t;
using std::min;
using std::isfinite;
constexpr int LOW=0,HIGH=1,INPUT=0,INPUT_PULLUP=2,OUTPUT=1;
constexpr int CAN_OK=0,CAN_MSGAVAIL=1,MCP_ANY=0,MCP_NORMAL=0,CAN_500KBPS=500,MCP_8MHZ=8;
constexpr int SERIAL_8N1=0;
constexpr float PI=3.14159265358979323846f;
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
void delay(unsigned n){clockMs+=n;}
int pins[40]={};
int pinModes[40]={};
int hornLatchAtOutput=-1;
void pinMode(int pin,int mode){if(pin==13 && mode==OUTPUT)hornLatchAtOutput=pins[pin];pinModes[pin]=mode;}
int digitalRead(int pin){return pins[pin];}
void digitalWrite(int pin,int value){pins[pin]=value;}
template<class T> T constrain(T value,T low,T high){return std::max(low,std::min(high,value));}
struct SerialMock {
  std::deque<char> input;
  int available(){return input.size();}
  int read(){const auto c=input.front();input.pop_front();return c;}
  void begin(int){}
  template<class... T> void printf(const char*,T...){}
  template<class T> void print(T){}
  template<class T> void print(T,int){}
  template<class T> void println(T){}
  void println(){}
} Serial;
struct SpiMock {void begin(int sck,int miso,int mosi,int cs){assert(sck==18 && miso==19 && mosi==23 && cs==25);}} SPI;
struct Frame {uint32_t id; uint8_t len,data[8]={};};
std::deque<Frame> incoming;
std::vector<Frame> outgoing;
void push(uint32_t id,std::initializer_list<uint8_t> bytes){
  Frame f{};f.id=id;f.len=bytes.size();std::copy(bytes.begin(),bytes.end(),f.data);incoming.push_back(f);
}
Frame last(uint32_t id){
  for(auto f=outgoing.rbegin();f!=outgoing.rend();++f)if(f->id==id)return *f;
  assert(false);return {};
}
size_t count(uint32_t id){return std::count_if(outgoing.begin(),outgoing.end(),[&](const Frame &f){return f.id==id;});}
bool canInitWorks=true,canModeWorks=true,canTxWorks=true;
unsigned canInitCalls=0,canAbortCalls=0;
uint8_t canErrorFlags=0;
struct MCP_CAN {
  explicit MCP_CAN(int cs){assert(cs==25);}
  int begin(int mode,int speed,int crystal){
    assert(mode==MCP_ANY && speed==CAN_500KBPS && crystal==MCP_8MHZ);
    ++canInitCalls;return canInitWorks?CAN_OK:2;
  }
  int setMode(int mode){assert(mode==MCP_NORMAL);return canModeWorks?CAN_OK:2;}
  int getError(){return canErrorFlags;}
  int errorCountTX(){return canErrorFlags?255:0;}
  int errorCountRX(){return 0;}
  int abortTX(){++canAbortCalls;return CAN_OK;}
  int checkReceive(){return incoming.empty()?0:CAN_MSGAVAIL;}
  int readMsgBuf(unsigned long *id,byte *len,byte *bytes){
    assert(!incoming.empty());auto f=incoming.front();incoming.pop_front();*id=f.id;*len=f.len;memcpy(bytes,f.data,8);return CAN_OK;
  }
  int sendMsgBuf(unsigned long id,byte,byte len,byte *bytes){
    if(!canTxWorks)return 2;
    Frame f{};f.id=id;f.len=len;memcpy(f.data,bytes,len);outgoing.push_back(f);return CAN_OK;
  }
};
'''
node1_io = r'''
#define ESP_ARDUINO_VERSION_MAJOR 3
using gpio_num_t=int;
int gpio_set_level(gpio_num_t pin,int value){pins[pin]=value;return 0;}
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*){}
void portEXIT_CRITICAL(portMUX_TYPE*){}
constexpr int WIFI_STA=1,WIFI_SECOND_CHAN_NONE=0,ESP_OK=0;
struct esp_now_recv_info_t {};
struct WifiMock {
  bool mode(int){return true;}void setSleep(bool){}void setAutoReconnect(bool){}
  const char *macAddress(){return "68:09:47:DF:8E:E8";}
} WiFi;
int esp_wifi_set_channel(uint8_t channel,int){assert(channel==1);return ESP_OK;}
int esp_now_init(){return ESP_OK;}int esp_now_deinit(){return ESP_OK;}
int esp_now_register_recv_cb(void (*)(const esp_now_recv_info_t*,const uint8_t*,int)){return ESP_OK;}
uint32_t esp_random(){return 9;}
int inputAdc=0;
void analogReadResolution(int bits){assert(bits==12);}
int analogRead(int pin){assert(pin==34);return inputAdc;}
void analogWrite(int pin,int value){
  assert((pin==32 || pin==33) && value>=0 && value<=255);
  pins[pin]=value;assert(!(pins[32] && pins[33]));
}
'''
node1 = r'''
int main(){
  setup();assert(canReady && steeringRadioReady);
  assert(pinModes[13]==OUTPUT && pins[13]==HORN_RELAY_OFF && hornLatchAtOutput==HORN_RELAY_OFF);
  IMUData packet={0,0};
  for(float angle : {-270.0f,-180.1f,180.1f,270.0f}) {
    packet.steeringAngle=angle; ++packet.sequence; clockMs+=50;
    onSteeringAngle(nullptr,(const uint8_t*)&packet,sizeof(packet)); loop();
    assert(steeringFeedback.angle==angle && steering.feedbackAvailable);
    const auto status=last(0x100);
    assert(status.len==8 && steeringAngleFrameValid(status.data) && status.data[3]==1);
    assert(steeringAngleTenths(status.data)==lroundf(angle*10));
  }
  // Exact new user sender ABI, including its packed bool hornPressed.
  struct __attribute__((packed)) SenderPacket { float steeringAngle; uint32_t sequence; bool hornPressed; };
  static_assert(sizeof(SenderPacket)==9);
  for(bool pressed : {false,true,false}) {
    SenderPacket sent={-269.5f,++packet.sequence,pressed}; clockMs+=50;
    onSteeringAngle(nullptr,(const uint8_t*)&sent,sizeof(sent)); loop();
    assert(pins[13]==(pressed?HORN_RELAY_ON:HORN_RELAY_OFF));
    assert(wirelessHornFrameValid(last(0x103).data) && last(0x103).data[0]==pressed);
    assert(steeringFeedback.angle==sent.steeringAngle && steeringFeedback.hornAvailable && steeringFeedback.hornPressed==pressed);
    assert(last(0x100).data[3]==1 && steeringAngleTenths(last(0x100).data)==-2695);
  }
  IMUDataWithPitch extended={25,++packet.sequence,1,-9,1};clockMs+=50;
  onSteeringAngle(nullptr,(const uint8_t*)&extended,sizeof(extended));loop();
  assert(steeringFeedback.pitchAvailable && steeringFeedback.pitch==-9);
  assert(last(WHEEL_PITCH_ID).data[3]==1 && steeringAngleTenths(last(WHEEL_PITCH_ID).data)==-90);
  assert(steeringFeedback.invalid==0);
  puts("IMU sender compatibility passed: old 8-byte and new 9-byte packed horn callback, channel 1, signed +/-270 degree CAN telemetry");
  // Exercise serial controls through the actual live loop and CAN handlers.
  inputAdc=795;Serial.input.push_back('a');clockMs+=50;loop();
  assert(serialSteeringActive && pins[32]==60 && pins[33]==0);
  assert(last(0x301).data[0]==uint8_t(795) && last(0x301).data[1]==uint8_t(795>>8));
  assert(last(0x111).data[0]==1 && last(0x111).data[1]==60);
  Frame motion{};motion.id=0x110;motion.len=8;motion.data[0]=2;motion.data[1]=100;motion.data[2]=1;
  steeringWrite32(motion.data+4,steering.token);incoming.push_back(motion);loop();
  assert(serialSteeringActive && pins[32]==60 && pins[33]==0); // Serial owns the motor.
  push(0x110,{0,0,0,0,0,0,0,0});loop();
  assert(!serialSteeringActive && !pins[32] && !pins[33]); // CAN STOP remains effective.
  incoming.push_back(motion);loop();assert(!pins[32] && !pins[33]); // Old CAN token cannot resume.
  Serial.input.push_back('d');loop();assert(serialSteeringActive && pins[33]==60);
  canTxWorks=false;clockMs+=50;loop();assert(!serialSteeringActive && !pins[32] && !pins[33]);
  canTxWorks=true;clockMs+=1000;loop();assert(canReady && !pins[32] && !pins[33]);
  puts("Live serial/CAN integration passed: pedal frames and output telemetry continue, serial ownership, CAN STOP, stale-token rejection and TX-fault stop");
  for(inputAdc=0;inputAdc<=4095;++inputAdc){
    clockMs+=50;canRecovery.received(clockMs);loop();const auto frame=last(0x301);
    assert(frame.len==2 && frame.data[0]==uint8_t(inputAdc) && frame.data[1]==uint8_t(inputAdc>>8));
  }
  inputAdc=869;clockMs+=50;loop();
  const auto idle=last(0x301);assert(idle.data[0]==0x65 && idle.data[1]==3);
  const auto successful=txOK,failed=txFailed;
  canTxWorks=false;clockMs+=50;loop();assert(txFailed>failed && txOK==successful);
  canTxWorks=true;clockMs+=1000;loop();assert(lastSendOK && txOK>successful);
  // Boundary at 1.0 V: ADC 1240 may claim, 1241 must be rejected.
  for(int raw : {930,992,993,1100,1240,1241}) {
    driveGate=RemoteDriveGate{};inputAdc=raw;
    Frame probe{};probe.id=0x120;probe.len=8;probe.data[0]=1;probe.data[1]=2;
    probe.data[2]=98;probe.data[6]=1;incoming.push_back(probe);clockMs+=50;loop();
    assert((driveGate.phase==1)==(raw<=1240));
  }
  driveGate=RemoteDriveGate{};inputAdc=0;clockMs+=50;loop();
  uint8_t claim[8]={1,2,99,0,0,0,1,0};
  Frame f{};f.id=0x120;f.len=8;memcpy(f.data,claim,8);incoming.push_back(f);loop();assert(driveGate.phase==1);
  const auto sent=count(0x301);inputAdc=4095;
  for(int n=2;n<=5;++n){clockMs+=50;f.data[6]=n;incoming.push_back(f);loop();assert(driveGate.phase==1 && count(0x301)==sent);}
  clockMs+=350;loop();assert(driveGate.phase==2 && last(0x301).data[0]==0xE0 && last(0x301).data[1]==3);
  puts("Node 1 raw CAN tests passed: all 4096 ADC values unchanged, little-endian, physical raw baseline retained; locked sender emits 0.8 V idle");
  canInitWorks=false;initializeCAN();const auto attempts=canInitCalls;
  assert(!canReady);clockMs=lastCanAttempt+999;loop();assert(canInitCalls==attempts);
  canInitWorks=true;clockMs=lastCanAttempt+1000;loop();
  assert(canReady && canInitCalls==attempts+1 && last(0x111).len==8);
  puts("Node 1 recovers a failed CAN startup and publishes steering status without rebooting");
  driveGate.phase=1;driveGate.lastAt=clockMs;
  websiteHorn.held=true;websiteHorn.lastCommandAt=clockMs;
  serviceHornRelay();assert(hornRelayOn);
  canErrorFlags=0x20;clockMs+=100;serviceCanHealth();
  assert(!canReady && driveGate.phase==2 && !websiteHorn.held && !hornRelayOn && !pins[32] && !pins[33]);
  canErrorFlags=0;clockMs=canRecovery.faultAt+1000;loop();
  assert(canReady && driveGate.phase==2 && !websiteHorn.held);
  puts("Node 1 runtime CAN recovery passed: bus-off stops website horn and motion, aborts TX, resets controller and keeps old ownership locked");
}
'''
node3 = r'''
int main(){
  std::fill(pins,pins+40,HIGH);setup();loop();
  assert(last(0x300).len==1 && last(0x300).data[0]==0);
  clockMs=10;pins[4]=LOW;loop();clockMs=34;loop();assert(last(0x300).data[0]==0);
  clockMs=35;loop();assert(last(0x300).data[0]==1);
  clockMs=40;for(auto pin:switchPins)pins[pin]=LOW;loop();clockMs=65;loop();
  assert(last(0x300).data[0]==63);
  auto sent=count(0x300);clockMs=1064;loop();assert(count(0x300)==sent);
  clockMs=1065;loop();assert(count(0x300)==sent+1);
  const auto status=last(0x122);assert(status.len==8 && driveStatusValid(status.data,63));
  assert(status.data[1]==63 && status.data[2]==0);
  uint8_t claim[8]={1,0,0,0,0,0,1,0};driveWrite32(claim+2,111);
  Frame f{};f.id=0x120;f.len=8;memcpy(f.data,claim,8);incoming.push_back(f);
  clockMs=1070;sent=count(0x300);loop();assert(driveGate.phase==1 && count(0x300)==sent);
  clockMs=1420;loop();assert(driveGate.phase==2);
  assert(last(0x300).data[0]==(2|8|16)); // Expired remote ownership holds direction.
  incoming.push_back(f);loop();assert(driveGate.phase==2); // Expired session cannot resume.
  f.data[0]=2;incoming.push_back(f);loop();assert(driveGate.phase==2); // Physical reverse mismatch.
  clockMs=1430;pins[33]=HIGH;loop();clockMs=1455;incoming.push_back(f);loop();
  assert(driveGate.phase==0 && last(0x300).data[0]==31);
  // Literal wiring from the supplied working sketch; test every combination.
  const int expectedPins[]={4,5,13,14,26,33};
  assert(SWITCH_COUNT==6 && SWITCH_VALID_MASK==0x3f && SWITCH_REVERSE==0x20);
  assert(pinModes[32]==0); // Removed SPST2 must never be configured as an input.
  for(int bit=0;bit<6;++bit)assert(switchPins[bit]==expectedPins[bit] && pinModes[expectedPins[bit]]==INPUT_PULLUP);
  for(unsigned mask=0;mask<64;++mask) {
    clockMs+=30;
    canRecovery.received(clockMs); // Other active nodes still send periodic frames.
    for(int bit=0;bit<6;++bit)pins[expectedPins[bit]]=(mask&(1U<<bit))?LOW:HIGH;
    loop();clockMs+=25;loop();
    const auto actual=last(0x300);assert(actual.len==1 && actual.data[0]==mask);
    assert(buildPayload()==mask);
  }
  // Chattering contacts must not publish a transient reverse or switch state.
  for(int bit=0;bit<6;++bit) {
    clockMs+=10;pins[expectedPins[bit]]=HIGH;loop();
    clockMs+=10;pins[expectedPins[bit]]=LOW;loop();
    clockMs+=25;loop();assert(buildPayload()==63);
  }
  // A failed TX aborts queued frames and backs off before controller recovery.
  const auto calls=canInitCalls;const auto failed=txFailed;sent=count(0x300);
  canTxWorks=false;sendPending=true;++clockMs;loop();
  assert(sendPending && txFailed>failed && count(0x300)==sent && canInitCalls==calls && !canReady && canAbortCalls);
  canTxWorks=true;clockMs+=1000;loop();assert(canReady);
  ++clockMs;loop();assert(!sendPending && count(0x300)==sent+1);
  canInitWorks=false;initializeCAN();assert(!canReady);
  const auto attempts=canInitCalls;clockMs+=999;loop();assert(canInitCalls==attempts);
  canInitWorks=true;++clockMs;loop();assert(canReady && canInitCalls==attempts+1);
  canModeWorks=false;initializeCAN();assert(!canReady);
  canModeWorks=true;clockMs+=1000;loop();assert(canReady);
  // Bus-off after a successful boot must stop ownership, abort old frames,
  // and recover without rebooting the ESP32 or resuming the old session.
  driveGate.phase=1;driveGate.lastAt=clockMs;
  canErrorFlags=0x20;clockMs+=100;loop();
  assert(!canReady && driveGate.phase==2 && canRecovery.reason==2);
  const auto resets=canInitCalls;canErrorFlags=0;clockMs=canRecovery.faultAt+999;loop();assert(canInitCalls==resets);
  clockMs=canRecovery.faultAt+1000;loop();assert(canReady && canInitCalls==resets+1 && driveGate.phase==2);
  puts("Node 3 runtime CAN recovery passed: bus-off, queued-TX abort, backoff, reset and ownership remains locked");
  puts("Node 3 sender tests passed: exact GPIOs, all 64 switch masks, bounce, DLC1, 1 s refresh, TX retry, init/mode recovery, gate telemetry, ownership expiry and reverse handoff");
}
'''
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    for name, io, scenario, polarity in [('node1_bcm', node1_io, node1,0), ('node1_bcm',node1_io,node1,1), ('node3_ccm', '', node3,0)]:
        source = (root / name / f'{name}.ino').read_text()
        source = re.sub(r'^#include (?:<.*>|"CanCommon.h")\s*$', '', source, flags=re.M)
        cpp = tmp / f'{name}.cpp'
        cpp.write_text(common + io + source + scenario)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-DNODE1_HORN_ACTIVE_LOW={polarity}', '-I', str(root / name), str(cpp), '-o', str(tmp / name)], check=True)
        subprocess.run([str(tmp / name)], check=True)

    # Compare every ADC value across the actual firmware and browser functions.
    cpp = tmp / 'scaling.cpp'
    cpp.write_text('#include <cstdio>\n#include "ThrottleCalibration.h"\nint main(){for(int adc=0;adc<4096;++adc)printf("%u\\n",throttlePercent(adc));}')
    subprocess.run(['g++', '-std=c++17', '-I', str(root / 'node2_pcm'), str(cpp), '-o', str(tmp / 'scaling')], check=True)
    values = [int(value) for value in subprocess.check_output([str(tmp / 'scaling')], text=True).splitlines()]
    script = """
import {throttlePercent} from './dashboard/src/diagnostics.js';
let text='';for await(const part of process.stdin)text+=part;
const values=JSON.parse(text),differences=values.flatMap((firmware,adc)=>firmware===throttlePercent(adc)?[]:[adc]);
if(differences.length)throw new Error(`Firmware/browser throttle mismatch at ADC ${differences}`);
console.log(`Throttle transfer scaling passed: all ${values.length} ADC values match firmware and dashboard`);
"""
    subprocess.run(['node', '--input-type=module', '-e', script], cwd=root, input=json.dumps(values), text=True, check=True)
