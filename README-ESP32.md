# SRT CAN dashboard and vehicle nodes

Five classic ESP32 nodes share a 500 kbps CAN bus. Nodes 1–3 use the supplied working accelerator, motor controller and switch sketches. Node 4 sends GPS, ignition and pitch. Node 5 hosts the offline dashboard and CAN log over WiFi.

## Current CAN protocol

| Sender | ID | DLC | Data | Timing |
| --- | --- | --- | --- | --- |
| Node 1 | `0x301` | 2 | Raw ADC 0–4095, **little-endian**: low byte then high byte | Every 50 ms |
| Node 3 | `0x300` | 1 | Switch bitmask: cruise=0, ECO=1, reverse=2, hill=3, horn=4, **headlight=5, autonomous=6** | On change after 25 ms debounce, plus every 1 s |
| Node 4 | `0x400` | 8 | GPS degrees/minutes, fix and satellites | Every 1 s |
| Node 4 | `0x401` | 8 | Ignition and on-time | Every 1 s |
| Node 4 | `0x500` | 8 | Hill request, pitch and validity, hold=0% | 50 ms with hill requested, otherwise 500 ms |
| Nodes 4/5 | `0x600` | 8 | Node heartbeat and counters | Every 1 s |

**Node 2 receives only:** the supplied motor sketch sends no motor status or heartbeat. Nodes 1 and 3 also send no heartbeat. The old eight-byte `0x300`/`0x301` layouts and Node 1 `0x303` pedal calibration diagnostic are no longer used. Old-format frames remain visible in the CAN log, but do not update accelerator/switch telemetry.

Node 2 decodes reverse from bit 2 and maps the little-endian accelerator ADC to PWM on GPIO13/14. `ACCEL_MIN_ADC=0` and `ACCEL_MAX_ADC=4095` are the supplied mapping endpoints. Reverse relays are **active LOW**: GPIO33/26 LOW=ON, HIGH=OFF. Production runs the supplied motor control logic, including its zero-throttle step before direction changes, 500 ms accelerator timeout and 3000 ms switch timeout. Unlike the previous firmware, production PWM and reverse outputs are enabled. The switch timeout resets throttle and selects forward; subsequent accelerator frames follow the supplied controller logic.

## Wiring

All MCP2515 modules: GPIO18 SCK, GPIO19 MISO, GPIO23 MOSI, GPIO25 CS. The common helper used by Nodes 4/5 also configures GPIO27 INT. Use 500 kbps and match the configured **8 MHz crystal** to the module. Connect common grounds and CANH/CANL, with 120 Ω termination at each physical end and 3.3 V-compatible ESP32 logic.

- Node 1: GPIO34 accelerator only. No steering motor, horn, wireless or Pi UART code. The 1.1–4.52 V sensor output needs the existing divider: **10 kΩ from pedal OUT to GPIO34, 15 kΩ from GPIO34 to GND**. Expected pin voltage is about 0.66–2.71 V. Never connect the 4.52 V output directly to the ESP32. The new code sends raw ADC; it does not use saved NVS calibration.
- Node 2: PWM GPIO13/14, active-low reverse relays GPIO33/26. No brake input.
- Node 3: active-low switches to ground: cruise GPIO4, ECO GPIO5, reverse GPIO13, hill GPIO14, horn GPIO26, headlight GPIO33, autonomous GPIO32. Release the GPIO4/5 switches during boot.
- Node 4: GPS TX to GPIO16 at 9600 baud; conditioned ignition input GPIO34; MPU6050 SDA/SCL GPIO21/22. Its hill receiver reads bit 3 of the new one-byte switch frame and allows the 1 s broadcast interval before timing out at 3 s.
- Node 5: MCP2515, WiFi AP and LittleFS dashboard; no local speed sensor.

## Serial diagnosis

Open Serial Monitor at **115200 baud** for every node. Successful transmissions and received project messages print named values and units, not packed payload bytes. Timestamped lines identify direction (`CAN TX` or `CAN RX`), CAN ID and DLC. Examples:

```text
[1000 ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=2000/4095 | voltage estimate=1.612 V
[1000 ms] CAN TX ID=0x300 DLC=1 | Cruise: ON | ECO: OFF | Reverse: ON | Hill Hold: OFF | Horn: OFF | Headlight: ON | Autonomous: OFF
[1000 ms] CAN RX ID=0x301 DLC=2 | ADC=2000/4095 | throttle=48% PWM=122/255 | reverse=ON
```

- Node 1 reports live ADC, estimated voltage, successful/failed sends and time since its last successful send every 250 ms.
- Node 2 reports received ADC and switch values, throttle percent/PWM, relay polarity/state, input ages, timeouts, and receive counts. It explicitly identifies itself as receive-only.
- Node 3 prints every named switch after debounce and successful sends, plus counters and pending-send failures every 250 ms. Initialization failures retry every second.
- Nodes 4/5 print decoded RX/TX values, controller setup attempts, send failures (rate-limited to once per second), and a one-second controller summary. Node 5 also prints WiFi clients, filesystem state and Node 1/3 freshness.
Heartbeat byte counters wrap at 256; the local Serial totals use 32-bit counters.

A successful controller initialization checks SPI/controller setup; physical CAN sends need another active node to acknowledge them. TX failures report wiring/termination/peer checks.

## Dashboard and upload

The repository root contains the five working sketches:

- `node1_bcm.ino`: accelerator sender.
- `node2_pcm.ino`: motor controller.
- `node3_ccm.ino`: switch sender.
- `node4_tcu.ino`: GPS, ignition and pitch.
- `node5_dash.ino`: WiFi dashboard and CAN log.

`CanCommon.h` is shared by Nodes 4/5. Temporary test sketches and the standalone CAN monitor have been removed.

Compile each node separately: Arduino combines all `.ino` files in a sketch folder, so do not compile the repository root as one sketch. Copy the desired `.ino` into a separate folder with the same base name (for example, `node4_tcu/node4_tcu.ino`) outside this repository. For Nodes 4/5, copy `CanCommon.h` into that folder too. For Node 5, copy the rebuilt root `data/` directory into its sketch folder before uploading LittleFS. Open that individual sketch in Arduino IDE. Use classic **ESP32 Dev Module**, with the ESP32 board package, `mcp_can`, `TinyGPSPlus`, `Adafruit_MPU6050`, `Adafruit_Sensor` and `Adafruit_BusIO` libraries. Node 5 needs **No OTA (2MB APP/2MB SPIFFS)** and a LittleFS uploader.

```bash
cd dashboard
npm ci
npm run build:esp
```

Upload each node sketch from its separate Arduino folder. For `node5_dash.ino`, upload the rebuilt `data/` folder as **LittleFS**; keep the `.gz` filenames. Join `srt-dash` with the configured initial password `12345678`, then open `http://192.168.4.1`.

The accelerator gauge shows ADC full-scale percent (`floor(ADC × 100 / 4095)`), not measured motor output or calibrated pedal travel. GPIO34 voltage is an ADC-based estimate (`ADC × 3.3 / 4095`), not a calibrated voltage measurement. The switch panel shows all seven bits in the supplied order. Node 1 goes stale after 500 ms and Node 3 after 3000 ms, based on their data frames; no heartbeat is needed. Node 2 shows **NO TELEMETRY**, and its motor output is unavailable. Reverse request is labeled as a switch request, not confirmed relay feedback.

`/api/vehicle` returns decoded data and node states. `/api/gps` returns a GPS fix, and `/api/data` returns speed estimated from successive fixes. Missing/stale signals are `null`. `/api/can-log` returns the latest 64 frames, with `data` containing exactly `dlc` bytes. Enter an exact hex ID such as `0x301` or `301` in **CAN LOG**; **Clear filter** restores all IDs. The API supports `/api/can-log?id=0x301`. Invalid IDs return HTTP 400. Filtering never changes reception or telemetry and applies to the existing 64-frame buffer.

## Automated checks

For the receiver/API regression checks, run `python3 dashboard/tools/test-can-protocol.py` from the repository root (requires `g++`). This compiles the actual Node 5 decoder/API functions against a simulated CAN bus and checks endian order, all switch bits, data expiry, invalid frames, log filtering and clock wraparound. `dashboard/tools/smoke-packed.cjs` checks the packed website in Playwright, including the mobile CAN filter and missing-data display.

The GPS `0x400` format has no coordinate sign bits; Node 4 reports a fix only for nonnegative coordinates. Firmware compilation and simulated/browser checks do not test physical boards, wiring or sensors.
