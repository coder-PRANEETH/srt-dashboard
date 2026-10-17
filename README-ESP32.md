# SRT CAN dashboard and vehicle nodes

Five classic ESP32 vehicle nodes share a 500 kbps CAN bus. A separate steering IMU ESP32 sends its angle wirelessly to Node 1 by ESP-NOW. The Node 5 ESP32 hosts the offline dashboard over its WiFi access point. `dashboard` and `car_fleet` are the two website sources. Each `nodeN_*` folder, plus `steering_imu_esp`, has a `test` sketch for bench diagnostics and a `production` sketch for its role.

Every test and production `.ino` starts with its own connection list and verification procedure. Test sketches run on one board at a time: Nodes 1-5 use MCP2515 internal loopback, so no second CAN node or CANH/CANL bus is required. Loopback checks the local SPI controller path, not the external transceiver or bus wiring.

## Wiring and build

All MCP2515 modules: GPIO18 SCK, GPIO19 MISO, GPIO23 MOSI, GPIO25 CS, GPIO27 INT. The sketches select an **8 MHz MCP2515 oscillator**. Confirm the module crystal; change `MCP_8MHZ` in each sketch's `CanCommon.h` if it is 16 MHz. Use 3.3 V compatible logic on ESP32 inputs, twisted CANH/CANL, common ground, and one 120 Ω terminator at each bus end.

- Node 1 BCM: GPIO34 pedal ADC, GPIO16/17 Pi5 UART, ESP-NOW steering receiver on WiFi channel 1, GPIO13 horn, GPIO32 LPWM and GPIO33 RPWM on the BTS7960 steering driver. The MPU6050 is **not** on Node 1. The headlight output is intentionally unused.
- Node 2 PCM: GPIO13/14 motor PWM, GPIO26/33 reverse relays. **GPIO22 brake sensing has been removed.**
- Node 3 CCM: active-low switches to ground on GPIO4/5/13/14/26/32/33.
- Node 4 TCU: GPS TX to GPIO16, ignition logic to GPIO34, MPU6050 SDA/SCL to GPIO21/22. The IMU X-axis is treated as longitudinal for pitch; verify its mounting sign on the bench.
- Node 5 DASH: MCP2515 plus WiFi; no local speed potentiometer.
- Steering IMU ESP32: MPU6050 SDA/SCL on GPIO21/22, ESP-NOW transmitter on channel 1; it is not connected to CAN.

Open each `test` or `production` folder as a separate Arduino sketch. Install the ESP32 board package, `mcp_can`, `TinyGPSPlus`, `Adafruit_MPU6050`, `Adafruit_Sensor`, and `Adafruit_BusIO`. Use a classic ESP32 Dev Module; Node 5 also needs the **No OTA (2MB APP/2MB SPIFFS)** partition and LittleFS uploader. Build the site and pack its assets:

```bash
cd dashboard
npm ci
npm run build:esp
```

Upload `node5_dash/production/production.ino`, then upload `node5_dash/production/data/` as **LittleFS**. The packer gzips HTML, CSS, JS, and SVG and keeps the `.gz` names; do not manually recompress the folder. Connect to WiFi `srt-dash` (initial password `12345678`, change in the Node 5 sketch) and open `http://192.168.4.1`.

Node 5 APIs: `/api/vehicle` exposes decoded CAN signals and five node health states, `/api/gps` exposes the current fix, `/api/data` exposes speed estimated from successive GPS fixes, and `/api/can-log` exposes the latest 64 frames. Values become `null` when the related frame is stale; heartbeats go offline after 3 seconds. The CAN matrix does not define a vehicle speed signal, so GPS speed is an estimate and stays unavailable until two usable fixes arrive.

## Bench and activation notes

Open each test sketch's Serial Monitor at 115200. Nodes 1-5 print local CAN loopback sent/pass counts using test IDs `0x7A1` through `0x7A5`; both counts should rise on the single board. Node 1 checks its pedal ADC, ESP-NOW radio initialization, and UART with a temporary GPIO17-to-GPIO16 jumper. Its keyboard motor check is `m` to arm for five seconds, then `l` or `r` for one 25% PWM pulse lasting 250 ms; `s` stops immediately. The horn test is `m` then `h`. Node 2 uses `m` then `1`/`2` for PWM or `3`/`4` for relay input pulses, with `s` for stop. Each pulse consumes the arm. Keep steering movement clear and use a fused 12 V motor supply for the motor check; start with a meter on output pins. Node 3 checks each switch, Node 4 checks GPS/ignition/MPU6050, and Node 5 checks its access point and LittleFS. The steering IMU test checks its own MPU6050 and radio initialization without sending to Node 1. A single board cannot prove the external CAN transceiver or ESP-NOW delivery; use production firmware on both ends for the integration check.

The steering IMU production sketch targets Node 1 MAC `30:76:F5:94:6A:60`, supplied with the working wireless pair. Confirm `node1_bcm/test` prints that same MAC. Production sender and receiver use the packed `{uint32_t sequence; float steeringAngle;}` payload on WiFi channel 1. The sender calibrates the MPU6050 gyro while still, samples I2C at 400 kHz, and sends every 10 ms; Node 1 queues received packets, reports the latest angle over CAN every 20 ms, and marks the wireless reading stale after 500 ms without a packet.

The Node 1 horn relay is configured active high: GPIO13 sends 3.3 V to the relay input while the CAN horn request is fresh. The relay switches the separate 12 V horn supply. Production traction and reverse relay outputs remain locked by default. They require motor PWM limits, bench validation, and an explicit constant change in the Node 2 production sketch. Reverse relay inputs are active high at 3.3 V as confirmed by the user. The 12 V BTS7960 steering driver uses GPIO32 LPWM and GPIO33 RPWM, with both enable inputs tied to 5 V. Add external pulldowns on LPWM/RPWM so the motor stays off during ESP32 reset and boot; the Node 1 production sketch drives both low once running. Closed-loop angle targets and end stops still need confirmation. Production steering drive, remote immobilization, cruise throttle hold, hill hold throttle, and summon actuation stay inactive because the supplied pin sheets/matrix do not define a safe closed-loop controller or secure command authentication. Node 4 sends valid measured pitch in 0x500 when its MPU6050 responds, while holding throttle remains zero. Telemetry for defined frames is active. Brake is unavailable after removal of its input.

The 0x400 GPS format has no latitude/longitude sign bits; Node 4 therefore sends a fix only for nonnegative coordinates. No physical boards were available for validation; compile and browser checks do not prove wiring, sensor operation, or actuator safety.
