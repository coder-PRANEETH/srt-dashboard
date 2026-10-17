/*
Node 5 DASH / WiFi host - BENCH TEST. Serial Monitor: 115200 baud.
Connections:
  MCP2515 wiring listed below is the only local CAN connection. No speed
  sensor or potentiometer is attached to Node 5. WiFi uses the ESP32 radio.
  Use classic ESP32 Dev Module and No OTA (2 MB APP / 2 MB SPIFFS) layout.
  First upload production/data as LittleFS from the production sketch
  folder after packing the site. Then flash this test without erasing the
  filesystem; the test checks for /index.html.gz in that partition.
Verification:
  Serial should report CAN loopback READY and LittleFS MOUNTED; Packed
  index is found when the compressed filesystem was already uploaded.
  Join WiFi SSID srt-dash-test with password 12345678 and open
  http://192.168.4.1/; it reports CAN loopback sent/pass, LittleFS and
  WiFi clients. Both loopback counts should grow without another node.
  Physical CANH/CANL bus wiring is checked later with production nodes.

Local CAN controller connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT; MCP2515 GND -> ESP32 GND.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  Internal MCP2515 loopback needs no CANH/CANL bus, termination or peer.
  It tests SPI/controller TX/RX, not the external transceiver or bus.
*/

#include "CanCommon.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>

constexpr uint32_t LOCAL_CAN_ID = 0x7A5;
WebServer server(80);
bool fsReady = false;
uint32_t lastReport = 0, canSent = 0, canPassed = 0;
uint8_t canSequence = 0;

void sendStatus() {
  String body = "Node 5 isolated test\nCAN controller=";
  body += canReady ? "ready" : "failed";
  body += "\nCAN local loopback sent=" + String(canSent);
  body += " pass=" + String(canPassed);
  body += "\nLittleFS=";
  body += fsReady ? "mounted" : "failed";
  body += "\nPacked index=";
  body += fsReady && LittleFS.exists("/index.html.gz") ? "found" : "missing";
  body += "\nWiFi clients=" + String(WiFi.softAPgetStationNum()) + "\n";
  server.send(200, "text/plain", body);
}

void setup() {
  Serial.begin(115200);
  fsReady = LittleFS.begin(false);
  const bool canStarted = canBegin(true);
  WiFi.mode(WIFI_AP);
  const bool apReady = WiFi.softAP("srt-dash-test", "12345678");
  server.on("/", sendStatus);
  server.begin();
  Serial.printf("Node 5 CAN local loopback %s | LittleFS %s | WiFi AP %s\n",
                canStarted ? "READY" : "FAILED",
                fsReady ? "MOUNTED" : "FAILED", apReady ? "READY" : "FAILED");
  Serial.printf("Packed index: %s | Test URL: http://%s/\n",
                fsReady && LittleFS.exists("/index.html.gz") ? "found" : "missing",
                WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();
  uint32_t id;
  uint8_t len, data[8];
  while (canRead(id, len, data)) {
    if (id == LOCAL_CAN_ID && len == 8 && data[0] == 5 && data[2] == 0xA5)
      ++canPassed;
  }
  if (millis() - lastReport >= 1000) {
    lastReport = millis();
    const uint8_t probe[8] = {5, canSequence++, 0xA5, 0, 0, 0, 0, 0};
    if (canSend(LOCAL_CAN_ID, probe)) ++canSent;
    Serial.printf("CAN local loopback sent=%lu pass=%lu | WiFi clients=%u\n",
                  (unsigned long)canSent, (unsigned long)canPassed,
                  WiFi.softAPgetStationNum());
  }
}
