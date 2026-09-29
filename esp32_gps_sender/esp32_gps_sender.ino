/*
 * ESP32 #2 - GPS SENDER  (GPS -> CAN bus)
 *
 * Reads the GPS module and broadcasts the fix on the CAN bus every
 * second. ESP32 #1 receives it and puts it on the website.
 *
 * WIRING - GPS module
 *   GPS VCC -> 3V3      (5V can destroy a 3.3V-only module)
 *   GPS GND -> GND
 *   GPS TX  -> GPIO16   crossed! TX goes to RX
 *   (GPS RX is left unconnected - we never send it commands)
 *
 * WIRING - MCP2515 CAN module
 *   VCC -> 5V (or 3V3 if your board is a 3.3V version)
 *   GND -> GND
 *   SCK -> GPIO18
 *   SI  -> GPIO23
 *   SO  -> GPIO19
 *   CS  -> GPIO25
 *   INT -> GPIO27       (not used here, but wire it anyway)
 *
 * WIRING - the bus itself (this is what people forget)
 *   CANH on this module -> CANH on ESP32 #1's module
 *   CANL on this module -> CANL on ESP32 #1's module
 *   A 120 ohm resistor across CANH/CANL at EACH end. Most MCP2515
 *   boards have one fitted already - with only two nodes, that is
 *   exactly right. Three or more nodes: only the two END nodes keep it.
 *   Also join the two GNDs.
 *
 * NOTE ON THE CRYSTAL
 *   Look at the silver crystal on the MCP2515 board: it says 8.000 or
 *   16.000. Set CAN_CRYSTAL below to match, or nothing will be
 *   received. Most cheap red boards are 8 MHz.
 */

#include <TinyGPSPlus.h>
#include <SPI.h>
#include <mcp_can.h>

/* ---------------- GPS ---------------- */

#define GPS_RX 16
#define GPS_TX -1          /* receive only - we never talk to the module */
#define GPS_BAUD 9600

/* ---------------- CAN ---------------- */

#define CAN_CS 25
#define CAN_INT 27
#define CAN_SPEED CAN_500KBPS

/* MUST match the crystal printed on your MCP2515 board. */
#define CAN_CRYSTAL MCP_8MHZ

/*
 * A CAN frame holds only 8 bytes, and the GPS record is far bigger, so
 * it is split across three IDs instead of being sent as JSON text.
 * ESP32 #1 reassembles them.
 *
 *   0x100  lat (int32, deg * 1e7) + lon (int32, deg * 1e7)
 *   0x101  speed (uint16, km/h * 10) + course (uint16, deg * 10)
 *          + sats (uint8) + fix (uint8) + hdop (uint16, * 100)
 *   0x102  heartbeat, a counter so #1 can tell "no fix" from "no wire"
 */
#define CAN_ID_POS 0x100
#define CAN_ID_INFO 0x101
#define CAN_ID_BEAT 0x102

/* ---------------- timing ---------------- */

#define SEND_EVERY_MS 1000
#define PRINT_EVERY_MS 2000

/*
 * How stale a position may be and still be sent.
 *
 * TinyGPSPlus only refreshes the age when a sentence arrives WITH the
 * fix flag set. Under a weak sky the module keeps streaming sentences
 * but clears that flag, so a short limit throws away a position that is
 * still perfectly usable.
 */
#define FIX_MAX_AGE_MS 30000

/* Below this speed the module's course reading is meaningless. */
#define MIN_COURSE_SPEED 2.0

/* ---------------- anti-jitter ----------------
 *
 * A stationary consumer GPS does not sit still. Its reported position
 * wanders a few metres as satellites drift and the atmosphere changes,
 * so the map marker twitches and the trail fuzzes while parked.
 *
 * Three cheap filters fix that, in order of how much they help:
 *
 *  1. Deadband. Ignore any move smaller than JITTER_M. Real driving
 *     easily clears it; receiver noise almost never does.
 *  2. Smoothing. Once moving, blend each new point with the previous
 *     one instead of jumping straight to it.
 *  3. HDOP gate. Above HDOP_MAX the fix geometry is too poor to trust,
 *     which is exactly when the wild outliers arrive.
 */

/* Hold position until the receiver claims we moved at least this far. */
#define JITTER_M 5.0

/* Parked below this speed, so apply the deadband. */
#define STILL_SPEED 1.5

/*
 * Smoothing strength: 0 = never move, 1 = follow the raw fix exactly.
 *
 * Two values, because one number cannot serve both cases. Heavy
 * smoothing is what kills the parked twitch, but applied while driving
 * it leaves the marker trailing metres behind the car (measured: 0.35
 * alone put it 27 m back at 54 km/h). So smooth hard when crawling and
 * barely at all once genuinely moving.
 */
#define SMOOTH_STILL 0.35
#define SMOOTH_MOVING 0.80

/* Reject fixes with worse geometry than this (2 good, 5 weak, 10 bad). */
#define HDOP_MAX 5.0

TinyGPSPlus gps;
HardwareSerial GPS(2);
MCP_CAN CAN(CAN_CS);

bool canReady = false;
bool showRaw = false;
unsigned long lastSend = 0;
unsigned long lastPrint = 0;
unsigned long beat = 0;
unsigned long sentOk = 0;
unsigned long sentFail = 0;

/* Position we are willing to send. */
bool haveFix() {
  return gps.location.isValid() && gps.location.age() < FIX_MAX_AGE_MS;
}

/* The filtered position actually broadcast, and its raw counterpart. */
double outLat = 0, outLon = 0;
bool outValid = false;
double lastRawLat = 0, lastRawLon = 0;
unsigned long heldCount = 0;

/*
 * Metres between two nearby points.
 *
 * Over the few metres that matter here the curvature of the Earth is
 * irrelevant, so flat-Earth maths is both accurate enough and far
 * cheaper than a haversine on every fix.
 */
double metresBetween(double aLat, double aLon, double bLat, double bLon) {
  double dLat = (bLat - aLat) * 111320.0;
  double dLon = (bLon - aLon) * 111320.0 * cos(aLat * PI / 180.0);
  return sqrt(dLat * dLat + dLon * dLon);
}

/*
 * Decide what position to broadcast this second.
 *
 * Called once per send so the deadband is judged against what we last
 * actually transmitted, not against every intermediate reading.
 */
void updateFiltered() {
  if (!haveFix()) {
    outValid = false;
    return;
  }

  double rawLat = gps.location.lat();
  double rawLon = gps.location.lng();

  lastRawLat = rawLat;
  lastRawLon = rawLon;

  /* Poor geometry produces the big wild jumps - ignore those fixes and
   * keep showing the last good one. */
  if (gps.hdop.isValid() && gps.hdop.hdop() > HDOP_MAX) {
    heldCount++;
    return;
  }

  /* First good fix: take it as-is, nothing to smooth against. */
  if (!outValid) {
    outLat = rawLat;
    outLon = rawLon;
    outValid = true;
    return;
  }

  double moved = metresBetween(outLat, outLon, rawLat, rawLon);

  /* Parked and the move is within the noise floor: stay put. */
  bool moving = gps.speed.isValid() && gps.speed.kmph() >= STILL_SPEED;

  if (!moving && moved < JITTER_M) {
    heldCount++;
    return;
  }

  /* Ease toward the new point rather than snapping to it, so a single
   * noisy reading cannot yank the marker. Light touch while moving, so
   * the marker still tracks the car closely. */
  double k = moving ? SMOOTH_MOVING : SMOOTH_STILL;

  outLat += (rawLat - outLat) * k;
  outLon += (rawLon - outLon) * k;
}

/* Pack a signed 32-bit value little-endian. */
void put32(INT8U *b, int32_t v) {
  b[0] = v & 0xFF;
  b[1] = (v >> 8) & 0xFF;
  b[2] = (v >> 16) & 0xFF;
  b[3] = (v >> 24) & 0xFF;
}

/* Pack an unsigned 16-bit value little-endian. */
void put16(INT8U *b, uint16_t v) {
  b[0] = v & 0xFF;
  b[1] = (v >> 8) & 0xFF;
}

/* Send one frame and count the result. */
void sendFrame(INT32U id, INT8U *buf) {
  if (CAN.sendMsgBuf(id, 0, 8, buf) == CAN_OK) sentOk++;
  else sentFail++;
}

/*
 * Broadcast the current fix as three CAN frames.
 *
 * Scaling everything to integers keeps each field an exact width and
 * avoids sending floats, whose byte layout would have to match on both
 * boards. 1e7 on lat/lon keeps about 1 cm of precision.
 */
void sendCan() {
  updateFiltered();

  bool fix = haveFix() && outValid;
  INT8U buf[8];

  /* --- 0x100 : position --- (filtered, not raw) */
  int32_t lat = fix ? (int32_t)(outLat * 1e7) : 0;
  int32_t lon = fix ? (int32_t)(outLon * 1e7) : 0;
  put32(buf, lat);
  put32(buf + 4, lon);
  sendFrame(CAN_ID_POS, buf);

  /* --- 0x101 : speed, course, sats, fix, hdop --- */
  uint16_t spd = (fix && gps.speed.isValid())
                   ? (uint16_t)(gps.speed.kmph() * 10) : 0;

  /*
   * Course is sent as 0xFFFF ("unknown") rather than 0 when the module
   * is stationary. A u-blox leaves the RMC course field empty while
   * parked, and a real 0 would read as due north and pin the map
   * marker there. ESP32 #1 omits the field when it sees 0xFFFF, and the
   * website then uses its own track bearing instead.
   */
  uint16_t crs = 0xFFFF;
  if (fix && gps.course.isValid() && gps.speed.isValid() &&
      gps.speed.kmph() >= MIN_COURSE_SPEED)
    crs = (uint16_t)(gps.course.deg() * 10);

  uint16_t hdop = gps.hdop.isValid() ? (uint16_t)(gps.hdop.hdop() * 100) : 0;

  put16(buf, spd);
  put16(buf + 2, crs);
  buf[4] = gps.satellites.isValid() ? (INT8U)gps.satellites.value() : 0;
  buf[5] = fix ? 1 : 0;
  put16(buf + 6, hdop);
  sendFrame(CAN_ID_INFO, buf);

  /* --- 0x102 : heartbeat --- */
  beat++;
  put32(buf, (int32_t)beat);
  put32(buf + 4, (int32_t)(millis() / 1000));
  sendFrame(CAN_ID_BEAT, buf);
}

/* Everything being put on the bus, in readable form. */
void printStatus() {
  bool fix = haveFix() && outValid;

  Serial.println();
  Serial.println("---------- GPS -> CAN ----------");

  Serial.print("CAN module      : ");
  Serial.println(canReady ? "ready" : "NOT FOUND - check wiring/crystal");

  Serial.print("frames sent ok  : ");
  Serial.print(sentOk);
  Serial.print("   failed: ");
  Serial.println(sentFail);

  if (sentFail > 0 && sentOk == 0)
    Serial.println("  -> nothing is being accepted: no 120 ohm "
                   "termination, or CANH/CANL swapped");

  Serial.print("chars received  : ");
  Serial.println(gps.charsProcessed());

  Serial.print("checksum errors : ");
  Serial.println(gps.failedChecksum());

  Serial.print("satellites      : ");
  Serial.println(gps.satellites.isValid() ? gps.satellites.value() : 0);

  Serial.print("location valid  : ");
  Serial.println(gps.location.isValid() ? "yes" : "no");

  if (gps.location.isValid()) {
    Serial.print("age of fix      : ");
    Serial.print(gps.location.age());
    Serial.println(" ms");
  }

  Serial.print("FIX             : ");
  Serial.println(fix ? "YES" : "NO  (30-90 s outdoors)");

  if (fix) {
    Serial.print("raw position    : ");
    Serial.print(gps.location.lat(), 6);
    Serial.print(", ");
    Serial.println(gps.location.lng(), 6);

    Serial.print("sent (filtered) : ");
    Serial.print(outLat, 6);
    Serial.print(", ");
    Serial.println(outLon, 6);

    Serial.print("raw vs sent     : ");
    Serial.print(metresBetween(outLat, outLon,
                               gps.location.lat(), gps.location.lng()), 2);
    Serial.println(" m  (jitter being absorbed)");
  }

  Serial.print("held still      : ");
  Serial.print(heldCount);
  Serial.println(" times (noise rejected)");

  if (gps.speed.isValid()) {
    Serial.print("speed           : ");
    Serial.print(gps.speed.kmph(), 1);
    Serial.println(" km/h");
  }

  if (gps.course.isValid()) {
    Serial.print("course          : ");
    Serial.print(gps.course.deg(), 1);
    Serial.println(" deg");
  }

  if (gps.hdop.isValid()) {
    Serial.print("HDOP            : ");
    Serial.println(gps.hdop.hdop(), 2);
  }

  Serial.print("heartbeat       : ");
  Serial.println(beat);

  /* The exact bytes just put on the wire. */
  int32_t lat = fix ? (int32_t)(gps.location.lat() * 1e7) : 0;
  int32_t lon = fix ? (int32_t)(gps.location.lng() * 1e7) : 0;
  uint16_t spd = (fix && gps.speed.isValid())
                   ? (uint16_t)(gps.speed.kmph() * 10) : 0;
  uint16_t crs = 0xFFFF;
  if (fix && gps.course.isValid() && gps.speed.isValid() &&
      gps.speed.kmph() >= MIN_COURSE_SPEED)
    crs = (uint16_t)(gps.course.deg() * 10);

  Serial.println("--- frames on the bus ---");
  Serial.printf("  0x100 pos  : lat=%ld lon=%ld\n", (long)lat, (long)lon);
  Serial.printf("  0x101 info : speed=%u course=%s sats=%u fix=%u\n",
                spd,
                crs == 0xFFFF ? "none" : String(crs).c_str(),
                (unsigned)(gps.satellites.isValid()
                             ? gps.satellites.value() : 0),
                (unsigned)(fix ? 1 : 0));
  Serial.printf("  0x102 beat : %lu\n", beat);

  /* What ESP32 #1 will rebuild and hand to the website. */
  Serial.print("website sees    : ");
  Serial.print("{\"lat\":");
  Serial.print(fix ? gps.location.lat() : 0.0, 6);
  Serial.print(",\"lon\":");
  Serial.print(fix ? gps.location.lng() : 0.0, 6);
  Serial.print(",\"sats\":");
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
  Serial.print(",\"fix\":");
  Serial.print(fix ? "true" : "false");
  Serial.println("}");

  Serial.println("--------------------------------");
}

void setup() {
  Serial.begin(115200);
  GPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  pinMode(CAN_INT, INPUT);

  Serial.println();
  Serial.println("================================");
  Serial.println("   ESP32 #2 - GPS -> CAN SENDER");
  Serial.println("================================");

  /* Keep retrying rather than freezing, so the GPS output is still
   * usable while the CAN wiring is being sorted out. */
  for (int i = 0; i < 5 && !canReady; i++) {
    if (CAN.begin(MCP_ANY, CAN_SPEED, CAN_CRYSTAL) == CAN_OK) {
      canReady = true;
    } else {
      Serial.println("CAN init failed - check CS pin and crystal setting");
      delay(400);
    }
  }

  if (canReady) {
    CAN.setMode(MCP_NORMAL);      /* out of loopback, onto the real bus */
    Serial.println("CAN ready at 500 kbps");
  } else {
    Serial.println("CAN NOT AVAILABLE - still printing GPS below");
  }

  Serial.println("Send any character to toggle raw NMEA.");
}

void loop() {
  /* Feed every byte from the module into the parser. */
  while (GPS.available()) {
    char c = GPS.read();
    gps.encode(c);
    if (showRaw) Serial.write(c);
  }

  /* Any keypress toggles the raw NMEA dump. */
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    showRaw = !showRaw;
    Serial.println();
    Serial.println(showRaw ? ">>> raw NMEA ON" : ">>> raw NMEA OFF");
  }

  if (canReady && millis() - lastSend >= SEND_EVERY_MS) {
    lastSend = millis();
    sendCan();
  }

  if (millis() - lastPrint >= PRINT_EVERY_MS) {
    lastPrint = millis();
    printStatus();
  }
}
