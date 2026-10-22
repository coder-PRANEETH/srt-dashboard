# SRT offline vehicle dashboard

This website is served by the Node 5 ESP32 from LittleFS. It reads decoded live CAN data from `/api/vehicle`, GPS from `/api/gps`, and raw frames from `/api/can-log`. The route map uses local tiles in `public/tiles` and works without internet.

```bash
npm ci
npm run dev
npm run build:esp
```

`build:esp` builds and compresses the site directly into `../node5_dash/data/`. Open `../node5_dash/node5_dash.ino` in Arduino IDE and upload its data folder with LittleFS and the No OTA partition. Every node now has a separate sketch folder with its project headers included. See [the ESP32 guide](../README-ESP32.md).


## Cockpit and diagnostics

`/` is the minimal black/orange cockpit. White labels and rules, a brief SRT intro and a wheel driven by Node 1's wireless steering angle use local CSS/SVG only. The offline map mounts only after **MAP** is pressed. Open `/control` or `/control/` directly for press-and-hold LEFT/RIGHT controls at PWM 60 on Node 1 GPIO32/33, with current angle, STOP and command expiry. Missing ESP-NOW feedback stops steering after 250 ms. No control button, CAN or diagnostics navigation appears on the homepage.

Open `/status` or `/status/` directly for node sensors, CAN log filtering/export, fault transitions and host memory/storage. Rewritten Node 2 reports its commanded PWM/reverse through `0x201` and health through `0x600`; older receive-only firmware remains unknown. Deploy updated Node 2 and Node 5 firmware with rebuilt `data/` on Node 5 to show this feedback.

Verification from the repository root:

```sh
python3 dashboard/tools/sync-sketch-headers.py --check
python3 dashboard/tools/test-can-protocol.py
python3 dashboard/tools/test-powertrain.py
python3 dashboard/tools/test-senders.py
python3 dashboard/tools/test-steering.py
node --test dashboard/tools/diagnostics.test.mjs dashboard/tools/steering-client.test.mjs
cd dashboard
npm run lint
npm run build:esp
# Requires Playwright and Chromium; optionally set PLAYWRIGHT_MODULE and BROWSER_PATH.
node tools/smoke-packed.cjs
```

The CAN buffer is 64 frames; browser events are capped at 24 and track points at 600. No new runtime packages, remote fonts, video or CDN assets are used. See `../README-ESP32.md` for partition sizes and upload instructions.

Throttle estimates subtract 0.7 V and clamp to zero; raw ADC/voltage remain in diagnostics. Both Node 1 and Node 5 require `ThrottleCalibration.h`, `SteeringControl.h` and `SteeringFeedback.h` beside their sketches. CAN `0x111` reports applied PWM; `0x100` reports wireless IMU angle, source age, link state and sequence. Node 1 listens for the existing packed 8-byte ESP-NOW packet on channel 1 and prints its receiver STA MAC at boot. See the ESP32 guide for the channel/MAC requirements.
