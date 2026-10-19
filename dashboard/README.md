# SRT offline vehicle dashboard

This website is served by the Node 5 ESP32 from LittleFS. It reads decoded live CAN data from `/api/vehicle`, GPS from `/api/gps`, and raw frames from `/api/can-log`. The route map uses local tiles in `public/tiles` and works without internet.

```bash
npm ci
npm run dev
npm run build:esp
```

`build:esp` builds and compresses the site into `../data/`. Upload that folder with the Node 5 production sketch and the No OTA partition. See [the ESP32 guide](../README-ESP32.md).
