/*
Node 5 DASH / WiFi host - PRODUCTION. Serial Monitor: 115200 baud.
Connections:
  MCP2515 wiring listed below links this host to the five-node CAN bus.
  No local speed sensor/potentiometer is connected. WiFi serves the site.
  Use classic ESP32 Dev Module with No OTA (2 MB APP / 2 MB SPIFFS).
  Upload this sketch AND compressed data/ as LittleFS; packing
  is done from dashboard with npm run build:esp. Do not rename .gz files.
Verification:
  Join SSID srt-dash, initial password 12345678, then visit
  http://192.168.4.1/ and /api/vehicle, /api/gps, /api/can-log.
  Dashboard should load from LittleFS; /api/can-log frame count and
  Node 1/3 activity should appear from 0x301/0x300 without heartbeats.
  /api/vehicle shows little-endian pedal ADC, ADC-scale percent and an
  estimated GPIO voltage. Node 2 is receive-only; its health is unknown.
  /api/gps shows a fix only after valid 0x400; /api/data speed needs two
  successive usable GPS fixes. Change the initial AP password in code.

Shared CAN connection (classic ESP32 + MCP2515):
  GPIO18 -> SCK, GPIO19 <- SO/MISO, GPIO23 -> SI/MOSI,
  GPIO25 -> CS, GPIO27 <- INT. Join ESP32, MCP2515 and vehicle grounds.
  Connect CANH/CANL to the same bus; fit 120 ohms at each physical end.
  Code uses 500 kbit/s and an 8 MHz MCP2515 crystal (CanCommon.h).
  Power the module per its rating and use 3.3 V-compatible SPI/INT logic.
  A successful CAN startup confirms SPI init; RX needs another CAN node.
*/

#include "CanCommon.h"
#include "ThrottleCalibration.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <math.h>

// Upload this sketch AND data/ with LittleFS (No OTA partition).
const char *AP_SSID = "srt-dash";
const char *AP_PASS = "12345678"; // Change before deployment.
WebServer server(80);
struct Sample { uint8_t bytes[8] = {}; uint8_t len = 0; uint32_t at = 0, received = 0, invalid = 0; bool seen = false; };
const uint16_t ids[] = {0x100,0x101,0x151,0x200,0x201,0x300,0x301,0x302,0x400,0x401,0x500,0x501};
Sample samples[sizeof(ids)/sizeof(ids[0])];
struct NodeState { uint8_t status = 0, error = 0, tx = 0, rx = 0; uint32_t at = 0; bool seen = false; } nodes[6];
struct LogFrame { uint32_t seq, ms, id; uint8_t len, bytes[8]; } logFrames[64];
uint8_t logNext = 0, logCount = 0;
uint32_t frameCount = 0, lastBeat = 0, malformedFrames = 0, unknownFrames = 0;
bool fsReady = false;
float gpsSpeedKmh = NAN, prevLat = NAN, prevLon = NAN;
uint32_t prevGpsAt = 0;

Sample *sampleFor(uint32_t id) {
  for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); ++i) if (ids[i] == id) return &samples[i];
  return nullptr;
}
bool fresh(const Sample *s, uint32_t timeout) { return s && s->seen && millis() - s->at < timeout; }
uint16_t le16(const uint8_t *b) { return b[0] | ((uint16_t)b[1] << 8); }
uint16_t be16(const uint8_t *b) { return ((uint16_t)b[0] << 8) | b[1]; }
float degToRad(float deg) { return deg * 0.01745329252f; }
float distanceMeters(float a, float b, float c, float d) {
  float x = degToRad(c - a), y = degToRad(d - b);
  float h = sinf(x/2)*sinf(x/2) + cosf(degToRad(a))*cosf(degToRad(c))*sinf(y/2)*sinf(y/2);
  return 12742000.0f * asinf(fminf(1.0f, sqrtf(h)));
}
void readCan() {
  uint32_t id; uint8_t len, data[8];
  while (canRead(id, len, data)) {
    const uint32_t now = millis();
    ++frameCount;
    LogFrame &f = logFrames[logNext];
    f.seq = frameCount; f.ms = now; f.id = id; f.len = len; memcpy(f.bytes, data, 8);
    logNext = (logNext + 1) % 64; if (logCount < 64) ++logCount;
    if (id == 0x600 && len == 8 && data[0] >= 1 && data[0] <= 5) {
      NodeState &n = nodes[data[0]];
      n.seen = true; n.at = now; n.status = data[1]; n.error = data[3]; n.tx = data[4]; n.rx = data[5];
    }
    Sample *s = sampleFor(id);
    const uint8_t expectedLength = id == 0x300 ? 1 : id == 0x301 ? 2 : 8;
    const bool validGps = id != 0x400 || !data[6] ||
      (data[0] <= 90 && data[3] <= 180 && be16(data+1) < 6000 && be16(data+4) < 6000 &&
       (data[0] < 90 || be16(data+1) == 0) && (data[3] < 180 || be16(data+4) == 0));
    const bool validPitch = id != 0x500 || !data[3] || data[1] <= 180;
    const bool validPayload = len == expectedLength && (id != 0x301 || le16(data) <= 4095) && validGps && validPitch;
    if (s) { ++s->received; if (!validPayload) { ++s->invalid; ++malformedFrames; } }
    else if (id == 0x600) { if (len != 8 || data[0] < 1 || data[0] > 5) ++malformedFrames; }
    else ++unknownFrames;
    if (s && validPayload) {
      memcpy(s->bytes, data, 8); s->len = len; s->at = now; s->seen = true;
      if (id == 0x400) {
        if (data[6] && data[0] <= 90 && data[3] <= 180 && be16(data+1) < 6000 && be16(data+4) < 6000) {
          float lat = data[0] + be16(data+1)/6000.0f;
          float lon = data[3] + be16(data+4)/6000.0f;
          if (isfinite(prevLat) && now - prevGpsAt >= 500 && now - prevGpsAt < 10000) {
            float meters = distanceMeters(prevLat, prevLon, lat, lon);
            float speed = meters * 3600.0f / (now - prevGpsAt);
            gpsSpeedKmh = meters < 3.0f ? 0.0f : (speed <= 160.0f ? speed : NAN);
          }
          prevLat = lat; prevLon = lon; prevGpsAt = now;
        } else { gpsSpeedKmh = NAN; prevLat = prevLon = NAN; }
      }
    }
  }
}
void jsonHeader() { server.sendHeader("Cache-Control", "no-store"); server.sendHeader("Access-Control-Allow-Origin", "*"); }
void sendGps() {
  readCan();
  Sample *g = sampleFor(0x400);
  bool valid = fresh(g, 3000) && g->bytes[6] && g->bytes[0] <= 90 && g->bytes[3] <= 180 &&
               be16(g->bytes+1) < 6000 && be16(g->bytes+4) < 6000;
  String json = "{\"fix\":"; json += valid ? "true" : "false";
  if (valid) {
    json += ",\"lat\":" + String(g->bytes[0] + be16(g->bytes+1)/6000.0, 6);
    json += ",\"lon\":" + String(g->bytes[3] + be16(g->bytes+4)/6000.0, 6);
    json += ",\"sats\":" + String(g->bytes[7]);
    json += ",\"ageMs\":" + String(millis() - g->at);
    if (isfinite(gpsSpeedKmh)) json += ",\"speed\":" + String(gpsSpeedKmh, 1);
  }
  json += "}"; jsonHeader(); server.send(200, "application/json", json);
}
void sendSpeed() {
  readCan(); Sample *g = sampleFor(0x400);
  const bool valid = fresh(g, 3000) && g->bytes[6] && isfinite(gpsSpeedKmh);
  jsonHeader(); server.send(200, "application/json", valid ? "{\"value\":" + String(gpsSpeedKmh, 1) + "}" : "{\"value\":null}");
}
String maybeNumber(bool valid, float value, unsigned decimals = 0) { return valid ? String(value, decimals) : "null"; }
String maybeBool(bool valid, bool value) { return valid ? (value ? "true" : "false") : "null"; }
void sendVehicle() {
  readCan();
  Sample *steer=sampleFor(0x100), *lamps=sampleFor(0x101), *immo=sampleFor(0x151), *cmd=sampleFor(0x200);
  Sample *motor=sampleFor(0x201), *sw=sampleFor(0x300), *accel=sampleFor(0x301), *brake=sampleFor(0x302);
  Sample *ign=sampleFor(0x401), *hill=sampleFor(0x500), *summon=sampleFor(0x501);
  bool a=fresh(steer, 250), l=fresh(lamps, 500), m=fresh(motor, 250), s=fresh(sw, 3001);
  bool p=fresh(accel, 501), b=fresh(brake, 250), i=fresh(ign, 3000), h=fresh(hill, 1000);
  String j; j.reserve(1600);
  j = "{\"canReady\":"; j += canReady ? "true" : "false";
  j += ",\"frames\":" + String(frameCount);
  j += ",\"steeringDeg\":" + maybeNumber(a && steer->bytes[3], (int16_t)be16(steer->bytes)/10.0f, 1);
  j += ",\"steeringLink\":" + maybeBool(a, steer->bytes[3]);
  j += ",\"horn\":" + maybeBool(l, lamps->bytes[0]);
  j += ",\"headlight\":" + maybeBool(l, lamps->bytes[1]);
  j += ",\"immoState\":" + maybeNumber(fresh(immo, 3000), immo->bytes[0]);
  j += ",\"immoCommand\":" + maybeNumber(fresh(cmd, 3000), cmd->bytes[0]);
  j += ",\"motor1\":" + maybeBool(m, motor->bytes[0]);
  j += ",\"motor2\":" + maybeBool(m, motor->bytes[1]);
  j += ",\"reverseActive\":" + maybeBool(m, motor->bytes[2]);
  j += ",\"throttleOut\":" + maybeNumber(m, motor->bytes[3]);
  j += ",\"cruiseActive\":" + maybeBool(m, motor->bytes[4]);
  j += ",\"ecoActive\":" + maybeBool(m, motor->bytes[5]);
  j += ",\"hillActive\":" + maybeBool(m, motor->bytes[6]);
  const uint16_t adc = le16(accel->bytes);
  j += ",\"acceleratorAdc\":" + maybeNumber(p, adc);
  // Remove the measured idle offset; retain raw ADC and raw pin-voltage estimate.
  j += ",\"acceleratorPct\":" + maybeNumber(p, throttlePercent(adc));
  j += ",\"acceleratorVoltageEstimate\":" + maybeNumber(p, throttleVoltage(adc), 3);
  j += ",\"acceleratorRawVoltageEstimate\":" + maybeNumber(p, throttleRawVoltage(adc), 3);
  j += ",\"brake\":" + maybeBool(b, brake->bytes[0]);
  j += ",\"ignition\":" + maybeBool(i, ign->bytes[0]);
  j += ",\"ignitionUptime\":" + maybeNumber(i, be16(ign->bytes+1));
  j += ",\"hillRequest\":" + maybeBool(h, hill->bytes[0]);
  j += ",\"pitchDeg\":" + maybeNumber(h && hill->bytes[3], (int)hill->bytes[1]-90);
  j += ",\"pitchValid\":" + maybeBool(h, hill->bytes[3]);
  j += ",\"holdPct\":" + maybeNumber(h, hill->bytes[2]);
  j += ",\"summonActive\":" + maybeBool(fresh(summon, 250), summon->bytes[4]);
  const char *names[] = {"cruise","eco","reverse","hill","horn","headlight","autonomous"};
  j += ",\"switches\":{";
  for (uint8_t n=0; n<7; ++n) { if (n) j += ','; j += "\"" + String(names[n]) + "\":" + maybeBool(s, sw->bytes[0] & (1U << n)); }
  j += "},\"nodes\":[";
  for (uint8_t n=1; n<=5; ++n) {
    if (n>1) j += ',';
    if (n == 2) {
      // This supplied motor controller receives frames but transmits no status.
      j += "{\"id\":2,\"alive\":null,\"source\":\"receive-only\",\"status\":null,\"error\":null,\"tx\":null,\"rx\":null,\"ageMs\":null}";
      continue;
    }
    if (n == 1 || n == 3) {
      const Sample *signal = n == 1 ? accel : sw;
      const bool alive = n == 1 ? p : s;
      j += "{\"id\":" + String(n) + ",\"alive\":" + (alive ? "true" : "false");
      j += ",\"source\":\"data\",\"status\":null,\"error\":null,\"tx\":null,\"rx\":null";
      j += ",\"ageMs\":" + (signal->seen ? String(millis() - signal->at) : "null");
    } else {
      const bool alive = n == 5 ? canReady : nodes[n].seen && millis() - nodes[n].at < 3000;
      j += "{\"id\":" + String(n) + ",\"alive\":" + (alive ? "true" : "false");
      j += ",\"source\":\"" + String(n == 5 ? "local" : "heartbeat") + "\"";
      j += ",\"status\":" + String(n == 5 ? (canReady ? 1 : 0) : nodes[n].status);
      j += ",\"error\":" + String(n == 5 ? (canReady ? 0 : 1) : nodes[n].error);
      j += ",\"tx\":" + String(n == 5 ? canTxCount : nodes[n].tx);
      j += ",\"rx\":" + String(n == 5 ? canRxCount : nodes[n].rx);
      j += ",\"ageMs\":" + (n == 5 ? String(0) : (nodes[n].seen ? String(millis()-nodes[n].at) : "null"));
    }
    j += '}';
  }
  j += "]}";
  jsonHeader(); server.send(200, "application/json", j);
}
void sendCanLog() {
  // Optional exact ID filter in hexadecimal, e.g. ?id=0x301 or ?id=301.
  // Filtering this response never changes CAN reception or vehicle telemetry.
  String filter = server.arg("id"); filter.trim();
  const bool filtered = filter.length() > 0;
  uint32_t filterId = 0;
  if (filtered) {
    unsigned start = filter.startsWith("0x") || filter.startsWith("0X") ? 2 : 0;
    bool valid = filter.length() > start && filter.length() - start <= 8;
    for (unsigned i = start; valid && i < filter.length(); ++i) {
      const char c = filter[i];
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) valid = false;
      else filterId = (filterId << 4) | (uint32_t)digit;
    }
    if (!valid || filterId > 0x1FFFFFFFUL) {
      jsonHeader(); server.send(400, "application/json", "{\"error\":\"id must be hex from 0 to 1FFFFFFF\"}");
      return;
    }
  }
  readCan(); String j; j.reserve(9500);
  j = "{\"ready\":"; j += canReady ? "true" : "false";
  j += ",\"uptimeMs\":" + String(millis());
  j += ",\"total\":" + String(frameCount) + ",\"frames\":[";
  bool first = true;
  for (uint8_t n=0; n<logCount; ++n) {
    const LogFrame &f=logFrames[(logNext+64-logCount+n)%64];
    if (filtered && f.id != filterId) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"seq\":"+String(f.seq)+",\"ms\":"+String(f.ms)+",\"id\":"+String(f.id)+",\"dlc\":"+String(f.len)+",\"data\":[";
    for (uint8_t k=0;k<f.len;++k) { if (k) j+=','; j+=String(f.bytes[k]); }
    j += "]}";
  }
  j += "]}"; jsonHeader(); server.send(200, "application/json", j);
}
const char *mime(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  return "application/octet-stream";
}
bool sendFile(String path) {
  if (!fsReady || path.indexOf("..") >= 0) return false;
  if (path.endsWith("/")) path += "index.html";
  String filePath = path + ".gz";
  bool gz = LittleFS.exists(filePath);
  if (!gz) filePath = path;
  if (!LittleFS.exists(filePath)) return false;
  File f = LittleFS.open(filePath, "r"); if (!f || f.isDirectory()) return false;
  server.sendHeader("Cache-Control", path.startsWith("/tiles/") ? "max-age=31536000" : "no-cache");
  if (gz) server.sendHeader("Content-Encoding", "gzip");
  server.setContentLength(f.size()); server.send(200, mime(path), "");
  char chunk[1024];
  while (f.available() && server.client().connected()) {
    readCan(); size_t n=f.readBytes(chunk, sizeof(chunk)); server.sendContent(chunk, n); yield();
  }
  f.close(); readCan(); return true;
}
// Bounded, on-demand diagnostics. Histories stay in the browser.
void sendDiagnostics() {
  readCan();
  String j; j.reserve(1800);
  j = "{\"uptimeMs\":" + String(millis());
  j += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  j += ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap());
  j += ",\"largestFreeBlock\":" + String(ESP.getMaxAllocHeap());
  j += ",\"fsReady\":" + String(fsReady ? "true" : "false");
  j += ",\"fsTotal\":" + String(fsReady ? LittleFS.totalBytes() : 0);
  j += ",\"fsUsed\":" + String(fsReady ? LittleFS.usedBytes() : 0);
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  j += ",\"canReady\":" + String(canReady ? "true" : "false");
  j += ",\"rxTotal\":" + String(canRxTotal) + ",\"txTotal\":" + String(canTxTotal);
  j += ",\"txFailed\":" + String(canTxFailed);
  j += ",\"lastRxAgeMs\":" + (canRxTotal ? String(millis() - canLastRxAt) : "null");
  j += ",\"malformedFrames\":" + String(malformedFrames) + ",\"unknownFrames\":" + String(unknownFrames);
  j += ",\"signals\":[";
  const uint16_t signalIds[] = {0x301,0x300,0x400,0x401,0x500};
  const uint16_t timeouts[] = {501,3001,3000,3000,1000};
  for (uint8_t n=0; n<5; ++n) {
    const Sample *s = sampleFor(signalIds[n]);
    if (n) j += ',';
    j += "{\"id\":" + String(signalIds[n]) + ",\"seen\":" + String(s->seen ? "true" : "false");
    j += ",\"fresh\":" + String(fresh(s, timeouts[n]) ? "true" : "false");
    j += ",\"ageMs\":" + (s->seen ? String(millis()-s->at) : "null");
    j += ",\"timeoutMs\":" + String(timeouts[n]) + ",\"dlc\":" + String(s->len);
    j += ",\"received\":" + String(s->received) + ",\"invalid\":" + String(s->invalid) + "}";
  }
  j += "]}"; jsonHeader(); server.send(200, "application/json", j);
}
void sendDashboard() {
  if (!sendFile("/index.html")) server.send(404,"text/plain","website missing: upload LittleFS data");
}
void setup() {
  Serial.begin(115200);
  fsReady = LittleFS.begin(false);
  if (!fsReady) Serial.println("LittleFS unavailable: upload data/");
  if (!canBegin()) Serial.println("CAN unavailable: check MCP2515 wiring/crystal");
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); WiFi.setSleep(false);
  server.on("/api/data", sendSpeed); server.on("/api/gps", sendGps);
  server.on("/api/vehicle", sendVehicle); server.on("/api/can-log", sendCanLog);
  server.on("/api/diagnostics", sendDiagnostics);
  server.on("/status", sendDashboard); server.on("/status/", sendDashboard);
  server.onNotFound([](){
    if (server.uri().startsWith("/api/")) { server.send(404,"application/json","{\"error\":\"unknown API\"}"); return; }
    if (sendFile(server.uri())) return;
    if (server.uri().startsWith("/tiles/")) { server.send(404,"text/plain","no tile"); return; }
    if (!sendFile("/index.html")) server.send(404,"text/plain","website missing: upload LittleFS data");
  });
  server.begin(); Serial.print("Dashboard: http://"); Serial.println(WiFi.softAPIP());
}
void loop() {
  readCan(); server.handleClient(); readCan();
  if (millis()-lastBeat >= 1000) { lastBeat=millis(); canPrintStatus("Node 5 dashboard");
    Serial.printf("Dashboard: WiFi clients=%u LittleFS=%s buffered frames=%u total frames=%lu | Node 1=%s Node 3=%s | Node 2=receive-only\n",
                  WiFi.softAPgetStationNum(), fsReady ? "READY" : "MISSING", logCount, (unsigned long)frameCount,
                  fresh(sampleFor(0x301), 501) ? "LIVE" : "NO RECENT DATA", fresh(sampleFor(0x300), 3001) ? "LIVE" : "NO RECENT DATA");
    canHeartbeat(5, canReady?1:0, canReady?0:1); }
}
