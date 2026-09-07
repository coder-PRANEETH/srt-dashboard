# ESP32 Setup — Run & Upload Guide

Two ESP32 boards:

| Board | Sketch | Job |
|---|---|---|
| **ESP32 #1** | `esp32_dashboard_host` | Makes its own WiFi, serves the website |
| **ESP32 #2** | `esp32_gps_sender` | Reads the GPS, sends it to #1 over a wire |
| (#2, testing) | `esp32_gps_test` | Diagnostic only — run this first |

---

## 1. One-time setup

**Install the ESP32 boards** — Arduino IDE → File → Preferences → *Additional Board Manager URLs*:

```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

Then Tools → Board → Boards Manager → search **esp32** → Install.

**Install the GPS library** — Tools → Manage Libraries → search **TinyGPSPlus** (by Mikal Hart) → Install.

**Install the filesystem uploader** (needed once, for ESP32 #1 only).
Download the `.vsix` from
[arduino-littlefs-upload releases](https://github.com/earlephilhower/arduino-littlefs-upload/releases)
and drop it into `~/.arduinoIDE/plugins/` (create the folder if missing), then restart the IDE.

> Requires Arduino IDE **2.2.1+**. The uploader appears as a command in the
> Ctrl+Shift+P palette, not in the Tools menu.

---

## 2. Wiring

**GPS module → ESP32 #2**

| GPS | ESP32 #2 | Note |
|---|---|---|
| VCC | 3V3 | **not 5V** — it can destroy the module |
| GND | GND | |
| TX | GPIO16 | crossed: TX → RX |
| RX | *(not connected)* | we never send the module commands |

**ESP32 #2 → ESP32 #1**

| ESP32 #2 | ESP32 #1 |
|---|---|
| GPIO17 (TX) | GPIO16 (RX) |
| GND | GND — **required**, or the link reads garbage |

> Do **not** join 3V3 or 5V between the boards. Power each from its own USB.

**Potentiometer → ESP32 #1** (the speed dial)

| Pot | ESP32 #1 |
|---|---|
| outer leg | 3V3 |
| wiper (middle) | GPIO34 |
| outer leg | GND |

> Must be GPIO 32–39. Other analog pins read 0 whenever WiFi is on.

---

## 3. Test the GPS first

Plug in **ESP32 #2 only**.

1. Tools → Board → **ESP32 Dev Module**
2. Tools → Port → pick your board
3. Open `esp32_gps_test/esp32_gps_test.ino` → Upload
4. Tools → Serial Monitor, set baud to **115200**

What you should see:

- First seconds: `NO DATA` — fine only if it clears within a second or two
- Then: satellites climbing, `FIX: NO`
- After 30–90 seconds **outdoors**: a real position

**Indoors it may never lock.** Go to a window or outside. If it stays at
`NO DATA`, check that GPS TX goes to GPIO16 (crossed), that GND is
connected, and try 38400 baud — some modules ship at that rate.

---

## 4. Upload ESP32 #2 (the sender)

Same board and port settings. Open `esp32_gps_sender/esp32_gps_sender.ino`
and Upload. The Serial Monitor prints a status block every 2 seconds
ending in the exact line it sends to ESP32 #1:

```
sending         : {"lat":10.728012,"lon":79.019534,"sats":9,"speed":0.4,"fix":true}
```

---

## 5. Upload ESP32 #1 (the website host)

This board needs **two** uploads: the program, then the website.

### 5a. Build the website

```bash
cd dashboard
npm install          # first time only
npm run build
python3 tools/pack-fs.py
```

`pack-fs.py` gzips the JS/CSS, drops blank map tiles, and copies
everything into `esp32_dashboard_host/data/`. It prints the headroom —
if it says **DOES NOT FIT**, re-download tiles at a lower zoom.

### 5b. Set the partition scheme — **do not skip this**

Tools → **Partition Scheme** → **`No OTA (2MB APP/2MB SPIFFS)`**

The default scheme leaves far too little room and the website upload will
fail or the page will not load.

### 5c. Upload the program

Open `esp32_dashboard_host/esp32_dashboard_host.ino` → Upload.

### 5d. Upload the website

**Close the Serial Monitor first** — it holds the port and the upload fails.

Press `Ctrl+Shift+P` → type **"Upload LittleFS to Pico/ESP8266/ESP32"** → Enter.

This takes a minute or two (~1.5 MB).

---

## 6. Run it

1. Power both boards (each on its own USB).
2. On your phone or laptop, connect to WiFi:
   - **Network:** `srt-dash`
   - **Password:** `srt-dash-2026`
3. Open **http://192.168.4.1**

Your phone may warn "no internet" — that is expected, stay connected.

Health check: **http://192.168.4.1/status** shows filesystem usage, how
long ago the last fix arrived, and the raw record.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Page blank / "upload the data folder" | Step 5d not done, or wrong partition scheme (5b) |
| `LittleFS failed!` on serial | Wrong partition scheme — set it and redo 5c **and** 5d |
| Website loads, car never moves | Check the GPIO17→GPIO16 wire **and the shared GND** |
| `/status` says `fix: never` | ESP32 #2 isn't sending — recheck it in Serial Monitor |
| Map frozen at one spot | No fresh fix for 5 s; go outdoors |
| Speed dial stuck at 0 | Pot wiper must be on GPIO34 (32–39 only) |
| Upload fails / port busy | Close Serial Monitor; hold **BOOT** while it says "Connecting..." |
| GPS shows `NO DATA` | TX→GPIO16 crossed, GND joined, try 38400 baud |

---

## The data contract

ESP32 #2 → ESP32 #1, one line per second:

```json
{"lat":10.728012,"lon":79.019534,"sats":9,"speed":34.5,"course":78.4,"fix":true}
```

`speed` and `course` are **optional**. `course` is deliberately omitted
below 2 km/h — the module reports garbage when stationary, and sending 0
would pin the map marker to due north.

The website reads two endpoints on ESP32 #1:

- `GET /api/data` → `{"value":75.4}` — potentiometer as km/h
- `GET /api/gps` → the record above, or `fix:false` if #2 went quiet
