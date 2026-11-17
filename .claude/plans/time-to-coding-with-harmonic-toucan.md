# Plan: ESP32 servo gate test, triggered by the AI model
Project: C:\Work\WashingParking

## Context
The YOLO model runs on the PC (`SmartPK_PC/station/`, program `tram_ai.py`). The ESP32 is linked by UART text lines (`EspLink.send`, `esp.py:135`, "line\n", 115200). Goal: when the AI detects a vehicle, the PC sends `CAR`; the ESP32 turns the servo on GPIO13 to 90°, holds 5 s, closes. Only `tram_ai.py` is covered; `pc_ai_camera.py` has its own EspLink copy and is not changed.

## Decisions
- Port auto-detect is by USB VID (`esp.py:36,47`), not by reply, so no handshake is required. Firmware replies `PONG` to `PING` (the PC sends PING on open) and ignores every other line (e.g. `PLATE,...`). Strip `\r`/`\n` before comparing.
- Standalone PlatformIO project `esp32_servo_test/` (existing .ino untouched). `lib_deps = madhephaestus/ESP32Servo`.
- Board default `esp32-s3-devkitc-1` (the existing firmware uses GPIO38/10, which implies an S3); change if different. Assumes a 180° servo.
- 5 s hold is non-blocking (`millis()`); a new `CAR` while open restarts the timer.
- PC hook: in `Inspector.step` (`inspection.py:175-178`), right after `self.state = XE_VAO`, send `CAR` via a small helper next to `lcd()` that uses the same guard (`hub.esp_link`, `is_open`). XE_VAO is entered once per car, so one trigger per car.

## Files
- `esp32_servo_test/platformio.ini`, `esp32_servo_test/src/main.cpp` (~35 lines: attach GPIO13, write(0); loop reads a line: `CAR` -> write(90), start timer; `PING` -> `PONG`; after 5000 ms write(0)).
- `SmartPK_PC/station/inspection.py`: add `cua_mo()` helper (like `lcd()`, lines 35-44) and call it at line 176.

## Steps
1. Write firmware -> `pio run` builds.
2. Upload, then in `pio device monitor` type `CAR` -> servo opens 90°, closes after 5 s. Firmware-only test.
3. Add PC hook -> `python -c "import ast;ast.parse(open('station/inspection.py').read())"`-level syntax check.
4. End-to-end: close the PlatformIO monitor (only one program can hold the COM port), run `python run.py tram_ai.py`, put a car/toy-car image in front of the camera -> log shows "Xe vào trạm kiểm tra" and the servo opens. Real camera needed: `--no-camera` + "Thử ảnh" goes through `analyze_image()` and probably does not hit the `XE_VAO` transition (verify while implementing).

## Open questions
- Board model and servo type (defaults above).
