# SRT offline vehicle dashboard

This website is served by the Node 5 ESP32 from LittleFS. It reads decoded live CAN data from `/api/vehicle`, GPS from `/api/gps`, and raw frames from `/api/can-log`. The route map uses local tiles in `public/tiles` and works without internet.

```bash
npm ci
npm run dev
npm run build:esp
```

`build:esp` builds and compresses the site directly into `../node5_dash/data/`. Open `../node5_dash/node5_dash.ino` in Arduino IDE and upload its data folder with LittleFS and the No OTA partition. Every node now has a separate sketch folder with its project headers included. See [the ESP32 guide](../README-ESP32.md).


## Cockpit and diagnostics

`/` is the minimal black/orange cockpit. White labels and rules, a brief SRT intro and a wheel driven by Node 1's wireless steering angle use local CSS/SVG only. The offline map mounts only after **MAP** is pressed. Open `/control` or `/control/` directly for the **circular joystick**: north/south requests forward/reverse traction, east/west requests steering motor speed, both 0–255 PWM. NE at full travel is 180 throttle / 180 right steering. Separate limits, requested/applied PWM, live angle and a fixed STOP button are visible. Drag distance scales speed; release stops both axes. Arrow keys work too. **BUTTONS** retains the older separate controls with PWM 60 steering. Missing ESP-NOW feedback hides the angle reading; steering motor-speed controls remain available while Node 1 output status is live. Command expiry and STOP remain effective. `/controls` and `/controls/` are aliases for the control page. No control button, CAN or diagnostics navigation appears on the homepage.

Upload updated **Nodes 1, 2, 3 and 5** plus Node 5 LittleFS assets. Joystick requests use one `/api/joystick` POST at up to 20 Hz. Node 5 waits for ownership acknowledgements from all three control nodes, then sends Node 2 demand above idle on `0x123` and variable Node 1 steering on `0x110`; `0x124` reports motor ownership/applied PWM. Physical pedal presses are ignored during a held remote session. Entry and handback require an idle pedal; connection loss/STOP latches the 0.8 V-equivalent idle signal. Existing direction interlocks, 1,000 ms rise / 400 ms fall, five-second hold limit and watchdogs remain. Steering PWM sets motor speed, not wheel angle; releasing does not center the wheels.

Open `/status` or `/status/` directly for node sensors, CAN log filtering/export, fault transitions and host memory/storage. Rewritten Node 2 reports its commanded PWM/reverse through `0x201` and health through `0x600`; older receive-only firmware remains unknown. Deploy updated Node 2 and Node 5 firmware with rebuilt `data/` on Node 5 to show this feedback. Node 2 now defaults to enabled outputs and the supplied working **active-LOW** reverse wiring (GPIO33/26: HIGH forward, LOW reverse). `/status` names its stop reasons, and `/api/vehicle.powertrainReason` reports the current reason or `null` for stale data.

Verification from the repository root:

```sh
python3 dashboard/tools/sync-sketch-headers.py --check
python3 dashboard/tools/test-can-protocol.py
python3 dashboard/tools/test-powertrain.py
python3 dashboard/tools/test-senders.py
python3 dashboard/tools/test-steering.py
node --test dashboard/tools/diagnostics.test.mjs dashboard/tools/steering-client.test.mjs dashboard/tools/drive-client.test.mjs dashboard/tools/joystick.test.mjs
cd dashboard
npm run lint
npm run build:esp
# Requires Playwright and Chromium; optionally set PLAYWRIGHT_MODULE and BROWSER_PATH.
node tools/smoke-packed.cjs
node tools/smoke-joystick.cjs
```

The CAN buffer is 64 frames; browser events are capped at 24 and track points at 600. No new runtime packages, remote fonts, video or CDN assets are used. See `../README-ESP32.md` for partition sizes and upload instructions.

Throttle estimates subtract 0.8 V and clamp to zero; raw ADC/voltage remain in diagnostics. Both Node 1 and Node 5 require `ThrottleCalibration.h`, `SteeringControl.h` and `SteeringFeedback.h` beside their sketches. CAN `0x111` reports applied PWM; `0x100` reports wireless IMU angle, source age, link state and sequence. Node 1 listens for the packed 8-byte angle/sequence packet and the newer 9-byte packet with hornPressed on channel 1 and prints its receiver STA MAC at boot. See the ESP32 guide for the channel/MAC requirements.

Node 3 uses six active-low switches on `0x300`, DLC 1: Cruise (GPIO4/bit0), HillHold (GPIO5/bit1), Autonomous (GPIO13/bit2), Remote mode (GPIO14/bit3), Horn (GPIO26/bit4), Reverse (GPIO33/bit5). Bits 6–7 are reserved. The status panel and CAN decoder use this order; ECO/headlight inputs and GPIO32 have been removed. Update Nodes 1–5 together and upload the rebuilt LittleFS assets.

Physical throttle matches the supplied working Node 2 receiver: `percent = floor(ADC × 100 / 4095)` and `PWM = floor(percent × 255 / 100)`, with a 62/255 minimum output and the 1,000 ms rise / 400 ms fall. All 4,096 raw ADC targets are tested against that baseline with the idle floor. ADC 2000 therefore targets 48% / PWM 122, while the independent corrected dashboard gauge reads 32%. Nonzero released-pedal input up to ADC 992 (0.8 V estimate) is accepted for idle arming and interlocked shifts. The latest sender's reverse bit 5 requires the matching updated receiver; the old supplied receiver reads bit 2 and cannot be mixed with it.

Button throttle defaults to 100%, matching joystick full north. Both use the same `0x123` demand path, ramp and idle voltage. Reverse performs a zero-demand transaction until Node 2 reports the requested applied direction; the UI shows SWITCHING until then. Joystick/drive motion replies allow 300 ms, while the firmware lease remains 350 ms. Node 5 retains web CAN logging but disables blocking per-frame UART output. Touch edge/corner tests include a 270 ms reply delay.
