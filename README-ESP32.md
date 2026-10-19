# SRT CAN dashboard and vehicle nodes

The dashboard uses a 500 kbps CAN bus. Node 1 reads the accelerator and drives the steering motor; Node 3 sends switch inputs. Node 4 sends GPS, ignition and pitch. Node 5 hosts the offline dashboard and CAN log over WiFi.

## Current CAN protocol

| Sender | ID | DLC | Data | Timing |
| --- | --- | --- | --- | --- |
| Node 5 → Node 1 | `0x110` | 8 | Direction (0 STOP, 1 LEFT, 2 RIGHT), PWM (0 or 60), sequence LE16, Node 1 token LE32 | During a held button, approximately every 80–330 ms |
| Node 1 | `0x111` | 8 | Applied direction, PWM, stop reason, protocol version=1, token LE32 | Every 100 ms and on change |
| Node 1 or remote host | `0x301` | 2 | Raw ADC 0–4095, **little-endian**: low byte then high byte | Every 50 ms |
| Node 3 or remote host | `0x300` | 1 | Switch bitmask: cruise=0, ECO=1, reverse=2, hill=3, horn=4, **headlight=5, autonomous=6** | On change after 25 ms debounce, plus every 1 s |
| Node 5 → Nodes 1/3 | `0x120` | Operation, reverse, session LE32, sequence LE16 | Every 50 ms while held/returning ownership |
| Node 1 / Node 3 | `0x121` / `0x122` | Phase + reverse bit 7, physical input LE16, marker `0xD1`, session LE32 | Every 100 ms |
| Node 4 | `0x400` | 8 | GPS degrees/minutes, fix and satellites | Every 1 s |
| Node 4 | `0x401` | 8 | Ignition and on-time | Every 1 s |
| Node 4 | `0x500` | 8 | Hill request, pitch and validity, hold=0% | 50 ms with hill requested, otherwise 500 ms |
| Nodes 4/5 | `0x600` | 8 | Node heartbeat and counters | Every 1 s |

**Node 2 has no CAN feedback.** Its current repository sketch is a raw GPIO34 ADC serial test; this change leaves it untouched. Nodes 1 and 3 send data without heartbeats. The old eight-byte `0x300`/`0x301` layouts are not used. Old-format frames stay visible in the CAN log but do not refresh telemetry.

## Wiring

All MCP2515 modules: GPIO18 SCK, GPIO19 MISO, GPIO23 MOSI, GPIO25 CS. The common helper used by Nodes 4/5 also configures GPIO27 INT. Use 500 kbps and match the configured **8 MHz crystal** to the module. Connect common grounds and CANH/CANL, with 120 Ω termination at each physical end and 3.3 V-compatible ESP32 logic.

- Node 1: GPIO34 accelerator, **GPIO32 steering LPWM and GPIO33 steering RPWM**. Steering uses only the user-tested `analogWrite` configuration at **60/255**. LEFT sets RPWM=0 then LPWM=60; RIGHT sets LPWM=0 then RPWM=60; STOP sets both to zero. The 1.1–4.52 V sensor output needs the existing divider: **10 kΩ from pedal OUT to GPIO34, 15 kΩ from GPIO34 to GND**. Expected pin voltage is about 0.66–2.71 V. Never connect the 4.52 V output directly to the ESP32. The new code sends raw ADC; it does not use saved NVS calibration.
- Node 2: current sketch reads GPIO34 and prints raw ADC. No Node 2 motor or relay pin assignments are used for steering.
- Node 3: active-low switches to ground: cruise GPIO4, ECO GPIO5, reverse GPIO13, hill GPIO14, horn GPIO26, headlight GPIO33, autonomous GPIO32. Release the GPIO4/5 switches during boot.
- Node 4: GPS TX to GPIO16 at 9600 baud; conditioned ignition input GPIO34; MPU6050 SDA/SCL GPIO21/22. Its hill receiver reads bit 3 of the new one-byte switch frame and allows the 1 s broadcast interval before timing out at 3 s.
- Node 5: MCP2515, WiFi AP and LittleFS dashboard; no local speed sensor.

## Serial diagnosis

Open Serial Monitor at **115200 baud** for every node. Successful transmissions and received project messages print named values and units, not packed payload bytes. Timestamped lines identify direction (`CAN TX` or `CAN RX`), CAN ID and DLC. Examples:

```text
[1000 ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=2000/4095 | corrected voltage estimate=0.912 V
[1000 ms] CAN TX ID=0x300 DLC=1 | Cruise: ON | ECO: OFF | Reverse: ON | Hill Hold: OFF | Horn: OFF | Headlight: ON | Autonomous: OFF
```

- Node 1 reports live ADC, estimated voltage, successful/failed sends and time since its last successful send every 250 ms.
- The current Node 2 sketch prints raw ADC only; the dashboard cannot infer its health.
- Node 3 prints every named switch after debounce and successful sends, plus counters and pending-send failures every 250 ms. Initialization failures retry every second.
- Nodes 4/5 print decoded RX/TX values, controller setup attempts, send failures (rate-limited to once per second), and a one-second controller summary. Node 5 also prints WiFi clients, filesystem state and Node 1/3 freshness.
Heartbeat byte counters wrap at 256; the local Serial totals use 32-bit counters.

A successful controller initialization checks SPI/controller setup; physical CAN sends need another active node to acknowledge them. TX failures report wiring/termination/peer checks.

## Dashboard and upload

The repository root contains the five working sketches:

- `node1_bcm.ino`: accelerator sender and steering receiver/output.
- `node2_pcm.ino`: existing raw ADC serial test, unchanged.
- `node3_ccm.ino`: switch sender.
- `node4_tcu.ino`: GPS, ignition and pitch.
- `node5_dash.ino`: WiFi dashboard and CAN log.

`CanCommon.h` is shared by Nodes 4/5. **Nodes 1 and 5 also require `ThrottleCalibration.h` and `SteeringControl.h`** in their individual sketch folders. Temporary test sketches and the standalone CAN monitor have been removed.

Compile each node separately: Arduino combines all `.ino` files in a sketch folder, so do not compile the repository root as one sketch. Copy the desired `.ino` into a separate folder with the same base name (for example, `node4_tcu/node4_tcu.ino`) outside this repository. For Nodes 4/5, copy `CanCommon.h` into that folder too. For Nodes 1/5, also copy `ThrottleCalibration.h` and `SteeringControl.h`. For Nodes 1/3/5, also copy `RemoteDrive.h`. For Node 5, copy the rebuilt root `data/` directory into its sketch folder before uploading LittleFS. Open that individual sketch in Arduino IDE. Use classic **ESP32 Dev Module**, with the ESP32 board package, `mcp_can`, `TinyGPSPlus`, `Adafruit_MPU6050`, `Adafruit_Sensor` and `Adafruit_BusIO` libraries. Node 5 needs **No OTA (2MB APP/2MB SPIFFS)** and a LittleFS uploader.

```bash
cd dashboard
npm ci
npm run build:esp
```

Upload each node sketch from its separate Arduino folder. For `node5_dash.ino`, upload the rebuilt `data/` folder as **LittleFS**; keep the `.gz` filenames. Join `srt-dash` with the configured initial password `12345678`, then open `http://192.168.4.1`.

Throttle voltage is `max(0, ADC × 3.3 / 4095 − 0.7)` volts. The gauge shows `floor(correctedVoltage × 100 / 2.6)`, limited to 0–100%; this is an ADC estimate, not measured motor output. Raw ADC and raw pin-voltage estimate remain available under Node 1 on `/status`, and the two-byte `0x301` payload remains raw/unchanged. Example: ADC 2000 → raw 1.612 V → corrected 0.912 V → 35%. Missing readings remain unavailable, not zero.

The switch panel shows all seven inputs. Node 1 accelerator data expires after 500 ms and Node 3 after 3000 ms. Node 2 has no telemetry; its output remains unknown.

The home page contains driving instruments and the **CONTROL** control: GPS speed, IMU pitch, corrected accelerator percentage, trip, heading and satellites. **MAP** opens the offline map fullscreen; **CLOSE** or Escape returns. Its SRT opening animation runs once per tab session and honors reduced motion. The wheel follows **IMU pitch**, not steering angle; missing pitch freezes the last angle and shows unavailable.


### Mobile throttle and reverse

Upload **Nodes 1, 3 and 5 together**, with `RemoteDrive.h` in all three sketch folders, plus rebuilt Node 5 LittleFS assets. The host simulates the existing receiver protocol: `0x301`, DLC 2, raw 12-bit ADC little-endian; `0x300`, DLC 1, reverse in bit 2. No traction GPIO assignments or Node 2 code are changed. **The checked-in Node 2 is ADC-only and cannot drive a motor.** These controls require the vehicle's working CAN motor receiver; its motor output and stop watchdog cannot be verified from this repository.

Open **CONTROL** from the homepage. Steering and throttle support simultaneous touches; **STOP** cancels both. The mobile control panel has an adjustable **ADC %**, initially zero: `ADC = percent × 4095 / 100`. This is the transmitted raw input scale, not traction PWM or the offset-corrected gauge percentage. Hold THROTTLE to send it; release to send zero. REVERSE selects the next direction only with no held throttle and at least one second at zero. A reversal holds the old direction at zero for one second, switches direction, then waits another second before throttle. This interlock measures zero command time, **not vehicle speed**; select direction only when physically stationary.

The host first asks Nodes 1/3 to silence their normal `0x301`/`0x300` broadcasts. Both must acknowledge the same session before the host sends simulated input every 50 ms. Physical pedal above the 0.7 V idle threshold, missing acknowledgements, CAN send failure, 350 ms command expiry or a 5-second hold ends the gesture. Nodes independently latch zero throttle/held direction on lease expiry. Cruise and autonomous bits are cleared during simulation; other switch bits are preserved. Recovery requires a new press. Loss of the physical CAN bus still requires the working Node 2 receiver's own output watchdog; a CAN zero command is not a brake.

After release, normal switches return before the pedal sender. The physical pedal must be idle and the physical reverse switch must match the last transmitted direction; otherwise zero remains latched. `MATCH REVERSE SWITCH` identifies that condition. `0x120` carries the handoff command; `0x121` and `0x122` report physical inputs and ownership. `/api/drive` GET is read-only; same-origin POST with `X-SRT-Control: 1` accepts `action=throttle` (`token`, increasing `seq`, `percent`), `action=reverse` (`token`, `reverse=0|1`), or `action=stop`. Outgoing simulation frames are marked `tx` in the bounded CAN log. `/api/vehicle.remoteDrive` reports command state and physical input/gate feedback separately.

### Remote steering

Upload **both Node 1 and Node 5**, and upload the rebuilt `data/` LittleFS assets to Node 5. Join dashboard WiFi, open the homepage, and press **CONTROL**. Hold LEFT/RIGHT to turn at PWM 60. Release, drag off the button, cancel touch, switch tabs, close the panel or press STOP to stop. Space/Enter supports keyboard holds. Reconnection never resumes a previous press.

Node 1 independently stops after **350 ms without a new valid CAN command**, on CAN send failure, or after **5 seconds of continuous hold**. Both outputs start at zero. Every stop changes the command token, and duplicate/out-of-order commands cannot extend a hold. A direction change requires releasing first. Old Node 1 firmware without `0x111` feedback cannot enable these controls. The dashboard also expires its HTTP command lease at 350 ms.

`GET /api/steering` is read-only. Commands use same-origin `POST /api/steering` with `X-SRT-Control: 1` and form fields `direction`, `token`, `seq`; STOP needs only `direction=stop`. HTTP acknowledges CAN transmission. The displayed PWM output comes from Node 1, **not a steering angle or movement sensor**. No limit-switch or position feedback exists in this configuration. First verify direction and release behavior with the front wheels lifted and motor power readily disconnectable.

Node 1 steering stop reasons: 0 normal, 1 CAN failure, 2 command timeout, 3 invalid command, 4 hold limit. `/status` highlights unavailable steering or nonzero stop reasons. A new valid press or STOP clears the reason. Only the steering motor configuration on Node 1 is changed; no traction motor/relay code is added.

Open **http://192.168.4.1/status** directly for diagnostics (also `/status/`). There is deliberately no homepage link to it. Tap a node for its sensors, readings and freshness. Node 2 always shows unknown/no feedback. CAN log supports exact hex ID, node, message/raw-byte search and issues-only filters. **RESET** clears filters. **PAUSE** freezes the browser snapshot while CAN reception continues; **COPY** and **EXPORT** include filtered frames, diagnostics and recent events. The browser retains at most 64 frames, 24 fault transitions and 600 route points.

`/api/vehicle` returns decoded data and node states. `/api/gps` returns a GPS fix; `/api/data` returns speed estimated from successive fixes. Missing/stale signals are `null`. `/api/can-log` returns the latest 64 frames, `uptimeMs`, and exactly `dlc` bytes per frame. The optional exact hex filter `/api/can-log?id=0x301` still works; invalid IDs return HTTP 400. `/api/diagnostics` returns per-signal freshness, invalid-payload counts, total RX/TX/errors, free/minimum/largest heap block, LittleFS usage and WiFi clients. `/status` now returns HTML, not JSON; consumers of its old JSON must use `/api/vehicle`.

## Automated checks

Run `python3 dashboard/tools/test-steering.py` for the actual Node 1 GPIO adapter and lease state-machine tests; run `node --test dashboard/tools/diagnostics.test.mjs dashboard/tools/steering-client.test.mjs dashboard/tools/drive-client.test.mjs` for decoders and browser command-race tests. For the receiver/API regression checks, run `python3 dashboard/tools/test-can-protocol.py` from the repository root (requires `g++`). This compiles the actual Node 5 decoder/API functions against a simulated CAN bus and checks endian order, all switch bits, data expiry, invalid frames, log filtering and clock wraparound. `dashboard/tools/smoke-packed.cjs` checks the packed website in Playwright, including the mobile CAN filter and missing-data display.

The GPS `0x400` format has no coordinate sign bits; Node 4 reports a fix only for nonnegative coordinates. Firmware compilation and simulated/browser checks do not test physical boards, wiring or sensors.


### Remote control validation (2026-10-20)

- Node 1 compiled at 299,063 bytes flash and 22,608 bytes static RAM with the default ESP32 Dev Module partition.
- Node 3 compiled at 281,912 bytes flash and 22,364 bytes static RAM.
- Node 5 compiled with ESP32 core 3.3.11, `esp32:esp32:esp32:PartitionScheme=no_ota`.
- Firmware uses 1,006,316 bytes (47%) of the 2,097,152-byte application partition; static RAM uses 49,480 bytes (15%) of 327,680 bytes.
- Packed frontend JS/CSS is about 123 KB gzip. Local tiles remain included. The packer estimates 1.47 MiB of file blocks, a 64 KiB metadata reserve and 352 KiB headroom.
- A real LittleFS image was successfully created with `mklittlefs -c data -b 4096 -p 256 -s 1966080 build/srt-littlefs.bin`. Partition size is 1,966,080 bytes, offset `0x210000`, for the specified No OTA layout.
- Lint, native CAN/API regression tests, decoder/route tests and packed-site browser tests pass. Browser tests cover 320/390 px phones, 800×480, desktop, map lazy loading, routes, stale data, IMU freeze, filters, pause/resume, copy/export and API recovery.
- These are compile, image and simulated-browser checks. On-device runtime heap and actual bus/WiFi behavior still require the physical ESP32; inspect Node 5 on `/status` during operation.
