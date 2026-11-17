# Sơ đồ nối dây – ESP32 DevKit V1 (giai đoạn 1)

Nguồn chân: `firmware/src/main.cpp`. Nhãn D18… là nhãn in trên mạch DOIT (chưa đối chiếu tài liệu chính thức).

## Bảng chân

| Thiết bị | Chân thiết bị | Nối vào |
|---|---|---|
| **Servo cổng vào** | tín hiệu (cam/vàng) | **D18** (GPIO18) |
| | V+ (đỏ) | **+5 V nguồn riêng** |
| | GND (nâu/đen) | **− nguồn riêng** và **GND ESP** |
| **Module relay bơm nước** | IN | **D23** (GPIO23) |
| | VCC | **VIN** (5 V từ USB) |
| | GND | GND ESP |
| | COM / NO | nối tiếp vào dây + của bơm (xem dưới) |
| **OLED 128x64 I2C** | GND | GND ESP |
| | VDD | **3V3** (không dùng VIN/5 V) |
| | SCK | **D22** (GPIO22) |
| | SDA | **D21** (GPIO21) |
| **Nút chọn rửa** | chân 1 | **D27** (GPIO27) |
| | chân 2 | GND ESP (không cần điện trở, firmware bật pull-up trong) |

Màu dây servo theo quy ước SG90 thường gặp; kiểm tra nhãn trên servo của bạn. OLED và relay: nối theo **nhãn in trên module**.

## Nguồn và GND

```
 USB (laptop) ──► ESP32 DevKit V1
                   ├─ 3V3 ──► OLED VDD
                   ├─ VIN ──► relay VCC
                   ├─ GND ──┬─► OLED GND, relay GND, nút (chân 2)
                   │        └─► GND chung ◄──┐
 Nguồn 5 V riêng (servo)                     │
   ├─ +5 V ──► V+ servo cổng vào             │
   └─ −    ──► GND servo cổng vào ───────────┘

 Nguồn bơm (+) ──► relay COM   relay NO ──► bơm (+)
 Nguồn bơm (−) ─────────────────────────────► bơm (−)
```

- Servo **không** lấy nguồn từ VIN/3V3 của ESP: servo kéo dòng lớn có thể làm ESP reset.
- **GND phải nối chung** (ESP, nguồn servo, OLED, relay). Thiếu GND chung → servo giật hoặc không chạy.
- Bơm nối qua **COM/NO**: mất điện hoặc ESP reset → bơm tắt.
- Chỉ dùng bơm **điện một chiều điện áp thấp** (5–12 V). Bơm 220 V: **dừng lại**, hỏi trước khi nối.

## Relay bơm nước

Nhận `OPEN_WASH` (xe bẩn → server gửi ngay; xe sạch → khi khách bấm nút) → relay đóng, bơm chạy **10 s** rồi ESP tự tắt (không cần mạng). OLED hiện `BOM NUOC: CHAY` / `TAT`.

Mức kích relay **chưa kiểm**: firmware mặc định `RELAY_ACTIVE_LOW=1` (đa số module relay rẻ kích mức thấp).

## Chân không được dùng

| Chân | Lý do |
|---|---|
| D0, D2, D5, D12, D15 | chân strapping: nối vào có thể làm mạch không khởi động / không nạp được |
| GPIO 6–11 | nối với bộ nhớ flash |
| D34–D39 | chỉ đọc vào, không có pull-up |

D19 hiện còn trống (trước đây là servo cổng rửa, đã bỏ).

## Thứ tự làm

1. Rút USB, tắt nguồn servo, rồi mới nối dây. **Lần đầu: chưa nối bơm vào COM/NO.**
2. Cắm USB → ESP chạy, OLED hiện `MANG: OFFLINE` rồi `MANG: ONLINE` khi vào được broker. Relay phải **nhả** (LED relay tắt, không có tiếng tách). Nếu relay hút ngay lúc khởi động → mức kích ngược: đổi `RELAY_ACTIVE_LOW` (xem bảng dưới).
3. Gửi thử `OPEN_WASH` → relay tách đóng, 10 s sau tách nhả. Đúng rồi mới nối bơm.
4. Bật nguồn 5 V của servo.
5. Bản đang nạp có `SERVO_SAFE_90=1`: servo chỉ nhận xung 1500 us lúc khởi động. Đứng yên ở giữa → servo 180°. Quay liên tục → servo 360°: **dừng lại**, không tắt SAFE_90.

## Kiểm tra nhanh

| Hiện tượng | Kiểm tra |
|---|---|
| OLED trắng/không hiện | log khởi động có `[OLED] thay 0x3C`? Nếu `KHONG thay`: kiểm tra SDA/SCK có bị đảo, VDD có ở 3V3 |
| OLED lệch hình, có cột rác ở mép | chip là SH1106: thêm `-DOLED_SH1106=1` vào `[env:esp32dev]` rồi nạp lại |
| Không lên `ONLINE` | `BROKER_IP` trong `firmware/src/secrets.h` đúng IP laptop hiện tại? Mosquitto đang chạy? |
| Relay hút lúc khởi động, nhả khi `OPEN_WASH` | mức kích ngược: thêm `-DRELAY_ACTIVE_LOW=0` vào `[env:esp32dev]` rồi nạp lại |
| Relay hút mãi không nhả | module relay 5 V kích mức thấp nhận 3,3 V từ ESP có thể không nhả: thử cấp relay VCC bằng 3V3, hoặc dùng module có jumper chọn mức kích / module 3,3 V |
| ESP tự reset khi servo chạy | servo đang lấy nguồn từ ESP → chuyển sang nguồn 5 V riêng |
| Nhấn nút không có tác dụng | chỉ có tác dụng khi OLED đang nhấp nháy `BAM NUT DE RUA XE` |
