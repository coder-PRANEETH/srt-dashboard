# ESP32 offline dashboard

The dashboard runs from ESP32 #1's own WiFi access point. It needs no internet connection. ESP32 #2 reads the GPS receiver and sends its fix to #1 over CAN.

| Board | Sketch | Role |
|---|---|---|
| ESP32 #1 | `esp32_dashboard_host/esp32_dashboard_host.ino` | Hosts the site and receives CAN frames |
| ESP32 #2 | `esp32_gps_sender/esp32_gps_sender.ino` | Reads GPS and sends three CAN frames each second |
| ESP32 #2, diagnostic | `esp32_gps_test/esp32_gps_test.ino` | Checks the GPS alone |

## Setup

Install the ESP32 board package, the **TinyGPSPlus** and **mcp_can** Arduino libraries, and a LittleFS data uploader for Arduino IDE. Select **ESP32 Dev Module** and **No OTA (2MB APP/2MB SPIFFS)** for ESP32 #1. The CAN crystal setting in both sketches must match the crystal on each MCP2515 board; the current setting is 8 MHz.

### Wiring

- GPS TX → ESP32 #2 GPIO16; GPS GND → ESP32 #2 GND. GPS RX is unused. Supply the GPS with the voltage specified by its module.
- On **each** ESP32, MCP2515 SCK → GPIO18, SI → GPIO23, SO → GPIO19, CS → GPIO25, INT → GPIO27, and GND → GND. Power each CAN module according to its board rating.
- Connect CANH to CANH, CANL to CANL, and the grounds of the two ESP32 boards. Fit a 120 Ω terminator at each end of the CAN bus.
- Potentiometer outer legs → ESP32 #1 3V3 and GND; wiper → GPIO34.

### Upload

1. For a GPS check, upload `esp32_gps_test` to ESP32 #2. Open Serial Monitor at **115200**. It reports bytes, checksum errors, satellites, fix, speed, and HDOP. The GPS UART is configured for **9600** baud in both sender and diagnostic sketches.
2. Upload `esp32_gps_sender` to ESP32 #2.
3. Build and pack the local website assets:

   ```bash
   cd dashboard
   npm install
   npm run build
   python3 tools/pack-fs.py
   ```

4. Upload `esp32_dashboard_host` to ESP32 #1 with the partition scheme above. Close Serial Monitor and upload the generated `esp32_dashboard_host/data/` folder with the LittleFS uploader.
5. Connect your phone or laptop to **srt-dash**, password **12345678**, and open **http://192.168.4.1**. A “no internet” WiFi warning is expected; stay connected to this access point.

The included map tiles cover about 2 km around SASTRA at zoom levels 13–17. The site uses only local assets, so areas outside that tile set appear blank. To cover another area, obtain licensed offline tiles and put them under `dashboard/public/tiles/{z}/{x}/{y}.png`, then build, pack, and upload again. The packer checks LittleFS capacity before writing.

## Data and troubleshooting

- `GET /api/data` returns `{"value":75.4}` from the potentiometer in km/h.
- `GET /api/gps` returns the filtered position, speed, course when available, satellites, HDOP when available, fix state, and age in milliseconds. A missing fix returns `"fix":false`.
- `GET /status` reports filesystem usage, CAN frame count, heartbeat, and the latest GPS JSON.

The sender rejects poor HDOP and implausible one-sample jumps before sending coordinates. The browser also rejects implausible route points. Both keep the last accepted path when a bad sample arrives; no GPS receiver can guarantee an accurate position without a good sky view.

| Symptom | Check |
|---|---|
| Blank page | LittleFS upload, partition scheme, and compressed asset response |
| GPS shows NO DATA | GPS TX → GPIO16, ground, and GPS UART baud |
| CAN not found | MCP2515 power, CS wiring, and 8/16 MHz crystal setting |
| No fix on site | Outdoor sky view, both CAN wires, termination, shared ground, and `/status` |
| Map has blank tiles | Position outside the included offline tile area |
| Speed dial has no reading | Potentiometer wiper on GPIO34 and `/api/data` |
