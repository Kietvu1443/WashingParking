Project: C:\Work\WashingParking

# Stage 1 — end-to-end flow (mock AI → server → ESP32 over WiFi/MQTT)

## Context
Prove the basic wash+parking flow with real ESP32 hardware, with the AI mocked: plate → DB row → entrance gate opens/closes → condition DIRTY/CLEAN → wash gate or user choice (button / web, 15 s → PARKING). The teacher requires server→ESP32 over WiFi. The existing `ESP32/Source/lab4_parking_1file.ino` (9k lines, WiFi AP + UART) and `Brain/` stay untouched; stage 1 is a new small `stage1/` folder. Brain's `PlateReader` etc. plug into `ai.py` at stage 2.

## Decisions (settled with user)
- Hardware: ESP32-S3, 2 servos (entrance gate, wash-bay gate), 1 button, 2 LEDs, no IR → gates close on an ESP-side timer (5 s). Servos on separate 5 V supply, common GND.
- Transport: **MQTT + Mosquitto on the laptop** — why below.
- Network: phone hotspot; laptop + ESP32 both in station mode on it.
- Server: Python 3.12 + FastAPI (Swagger `/docs` = mock-AI test UI) + sqlite3 + paho-mqtt.
- Firmware: new ~200-line PlatformIO sketch, libs PubSubClient + ESP32Servo (S3 support to verify at CP3).
- Button: any press within 15 s = WASH; no press = PARKING. `POST /choice` does the same from the web.
- Test D: test command `DROP_WIFI,<sec>` makes the ESP drop WiFi on purpose → fully scripted.
- Spec fixes: extra state `AWAITING_CHOICE`; **one DB row per visit** (plate is a column, so repeat visits don't collide); ESP closes gates on its **own** timer (gate can't stay open when WiFi dies); commands are QoS0, not retained, clean session → a command sent while the ESP is offline is **dropped, never replayed late** (no surprise gate opening after reconnect).

### MQTT vs HTTP vs WebSocket (5 lines, for the report)
1. HTTP: ESP must poll the server (lag + wasted requests) or run its own web server; no built-in "device offline" signal.
2. WebSocket: real push without a broker, but heartbeat, reconnect and message routing are hand-written on both sides.
3. MQTT: push via a broker; topics name every message, so the flow is easy to show with `mosquitto_sub -v -t 'wp/#'`.
4. MQTT Last Will: the broker itself announces `OFFLINE` when the ESP drops, which is exactly what Test D needs.
5. Cost: one extra program (Mosquitto) on the laptop. **Pick MQTT.**

## Step 1 — State machine (server, per visit)
| From | Trigger | To | Server action |
|---|---|---|---|
| IDLE | `POST /cam1` (mock plate) | PLATE_CAPTURED | insert visit row (plate, photo_path, time) |
| PLATE_CAPTURED | ESP online | GATE_OPEN | publish `OPEN_GATE` |
| PLATE_CAPTURED | ESP offline | ERROR | log "esp offline" |
| GATE_OPEN | status `GATE_CYCLES` > value recorded when `OPEN_GATE` was sent | INSPECTING | — (a lost `OPEN_GATE` never raises the counter, so it can't advance the flow; works even if the OPEN status was never seen) |
| GATE_OPEN | counter not increased within 30 s (> Test D drop + rejoin), or counter dropped below baseline (ESP rebooted) | ERROR | log "gate cycle not seen" |
| INSPECTING | `POST /cam2` result DIRTY | WASHING | publish `OPEN_WASH` |
| INSPECTING | `POST /cam2` result CLEAN | AWAITING_CHOICE | publish `ASK_CHOICE`, start 15 s timer |
| AWAITING_CHOICE | event `BUTTON` or `POST /choice wash` | WASHING | publish `END_CHOICE`, `OPEN_WASH` |
| AWAITING_CHOICE | 15 s timeout or `POST /choice park` | PARKED | publish `END_CHOICE` |
| any | command needed while ESP offline | ERROR | — |

**One active visit at a time** (status messages carry no visit id): `/cam1` returns 409 while a visit is non-terminal; WASHING, PARKED, ERROR are terminal in stage 1; `/cam2` and `/choice` act on the single active visit.

Every transition writes `visits.state` + one row in `events(visit_id, state, at)` → all timestamps kept. ESP offline alone does **not** cause ERROR; on reconnect the ESP republishes status and the server continues (Test D: `GATE_CYCLES=n+1` arrives after reconnect → INSPECTING).

Gate-cycle counter: ESP holds `gate_cycles` (RAM, starts 0 at boot), +1 each time the entrance gate finishes closing. The server stores the last seen value as the baseline at the moment it publishes `OPEN_GATE` (ESP must be online, so a status has been seen).

## MQTT messages (plain text payloads, no JSON lib on ESP)
| Topic | Dir | Payload | Notes |
|---|---|---|---|
| `wp/esp/cmd` | server→ESP | `OPEN_GATE` | entrance servo opens, auto-closes after 5 s (ESP timer) |
| `wp/esp/cmd` | server→ESP | `OPEN_WASH` | wash servo opens, auto-closes after 5 s |
| `wp/esp/cmd` | server→ESP | `ASK_CHOICE` / `END_CHOICE` | LED2 blinks + button armed / off |
| `wp/esp/cmd` | server→ESP | `DROP_WIFI,15` | test hook: disconnect WiFi 15 s, non-blocking (> 7.5 s Last-Will delay, so `OFFLINE` reliably appears) |
| `wp/esp/status` | ESP→server | `ONLINE,GATE=OPEN\|CLOSED,WASH=OPEN\|CLOSED,GATE_CYCLES=<n>` | retained; sent on connect + every gate change; `n` = completed entrance open→close cycles since boot |
| `wp/esp/status` | broker→server | `OFFLINE` | Last Will, retained; keepalive 5 s so it fires fast |
| `wp/esp/event` | ESP→server | `BUTTON` | only while choice armed (debounced) |

## Step 2 — Files
```
stage1/
  server/app.py        FastAPI endpoints: /cam1 /cam2 /choice /visits /visits/{id}
  server/flow.py       state machine above (one lock; MQTT thread + HTTP threads both call it)
  server/db.py         sqlite3: visits, events
  server/ai.py         read_plate(img, mock=None), classify_condition(img, mock=None) — mock now, Brain later
  server/mqtt_link.py  paho client: publish cmd, parse status/event, track esp_online
  server/test_flow.py  assert-based self-test with a fake publisher + short timeout (no broker needed)
  server/requirements.txt  fastapi uvicorn paho-mqtt
  demo.py              runs Tests A–D against the live server (stdlib urllib), prints each step
  mosquitto.conf       listener 1883 0.0.0.0, allow_anonymous true (Mosquitto 2.x is localhost-only by default)
  firmware/platformio.ini, firmware/src/main.cpp, firmware/src/secrets.h.example (WiFi + broker IP)
  README.md            Vietnamese: wiring, run steps, message table. Must also say:
                       - one line: AI is mocked in stage 1 (plate/condition come from the request)
                       - phone hotspot must be 2.4 GHz (ESP32 has no 5 GHz)
                       - before each session: check laptop IP (`ipconfig`), update `secrets.h` broker IP, reflash
                       - firewall rule for port 1883 on the Public profile (hotspots are usually Public)
                       - `POST /test/drop_wifi` is a stage-1-only test hook (remove for real deployment)
                       - flashing overwrites the lab4 firmware on this board
```
Pins (decided 2026-11-16 — board IS the lab4 board, so reuse its wired parts): entrance servo GPIO18 (lab4 gate in), wash servo GPIO39 (lab4 gate out), LED1 online = WS2812 GPIO11, LED2 choice = WS2812 GPIO12 (Adafruit NeoPixel; core 2.0.17 `neopixelWrite` only supports one pin), button GPIO15 → GND (only new wire). Datasheet-verified: none are strapping (0/3/45/46), flash/PSRAM (26–37), USB (19/20). Module marking unreadable → default 8 MB flash, PSRAM off. Servos: drive by LEDC like lab4 (no ESP32Servo); lab4 supports 180° and 360° servos → ask type before CP3.

## Build order — checkpoints (wait for user "OK" after each)
- [x] **CP0 setup** (done 2026-11-16: Mosquitto 2.1.2 installed; its Windows service stopped + set Manual so `stage1/mosquitto.conf` owns 1883; firewall rule "Mosquitto MQTT 1883" Private+Public; pub/sub OK via Wi-Fi IP; `pio run` OK. Start broker: `& "C:\Program Files\mosquitto\mosquitto.exe" -c stage1\mosquitto.conf -v`): install Mosquitto (winget, ID to verify), run with `stage1/mosquitto.conf`, allow inbound TCP 1883 in Windows firewall (Private + Public profiles); PlatformIO (VS Code extension) installed and an empty S3 project builds. Verify: `mosquitto_sub -v -t 'wp/#'` in one terminal, `mosquitto_pub -t wp/test -m hi` shows `wp/test hi`; `pio run` succeeds.
- [x] **CP1 server without ESP** (done 2026-11-16: 18/18 test_flow; live check with real broker + fake ESP passed. Additions beyond the table, user may veto: park while ESP offline -> still PARKED (no gate to move, END_CHOICE only if online); unfinished visits -> ERROR "server restarted" on server start; `GET /esp` status; `POST /test/drop_wifi?sec=` hook for demo.py Test D. Run: `cd stage1/server; .venv\Scripts\uvicorn app:app --host 0.0.0.0 --port 8000`): db + flow + app + test_flow. Pin paho-mqtt version; check its 2.x API (callback API version, signatures) in docs, not from memory. Verify: `python test_flow.py` passes all cases (A/B/C-button/C-timeout/offline→ERROR); fake ESP with `mosquitto_pub -t wp/esp/status -m ONLINE,GATE=CLOSED,WASH=CLOSED,GATE_CYCLES=0`, then `...GATE_CYCLES=1` after `/cam1`, and walk `/docs`. test_flow covers: counter unchanged → timeout ERROR; counter +1 without any OPEN status → INSPECTING; counter reset (reboot) → ERROR.
- **2026-11-16 pivot (user): simulate with Wokwi for VS Code before hardware.** Files: `stage1/firmware/{wokwi.toml,diagram.json,src/net_config.h}`; env `[env:wokwi]` (-DSIM_WOKWI, BROKER_HOST, TOPIC_PREFIX) vs `[env:esp32s3]` (default_envs, secrets.h, -DSERVO_SAFE_90=1). Route A = Wokwi private gateway `host.wokwi.internal` → local Mosquitto (pricing page: Private IoT Gateway is paid-plan only; VS Code docs say the extension bundles it → decided by runtime: ESP IP 10.13.37.2 = private, 10.10.0.2 = public). Route B = Wokwi-GUEST + public broker + random TOPIC_PREFIX; server `WP_BROKER`/`WP_TOPIC_PREFIX`/`WP_GATE_TIMEOUT`. Servos now via ESP32Servo 3.0.6 (builds on core 2.0.17; attach 500–2500 us → 90° = 1500 us) instead of LEDC — LEDC is the fallback if the sim shows it fails. Firmware for CP3/CP4 written (gate/wash auto-close first in loop, GATE_CYCLES, button debounce, DROP_WIFI without clean MQTT disconnect). `stage1/demo.py` written (`--sim` labels output, `WP_DEMO_SCALE`). **Everything here is SIMULATED, not hardware-verified. Stage 1 is DONE only after demo.py passes on the real ESP32.**
  - Sim results 2026-11-16 (route A works: ESP IP 10.13.37.2 = private gateway → local Mosquitto): A ✅ B ✅ C2 ✅ D ✅ (rerun) C1 ✅ (rerun; press injected via sim-only serial 'b' → same debounce/publish path; real GPIO15 path shown by 4 manual presses logged 22:11–22:12). Sim runs ~50% speed → sim-only additions: heartbeat publish every 2 s (`<prefix>/esp/hb`, else broker "exceeded timeout"), CHOICE_MAX_MS 400 s, serial 'b' virtual press; run with server `WP_GATE_TIMEOUT=120 WP_CHOICE_TIMEOUT=180`, demo `WP_DEMO_SCALE=3`. Sim pauses (tab hidden?) caused one failed D run.
  - NOT covered by sim (test on hardware): servo type 180/360 + angle calibration; SERVO_SAFE_90 build behaviour; hotspot 2.4 GHz + secrets.h + laptop IP; firewall inbound 1883 (sim traffic arrives via 127.0.0.1); wiring (lab4 parts, new GPIO15 wire, servo 5 V supply/common GND, real bounce); keepalive-only OFFLINE timing (no heartbeat); default timeouts 30 s/15 s; NeoPixel colours.
- [ ] **CP2 firmware connect** (NOT run on hardware yet: board not detected on USB, secrets.h unfilled): **FIRST remind user: phone hotspot (2.4 GHz) gives the laptop a new IP (not 192.168.0.182) → run `ipconfig` on the hotspot, put that IP in `secrets.h`.** Ask the module marking (e.g. N8R8 / N16R8) for platformio.ini flash/PSRAM settings, and whether this is the lab4 board with GPIO4/5 already wired. Board = ESP32-S3 DevKit (user-confirmed); pins 18/16/4/5/6 avoid flash/PSRAM 26–37, USB 19/20, strapping 0/3/45/46. WiFi STA + MQTT + status/LWT + LED1 + reconnect loop. Loop rules (core of Test D): gate auto-close check runs first in `loop()`; MQTT connect only when `WiFi.status()==WL_CONNECTED`; retries rate-limited by `millis()`; short socket timeout (check `setSocketTimeout` exists in pinned PubSubClient); servos driven to CLOSED in `setup()` so a reset can't leave a gate open. Verify: sub shows `ONLINE,...`; unplug ESP → `OFFLINE` within ~8 s.
- [ ] **CP3 servos**: OPEN_GATE/OPEN_WASH + 5 s auto-close via millis(); calibrate open/closed angles on the real gates. Verify ESP32Servo builds for S3 first. Verify: Test A from `/docs` → servo opens, closes, status flips, visit → INSPECTING.
- [ ] **CP4 button + DROP_WIFI**: ASK_CHOICE/END_CHOICE, debounced BUTTON event, DROP_WIFI non-blocking. Verify: Tests B, C, D by hand.
- [ ] **CP5 demo.py + README**: one script runs A→D (pauses "press the button now" for C-button). Each of B, C-button, C-timeout, D starts with a fresh `/cam1` (one 5 s gate cycle each), so a full run takes ~2 min. Verify: full run output, DB rows/events printed.

## Verification (definition of done)
`python demo.py` with server + broker + ESP running: A) plate 51A-123.45 → row, gate opens/closes; B) DIRTY → wash servo, state WASHING; C) CLEAN → button → WASHING, and no press → PARKED after 15 s; D) OPEN_GATE + DROP_WIFI,15 → gate closes on its own while offline (counter +1 offline), `OFFLINE` then `ONLINE,GATE=CLOSED,...,GATE_CYCLES=n+1`, visit continues to INSPECTING.

## Open / to verify during build
- Exact S3 board (PlatformIO `board =`), pin safety on it. If it is the lab4 board: flashing overwrites lab4 firmware, and GPIO4/5 are slot-1 pins in `MO_HINH 1` → confirm pins against what is physically wired.
- ESP32Servo on S3 with the installed Arduino-ESP32 core; fallback = LEDC directly (lab4 does this).
- Winget package ID for Mosquitto.
