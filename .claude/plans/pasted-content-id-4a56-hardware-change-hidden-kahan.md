C:\Work\WashingParking\stage1

# Retarget stage1 firmware: ESP32-S3 → classic ESP32 DevKit V1

## Context
The real board is an ESP32 DevKit V1 (classic ESP32, micro-USB with a USB-serial bridge), not an S3. The current pin map doesn't work on it: GPIO39 is input-only, GPIO11 is flash, and GPIO12 and GPIO15 are strapping pins. Goal: firmware, Wokwi sim and README all on the classic ESP32 with a safe pin map, then rerun Test A–D in simulation. Results stay labelled "simulated, not hardware-verified".

## Pin map (sources: ESP-IDF GPIO page for ESP32 v6.1.1, ESP32 datasheet strapping table)
| Function | Old (S3) | New | Check |
|---|---|---|---|
| Entry-gate servo | 18 | **18** | GPIO table: no SPI, GPI or JTAG note → **verified** normal I/O |
| Wash-gate servo | 39 | **19** | same → **verified** |
| Online WS2812 LED | 11 | **22** | same → **verified** |
| Choice WS2812 LED | 12 | **23** | same → **verified** |
| Button → GND, INPUT_PULLUP | 15 | **27** | row has ADC2_CH7 / RTC_GPIO17, no SPI/GPI/JTAG → **verified** output-capable with a pull-up (GPI note: only 34–39 lack pull-ups) |

- **Verified:** GPIO6–11 = SPI flash. GPIO34–39 = input only, no pull-ups. GPIO12–15 = JTAG. Strapping pins = GPIO0, GPIO2, MTDI, MTDO, GPIO5 (datasheet table). None of the new pins appear in any of these lists.
- **Verified (Wokwi `board.json` on GitHub):** the `board-esp32-devkit-c-v4` pins `18 19 22 23 27 5V GND.1 GND.2 TX RX` exist. PlatformIO `esp32doit-devkit-v1` exists (ESP32, 4 MB) per the local `pio boards`.
- **Assumed / not verified:**
  - MTDI = GPIO12 and MTDO = GPIO15 (standard naming; not quoted from the docs).
  - Boot-time glitches on 18/19/22/23/27 (docs say nothing).
  - DOIT silkscreen labels D18/D19/D22/D23/D27 and VIN (5 V) on the DOIT board (not an Espressif board; no official doc).
  - ADC2/WiFi has no effect on GPIO27 because it is used only as digital input.

## Decisions
- `board = esp32doit-devkit-v1` goes in the shared `[env]`, so both envs build for classic ESP32. Reason: Wokwi runs the same binary on a classic part.
- Replace `[env:esp32s3]` with `[env:esp32dev]` (`-DSERVO_SAFE_90=1`) and set `default_envs = esp32dev`. Reason: one pin map; the S3 board is not used.
- No logic changes; mock routes and real-AI code are not touched.

## Steps
1. [ ] `firmware/platformio.ini`: update the header comment (board, 4 MB, new pins), `default_envs`, `[env]` board and `[env:esp32dev]`; the wokwi env keeps its flags. → verify in step 4.
2. [ ] `firmware/src/main.cpp`: the 5 `#define` pins plus the S3 comments (line 1, line 25 `env esp32s3`). → `grep -n "S3\|esp32s3" src/` returns empty.
3. [ ] `firmware/diagram.json`: part `board-esp32-devkit-c-v4`; PWM/DIN/button wires → 18/19/22/23/27; keep 5V/GND.x/TX/RX. → valid JSON (`python -m json.tool`).
4. [ ] Build: `~/.platformio/penv/Scripts/pio run -e esp32dev` and `-e wokwi`. → both `SUCCESS` (confirms that ESP32Servo 3.0.6 and NeoPixel 1.12.3 compile for the classic ESP32).
4b. [ ] **STOP after the build → hand off to the real board first.** User flashes env `esp32dev` (SAFE_90) and checks CP2: `mosquitto_sub -v -t "wp/#"` shows `ONLINE,...`; unplug → `OFFLINE`. Give a recommendation (real board vs sim) along with the build result. Label something "hardware-verified" ONLY if it actually ran on the board.
5. [ ] **Fallback only, if the real board gets stuck (no wokwi-cli):** start mosquitto and the server with `WP_GATE_TIMEOUT=120 WP_CHOICE_TIMEOUT=180`; the user **restarts** the Wokwi simulator in VS Code (diagram changed) and keeps the tab visible. I run `WP_DEMO_SCALE=3 python ..\demo.py --sim`; the user clicks the button/presses B in C1. → A–D result, labelled "simulated, not hardware-verified". If it can't be run → report NOT RUN.
6. [ ] `stage1/README.md`: line 12 (PlatformIO classic ESP32), line 16 new wiring table (with VIN/5V for servos and LEDs, 3.3 V logic), line 36/44 `esp32s3`→`esp32dev`, checklist:
   - item 1: micro-USB **data** cable; CP210x or CH340 driver (check the chip next to the USB port) if Device Manager shows no COM port
   - item 3: `pio run -t upload` (env `esp32dev`); if it hangs at `Connecting....` hold **BOOT** until writing starts
   - items 4/6/8: GPIO22/GPIO23, `[env:esp32dev]`
   - wiring notes: separate 5 V supply for the servos with GND **shared** with the ESP; leave GPIO 0/2/5/12/15 **unconnected** while flashing; WS2812 caveat: 3.3 V data on a 5 V LED is marginal (power the LED from 3.3 V or use a level shifter / 74AHCT125 if it flickers)
   Also update the "Trạng thái" line. → `grep -n "S3\|esp32s3\|GPIO39\|GPIO11\|GPIO12\|GPIO15" README.md` returns empty.

## Not touched (to flag to user)
- The root `CLAUDE.md` still says ESP32-S3 / `esp32s3`; not edited unless the user asks.
- `ESP32/Source/lab4_parking_1file.ino` (old lab4 firmware) is out of scope.

## Verification
Step 4 builds both envs. Step 5 is the sim run of demo.py A–D (simulated only). Step 6 is the README grep.
