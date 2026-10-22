# SRT CAN dashboard and vehicle nodes

The dashboard uses a 500 kbps CAN bus. Node 1 reads the accelerator and drives the steering motor; Node 2 receives throttle/switch inputs and controls traction and reverse; Node 3 sends switch inputs. Node 4 sends GPS, ignition and pitch. Node 5 hosts the offline dashboard and CAN log over WiFi.

## Current CAN protocol

| Sender | ID | DLC | Data | Timing |
| --- | --- | --- | --- | --- |
| Node 1 | `0x100` | 8 | Signed steering angle BE16 in 0.1°, source age in ms (255=saturated/unknown), live flag, ESP-NOW sequence LE32 | Every 50 ms and on link change |
| Node 5 → Node 1 | `0x110` | 8 | Direction (0 STOP, 1 LEFT, 2 RIGHT), PWM (0 or 60), sequence LE16, Node 1 token LE32 | During a held button, approximately every 80–330 ms |
| Node 1 | `0x111` | 8 | Applied direction, PWM, stop reason, protocol version=1, token LE32 | Every 100 ms and on change |
| Node 1 or remote host | `0x301` | 2 | Raw ADC 0–4095, **little-endian**: low byte then high byte | Every 50 ms |
| Node 2 | `0x201` | 8 | Motor 1 on, motor 2 on, applied reverse, output %, cruise active=0, ECO selected, hill active=0, stop reason | Every 50 ms |
| Node 3 or remote host | `0x300` | 1 | Switch bitmask: cruise=0, ECO=1, reverse=2, hill=3, horn=4, **headlight=5, autonomous=6** | On change after 25 ms debounce, plus every 1 s |
| Node 5 → Nodes 1/3 | `0x120` | 8 | Operation, reverse, session LE32, sequence LE16 | Every 50 ms while held/returning ownership |
| Node 1 / Node 3 | `0x121` / `0x122` | 8 | Phase + reverse bit 7, physical input LE16, marker `0xD1`, session LE32 | Every 100 ms |
| Node 4 | `0x400` | 8 | GPS degrees/minutes, fix and satellites | Every 1 s |
| Node 4 | `0x401` | 8 | Ignition and on-time | Every 1 s |
| Node 4 | `0x500` | 8 | Hill request, pitch and validity, hold=0% | 50 ms with hill requested, otherwise 500 ms |
| Nodes 2/4/5 | `0x600` | 8 | Node heartbeat and counters | Every 1 s |

Node 2 now supplies output-command feedback and a heartbeat. Older receive-only Node 2 firmware still shows unknown until a heartbeat arrives. Nodes 1 and 3 send data without heartbeats. The old eight-byte `0x300`/`0x301` layouts are not used; Node 2 rejects them and stops throttle.

## Wiring

All MCP2515 modules: GPIO18 SCK, GPIO19 MISO, GPIO23 MOSI, GPIO25 CS. The common helper used by Nodes 2/4/5 also configures GPIO27 INT. Use 500 kbps and match the configured **8 MHz crystal** to the module. Connect common grounds and CANH/CANL, with 120 Ω termination at each physical end and 3.3 V-compatible ESP32 logic.

- Node 1: GPIO34 accelerator, **GPIO32 steering LPWM and GPIO33 steering RPWM**. Steering uses only the user-tested `analogWrite` configuration at **60/255**. LEFT sets RPWM=0 then LPWM=60; RIGHT sets LPWM=0 then RPWM=60; STOP sets both to zero. The 1.1–4.52 V sensor output needs the existing divider: **10 kΩ from pedal OUT to GPIO34, 15 kΩ from GPIO34 to GND**. Expected pin voltage is about 0.66–2.71 V. Never connect the 4.52 V output directly to the ESP32. The new code sends raw ADC; it does not use saved NVS calibration.
- Node 2: restored historical wiring from `ec18a47`: **GPIO13/14 motor-controller PWM, 1 kHz / 8 bit; GPIO26/33 active-high reverse relay inputs**. No local pedal or brake GPIO. `NODE2_ENABLE_TRACTION_OUTPUTS` and `NODE2_ENABLE_REVERSE_RELAYS` retain the historical default `0`; set each to `1` in the sketch after verifying the controller wiring and polarity. Relay inputs are logic signals; keep their separate 12 V load supply off ESP32 pins.
- Node 3: active-low switches to ground: cruise GPIO4, ECO GPIO5, reverse GPIO13, hill GPIO14, horn GPIO26, headlight GPIO33, autonomous GPIO32. Release the GPIO4/5 switches during boot.
- Node 4: GPS TX to GPIO16 at 9600 baud; conditioned ignition input GPIO34; MPU6050 SDA/SCL GPIO21/22. Its hill receiver reads bit 3 of the new one-byte switch frame and allows the 1 s broadcast interval before timing out at 3 s.
- Node 5: MCP2515, WiFi AP and LittleFS dashboard; no local speed sensor.

## Serial diagnosis

Open Serial Monitor at **115200 baud** for every node. Successful transmissions and received project messages print named values and units, not packed payload bytes. Timestamped lines identify direction (`CAN TX` or `CAN RX`), CAN ID and DLC. Examples:

```text
[1000 ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=2000/4095 | raw pin voltage estimate=1.612 V
[1000 ms] CAN TX ID=0x300 DLC=1 | Cruise: ON | ECO: OFF | Reverse: ON | Hill Hold: OFF | Horn: OFF | Headlight: ON | Autonomous: OFF
```

- Node 1 reports live raw ADC and raw pin-voltage estimate, wireless steering angle/sequence/age, rejected packet counters, successful/failed sends and time since its last successful send every 250 ms. Its CAN pedal payload contains the original ADC value, including the idle baseline, with no voltage subtraction. At boot it prints its steering receiver STA MAC and ESP-NOW channel.
- Node 2 prints raw ADC, corrected voltage, requested/output percentage, applied reverse, all seven switch requests and stop reason every 250 ms; `0x201` reports output commands and `0x600` reports health. These are software commands, not motor/relay measurements.
- Node 3 prints every named switch after debounce and successful sends, plus counters and pending-send failures every 250 ms. Initialization failures retry every second.
- Nodes 4/5 print decoded RX/TX values, controller setup attempts, send failures (rate-limited to once per second), and a one-second controller summary. Node 5 also prints WiFi clients, filesystem state and Node 1/3 freshness.
Heartbeat byte counters wrap at 256; the local Serial totals use 32-bit counters.

A successful controller initialization checks SPI/controller setup; physical CAN sends need another active node to acknowledge them. TX failures report wiring/termination/peer checks.

## Dashboard and upload

Each node has its own Arduino sketch folder. Open the matching `.ino` directly:

- [node1_bcm/node1_bcm.ino](node1_bcm/node1_bcm.ino): accelerator sender and steering receiver/output.
- [node2_pcm/node2_pcm.ino](node2_pcm/node2_pcm.ino): throttle/switch CAN receiver, dual PWM, reverse relays and output/health telemetry.
- [node3_ccm/node3_ccm.ino](node3_ccm/node3_ccm.ino): switch sender.
- [node4_tcu/node4_tcu.ino](node4_tcu/node4_tcu.ino): GPS, ignition and pitch.
- [node5_dash/node5_dash.ino](node5_dash/node5_dash.ino): WiFi dashboard and CAN log.

Every folder already contains its required project headers, so it can also be copied elsewhere and opened independently. Shared header sources remain at the repository root. After editing a root header, run `python3 dashboard/tools/sync-sketch-headers.py` to update its copies, or add `--check` to check consistency without changing files.

In Arduino IDE, choose **File → Open**, select one of the `.ino` files above, and compile/upload that node separately. Use classic **ESP32 Dev Module**, with the ESP32 board package, `mcp_can`, `TinyGPSPlus`, `Adafruit_MPU6050`, `Adafruit_Sensor` and `Adafruit_BusIO` libraries. Node 5 needs **No OTA (2MB APP/2MB SPIFFS)** and a LittleFS uploader. Its upload assets are already inside **`node5_dash/data/`**; the dashboard build writes there directly.

```bash
cd dashboard
npm ci
npm run build:esp
```

Upload each node sketch from its own folder. With `node5_dash/node5_dash.ino` open, upload **`node5_dash/data/`** as **LittleFS**; keep the `.gz` filenames. Join `srt-dash` with the configured initial password `12345678`, then open `http://192.168.4.1`.

Throttle voltage is `max(0, ADC × 3.3 / 4095 − 0.7)` volts. The gauge shows `floor(correctedVoltage × 100 / 2.6)`, limited to 0–100%; this is an ADC estimate, not measured motor output. Raw ADC and raw pin-voltage estimate remain available under Node 1 on `/status`, and the two-byte `0x301` payload remains raw/unchanged. Example: ADC 2000 → raw 1.612 V → corrected 0.912 V → 35%. Missing readings remain unavailable, not zero.

The switch panel shows all seven inputs. Node 1 accelerator data expires after 500 ms and Node 3 after 3000 ms. Node 2 output telemetry expires after 250 ms; its heartbeat expires after 3 s. Rebuild/upload Node 5 firmware and frontend to show the new Node 2 health and commanded outputs.

### Node 2 throttle and reverse

The rewrite restores the Node 2 production wiring and the 40% ECO target cap from `ec18a47`, adapting the receiver to the current short frames. Pedal scaling uses the same 0.7 V idle subtraction as the dashboard. The target duty is `floor(throttlePercent × 255 / 100)`. Both PWM outputs follow it with a **linear 400 ms full-scale ramp in either direction**: a 25 percentage-point change takes about 100 ms. Fast pedal presses, ordinary releases, target changes and ECO transitions all use this ramp. Enabling ECO above 40% ramps down toward its 40% target. All seven switch inputs are decoded; cruise, hill and autonomous requests do not create throttle hold. Horn/headlight remain requests only on Node 2.

`THROTTLE_RAMP_UP_MS` and `THROTTLE_RAMP_DOWN_MS` in `node2_pcm/node2_pcm.ino` set the full-scale rise/fall times. The ramp uses elapsed time and fractional PWM steps, without blocking delays. A delayed loop contributes at most 10 ms of ramp progress, avoiding a large catch-up step; under sustained loop delays the ramp takes longer. `0x201` byte 3 reports the **applied PWM rounded to whole percent**, not the target; its motor-on flags follow nonzero PWM even when a very small duty rounds to 0%. Serial shows pedal, target, applied percent and PWM counts separately.

Node 2 stops PWM at 250 ms without a valid pedal frame, or 3001 ms without valid switches, allowing Node 3's normal one-second broadcast. Invalid input, CAN send failure and PWM failure also stop PWM. Startup and recovery require an idle pedal before applying throttle. A direction change cuts PWM immediately, requires a continuous zero command for one second, switches both relays, then waits a further second and requires idle before throttle can resume. Reverse stays engaged while reverse throttle is applied, and faults hold the last direction while stopping PWM. This interlock measures zero-command time, not vehicle speed; shift only while physically stationary. No brake input or output exists in this wiring.

`0x201` byte 7 and Node 2 heartbeat error codes: 0 ready, 1 CAN unavailable/send failure, 2 PWM setup/write failure, 3 pedal missing/stale/invalid, 4 switches missing/stale/invalid, 5 release pedal, 6 direction change/settling, 7 traction disabled, 8 reverse relays disabled. PWM setup/write failure remains stopped until reboot. Motor-on bytes describe nonzero software PWM commands, not sensed rotation.

The home page contains driving instruments: GPS speed, wireless steering angle, corrected accelerator percentage, trip, heading and satellites. **MAP** opens the offline map fullscreen; **CLOSE** or Escape returns. Its SRT opening animation runs once per tab session and honors reduced motion. The wheel follows **Node 1's wireless steering angle**; missing feedback freezes the last angle and shows unavailable. Node 4 pitch remains a separate sensor in diagnostics.


### Mobile throttle and reverse

Upload **Nodes 1, 3 and 5 together**, with `RemoteDrive.h` in all three sketch folders, plus rebuilt Node 5 LittleFS assets. Node 2 now implements the receiver for `0x301`, DLC 2, raw 12-bit ADC little-endian, and `0x300`, DLC 1, reverse in bit 2. Upload Node 2 with `CanCommon.h` and `ThrottleCalibration.h`; its historical output-enable defaults must be configured for motor operation as described above. Physical motor/relay behavior still requires hardware verification.

Open **http://192.168.4.1/control** directly (also `/control/`). The homepage has no CONTROL button. Hold THROTTLE with one finger and LEFT/RIGHT with another. Moving focus between buttons keeps touch holds active. Releasing either control stops only that input; **STOP** cancels both. The separate `/control` page has an adjustable **ADC %**, initially zero: `ADC = percent × 4095 / 100`. This is the transmitted raw input scale, not traction PWM or the offset-corrected gauge percentage. Hold THROTTLE to send it; release to send zero. REVERSE selects the next direction only with no held throttle and at least one second at zero. A reversal holds the old direction at zero for one second, switches direction, then waits another second before throttle. This interlock measures zero command time, **not vehicle speed**; select direction only when physically stationary.

The host first asks Nodes 1/3 to silence their normal `0x301`/`0x300` broadcasts. Both must acknowledge the same session before the host sends simulated input every 50 ms. Physical pedal above the 0.7 V idle threshold, missing acknowledgements, CAN send failure, 350 ms command expiry or a 5-second hold ends the gesture. Nodes independently latch zero throttle/held direction on lease expiry. Cruise and autonomous bits are cleared during simulation; other switch bits are preserved. Recovery requires a new press. Loss of the physical CAN bus still requires the working Node 2 receiver's own output watchdog; a CAN zero command is not a brake.

After release, normal switches return before the pedal sender. The physical pedal must be idle and the physical reverse switch must match the last transmitted direction; otherwise zero remains latched. `MATCH REVERSE SWITCH` identifies that condition. `0x120` carries the handoff command; `0x121` and `0x122` report physical inputs and ownership. `/api/drive` GET is read-only; same-origin POST with `X-SRT-Control: 1` accepts `action=throttle` (`token`, increasing `seq`, `percent`), `action=reverse` (`token`, `reverse=0|1`), or `action=stop`. Outgoing simulation frames are marked `tx` in the bounded CAN log. `/api/vehicle.remoteDrive` reports command state and physical input/gate feedback separately.

### Remote steering

Upload **both Node 1 and Node 5**, and upload the rebuilt `data/` LittleFS assets to Node 5. Join dashboard WiFi and open **http://192.168.4.1/control** directly (also `/control/`). Hold LEFT/RIGHT to turn at PWM 60. Release, drag off the button, cancel touch, switch tabs, leave the control page or press STOP to stop. Space/Enter supports keyboard holds. Reconnection never resumes a previous press.

Node 1 independently stops after **250 ms without valid advancing ESP-NOW angle packets**, **350 ms without a new valid CAN command**, on CAN send failure, or after **5 seconds of continuous hold**. Both outputs start at zero. Every stop changes the command token, and duplicate/out-of-order commands cannot extend a hold. A direction change requires releasing first. The controls require live `0x111` output status and `0x100` angle feedback. The dashboard also expires its HTTP command lease at 350 ms; source age and CAN age both count toward angle expiry.

`GET /api/steering` is read-only. Commands use same-origin `POST /api/steering` with `X-SRT-Control: 1` and form fields `direction`, `token`, `seq`; STOP needs only `direction=stop`. HTTP acknowledges CAN transmission. Applied PWM comes from `0x111`; current angle, availability, age and sequence come from `0x100` as `angleDeg`, `angleAvailable`, `angleAgeMs` and `angleSequence`. The control panel shows the current angle separately from motor direction. Successful steering and throttle/handoff transmissions appear as `tx` frames in the CAN log.

Node 1 steering stop reasons: 0 normal, 1 CAN failure, 2 command timeout, 3 invalid command, 4 hold limit, **5 steering angle unavailable**. `/status` highlights unavailable steering or nonzero stop reasons. A new valid press or STOP clears the reason. Steering outputs remain on Node 1; traction PWM and reverse relays are on Node 2.

### Steering-wheel ESP-NOW input

Node 1 starts an ESP-NOW receive callback at boot in station mode with sleep disabled. `STEERING_WIFI_CHANNEL` in `node1_bcm.ino` defaults to **1**. This is a wireless receiver, so no TCP/UDP port or GPIO input is opened. The existing transmitter must use the same radio channel and target Node 1's **STA MAC printed at 115200 baud**. Check that it matches the transmitter's configured `00:4B:12:34:C8:74`; receiver firmware cannot redirect packets sent to another MAC. Unencrypted reception does not require adding a sending peer on Node 1 ([Espressif ESP-NOW documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)).

The incoming packet exactly matches the supplied transmitter: packed **float steeringAngle + uint32_t sequence**, 8 bytes total, native ESP32 little-endian. Node 1 rejects wrong lengths, NaN/infinite values, angles outside −180…180°, duplicates, and backward sequences while the stream is fresh. Sequence and millis rollover are handled. A transmitter sequence reset is accepted after the 250 ms feedback timeout. Returning feedback never restarts a stopped hold; the user must press again. No transmitter sketch was added or changed.

The received angle is the current steering-wheel measurement. Existing LEFT/RIGHT holds remain the motor commands, with live-angle feedback required. There is no automatic target-angle/PID control or inferred mechanical travel limit. The supplied gyro integration measures a relative angle from its startup center and can drift; the receiver does not create an absolute steering reference.

Open **http://192.168.4.1/status** directly for diagnostics (also `/status/`). There is deliberately no homepage link to it. Tap a node for its sensors, readings and freshness. Node 2 uses its heartbeat for health and `0x201` for commanded PWM/reverse; older firmware without a heartbeat remains unknown. CAN log supports exact hex ID, node, message/raw-byte search and issues-only filters. **RESET** clears filters. **PAUSE** freezes the browser snapshot while CAN reception continues; **COPY** and **EXPORT** include filtered frames, diagnostics and recent events. The browser retains at most 64 frames, 24 fault transitions and 600 route points.

`/api/vehicle` returns decoded data and node states. `/api/gps` returns a GPS fix; `/api/data` returns speed estimated from successive fixes. Missing/stale signals are `null`. `/api/can-log` returns the latest 64 frames, `uptimeMs`, and exactly `dlc` bytes per frame. The optional exact hex filter `/api/can-log?id=0x301` still works; invalid IDs return HTTP 400. `/api/diagnostics` returns per-signal freshness, invalid-payload counts, total RX/TX/errors, free/minimum/largest heap block, LittleFS usage and WiFi clients. `/status` now returns HTML, not JSON; consumers of its old JSON must use `/api/vehicle`.

## Automated checks

Run `python3 dashboard/tools/sync-sketch-headers.py --check` to check the local header copies. Run `python3 dashboard/tools/test-powertrain.py` for the actual Node 2 receiver/PWM/relay code, including linear rise/fall, target changes, loop jitter/stalls, applied-output telemetry, ECO transitions, disabled defaults, enabled outputs, reverse interlocks, immediate fault stops, expiry and recovery. Run `python3 dashboard/tools/test-steering.py` for Node 1's ESP-NOW callback, GPIO adapter, packet validation and command/feedback watchdogs. Run `python3 dashboard/tools/test-senders.py` for the actual Node 1/3/4 sketches: unchanged raw pedal transmission for all 4,096 ADC values, debounce, switch handoff, GPS boundary rounding, ignition byte order, IMU encoding, expiry and bounded receive loops. This also compares all 4,096 ADC values between firmware and browser throttle scaling. These tests read each sketch and its local headers from the node's folder.

Run `node --test dashboard/tools/diagnostics.test.mjs dashboard/tools/steering-client.test.mjs dashboard/tools/drive-client.test.mjs` for decoders and browser command-race tests. Run `python3 dashboard/tools/test-can-protocol.py` from the repository root (requires `g++`) for Node 5's actual decoder/API functions against a simulated CAN bus: endian order, all switch bits, angle/source age, data expiry, invalid flags, handoffs, log filtering and clock wraparound. `dashboard/tools/smoke-packed.cjs` checks the packed website in Playwright, including wireless angle display/freeze, controls, mobile CAN filters and missing-data display.

The GPS `0x400` format has no coordinate sign bits; Node 4 reports a fix only for nonnegative coordinates. Firmware compilation and simulated/browser checks do not test physical boards, wiring or sensors.


### Verification (2026-10-22)

All five sketches compile separately with ESP32 core **3.3.11** and `mcp_can` **1.5.1**. Nodes 1–4 use `esp32:esp32:esp32`; Node 5 uses `esp32:esp32:esp32:PartitionScheme=no_ota`.

| Sketch | Flash bytes | Static RAM bytes | Result |
| --- | ---: | ---: | --- |
| Node 1 | 910,919 / 1,310,720 | 45,972 / 327,680 | PASS |
| Node 2 | 293,087 / 1,310,720 | 22,512 / 327,680 | PASS |
| Node 3 | 281,948 / 1,310,720 | 22,364 / 327,680 | PASS |
| Node 4 | 316,568 / 1,310,720 | 24,044 / 327,680 | PASS |
| Node 5 | 1,007,988 / 2,097,152 | 49,480 / 327,680 | PASS |

- Native tests pass for Node 1's wireless callback/output adapter, Node 2's actual receiver/output code, Node 3/4's actual sender sketches, and Node 5's decoder, HTTP control and handoff functions.
- Node 2's 400 ms rise/fall ramp passes linearity, release, retargeting, scheduling, applied-output telemetry, ECO and immediate-fault tests. It also compiles with traction and reverse outputs enabled: 293,699 bytes flash and 22,520 bytes static RAM. Default builds retain the historical output-enable settings.
- Transfer checks cover packet size, signed angle, little/big-endian fields, all seven switch bits, input domains, sequence/millis wrap, source/CAN age, ownership, command expiry and fault recovery. Firmware and browser throttle percentages match for **all 4,096 ADC values**.
- **13 JavaScript tests pass**. Both `dashboard` and `car_fleet` pass lint and production builds. The fleet application uses saved demo records; it has no live ESP32 telemetry connection.
- The packed dashboard passes Playwright checks at 320/390 px phones, 800×480 and desktop sizes: current steering angle and freeze on loss, simultaneous steering/throttle, STOP, release/cancel/blur, reconnection, routes, map loading, filters, copy/export and API recovery.
- Rebuilt `node5_dash/data/` contains 229 files. JS/CSS is about 124 KB gzip. `mklittlefs -c node5_dash/data -b 4096 -p 256 -s 1966080 build/srt-littlefs.bin` creates the **1,966,080-byte** LittleFS image for the No OTA partition at `0x210000`.
- The audit fixed GPS rounding at degree boundaries, inconsistent payload-domain validation, unbounded receive batches, missing steering TX log entries and incomplete Node 1/3 send counters. The dashboard build no longer uses the deprecated bundler option.

These checks use compiled firmware, native I/O mocks and a browser, without flashing boards. Physical CAN acknowledgements, radio channel/MAC matching, sensor centering/drift, motor direction and runtime latency remain on-device checks. Node 2's historical output-enable defaults remain zero; GPS coordinates retain the unsigned format described above.
