"""Run the actual Node 1/3/4 sketches with mocked pins, CAN, radio, GPS and IMU."""
from pathlib import Path
import json
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
common = r'''
#include <algorithm>
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
void pinMode(int pin,int mode){pinModes[pin]=mode;}
int digitalRead(int pin){return pins[pin];}
template<class T> T constrain(T value,T low,T high){return std::max(low,std::min(high,value));}
struct SerialMock {
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
unsigned canInitCalls=0;
struct MCP_CAN {
  explicit MCP_CAN(int cs){assert(cs==25);}
  int begin(int mode,int speed,int crystal){
    assert(mode==MCP_ANY && speed==CAN_500KBPS && crystal==MCP_8MHZ);
    ++canInitCalls;return canInitWorks?CAN_OK:2;
  }
  int setMode(int mode){assert(mode==MCP_NORMAL);return canModeWorks?CAN_OK:2;}
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
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*){}
void portEXIT_CRITICAL(portMUX_TYPE*){}
constexpr int WIFI_STA=1,WIFI_SECOND_CHAN_NONE=0,ESP_OK=0;
struct esp_now_recv_info_t {};
struct WifiMock {
  bool mode(int){return true;}void setSleep(bool){}void setAutoReconnect(bool){}
  const char *macAddress(){return "00:4B:12:34:C8:74";}
} WiFi;
int esp_wifi_set_channel(uint8_t,int){return ESP_OK;}
int esp_now_init(){return ESP_OK;}int esp_now_deinit(){return ESP_OK;}
int esp_now_register_recv_cb(void (*)(const esp_now_recv_info_t*,const uint8_t*,int)){return ESP_OK;}
uint32_t esp_random(){return 9;}
int inputAdc=0;
void analogReadResolution(int bits){assert(bits==12);}
int analogRead(int pin){assert(pin==34);return inputAdc;}
void analogWrite(int pin,int value){
  assert((pin==32 || pin==33) && (value==0 || value==60));
  pins[pin]=value;assert(!(pins[32] && pins[33]));
}
'''
node1 = r'''
int main(){
  setup();assert(canReady && steeringRadioReady);
  for(inputAdc=0;inputAdc<=4095;++inputAdc){
    clockMs+=50;loop();const auto frame=last(0x301);
    assert(frame.len==2 && frame.data[0]==uint8_t(inputAdc) && frame.data[1]==uint8_t(inputAdc>>8));
  }
  inputAdc=869;clockMs+=50;loop();
  const auto idle=last(0x301);assert(idle.data[0]==0x65 && idle.data[1]==3);
  const auto successful=txOK,failed=txFailed;
  canTxWorks=false;clockMs+=50;loop();assert(txFailed>failed && txOK==successful);
  canTxWorks=true;clockMs+=50;loop();assert(lastSendOK && txOK>successful);
  puts("Node 1 raw CAN tests passed: all 4096 ADC values unchanged, little-endian, 0.7 V baseline retained");
}
'''
node3 = r'''
int main(){
  std::fill(pins,pins+40,HIGH);setup();loop();
  assert(last(0x300).len==1 && last(0x300).data[0]==0);
  clockMs=10;pins[4]=LOW;loop();clockMs=34;loop();assert(last(0x300).data[0]==0);
  clockMs=35;loop();assert(last(0x300).data[0]==1);
  clockMs=40;for(auto pin:switchPins)pins[pin]=LOW;loop();clockMs=65;loop();
  assert(last(0x300).data[0]==127);
  auto sent=count(0x300);clockMs=1064;loop();assert(count(0x300)==sent);
  clockMs=1065;loop();assert(count(0x300)==sent+1);
  const auto status=last(0x122);assert(status.len==8 && driveStatusValid(status.data,127));
  assert(status.data[1]==127 && status.data[2]==0);
  uint8_t claim[8]={1,0,0,0,0,0,1,0};driveWrite32(claim+2,111);
  Frame f{};f.id=0x120;f.len=8;memcpy(f.data,claim,8);incoming.push_back(f);
  clockMs=1070;sent=count(0x300);loop();assert(driveGate.phase==1 && count(0x300)==sent);
  clockMs=1420;loop();assert(driveGate.phase==2);
  assert(last(0x300).data[0]==(2|8|16|32)); // Expired remote ownership holds direction.
  incoming.push_back(f);loop();assert(driveGate.phase==2); // Expired session cannot resume.
  f.data[0]=2;incoming.push_back(f);loop();assert(driveGate.phase==2); // Physical reverse mismatch.
  clockMs=1430;pins[13]=HIGH;loop();clockMs=1455;incoming.push_back(f);loop();
  assert(driveGate.phase==0 && last(0x300).data[0]==123);
  // Literal wiring from the supplied working sketch; test every combination.
  const int expectedPins[]={4,5,13,14,26,33,32};
  for(int bit=0;bit<7;++bit)assert(switchPins[bit]==expectedPins[bit] && pinModes[expectedPins[bit]]==INPUT_PULLUP);
  for(unsigned mask=0;mask<128;++mask) {
    clockMs+=30;
    for(int bit=0;bit<7;++bit)pins[expectedPins[bit]]=(mask&(1U<<bit))?LOW:HIGH;
    loop();clockMs+=25;loop();
    const auto actual=last(0x300);assert(actual.len==1 && actual.data[0]==mask);
    assert(buildPayload()==mask);
  }
  // Chattering contacts must not publish a transient reverse or switch state.
  for(int bit=0;bit<7;++bit) {
    clockMs+=10;pins[expectedPins[bit]]=HIGH;loop();
    clockMs+=10;pins[expectedPins[bit]]=LOW;loop();
    clockMs+=25;loop();assert(buildPayload()==127);
  }
  // A failed TX remains pending; it must not reset the CAN controller.
  const auto calls=canInitCalls,failed=txFailed;sent=count(0x300);
  canTxWorks=false;sendPending=true;++clockMs;loop();
  assert(sendPending && txFailed>failed && count(0x300)==sent && canInitCalls==calls);
  canTxWorks=true;++clockMs;loop();assert(!sendPending && count(0x300)==sent+1);
  canInitWorks=false;initializeCAN();assert(!canReady);
  const auto attempts=canInitCalls;clockMs+=999;loop();assert(canInitCalls==attempts);
  canInitWorks=true;++clockMs;loop();assert(canReady && canInitCalls==attempts+1);
  canModeWorks=false;initializeCAN();assert(!canReady);
  canModeWorks=true;clockMs+=1000;loop();assert(canReady);
  puts("Node 3 sender tests passed: exact GPIOs, all 128 switch masks, bounce, DLC1, 1 s refresh, TX retry, init/mode recovery, gate telemetry, ownership expiry and reverse handoff");
}
'''
node4_io = r'''
bool canReady=false,imuPresent=true,imuSampleOK=true;
uint8_t canTxCount=0,canRxCount=0;
bool canBegin(){canReady=true;return true;}
void canPrintStatus(const char*){}
template<size_t N>bool canSend(uint32_t id,const uint8_t(&bytes)[N]){
  Frame f{};f.id=id;f.len=N;memcpy(f.data,bytes,N);outgoing.push_back(f);++canTxCount;return true;
}
bool canRead(uint32_t &id,uint8_t &len,uint8_t(&bytes)[8]){
  if(incoming.empty())return false;
  auto f=incoming.front();incoming.pop_front();id=f.id;len=f.len;memcpy(bytes,f.data,8);++canRxCount;return true;
}
void canHeartbeat(uint8_t node,uint8_t status,uint8_t error){
  uint8_t d[8]={node,status,(uint8_t)(millis()/1000),error,canTxCount,canRxCount,0,0};canSend(0x600,d);
}
struct HardwareSerial {
  explicit HardwareSerial(int){}
  void setRxBufferSize(unsigned){}
  void begin(int,int,int,int){}
  int available(){return 0;}
  int read(){return 0;}
};
struct TinyGPSPlus {
  struct Location {
    bool valid=true;uint32_t ageMs=0;double latitude=10.99999,longitude=79.99999;
    bool isValid(){return valid;}uint32_t age(){return ageMs;}
    double lat(){return latitude;}double lng(){return longitude;}
  } location;
  struct Satellites {bool isValid(){return true;}uint32_t value(){return 14;}} satellites;
  void encode(int){}
};
struct sensors_event_t {struct {float x=0,y=0,z=0;} acceleration;};
constexpr int MPU6050_RANGE_8_G=0,MPU6050_BAND_21_HZ=0;
struct Adafruit_MPU6050 {
  bool begin(uint8_t){return imuPresent;}
  void setAccelerometerRange(int){}void setFilterBandwidth(int){}
  bool getEvent(sensors_event_t *a,sensors_event_t*,sensors_event_t*){
    a->acceleration.x=-1;a->acceleration.z=1;return imuSampleOK;
  }
};
struct WireMock {void begin(int,int){}void setTimeOut(int){}} Wire;
'''
node4 = r'''
int main(){
  setup();pins[34]=HIGH;clockMs=1000;loop();
  auto p=last(0x400);assert(p.len==8 && p.data[6]==1 && p.data[7]==12);
  assert(p.data[0]==11 && p.data[1]==0 && p.data[2]==0);
  assert(p.data[3]==80 && p.data[4]==0 && p.data[5]==0); // Degree-boundary carry.
  assert(last(0x401).data[0]==1 && last(0x401).data[2]==0);
  clockMs=2000;loop();assert(last(0x401).data[1]==0 && last(0x401).data[2]==1);
  push(0x300,{8});clockMs=2001;loop();clockMs=2050;loop();
  auto h=last(0x500);assert(h.data[0]==1 && h.data[1]==135 && h.data[2]==0 && h.data[3]==1);
  clockMs=5001;loop();assert(last(0x500).data[0]==1); // One-second switch refresh is allowed.
  clockMs=5501;loop();assert(last(0x500).data[0]==0);
  push(0x300,{0x88});clockMs=5502;loop();assert(!switchSeen);
  gps.location.latitude=-10;clockMs=6501;loop();assert(!last(0x400).data[6]);
  gps.location.latitude=90;gps.location.longitude=180;clockMs=7501;loop();
  p=last(0x400);assert(p.data[0]==90 && p.data[3]==180 && p.data[6]==1);
  gps.location.ageMs=3000;clockMs=8501;imuSampleOK=false;loop();
  assert(!last(0x400).data[6] && !last(0x500).data[3]);
  const auto beat=last(0x600);assert(beat.data[0]==4 && beat.data[1]==0 && beat.data[3]==2);
  for(int n=0;n<20;++n) { push(0x777,{0}); }
  loop();assert(incoming.size()==12);
  puts("Node 4 sender tests passed: GPS degree carry/domain/age, ignition BE16, pitch encoding, switch expiry, invalid inputs, heartbeat and bounded RX");
}
'''

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    for name, io, scenario in [('node1_bcm', node1_io, node1), ('node3_ccm', '', node3), ('node4_tcu', node4_io, node4)]:
        source = (root / name / f'{name}.ino').read_text()
        source = re.sub(r'^#include (?:<.*>|"CanCommon.h")\s*$', '', source, flags=re.M)
        cpp = tmp / f'{name}.cpp'
        cpp.write_text(common + io + source + scenario)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / name), str(cpp), '-o', str(tmp / name)], check=True)
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
