# SRT CAN dashboard and vehicle nodes

The dashboard uses a 500 kbps CAN bus. Node 1 reads the accelerator and drives the steering motor; Node 2 receives throttle/switch inputs and controls traction and reverse; Node 3 sends switch inputs. Node 5 reads GPS directly over UART and hosts the offline dashboard and CAN log over WiFi. The active node IDs are 1, 2, 3 and 5; Node 4 and ignition handling have been removed.

## Current CAN protocol

| Sender | ID | DLC | Data | Timing |
| --- | --- | --- | --- | --- |
| Node 1 | `0x100` | 8 | Signed steering angle BE16 in 0.1°, source age in ms (255=saturated/unknown), live flag, ESP-NOW sequence LE32 | Every 50 ms and on link change |
| Node 5 → Node 1 | `0x110` | 8 | Direction (0 STOP, 1 LEFT, 2 RIGHT), PWM (0 for STOP; 1–255 while turning), sequence LE16, Node 1 token LE32 | Joystick updates at up to 20 Hz; buttons retain duty 60 |
| Node 1 | `0x111` | 8 | Applied direction, PWM, stop reason, protocol version=2, token LE32 | Every 100 ms and on change |
| Node 1 | `0x102` | 8 | Wheel pitch: signed BE16 tenths, source age, valid flag, sequence LE32; display only | Every 50 ms |
| Node 1 | `0x301` | 2 | Raw ADC 0–4095, **little-endian**: low byte then high byte | Every 50 ms |
| Node 2 | `0x201` | 8 | Motor 1 on, motor 2 on, applied reverse, output %, cruise active, reserved=0, hill active=0, stop reason (`/api/vehicle.powertrainReason`) | Every 50 ms |
| Node 3 | `0x300` | 1 | Switch bitmask: cruise=0, HillHold=1, autonomous=2, remote mode=3, horn=4, **reverse=5**; bits 6–7 reserved=0 | On change after 25 ms debounce, plus every 1 s |
| Node 5 → Nodes 1/2/3 | `0x120` | 8 | Operation; flags (bit 0 reverse, bit 1 direct PWM); session LE32; sequence LE16 | Every 50 ms while held/returning ownership |
| Node 1 / Node 3 | `0x121` / `0x122` | 8 | Phase + reverse bit 7, physical input LE16, marker `0xD1`, session LE32 | Every 100 ms |
| Node 5 → Node 2 | `0x123` | 8 | Traction demand 0–255 above idle, reverse 0/1, sequence LE16, session LE32 | Every 50 ms during button/joystick control |
| Node 2 | `0x124` | 8 | Phase + **applied reverse** bit 7, applied PWM LE16, marker `0xD1`, session LE32 | Every 100 ms |
| Node 2 / Node 5 | `0x602` / `0x605` | 8 | Node heartbeat and counters; unique arbitration IDs | Every 1 s |

Node 2 now supplies output-command feedback and a heartbeat. Older receive-only Node 2 firmware still shows unknown until a heartbeat arrives. Nodes 1 and 3 send data without heartbeats. The old eight-byte `0x300`/`0x301` layouts are not used; Node 2 rejects them and stops throttle.

Update Nodes 1/2/3/5 together for this six-switch layout and upload the rebuilt Node 5 LittleFS assets. The `0x300` ID/DLC is unchanged, so older firmware cannot distinguish this layout from the previous seven-switch mapping. `SwitchSignals.h` is the shared bit definition; all sketch folders include a synchronized local copy. The vehicle API switch keys are `cruise`, `hill`, `autonomous`, `remoteMode`, `horn`, `reverse`.

## Wiring

All MCP2515 modules: GPIO18 SCK, GPIO19 MISO, GPIO23 MOSI, GPIO25 CS. The common helper used by Nodes 2/5 also configures GPIO27 INT. Use 500 kbps and match the configured **8 MHz crystal** to the module. Connect common grounds and CANH/CANL, with 120 Ω termination at each physical end and 3.3 V-compatible ESP32 logic.

- Node 1: GPIO34 accelerator, **GPIO32 steering LPWM and GPIO33 steering RPWM**. Steering retains the tested `analogWrite` wiring, with joystick duty **1–255** and legacy button duty **60/255**. LEFT drives only LPWM; RIGHT drives only RPWM; STOP sets both to zero. Steering status version 2 advertises variable PWM; old version 1 remains compatible with button mode only. The 1.1–4.52 V sensor output needs the existing divider: **10 kΩ from pedal OUT to GPIO34, 15 kΩ from GPIO34 to GND**. Expected pin voltage is about 0.66–2.71 V. Never connect the 4.52 V output directly to the ESP32. The code sends raw ADC; it does not use saved NVS calibration.
- Node 2: matches the supplied running motor receiver: **GPIO13/14 motor-controller PWM, 1 kHz / 8 bit; GPIO33/26 active-LOW reverse relay inputs**. **LOW = reverse ON; HIGH = forward / relays OFF**. Both relay pins are preloaded HIGH before becoming outputs. No local pedal or brake GPIO. `NODE2_ENABLE_TRACTION_OUTPUTS` and `NODE2_ENABLE_REVERSE_RELAYS` default to **`1`**, so the normal sketch drives the supplied hardware. Override either to `0` for a bench build with that output disabled. Relay inputs are logic signals; keep their separate 12 V load supply off ESP32 pins.
- Node 3: active-low switches to ground: cruise GPIO4, HillHold GPIO5, autonomous GPIO13, remote mode GPIO14, horn GPIO26, reverse GPIO33. GPIO32 is unused. `PUSH_BTN` retains GPIO26 from the existing wiring. Release the GPIO4/5 switches during boot.
- Node 5: MCP2515, WiFi AP, LittleFS dashboard and local GPS. Connect **GPS TX → GPIO16 (RX2)** and optionally **GPS RX ← GPIO17 (TX2)**, with common GND. UART is **9600 baud, 8N1**. The previous receiver only needed GPS TX; leaving GPS RX disconnected is supported. Power the GPS per its module specification; UART logic must be 3.3 V compatible. GPIO34 has no ignition connection.

Node 2 preloads the relay output latches with `gpio_set_level(HIGH)`, then enables the pins and explicitly writes OFF again. ESP32 Arduino core 3.x ignores `digitalWrite` on an unconfigured GPIO, so a `digitalWrite(HIGH)` before `pinMode(OUTPUT)` alone does not establish the initial OFF state. Startup, direction changes and periodic Node 2 logs now show the GPIO33/26 pin readings. These readings describe the ESP32 pins, not relay-contact feedback. Both `0x201` and `0x124` report the applied reverse command; the latter no longer reports the inactive remote gate's default forward direction during physical control.

## Serial diagnosis

Open Serial Monitor at **115200 baud** for every node. Successful transmissions and received project messages print named values and units, not packed payload bytes. Timestamped lines identify direction (`CAN TX` or `CAN RX`), CAN ID and DLC. Examples:

```text
[1000 ms] CAN TX ID=0x301 DLC=2 | accelerator ADC=2000/4095 | raw pin voltage estimate=1.612 V
[1000 ms] CAN TX ID=0x300 DLC=1 | Cruise: ON | HillHold: OFF | Autonomous: ON | Remote mode: OFF | Horn: OFF | Reverse: ON
```

- Node 1 reports live raw ADC and raw pin-voltage estimate, wireless steering angle/sequence/age, rejected packet counters, successful/failed sends and time since its last successful send every 250 ms. Its CAN pedal payload contains the original ADC value, including the idle baseline, with no voltage subtraction. At boot it prints its steering receiver STA MAC and ESP-NOW channel.
- Node 2 prints raw ADC, corrected voltage, requested/output percentage, applied reverse, all six switch requests and stop reason every 250 ms; `0x201` reports output commands and `0x602` reports health. These are software commands, not motor/relay measurements.
- Node 3 prints every named switch after debounce and successful sends, plus counters and pending-send failures every 250 ms. Initialization failures retry every second.
- Node 5 prints local GPS UART/fix/checksum counters, controller summaries, errors, WiFi clients and filesystem state; its per-frame Serial printing is disabled so UART output cannot delay HTTP command leases. The bounded web CAN log and counters still record every frame. Node 2 limits repetitive frame lines to four per second per ID/direction and eight frame lines per second overall, including horn and wheel-pitch frames.
Heartbeat byte counters wrap at 256; the local Serial totals use 32-bit counters.

A successful controller initialization checks SPI/controller setup; physical CAN sends need another active node to acknowledge them. TX failures report wiring/termination/peer checks.

## Dashboard and upload

Each node has its own Arduino sketch folder. Open the matching `.ino` directly:

- [steering_tx/steering_tx.ino](steering_tx/steering_tx.ino): wheel MPU6050 steering/horn/pitch ESP-NOW transmitter.
- [node1_bcm/node1_bcm.ino](node1_bcm/node1_bcm.ino): accelerator sender and steering receiver/output.
- [node2_pcm/node2_pcm.ino](node2_pcm/node2_pcm.ino): throttle/switch CAN receiver, dual PWM, reverse relays and output/health telemetry.
- [node3_ccm/node3_ccm.ino](node3_ccm/node3_ccm.ino): switch sender.
- [node5_dash/node5_dash.ino](node5_dash/node5_dash.ino): local GPS, WiFi dashboard and CAN log.

Every folder already contains its required project headers, so it can also be copied elsewhere and opened independently. Shared header sources remain at the repository root. After editing a root header, run `python3 dashboard/tools/sync-sketch-headers.py` to update its copies, or add `--check` to check consistency without changing files.

In Arduino IDE, choose **File → Open**, select one of the `.ino` files above, and compile/upload that node separately. Use classic **ESP32 Dev Module**, with the ESP32 board package, `mcp_can` and `TinyGPSPlus` libraries. Node 5 needs **No OTA (2MB APP/2MB SPIFFS)** and a LittleFS uploader. Its upload assets are already inside **`node5_dash/data/`**; the dashboard build writes there directly.

```bash
cd dashboard
npm ci
npm run build:esp
```

Upload each node sketch from its own folder. With `node5_dash/node5_dash.ino` open, upload **`node5_dash/data/`** as **LittleFS**; keep the `.gz` filenames. Join `srt-dash` with the configured initial password `12345678`, then open `http://192.168.4.1`.

Throttle voltage is `max(0, ADC × 3.3 / 4095 − 0.8)` volts. The gauge shows `floor(correctedVoltage × 100 / 2.5)`, limited to 0–100%; this is an ADC estimate, not measured motor output. Raw ADC and raw pin-voltage estimate remain available under Node 1 on `/status`, and the two-byte `0x301` payload remains raw/unchanged. Example: ADC 2000 → raw 1.612 V → corrected 0.812 V → 32%. Missing readings remain unavailable, not zero.

The switch panel shows all six inputs. Node 1 accelerator data expires after 500 ms and Node 3 after 3000 ms. Node 2 output telemetry expires after 250 ms; its heartbeat expires after 3 s. Rebuild/upload Node 5 firmware and frontend to show the new Node 2 health and commanded outputs.

### Node 2 throttle and reverse

The receiver retains the supplied running sketches' CAN IDs, lengths, byte order, GPIOs and active-LOW reverse polarity. Physical throttle uses `floor(ADC × 100 / 4095)` followed by `floor(percent × 255 / 100)`, with the requested **minimum PWM of 62/255** on both GPIO13 and GPIO14. That is nominally **0.802 V on a filtered 3.3 V PWM output**. The minimum applies at startup once PWM is attached, idle, STOP, expired/missing commands and CAN faults; failed PWM hardware or a disabled traction build is cleared to zero. Normal output rises linearly from idle to full in **2,500 ms**, and falls from full to idle in **400 ms**. Watchdog/fault stops return immediately to idle. This is an electrical idle command, not a measured motor voltage or braking action. ECO and headlight inputs remain removed; Unavailable Hill assist selection blocks traction at the configured idle output.

For example, ADC 2000 still requests **48% / PWM 122**; the separate corrected dashboard gauge now reads **32%** after subtracting 0.8 V. Native tests compare all **4,096 ADC values** against `max(62, workingReceiverPWM)`. The pedal release threshold is **1.0 V on the GPIO34 estimate**, ADC 0–1240; larger readings block remote entry and manual recovery until released. A direction change first returns to the idle PWM and writes both relay pins in the same control-loop pass. There is no added Node 2 relay delay. Faults hold the last applied direction while returning throttle to idle.

| Switch | GPIO / bit | Implemented behavior |
| --- | --- | --- |
| Cruise | 4 / 0 | Latches the currently applied Node 2 PWM voltage until cruise is switched off |
| HillHold | 5 / 1 | Unavailable with the rotating wheel IMU; selecting it blocks pedal/remote traction at idle, reason 10 |
| Autonomous | 13 / 2 | Request and diagnostic indication; no autonomous controller |
| Remote mode | 14 / 3 | Enables mobile controls; blocks physical pedal throttle and requires the remote handoff protocol |
| Horn | 26 / 4 | Node 3 request/diagnostic; Node 1's wireless horn input controls its GPIO13 relay |
| Reverse | 33 / 5 | Controls both active-LOW reverse relays immediately after cutting PWM, with an idle pedal |

Cruise captures the applied PWM when its switch turns on and holds that output while the switch remains on; turning it off returns control to the pedal. Remote mode blocks manual pedal throttle until the switch is off, and mobile control claims require the switch to be on. Node 2 still requires an idle pedal for remote ownership and ignores pedal updates while a remote session owns the gate. The supplied running motor receiver only actuates throttle and reverse. Node 1's separate GPIO13 relay output combines the fresh wireless steering-horn packet and the website horn lease; the Node 3 horn switch bit remains a request/diagnostic only.

`THROTTLE_RAMP_UP_MS` and `THROTTLE_RAMP_DOWN_MS` in `node2_pcm/node2_pcm.ino` set the full-scale rise/fall times. The ramp uses elapsed time and fractional PWM steps, without blocking delays. A delayed loop contributes at most 10 ms of ramp progress, avoiding a large catch-up step; under sustained loop delays the ramp takes longer. `0x201` byte 3 reports the **applied PWM rounded to whole percent**, not the target; its motor-on flags indicate output above idle PWM 62. Serial shows pedal, target, applied percent and PWM counts separately.

Node 2 returns to the **0.8 V-equivalent idle PWM** after more than 500 ms without a valid pedal frame (millisecond 501), or more than 3000 ms without valid switches (millisecond 3001). Invalid input and CAN send failure also return immediately to idle; PWM setup/write failure clears both outputs. Startup and recovery still require an idle physical pedal before positive throttle is allowed. Direction changes return to idle before switching and have no added timed wait in Node 2. Reverse stays engaged on a fault. These checks use commanded throttle, not vehicle speed; the wiring has no brake input/output.

`0x201` byte 7 and Node 2 heartbeat error codes: 0 ready, 1 CAN unavailable/send failure, 2 PWM setup/write failure, 3 pedal missing/stale/invalid, 4 switches missing/stale/invalid, 5 release pedal, 6 release pedal to change direction, 7 traction disabled, 8 reverse relays disabled, **9 remote command missing/expired**. `/status` names each reason in the Node 2 sensor details, fault list and CAN log. `/api/vehicle.powertrainReason` becomes `null` when output telemetry is stale. PWM setup/write failure remains stopped until reboot. Motor-on bytes describe software PWM commands above the idle minimum, not sensed rotation.

The home page contains driving instruments: GPS speed, wireless steering angle, corrected accelerator percentage, trip, heading and satellites. **MAP** opens the offline map fullscreen; **CLOSE** or Escape returns. Its SRT opening animation runs once per tab session and honors reduced motion. The wheel follows **Node 1's wireless steering angle**; missing feedback freezes the last angle and shows unavailable. GPS diagnostics now appear under Node 5; the removed pitch sensor is no longer monitored.


### Joystick remote control

Upload **Nodes 1, 2, 3 and 5 together**, plus rebuilt Node 5 LittleFS assets. This build changes traction demand to include the 0.8 V idle floor; update Node 2 as well as the host. Open **http://192.168.4.1/control**; the circular joystick is the default view and **BUTTONS** opens the older separate controls. Drag anywhere around the circle. Vertical travel requests forward/reverse traction demand; horizontal travel requests left/right steering motor PWM. Both scales are **0–255**, with separate adjustable limits. The radial center deadzone is 6%; outside it the remaining travel scales linearly. Diagonal travel is normalized to the circle, so a full north-east request is approximately **180 throttle / 180 right steering**, rather than 255 on both axes.

| Joystick position at full travel | Throttle PWM | Steering PWM | Direction |
| --- | ---: | ---: | --- |
| Center / released | 0 | 0 | Stop |
| North | 255 | 0 | Forward |
| North-east | 180 | 180 right | Forward |
| East | 0 | 255 right | No traction |
| South-east | 180 | 180 right | Reverse |
| South | 255 | 0 | Reverse |
| South-west | 180 | 180 left | Reverse |
| West | 0 | 255 left | No traction |
| North-west | 180 | 180 left | Forward |

The UI updates immediately and sends one coalesced same-origin `POST /api/joystick` at up to **20 Hz**, without overlapping motion requests. Form fields are `action=move`, `token`, increasing `seq`, `throttle=0..255`, `steering=0..255`, `direction=0|1|2` (stop/left/right) and `reverse=0|1`. Example NE packet: `action=move&token=<current>&seq=1&throttle=180&steering=180&direction=2&reverse=0`. Read-only GET returns readiness, requested/applied PWM, current angle and status. `action=stop` stops both axes. The legacy `/api/drive` and `/api/steering` APIs remain available.

Node 5 first obtains matching ownership acknowledgements from **all three control nodes** (`0x121`, `0x122`, `0x124`). Node 1 silences physical pedal commands and Node 3 silences physical switch commands. The joystick sends demand 0–255 on `0x123`; **0 means the 0.8 V idle signal**, not electrical zero. Node 2 maps it to `62 + floor(193 × demand / 255)` and applies the 2,500 ms idle-to-full rise / 400 ms fall. All 256 input demand values are accepted; the baseline leaves 194 distinct output duty values, 62–255. Steering PWM mapping is unchanged. The UI separates requested demand from applied PWM; neither measures RPM.

**While held, physical accelerator presses are ignored and do not cancel remote control.** The physical reading remains visible in diagnostics. Entry requires an idle pedal. Releasing the joystick, STOP, tab/focus loss, touch cancellation, a failed request, missing control-node status/acknowledgements, a command lease expiry stops the session. Nodes latch the idle signal until a valid new session or a safe handback; a pressed physical pedal cannot take over. Normal switches return before the pedal, and the host waits for Node 2's release acknowledgement too. The physical pedal must be idle and the reverse switch must match before manual control resumes.

Left/right controls **steering motor speed, not a target wheel angle**. Releasing stops the motor; it does not return the wheels to center. A left/right reversal sends zero and waits for Node 1's stopped status with a new token before driving the opposite output. Up/down reversals wait for Node 2's direction interlock; no speed sensor or brake is inferred. Arrow keys also control the joystick, with Space/STOP stopping both axes. No automatic restart occurs after a stopped browser gesture.

### Mobile throttle and reverse (BUTTONS mode)

Use matching **Nodes 1/2/3/5** and Node 5 LittleFS assets. Both button throttle and joystick traction use Node 2's direct demand protocol on `0x123`; Node 5 no longer injects `0x301` or `0x300` inputs. Physical pedal transmission/conversion remains unchanged.

Open **http://192.168.4.1/control** (also `/controls`) and turn on Node 3's physical **Remote mode** switch to enable mobile control. With it off, the page only asks you to turn the switch on. Choose **BUTTONS** when enabled. THROTTLE LEVEL defaults to **100%**, equivalent to full north on the joystick. Demand is `round(percent × 255 / 100)`; Node 2 applies `62 + floor(193 × demand / 255)` with the same linear ramp and 0.8 V idle as joystick control. The slider can reduce demand; 0% disables the hold. After manual handback, a new throttle press uses Node 2's latest applied direction, including physical switch changes since the previous website poll. A latched remote direction remains selected until explicit change or manual handback. Hold THROTTLE and LEFT/RIGHT with separate fingers. Releasing one preserves the other; STOP cancels both.

REVERSE starts a zero-demand transaction and renews it until Node 2 reports the applied direction. Node 5 first waits for a new `0x124` acknowledgement of idle output in the existing direction, then requests the new direction and waits for its acknowledgement. Only then may traction rise. The button shows SWITCHING during this transaction and ON/OFF from the applied direction. Fixed one-second host waits have been removed. A joystick up/down reversal uses the same idle-before-direction sequence. These are commanded-output acknowledgements, not measured vehicle speed or relay contacts; select reverse while stationary.

All three ownership acknowledgements (`0x121`, `0x122`, `0x124`) are required. Physical pedal presses are ignored during an active remote session. Failure, STOP, lease expiry latches idle; a new intentional gesture can claim control again. Manual handback restores switches before pedal input and requires the physical reverse switch to match the applied remote direction and the physical pedal to be idle. The UI reports `MATCH REVERSE SWITCH FOR MANUAL` when needed.

`/api/drive` GET is read-only. Same-origin POST with `X-SRT-Control: 1` accepts `action=throttle` (`token`, increasing `seq`, `percent`), `action=reverse` (`token`, increasing `seq`, `reverse=0|1`), or `action=stop`. Replies include requested/applied PWM, `appliedReverse` and `shifting`. A delayed reverse completion reply is idempotent and cannot restart motion.

Joystick and drive motion replies allow **300 ms**, below the unchanged **350 ms** firmware command lease. Read-only polls and STOP replies allow 1,000 ms. Pending idle polls are cancelled when a gesture starts; motion requests remain serialized and coalesce to the latest stick position. A timeout stops the gesture and reports `COMMAND TIMED OUT — RELEASE & RETRY`; reconnecting cannot resume it. The firmware lease is not extended when a reply is slow. Button steering retains its existing 250 ms motion deadline.

### Remote steering

The remote button path is `POST /api/steering` → Node 5 CAN **0x110, DLC 8** → Node 1's existing `applySteering()` → GPIO32/33. Bytes are direction (0 stop / 1 left / 2 right), PWM (buttons use 60), sequence LE16, and Node 1 token LE32. Node 1 replies with **0x111, DLC 8**, which enables the controls. Serial `a` / `d` uses the same output function locally; serial `+` changes only the local test PWM. Local operation alone does not prove the CAN return path is connected.

If the buttons are disabled, read `http://192.168.4.1/api/steering`. This build reports `controlVersion:3`, `angleRequired:false`, `canReady`, `nodeSeen`, `nodeInvalid`, `ageMs`, and a precise `message`. Missing status, stale status and a dashboard CAN startup failure have different messages. Node 1 prints `RX110`, accepted/rejected commands and `TX111` success at 115200 baud; both Node 1 and Node 5 retry failed CAN initialization. Upload Node 1 and Node 5 **firmware plus Node 5 LittleFS** together. Rebuilding only the website cannot replace an older Node 5 angle-dependent handler.

For button-mode steering, upload **both Node 1 and Node 5**, and upload the rebuilt `data/` LittleFS assets to Node 5. Join dashboard WiFi and open **http://192.168.4.1/control** directly (also `/control/`), then select **BUTTONS**. Hold LEFT/RIGHT to turn at PWM 60. Release, drag off the button, cancel touch, switch tabs, leave the control page or press STOP to stop. Space/Enter supports keyboard holds. Reconnection never resumes a previous press. Joystick steering uses the full variable PWM range described above and requires updated Nodes 1/2/3/5.

Node 1 independently stops after **350 ms without a new valid CAN command**, on CAN send failure, when its controller needs recovery. Valid advancing commands renew a hold without a total time cap. Both outputs start at zero. Every stop changes the command token, and duplicate/out-of-order commands cannot extend a hold. A direction change requires releasing first. The controls require live `0x111` output status. Angle feedback is optional for motor-speed commands; missing or stale `0x100` is shown as unavailable without disabling steering. The dashboard also expires its HTTP command lease at 350 ms; source age and CAN age both count toward angle expiry.

`GET /api/steering` is read-only. Commands use same-origin `POST /api/steering` with `X-SRT-Control: 1` and form fields `direction`, `token`, `seq`; STOP needs only `direction=stop`. HTTP acknowledges CAN transmission. Applied PWM comes from `0x111`; current angle, availability, age and sequence come from `0x100` as `angleDeg`, `angleAvailable`, `angleAgeMs` and `angleSequence`. The control panel shows the current angle separately from motor direction. Successful steering and throttle/handoff transmissions appear as `tx` frames in the CAN log.

Node 1 steering stop reasons: 0 normal, 1 CAN failure, 2 command timeout, 3 invalid command, 4 local Serial test time limit (or legacy CAN hold limit), **5 steering angle unavailable (legacy firmware)**. `/status` highlights unavailable steering or nonzero stop reasons. A new valid press or STOP clears the reason. Steering outputs remain on Node 1; traction PWM and reverse relays are on Node 2.

For a local steering motor check, open Node 1's Serial Monitor at **115200 baud**. Send `a` for left, `d` for right, `+` / `-` to adjust PWM by 10, and `s` or space to stop. Selected PWM starts at 60 and is clamped to 0–255; speed changes apply while running, and zero stops. Each `a` / `d` starts a maximum five-second run; speed adjustments do not extend it. This local test works without angle feedback and takes steering control until stopped, while existing CAN tasks continue. CAN STOP and CAN transmission failures still stop the motor. After the test, normal CAN steering requires a current command token; angle feedback remains optional.

### Dashboard modes and wheel pitch

The homepage shows all six physical switch states. Turning a switch on shows a large centered announcement for 3.5 seconds. Cruise and reverse distinguish a selected/requested switch from applied powertrain status; autonomous remains a request. These displays do not bypass physical switches.

The supplied MPU6050 rotates with the steering wheel. Its pitch is relative to the sensor axes and is **display only**, not vehicle incline. The current `steering_tx` sketch follows the supplied Z-gyro sketch and sends steering and horn in its original 9-byte layout. It does not transmit pitch. Node 1 still accepts older 14-byte packets that contain display-only wheel pitch; those readings cannot be used as a chassis reference. The current transmitter retains gyro steering integration, GPIO27 active-low horn, receiver MAC and channel 1. No second sensor or Node 4 is added.

Automatic hill throttle is **not enabled**. Selecting Hill assist returns Node 2 to its normal 0.8 V-equivalent idle signal, blocks physical pedal and remote traction, reports stop reason 10, and displays why the mode is unavailable. Turning it off requires releasing the pedal before manual acceleration can resume. The requested 2.0 V-at-9° calibration is not applied to the rotating sensor; an uncompensated wheel angle can change its apparent pitch even on a constant slope. This is not a brake or a guarantee against rollback.

Upload the updated **steering transmitter, Nodes 1/2/5, and Node 5 LittleFS assets** for the new telemetry and interlock. Node 3 retains its existing switch layout.

### Steering-wheel ESP-NOW input

Node 1 starts an ESP-NOW receive callback at boot in station mode with sleep disabled. `STEERING_WIFI_CHANNEL` in `node1_bcm.ino` is **1**. The user-reported Node 1 STA MAC is **`68:09:47:DF:8E:E8`**; confirm it against the address printed at 115200 baud. In the IMU **sender**, replace the old destination `00:4B:12:34:C8:74` with `uint8_t receiverMAC[] = {0x68, 0x09, 0x47, 0xDF, 0x8E, 0xE8};`. Node 1 uses its own hardware MAC and does not filter the IMU's source MAC. Changing a receiver cannot redirect packets addressed to another board.

Set the sender's radio to channel 1 too: add `#include <esp_wifi.h>`, then check `esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)` after `WiFi.mode(WIFI_STA)` and before `esp_now_init()`. Set `peer.channel = 1`. Its original `peer.channel = 0` means use the sender's current channel; it does not discover the receiver's channel. Unencrypted reception does not require adding a sending peer on Node 1 ([Espressif ESP-NOW documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)).

Node 1 accepts the original ESP-NOW formats: packed **float steeringAngle + uint32_t sequence** (8 bytes), and that same prefix followed by **bool hornPressed** (9 bytes), native ESP32 little-endian. The receiver validates the horn byte as 0/1 and prints its fresh state in Serial. A fresh 9-byte packet with `hornPressed=true` drives the Node 1 GPIO13 relay output HIGH; release, missing horn data, or 250 ms without a valid steering packet cancels the wireless source. The relay turns off when neither wireless nor website source is pressed. The older 8-byte packet remains supported and cannot activate the wireless horn source. This does not alter Node 3 switch frames. The accepted range is **−270° to +270°, including 0°**, matching the sender's clamp. Node 1 and Node 5 share this limit; dashboard display and controls accept the same range. Node 1 rejects wrong lengths, NaN/infinite values, out-of-range angles, duplicates, and backward sequences while the stream is fresh. Sequence and millis rollover are handled. A transmitter sequence reset is accepted after the 250 ms feedback timeout. Returning feedback never restarts a stopped hold; the user must press again. The previously supplied pitch transmitter appended packed `float pitchAngle` and `uint8_t pitchValid` after the original nine bytes, giving a **14-byte** packet. Node 1 retains that compatibility. The current transmitter uses the original **9-byte** packet for live gyro readings and a **10-byte** extension (one `angleValid` byte after the horn byte) during calibration or I2C failure. Node 1 accepts all four lengths; the 10-byte fallback keeps horn traffic fresh while marking steering unavailable. Pitch and its source freshness travel on CAN `0x102` to Node 5, which exposes `wheelPitchDeg`, `wheelPitchValid`, and `wheelPitchForControl=false`. Older 8/9-byte senders still provide steering/horn but show no pitch. Channel settings matter only if packets stop arriving. Upload updated Nodes 1 and 5 and Node 5 LittleFS assets for the full range throughout the system.

The received angle is the current steering-wheel measurement. Existing LEFT/RIGHT holds remain motor-speed commands, independent of angle availability. There is no automatic target-angle/PID control or inferred mechanical travel limit. The supplied gyro integration measures a relative angle from its startup center and can drift; the receiver does not create an absolute steering reference.

Open **http://192.168.4.1/status** directly for diagnostics (also `/status/`). There is deliberately no homepage link to it. Tap a node for its sensors, readings and freshness. Node 2 uses its heartbeat for health and `0x201` for commanded PWM/reverse; older firmware without a heartbeat remains unknown. CAN log supports exact hex ID, node, message/raw-byte search and issues-only filters. **RESET** clears filters. **PAUSE** freezes the browser snapshot while CAN reception continues; **COPY** and **EXPORT** include filtered frames, diagnostics and recent events. The browser retains at most 64 frames, 24 fault transitions and 600 route points.

`/api/vehicle` returns decoded data and node states. `/api/gps` returns the local UART GPS state; `/api/data` returns fresh GPS-reported speed in km/h. Both endpoints work without CAN. The GPS response includes signed latitude/longitude, satellites and HDOP, source/fix ages, UART receive age, character count, passed/failed checksums and fix-sentence count. Satellites remain visible before a fix; missing/stale values are `null`. Position or speed expires after 3 seconds, and a checksum-valid GGA/RMC no-fix report invalidates the fix immediately. The parser drains at most 256 bytes per service call, including while serving files, to preserve control responsiveness. No GPS data is sent or consumed over CAN; retired `0x400`/`0x401`/`0x500` frames cannot supply these APIs. Ignition, pitch and hold-percentage fields have been removed from `/api/vehicle`. Missing/stale signals are `null`. `/api/can-log` returns the latest 64 frames, `uptimeMs`, and exactly `dlc` bytes per frame. The optional exact hex filter `/api/can-log?id=0x301` still works; invalid IDs return HTTP 400. `/api/diagnostics` returns per-signal freshness, invalid-payload counts, total RX/TX/errors, free/minimum/largest heap block, LittleFS usage and WiFi clients. `/status` now returns HTML, not JSON; consumers of its old JSON must use `/api/vehicle`.

## Automated checks

Run `python3 dashboard/tools/sync-sketch-headers.py --check` to check the local header copies. Run `python3 dashboard/tools/test-powertrain.py` for the actual Node 2 receiver/PWM/relay code, including **the unmodified vehicle defaults**, relay OFF-before-output startup, active-LOW forward/reverse levels, exact 500/501 ms pedal expiry, linear rise/fall, target changes, loop jitter/stalls, applied-output telemetry, all six switch requests, bench overrides, reverse interlocks, immediate fault stops and recovery. Run `python3 dashboard/tools/test-steering.py` for Node 1's ESP-NOW callback, GPIO adapter, packet validation, optional angle telemetry and command watchdogs. Run `python3 dashboard/tools/test-senders.py` for the actual Node 1/3 sketches: unchanged raw pedal transmission for all 4,096 ADC values, **all 64 switch combinations on the supplied GPIOs**, debounce/bounce, TX failure/retry, CAN initialization/mode recovery, switch handoff, expiry and bounded receive loops. This also compares all 4,096 ADC values between firmware and browser throttle scaling. These tests read each sketch and its local headers from the node's folder.

Run `python3 dashboard/tools/test-control-link.py` for actual Node 5 → Node 2 demand output across all 100 button levels, full-north/100% equivalence for the entire PWM ramp at 50/2000 ms browser cadences, physical-pedal blocking through STOP, direction changes between website polls, reverse completion, idle-before-direction acknowledgement and independent steering. Run `python3 dashboard/tools/test-vehicle-control-link.py` for combined actual Node 1/2/5 edge/corner sweeps with delayed steering status. Run `python3 dashboard/tools/test-steering-link.py` for the actual Node 5 HTTP handler → actual Node 1 CAN receiver/GPIO code → returned status, including left/right, command renewal, STOP, expiry and missing-angle operation.

Run `python3 dashboard/tools/test-reverse-link.py` to connect the actual Node 3 sender to the actual Node 2 receiver through simulated CAN. It checks boot OFF, physical GPIO33 ON/OFF, all 64 switch combinations, nonzero pedal idle, short requests applied after debounce without an extra receiver delay, matching applied-direction telemetry, remote suppression and ordered handback. The GPIO mocks ignore unconfigured `digitalWrite` calls like ESP32 core 3.x; restoring the old startup sequence fails the OFF-before-output assertion. Missing pedal data and ADC values above the current idle threshold are checked as intentional shift blocks.

Run `node --test dashboard/tools/diagnostics.test.mjs dashboard/tools/steering-client.test.mjs dashboard/tools/drive-client.test.mjs` for decoders and browser command-race tests. Run `python3 dashboard/tools/test-can-protocol.py` from the repository root (requires `g++`) for Node 5's actual decoder/API functions against a simulated CAN bus: endian order, all switch bits, angle/source age, data expiry, invalid flags, handoffs, log filtering and clock wraparound. `dashboard/tools/smoke-packed.cjs` checks the packed website in Playwright, including wireless angle display/freeze, controls, mobile CAN filters and missing-data display.

Local GPS uses full signed coordinates directly from TinyGPSPlus; there is no CAN coordinate encoding or hemisphere restriction. Firmware compilation and simulated/browser checks do not test physical boards, wiring or sensors.


### Verification

Run `cd dashboard && npm test` for native/JavaScript checks, `npm run lint`, and `npm run build:esp` to regenerate Node 5's LittleFS files. `npm run test:ui` checks the packed browser bundle with Playwright/Chromium. Native host tests compile the installed TinyGPSPlus parser; install that library with Arduino CLI or set `TINYGPSPLUS_SRC` to its source directory. `test-can-protocol.py` feeds real GP/GN NMEA sentences through Node 5's actual UART service and verifies local operation with CAN unavailable, signed coordinates, GPS-reported speed, no-fix reports, checksum failures, expiry, bounded reads and clock rollover. It also confirms that retired GPS/ignition/pitch CAN frames cannot populate the APIs.

Compile Node 5 with `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota node5_dash`. Native fixtures and firmware compilation do not verify physical wiring, RF reception or actuator behavior.

### On-vehicle verification

With the drive wheels raised, use Serial Monitor at 115200 baud and `/status` to verify the physical results. Upload the matching **Nodes 1/2/3/5** for the latest six-switch mapping, plus **Node 5's rebuilt LittleFS assets**. The older supplied Node 2 reads reverse bit 2; it must not be mixed with the latest Node 3 sender, whose reverse is bit 5.

| Check | Expected result |
| --- | --- |
| Boot, physical pedal released | Node 2 PWM13/14 hold idle PWM 62 after PWM setup; GPIO33/26 both HIGH; after valid frames and an idle pedal, reason 0 / ready and the 0.8 V-equivalent idle minimum |
| Pedal sweep | Node 1 sends raw ADC little-endian every 50 ms; Node 2 reaches the original raw PWM targets, clamped to idle PWM 62, on both outputs through the added ramp; the corrected dashboard gauge is a separate estimate |
| Each of six switches | Exactly the named bit changes after 25 ms; unchanged state is refreshed each second |
| HillHold ON with a high pedal target | Immediate idle command on both outputs; stop reason 10; pedal cannot add throttle; switch off and release pedal to resume |
| Reverse while pedal released | Node 2 returns PWM to idle, then sets both relays LOW in the same control-loop pass; only Node 3’s existing 25 ms debounce and bus/loop latency precede reception |
| Return to forward | With the pedal released, PWM returns to idle and both relays become HIGH immediately; a pressed pedal blocks the relay change until release |
| Stop Node 1 pedal updates | PWM returns to idle after more than 500 ms, reason 3; recovery requires releasing the pedal |
| Stop Node 3 switch updates | PWM returns to idle after more than 3 s, reason 4; last applied direction is held; recovery requires valid switches and an idle pedal |
| Remote throttle, reverse, STOP | `/control` distinguishes requested demand and applied PWM from physical inputs; STOP sends idle demand; Node 2 output status must settle at PWM 62, not zero |
| Steering LEFT / RIGHT / STOP | Same GPIO32/33 and duty 60 as the working test; only one side is driven; release or stale command stops it; missing angle only hides the angle reading |
| Local GPS | Fix, satellites and UART diagnostics appear under Node 5; disconnecting GPS expires location/speed, without affecting CAN controls |

Hill assist remains unavailable without a chassis pitch reference and enforces the idle interlock above. Autonomous remains a switch request only. Cruise, remote-mode gating and Node 1’s wireless horn relay operate as documented above.


### Horn relay and mobile button

Node 1 drives **D13 / GPIO13** from either the steering-wheel GPIO27 button or the website **HORN · HOLD** button. `/control` (joystick) and its button-control view both include the button; `/controls` is an alias. Mobile horn requires the physical REMOTE MODE switch. The wireless horn works independently of that switch and CAN availability. Releasing one source does not cancel the other.

- Default relay polarity is active HIGH. For an active LOW module, compile Node 1 with `-DNODE1_HORN_ACTIVE_LOW=1`. Firmware preloads OFF before enabling the GPIO output.
- Wireless packets expire after 250 ms. Website press renewals arrive about every 80 ms; both Node 5 and Node 1 expire the website source after 350 ms without renewal. Continuous renewed holds have no total time cap. STOP, pointer release/cancel, dragging off the horn button, keyboard release, blur, hidden page, navigation, remote mode off, or link failure cancel the website source. An independent wireless press may keep the horn on.
- CAN **0x112**, DLC8: website press/stop in byte0, byte1=1 for press / 0 for stop, sequence LE16 in bytes2–3, Node 1 lease token LE32 in bytes4–7. STOP is always accepted; stale tokens and repeated sequences cannot restart a released horn.
- CAN **0x103**, DLC8: combined GPIO command in byte0; byte1 bits0/1/2 = wireless known / wireless pressed / website held; byte2 = wireless source age (255 unknown/saturated); byte3 = version2; bytes4–7 = Node 1 horn token LE32. This reports the GPIO command, not measured relay contact state.
- `/api/horn`: GET status; same-origin POST with `X-SRT-Control: 1`, `action=press`, dashboard `token`, advancing `seq`; `action=stop` cancels only the website source. The main dashboard horn indicator follows Node 1's combined command rather than the unused Node 3 horn request.

Upload **steering_tx**, **Node 1**, **Node 5**, and rebuilt **Node 5 LittleFS data** for the complete horn path. Native tests cover both relay polarities, source arbitration, startup OFF, continuous holds, rollover, replay, expiry and HTTP/CAN/GPIO integration. Hardware relay operation still needs checking on the vehicle.

### Steering TX horn troubleshooting

The transmitter samples GPIO27 before and after I2C work, debounces it for 15 ms, and transmits every 10 ms (or sooner after a button change). Its radio and peer are explicitly fixed to Node 1 channel 1. Gyro failure no longer skips horn transmission or hangs startup: the 10-byte fallback marks angle invalid while allowing the horn to operate. Upload **both `steering_tx` and updated `node1_bcm`** to support that fallback; normal gyro operation retains the supplied packed 9-byte ABI.

Open the transmitter Serial Monitor at **115200**. Released must show **GPIO27=HIGH**, pressed must show **GPIO27=LOW / Horn=PRESSED**. The button connects **GPIO27 to GND** with `INPUT_PULLUP`. If GPIO27 never changes, inspect the button contacts, pin number and ground wiring. `radio ACK` counts MAC-layer deliveries; `FAIL` or `queueErrors` indicate radio problems. ACK does not prove Node 1 decoded the packet or the relay actuated. Compare Node 1's printed STA MAC with `68:09:47:DF:8E:E8`; Node 1 should print `IMU horn=PRESSED` and `Horn D13=ON` on press. If Node 1 reports ON but the relay does not actuate, check its polarity setting and relay wiring.


### CAN stall recovery and held throttle

Upload matching **Nodes 1, 2, 3 and 5**, then Node 5's rebuilt **LittleFS data**. No steering-transmitter update is required for this recovery change.

- Pedal release/remote entry/manual recovery now accept **up to 1.0 V** (ADC 0–1240). The separate motor-controller idle remains **PWM 62 / about 0.8 V**, and the dashboard's 0.8 V voltage correction and physical throttle transfer remain unchanged. Physical pedal input stays blocked while a remote session owns traction.
- The button throttle sends immediately and renews every **2 seconds** while held. Node 5 grants a **2.5-second HTTP lease**, continues sending `0x120`/`0x123` every 50 ms, and stops when the browser lease expires. Release/STOP sends immediately. Joystick changes and steering commands retain their faster 50/80 ms renewal and 350 ms lease. Renewed CAN steering no longer has a five-second total cap; the separate local Serial motor test still does.
- Every CAN node aborts queued TX on a send failure and marks its controller unavailable. The controller is reset/reconfigured after a **one-second backoff**, with one bounded initialization attempt per retry. Bus-off, error-passive lasting 300 ms, failed SPI reads, and **1.5 seconds without RX after peer traffic** also trigger recovery. Historical RX overflow flags alone do not force resets. Recovery discards old motion/horn commands and locks remote ownership; the user must release and press again after the link returns. Node 1's independent wireless horn source still works.
- Nodes 2 and 5 formerly shared heartbeat ID `0x600` despite carrying different data. They now use **`0x602` and `0x605`** to avoid same-ID collisions. Node 5 and browser diagnostics still accept legacy `0x600`, and reject new-ID heartbeat payloads naming a different node.
- Serial reports the recovery reason and captured **EFLG, TEC and REC**, plus fault/reset counts. Node 5 exposes `canRecoveryFaults`, `canRecoveryResets`, `canRecoveryPending`, `canLastFault`, `canErrorFlags`, `canTxErrors` and `canRxErrors` in `/api/diagnostics`.

Confirmed software gaps were missing runtime recovery, queued retries left after driver timeout, shared heartbeat IDs and unrestricted horn/pitch console logging. The horn-specific trigger still needs a vehicle measurement: relay/horn switching can introduce supply dips, ground noise or inductive transients. Check the horn load return and relay suppression, module supplies, twisted CAN wiring and two end terminators. With power removed, two 120-ohm end terminators should measure about 60 ohms across CANH/CANL. Firmware cannot clear a shorted bus, failed transceiver or continuing power problem.

`python3 dashboard/tools/test-can-recovery.py` exercises the actual shared adapter with queued TX, bus-off, passive/SPI faults, silent reception, bounded reset/backoff, unique heartbeat IDs and clock wrap. The existing sender/link tests also check runtime Node 1/3 recovery, gesture invalidation, 1.0 V boundaries and the full throttle ramp with two-second browser renewals.
