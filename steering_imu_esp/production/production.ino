/*
Steering IMU ESP32 - PRODUCTION. Serial diagnostics: 115200 baud.
Connections:
  MPU6050 SDA -> GPIO21, SCL -> GPIO22, VCC -> compatible 3.3 V, GND.
  No CAN transceiver is connected to this ESP32; it sends ESP-NOW to
  Node 1 MAC 30:76:F5:94:6A:60 on WiFi channel 1.
  Packed payload is uint32_t sequence followed by float steeringAngle.
Verification:
  Keep steering still for startup offset calibration. Serial should print
  transmitter ready, TX count/angle every 200 ms, and delivery OK.
  Check Node 1 production and Node 5 for valid steering CAN frame 0x100;
  the isolated Node 1 test sketch deliberately receives no ESP-NOW data.
  If Node 1 MAC changes, update receiverMAC in THIS production sketch.
  This gyro-integrated angle is relative to startup and can drift; it is
  telemetry only, not a calibrated absolute steering position sensor.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>

// Node 1 MAC from the working ESP-NOW pair supplied by the user.
uint8_t receiverMAC[6] = {0x30, 0x76, 0xF5, 0x94, 0x6A, 0x60};
constexpr uint8_t WIFI_CHANNEL = 1;
constexpr uint32_t SEND_INTERVAL_MS = 10;
#pragma pack(push,1)
struct SteeringData { uint32_t sequence; float steeringAngle; };
#pragma pack(pop)
Adafruit_MPU6050 mpu;
float gyroOffsetZ = 0, steeringAngle = 0;
uint32_t sequenceNumber = 0, previousMicros = 0, lastSend = 0, lastPrint = 0;
volatile bool sendDone = false, sendSuccess = false;
void onSent(const wifi_tx_info_t *, esp_now_send_status_t status) {
  sendSuccess = status == ESP_NOW_SEND_SUCCESS;
  sendDone = true;
}
void setup() {
  Serial.begin(115200); delay(500);
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); delay(200);
  uint8_t ownMAC[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, ownMAC) == ESP_OK)
    Serial.printf("Steering IMU MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",ownMAC[0],ownMAC[1],ownMAC[2],ownMAC[3],ownMAC[4],ownMAC[5]);
  if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("WiFi channel setup failed"); while(true) delay(1000);
  }
  Wire.begin(21,22);
  Wire.setClock(400000);
  if (!mpu.begin()) { Serial.println("MPU6050 not detected on GPIO21/22"); while(true) delay(1000); }
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("Keep steering wheel still for gyro calibration"); delay(1000);
  double sum=0;
  for(int n=0;n<300;++n){sensors_event_t a,g,t;mpu.getEvent(&a,&g,&t);sum+=g.gyro.z;delay(3);}
  gyroOffsetZ=sum/300.0;
  if (esp_now_init()!=ESP_OK) { Serial.println("ESP-NOW init failed"); while(true) delay(1000); }
  esp_now_register_send_cb(onSent);
  esp_now_peer_info_t peer={}; memcpy(peer.peer_addr,receiverMAC,6);
  peer.channel=WIFI_CHANNEL; peer.ifidx=WIFI_IF_STA; peer.encrypt=false;
  if (esp_now_add_peer(&peer)!=ESP_OK) { Serial.println("ESP-NOW peer add failed"); while(true) delay(1000); }
  previousMicros=micros();
  lastSend=millis();
  Serial.println("Steering IMU sender ready on channel 1");
}
void loop() {
  sensors_event_t a,g,t; mpu.getEvent(&a,&g,&t);
  uint32_t currentMicros=micros();
  float dt=(uint32_t)(currentMicros-previousMicros)/1000000.0f;
  previousMicros=currentMicros;
  if (dt>0 && dt<0.1f) steeringAngle+=(g.gyro.z-gyroOffsetZ)*180.0f/PI*dt;
  uint32_t now=millis();
  if (now-lastSend>=SEND_INTERVAL_MS) {
    lastSend=now;
    SteeringData packet={++sequenceNumber,steeringAngle};
    esp_err_t result=esp_now_send(receiverMAC,(const uint8_t*)&packet,sizeof(packet));
    if (result!=ESP_OK) Serial.printf("ESP-NOW queue error: %d\n",result);
  }
  if (now-lastPrint>=200) {
    lastPrint=now;
    Serial.printf("TX #%lu | Angle: %.2f deg | Last delivery: %s\n",
      (unsigned long)sequenceNumber,steeringAngle,sendDone?(sendSuccess?"OK":"FAIL"):"WAIT");
    sendDone=false;
  }
}
