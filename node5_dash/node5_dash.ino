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
  estimated GPIO voltage. Node 2 reports motor output on 0x201 and
  health on 0x600; older receive-only firmware remains unknown.
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
#include "SteeringControl.h"
#include "SteeringFeedback.h"
#include "RemoteDrive.h"
#include <esp_system.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <math.h>

// Upload this sketch AND data/ with LittleFS (No OTA partition).
const char *AP_SSID = "srt-dash";
const char *AP_PASS = "12345678"; // Change before deployment.
WebServer server(80);
struct Sample { uint8_t bytes[8] = {}; uint8_t len = 0; uint32_t at = 0, received = 0, invalid = 0; bool seen = false; };
SteeringRelay steeringRelay;
struct DriveHost {
  uint32_t token=1, session=0, lastHttp=0, started=0, lastTx=0, settledAt=0, stoppedAt=0;
  uint16_t httpSeq=0, gateSeq=0, adc=0;
  uint8_t percent=0;
  bool held=false, paired=false, reverse=false, wireReverse=false, directionSet=false;
  const char *reason="";
} drive;
void serviceDrive();
const uint16_t ids[] = {0x121,0x122,0x111,0x100,0x101,0x151,0x200,0x201,0x300,0x301,0x302,0x400,0x401,0x500,0x501};
Sample samples[sizeof(ids)/sizeof(ids[0])];
struct NodeState { uint8_t status = 0, error = 0, tx = 0, rx = 0; uint32_t at = 0; bool seen = false; } nodes[6];
struct LogFrame { uint32_t seq, ms, id; uint8_t len, bytes[8]; bool tx=false; } logFrames[64];
uint8_t logNext = 0, logCount = 0;
uint32_t frameCount = 0, lastBeat = 0, malformedFrames = 0, unknownFrames = 0;
bool fsReady = false;
float gpsSpeedKmh = NAN, prevLat = NAN, prevLon = NAN;
uint32_t prevGpsAt = 0;

template<size_t N> bool sendLoggedCan(uint32_t id, const uint8_t (&data)[N]) {
  if(!canSend(id,data)) return false;
  LogFrame &f=logFrames[logNext]; f.seq=++frameCount; f.ms=millis(); f.id=id; f.len=N; f.tx=true;
  memset(f.bytes,0,8); memcpy(f.bytes,data,N);
  logNext=(logNext+1)%64; if(logCount<64) ++logCount;
  return true;
}

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
  for (uint8_t n=0; n<16 && canRead(id, len, data); ++n) {
    const uint32_t now = millis();
    ++frameCount;
    LogFrame &f = logFrames[logNext];
    f.tx=false; f.seq = frameCount; f.ms = now; f.id = id; f.len = len; memcpy(f.bytes, data, 8);
    logNext = (logNext + 1) % 64; if (logCount < 64) ++logCount;
    const bool validHeartbeat = len == 8 && data[0] >= 1 && data[0] <= 5 && data[1] <= 1;
    if (id == 0x600 && validHeartbeat) {
      NodeState &n = nodes[data[0]];
      n.seen = true; n.at = now; n.status = data[1]; n.error = data[3]; n.tx = data[4]; n.rx = data[5];
    }
    Sample *s = sampleFor(id);
    const uint8_t expectedLength = id == 0x300 ? 1 : id == 0x301 ? 2 : 8;
    const bool validGps = id != 0x400 || (data[6] <= 1 && (!data[6] ||
      (data[0] <= 90 && data[3] <= 180 && be16(data+1) < 6000 && be16(data+4) < 6000 &&
       (data[0] < 90 || be16(data+1) == 0) && (data[3] < 180 || be16(data+4) == 0))));
    const bool validPitch = id != 0x500 || (data[0] <= 1 && data[2] <= 100 && data[3] <= 1 && (!data[3] || data[1] <= 180));
    const bool validMotor = id != 0x201 || (data[0]<=1 && data[1]<=1 && data[2]<=1 && data[3]<=100 &&
      data[4]<=1 && data[5]<=1 && data[6]<=1 && data[7]<=8);
    const bool validPayload = len == expectedLength && (id != 0x301 || le16(data) <= 4095) &&
      (id != 0x300 || !(data[0]&0x80)) && (id != 0x401 || data[0]<=1) && validGps && validPitch && validMotor &&
      (id != STEERING_ANGLE_ID || steeringAngleFrameValid(data)) &&
      ((id != 0x121 && id != 0x122) || driveStatusValid(data,id==0x121 ? 4095 : 127)) &&
      (id != STEERING_STATUS_ID || (data[0] <= 2 && data[1] == (data[0] ? STEERING_PWM : 0) && data[2] <= 5 && data[3] == 1));
    if (s) { ++s->received; if (!validPayload) { ++s->invalid; ++malformedFrames; } }
    else if (id == 0x600) { if (!validHeartbeat) ++malformedFrames; }
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
  bool a=fresh(steer, STEERING_FEEDBACK_TIMEOUT_MS) && steeringAngleFrameFresh(steer->bytes, millis()-steer->at);
  bool l=fresh(lamps, 500), m=fresh(motor, 250), s=fresh(sw, 3001);
  bool p=fresh(accel, 501), b=fresh(brake, 250), i=fresh(ign, 3000), h=fresh(hill, 1000);
  String j; j.reserve(1600);
  j = "{\"canReady\":"; j += canReady ? "true" : "false";
  j += ",\"frames\":" + String(frameCount);
  j += ",\"steeringDeg\":" + maybeNumber(a, steeringAngleTenths(steer->bytes)/10.0f, 1);
  j += ",\"steeringLink\":" + String(a ? "true" : "false");
  j += ",\"steeringAgeMs\":" + (steer->seen ? String(millis()-steer->at+steer->bytes[2]) : "null");
  j += ",\"steeringSequence\":" + (steer->seen ? String(steeringAngleSequence(steer->bytes)) : "null");
  j += ",\"horn\":" + maybeBool(l, lamps->bytes[0]);
  j += ",\"headlight\":" + maybeBool(l, lamps->bytes[1]);
  j += ",\"immoState\":" + maybeNumber(fresh(immo, 3000), immo->bytes[0]);
  j += ",\"immoCommand\":" + maybeNumber(fresh(cmd, 3000), cmd->bytes[0]);
  j += ",\"motor1\":" + maybeBool(m, motor->bytes[0]);
  j += ",\"motor2\":" + maybeBool(m, motor->bytes[1]);
  j += ",\"reverseActive\":" + maybeBool(m, motor->bytes[2]);
  j += ",\"throttleOut\":" + maybeNumber(m, motor->bytes[3]);
  j += ",\"powertrainReason\":" + maybeNumber(m, motor->bytes[7]);
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
    if (n == 2 && !nodes[2].seen) {
      // Older receive-only Node 2 firmware remains unobservable.
      j += "{\"id\":2,\"alive\":null,\"source\":\"receive-only\",\"status\":null,\"error\":null,\"tx\":null,\"rx\":null,\"ageMs\":null}";
      continue;
    }
    if (n == 1 || n == 3) {
      const Sample *gate = sampleFor(n==1 ? 0x121 : 0x122);
      const Sample *signal = fresh(gate,350) ? gate : n == 1 ? accel : sw;
      const bool alive = fresh(gate,350) || (n == 1 ? p : s);
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
  const Sample *st = sampleFor(STEERING_STATUS_ID);
  const bool steeringFresh = fresh(st, 600);
  j += "],\"steering\":{\"available\":" + String(steeringFresh ? "true" : "false");
  j += ",\"direction\":" + maybeNumber(steeringFresh, st->bytes[0]);
  j += ",\"pwm\":" + maybeNumber(steeringFresh, st->bytes[1]);
  j += ",\"reason\":" + maybeNumber(steeringFresh, st->bytes[2]);
  j += ",\"ageMs\":" + (st->seen ? String(millis()-st->at) : "null") + "}";
  const Sample *dp=sampleFor(0x121), *ds=sampleFor(0x122);
  j+=",\"remoteDrive\":{\"active\":"+String(drive.held?"true":"false")+",\"adc\":"+String(drive.adc);
  j+=",\"reverse\":"+String(drive.wireReverse?"true":"false")+",\"reason\":\""+String(drive.reason)+"\"";
  j+=",\"pedalAdc\":"+maybeNumber(fresh(dp,350),le16(dp->bytes+1));
  j+=",\"switchMask\":"+maybeNumber(fresh(ds,350),ds->bytes[1]);
  j+=",\"pedalGate\":"+maybeNumber(fresh(dp,350),dp->bytes[0]&3);
  j+=",\"switchGate\":"+maybeNumber(fresh(ds,350),ds->bytes[0]&3)+"}}";
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
    j += "{\"seq\":"+String(f.seq)+",\"ms\":"+String(f.ms)+",\"id\":"+String(f.id)+",\"dlc\":"+String(f.len)+",\"tx\":"+String(f.tx ? "true" : "false")+",\"data\":[";
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
    readCan(); serviceDrive(); serviceSteeringRelay(); size_t n=f.readBytes(chunk, sizeof(chunk)); server.sendContent(chunk, n); yield();
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
  const uint16_t signalIds[] = {0x301,0x300,0x400,0x401,0x500,0x111,0x121,0x122,STEERING_ANGLE_ID};
  const uint16_t timeouts[] = {501,3001,3000,3000,1000,600,350,350,STEERING_FEEDBACK_TIMEOUT_MS};
  for (uint8_t n=0; n<sizeof(signalIds)/sizeof(signalIds[0]); ++n) {
    const Sample *s = sampleFor(signalIds[n]);
    if (n) j += ',';
    j += "{\"id\":" + String(signalIds[n]) + ",\"seen\":" + String(s->seen ? "true" : "false");
    const bool live = fresh(s, timeouts[n]) && (signalIds[n] != STEERING_ANGLE_ID || steeringAngleFrameFresh(s->bytes, millis()-s->at));
    j += ",\"fresh\":" + String(live ? "true" : "false");
    j += ",\"ageMs\":" + (s->seen ? String(millis()-s->at) : "null");
    j += ",\"timeoutMs\":" + String(timeouts[n]) + ",\"dlc\":" + String(s->len);
    j += ",\"received\":" + String(s->received) + ",\"invalid\":" + String(s->invalid) + "}";
  }
  j += "]}"; jsonHeader(); server.send(200, "application/json", j);
}
void sendDashboard() {
  if (!sendFile("/index.html")) server.send(404,"text/plain","website missing: upload LittleFS data");
}
// Steering writes require a same-origin custom-header POST. Read-only telemetry
// remains public to clients on the dashboard AP; no GET can actuate a motor.
bool steeringNodeReady() {
  const Sample *s = sampleFor(STEERING_STATUS_ID);
  const Sample *angle = sampleFor(STEERING_ANGLE_ID);
  return canReady && fresh(s, 600) && s->bytes[3] == 1 &&
    fresh(angle, STEERING_FEEDBACK_TIMEOUT_MS) && steeringAngleFrameFresh(angle->bytes, millis()-angle->at);
}
void stopSteeringRelay() {
  steeringRelay.stop();
  const uint8_t data[8] = {};
  sendLoggedCan(STEERING_COMMAND_ID, data);
}
void serviceSteeringRelay() {
  const Sample *s = sampleFor(STEERING_STATUS_ID);
  if (steeringRelay.direction && (!steeringNodeReady() || steeringRead32(s->bytes+4) != steeringRelay.nodeToken)) {
    stopSteeringRelay();
  } else if (steeringRelay.tick(millis())) {
    const uint8_t data[8] = {};
    sendLoggedCan(STEERING_COMMAND_ID, data);
  }
}
void sendSteeringState() {
  readCan(); serviceSteeringRelay();
  const Sample *s = sampleFor(STEERING_STATUS_ID);
  const Sample *angle = sampleFor(STEERING_ANGLE_ID);
  const bool statusLive = fresh(s, 600);
  const bool angleLive = fresh(angle, STEERING_FEEDBACK_TIMEOUT_MS) && steeringAngleFrameFresh(angle->bytes, millis()-angle->at);
  const bool ready = steeringNodeReady();
  String j = "{\"ready\":" + String(ready ? "true" : "false");
  j += ",\"token\":" + String(steeringRelay.token);
  j += ",\"direction\":" + (statusLive ? String(s->bytes[0]) : "null");
  j += ",\"pwm\":" + (statusLive ? String(s->bytes[1]) : "null");
  j += ",\"reason\":" + (statusLive ? String(s->bytes[2]) : "null");
  j += ",\"angleAvailable\":" + String(angleLive ? "true" : "false");
  j += ",\"angleDeg\":" + maybeNumber(angleLive, steeringAngleTenths(angle->bytes)/10.0f, 1);
  j += ",\"angleAgeMs\":" + (angle->seen ? String(millis()-angle->at+angle->bytes[2]) : "null");
  j += ",\"angleSequence\":" + (angle->seen ? String(steeringAngleSequence(angle->bytes)) : "null");
  j += ",\"ageMs\":" + (s->seen ? String(millis()-s->at) : "null");
  j += ",\"timeoutMs\":350,\"holdMs\":5000}";
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", j);
}
bool steeringDecimal(const String &s, uint32_t &value) {
  if (!s.length() || s.length()>10) return false;
  uint64_t parsed = 0;
  for (unsigned n=0; n<s.length(); ++n) {
    if (s[n]<'0' || s[n]>'9') return false;
    parsed = parsed*10 + (s[n]-'0'); if (parsed>0xFFFFFFFFULL) return false;
  }
  value = parsed; return true;
}
void handleSteeringCommand() {
  server.sendHeader("Cache-Control", "no-store");
  if (server.header("X-SRT-Control") != "1" ||
      (server.hasHeader("Origin") && server.header("Origin") != "http://" + server.hostHeader())) {
    server.send(403,"application/json","{\"error\":\"Same-origin control required\"}"); return;
  }
  readCan(); serviceSteeringRelay();
  const String dir = server.arg("direction");
  if (dir == "stop") { stopSteeringRelay(); sendSteeringState(); return; }
  uint32_t token, seq;
  if ((dir != "left" && dir != "right") || !steeringDecimal(server.arg("token"),token) ||
      !steeringDecimal(server.arg("seq"),seq) || seq == 0 || seq > 65535) {
    stopSteeringRelay(); server.send(400,"application/json","{\"error\":\"Invalid steering command\"}"); return;
  }
  const Sample *s = sampleFor(STEERING_STATUS_ID);
  const bool ready = steeringNodeReady();
  if (!steeringRelay.accept(dir=="left" ? 1 : 2, seq, token, steeringRead32(s->bytes+4), ready, millis())) {
    stopSteeringRelay(); server.send(ready ? 409 : 503,"application/json","{\"error\":\"Release and retry\"}"); return;
  }
  uint8_t data[8] = {steeringRelay.direction, STEERING_PWM, (uint8_t)seq, (uint8_t)(seq>>8),0,0,0,0};
  steeringWrite32(data+4, steeringRelay.nodeToken);
  if (!sendLoggedCan(STEERING_COMMAND_ID, data)) {
    stopSteeringRelay(); server.send(503,"application/json","{\"error\":\"CAN send failed\"}"); return;
  }
  // This acknowledges transmission, not physical steering position.
  sendSteeringState();
}

bool driveCapable() {
  return canReady && fresh(sampleFor(0x121),250) && fresh(sampleFor(0x122),250);
}
bool drivePedalIdle() { return driveCapable() && le16(sampleFor(0x121)->bytes+1)<=DRIVE_IDLE_ADC; }
bool driveGateAt(uint16_t id, uint8_t phase) {
  const Sample *s=sampleFor(id);
  return fresh(s,250) && (s->bytes[0]&3)==phase && driveRead32(s->bytes+4)==drive.session;
}
bool driveReady() {
  return drivePedalIdle() && (drive.held || ((sampleFor(0x121)->bytes[0]&3)!=1 && (sampleFor(0x122)->bytes[0]&3)!=1));
}
bool driveCanShift() { return !drive.held && (!drive.session || millis()-drive.stoppedAt>=DRIVE_SETTLE_MS); }
bool sendDriveGate(uint8_t op) {
  uint8_t data[8]={op,(uint8_t)drive.wireReverse,0,0,0,0,0,0};
  driveWrite32(data+2,drive.session); ++drive.gateSeq;
  data[6]=drive.gateSeq; data[7]=drive.gateSeq>>8;
  return sendLoggedCan(DRIVE_GATE_ID,data);
}
bool sendDriveAdc(uint16_t adc) {
  const uint8_t data[2]={(uint8_t)adc,(uint8_t)(adc>>8)};
  const bool sent=sendLoggedCan(0x301,data); drive.adc=sent ? adc : 0; return sent;
}
void stopDrive(const char *reason="") {
  const bool owned=drive.paired || driveGateAt(0x121,1) || driveGateAt(0x121,2);
  if(drive.held) drive.stoppedAt=millis();
  drive.held=false; drive.paired=false; drive.percent=0; drive.adc=0; drive.reason=reason;
  if(++drive.token==0) ++drive.token;
  if(drive.session) { if(owned) sendDriveAdc(0); sendDriveGate(0); }
}
void serviceDrive() {
  const uint32_t now=millis();
  if(drive.held && (now-drive.lastHttp>=DRIVE_LEASE_MS || now-drive.started>=DRIVE_HOLD_MS)) stopDrive("RELEASE & RETRY");
  if(drive.held && !drivePedalIdle()) stopDrive("PEDAL / LINK FAULT");
  if(!drive.session || now-drive.lastTx<50) return;
  drive.lastTx=now;
  if(!drive.held) {
    // Nodes latch zero/held direction on expiry. Restore switches before pedal.
    const bool pedalNormal=fresh(sampleFor(0x121),250) && (sampleFor(0x121)->bytes[0]&3)==0;
    const bool switchesNormal=fresh(sampleFor(0x122),250) && (sampleFor(0x122)->bytes[0]&3)==0;
    if(pedalNormal && switchesNormal) { drive.session=0; return; }
    // A rejected partial claim must not inject zeros over the normal pedal sender.
    if(!pedalNormal) sendDriveAdc(0);
    sendDriveGate(0);
    if(drivePedalIdle() && now-drive.stoppedAt>=DRIVE_SETTLE_MS && bool(sampleFor(0x122)->bytes[1]&4)==drive.wireReverse) {
      if(driveGateAt(0x122,2)) sendDriveGate(2);
      else if(driveGateAt(0x122,0)) sendDriveGate(3);
    }
    return;
  }
  const bool paired=driveGateAt(0x121,1) && driveGateAt(0x122,1);
  if(drive.paired && !paired) { stopDrive("HANDOFF LOST"); return; }
  if(!paired && now-drive.started>=600) { stopDrive("HANDOFF FAILED"); return; }
  if(paired && !drive.paired) { drive.paired=true; drive.settledAt=now; }
  // First hold the old direction at zero; then switch and settle again.
  if(paired && drive.wireReverse!=drive.reverse && now-drive.settledAt>=DRIVE_SETTLE_MS) {
    drive.wireReverse=drive.reverse; drive.settledAt=now;
  }
  if(!sendDriveGate(1)) { stopDrive("CAN SEND FAILED"); return; }
  if(!paired) return;
  const uint8_t switches[1]={driveSwitchMask(sampleFor(0x122)->bytes[1],drive.wireReverse)};
  const bool directionAck=bool(sampleFor(0x121)->bytes[0]&0x80)==drive.wireReverse && bool(sampleFor(0x122)->bytes[0]&0x80)==drive.wireReverse;
  const uint16_t adc=directionAck && drive.wireReverse==drive.reverse && now-drive.settledAt>=DRIVE_SETTLE_MS ? (uint32_t)drive.percent*4095/100 : 0;
  if(!sendLoggedCan(0x300,switches) || !sendDriveAdc(adc)) stopDrive("CAN SEND FAILED");
}
void sendDriveState() {
  readCan(); serviceDrive();
  const bool capable=driveCapable();
  if(capable && !drive.directionSet && !drive.session) drive.reverse=bool(sampleFor(0x122)->bytes[1]&4);
  const char *message=drive.reason;
  if(!capable) message="NODES 1 / 3 UNAVAILABLE";
  else if(!drivePedalIdle()) message="RELEASE PEDAL";
  else if(drive.held && !drive.adc) message="ARMING";
  else if(!drive.held && drive.session && bool(sampleFor(0x122)->bytes[1]&4)!=drive.wireReverse) message="MATCH REVERSE SWITCH";
  String j="{\"ready\":"+String(driveReady()?"true":"false")+",\"token\":"+String(drive.token);
  j+=",\"held\":"+String(drive.held?"true":"false")+",\"adc\":"+String(drive.adc)+",\"percent\":"+String(drive.percent);
  j+=",\"reverse\":"+String(drive.reverse?"true":"false")+",\"canShift\":"+String(driveCanShift()?"true":"false");
  j+=",\"message\":\""+String(message)+"\",\"timeoutMs\":350,\"holdMs\":5000}";
  server.sendHeader("Cache-Control","no-store"); server.send(200,"application/json",j);
}
void handleDriveCommand() {
  server.sendHeader("Cache-Control","no-store");
  if(server.header("X-SRT-Control")!="1" || (server.hasHeader("Origin") && server.header("Origin")!="http://"+server.hostHeader())) {
    server.send(403,"application/json","{\"error\":\"Same-origin control required\"}"); return;
  }
  readCan(); serviceDrive();
  const String action=server.arg("action");
  if(action=="stop") { stopDrive(); sendDriveState(); return; }
  uint32_t token=0,seq=0,percent=0,reverse=0;
  if(!steeringDecimal(server.arg("token"),token) || (action!="throttle" && action!="reverse") ||
    (action=="throttle" && (!steeringDecimal(server.arg("seq"),seq) || !seq || seq>65535 || !steeringDecimal(server.arg("percent"),percent) || percent<1 || percent>100)) ||
    (action=="reverse" && (!steeringDecimal(server.arg("reverse"),reverse) || reverse>1))) {
    stopDrive("INVALID COMMAND"); server.send(400,"application/json","{\"error\":\"Invalid drive command\"}"); return;
  }
  if(token!=drive.token || !driveReady() || (action=="reverse" && !driveCanShift())) {
    stopDrive("RELEASE & RETRY"); server.send(409,"application/json","{\"error\":\"Release and retry\"}"); return;
  }
  if(action=="reverse") {
    drive.reverse=reverse; drive.directionSet=true; drive.reason="";
    if(++drive.token==0) ++drive.token;
    sendDriveState(); return;
  }
  if(drive.held && (seq<=drive.httpSeq || percent!=drive.percent)) {
    stopDrive("RELEASE & RETRY"); server.send(409,"application/json","{\"error\":\"Release and retry\"}"); return;
  }
  if(!drive.held) {
    drive.session=drive.token; drive.gateSeq=0; drive.started=millis(); drive.paired=false;
    const Sample *sw=sampleFor(0x122);
    drive.wireReverse=(sw->bytes[0]&3)==2 ? bool(sw->bytes[0]&0x80) : bool(sw->bytes[1]&4);
    drive.percent=percent; drive.held=true; drive.reason="";
    drive.lastTx=millis()-50;
  }
  drive.httpSeq=seq; drive.lastHttp=millis(); sendDriveState();
}

void setup() {
  Serial.begin(115200);
  drive.token=esp_random(); if(!drive.token) drive.token=1;
  steeringRelay.token = esp_random(); if (!steeringRelay.token) steeringRelay.token = 1;
  fsReady = LittleFS.begin(false);
  if (!fsReady) Serial.println("LittleFS unavailable: upload data/");
  if (!canBegin()) Serial.println("CAN unavailable: check MCP2515 wiring/crystal");
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); WiFi.setSleep(false);
  const char *controlHeaders[] = {"X-SRT-Control", "Origin"};
  server.collectHeaders(controlHeaders, 2);
  server.on("/api/drive", HTTP_GET, sendDriveState);
  server.on("/api/drive", HTTP_POST, handleDriveCommand);
  server.on("/api/steering", HTTP_GET, sendSteeringState);
  server.on("/api/steering", HTTP_POST, handleSteeringCommand);
  server.on("/api/data", sendSpeed); server.on("/api/gps", sendGps);
  server.on("/api/vehicle", sendVehicle); server.on("/api/can-log", sendCanLog);
  server.on("/api/diagnostics", sendDiagnostics);
  server.on("/status", sendDashboard); server.on("/status/", sendDashboard);
  server.on("/control", sendDashboard); server.on("/control/", sendDashboard);
  server.onNotFound([](){
    if (server.uri().startsWith("/api/")) { server.send(404,"application/json","{\"error\":\"unknown API\"}"); return; }
    if (sendFile(server.uri())) return;
    if (server.uri().startsWith("/tiles/")) { server.send(404,"text/plain","no tile"); return; }
    if (!sendFile("/index.html")) server.send(404,"text/plain","website missing: upload LittleFS data");
  });
  server.begin(); Serial.print("Dashboard: http://"); Serial.println(WiFi.softAPIP());
}
void loop() {
  readCan(); serviceDrive(); serviceSteeringRelay(); server.handleClient(); readCan(); serviceDrive(); serviceSteeringRelay();
  if (millis()-lastBeat >= 1000) { lastBeat=millis(); canPrintStatus("Node 5 dashboard");
    Serial.printf("Dashboard: WiFi clients=%u LittleFS=%s buffered frames=%u total frames=%lu | Node 1=%s Node 3=%s | Node 2 status: inspect 0x201/0x600\n",
                  WiFi.softAPgetStationNum(), fsReady ? "READY" : "MISSING", logCount, (unsigned long)frameCount,
                  fresh(sampleFor(0x301), 501) ? "LIVE" : "NO RECENT DATA", fresh(sampleFor(0x300), 3001) ? "LIVE" : "NO RECENT DATA");
    canHeartbeat(5, canReady?1:0, canReady?0:1); }
}
