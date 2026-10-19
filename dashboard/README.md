# SRT offline vehicle dashboard

This website is served by the Node 5 ESP32 from LittleFS. It reads decoded live CAN data from `/api/vehicle`, GPS from `/api/gps`, and raw frames from `/api/can-log`. The route map uses local tiles in `public/tiles` and works without internet.

```bash
npm ci
npm run dev
npm run build:esp
```

`build:esp` builds and compresses the site into `../data/`. Upload that folder with the Node 5 production sketch and the No OTA partition. See [the ESP32 guide](../README-ESP32.md).


## Cockpit and diagnostics

`/` is the minimal black/orange cockpit. White labels and rules, a brief SRT intro and a pitch-driven wheel use local CSS/SVG only. The offline map mounts only after **MAP** is pressed. No CAN or diagnostics navigation appears on the homepage.

Open `/status` or `/status/` directly for node sensors, CAN log filtering/export, fault transitions and host memory/storage. Node 2 cannot report its state and remains unknown. Deploy the updated Node 5 firmware together with `data/` to provide `/api/diagnostics` and the HTML status route.

Verification from the repository root:

```sh
python3 dashboard/tools/test-can-protocol.py
node --test dashboard/tools/diagnostics.test.mjs
cd dashboard
npm run lint
npm run build:esp
# Requires Playwright and Chromium; optionally set PLAYWRIGHT_MODULE and BROWSER_PATH.
node tools/smoke-packed.cjs
```

The CAN buffer is 64 frames; browser events are capped at 24 and track points at 600. No new runtime packages, remote fonts, video or CDN assets are used. See `../README-ESP32.md` for partition sizes and upload instructions.
