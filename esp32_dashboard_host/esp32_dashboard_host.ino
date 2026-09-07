
/*
 * ESP32 #1 - WEB SERVER + GPS RECEIVER
 *
 * ESP32 #2 GPIO17 (TX) -> ESP32 #1 GPIO16 (RX)
 * ESP32 #2 GND         -> ESP32 #1 GND
 */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

const char *AP_SSID = "srt-dash";
const char *AP_PASS = "srt-dash-2026";

/* ESP32 #2 -> ESP32 #1 */
#define LINK_RX 16
#define LINK_TX -1
#define LINK_BAUD 115200

/* Potentiometer */
#define POT_PIN 34
#define MAX_SPEED 160.0

/* GPS timeout */
#define FIX_TIMEOUT_MS 35000

const char *NO_FIX =
  "{\"lat\":0,\"lon\":0,\"sats\":0,\"fix\":false}";

WebServer server(80);
HardwareSerial gpsLink(2);

char gpsCache[160];
unsigned long gpsAt = 0;

char rxBuf[160];
int rxLen = 0;


/* --------------------------------------------------
   FILE SERVER
   -------------------------------------------------- */

const char *mimeFor(const String &p) {
  if (p.endsWith(".html")) return "text/html";
  if (p.endsWith(".js"))   return "application/javascript";
  if (p.endsWith(".css"))  return "text/css";
  if (p.endsWith(".png"))  return "image/png";
  if (p.endsWith(".svg"))  return "image/svg+xml";

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

  if (path.startsWith("/assets/") ||
      path.startsWith("/tiles/")) {

    server.sendHeader(
      "Cache-Control",
      "max-age=31536000"
    );
  }

  server.streamFile(f, mimeFor(path));

  f.close();

  return true;
}


/* --------------------------------------------------
   RECEIVE GPS JSON FROM ESP32 #2
   -------------------------------------------------- */

void readLink() {

  while (gpsLink.available()) {

    char c = gpsLink.read();

    if (c == '\r')
      continue;

    if (c != '\n') {

      if (rxLen < sizeof(rxBuf) - 1) {

        rxBuf[rxLen++] = c;

      } else {

        /* Line too long */
        rxLen = 0;
      }

      continue;
    }


    /* End of line */

    rxBuf[rxLen] = '\0';


    if (rxLen > 2 &&
        rxBuf[0] == '{' &&
        rxBuf[rxLen - 1] == '}' &&
        strstr(rxBuf, "\"fix\"")) {

      strcpy(gpsCache, rxBuf);

      gpsAt = millis();


      /* Do not print every fix: at 200 Hz serial logging blocks reception. */
    }


    rxLen = 0;
  }
}


/* --------------------------------------------------
   SPEED API
   -------------------------------------------------- */

void handleData() {

  unsigned long sum = 0;

  for (int i = 0; i < 16; i++)
    sum += analogRead(POT_PIN);


  float speed =
    (sum / 16.0) /
    4095.0 *
    MAX_SPEED;


  server.sendHeader(
    "Access-Control-Allow-Origin",
    "*"
  );
  server.sendHeader(
    "Cache-Control",
    "no-store, no-cache, must-revalidate"
  );

  server.send(
    200,
    "application/json",
    "{\"value\":" +
    String(speed, 1) +
    "}"
  );
}


/* --------------------------------------------------
   GPS API
   -------------------------------------------------- */

void handleGps() {

  bool fresh =
    gpsAt &&
    (millis() - gpsAt < FIX_TIMEOUT_MS);


  server.sendHeader(
    "Access-Control-Allow-Origin",
    "*"
  );
  server.sendHeader(
    "Cache-Control",
    "no-store, no-cache, must-revalidate"
  );


  if (fresh) {

    server.send(
      200,
      "application/json",
      gpsCache
    );

  } else {

    server.send(
      200,
      "application/json",
      NO_FIX
    );
  }
}


/* --------------------------------------------------
   STATUS
   -------------------------------------------------- */

void handleStatus() {

  String s =
    "fs   : " +
    String(LittleFS.usedBytes() / 1024) +
    "/" +
    String(LittleFS.totalBytes() / 1024) +
    " KB\n";


  s += "fix  : ";

  if (gpsAt)
    s += String((millis() - gpsAt) / 1000) +
         "s ago";
  else
    s += "never";


  s += "\ndata : ";
  s += gpsCache;
  s += "\n";


  server.send(
    200,
    "text/plain",
    s
  );
}


/* --------------------------------------------------
   FILE FALLBACK
   -------------------------------------------------- */

void handleNotFound() {

  if (sendFile(server.uri()))
    return;


  if (server.uri().startsWith("/tiles/")) {

    server.send(
      404,
      "text/plain",
      "no tile"
    );

    return;
  }


  if (!sendFile("/index.html")) {

    server.send(
      404,
      "text/plain",
      "website missing - upload the data folder"
    );
  }
}


/* --------------------------------------------------
   SETUP
   -------------------------------------------------- */

void setup() {

  Serial.begin(115200);

  strcpy(
    gpsCache,
    NO_FIX
  );


  /* ESP32 #2 GPIO17 -> ESP32 #1 GPIO16 */

  gpsLink.begin(
    LINK_BAUD,
    SERIAL_8N1,
    LINK_RX,
    LINK_TX
  );


  /* Potentiometer */

  analogSetPinAttenuation(
    (gpio_num_t)POT_PIN,
    ADC_11db
  );


  /* LittleFS */

  if (!LittleFS.begin(false)) {

    Serial.println(
      "LittleFS failed!"
    );

  } else {

    Serial.printf(
      "LittleFS %u/%u KB\n",
      LittleFS.usedBytes() / 1024,
      LittleFS.totalBytes() / 1024
    );
  }


  /* WiFi Access Point */

  WiFi.mode(WIFI_AP);

  WiFi.softAP(
    AP_SSID,
    AP_PASS
  );

  WiFi.setSleep(false);


  Serial.print("WiFi \"");
  Serial.print(AP_SSID);
  Serial.print("\" -> http://");
  Serial.println(WiFi.softAPIP());


  /* API routes */

  server.on(
    "/api/data",
    handleData
  );

  server.on(
    "/api/gps",
    handleGps
  );

  server.on(
    "/status",
    handleStatus
  );


  server.onNotFound(
    handleNotFound
  );


  server.begin();

  Serial.println(
    "Web server started"
  );

  Serial.println(
    "Waiting for GPS data..."
  );
}


/* --------------------------------------------------
   LOOP
   -------------------------------------------------- */

void loop() {

  /* Receive GPS JSON */

  readLink();


  /* Handle website requests */

  server.handleClient();
}
