/*
 * ESP32 #1 - WEB SERVER + CAN RECEIVER
 *
 * Hosts the dashboard website from flash on its own WiFi, and receives
 * the GPS fix from ESP32 #2 over the CAN bus.
 *
 * BEFORE UPLOADING - both of these matter:
 *   1. Tools > Partition Scheme > "No OTA (2MB APP/2MB SPIFFS)"
 *   2. Upload the website itself (Ctrl+Shift+P > Upload LittleFS)
 *
 * WIRING - MCP2515 CAN module
 *   VCC -> 5V (or 3V3 if your board is a 3.3V version)
 *   GND -> GND
 *   SCK -> GPIO18
 *   SI  -> GPIO23
 *   SO  -> GPIO19
 *   CS  -> GPIO25
 *   INT -> GPIO27
 *   SPI/INT must use 3.3V logic; level-shift a 5V-logic module.
 *
 * WIRING - the bus itself
 *   CANH -> CANH on ESP32 #2's module
 *   CANL -> CANL on ESP32 #2's module
 *   120 ohm across CANH/CANL at each end (most boards have one fitted),
 *   and join the two GNDs.
 *
 * WIRING - potentiometer (speed dial)
 *   outer leg -> 3V3
 *   wiper     -> GPIO34    must be 32-39; other pins read 0 with WiFi on
 *   outer leg -> GND
 *
 * THEN: connect to WiFi "srt-dash" and open http://192.168.4.1
 */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <mcp_can.h>

const char *AP_SSID = "srt-dash";
const char *AP_PASS = "12345678";

/* ---------------- CAN ---------------- */

#define CAN_CS 25
#define CAN_INT 27
#define CAN_SPEED CAN_500KBPS

/* MUST match the crystal printed on your MCP2515 board (8.000/16.000). */
#define CAN_CRYSTAL MCP_8MHZ

#define CAN_ID_POS 0x100
#define CAN_ID_INFO 0x101
#define CAN_ID_BEAT 0x102

/* ---------------- other ---------------- */

#define POT_PIN 34
#define MAX_SPEED 160.0

/* A fix older than this counts as lost. */
#define FIX_TIMEOUT_MS 5000

WebServer server(80);
MCP_CAN CAN(CAN_CS);

bool canReady = false;

/* Latest values decoded off the bus. */
int32_t rxLat = 0, rxLon = 0;
uint16_t rxSpeed = 0, rxCourse = 0xFFFF, rxHdop = 0;
uint8_t rxSats = 0, rxFix = 0;
uint32_t rxBeat = 0;

unsigned long gpsAt = 0;          /* when a position frame last arrived */
unsigned long gpsInfoAt = 0;      /* metadata must also keep arriving */
bool seenPosition = false, seenInfo = false;
unsigned long canFrames = 0;

/* Bounded RAM history for the dashboard's CAN LOG panel. */
const uint8_t CAN_LOG_CAPACITY = 64;
struct CanLogFrame {
  unsigned long seq, ms;
  uint32_t id;
  uint8_t dlc, data[8];
  bool extended, remote;
};
CanLogFrame canLog[CAN_LOG_CAPACITY];
uint8_t canLogNext = 0, canLogCount = 0;

void readCan();

/* For the one-line serial report. */
unsigned long lastReport = 0;
unsigned long lastReportFrames = 0;

#define REPORT_EVERY_MS 1000


/* --------------------------------------------------
   FILE SERVER
   -------------------------------------------------- */

const char *mimeFor(const String &p) {
  if (p.endsWith(".html")) return "text/html";
  if (p.endsWith(".js"))   return "application/javascript";
  if (p.endsWith(".css"))  return "text/css";
  if (p.endsWith(".png"))  return "image/png";
  if (p.endsWith(".svg"))  return "image/svg+xml";
  if (p.endsWith(".json")) return "application/json";
  if (p.endsWith(".ico"))  return "image/x-icon";

  return "text/plain";
}

bool sendFile(String path) {

  if (path.endsWith("/"))
    path += "index.html";

  String gz = path + ".gz";

  bool useGz = LittleFS.exists(gz);

  if (!useGz && !LittleFS.exists(path))
    return false;

  File f = LittleFS.open(useGz ? gz : path, "r");

  if (!f)
    return false;

  if (f.isDirectory()) {
    f.close();
    return false;
  }

  if (path.startsWith("/tiles/")) {
    server.sendHeader("Cache-Control", "max-age=31536000");
  } else {
    /* The app files change every time the site is rebuilt. Caching them
     * for a year means a rebuilt dashboard never reaches the browser,
     * which looks exactly like "the CSS stopped working". */
    server.sendHeader("Cache-Control", "no-cache");
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");

  /* LittleFS stores compressed app files as .gz, but the URL and MIME type
   * remain .js/.css. Tell the browser to decompress the response. */
  if (useGz) server.sendHeader("Content-Encoding", "gzip");

  /* Drain CAN between blocks while a large JS bundle is being served.
   * The MCP2515 has only two receive buffers. */
  server.setContentLength(f.size());
  server.send(200, mimeFor(path), "");
  char chunk[1024];
  while (f.available() && server.client().connected()) {
    readCan();
    size_t count = f.readBytes(chunk, sizeof(chunk));
    server.sendContent(chunk, count);
    yield();
  }
  readCan();

  f.close();

  return true;
}


/* --------------------------------------------------
   CAN RECEIVE
   -------------------------------------------------- */

int32_t get32(INT8U *b) {
  return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

uint16_t get16(INT8U *b) {
  return (uint16_t)(b[0] | (b[1] << 8));
}

/*
 * Drain every frame waiting in the controller.
 *
 * ESP32 #2 splits the fix across three IDs because a CAN frame only
 * holds 8 bytes; we decode whichever ones have arrived.
 */
void readCan() {
  if (!canReady) return;

  INT32U id;
  INT8U len;
  INT8U buf[8];

  while (CAN.checkReceive() == CAN_MSGAVAIL) {

    if (CAN.readMsgBuf(&id, &len, buf) != CAN_OK) break;

    canFrames++;
    CanLogFrame &frame = canLog[canLogNext];
    frame.seq = canFrames;
    frame.ms = millis();
    /* mcp_can encodes extended/RTR flags in the top two ID bits. */
    frame.id = id & 0x1FFFFFFFUL;
    frame.extended = (id & 0x80000000UL) != 0;
    frame.remote = (id & 0x40000000UL) != 0;
    frame.dlc = len > 8 ? 8 : len;
    memset(frame.data, 0, sizeof(frame.data));
    if (!frame.remote) memcpy(frame.data, buf, frame.dlc);
    canLogNext = (canLogNext + 1) % CAN_LOG_CAPACITY;
    if (canLogCount < CAN_LOG_CAPACITY) canLogCount++;

    if (id == CAN_ID_POS && len == 8) {
      rxLat = get32(buf);
      rxLon = get32(buf + 4);
      gpsAt = millis();
      seenPosition = true;

    } else if (id == CAN_ID_INFO && len == 8) {
      rxSpeed = get16(buf);
      rxCourse = get16(buf + 2);
      rxSats = buf[4];
      rxFix = buf[5];
      rxHdop = get16(buf + 6);
      gpsInfoAt = millis();
      seenInfo = true;

    } else if (id == CAN_ID_BEAT && len == 8) {
      rxBeat = (uint32_t)get32(buf);
    }
  }
}

/*
 * Rebuild the JSON the website expects.
 *
 * Field names must match what App.jsx reads. Course is omitted entirely
 * when the sender marked it unknown (0xFFFF) - sending 0 would read as
 * a real heading of due north.
 */
void buildJson(char *out, int size) {
  const unsigned long now = millis();
  const unsigned long positionAge = now - gpsAt;
  const unsigned long infoAge = now - gpsInfoAt;
  bool fix = seenPosition && seenInfo && rxFix == 1 &&
             positionAge < FIX_TIMEOUT_MS && infoAge < FIX_TIMEOUT_MS &&
             rxLat >= -900000000 && rxLat <= 900000000 &&
             rxLon >= -1800000000 && rxLon <= 1800000000 &&
             (rxLat != 0 || rxLon != 0);

  if (!fix) {
    snprintf(out, size, "{\"lat\":0,\"lon\":0,\"sats\":0,\"fix\":false}");
    return;
  }

  char hdop[24] = "", course[28] = "";
  if (rxHdop)
    snprintf(hdop, sizeof(hdop), ",\"hdop\":%.2f", rxHdop / 100.0);
  if (rxCourse < 3600)
    snprintf(course, sizeof(course), ",\"course\":%.1f", rxCourse / 10.0);

  /* One bounded write also remains safe if a caller supplies a small buffer. */
  snprintf(out, size,
           "{\"lat\":%.6f,\"lon\":%.6f,\"sats\":%u,\"speed\":%.1f%s,"
           "\"ageMs\":%lu%s,\"fix\":true}",
           rxLat / 1e7, rxLon / 1e7, rxSats, rxSpeed / 10.0, hdop,
           positionAge > infoAge ? positionAge : infoAge, course);
}

void handleCanLog() {
  String json;
  json.reserve(11000);
  json = "{\"ready\":";
  json += canReady ? "true" : "false";
  json += ",\"total\":" + String(canFrames) + ",\"frames\":[";
  const uint8_t first = (canLogNext + CAN_LOG_CAPACITY - canLogCount) % CAN_LOG_CAPACITY;
  for (uint8_t i = 0; i < canLogCount; i++) {
    const CanLogFrame &frame = canLog[(first + i) % CAN_LOG_CAPACITY];
    if (i) json += ',';
    json += "{\"seq\":" + String(frame.seq) + ",\"ms\":" + String(frame.ms) +
            ",\"id\":" + String(frame.id) + ",\"dlc\":" + String(frame.dlc) +
            ",\"extended\":" + (frame.extended ? "true" : "false") +
            ",\"remote\":" + (frame.remote ? "true" : "false") + ",\"data\":[";
    for (uint8_t b = 0; b < (frame.remote ? 0 : frame.dlc); b++) {
      if (b) json += ',';
      json += String(frame.data[b]);
    }
    json += "]}";
  }
  json += "]}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}


/* --------------------------------------------------
   SPEED API
   -------------------------------------------------- */

void handleData() {

  unsigned long sum = 0;

  for (int i = 0; i < 16; i++)
    sum += analogRead(POT_PIN);

  float speed = (sum / 16.0) / 4095.0 * MAX_SPEED;

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");

  server.send(200, "application/json",
              "{\"value\":" + String(speed, 1) + "}");
}


/* --------------------------------------------------
   GPS API
   -------------------------------------------------- */

void handleGps() {

  char json[160];
  buildJson(json, sizeof(json));

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");

  server.send(200, "application/json", json);
}


/* --------------------------------------------------
   STATUS
   -------------------------------------------------- */

void handleStatus() {

  char json[160];
  buildJson(json, sizeof(json));

  String s = "fs     : " + String(LittleFS.usedBytes() / 1024) + "/" +
             String(LittleFS.totalBytes() / 1024) + " KB\n";

  s += "can    : ";
  s += canReady ? "ready" : "NOT FOUND";
  s += "\nframes : " + String(canFrames);
  s += "\nbeat   : " + String(rxBeat);
  s += "\nfix    : ";
  s += seenPosition ? String((millis() - gpsAt) / 1000) + "s ago" : "never";
  s += "\njson   : ";
  s += json;
  s += "\n";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", s);
}


/* --------------------------------------------------
   NOT FOUND
   -------------------------------------------------- */

void handleNotFound() {

  if (server.uri().startsWith("/api/")) {
    server.send(404, "application/json", "{\"error\":\"unknown API\"}");
    return;
  }

  if (sendFile(server.uri())) return;

  /* A missing tile is normal - the site draws a blank square there. */
  if (server.uri().startsWith("/tiles/")) {
    server.send(404, "text/plain", "no tile");
    return;
  }

  if (server.uri().startsWith("/assets/")) {
    server.send(404, "text/plain", "asset missing - rebuild and upload data");
    return;
  }

  if (!sendFile("/index.html"))
    server.send(404, "text/plain",
                "website missing - upload the data folder");
}


/* --------------------------------------------------
   SETUP
   -------------------------------------------------- */

void setup() {
  Serial.begin(115200);

  pinMode(CAN_INT, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation((gpio_num_t)POT_PIN, ADC_11db);

  /* false = do NOT format on failure; that would erase the website. */
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS failed!");
    Serial.println("Set Partition Scheme to No OTA, then upload data.");
  } else {
    Serial.printf("LittleFS %u/%u KB\n", LittleFS.usedBytes() / 1024,
                  LittleFS.totalBytes() / 1024);
  }

  for (int i = 0; i < 5 && !canReady; i++) {
    if (CAN.begin(MCP_ANY, CAN_SPEED, CAN_CRYSTAL) == CAN_OK) {
      canReady = true;
    } else {
      Serial.println("CAN init failed - check CS pin and crystal setting");
      delay(400);
    }
  }

  if (canReady) {
    CAN.setMode(MCP_NORMAL);
    Serial.println("CAN ready at 500 kbps");
  } else {
    Serial.println("CAN NOT AVAILABLE - website will show no fix");
  }

  /* Access point only - no internet out here. */
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);     /* SoftAP drops clients when the radio sleeps */

  Serial.print("WiFi \"");
  Serial.print(AP_SSID);
  Serial.print("\"  ->  http://");
  Serial.println(WiFi.softAPIP());

  server.on("/api/data", handleData);
  server.on("/api/gps", handleGps);
  server.on("/api/can-log", handleCanLog);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
}

/*
 * One line a second: are frames arriving, and do we have a fix?
 *
 * "CAN: yes" means new frames landed since the last line - so the bus
 * is live right now, not merely that something arrived once at boot.
 */
void report() {
  bool receiving = canFrames > lastReportFrames;
  lastReportFrames = canFrames;

  bool fresh = seenPosition && seenInfo &&
               millis() - gpsAt < FIX_TIMEOUT_MS &&
               millis() - gpsInfoAt < FIX_TIMEOUT_MS;

  Serial.print("CAN: ");
  Serial.print(receiving ? "yes" : "no ");
  Serial.print("   fix: ");
  Serial.println((fresh && rxFix) ? "true" : "false");
}

void loop() {
  readCan();
  server.handleClient();

  if (millis() - lastReport >= REPORT_EVERY_MS) {
    lastReport = millis();
    report();
  }
}
