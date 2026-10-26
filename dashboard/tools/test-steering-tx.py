"""Exercise the actual transmitter loop, radio payload and Node 1 decoder."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'steering_tx/steering_tx.ino').read_text()
source = re.sub(r'^#include [<"].*[>"]\s*$', '', source, flags=re.M)
shim = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <algorithm>
#include "SteeringFeedback.h"
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#ifndef ESP_IDF_VERSION
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,0)
#endif
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*){}
void portEXIT_CRITICAL(portMUX_TYPE*){}
constexpr int INPUT_PULLUP=2,LOW=0,HIGH=1,WIFI_STA=1,WIFI_IF_STA=0,WIFI_SECOND_CHAN_NONE=0;
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0;
enum esp_now_send_status_t {ESP_NOW_SEND_SUCCESS,ESP_NOW_SEND_FAIL};
struct esp_now_send_info_t {};
uint32_t clockUs=0;
uint32_t micros(){return clockUs;}
uint32_t millis(){return clockUs/1000;}
void delay(unsigned ms){clockUs+=ms*1000;}
int rawPin=HIGH,buttonMode=-1;
int digitalRead(int pin){assert(pin==27);return rawPin;}
void pinMode(int pin,int mode){assert(pin==27);buttonMode=mode;}
template<class T> T constrain(T v,T lo,T hi){return std::max(lo,std::min(hi,v));}
struct SerialMock {
 void begin(int baud){assert(baud==115200);}
 template<class... T> void printf(const char*,T...){}
 template<class T> void print(T){}
 template<class T> void println(T){}
} Serial;
struct WifiMock {
 bool mode(int v){assert(v==WIFI_STA);return true;}
 void setSleep(bool v){assert(!v);}
 void setAutoReconnect(bool v){assert(!v);}
 const char *macAddress(){return "sender";}
} WiFi;
int channel=0;
esp_err_t esp_wifi_set_channel(int c,int second){assert(second==0);channel=c;return ESP_OK;}
esp_err_t esp_now_init(){return ESP_OK;}
struct esp_now_peer_info_t {uint8_t peer_addr[6];int channel;int ifidx;bool encrypt;};
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *p){
 const uint8_t mac[6]={0x68,0x09,0x47,0xDF,0x8E,0xE8};
 assert(!memcmp(mac,p->peer_addr,6) && p->channel==1 && p->ifidx==WIFI_IF_STA && !p->encrypt);return ESP_OK;
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,5,0)
using SendCallback=void (*)(const esp_now_send_info_t*,esp_now_send_status_t);
#else
using SendCallback=void (*)(const uint8_t*,esp_now_send_status_t);
#endif
SendCallback callback=nullptr;
esp_err_t esp_now_register_send_cb(SendCallback cb){callback=cb;return ESP_OK;}
bool queueWorks=true;
std::deque<std::vector<uint8_t>> packets;
esp_err_t esp_now_send(const uint8_t*,const uint8_t *p,size_t len){
 if(!queueWorks)return 7;
 packets.emplace_back(p,p+len);return ESP_OK;
}
bool i2cWorks=true;
int16_t gyroZ=0;
struct WireMock {
 unsigned readIndex=0;
 void begin(int sda,int scl){assert(sda==21 && scl==22);}
 void setTimeOut(int ms){assert(ms==10);}
 void beginTransmission(int addr){assert(addr==0x68);}
 void write(uint8_t){}
 int endTransmission(bool=true){return i2cWorks?0:1;}
 int requestFrom(int addr,uint8_t len,uint8_t stop){assert(addr==0x68 && len==2 && stop);readIndex=0;return i2cWorks?2:0;}
 uint8_t read(){return readIndex++==0?((uint16_t)gyroZ>>8):(uint8_t)gyroZ;}
} Wire;
'''
scenario = r'''
SteeringFeedback receiver;
uint32_t lastSeq=0;bool received=false;
void deliver(bool success=true){
 if(packets.empty())return;
 const auto p=packets.front();packets.pop_front();
 callback(nullptr,success?ESP_NOW_SEND_SUCCESS:ESP_NOW_SEND_FAIL);
 if(!success)return;
 assert(p.size()==9 || p.size()==10);
 assert(receiver.accept(p.data(),p.size(),millis()));
 if(received)assert(receiver.sequence>lastSeq);
 received=true;lastSeq=receiver.sequence;
}
void run(unsigned ms){for(unsigned n=0;n<ms/2;++n){deliver();loop();}}
int main(int argc,char**){
 i2cWorks=argc==1;setup();deliver();
 assert(buttonMode==INPUT_PULLUP && channel==1 && radioReady);
 run(30);assert(!hornPressed && !receiver.hornActive(millis()));
 rawPin=LOW;run(8);rawPin=HIGH;run(20);assert(!hornPressed); // Contact bounce.
 rawPin=LOW;run(40);assert(hornPressed && receiver.hornActive(millis()));
 assert(receiver.fresh(millis())==i2cWorks);
 if(i2cWorks){
   assert(packets.empty() || packets.front().size()==9);
   gyroZ=1310;run(100);assert(receiver.angle>0);
 }
 i2cWorks=false;rawPin=HIGH;run(40);
 assert(!hornPressed && !receiver.hornActive(millis()) && !receiver.fresh(millis()));
 rawPin=LOW;run(7000);assert(hornPressed && receiver.hornActive(millis()) && !receiver.fresh(millis()));
 // A radio delivery failure is observable and does not prevent the next retry.
 delay(10);loop();assert(!packets.empty());deliver(false);run(30);assert(deliveryFailed && receiver.hornActive(millis()));
 queueWorks=false;deliver();run(30);assert(queueFailures && !sendPending);
 rawPin=HIGH;queueWorks=true;run(40);assert(!receiver.hornActive(millis()));
 // With no delivery callback, the sender never queues obsolete button states.
 deliver();delay(10);loop();const auto queued=packets.size();assert(queued==1);delay(20);loop();assert(packets.size()==queued);
 delay(350);loop();assert(packets.size()==queued && !receiver.hornActive(millis()));
 puts("Actual steering TX passed: GPIO27 pull-up/bounce, 9-byte ABI, 10-byte fault fallback, channel/MAC, press/release, >7s hold, gyro/startup failure, Node1 decode, radio ACK/failure, queue retry and no stale backlog");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    cpp = tmp / 'tx.cpp'
    cpp.write_text(shim + source + scenario)
    for version in (50500, 50400):
        binary = tmp / f'tx-{version}'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-DESP_IDF_VERSION={version}', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([str(binary), 'missing-imu'], check=True)
