# SRT car dashboard

This is the site served by `esp32_dashboard_host`. It displays the potentiometer speed from `/api/data` and the filtered CAN GPS fix from `/api/gps`. The route map uses only local tiles under `public/tiles`; there are no hosted fonts or map requests.

```bash
npm install
npm run dev       # local preview; live values need the ESP32 access point
npm run build
python3 tools/pack-fs.py
```

The packer creates `../esp32_dashboard_host/data/` for LittleFS upload and checks the partition size. See [the ESP32 guide](../README-ESP32.md) for wiring and upload steps.
