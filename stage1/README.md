# Giai đoạn 1 – luồng rửa/đỗ xe end-to-end (server → ESP32 qua WiFi/MQTT)

**AI được giả lập ở giai đoạn 1:** biển số và tình trạng bẩn/sạch lấy từ request (`/cam1`, `/cam2` trên Swagger `/docs`), chưa dùng camera/mô hình.

Trạng thái (2026-11-16): Test A–D **đạt trên mô phỏng Wokwi, CHƯA kiểm trên phần cứng**. Giai đoạn 1 chỉ xong khi `demo.py` đạt với ESP32 thật.

## Thành phần

| Thư mục | Nội dung |
|---|---|
| `server/` | FastAPI + SQLite + paho-mqtt: `app.py` (API), `flow.py` (máy trạng thái), `db.py`, `mqtt_link.py`, `ai.py` (mock), `test_flow.py` |
| `firmware/` | PlatformIO ESP32-S3: `src/main.cpp`, `src/net_config.h`, `src/secrets.h.example`; `wokwi.toml` + `diagram.json` cho mô phỏng |
| `mosquitto.conf` | broker nghe 0.0.0.0:1883, không mật khẩu |
| `demo.py` | chạy Test A–D qua HTTP (chỉ thư viện chuẩn) |

Chân (bo lab4): servo cổng vào GPIO18, servo cổng rửa GPIO39, LED WS2812 GPIO11 (online) / GPIO12 (chọn), nút GPIO15 → GND. **Nạp firmware này sẽ ghi đè firmware lab4.**

## Chạy

```powershell
# 1. Broker (service mosquitto của Windows đã tắt, để file conf này giữ cổng 1883)
& "C:\Program Files\mosquitto\mosquitto.exe" -c stage1\mosquitto.conf -v
# 2. Server (trong stage1/server; lần đầu: py -3.12 -m venv .venv; .venv\Scripts\pip install -r requirements.txt)
.venv\Scripts\uvicorn app:app --host 0.0.0.0 --port 8000
# 3. Kiểm tra logic không cần ESP
.venv\Scripts\python test_flow.py                     # nhanh, timeout 0.2 s
.venv\Scripts\python test_flow.py --default-timeouts  # timeout thật 30 s / 15 s (~2 phút)
# 4. Demo (server + broker + ESP đang chạy)
.venv\Scripts\python ..\demo.py
```

`POST /test/drop_wifi?sec=` chỉ dùng cho Test D của giai đoạn 1 — **bỏ khi triển khai thật**.

## Khác biệt bản mô phỏng và bản mạch thật

Mỗi khác biệt nằm sau đúng **một** cờ build (firmware) hoặc biến môi trường (server/demo). **Không đặt gì = bản mạch thật.** Firmware: `pio run` (mặc định env `esp32s3`) = mạch thật; `pio run -e wokwi` = mô phỏng.

| Khác biệt | Cờ / biến | Mạch thật (mặc định) | Mô phỏng Wokwi | Vì sao mô phỏng khác |
|---|---|---|---|---|
| Mạng WiFi + broker | `SIM_WOKWI` (build flag) | không đặt: `secrets.h` (hotspot, `BROKER_IP`), TCP timeout 1 s | `Wokwi-GUEST` kênh 6, `BROKER_HOST=host.wokwi.internal`, TCP timeout 5 s | mạng ảo của Wokwi |
| Heartbeat MQTT | `SIM_HEARTBEAT_MS` | `0` (tắt, chỉ keepalive 5 s) | `2000` | mô phỏng chạy ~50% tốc độ → PINGREQ tới trễ, broker cắt kết nối |
| Cửa sổ chọn phía ESP | `CHOICE_MAX_MS` | `20000` | `400000` | phải dài hơn `WP_CHOICE_TIMEOUT` của server |
| Nút ảo qua serial (`b`) | `SIM_SERIAL_BUTTON` | `0` | `1` | để bấm nút tự động trong demo |
| Servo an toàn 1500 us | `SERVO_SAFE_90` | `1` trong env `esp32s3` (tới khi biết loại servo) | `0` | Wokwi chỉ có servo 180° |
| Timeout cổng (server) | `WP_GATE_TIMEOUT` | `30` | `120` | mô phỏng chậm |
| Timeout chọn (server) | `WP_CHOICE_TIMEOUT` | `15` | `180` | đủ thời gian bấm nút trong mô phỏng |
| Hệ số chờ của demo | `WP_DEMO_SCALE` | `1` | `3` | mô phỏng chậm |

Chạy demo mô phỏng: server với `WP_GATE_TIMEOUT=120`, `WP_CHOICE_TIMEOUT=180`; `WP_DEMO_SCALE=3 python ..\demo.py --sim`; mở thư mục `stage1/firmware` trong VS Code → F1 → *Wokwi: Start Simulator*, **giữ tab mô phỏng hiển thị** (ẩn tab thì mô phỏng dừng).

## Checklist ngày có phần cứng (làm đúng thứ tự)

1. **Cáp / cổng COM:** cắm ESP32-S3 (thử cổng USB-C "COM" hoặc "USB", cáp có dây dữ liệu). Kiểm tra: `pio device list` hoặc Device Manager thấy cổng mới (không phải "Standard Serial over Bluetooth").
2. **Hotspot + `secrets.h`:** bật hotspot điện thoại **2.4 GHz** (ESP32 không vào được 5 GHz), cho laptop vào hotspot, chạy `ipconfig` → lấy IPv4 của Wi-Fi. Chép `firmware/src/secrets.h.example` → `secrets.h`, điền `WIFI_SSID`, `WIFI_PASS`, `BROKER_IP` = IP vừa lấy. **IP đổi mỗi lần đổi mạng → kiểm tra lại mỗi buổi.** Tường lửa: rule "Mosquitto MQTT 1883" phải bật cho cả **Public** (hotspot thường là Public).
3. **Nạp bản SAFE_90:** trong `stage1/firmware`: `pio run -t upload` (env mặc định `esp32s3`, có `SERVO_SAFE_90=1`). Mở `pio device monitor`, thấy `SAFE_90=1` và dòng `ca 2 servo nhan 1500 us`.
4. **Kiểm CP2:** `mosquitto_sub -v -t "wp/#"` thấy `ONLINE,GATE=CLOSED,WASH=CLOSED,GATE_CYCLES=0`; LED GPIO11 xanh. Rút điện ESP → trong ~8 s thấy `wp/esp/status OFFLINE`.
5. **Quyết định loại servo:** quan sát 2 servo khi nhận 1500 us. Đứng yên ở giữa hành trình → servo 180°. Quay liên tục (dù chậm) → servo 360° → **dừng lại**, không bật chuyển động; báo lại để đổi code.
6. **Tắt SAFE_90 (chỉ khi là servo 180°):** xoá dòng `-DSERVO_SAFE_90=1` trong `[env:esp32s3]` của `platformio.ini`, nạp lại.
7. **CP3:** từ `/docs` gọi `/cam1` → cổng vào mở rồi tự đóng sau 5 s, `GATE_CYCLES` tăng, lượt → `INSPECTING`. Hiệu chỉnh góc `GATE_*_DEG` / `WASH_*_DEG` trong `main.cpp` theo cổng thật.
8. **CP4:** `/cam2 CLEAN` → LED GPIO12 nhấp nháy, bấm nút → `WASHING`; không bấm → `PARKED` sau 15 s. `POST /test/drop_wifi?sec=15` → `OFFLINE` rồi `ONLINE` lại.
9. **demo.py với timeout mặc định:** server chạy **không** đặt `WP_GATE_TIMEOUT` / `WP_CHOICE_TIMEOUT`; `python ..\demo.py` (không `--sim`, không `WP_DEMO_SCALE`). Bấm nút khi được nhắc. Tất cả đạt → giai đoạn 1 xong.
