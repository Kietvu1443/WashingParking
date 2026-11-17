# Giai đoạn 1 – luồng rửa/đỗ xe end-to-end (server → ESP32 qua WiFi/MQTT)

**AI được giả lập ở giai đoạn 1:** biển số và tình trạng bẩn/sạch lấy từ request (`/cam1`, `/cam2` trên Swagger `/docs`), chưa dùng camera/mô hình. Có thể bật AI thật bằng `AI_MODE=real` (xem mục **AI thật** ở cuối).

Trạng thái (2026-11-17): giao diện web + gán nhãn đã thêm (kiểm bằng trình duyệt trên server thử, ESP giả qua MQTT). firmware chuyển sang **ESP32 DevKit V1** (ESP32 thường); **CP2 (ONLINE/OFFLINE) đạt trên mạch thật** (bản còn LED WS2812). Sau đó đổi LED sang màn OLED: mạch thật thấy OLED ở 0x3C, vẽ 1 lần ~35 ms, người dùng xác nhận màn hiện. Bỏ servo cổng rửa; bơm nước qua relay GPIO23 (chạy 10 s khi nhận `OPEN_WASH`): build được, **relay chưa kiểm trên mạch thật**. **Chưa chạy lại Test A–D trên mô phỏng** (kết quả Wokwi A–D ngày 2026-11-16 là của bản ESP32-S3 cũ). Giai đoạn 1 chỉ xong khi `demo.py` đạt với ESP32 thật.

## Thành phần

| Thư mục | Nội dung |
|---|---|
| `server/` | FastAPI + SQLite + paho-mqtt: `app.py` (API), `flow.py` (máy trạng thái), `db.py`, `mqtt_link.py`, `ai.py` (mock), `test_flow.py` |
| `firmware/` | PlatformIO ESP32 thường (DevKit V1, board `esp32doit-devkit-v1`, 4 MB): `src/main.cpp`, `src/net_config.h`, `src/secrets.h.example`; `wokwi.toml` + `diagram.json` cho mô phỏng |
| `mosquitto.conf` | broker nghe 0.0.0.0:1883, không mật khẩu |
| `demo.py` | chạy Test A–D qua HTTP (chỉ thư viện chuẩn) |

Nối dây (ESP32 DevKit V1; nhãn D18… trên mạch DOIT, chưa đối chiếu tài liệu chính thức):

| Thiết bị | Tín hiệu | Nguồn |
|---|---|---|
| Servo cổng vào | GPIO18 (D18) | 5 V riêng, GND chung với ESP |
| Module relay bơm nước | IN → GPIO23 (D23) | VCC → VIN, GND chung; bơm nối qua COM/NO |
| OLED 128x64 I2C (trạng thái + nhắc chọn) | SDA → GPIO21 (D21), SCK/SCL → GPIO22 (D22) | VDD → **3V3**, GND → GND |
| Nút | GPIO27 (D27) → GND (INPUT_PULLUP) | – |

- Logic ESP32 là **3,3 V**. Servo dùng nguồn 5 V riêng (không lấy từ chân ESP), **GND nối chung** với ESP.
- OLED: nối theo **nhãn in trên module** (thứ tự chân khác nhau giữa các loại). Cấp 3V3, không cấp 5 V. Màn hiện `MANG`, `CONG VAO`, `BOM NUOC`; khi chờ chọn thì ô `BAM NUT DE RUA XE` nhấp nháy. Log khởi động có `[OLED] thay 0x3C` / `KHONG thay` (không thấy màn thì phần còn lại vẫn chạy).
- Không nối gì vào GPIO 0/2/5/12/15 (chân strapping) khi nạp. Tránh GPIO 6–11 (flash) và 34–39 (chỉ vào, không pull-up).

## Chạy

**Nhanh nhất:** `python stage1\server\run.py` (từ thư mục bất kỳ; tự tạo `.venv` + cài thư viện khi cần, camera kiểm tra
mặc định USB 0, `--cam 1` / `--cam rtsp://...` / `--cam none`, `--port`). Mở `http://127.0.0.1:8000/label`. Broker vẫn
phải chạy riêng (bước 1) nếu dùng luồng rửa/gửi xe.

```powershell
# 1. Broker (service mosquitto của Windows đã tắt, để file conf này giữ cổng 1883)
& "C:\Program Files\mosquitto\mosquitto.exe" -c stage1\mosquitto.conf -v
# 2. Server (trong stage1/server; lần đầu: py -3.12 -m venv .venv; .venv\Scripts\pip install -r requirements.txt)
.venv\Scripts\uvicorn app:app --host 127.0.0.1 --port 8000   # chỉ máy này; mở mạng: xem WP_WEB_PASS bên dưới
# 3. Kiểm tra logic không cần ESP
.venv\Scripts\python test_flow.py                     # nhanh, timeout 0.2 s
.venv\Scripts\python test_flow.py --default-timeouts  # timeout thật 30 s / 15 s (~2 phút)
# 4. Demo (server + broker + ESP đang chạy)
.venv\Scripts\python ..\demo.py
```

`POST /test/drop_wifi?sec=` chỉ dùng cho Test D của giai đoạn 1 — **bỏ khi triển khai thật**.

## Giao diện web + gán nhãn

Mở `http://127.0.0.1:8000/` (cùng server, không cần chạy thêm gì). Mặc định **không có mật khẩu** (server ghi cảnh báo
khi khởi động) nên chỉ chạy `--host 127.0.0.1`. Muốn mở cho máy khác (`--host 0.0.0.0`) thì **bắt buộc** đặt
`$env:WP_WEB_PASS="<mật khẩu>"`: mọi trang/API đòi Basic-auth (tên đăng nhập tuỳ ý). `demo.py` tự gửi mật khẩu nếu biến
này được đặt. ESP nói chuyện qua broker nên không bị ảnh hưởng.

- **Vận hành:** trạng thái ESP, lượt hiện tại (biển, trạng thái, ảnh camera 1), nút *Xe vào* (`/cam1`), *BẨN / SẠCH* (`/cam2`), *Rửa xe / Gửi xe* (`/choice`, có đếm ngược 15 s), lượt gần đây, nút *Ngắt WiFi 15 s*. Bản `AI_MODE=real` có ảnh camera trực tiếp (`/cam/cam1.jpg`, `/cam/cam2.jpg`) và chọn tệp ảnh.
- **Gán nhãn – Bẩn/Sạch** (`dataset_ban_sach/`): tải ảnh lên theo lớp, chuyển lớp, xoá, *Tải zip*. Bản `AI_MODE=real` **tự lưu** ảnh camera 2 mỗi khi nhân viên bấm BẨN/SẠCH (cắt theo khung xe nếu AI thấy xe). Zip: `images/<ban|sach>/*.jpg` + `classes.txt` (kiểu YOLO classify; notebook phải tự chia train/val – khác `train_hu_hong.ipynb` vì đó là YOLO detect).
- **Gán nhãn – Biển số** (`dataset_bien_so/`): bản `AI_MODE=real` tự lưu ảnh cắt biển khi nhân viên **gõ** biển ở Camera 1 và AI có thấy khung biển; sửa chữ, xoá, *Tải zip* (`images/` + `labels/` + `rec_gt.txt` kiểu PaddleOCR). Chỉ bắt được biển AI không chắc (422 → nhân viên gõ); biển AI đọc sai mà tự tin thì không lưu. Chưa kiểm định dạng chữ (có `-`/`.` hay không) khớp `rec.onnx` trước khi huấn luyện.
- Xoá = chuyển vào `_deleted/`, không xoá hẳn. Thư mục dữ liệu: `WP_DATASET_DIR` (mặc định `stage1/server`), đã gitignore, không bao giờ ghi vào `Brain/`.
- Kiểm tra: `.venv\Scripts\python test_dataset.py` → `30 test đạt`.

### Hai bộ ảnh bẩn/sạch – đừng trộn

| Thư mục | Nguồn ảnh | Nhãn | Dùng cho |
|---|---|---|---|
| `dataset_ban_sach/` | tự lưu từ `/cam2` (`AI_MODE=real`) + tải lên | **chưa kiểm** | tham khảo |
| `dataset_dirt/` | **chỉ** chụp tay qua trang `/label` | gán bằng mắt ngay lúc chụp | chỉnh ngưỡng luật bụi, sau này huấn luyện |

### Trang `/label` (camera kiểm tra, `dataset_dirt/`)

Mở `http://127.0.0.1:8000/label` (nút *Chụp bộ bẩn/sạch* trên trang chính). Cần `WP_CAM2_SRC` (số USB hoặc `rtsp://…`;
bản mock cũng mở camera nếu đặt biến này, khi đó cần `requirements-ai.txt`). Xem trước ~1 s/lần; **BẨN / SẠCH** = chụp +
gán nhãn một lần bấm; *Xoá ảnh vừa chụp* (chỉ trong phiên hiện tại, chuyển vào `_deleted/`); *Phiên mới*; *Chụp ảnh tham
chiếu (SẠCH)* cho phiên hiện tại; *Đánh giá*; *Tải zip* (`clean/`, `dirty/`, `metadata.jsonl`, mọi `reference_<phiên>.jpg`).
`metadata.jsonl` chỉ ghi thêm: `{id, cls, ext, at, session, camera}` (camera đã che mật khẩu); xoá = dòng `{id, deleted: true}`.

**Luật bụi (`dust.py`) chỉ là heuristic thô, không phải mô hình**: so ảnh với ảnh SẠCH tham chiếu của *cùng phiên* trong
vùng ROI, cân độ sáng (theo trung vị), làm mờ, đếm tỉ lệ điểm ảnh khác > `WP_DUST_PIX`; BẨN nếu tỉ lệ > `WP_DUST_RATIO`.
- Ảnh tham chiếu theo phiên (`reference_<phiên>.jpg`; chụp lại thì bản cũ vào `_deleted/`). *Đánh giá* so mỗi ảnh với
  tham chiếu của chính phiên nó, **bỏ qua** (và báo số lượng) ảnh thuộc phiên chưa có tham chiếu. `/cam2` thật sau này sẽ
  dùng tham chiếu mới nhất.
- *Đánh giá* chọn ngưỡng trên các phiên đầu và đo precision/recall (BẨN = dương) trên ~30% phiên cuối giữ riêng. Chỉ có
  1 phiên thì ghi rõ "ngưỡng chọn và đo trên cùng dữ liệu". Lớp < 30 ảnh → chỉ là chỉ báo thô.
- Giới hạn: camera tự chỉnh phơi sáng (cân sáng chỉ bù được một phần), xe đỗ lệch chỗ, bóng đổ, phản chiếu đều bị tính là
  "khác". Nên: điểm dừng xe cố định, khoá phơi sáng / cân bằng trắng của camera, **chụp lại ảnh tham chiếu (phiên mới)
  sau mỗi lần đổi ánh sáng**.
- Biến môi trường: `WP_DUST_ROI="x1,y1,x2,y2"` (phân số 0..1, mặc định cả khung), `WP_DUST_PIX` (mặc định 25),
  `WP_DUST_RATIO` (mặc định 0.05). Sai giá trị → *Đánh giá* báo lỗi 409.
- Kiểm tra: `test_dirt_label.py` → `29 test đạt`, `test_dust.py` → `25 test đạt` (cần numpy/cv2), `test_cams.py` → `9 test đạt`.

## Khác biệt bản mô phỏng và bản mạch thật

Mỗi khác biệt nằm sau đúng **một** cờ build (firmware) hoặc biến môi trường (server/demo). **Không đặt gì = bản mạch thật.** Firmware: `pio run` (mặc định env `esp32dev`) = mạch thật; `pio run -e wokwi` = mô phỏng.

| Khác biệt | Cờ / biến | Mạch thật (mặc định) | Mô phỏng Wokwi | Vì sao mô phỏng khác |
|---|---|---|---|---|
| Mạng WiFi + broker | `SIM_WOKWI` (build flag) | không đặt: `secrets.h` (hotspot, `BROKER_IP`), TCP timeout 1 s | `Wokwi-GUEST` kênh 6, `BROKER_HOST=host.wokwi.internal`, TCP timeout 5 s | mạng ảo của Wokwi |
| Heartbeat MQTT | `SIM_HEARTBEAT_MS` | `0` (tắt, chỉ keepalive 5 s) | `2000` | mô phỏng chạy ~50% tốc độ → PINGREQ tới trễ, broker cắt kết nối |
| Cửa sổ chọn phía ESP | `CHOICE_MAX_MS` | `20000` | `400000` | phải dài hơn `WP_CHOICE_TIMEOUT` của server |
| Nút ảo qua serial (`b`) | `SIM_SERIAL_BUTTON` | `0` | `1` | để bấm nút tự động trong demo |
| Chip OLED | `OLED_SH1106` | `0` (SSD1306); đặt `-DOLED_SH1106=1` trong env `esp32dev` nếu ảnh lệch/có cột rác ở mép | `0` | Wokwi chỉ có SSD1306 |
| Mức kích relay | `RELAY_ACTIVE_LOW` | `1` (đoán: đa số module rẻ kích mức thấp, chưa kiểm module thật) | `1` (relay Wokwi đặt `transistor` rõ trong `diagram.json`, chưa chạy) | – |
| Servo an toàn 1500 us | `SERVO_SAFE_90` | `1` trong env `esp32dev` (tới khi biết loại servo) | `0` | Wokwi chỉ có servo 180° |
| Timeout cổng (server) | `WP_GATE_TIMEOUT` | `30` | `120` | mô phỏng chậm |
| Timeout chọn (server) | `WP_CHOICE_TIMEOUT` | `15` | `180` | đủ thời gian bấm nút trong mô phỏng |
| Hệ số chờ của demo | `WP_DEMO_SCALE` | `1` | `3` | mô phỏng chậm |

Chạy demo mô phỏng: server với `WP_GATE_TIMEOUT=120`, `WP_CHOICE_TIMEOUT=180`; `WP_DEMO_SCALE=3 python ..\demo.py --sim`; mở thư mục `stage1/firmware` trong VS Code → F1 → *Wokwi: Start Simulator*, **giữ tab mô phỏng hiển thị** (ẩn tab thì mô phỏng dừng).

## Checklist ngày có phần cứng (làm đúng thứ tự)

1. **Cáp / cổng COM:** cắm ESP32 DevKit V1 bằng cáp micro-USB **có dây dữ liệu** (cáp chỉ sạc sẽ không hiện cổng). Kiểm tra: `pio device list` hoặc Device Manager thấy cổng mới (không phải "Standard Serial over Bluetooth"). Không thấy cổng → xem chip cạnh cổng USB và cài driver **CP210x** hoặc **CH340** tương ứng.
2. **Hotspot + `secrets.h`:** bật hotspot điện thoại **2.4 GHz** (ESP32 không vào được 5 GHz), cho laptop vào hotspot, chạy `ipconfig` → lấy IPv4 của Wi-Fi. Chép `firmware/src/secrets.h.example` → `secrets.h`, điền `WIFI_SSID`, `WIFI_PASS`, `BROKER_IP` = IP vừa lấy. **IP đổi mỗi lần đổi mạng → kiểm tra lại mỗi buổi.** Tường lửa: rule "Mosquitto MQTT 1883" phải bật cho cả **Public** (hotspot thường là Public).
3. **Nạp bản SAFE_90:** trong `stage1/firmware`: `pio run -t upload` (env mặc định `esp32dev`, có `SERVO_SAFE_90=1`). Nếu dừng ở `Connecting....`: giữ nút **BOOT** tới khi bắt đầu ghi. Mở `pio device monitor`, thấy `SAFE_90=1` và dòng `servo cong vao nhan 1500 us`.
4. **Kiểm CP2:** `mosquitto_sub -v -t "wp/#"` thấy `ONLINE,GATE=CLOSED,WASH=CLOSED,GATE_CYCLES=0`; OLED hiện `MANG: ONLINE` (log có `[OLED] thay 0x3C`). Rút điện ESP → trong ~8 s thấy `wp/esp/status OFFLINE`.
5. **Quyết định loại servo:** quan sát servo cổng vào khi nhận 1500 us. Đứng yên ở giữa hành trình → servo 180°. Quay liên tục (dù chậm) → servo 360° → **dừng lại**, không bật chuyển động; báo lại để đổi code.
6. **Tắt SAFE_90 (chỉ khi là servo 180°):** xoá dòng `-DSERVO_SAFE_90=1` trong `[env:esp32dev]` của `platformio.ini`, nạp lại.
7. **CP3:** từ `/docs` gọi `/cam1` → cổng vào mở rồi tự đóng sau 5 s, `GATE_CYCLES` tăng, lượt → `INSPECTING`. Hiệu chỉnh góc `GATE_*_DEG` trong `main.cpp` theo cổng thật.
8. **CP4:** `/cam2 CLEAN` → OLED nhấp nháy `BAM NUT DE RUA XE`, bấm nút → `WASHING`, relay bơm đóng, OLED `BOM NUOC: CHAY` rồi tự `TAT` sau 10 s (lần đầu chưa nối bơm, xem `WIRING.md`); không bấm → `PARKED` sau 15 s. `POST /test/drop_wifi?sec=15` → `OFFLINE` rồi `ONLINE` lại.
9. **demo.py với timeout mặc định:** server chạy **không** đặt `WP_GATE_TIMEOUT` / `WP_CHOICE_TIMEOUT`; `python ..\demo.py` (không `--sim`, không `WP_DEMO_SCALE`). Bấm nút khi được nhắc. Tất cả đạt → giai đoạn 1 xong.

## AI thật (`AI_MODE=real`, tuỳ chọn)

Không đặt `AI_MODE` (hoặc `AI_MODE=mock`) = AI giả lập như trên, `/cam1` `/cam2` nhận JSON y như cũ (`demo.py`, Wokwi không đổi). `AI_MODE=real` thay 2 route này bằng bản multipart chạy mô hình thật của `Brain/`.

**Phụ thuộc `Brain/`:** `server/ai.py` chỉ *import* `station.vision`, `station.plate`, `station.camera` từ thư mục `Brain/` (chưa có trong git, trước đây tên `SmartPK_PC/`) và không ghi gì vào đó. Đường dẫn mặc định: `<repo>/Brain`; khác thì đặt `WP_BRAIN_DIR`. Cần có `Brain/xe_best.onnx`, `Brain/bien_so.onnx`, `Brain/rec.onnx`; thiếu Brain hoặc mô hình → server báo lỗi rõ ngay khi khởi động.

```powershell
# trong stage1/server, cài 1 lần (cùng phiên bản với Brain)
.venv\Scripts\pip install -r requirements-ai.txt
$env:AI_MODE="real"; .venv\Scripts\uvicorn app:app --host 127.0.0.1 --port 8000
.venv\Scripts\python test_ai_real.py     # tự kiểm tra trên ảnh có sẵn trong Brain/ (không cần broker/ESP/camera)
```

| Biến | Mặc định | Ý nghĩa |
|---|---|---|
| `AI_MODE` | `mock` | `mock` hoặc `real` |
| `WP_BRAIN_DIR` | `../../Brain` (so với `server/`) | thư mục Brain |
| `WP_PLATE_MIN_CONF` | `0.95` | nhận biển khi đã xác nhận chéo **hoặc** tin cậy ≥ ngưỡng này |
| `WP_CAM1_SRC`, `WP_CAM2_SRC` | không đặt | số webcam USB (`0`, `1`…) hoặc `rtsp://user:pass@ip:554/...`; trùng nhau thì dùng chung 1 luồng camera |

**URL RTSP có mật khẩu: chỉ đặt trong biến môi trường** (`$env:WP_CAM1_SRC="rtsp://..."`), không ghi vào repo/README/test. Log chỉ hiện dạng đã che (`rtsp://***@ip`). Stage1 **tắt** chức năng dò LAN của `CamThread` (Brain gửi URL kèm mật khẩu tới mọi máy mở cổng 554 trong mạng): RTSP không vào được thì chỉ thử lại đúng địa chỉ đã đặt; camera đổi IP → sửa biến môi trường.

API khi `AI_MODE=real` (thử trên `/docs`):

- `POST /cam1` – `file` (ảnh, tuỳ chọn; không gửi = lấy khung mới nhất của `WP_CAM1_SRC`), `plate` (tuỳ chọn, biển nhân viên nhập tay).
  - AI đọc chắc chắn → tạo lượt, trả `{visit, ai}`; ảnh lưu ở `server/photos/` (`photo_path`).
  - AI không chắc → **422** `{detail: "cần nhập biển số thủ công", ai}`, không tạo lượt; gửi lại kèm `plate=...`.
  - `ai` = `{vehicle:{found,conf,box}, plate, conf, confirmed, box, ok, reason, ms:{car,plate,total}}`.
- `POST /cam2` – `file` (tuỳ chọn), `condition` (`DIRTY`/`CLEAN`, nhân viên chọn).
  - **Chưa có mô hình bẩn/sạch**: không có `condition` → 422, `ai.condition = null` (chỉ báo có xe hay không), lượt vẫn `INSPECTING`. Chỉ `condition` nhân viên gửi mới cho lượt đi tiếp. Không bao giờ đoán bẩn/sạch.

Đo trên ảnh có sẵn (2026-11-16, laptop, CPU): nạp mô hình ~0,7–1,8 s; tìm xe ~20 ms; đọc biển ~300 ms (tối đa ~0,7 s). 64 ảnh biển cắt sẵn: quy tắc nhận 49, từ chối 15, nhận mà sai 0 – so với bản đọc cũ trong tên file (đã soát bằng mắt 1 ảnh/nhóm), **không phải độ chính xác**. Độ chính xác thật: bỏ ảnh xe chụp thật vào `server/test_images/gt/<BIỂN>.jpg` rồi chạy `test_ai_real.py`.

**Còn thiếu cho bẩn/sạch (đề xuất, chưa làm):** gom ≥200 ảnh xe có nhãn DIRTY/CLEAN từ camera kiểm tra → train bộ phân loại 2 lớp, xuất ONNX → thêm biến `WP_DIRT_MODEL` cho `RealAI.cam2`. Tới lúc đó nhân viên chọn.
