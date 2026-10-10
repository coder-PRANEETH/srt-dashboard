# ESP32 offline dashboard

The dashboard runs from ESP32 #1's own WiFi access point. It needs no internet connection. ESP32 #2 reads the GPS receiver and sends its fix to #1 over CAN.

| Board | Sketch | Role |
|---|---|---|
| ESP32 #1 | `esp32_dashboard_host/esp32_dashboard_host.ino` | Hosts the site and receives CAN frames |
| ESP32 #2 | `esp32_gps_sender/esp32_gps_sender.ino` | Reads GPS and sends three CAN frames each second |
| ESP32 #2, diagnostic | `esp32_gps_test/esp32_gps_test.ino` | Checks the GPS alone |

## Setup

Use two classic **ESP32 / ESP32-WROOM-32** boards with at least **4 MB flash**. These GPIO and UART assignments are for the classic ESP32; other ESP32 variants need different wiring and settings. Install the ESP32 board package, the **TinyGPSPlus** and **mcp_can** Arduino libraries, and a LittleFS data uploader for Arduino IDE. Select **ESP32 Dev Module** and **No OTA (2MB APP/2MB SPIFFS)** for ESP32 #1. The partition menu says SPIFFS, but the sketch and uploader must use **LittleFS**. Its actual filesystem capacity is `0x1E0000` bytes (1.875 MiB), at flash offset `0x210000`.

The CAN crystal setting in both sketches must match the crystal on each MCP2515 board; the current setting is 8 MHz. Both use 500 kbps CAN.

### Wiring

- GPS TX → ESP32 #2 GPIO16; GPS GND → ESP32 #2 GND. GPS RX is unused. Supply the GPS with the voltage specified by its module.
- On **each** ESP32, MCP2515 SCK → GPIO18, SI → GPIO23, SO → GPIO19, CS → GPIO25, INT → GPIO27, and GND → GND. Power each CAN module according to its board rating. The ESP32 GPIO signals must stay within 3.3 V; a 5 V CAN module needs compatible logic levels or level shifting on signals returning to the ESP32.
- Connect CANH to CANH, CANL to CANL, and the grounds of the two ESP32 boards. Fit a 120 Ω terminator at each end of the CAN bus.
- Potentiometer outer legs → ESP32 #1 3V3 and GND; wiper → GPIO34.

### Upload

1. For a GPS check, upload `esp32_gps_test` to ESP32 #2. Open Serial Monitor at **115200**. It reports bytes, checksum errors, satellites, fix, speed, and HDOP. The GPS UART is configured for **9600** baud in both sender and diagnostic sketches.
2. Upload `esp32_gps_sender` to ESP32 #2.
3. Build and pack the local website assets:

   ```bash
   cd dashboard
   npm ci
   npm run build:esp
   ```

4. Upload `esp32_dashboard_host` to ESP32 #1 with the partition scheme above. Close Serial Monitor and upload the generated `esp32_dashboard_host/data/` folder with the LittleFS uploader. Keep the `.gz` suffixes and directory structure: the sketch serves the original URLs with `Content-Encoding: gzip`.
5. Connect your phone or laptop to **srt-dash**, password **12345678**, and open **http://192.168.4.1**. A “no internet” WiFi warning is expected; stay connected to this access point.

The included map tiles cover about 2 km around SASTRA at zoom levels 13–17. The site uses only local assets, so areas outside that tile set appear blank. To cover another area, obtain licensed offline tiles and put them under `dashboard/public/tiles/{z}/{x}/{y}.png`, then build, pack, and upload again. The packer estimates LittleFS capacity and reserves space for metadata before writing; creating an image with `mklittlefs` verifies the actual fit.

The prepared `esp32_dashboard_host/build/littlefs.bin` is a LittleFS image for the above partition, verified by creating and unpacking it with `mklittlefs`. Rebuild this image whenever the data folder changes before using it. The LittleFS uploader can generate its own image directly from `data/`.

## Data and troubleshooting

- `GET /api/data` returns `{"value":75.4}` from the potentiometer in km/h.
- `GET /api/gps` returns the filtered position, speed, course when available, satellites, HDOP when available, fix state, and age in milliseconds. A missing fix returns `"fix":false`.
- `GET /api/can-log` returns CAN readiness, total received frame count, and the latest 64 frames, ordered oldest first. Each frame has sequence, uptime in milliseconds, CAN ID, DLC, raw bytes, and extended/RTR flags.
- `GET /status` reports filesystem usage, CAN frame count, heartbeat, and the latest GPS JSON.

The sender rejects poor HDOP and implausible one-sample jumps before sending coordinates. Raw or accepted fixes expire after five seconds; the host also requires both fresh position and metadata frames. `ageMs` measures CAN receipt age, not the receiver's NMEA age. The browser keeps the last accepted path when a bad sample arrives and marks the fix stale. It polls the dial at 10 Hz and GPS at 2 Hz; the sender broadcasts once per second.

## Verification

Verified on 10 October 2026 with Arduino CLI 1.5.1, ESP32 core 3.3.11, TinyGPSPlus 1.0.3, and mcp_can 1.5.1. All three sketches compile. TinyGPSPlus emits a fall-through warning from its library code; there are no sketch compile errors. A clean dashboard install, build, and lint pass; patched development dependencies report zero findings in `npm audit` at verification time.

From the repository root:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota esp32_dashboard_host
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_gps_sender
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_gps_test
python3 tools/test-gps-sender.py
python3 tools/test-host-can.py
```

The desktop tests cover GPS loss, rejected fixes, stale fields, CAN packing/decoding, metadata loss, timeout rollover, and CAN history. The packed site was also opened in a browser with API fixtures: local assets, GPS display, CAN log, request rates, timeout recovery, and map controls passed without external network requests. To repeat the browser check, use `node dashboard/tools/smoke-packed.cjs`; its header explains the optional Playwright/browser installation. The 229-file LittleFS image fits the partition and unpacks byte-for-byte. No boards were connected during verification; GPS reception, CAN wiring, crystal frequency, and WiFi behavior still need a test on the actual hardware.

| Symptom | Check |
|---|---|
| Blank page | LittleFS upload, partition scheme, and compressed asset response |
| GPS shows NO DATA | GPS TX → GPIO16, ground, and GPS UART baud |
| CAN not found | MCP2515 power, CS wiring, and 8/16 MHz crystal setting |
| No fix on site | Outdoor sky view, both CAN wires, termination, shared ground, and `/status` |
| Map has blank tiles | Position outside the included offline tile area |
| Speed dial has no reading | Potentiometer wiper on GPIO34 and `/api/data` |
