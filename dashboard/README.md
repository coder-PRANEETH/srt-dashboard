# SRT car dashboard

This is the site served by `esp32_dashboard_host`. It displays the potentiometer speed from `/api/data`, the filtered CAN GPS fix from `/api/gps`, and a live CAN frame table from `/api/can-log`. The route map uses only local tiles under `public/tiles`; there are no hosted fonts or map requests.

```bash
npm ci
npm run dev       # local preview; live values need the ESP32 access point
npm run build:esp # build and pack the host's upload folder
```

The packer creates `../esp32_dashboard_host/data/` with gzipped HTML, JS, CSS, and SVG assets for LittleFS upload. It estimates usage against the `no_ota` partition and reserves room for filesystem metadata. See [the ESP32 guide](../README-ESP32.md) for wiring and upload steps.
