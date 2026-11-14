NẠP FIRMWARE ESP32-S3 - DÙNG CHUNG CHO MÔ HÌNH 1 VÀ MÔ HÌNH 2
=============================================================

Firmware: lab4_parking_1file\lab4_parking_1file.ino
  Nhận gói AI (camera, Lab 04), gói ML (Lab 03) và lệnh PLATE (hiện biển số lên LCD),
  khóa cổng vào khi bãi đầy, coi ô có cảm biến hỏng là bị chiếm.

BƯỚC 1. Arduino IDE 2: Boards Manager cài "esp32" của Espressif (đã thử bản 3.3.12);
        Library Manager cài "Adafruit NeoPixel" (1.15.5) và "LiquidCrystal I2C" (2.0.0).
BƯỚC 2. Mở lab4_parking_1file.ino, chọn mô hình ở dòng 17 (khung "CHỌN MÔ HÌNH" đầu tệp, chỉ có một dòng này):
            #define MO_HINH 1    Mô hình 1: góc cổng học trên web; ô 1 dùng GPIO4 / GPIO5
            #define MO_HINH 2    Mô hình 2: góc cổng cố định trong code; ô 1 dùng GPIO38 / GPIO10
BƯỚC 3. Menu Tools: Board "ESP32S3 Dev Module", USB CDC On Boot "Disabled", Flash Size "4MB",
        Partition Scheme "Default 4MB with spiffs", PSRAM "Disabled",
        Erase All Flash Before Sketch Upload "Disabled" (giữ góc cổng và cài đặt đã lưu).
        Chọn cổng COM của board (USB-Enhanced-SERIAL CH343) rồi bấm Upload.
BƯỚC 4. Mở Serial Monitor 115200: mỗi 2 giây board in trạng thái 4 ô và 2 cổng.
        Kiểm tra xong ĐÓNG Serial Monitor và Arduino IDE rồi mới chạy 2_chay_AI.bat.

Thử LCD hiện biển số: gõ trong Serial Monitor (chọn New Line)
    PLATE,59A1-815.91,1,Xe mo hinh 01     LCD: "BIEN 59A1-815.91" / "Xe mo hinh 01"
    PLATE,51F-999.99,0,                   LCD: "BIEN 51F-999.99"  / "XE LA CHUA DK"

Thư mục _ban_cu_khong_dung: baixe.ino (Mô hình 1) và baixe_MH2.ino (Mô hình 2) bản cũ,
chỉ để tham khảo. Hai bản này KHÔNG có lệnh PLATE, khóa cổng khi bãi đầy và xử lý cảm biến hỏng.
