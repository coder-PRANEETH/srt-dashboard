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

// Keep the complete bounded /api/can-log, but do not block HTTP command leases
// on UART frame printing when simultaneous traction/steering fills the bus.
#define CAN_FRAME_SERIAL_LOG 0
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
  uint16_t httpSeq=0, gateSeq=0, pwmSeq=0, adc=0;
  uint8_t percent=0, pwm=0;
  bool held=false, paired=false, reverse=false, wireReverse=false, directionSet=false, pwmMode=false;
  bool steeringOwned=false, shiftOnly=false, shiftSent=false;
  uint8_t shiftPhase=1;
  uint32_t shiftAckCount=0, completedShiftToken=0;
  bool completedReverse=false;
  const char *reason="";
} drive;
bool joystickSteerStopping=false;
uint32_t joystickStoppedNodeToken=0;
void serviceDrive();
void stopDrive(const char *reason);
const uint16_t ids[] = {0x121,0x122,0x124,0x111,0x100,0x101,0x151,0x200,0x201,0x300,0x301,0x302,0x400,0x401,0x500,0x501};
Sample samples[sizeof(ids)/sizeof(ids[0])];
struct NodeState { uint8_t status = 0, error = 0, tx = 0, rx = 0; uint32_t at = 0; bool seen = false; } nodes[6];
struct LogFrame { uint32_t seq, ms, id; uint8_t len, bytes[8]; bool tx=false; } logFrames[64];
uint8_t logNext = 0, logCount = 0;
uint32_t frameCount = 0, lastBeat = 0, malformedFrames = 0, unknownFrames = 0, lastCanAttempt = 0;
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
      data[4]<=1 && data[5]<=1 && data[6]<=1 && data[7]<=9);
    const bool validPayload = len == expectedLength && (id != 0x301 || le16(data) <= 4095) &&
      (id != 0x300 || !(data[0]&~SWITCH_VALID_MASK)) && (id != 0x401 || data[0]<=1) && validGps && validPitch && validMotor &&
      (id != STEERING_ANGLE_ID || steeringAngleFrameValid(data)) &&
      ((id != 0x121 && id != 0x122 && id != 0x124) || driveStatusValid(data,id==0x121 ? 4095 : id==0x122 ? SWITCH_VALID_MASK : 255)) &&
      (id != STEERING_STATUS_ID || steeringOutputValid(data));
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
  const char *names[] = {"cruise","hill","autonomous","remoteMode","horn","reverse"};
  j += ",\"switches\":{";
  for (uint8_t n=0; n<SWITCH_COUNT; ++n) { if (n) j += ','; j += "\"" + String(names[n]) + "\":" + maybeBool(s, sw->bytes[0] & (1U << n)); }
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
  const Sample *dp=sampleFor(0x121), *ds=sampleFor(0x122), *dm=sampleFor(0x124);
  j+=",\"remoteDrive\":{\"active\":"+String(drive.held?"true":"false")+",\"adc\":"+String(drive.adc);
  j+=",\"reverse\":"+String(drive.wireReverse?"true":"false")+",\"reason\":\""+String(drive.reason)+"\"";
  j+=",\"pwmMode\":"+String(drive.pwmMode?"true":"false")+",\"pwm\":"+String(drive.pwm);
  j+=",\"motorGate\":"+maybeNumber(fresh(dm,350),dm->bytes[0]&3);
  j+=",\"appliedPwm\":"+maybeNumber(fresh(dm,350),le16(dm->bytes+1));
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
  const uint16_t signalIds[] = {0x301,0x300,0x400,0x401,0x500,0x111,0x121,0x122,STEERING_ANGLE_ID,0x124};
  const uint16_t timeouts[] = {501,3001,3000,3000,1000,600,350,350,STEERING_FEEDBACK_TIMEOUT_MS,350};
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
  return canReady && fresh(s, 600) && steeringOutputValid(s->bytes);
}
const char *steeringLinkMessage() {
  const Sample *s=sampleFor(STEERING_STATUS_ID);
  if (!canReady) return "DASHBOARD CAN NOT READY";
  if (!s->seen) return s->invalid ? "NODE 1 STATUS FORMAT INVALID" : "WAITING FOR NODE 1 CAN STATUS";
  if (!fresh(s,600)) return "NODE 1 CAN STATUS STALE";
  return "";
}
void stopSteeringRelay() {
  steeringRelay.stop();
  const uint8_t data[8] = {};
  sendLoggedCan(STEERING_COMMAND_ID, data);
}
void serviceSteeringRelay() {
  const Sample *s = sampleFor(STEERING_STATUS_ID);
  if (steeringRelay.direction && (!steeringNodeReady() || steeringRead32(s->bytes+4) != steeringRelay.nodeToken)) {
    if(drive.held && drive.steeringOwned) stopDrive("STEERING OUTPUT LINK LOST");
    else stopSteeringRelay();
  } else if (steeringRelay.tick(millis())) {
    if(drive.held && drive.steeringOwned) { stopDrive("STEERING COMMAND EXPIRED"); return; }
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
  j += ",\"controlVersion\":3,\"angleRequired\":false,\"canReady\":" + String(canReady ? "true" : "false");
  j += ",\"nodeSeen\":" + String(s->seen ? "true" : "false") + ",\"nodeInvalid\":" + String(s->invalid);
  j += ",\"message\":\"" + String(steeringLinkMessage()) + "\"";
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
  return canReady && fresh(sampleFor(0x121),250) && fresh(sampleFor(0x122),250) &&
    fresh(sampleFor(DRIVE_MOTOR_STATUS_ID),250) && fresh(sampleFor(0x201),250);
}
bool drivePedalIdle() { return driveCapable() && le16(sampleFor(0x121)->bytes+1)<=DRIVE_IDLE_ADC; }
bool driveGateAt(uint16_t id, uint8_t phase) {
  const Sample *s = sampleFor(id);
  return fresh(s,250) && (s->bytes[0]&3)==phase && driveRead32(s->bytes+4)==drive.session;
}
bool driveReady() {
  return driveCapable() && (drive.held || (drivePedalIdle() &&
    (sampleFor(0x121)->bytes[0]&3)!=1 && (sampleFor(0x122)->bytes[0]&3)!=1 &&
    (sampleFor(0x124)->bytes[0]&3)!=1));
}
bool driveAppliedReverse() { return (sampleFor(0x124)->bytes[0]&0x80)!=0; }
bool driveManualMode() {
  return driveCapable() && (sampleFor(0x121)->bytes[0]&3)==0 &&
    (sampleFor(0x122)->bytes[0]&3)==0 && (sampleFor(0x124)->bytes[0]&3)==0;
}
bool driveCanShift() {
  return !drive.held && driveReady() && le16(sampleFor(0x124)->bytes+1)<=DRIVE_REST_PWM;
}
bool sendDriveGate(uint8_t op) {
  uint8_t data[8]={op,(uint8_t)(drive.wireReverse | 2),0,0,0,0,0,0};
  driveWrite32(data+2,drive.session); ++drive.gateSeq;
  data[6]=drive.gateSeq; data[7]=drive.gateSeq>>8;
  return sendLoggedCan(DRIVE_GATE_ID,data);
}
bool sendDrivePWM(uint8_t pwm) {
  const uint16_t seq=++drive.pwmSeq;
  uint8_t data[8]={pwm,(uint8_t)drive.wireReverse,(uint8_t)seq,(uint8_t)(seq>>8),0,0,0,0};
  driveWrite32(data+4,drive.session);
  return sendLoggedCan(DRIVE_PWM_ID,data);
}
void beginDrive(bool steeringOwned,bool reverse,bool shiftOnly=false) {
  drive.completedShiftToken=0;
  drive.pwmMode=true; drive.steeringOwned=steeringOwned; drive.shiftOnly=shiftOnly;
  drive.session=drive.token; drive.gateSeq=drive.pwmSeq=0; drive.pwm=drive.percent=0;
  drive.started=millis(); drive.paired=false; drive.held=true; drive.reason="";
  drive.wireReverse=driveAppliedReverse();
  drive.reverse=reverse; drive.directionSet=true; drive.lastTx=millis()-50;
  drive.shiftPhase=1; drive.shiftSent=false;
}
void stopDrive(const char *reason="") {
  const bool owned=drive.paired || driveGateAt(0x121,1) || driveGateAt(0x121,2);
  if(drive.held) drive.stoppedAt=millis();
  drive.held=false; drive.paired=false; drive.percent=0; drive.pwm=0; drive.adc=0; drive.reason=reason;
  drive.shiftOnly=false; drive.shiftPhase=0; drive.shiftSent=false;
  if(++drive.token==0) ++drive.token;
  if(drive.session) { if(owned) sendDrivePWM(0); sendDriveGate(0); }
  if(drive.steeringOwned) { stopSteeringRelay(); joystickSteerStopping=false; }
}
void expireDriveCommand() {
  const uint32_t now=millis();
  if(drive.held && (now-drive.lastHttp>=DRIVE_LEASE_MS || now-drive.started>=DRIVE_HOLD_MS)) stopDrive("RELEASE & RETRY");
  if(drive.held && (!driveCapable() || (drive.steeringOwned && !steeringNodeReady()))) stopDrive("LINK FAULT");
}
void serviceDrive() {
  expireDriveCommand();
  const uint32_t now=millis();
  if(!drive.session || now-drive.lastTx<50) return;
  drive.lastTx=now;
  if(!drive.held) {
    const bool pedalNormal=fresh(sampleFor(0x121),250) && (sampleFor(0x121)->bytes[0]&3)==0;
    const bool switchesNormal=fresh(sampleFor(0x122),250) && (sampleFor(0x122)->bytes[0]&3)==0;
    const bool motorNormal=fresh(sampleFor(0x124),250) && (sampleFor(0x124)->bytes[0]&3)==0;
    if(pedalNormal && switchesNormal && motorNormal) { drive.session=0; return; }
    if(!motorNormal) sendDrivePWM(0);
    sendDriveGate(0);
    if(drivePedalIdle() && bool(sampleFor(0x122)->bytes[1]&SWITCH_REVERSE)==drive.wireReverse) {
      if(driveGateAt(0x122,2)) sendDriveGate(2);
      else if(driveGateAt(0x122,0)) sendDriveGate(3);
    }
    return;
  }
  const bool paired=driveGateAt(0x121,1) && driveGateAt(0x122,1) && driveGateAt(0x124,1);
  if(drive.paired && !paired) { stopDrive("HANDOFF LOST"); return; }
  if(!paired && now-drive.started>=600) { stopDrive("HANDOFF FAILED"); return; }
  if(paired && !drive.paired) { drive.paired=true; drive.shiftPhase=1; drive.shiftSent=false; }
  const Sample *motor=sampleFor(0x124), *output=sampleFor(0x201);
  // A direction request must first get a NEW applied-idle acknowledgement.
  if(!drive.shiftPhase && drive.wireReverse!=drive.reverse) { drive.shiftPhase=1; drive.shiftSent=false; }
  const bool newIdleAck=paired && drive.shiftSent && motor->received!=drive.shiftAckCount &&
    le16(motor->bytes+1)<=DRIVE_REST_PWM && driveAppliedReverse()==drive.wireReverse;
  if(newIdleAck) {
    if(drive.wireReverse!=drive.reverse) {
      drive.wireReverse=drive.reverse; drive.shiftPhase=2; drive.shiftSent=false;
    } else drive.shiftPhase=0;
  }
  if(!sendDriveGate(1)) { stopDrive("CAN SEND FAILED"); return; }
  if(!paired) return;
  const bool ready=output->bytes[7]==0 && bool(output->bytes[2])==drive.wireReverse &&
    driveAppliedReverse()==drive.wireReverse;
  const uint8_t demand=!drive.shiftPhase && ready ? drive.pwm : 0;
  if(drive.shiftPhase && !drive.shiftSent) {
    drive.shiftAckCount=motor->received; drive.shiftSent=true;
  }
  if(!sendDrivePWM(demand)) { stopDrive("CAN SEND FAILED"); return; }
  // The reverse button renews a zero-demand transaction until the relay ACK.
  if(drive.shiftOnly && !drive.shiftPhase && ready) {
    drive.completedShiftToken=drive.token; drive.completedReverse=drive.reverse; stopDrive();
  }
}
void sendDriveState() {
  readCan(); serviceDrive();
  const bool capable=driveCapable();
  if(!drive.held && driveManualMode()) drive.reverse=driveAppliedReverse();
  const char *message=drive.reason;
  if(!capable) message="WAITING FOR CONTROL NODES";
  else if(!drive.held && !drivePedalIdle()) message="RELEASE PEDAL";
  else if(drive.held && !drive.paired) message="TAKING REMOTE CONTROL";
  else if(drive.held && drive.shiftPhase) message="WAITING FOR IDLE / DIRECTION ACK";
  else if(!drive.held && drive.session && bool(sampleFor(0x122)->bytes[1]&SWITCH_REVERSE)!=drive.wireReverse) message="MATCH REVERSE SWITCH FOR MANUAL";
  String j="{\"ready\":"+String(driveReady()?"true":"false")+",\"token\":"+String(drive.token);
  j+=",\"held\":"+String(drive.held?"true":"false")+",\"adc\":0,\"percent\":"+String(drive.percent);
  j+=",\"throttlePwm\":"+String(drive.pwm)+",\"appliedThrottlePwm\":"+maybeNumber(capable,le16(sampleFor(0x124)->bytes+1));
  j+=",\"reverse\":"+String(drive.reverse?"true":"false")+",\"appliedReverse\":"+(capable?String(driveAppliedReverse()?"true":"false"):"null");
  j+=",\"shifting\":"+String(drive.held && drive.shiftPhase?"true":"false")+",\"canShift\":"+String(driveCanShift()?"true":"false");
  j+=",\"message\":\""+String(message)+"\",\"timeoutMs\":350,\"holdMs\":5000}";
  server.sendHeader("Cache-Control","no-store"); server.send(200,"application/json",j);
}
void handleDriveCommand() {
  server.sendHeader("Cache-Control","no-store");
  if(server.header("X-SRT-Control")!="1" || (server.hasHeader("Origin") && server.header("Origin")!="http://"+server.hostHeader())) {
    server.send(403,"application/json","{\"error\":\"Same-origin control required\"}"); return;
  }
  readCan(); expireDriveCommand();
  const String action=server.arg("action");
  if(action=="stop") { stopDrive(); sendDriveState(); return; }
  uint32_t token=0,seq=0,percent=0,reverse=0;
  if(!steeringDecimal(server.arg("token"),token) || !steeringDecimal(server.arg("seq"),seq) || !seq || seq>65535 ||
    (action!="throttle" && action!="reverse") ||
    (action=="throttle" && (!steeringDecimal(server.arg("percent"),percent) || percent<1 || percent>100)) ||
    (action=="reverse" && (!steeringDecimal(server.arg("reverse"),reverse) || reverse>1))) {
    stopDrive("INVALID COMMAND"); server.send(400,"application/json","{\"error\":\"Invalid drive command\"}"); return;
  }
  const bool shifting=action=="reverse";
  if(shifting && !drive.held && token==drive.completedShiftToken && bool(reverse)==drive.completedReverse &&
      driveCapable() && driveAppliedReverse()==bool(reverse)) { sendDriveState(); return; }
  if(token!=drive.token || !driveReady() || (drive.held &&
      (drive.steeringOwned || drive.shiftOnly!=shifting || seq<=drive.httpSeq)) ||
      (!drive.held && shifting && !driveCanShift())) {
    stopDrive("RELEASE & RETRY"); server.send(409,"application/json","{\"error\":\"Release and retry\"}"); return;
  }
  if(!drive.held) {
    // A button throttle request carries no direction. After manual handback,
    // use the latest applied relay direction even if the browser's last poll
    // preceded a physical switch change. A locked remote choice stays latched.
    const bool direction=shifting ? bool(reverse) : driveManualMode() ? driveAppliedReverse() : drive.reverse;
    beginDrive(false,direction,shifting);
  }
  else if(shifting && drive.reverse!=bool(reverse)) {
    stopDrive("RELEASE & RETRY"); server.send(409,"application/json","{\"error\":\"Direction changed during shift\"}"); return;
  }
  drive.percent=percent; drive.pwm=shifting?0:(percent*255+50)/100;
  drive.httpSeq=seq; drive.lastHttp=millis(); sendDriveState();
}

#include "JoystickHost.h"

void setup() {
  Serial.begin(115200);
  drive.token=esp_random(); if(!drive.token) drive.token=1;
  steeringRelay.token = esp_random(); if (!steeringRelay.token) steeringRelay.token = 1;
  fsReady = LittleFS.begin(false);
  if (!fsReady) Serial.println("LittleFS unavailable: upload data/");
  if (!canBegin()) Serial.println("CAN unavailable: check MCP2515 wiring/crystal");
  lastCanAttempt=millis();
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); WiFi.setSleep(false);
  const char *controlHeaders[] = {"X-SRT-Control", "Origin"};
  server.collectHeaders(controlHeaders, 2);
  server.on("/api/drive", HTTP_GET, sendDriveState);
  server.on("/api/drive", HTTP_POST, handleDriveCommand);
  server.on("/api/joystick", HTTP_GET, sendJoystickState);
  server.on("/api/joystick", HTTP_POST, handleJoystickCommand);
  server.on("/api/steering", HTTP_GET, sendSteeringState);
  server.on("/api/steering", HTTP_POST, handleSteeringCommand);
  server.on("/api/data", sendSpeed); server.on("/api/gps", sendGps);
  server.on("/api/vehicle", sendVehicle); server.on("/api/can-log", sendCanLog);
  server.on("/api/diagnostics", sendDiagnostics);
  server.on("/status", sendDashboard); server.on("/status/", sendDashboard);
  server.on("/control", sendDashboard); server.on("/control/", sendDashboard);
  server.on("/controls", sendDashboard); server.on("/controls/", sendDashboard);
  server.onNotFound([](){
    if (server.uri().startsWith("/api/")) { server.send(404,"application/json","{\"error\":\"unknown API\"}"); return; }
    if (sendFile(server.uri())) return;
    if (server.uri().startsWith("/tiles/")) { server.send(404,"text/plain","no tile"); return; }
    if (!sendFile("/index.html")) server.send(404,"text/plain","website missing: upload LittleFS data");
  });
  server.begin(); Serial.print("Dashboard: http://"); Serial.println(WiFi.softAPIP());
}
void loop() {
  if (!canReady && millis()-lastCanAttempt>=1000) { canBegin(); lastCanAttempt=millis(); }
  readCan(); serviceDrive(); serviceSteeringRelay(); server.handleClient(); readCan(); serviceDrive(); serviceSteeringRelay();
  if (millis()-lastBeat >= 1000) { lastBeat=millis(); canPrintStatus("Node 5 dashboard");
    Serial.printf("Dashboard: WiFi clients=%u LittleFS=%s buffered frames=%u total frames=%lu | Node 1=%s Node 3=%s | Node 2 status: inspect 0x201/0x600\n",
                  WiFi.softAPgetStationNum(), fsReady ? "READY" : "MISSING", logCount, (unsigned long)frameCount,
                  fresh(sampleFor(0x301), 501) ? "LIVE" : "NO RECENT DATA", fresh(sampleFor(0x300), 3001) ? "LIVE" : "NO RECENT DATA");
    canHeartbeat(5, canReady?1:0, canReady?0:1); }
}
