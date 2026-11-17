
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_NeoPixel.h>

// ================================================================================
// CHỌN MÔ HÌNH TRƯỚC KHI NẠP: sửa số ở dòng dưới thành 1 hoặc 2
//   1 = Mô hình 1 (ô 1 dùng GPIO4 / GPIO5, góc cổng học trên web)
//   2 = Mô hình 2 (ô 1 dùng GPIO38 / GPIO10, góc cổng cố định trong code)
// arduino-cli: có thể thay bằng cờ -DMO_HINH=2 khi biên dịch.
// ================================================================================
#ifndef MO_HINH
#define MO_HINH 1
#endif

// ================================================================================
// PHẦN A – KHAI BÁO
// A1. CẤU HÌNH CHÂN
// Muốn đổi chân: chỉ sửa trong mục này.
// ================================================================================

// ----------------------------------------------------------------
// CHAN HC-SR04 (o 1, o 2 ben trai | o 3, o 4 ben phai)
// Muon doi chan o nao chi can sua 2 dong nay.
// ----------------------------------------------------------------
// O 4: TRIG = 47 (tren), ECHO = 21 (duoi) - ben phai shield.
// Cam nguoc TRIG/ECHO cung khong sao: code tu nhan va tu dao.
// trigPins[i] / echoPins[i] = chân TRIG / ECHO của ô i+1
// (Mô hình chọn ở dòng #define MO_HINH đầu tệp)
#if MO_HINH == 2
// Mô hình 2: dây ô 1 ở GPIO4/5 rồi GPIO15/16 không đo được, nên ô 1 chuyển sang
// TRIG = GPIO38, ECHO = GPIO10 (qua mạch chia áp); cắm ngược thì code tự nhận và tự đảo.
const uint8_t trigPins[4] = {38, 6, 1, 47};
const uint8_t echoPins[4] = {10, 7, 2, 21};
#else
const uint8_t trigPins[4] = {4, 6, 1, 47};
const uint8_t echoPins[4] = {5, 7, 2, 21};
#endif

// Den WS2812B o 1..4 (ben trai, lien nhau)
const uint8_t ledPins[4]  = {11, 12, 13, 14};

// Cong I2C rieng tren ESP32-S3 DEV KIT IO SHIELD: SDA = 8, SCL = 9
#define I2C_SDA        8
#define I2C_SCL        9

// ===== CHỌN MÔ HÌNH (2 mô hình dùng chung sơ đồ chân, chỉ khác cách lấy góc servo 2 cổng) =====
//  MO_HINH 1: góc mở / đóng học trên trang web rồi lưu flash (như baixe.ino)
//  MO_HINH 2: góc lấy từ 4 hằng số GATE_IN_ / GATE_OUT_ dưới đây (như baixe_MH2.ino)
//  Chọn mô hình ở dòng #define MO_HINH đầu tệp.
#if MO_HINH == 2
const float GATE_IN_OPEN   = 15;    // cổng VÀO – MỞ   (thanh chắn dựng lên)
const float GATE_IN_CLOSE  = 90;    // cổng VÀO – ĐÓNG (thanh chắn nằm ngang)
const float GATE_OUT_OPEN  = 29;    // cổng RA  – MỞ
const float GATE_OUT_CLOSE = 90;    // cổng RA  – ĐÓNG
// true  = luôn dùng 4 góc trên khi bật nguồn (chỉnh trên web chỉ tạm thời)
// false = ưu tiên góc đã "Lưu" trên web, chưa lưu thì dùng 4 góc trên
const bool  GATE_USE_CODE_ANGLES = true;
#endif

// Cong vao: ben trai (17, 18) | Cong ra: ben phai (39, 40)
#define SERVO_IN_PIN   18
#define IR_IN_PIN      17
#define SERVO_OUT_PIN  39
#define IR_OUT_PIN     40

// E18-D80NK NPN: co vat -> LOW
#define IR_ACTIVE_LOW  1

// ================================================================================
// A2. WIFI – điểm phát WiFi riêng của ESP32 + web server + DNS + bộ nhớ flash
// ================================================================================

// Tên WiFi / mật khẩu / kênh mà ESP32 phát ra
const char* WIFI_SSID     = "BAI_XE_ESP32S3";
const char* WIFI_PASSWORD = "12345678";
const int   WIFI_CHANNEL  = 6;
// Ten WiFi co the doi bang lenh UART (luu flash): WIFI,<ten> | WIFI,DEFAULT | WIFI,?   (de nhieu board trong lop khong trung ten)
String      wifiSsidNow   = WIFI_SSID;

// server: web cổng 80 | dnsServer: mọi tên miền → 192.168.4.1 | prefs: bộ nhớ flash
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;

// ================================================================================
// A3. [PHÂN LOẠI] NGƯỠNG ĐO XE (cm) – quyết định màu đèn / trạng thái ô
//   <= PARKING_LIMIT: có xe | GOOD_MIN..GOOD_MAX: đỗ tốt | < TOO_CLOSE: quá gần
// ================================================================================

const float PARKING_LIMIT = 20.0;   // <= 20 cm: co xe
const float GOOD_MIN      = 7.0;    // 5-10 cm: do tot
const float GOOD_MAX      = 12.0;
const float TOO_CLOSE     = 7.0;    // < 5 cm: qua gan
// > 20 cm hoac khong co vat: TRONG
// Tam do toi da ~5 m (pulseIn 30 ms). KHONG gioi han 0-20 cm khi do.

// ================================================================================
// A4. HC-SR04 – KẾT QUẢ ĐO TỪNG Ô (mảng 4 phần tử: chỉ số 0..3 = ô 1..4)
// ================================================================================

// parkingDistance: khoảng cách đo được (999 = không đo được / trống)
// finalState     : trạng thái hiển thị: EMPTY / GOOD / WARNING / CLOSE / AI
float         parkingDistance[4] = {999, 999, 999, 999};
bool          parkingOccupied[4] = {false, false, false, false};  // KET QUA CUOI (sau khi ket hop)
bool          hcOccupied[4]      = {false, false, false, false};  // theo HC-SR04
const char*   finalState[4]      = {"EMPTY", "EMPTY", "EMPTY", "EMPTY"};

// ================================================================================
// A5. CAMERA AI (nhận qua UART từ PC) + NHẬT KÝ XE + TỰ NHẬN TRIG/ECHO
// ================================================================================

// aiSlot: camera AI báo ô có xe | aiLastMs: lần cuối nhận AI | slotSource: nguồn xác định chỗ đỗ
// mismatch...: AI và HC-SR04 báo khác nhau | serialBuf0/1: bộ đệm dòng lệnh USB / UART
bool          aiSlot[4]       = {false, false, false, false};
unsigned long aiLastMs        = 0;
unsigned long aiMsgCount      = 0;
const unsigned long AI_TIMEOUT = 3000;   // 3 s khong co du lieu -> coi nhu mat ket noi

// ===== MACHINE LEARNING (Lab 03) – kết quả máy tính gửi xuống qua gói ML,l1,l2,l3,l4,c1,c2,c3,c4 =====
// ML_TIMEOUT_MS : gói ML cũ hơn 1,5 s thì coi như mất ML, quay về dùng HC-SR04
// CONF_THRESHOLD: chỉ tin kết quả ML khi độ tin cậy từ 0,80 trở lên
// mlSlot / mlConf: nhãn (1 = có xe) và độ tin cậy của từng ô trong gói ML mới nhất
const unsigned long ML_TIMEOUT_MS  = 1500;
const float         CONF_THRESHOLD = 0.80;
bool          mlSlot[4]  = {false, false, false, false};
float         mlConf[4]  = {0, 0, 0, 0};
unsigned long mlLastMs   = 0;
unsigned long mlMsgCount = 0;
// slotOff: ô bị tắt (cảm biến hỏng) luôn báo LỖI và KHÔNG BAO GIỜ được tính là chỗ trống.
// Mặc định cả 4 ô đều bật. Tắt hoặc bật 1 ô bằng lệnh UART (lưu flash, không cần nạp lại code):
//   SLOT,1,OFF tắt ô 1 | SLOT,1,ON bật lại | SLOT,? xem trạng thái
bool          slotOff[4] = {false, false, false, false};
// ===== 2 CHẾ ĐỘ (đổi bằng lệnh UART MODE,FUZZY | MODE,MODEL | MODE,? hoặc nút trên web, lưu flash) =====
//  opMode 0 = FUZZY: chạy như bình thường theo HC-SR04 (nhiều màu đèn theo khoảng cách), ML không tham gia
//  opMode 1 = MODEL: mô hình học máy trên máy tính quyết định, chỉ 2 kết quả TRỐNG / BỊ CHIẾM, đèn xanh / đỏ
//  whySrc: lý do quyết định của từng ô để hiện trên web (ML, CONFLICT, LOWCONF, FALLBACK, NOSRC, OFF)
int           opMode     = 0;
const char*   whySrc[4]  = {"-", "-", "-", "-"};   // ly do quyet dinh cua tung o (hien tren web)
// mlFresh(): true khi đã nhận gói ML và gói gần nhất mới hơn ML_TIMEOUT_MS (1,5 s)
bool mlFresh() { return mlLastMs != 0 && millis() - mlLastMs <= ML_TIMEOUT_MS; }
int           slotSource      = 2;       // 0 = chi HC-SR04 | 1 = chi camera AI | 2 = ket hop
bool          mismatchFlag[4] = {false, false, false, false};
unsigned long mismatchSince[4]= {0, 0, 0, 0};
String        serialBuf0      = "";
String        serialBuf1      = "";

// Nhat ky xe vao / ra
struct ParkEvent { unsigned long t; uint8_t slot; bool in; uint8_t by; };  // by: 0 HC, 1 AI, 2 ca hai
ParkEvent     events[10];
int           eventCount      = 0;
// candOcc / candSince: trạng thái "ứng viên" + thời điểm bắt đầu (chống nhiễu 1 s)
// lcdMsg / lcdMsgUntil: thông báo sự kiện hiện trên LCD trong 3 s
bool          candOcc[4]      = {false, false, false, false};
unsigned long candSince[4]    = {0, 0, 0, 0};
String        lcdMsg          = "";
unsigned long lcdMsgUntil     = 0;
// plateL1 / plateL2 / plateUntil: kết quả đọc biển số do máy tính gửi (lệnh PLATE), hiện cả 2 dòng LCD trong 5 s
String        plateL1         = "";
String        plateL2         = "";
unsigned long plateUntil      = 0;
uint8_t       sensorErr[4]       = {0, 0, 0, 0};  // 0 OK | 1 khong co vang | 2 ECHO ket cao | 3 < 2cm
// noEchoSince[i]: thời điểm ô i bắt đầu mất tiếng vang liên tục (0 = đang có vang). Mất quá NO_ECHO_FAULT_MS
// thì coi là cảm biến hỏng (rút dây, đứt dây ECHO): báo mã lỗi 4 và tính ô là bị chiếm ở cả hai chế độ.
const unsigned long NO_ECHO_FAULT_MS = 3000;
unsigned long noEchoSince[4]       = {0, 0, 0, 0};
unsigned long sensorRawUs[4]     = {0, 0, 0, 0};
uint8_t       echoStuckCount[4]  = {0, 0, 0, 0};  // so lan ECHO ket cao lien tiep

// TU DONG NHAN TRIG/ECHO cam nguoc:
// Neu 1 cam bien chua lan nao do duoc, sau 6 lan loi lien tiep code tu dao vai tro
// 2 chan TRIG <-> ECHO. Do duoc 1 lan la khoa lai va luu vao bo nho.
bool          hcSwap[4]   = {false, false, false, false};
bool          hcLocked[4] = {false, false, false, false};
uint8_t       hcFail[4]   = {0, 0, 0, 0};

// trigOf / echoOf: chân TRIG / ECHO ĐANG DÙNG của ô i (đã tính việc tự đảo)
uint8_t trigOf(int i) { return hcSwap[i] ? echoPins[i] : trigPins[i]; }
// echoOf(i): chân ECHO đang dùng của ô i (đổi chỗ với TRIG nếu ô này đã tự đảo)
uint8_t echoOf(int i) { return hcSwap[i] ? trigPins[i] : echoPins[i]; }

// applyHcPins(i): đặt chế độ chân cho ô i – ECHO là đầu vào (kéo xuống), TRIG là đầu ra mức thấp
void applyHcPins(int i)
{
  pinMode(echoOf(i), INPUT_PULLDOWN);
  pinMode(trigOf(i), OUTPUT);
  digitalWrite(trigOf(i), LOW);
}
// freeSlots: số chỗ còn trống | currentSensor: ô sẽ đo ở lượt tới (đo lần lượt 0→1→2→3)
int           freeSlots          = 4;
int           currentSensor      = 0;

// ================================================================================
// A6. ĐÈN WS2812B – mỗi ô 1 bóng, 1 chân riêng
// ================================================================================

Adafruit_NeoPixel slotPixel[4] = {
  Adafruit_NeoPixel(1, 11, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(1, 12, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(1, 13, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(1, 14, NEO_GRB + NEO_KHZ800)
};
uint32_t lastLedColor[4] = {1, 1, 1, 1};   // chi show() khi doi mau

// ================================================================================
// A7. LCD 1602 I2C (biến) – tự dò địa chỉ, mất kết nối thì thử lại mỗi 10 s
// ================================================================================

LiquidCrystal_I2C* lcd = nullptr;
bool          lcdOK    = false;
uint8_t       lcdAddr  = 0;
String        i2cFound = "";
unsigned long lastLcdRetry = 0;

// ================================================================================
// A8. SERVO – lớp ContServo (điều khiển bằng LEDC, dùng cho cả servo 180° và 360°)
// ================================================================================

#define SERVO_FREQ      50
#define SERVO_RES_BITS  14
#define SERVO_STOP_US   1500   // diem dung cua servo 360
#define SERVO_MIN_US    500    // servo 180: xung o 0 do  (SG90)
#define SERVO_MAX_US    2500   // servo 180: xung o 180 do (SG90)

// ContServo: tạo xung servo bằng LEDC (không cần thư viện ESP32Servo)
//   attach()     gắn chân + kênh, dừng ngay       stop()   ngắt xung (servo 360 đứng yên)
//   writeAngle() servo 180: tới góc 0..180°       run()    servo 360: quay trái / phải
class ContServo
{
  public:
    uint8_t pin = 0;
    uint8_t channel = 0;

    bool attach(uint8_t p, uint8_t ch)
    {
      pin = p;
      channel = ch;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      bool ok = ledcAttach(pin, SERVO_FREQ, SERVO_RES_BITS);
#else
      ledcSetup(channel, SERVO_FREQ, SERVO_RES_BITS);
      ledcAttachPin(pin, channel);
      bool ok = true;
#endif
      stop();
      return ok;
    }

    void writeDuty(uint32_t duty)
    {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      ledcWrite(pin, duty);
#else
      ledcWrite(channel, duty);
#endif
    }

    // Dung han: ngat xung -> servo 360 dung, khong bi troi
    void stop()
    {
      writeDuty(0);
    }

    // Servo 180: quay toi goc 0..180
    void writeAngle(float angle)
    {
      angle = constrain(angle, 0.0f, 180.0f);
      uint32_t us = SERVO_MIN_US + (uint32_t)((SERVO_MAX_US - SERVO_MIN_US) * angle / 180.0f);
      writeDuty((us * ((1UL << SERVO_RES_BITS) - 1)) / 20000UL);
    }

    // Servo 360: dir = +1 / -1, speedUs = do lech so voi 1500 us (100..600)
    void run(int dir, int speedUs)
    {
      int us = SERVO_STOP_US + (dir > 0 ? speedUs : -speedUs);
      uint32_t duty = ((uint32_t)us * ((1UL << SERVO_RES_BITS) - 1)) / 20000UL;
      writeDuty(duty);
    }
};

// ================================================================================
// A9. CỔNG – kiểu Gate (gates[0] = cổng VÀO, gates[1] = cổng RA)
// ================================================================================

// Gate: toàn bộ thông tin 1 cổng – servo, góc đã học, chuyển động, cảm biến E18
struct Gate
{
  const char*   name;
  ContServo     sv;
  int           mode;        // 180 = servo thuong (theo goc) | 360 = servo quay lien tuc
  bool          outputOn;    // servo 180: da xuat xung chua (chua hoc -> khong xuat)
  unsigned long lastStepMs;
  uint8_t       servoPin;
  uint8_t       irPin;

  // --- Hieu chinh toc do (thoi gian quay 90 do moi chieu) ---
  unsigned long ms90R;       // quay PHAI (+) 90 do mat bao nhieu ms
  unsigned long ms90L;       // quay TRAI (-) 90 do mat bao nhieu ms
  int           speedUs;     // toc do quay

  // --- Goc da hoc (goc uoc tinh, 0 = vi tri dat goc) ---
  bool          zeroSet;     // da dat goc 0 (Buoc 1)
  bool          openSet;
  bool          closeSet;
  float         openAng;
  float         closeAng;
  bool          learned;     // du dieu kien hoat dong tu dong

  // --- Chuyen dong ---
  float         ang;         // goc hien tai (uoc tinh)
  float         target;      // goc can toi
  int           motion;      // -1 quay trai | 0 dung | +1 quay phai
  unsigned long lastMotionMs;
  bool          savedOpen;

  // --- Dang do 90 do ---
  int           calDir;      // 0 = khong do | +1 / -1 dang do
  unsigned long calStart;
  float         calStartAng;
  unsigned long nudgeUntil;  // nhich theo thoi gian (khi chua do toc do)

  // --- Cam bien ---
  bool          sensorEn;
  bool          sensorInv;
  unsigned long confirmMs;
  unsigned long closeDelay;
  bool          rawPrev;
  bool          stable;
  unsigned long changeMs;
  bool          detPrev;
  unsigned long lastDetect;

  bool          manual;
};

// gates[0] = cổng VÀO, gates[1] = cổng RA
Gate gates[2];

// ================================================================================
// A10. HẸN GIỜ – lưu thời điểm chạy lần cuối của từng việc (millis)
// ================================================================================

unsigned long previousSensorMillis = 0;
unsigned long previousLCDMillis    = 0;
unsigned long previousSerialMillis = 0;
unsigned long previousLEDMillis    = 0;

const unsigned long SENSOR_INTERVAL = 70;
const unsigned long CAL_TIMEOUT     = 20000;  // do 90 do qua 20 s -> tu dung

// ================================================================================
// A11. TRANG WEB – tách riêng ra file web_page.h (HTML + JavaScript)
//   Trình duyệt tự gọi /api/status mỗi 0,5 s để cập nhật; nút bấm gọi /api/gate, /api/settings
// ================================================================================


// ===================== (gộp từ web_page.h) =====================
/*
 ================================================================================
   web_page.h – GIAO DIỆN WEB của bãi đỗ xe (HTML + CSS + JavaScript)
 ================================================================================
   File này CHỈ chứa trang CÀI ĐẶT & HỌC CỔNG, được baixe.ino #include.
   ESP32 gửi chuỗi WEB_PAGE khi mở http://192.168.4.1/caidat
   (trang chính http://192.168.4.1 là giao diện 3D trong web3d.h)

   Trang web gọi các API trong baixe.ino:
     GET /api/status    (mỗi 0,5 s)  → B9  handleStatusAPI   : trạng thái 4 ô + 2 cổng
     GET /api/gate?...  (nút bấm)    → B10 handleGateAPI     : mở / đóng / học cổng
     GET /api/settings?...           → B11 handleSettingsAPI : lưu cài đặt

   Sửa giao diện: chỉ sửa trong file này, nằm giữa R"rawliteral(  và  )rawliteral"
 ================================================================================
*/

#include <Arduino.h>

const char WEB_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cài đặt – Bãi đỗ xe thông minh</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:Inter,'Segoe UI',system-ui,Arial,sans-serif;background:#f2f5f3;color:#14211b;-webkit-font-smoothing:antialiased}
.header{background:#fff;color:#14211b;padding:18px 24px;border-bottom:1px solid #e2e8e4}
.header h1{margin:0;font-size:22px;font-weight:750;letter-spacing:-.01em}
.header p{margin:4px 0 0;color:#6a7a71;font-size:13px}
.container{max-width:1100px;margin:auto;padding:18px}
.card{background:#fff;border:1px solid #e2e8e4;border-radius:18px;padding:18px;box-shadow:0 1px 2px rgba(20,33,27,.05),0 6px 20px rgba(20,33,27,.05)}
.summary{text-align:center;margin-bottom:18px}
.dot{display:inline-block;width:10px;height:10px;background:#22c55e;border-radius:50%;margin-right:6px}
.dot.off{background:#ef4444}
.freeNumber{font-size:44px;font-weight:800;color:#1d7a4c;margin:6px 0}
.small{font-size:13px;color:#6a7a71}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}
.slot{border-top:9px solid #94a3b8}
.slot h2{margin:0 0 6px;font-size:20px}
.distance{font-size:30px;font-weight:bold;margin:10px 0}
.state{padding:11px 6px;border-radius:10px;font-weight:bold;text-align:center}
.raw{font-size:12px;color:#64748b;margin-top:8px;text-align:center;min-height:15px}
.empty{border-color:#22c55e}.empty .state{background:#dcfce7;color:#166534}
.good{border-color:#2563eb}.good .state{background:#dbeafe;color:#1e40af}
.warning{border-color:#eab308}.warning .state{background:#fef9c3;color:#854d0e}
.close{border-color:#ef4444}.close .state{background:#fee2e2;color:#991b1b}
.error{border-color:#7c3aed}.error .state{background:#ede9fe;color:#5b21b6;font-size:14px}
.gates{margin-top:18px;display:grid;grid-template-columns:1fr 1fr;gap:18px}
.sensor{margin-top:10px;padding:11px;border-radius:10px;font-weight:bold}
.car{background:#fee2e2;color:#991b1b}
.noCar{background:#dcfce7;color:#166534}
.gstate{margin-top:12px;font-size:19px;font-weight:bold}
.badge{display:inline-block;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:bold;margin-left:6px;vertical-align:middle}
.b-ok{background:#dcfce7;color:#166534}
.b-no{background:#fee2e2;color:#991b1b}
.b-man{background:#fef9c3;color:#854d0e}
.b-auto{background:#e0e7ff;color:#3730a3}
.learn{margin-top:14px;padding-top:14px;border-top:1px dashed #cbd5e1}
.learn h3{margin:0 0 8px;font-size:16px}
.step{font-size:14px;margin:8px 0;color:#334155}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.btns button{flex:1 1 45%;padding:13px 8px;border:none;border-radius:10px;font-weight:bold;font-size:15px;cursor:pointer;color:#fff;background:#1d7a4c;touch-action:none;user-select:none;-webkit-user-select:none}
.btns button.jog{background:#475569}
.btns button.red{background:#dc2626}
.btns button.green{background:#16a34a}
.btns button.teal{background:#0f766e}
.btns button.gray{background:#94a3b8}
.btns button.big{flex:1 1 100%;font-size:17px;padding:16px}
.btns button:disabled{opacity:.45}
.slider{display:flex;align-items:center;gap:10px;margin:8px 0}
.slider input{flex:1;height:30px}
.slider b{min-width:52px;text-align:right;font-size:18px}
.row select{width:105px;padding:8px;border:1px solid #cbd5e1;border-radius:8px;font-size:15px}
.info{font-size:13px;color:#475569;background:#f8fafc;border-radius:10px;padding:10px;margin-top:8px}
.settings{margin-top:18px}
.settings h2{margin-top:0}
.setgrid{display:grid;grid-template-columns:1fr 1fr;gap:18px}
.setbox{background:#f8fafc;border-radius:12px;padding:14px}
.setbox h3{margin:0 0 8px}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px;margin:9px 0;font-size:14px}
.row input[type=number]{width:105px;padding:8px;border:1px solid #cbd5e1;border-radius:8px;font-size:15px}
.row input[type=checkbox]{width:22px;height:22px}
.save{width:100%;margin-top:12px;padding:14px;border:none;border-radius:12px;background:#0f766e;color:#fff;font-size:17px;font-weight:bold;cursor:pointer}
.note{font-size:13px;color:#64748b;background:#fff7ed;border-radius:10px;padding:10px;margin-top:12px;line-height:1.5}
.legend{margin-top:18px}
.legend p{margin:7px 0}
.footer{text-align:center;margin-top:18px;color:#64748b;font-size:13px}
@media(max-width:850px){.grid{grid-template-columns:repeat(2,1fr)}}
@media(max-width:620px){.grid,.gates,.setgrid{grid-template-columns:1fr}}
</style>
</head>
<body>

<div class="header">
  <h1>Bãi đỗ xe thông minh</h1>
  <p>Cài đặt và học cổng · <a href="/" style="color:#1d7a4c;font-weight:700">← Về trang chính</a></p>
</div>

<div class="container">

<div class="card summary">
  <div><span class="dot" id="dot"></span><span id="conn">Đang kết nối...</span></div>
  <div style="margin-top:10px">SỐ CHỖ CÒN TRỐNG</div>
  <div class="freeNumber" id="freeSlots">-- / 4</div>
  <div class="small">📟 LCD: <b id="lcdState">--</b> • 📷 Camera AI: <b id="aiState">--</b></div>
</div>

<div class="grid" id="slots"></div>

<div class="card" style="margin-top:18px">
  <h2 style="margin-top:0">📋 Nhật ký xe vào / ra chỗ đỗ</h2>
  <div id="evlog" class="small">Chưa có sự kiện</div>
</div>

<div class="gates" id="gates"></div>

<div class="card settings">
  <h2>⚙️ Cài đặt</h2>
  <div class="row"><span><b>Xác định chỗ đỗ bằng</b></span>
    <select id="src" style="width:auto;padding:8px;border:1px solid #cbd5e1;border-radius:8px;font-size:15px">
      <option value="2">Kết hợp camera AI + HC-SR04</option>
      <option value="1">Chỉ camera AI</option>
      <option value="0">Chỉ HC-SR04</option>
    </select></div>
  <div class="small" style="margin-bottom:10px">Mất kết nối camera AI quá 3 giây thì hệ thống tự dùng HC-SR04.</div>
  <div class="setgrid" id="setgrid"></div>
  <button class="save" onclick="saveSettings()">💾 LƯU CÀI ĐẶT VÀO ESP32</button>
  <div class="note">
    ℹ️ Khoảng cách phát hiện của E18-D80NK chỉ chỉnh bằng <b>vít biến trở</b> trên thân cảm biến.<br>
    <b>Thời gian xác nhận</b>: cảm biến phải thấy xe liên tục bao lâu mới tính (tăng nếu hay báo nhầm).<br>
    <b>Chờ đóng</b>: xe rời cảm biến bao lâu thì hạ thanh chắn.<br>
    <b>Tốc độ servo</b>: đổi tốc độ thì phải <b>đo lại 90°</b> (Bước 2, 3) cổng đó.
  </div>
</div>

<div class="card legend">
  <strong>Trạng thái chỗ đỗ &amp; màu đèn</strong>
  <p>🟢 Trống (&gt;20 cm hoặc không có vật) – đèn XANH LÁ</p>
  <p>🟡 10–20 cm: Xe cần tiến thêm – đèn VÀNG</p>
  <p>🔵 5–10 cm: Đỗ tốt – đèn XANH DƯƠNG</p>
  <p>🔴 &lt;5 cm: Quá gần tường – đèn ĐỎ</p>
  <p>🔵 Camera AI thấy xe nhưng HC-SR04 chưa đo được – đèn XANH DƯƠNG</p>
  <p>🟣 Tím nhấp nháy: lỗi cảm biến (ECHO kẹt mức cao)</p>
</div>

<div class="footer">Cập nhật mỗi 1 giây • ESP32-S3 Smart Parking</div>
</div>

<script>
const GN=["Cổng vào","Cổng ra"];
let busy=false, failCount=0, settingsInit=false;

/* ---------- Dung khung trang ---------- */
(function build(){
  let h="";
  for(let i=0;i<4;i++){
    h+=`<div class="card slot empty" id="slot${i}"><h2>🚙 Vị trí ${i+1}</h2>
      <div class="distance"><span id="dist${i}">--</span> cm</div>
      <div class="state" id="st${i}">Đang đọc...</div>
      <div class="raw" id="raw${i}"></div>
      <div class="raw" id="ai${i}"></div></div>`;
  }
  document.getElementById("slots").innerHTML=h;

  h="";
  for(let g=0;g<2;g++){
    h+=`<div class="card">
      <h2>🚧 ${GN[g]} <span class="badge" id="lb${g}">--</span><span class="badge" id="mb${g}">--</span></h2>
      <div class="sensor noCar" id="sen${g}">Không có xe</div>
      <div class="gstate">Thanh chắn: <span id="gs${g}">--</span></div>
      <div class="small" id="ang${g}">Góc ước tính: --</div>
      <div class="learn">
        <h3 id="lt${g}">🎓 Học cổng</h3>

        <div class="step" id="hint${g}"></div>
        <div class="row"><span>Mỗi lần nhích</span>
          <select id="stp${g}"><option value="2">2°</option><option value="5" selected>5°</option><option value="10">10°</option><option value="30">30°</option></select></div>
        <div class="btns">
          <button class="jog" onclick="nudge(${g},-1)">⟲ Nhích trái</button>
          <button class="jog" onclick="nudge(${g},1)">Nhích phải ⟳</button>
          <button class="teal big" id="zb${g}" onclick="gate(${g},'zero')">📍 Đặt vị trí này = ĐÓNG (0°)</button>
        </div>

        <div class="step" id="calTxt${g}"></div>
        <div class="btns" id="calBtns${g}"></div>
        <div class="info" id="calInfo${g}">--</div>

        <div class="step" id="slTxt${g}"><b>Bước 4 (tinh chỉnh):</b> kéo thanh trượt tới góc mong muốn rồi bấm Lưu.</div>
        <div class="slider">
          <input type="range" min="-180" max="180" step="5" value="0" id="sl${g}"
            oninput="document.getElementById('slv${g}').innerText=this.value+'°'"
            onchange="gate(${g},'goto',undefined,'&angle='+this.value)">
          <b id="slv${g}">0°</b>
        </div>
        <div class="btns">
          <button class="green" onclick="gate(${g},'saveopen')">💾 Lưu góc này = MỞ</button>
          <button class="red" onclick="gate(${g},'saveclose')">💾 Lưu góc này = ĐÓNG</button>
        </div>
        <div class="info" id="linfo${g}">--</div>

        <div class="btns">
          <button class="green" onclick="gate(${g},'open')">⬆ MỞ</button>
          <button class="red" onclick="gate(${g},'close')">⬇ ĐÓNG</button>
          <button class="teal" onclick="gate(${g},'auto')">🔄 TỰ ĐỘNG</button>
          <button class="gray" onclick="if(confirm('Xóa dữ liệu đã học của ${GN[g]}?'))gate(${g},'forget')">🗑 Xóa học</button>
        </div>
      </div></div>`;
  }
  document.getElementById("gates").innerHTML=h;

  h="";
  for(let g=0;g<2;g++){
    h+=`<div class="setbox"><h3>🚧 ${GN[g]}</h3>
      <div class="row"><span>Bật cảm biến</span><input type="checkbox" id="en${g}"></div>
      <div class="row"><span>Đảo tín hiệu cảm biến</span><input type="checkbox" id="inv${g}"></div>
      <div class="row"><span>Thời gian xác nhận xe (ms)</span><input type="number" id="cf${g}" min="20" max="5000" step="10"></div>
      <div class="row"><span>Chờ đóng sau khi xe qua (ms)</span><input type="number" id="cd${g}" min="200" max="20000" step="100"></div>
      <div class="row"><span><b>Loại servo</b></span><select id="mode${g}"><option value="180">180° (thường)</option><option value="360">360° (quay liên tục)</option></select></div>
      <div class="row"><span>Tốc độ servo 360 (100–600)</span><input type="number" id="spd${g}" min="100" max="600" step="10"></div>
    </div>`;
  }
  document.getElementById("setgrid").innerHTML=h;
})();

/* ---------- Gui lenh ---------- */
async function api(url){
  try{ let r=await fetch(url,{cache:"no-store"}); return await r.text(); }
  catch(e){ return "Không gửi được lệnh tới ESP32"; }
}
async function gate(g,cmd,dir,extra){
  let t=await api(`/api/gate?g=${g}&cmd=${cmd}`+(dir!==undefined?`&dir=${dir}`:"")+(extra||""));
  if(t && t.startsWith("!")) alert(t.substring(1));
  else if(cmd==="saveopen"||cmd==="saveclose") alert(t);
  updateData();
}
function nudge(g,dir){
  gate(g,'nudge',dir,'&step='+document.getElementById('stp'+g).value);
}
async function saveSettings(){
  let q=[];
  for(let g=0;g<2;g++){
    q.push(`en${g}=`+(document.getElementById("en"+g).checked?1:0));
    q.push(`inv${g}=`+(document.getElementById("inv"+g).checked?1:0));
    for(const k of ["cf","cd","spd","mode"]) q.push(`${k}${g}=`+document.getElementById(k+g).value);
  }
  q.push("src="+document.getElementById("src").value);
  alert(await api("/api/settings?"+q.join("&")));
  settingsInit=false; updateData();
}

/* ---------- Hien thi ---------- */
function showSlot(i,d){
  let card=document.getElementById("slot"+i), st=document.getElementById("st"+i), raw=document.getElementById("raw"+i);
  card.className="card slot";
  document.getElementById("dist"+i).innerText = d.distance>=900 ? "--" : Number(d.distance).toFixed(1);
  let pins = `TRIG ${d.tp} • ECHO ${d.ep}`;
  raw.innerText = d.us>0 ? `Xung ECHO: ${d.us} µs • ${pins}` : (d.err===1 ? (d.lk ? `Không có vật cản phía trước • ${pins}` : `Chưa nhận tín hiệu – đang dò TRIG/ECHO • ${pins}`) : pins);
  let ai=document.getElementById("ai"+i);
  ai.innerHTML = d.ai<0 ? "📷 AI: --" : `📷 AI: <b>${d.ai?"CÓ XE":"trống"}</b>` + (d.mm ? ' • <b style="color:#dc2626">⚠ khác HC-SR04</b>' : "");
  if((d.err===2||d.err===4) && d.fs!=="AI"){card.classList.add("error");st.innerText=d.err===4?"⚠ MẤT TIẾNG VANG QUÁ 3 GIÂY":"⚠ ECHO KẸT MỨC CAO";raw.innerText="Kiểm tra chạm dây / mạch chia áp";return;}
  if(d.fs==="AI"){card.classList.add("good");st.innerText="CÓ XE (camera AI)";}
  else if(d.fs==="EMPTY"){card.classList.add("empty");st.innerText="TRỐNG";}
  else if(d.err===3){card.classList.add("close");st.innerText="QUÁ GẦN (< 2 cm)";}
  else if(d.fs==="GOOD"){card.classList.add("good");st.innerText="ĐỖ XE TỐT";}
  else if(d.fs==="CLOSE"){card.classList.add("close");st.innerText="QUÁ GẦN TƯỜNG";}
  else{card.classList.add("warning");st.innerText="CẦN TIẾN THÊM";}
}

function showGate(g,d){
  let sen=document.getElementById("sen"+g);
  if(!d.en){ sen.innerText="Cảm biến đang TẮT"; sen.className="sensor"; sen.style.background="#e2e8f0"; }
  else{ sen.style.background=""; sen.innerText = d.det ? "🚗 PHÁT HIỆN XE" : "Không có xe"; sen.className = d.det ? "sensor car" : "sensor noCar"; }

  let lb=document.getElementById("lb"+g);
  lb.innerText = d.learned ? "ĐÃ HỌC" : "CHƯA HỌC"; lb.className = "badge "+(d.learned?"b-ok":"b-no");
  let mb=document.getElementById("mb"+g);
  mb.innerText = d.manual ? "THỦ CÔNG" : "TỰ ĐỘNG"; mb.className = "badge "+(d.manual?"b-man":"b-auto");

  let gs = d.calDir ? "Đang đo 90°..."
         : !d.learned ? (d.mode===180 ? "CHƯA HỌC – hãy lưu góc MỞ và ĐÓNG" : "CHƯA HỌC – không tự hoạt động")
         : d.moving ? "Đang quay..."
         : d.isOpen ? "ĐANG MỞ" : "ĐANG ĐÓNG";
  document.getElementById("gs"+g).innerText=gs;
  document.getElementById("ang"+g).innerText = d.mode===180 ? `Servo 180° • Góc: ${d.ang}°`
    : (d.cal ? `Servo 360° • Góc ước tính: ${d.ang}°` : "Servo 360° • Góc ước tính: chưa đo tốc độ");

  let m180 = d.mode===180;
  document.getElementById("lt"+g).innerText = "🎓 Học cổng (servo "+(m180?"180°":"360°")+")";
  document.getElementById("hint"+g).innerHTML = m180
    ? "<b>Servo 180°:</b> nhích hoặc kéo thanh trượt tới khi thanh chắn <b>NẰM NGANG</b> → bấm <b>Lưu = ĐÓNG</b>; tới khi <b>DỰNG ĐỨNG</b> → bấm <b>Lưu = MỞ</b>."
    : "<b>Bước 1:</b> Nhích trái/phải cho thanh chắn <b>NẰM NGANG (ĐÓNG)</b>, rồi bấm Đặt 0°.";
  document.getElementById("zb"+g).style.display = m180 ? "none" : "";
  document.getElementById("calInfo"+g).style.display = m180 ? "none" : "";
  document.getElementById("slTxt"+g).innerHTML = m180
    ? "<b>Thanh trượt góc 0–180°:</b> kéo tới đâu servo quay tới đó."
    : "<b>Bước 4 (tinh chỉnh):</b> kéo thanh trượt tới góc mong muốn rồi bấm Lưu.";

  /* Buoc 2 - 3: do 90 do */
  let txt="", btn="";
  if(m180){
    txt="";
  }else if(d.calDir){
    txt=`⏱ Servo đang quay ${d.calDir>0?"PHẢI ⟳":"TRÁI ⟲"}. Bấm DỪNG đúng lúc thanh chắn quay được <b>90°</b>.`;
    btn=`<button class="red big" onclick="gate(${g},'calstop')">⏹ DỪNG – ĐÃ QUAY ĐỦ 90°</button>`;
  }else if(!d.zero){
    txt="<b>Bước 2:</b> làm Bước 1 trước (đặt vị trí ĐÓNG = 0°).";
  }else if(!d.m90R && !d.m90L){
    txt="<b>Bước 2:</b> Thanh chắn đang nằm ngang. Chọn chiều quay để <b>MỞ (dựng đứng)</b>:";
    btn=`<button onclick="gate(${g},'calstart',-1)">▶ Quay TRÁI lên 90° ⟲</button>
         <button onclick="gate(${g},'calstart',1)">▶ Quay PHẢI lên 90° ⟳</button>`;
  }else if(!d.m90R || !d.m90L){
    let back = d.m90R ? -1 : 1;
    txt="<b>Bước 3:</b> Thanh chắn đang dựng đứng. Bấm để quay ngược về <b>nằm ngang</b>:";
    btn=`<button class="teal big" onclick="gate(${g},'calstart',${back})">▶ Quay ${back>0?"PHẢI ⟳":"TRÁI ⟲"} về 90°</button>`;
  }else{
    txt="✅ Bước 2 và 3 đã xong. Có thể đo lại nếu thanh chắn quay lệch:";
    btn=`<button class="gray" onclick="gate(${g},'calstart',-1)">↻ Đo lại 90° quay trái</button>
         <button class="gray" onclick="gate(${g},'calstart',1)">↻ Đo lại 90° quay phải</button>`;
  }
  document.getElementById("calTxt"+g).innerHTML=txt;
  let cb=document.getElementById("calBtns"+g);
  if(cb.dataset.k!==txt){ cb.innerHTML=btn; cb.dataset.k=txt; }
  document.getElementById("calInfo"+g).innerText =
    `Tốc độ đo được: quay phải 90° = ${d.m90R||"--"} ms • quay trái 90° = ${d.m90L||"--"} ms`;

  /* Thanh truot */
  let sl=document.getElementById("sl"+g);
  sl.min = m180 ? 0 : -180;
  sl.max = 180;
  sl.disabled = !d.cal;
  if(document.activeElement!==sl){ sl.value=d.target; document.getElementById("slv"+g).innerText=d.target+"°"; }

  document.getElementById("linfo"+g).innerText = d.learned
    ? `Đã học: MỞ = ${d.oa}° • ĐÓNG = ${d.ca}°`
    : "Chưa học xong: cổng KHÔNG tự hoạt động (kể cả khi khởi động).";
}

function fillSettings(gs){
  for(let g=0;g<2;g++){
    document.getElementById("en"+g).checked = !!gs[g].en;
    document.getElementById("inv"+g).checked = !!gs[g].inv;
    document.getElementById("cf"+g).value = gs[g].cf;
    document.getElementById("cd"+g).value = gs[g].cd;
    document.getElementById("spd"+g).value = gs[g].spd;
    document.getElementById("mode"+g).value = gs[g].mode;
  }
}

async function updateData(){
  if(busy) return;
  busy=true;
  try{
    let ctl=new AbortController(); let to=setTimeout(()=>ctl.abort(),3000);
    let r=await fetch("/api/status",{cache:"no-store",signal:ctl.signal});
    clearTimeout(to);
    let d=await r.json();
    failCount=0;
    let fs=document.getElementById("freeSlots");          // hết chỗ: hiện BÃI ĐẦY màu đỏ
    fs.innerText = d.free==0 ? "BÃI ĐẦY" : d.free+" / 4";
    fs.style.color = d.free==0 ? "#dc2626" : "#1d7a4c";
    document.getElementById("lcdState").innerText=d.lcd;
    for(let i=0;i<4;i++) showSlot(i,d.slots[i]);
    for(let g=0;g<2;g++) showGate(g,d.gates[g]);
    if(!settingsInit){ fillSettings(d.gates); document.getElementById("src").value=d.src; settingsInit=true; }
    document.getElementById("aiState").innerText = d.aiOn ? "ĐANG KẾT NỐI" : "CHƯA KẾT NỐI";
    document.getElementById("aiState").style.color = d.aiOn ? "#16a34a" : "#dc2626";
    let ev = d.ev.map(e=>`<div>${e.in?"🚗➡️":"⬅️🚗"} <b>Xe ${e.in?"VÀO":"RỜI"} ô ${e.s}</b> • ${e.ago<60?e.ago+" giây":Math.floor(e.ago/60)+" phút"} trước • theo ${["HC-SR04","camera AI","AI + HC-SR04"][e.by]}</div>`).join("");
    document.getElementById("evlog").innerHTML = ev || "Chưa có sự kiện";
    document.getElementById("dot").className="dot";
    document.getElementById("conn").innerText="Hệ thống đang hoạt động";
  }catch(e){
    failCount++;
    if(failCount>=3){
      document.getElementById("dot").className="dot off";
      document.getElementById("conn").innerText="Mất kết nối ESP32 – kiểm tra WiFi BAI_XE_ESP32S3";
    }
  }
  busy=false;
}
updateData();
setInterval(updateData,1000);
</script>
</body>
</html>
)rawliteral";

// ===================== (hết web_page.h) =====================


// ===================== (gộp từ web3d.h) =====================
/*
 ================================================================================
   web3d.h – GIAO DIỆN 3D (Three.js) ĐÃ NÉN GZIP – FILE TỰ SINH, ĐỪNG SỬA TAY
 ================================================================================
   Sinh bởi giao_dien_3d/build_web3d.py lúc 13/10/2026 17:19
     WEB3D_INDEX_GZ : trang chính  "/"        (16903 byte → nén 4819 byte)
     WEB3D_APP_GZ   : "/app.js" (3D + Three.js) (636851 byte → nén 163779 byte)
   Muốn sửa giao diện: sửa trong thư mục giao_dien_3d/ rồi chạy lại build_web3d.py
 ================================================================================
*/

#include <Arduino.h>

const uint8_t WEB3D_INDEX_GZ[] PROGMEM = {
  31,139,8,0,0,0,0,0,2,255,197,60,107,139,236,200,117,223,253,43,42,26,150,157,
  9,45,141,30,221,61,51,234,153,78,188,247,230,131,97,119,189,246,222,4,131,241,135,106,
  169,212,45,143,90,106,75,234,121,184,105,200,131,216,96,98,226,37,36,198,95,194,110,22,
  178,144,196,16,179,75,2,247,18,242,97,150,252,143,217,95,146,115,170,74,82,233,213,221,
  115,51,38,92,102,70,143,170,83,167,206,251,156,58,186,151,127,224,39,94,126,191,98,100,
  145,47,163,233,183,46,241,15,137,104,60,191,210,110,66,109,122,185,96,212,159,94,46,89,
  78,137,183,160,105,198,242,43,109,157,7,250,185,38,159,198,116,201,112,44,187,93,37,105,
  174,17,47,137,115,22,195,168,219,208,207,23,87,62,187,9,61,166,243,155,65,24,135,121,
  72,35,61,243,104,196,174,172,58,8,47,137,146,20,94,45,216,146,41,96,162,112,190,200,
  137,79,211,107,24,158,135,121,196,166,239,61,124,30,146,175,63,121,124,243,107,114,199,72,
  190,120,248,42,158,147,101,24,47,46,79,197,128,203,44,191,135,63,110,154,36,249,70,215,
  103,115,247,40,176,131,81,224,76,116,221,163,169,15,183,65,0,215,97,124,237,30,89,67,
  219,178,102,112,183,92,231,12,94,141,233,25,61,179,224,62,10,99,230,30,49,155,157,179,
  33,220,102,73,144,195,196,81,112,30,140,225,150,122,30,224,7,211,253,51,58,244,202,7,
  2,166,0,47,159,136,137,204,14,28,118,1,79,83,92,132,141,134,231,206,76,220,21,144,
  125,230,49,124,68,151,51,150,186,71,254,197,197,200,162,112,63,139,214,128,135,67,207,102,
  254,8,241,88,80,63,185,117,77,98,173,238,136,13,63,233,124,70,143,109,115,224,56,3,
  251,108,96,152,163,147,129,73,198,248,210,236,124,139,48,0,49,166,83,192,131,5,54,11,
  202,39,51,88,54,96,103,204,222,254,241,146,249,33,37,199,171,148,5,44,205,116,149,59,
  46,114,227,100,163,146,215,244,173,161,101,149,228,181,70,86,192,113,231,212,96,23,129,197,
  252,138,194,231,62,181,46,206,74,10,219,67,199,177,103,37,133,45,106,143,173,64,161,176,
  195,188,224,156,53,40,108,142,173,145,233,55,137,108,157,57,35,155,22,68,14,204,177,57,
  162,53,34,59,212,242,45,133,200,236,130,50,167,34,242,152,82,39,176,123,137,108,14,240,
  159,49,68,2,159,227,139,97,227,141,83,167,46,110,197,54,85,234,90,150,69,173,209,118,
  251,135,155,89,114,167,103,225,79,195,120,238,206,146,212,103,169,14,79,182,168,126,27,253,
  150,205,174,195,92,207,217,93,142,99,0,150,255,227,117,150,187,150,105,190,179,157,37,254,
  253,102,73,211,121,24,187,230,100,70,189,235,121,154,172,99,223,189,161,233,49,114,227,100,
  194,153,37,239,129,90,39,147,0,180,201,181,0,219,83,203,24,142,200,119,64,183,210,193,
  187,31,179,121,194,200,159,126,231,221,65,118,159,229,108,169,175,195,129,78,87,171,136,233,
  226,193,224,251,201,44,201,147,193,183,83,208,218,65,70,227,76,207,88,26,2,115,36,134,
  8,87,207,150,32,7,11,220,8,141,81,189,67,154,49,127,59,91,231,121,18,15,50,22,
  49,47,7,205,95,173,243,13,71,3,244,20,96,228,18,73,121,87,31,190,241,214,105,6,
  47,87,73,136,152,202,151,174,31,102,116,22,49,127,147,172,168,23,230,247,46,236,101,34,
  135,198,73,174,211,40,74,110,203,165,221,32,241,214,153,126,19,102,33,76,146,144,27,15,
  57,90,141,103,180,126,191,73,214,57,151,83,148,130,44,137,66,159,8,194,10,193,59,153,
  200,247,122,18,4,96,29,113,216,150,110,106,155,155,112,70,130,122,39,41,205,67,192,44,
  78,98,182,53,184,54,108,84,94,241,39,39,91,35,165,97,180,89,37,89,200,71,7,225,
  29,104,79,24,35,116,147,208,117,158,16,147,152,19,110,82,221,49,48,181,45,4,168,133,
  39,19,41,87,41,90,80,215,106,160,143,56,159,76,128,164,171,136,222,187,65,196,238,38,
  248,75,247,195,20,232,132,235,2,102,235,101,60,1,134,206,99,61,4,113,200,92,220,49,
  75,39,115,186,2,89,132,133,87,212,247,129,241,122,158,192,3,208,136,201,79,65,222,124,
  118,231,142,182,198,44,165,177,191,17,88,14,113,240,130,113,68,248,117,129,26,245,195,117,
  230,90,118,215,38,10,2,171,20,170,76,64,133,251,60,13,253,9,92,129,155,169,97,201,
  133,243,86,44,122,110,154,226,30,213,201,69,195,56,17,42,4,90,7,194,178,228,40,8,
  194,207,242,184,192,218,86,176,182,75,172,81,235,118,162,143,236,157,180,249,218,68,32,7,
  2,73,22,27,214,40,43,87,119,23,201,13,75,55,45,122,160,5,59,41,71,25,20,184,
  116,195,54,61,100,19,163,59,104,119,178,93,210,48,150,6,68,143,24,88,69,46,67,146,
  149,174,141,114,238,224,175,161,160,210,157,112,222,174,53,198,7,91,3,35,130,77,77,110,
  208,54,133,193,189,46,125,182,155,173,144,23,51,150,223,50,214,47,63,227,54,15,80,130,
  184,16,222,166,48,2,127,109,23,86,101,235,20,250,225,100,149,191,103,35,115,18,177,28,
  128,235,184,58,110,68,55,76,155,45,5,190,194,34,100,117,180,119,9,118,3,11,35,11,
  151,229,228,48,230,250,190,11,198,185,66,208,51,32,165,53,108,201,252,197,197,69,191,208,
  247,115,175,182,235,241,72,37,138,229,32,123,0,83,18,74,1,62,175,228,247,188,133,192,
  200,124,167,95,231,184,119,42,124,32,254,3,216,68,196,0,203,240,238,56,140,73,6,126,
  111,80,155,68,236,209,59,3,46,212,43,154,114,73,51,208,14,109,118,90,167,30,187,212,
  80,48,129,125,137,146,212,7,126,3,171,160,198,30,200,156,166,168,42,60,27,43,60,187,
  64,158,141,43,141,239,65,114,183,217,149,184,91,13,81,29,155,77,166,181,45,193,126,43,
  128,22,96,149,134,160,27,247,155,29,92,228,152,116,137,81,143,81,173,1,150,40,4,97,
  4,52,114,103,220,143,196,44,203,142,45,195,60,147,67,179,159,172,129,217,170,153,87,105,
  104,138,65,240,182,24,130,33,76,161,246,220,107,112,155,194,33,45,55,197,84,12,94,133,
  26,86,116,178,229,184,21,13,211,77,205,244,227,47,136,149,150,240,36,103,186,240,90,64,
  247,32,37,242,167,84,73,105,73,16,22,10,245,214,64,199,140,210,83,25,246,167,88,113,
  110,247,11,156,49,20,28,183,148,236,28,177,150,171,236,228,104,43,102,3,77,206,105,158,
  29,178,213,148,173,24,205,143,49,50,208,131,48,31,64,42,4,102,251,216,70,18,14,96,
  255,39,39,194,176,13,59,45,174,88,232,57,180,84,213,32,139,243,112,151,222,78,0,77,
  93,90,39,203,28,22,136,16,227,186,220,243,44,74,188,235,3,120,208,84,175,250,38,199,
  37,232,44,79,147,120,222,0,95,193,114,144,64,220,124,20,104,25,86,221,203,140,123,189,
  140,2,159,100,75,136,68,55,10,142,35,128,219,183,139,10,237,98,255,124,186,68,85,1,
  114,209,116,121,102,11,25,9,98,42,16,216,75,69,69,13,135,109,101,59,98,217,202,135,
  4,175,97,89,5,52,161,204,23,149,127,185,56,204,191,212,87,22,209,7,204,4,189,200,
  67,40,10,232,220,100,187,203,208,247,35,134,238,67,4,250,117,191,141,178,60,170,68,185,
  178,34,197,112,240,127,56,208,181,10,228,218,122,57,220,21,112,150,233,197,133,10,211,88,
  67,90,211,86,147,20,35,246,163,200,243,65,156,195,34,86,25,10,3,83,247,39,86,59,
  6,168,99,113,100,5,14,133,76,88,176,234,200,59,15,70,190,35,114,55,16,15,130,174,
  2,242,183,17,121,1,161,76,18,209,108,240,238,139,100,157,134,44,37,31,178,219,119,7,
  203,36,78,120,232,213,20,11,195,28,163,136,230,116,150,117,250,73,36,232,176,110,202,38,
  207,96,14,156,182,185,225,170,8,120,236,180,185,42,209,236,142,100,193,108,106,60,68,65,
  29,210,221,114,170,98,93,105,133,219,230,86,188,54,146,120,243,212,76,100,107,220,38,233,
  53,39,253,33,230,90,154,103,147,155,102,226,140,113,63,220,62,163,181,84,99,22,208,229,
  52,223,26,144,136,174,105,84,58,71,75,152,74,172,40,60,87,40,222,157,14,181,131,96,
  54,95,226,44,191,83,138,10,252,156,46,233,17,62,174,35,50,218,37,80,202,138,68,68,
  240,251,133,166,39,218,62,223,31,132,117,37,226,205,245,13,81,69,96,189,65,109,171,242,
  82,11,164,113,151,78,163,100,100,217,92,246,230,135,70,176,197,70,71,61,38,165,59,173,
  80,233,223,163,189,79,118,179,219,163,162,234,91,213,42,82,6,162,14,105,105,153,54,143,
  59,20,24,217,131,74,24,64,185,198,93,128,173,7,209,84,48,70,116,104,170,207,113,56,
  236,249,216,26,155,62,155,203,92,67,214,214,78,106,183,179,147,147,10,25,226,209,248,134,
  102,13,247,167,68,159,101,204,1,197,180,163,40,161,72,206,106,7,96,36,65,79,115,86,
  20,92,246,150,25,246,59,246,178,42,98,171,219,44,42,95,6,34,158,39,73,148,117,32,
  193,157,27,210,94,22,113,240,242,128,154,77,145,207,20,11,59,202,42,133,42,9,138,56,
  227,202,135,59,251,51,158,182,37,62,84,17,26,218,214,52,224,253,217,93,19,241,61,137,
  81,115,248,78,131,254,22,9,210,17,2,207,195,85,7,179,10,198,8,131,36,106,151,58,
  187,129,169,153,120,86,176,99,56,105,101,58,109,115,165,6,6,38,84,211,173,97,17,24,
  224,233,194,158,8,248,118,1,50,202,3,0,6,75,151,53,12,196,125,70,211,103,246,25,
  165,144,118,84,77,228,138,83,63,188,105,71,113,231,178,96,33,7,193,185,211,140,69,155,
  221,134,8,139,28,88,2,55,34,48,51,164,107,83,188,186,42,235,206,70,16,178,200,47,
  125,39,22,237,45,251,249,164,188,85,124,137,146,28,223,60,37,109,27,242,56,160,155,168,
  67,21,232,134,151,145,69,132,140,145,115,149,107,217,117,167,119,216,182,134,253,126,162,21,
  58,21,40,20,186,215,171,54,213,80,89,159,28,84,15,42,223,217,175,117,59,235,97,2,
  56,158,22,238,45,228,237,150,233,221,18,102,237,79,180,224,24,141,39,125,18,157,178,224,
  102,41,117,238,14,181,222,145,18,85,132,219,151,165,182,107,159,227,34,227,229,230,196,36,
  182,42,53,189,73,169,115,104,82,90,0,226,185,37,219,236,178,60,157,145,248,214,136,216,
  156,197,141,72,181,110,43,90,5,138,66,252,247,135,37,5,120,2,204,137,159,82,164,29,
  43,115,15,100,160,163,184,95,53,31,222,26,16,45,172,64,186,147,180,138,211,121,17,184,
  112,18,32,142,222,245,189,240,231,124,97,57,1,67,120,178,176,203,34,55,176,175,89,249,
  58,107,242,219,54,235,211,229,161,89,21,226,108,141,159,172,97,57,40,195,230,89,219,234,
  142,149,74,152,201,5,134,155,153,106,74,17,33,200,36,90,13,173,135,79,174,137,118,69,
  254,23,173,106,195,65,98,212,194,176,39,151,219,225,214,183,71,62,116,28,132,16,107,53,
  125,151,156,195,239,59,119,86,59,235,226,20,67,84,120,195,193,239,39,7,27,215,214,32,
  11,167,235,36,132,215,150,90,210,177,10,163,42,97,116,186,99,140,157,153,194,147,45,164,
  88,147,59,229,183,56,146,50,124,150,121,105,184,66,85,169,215,80,56,226,123,108,128,49,
  106,213,237,224,252,17,201,159,38,81,154,220,62,23,123,106,149,237,253,2,211,12,14,42,
  132,166,104,170,6,60,218,81,209,236,146,249,234,53,153,109,154,94,73,9,8,120,126,176,
  229,231,217,63,196,126,158,43,112,225,115,246,35,181,234,46,185,208,229,118,107,36,231,81,
  89,10,134,37,145,71,246,178,58,218,242,59,231,61,213,208,186,55,82,193,181,189,208,240,
  64,47,180,2,131,183,105,84,252,170,0,200,172,199,117,187,67,183,67,36,220,110,73,212,
  24,60,225,109,152,123,139,13,116,72,64,126,74,99,30,90,67,84,95,52,67,52,159,183,
  143,190,109,251,80,45,148,38,167,149,86,215,27,35,106,129,154,157,113,159,42,155,11,4,
  174,174,75,131,156,91,72,33,235,154,54,233,206,49,249,126,177,26,139,23,82,102,148,211,
  67,107,239,241,33,79,74,246,22,59,156,147,6,202,37,162,208,88,228,93,119,213,85,170,
  192,178,54,176,216,25,7,23,36,233,210,229,87,24,96,255,224,24,209,197,176,227,247,84,
  190,22,144,141,249,14,100,249,128,180,167,90,44,222,206,218,111,177,9,9,95,67,23,75,
  90,19,118,174,67,81,50,7,2,111,90,39,44,232,36,98,22,61,135,39,170,59,160,6,
  236,167,120,32,168,171,204,55,216,59,80,72,190,105,170,69,31,60,170,106,153,112,190,67,
  100,82,127,116,63,131,214,34,28,209,182,201,227,131,108,114,185,4,201,67,200,34,122,108,
  128,72,168,202,42,123,85,87,87,244,171,128,19,214,211,78,168,14,208,44,111,246,238,112,
  205,226,2,37,8,203,67,248,14,201,61,214,97,208,0,107,222,39,149,109,179,170,131,180,
  93,77,39,71,150,105,155,208,115,215,172,17,244,23,60,139,99,14,179,179,86,81,211,211,
  81,86,213,176,76,165,39,228,194,188,185,149,123,54,178,5,184,176,2,166,213,191,61,19,
  130,176,101,196,197,234,135,162,248,247,163,58,13,141,101,187,70,209,200,22,250,188,180,221,
  115,228,176,76,32,192,96,243,157,117,235,103,60,245,232,47,102,33,34,255,47,135,31,114,
  225,222,211,15,249,254,237,142,63,188,5,212,195,222,178,61,230,252,73,237,49,7,240,99,
  183,212,163,116,221,210,48,175,167,105,135,148,70,14,88,186,43,128,91,70,88,253,249,191,
  28,227,143,218,199,248,8,246,57,123,107,26,196,232,57,176,111,135,35,141,42,126,129,150,
  235,206,24,168,62,219,29,119,20,29,141,69,79,99,225,139,71,157,212,231,164,124,134,3,
  175,134,121,144,196,228,121,244,108,179,47,175,90,70,65,24,211,162,68,41,99,158,182,216,
  214,206,104,134,109,64,29,217,75,179,190,196,155,10,192,16,134,106,15,128,51,60,56,4,
  111,180,49,72,88,111,27,127,215,14,236,37,201,32,37,105,101,209,197,243,90,185,119,79,
  147,14,143,5,228,17,180,72,247,118,156,122,137,130,197,1,126,94,224,113,80,69,89,6,
  31,114,198,1,89,150,113,147,228,172,59,176,220,83,70,172,11,203,153,218,118,9,219,224,
  241,22,194,54,18,207,235,140,29,59,114,23,17,81,242,89,64,221,183,201,189,249,92,116,
  62,155,131,83,37,57,105,70,253,167,161,9,90,26,160,111,231,210,32,132,192,181,78,117,
  107,210,123,128,56,62,244,52,121,44,83,108,132,175,68,101,130,45,202,145,223,174,105,235,
  158,19,56,93,56,117,110,49,248,181,136,126,58,187,150,121,1,170,236,32,25,21,38,177,
  56,149,108,159,67,142,27,85,207,241,83,210,216,242,59,13,165,79,215,194,88,251,100,163,
  52,37,244,42,95,173,110,89,149,42,129,3,222,182,3,52,239,0,6,200,188,63,189,22,
  179,181,122,138,205,90,39,152,122,50,178,179,93,79,57,83,46,14,37,121,215,49,180,1,
  43,21,112,180,52,219,203,83,241,149,207,229,169,248,70,9,15,133,224,3,38,154,65,179,
  33,241,32,112,207,174,52,68,20,62,27,162,197,61,239,73,215,200,2,62,106,185,210,140,
  83,141,240,196,230,74,123,133,213,18,248,192,233,241,205,23,218,244,163,203,83,10,224,120,
  133,81,5,4,225,17,17,135,26,26,124,143,148,83,29,81,189,210,208,7,226,85,9,236,
  35,0,243,73,72,188,199,215,159,199,11,109,250,205,63,124,113,121,42,160,245,64,173,129,
  3,17,40,33,125,240,248,250,63,115,50,123,124,253,187,120,14,128,126,241,143,21,160,83,
  190,79,216,47,82,30,191,219,2,26,64,35,144,4,140,119,248,241,150,181,235,91,41,120,
  123,9,118,90,157,35,251,164,97,42,86,168,138,55,112,198,166,145,208,191,210,48,72,124,
  69,1,149,203,16,48,8,167,228,85,250,248,230,119,30,160,251,248,250,191,87,192,16,152,
  164,82,27,183,38,104,125,234,209,16,54,169,77,95,60,124,202,241,129,141,169,100,46,160,
  191,135,83,148,233,211,23,139,199,215,159,221,147,229,195,87,100,5,116,253,219,120,174,208,
  0,176,151,236,103,41,208,32,19,135,224,37,218,216,63,169,193,115,101,147,248,172,177,187,
  107,190,8,208,39,135,205,124,130,240,197,54,196,185,9,96,200,145,11,82,198,180,233,16,
  22,135,55,232,68,167,228,148,192,173,184,70,89,20,163,149,181,138,214,49,65,187,242,174,
  196,27,127,31,128,220,123,143,111,126,3,20,11,31,223,252,197,154,92,47,224,239,95,197,
  117,28,203,249,85,227,160,54,253,147,143,63,114,108,253,99,135,92,134,124,125,209,205,167,
  113,190,85,232,242,41,252,189,248,226,231,21,248,58,109,250,245,175,80,29,174,129,169,57,
  137,81,152,191,249,243,127,174,182,122,32,222,47,30,223,252,61,64,185,121,248,52,1,82,
  165,116,63,206,28,143,57,88,5,4,184,206,16,143,135,47,97,232,41,17,23,21,218,135,
  226,240,193,195,167,49,89,60,252,91,188,32,239,191,120,89,96,0,223,180,241,165,100,211,
  30,160,10,86,111,120,58,36,127,230,190,36,223,119,95,126,203,114,191,75,108,248,113,224,
  103,232,126,247,242,20,102,148,139,158,74,49,131,203,152,150,235,99,115,157,214,212,111,209,
  82,70,146,66,195,225,254,74,115,124,196,235,43,137,150,243,178,207,56,136,201,234,204,37,
  216,178,15,40,36,91,80,119,120,31,74,141,113,88,211,6,192,70,81,2,174,81,34,205,
  6,37,228,225,249,84,197,93,161,93,233,40,180,182,18,137,230,51,130,166,187,169,74,69,
  7,154,86,147,250,178,101,170,162,70,167,177,44,135,139,67,97,48,187,170,217,108,17,69,
  129,1,208,121,149,181,38,98,187,38,160,73,157,170,182,180,105,66,184,212,32,193,210,117,
  140,68,213,42,30,204,123,181,129,139,146,34,136,56,191,252,12,118,90,62,146,13,70,5,
  24,31,12,38,90,96,69,0,56,184,150,64,151,125,44,90,205,72,242,128,54,83,56,218,
  216,54,142,185,133,118,160,172,116,34,47,31,254,233,158,228,15,191,5,69,0,219,241,243,
  181,54,197,39,157,19,211,4,79,120,43,175,8,184,146,187,132,130,205,250,230,103,111,58,
  103,100,49,93,65,225,39,47,231,188,255,63,255,190,38,165,231,251,124,23,91,90,142,243,
  235,95,113,30,69,96,241,67,50,127,248,210,35,49,210,8,0,253,205,95,54,88,86,145,
  92,182,227,236,50,171,101,139,136,96,75,109,3,62,91,38,170,199,33,242,219,7,196,254,
  63,72,225,125,192,95,18,16,207,95,210,78,34,172,40,180,6,215,128,136,175,34,20,66,
  190,254,108,137,172,255,87,238,199,255,250,179,110,234,67,137,42,175,123,63,149,34,77,161,
  229,146,48,5,30,125,226,113,7,255,27,114,41,148,73,48,102,197,184,10,38,252,76,13,
  114,35,40,45,95,105,166,49,210,166,230,96,244,240,235,203,83,241,166,57,194,210,72,161,
  146,83,171,127,152,173,77,109,245,237,169,152,4,23,2,173,38,151,202,240,175,82,252,242,
  73,23,207,196,177,188,52,232,16,110,16,30,235,1,85,148,130,167,125,65,233,120,36,60,
  218,171,134,239,238,159,197,252,217,153,51,20,179,94,60,190,254,34,22,209,75,140,177,209,
  191,44,15,0,48,180,207,71,156,180,0,0,248,131,129,3,172,157,31,178,52,131,143,150,
  199,98,230,247,214,15,159,145,57,174,223,48,37,138,137,174,133,178,101,160,46,109,177,74,
  45,165,25,0,163,62,123,250,62,10,236,53,170,59,128,135,123,85,50,188,100,185,130,144,
  61,6,81,163,240,93,175,206,25,118,165,97,12,244,203,152,68,213,76,77,97,171,178,88,
  117,10,223,176,240,248,140,199,25,50,248,232,214,125,49,106,133,71,28,218,244,195,249,250,
  241,205,223,197,187,70,130,147,214,166,220,121,55,194,190,186,128,201,99,253,78,97,242,160,
  69,39,165,31,213,93,225,76,70,101,203,34,216,224,161,93,1,14,158,115,211,84,123,2,
  95,248,214,205,76,241,187,136,197,229,210,13,23,26,137,189,214,196,155,31,229,116,112,178,
  58,109,65,70,58,211,15,193,0,253,54,39,215,15,255,5,124,116,106,230,194,139,32,4,
  0,64,149,179,18,159,25,105,211,31,60,124,73,155,228,170,220,209,188,224,59,36,50,200,
  136,8,10,45,205,109,85,50,88,186,70,15,19,105,53,146,64,161,133,45,203,252,163,50,
  199,112,22,160,17,56,178,102,34,40,91,43,60,17,103,252,240,233,160,119,165,193,105,169,
  241,227,236,143,110,174,224,107,127,211,114,172,51,235,2,98,29,252,207,6,16,56,31,136,
  44,193,164,14,182,206,255,115,138,255,5,230,181,234,244,173,66,0,0
};
const size_t  WEB3D_INDEX_GZ_LEN = 4819;

const uint8_t WEB3D_APP_GZ[] PROGMEM = {
  31,139,8,0,0,0,0,0,2,255,228,189,9,123,219,56,178,54,250,87,220,254,230,244,
  209,66,201,162,54,91,114,148,60,137,179,116,166,179,29,219,73,119,39,147,209,208,18,101,
  233,132,38,53,36,21,91,78,116,127,251,125,171,176,16,32,41,219,73,103,230,158,239,220,
  94,44,162,0,20,128,194,86,40,20,170,42,149,234,232,254,151,207,94,188,243,100,49,250,
  242,226,201,211,211,97,203,121,249,252,241,227,23,79,134,174,115,252,252,217,47,167,195,182,
  115,252,250,244,225,233,19,196,60,126,253,226,197,31,136,120,243,240,213,176,189,113,78,145,
  71,199,17,204,21,41,198,28,47,191,101,130,206,198,137,87,163,150,51,153,140,92,199,91,
  141,218,135,84,234,235,24,161,151,209,168,237,188,72,70,29,231,114,129,20,127,245,1,155,
  134,128,253,18,34,248,56,65,112,62,65,112,53,65,146,233,100,212,117,162,213,168,199,249,
  159,45,70,110,171,229,4,43,252,184,206,132,126,218,206,156,126,58,206,138,126,186,206,20,
  101,33,201,140,126,92,103,73,63,109,231,130,126,58,206,12,104,145,100,73,63,61,231,156,
  128,125,103,76,63,251,206,21,253,28,56,159,233,103,224,172,241,227,182,156,19,250,65,141,
  233,167,237,156,209,79,199,121,66,63,93,231,133,71,181,245,80,219,87,30,106,123,69,13,
  122,235,161,182,175,189,81,207,121,234,141,250,206,35,111,180,239,92,76,144,238,20,149,115,
  46,145,209,121,67,141,60,39,170,140,169,145,87,212,200,87,9,178,125,158,32,219,122,130,
  108,39,147,209,62,55,247,37,34,209,150,135,11,252,184,206,47,244,131,90,68,248,65,45,
  232,167,235,60,141,241,211,119,174,81,15,191,227,124,10,137,62,174,243,201,163,223,182,243,
  218,167,223,142,243,144,104,211,234,50,210,71,49,125,11,122,94,115,124,223,57,141,232,87,
  20,122,196,52,62,224,111,159,241,13,156,179,9,17,188,229,60,225,95,215,121,155,208,111,
  219,185,164,124,32,201,115,74,71,52,225,223,158,243,152,127,251,206,67,142,223,119,142,248,
  247,192,121,77,249,218,32,7,26,214,27,160,130,151,244,113,48,24,56,15,9,115,219,117,
  142,248,183,237,140,9,67,187,227,188,227,223,190,115,76,181,106,239,59,199,28,127,224,28,
  19,198,246,192,121,67,240,14,134,35,133,59,46,215,250,57,127,119,156,107,208,166,179,191,
  223,119,62,137,143,125,231,157,248,56,112,158,137,143,129,243,34,162,10,116,209,147,226,3,
  189,41,62,218,206,91,241,209,113,94,227,163,239,14,250,206,83,124,236,119,7,109,231,145,
  248,0,221,249,227,224,192,249,36,62,6,206,47,49,71,181,156,119,34,141,235,60,163,143,
  3,140,173,95,196,199,192,249,141,63,64,207,223,197,135,235,252,83,124,180,157,63,196,71,
  199,249,139,248,232,58,239,197,71,207,249,85,124,244,157,191,138,143,125,231,191,197,199,129,
  243,95,226,99,224,164,1,125,128,194,190,248,112,157,16,31,125,170,243,66,124,116,157,68,
  124,244,156,152,62,218,7,29,199,19,31,93,231,183,152,63,122,78,36,32,125,166,103,16,
  143,218,52,14,223,121,244,139,229,128,127,219,142,63,161,223,142,19,226,183,139,248,5,255,
  186,78,194,191,98,198,31,173,70,29,204,71,129,39,192,208,63,166,153,16,47,70,187,187,
  206,43,127,180,155,196,231,103,187,206,36,22,95,141,96,17,250,94,188,235,204,1,80,223,
  169,74,38,58,23,115,170,127,32,16,190,193,186,128,70,63,167,31,44,41,244,211,113,30,
  211,79,215,153,4,248,233,57,175,40,212,119,222,210,207,190,51,39,32,134,161,200,247,134,
  6,95,171,43,166,197,243,201,104,23,141,220,241,147,93,231,33,86,35,204,166,207,9,45,
  37,238,225,108,21,78,210,69,20,238,164,179,202,162,250,101,22,197,149,192,79,119,210,209,
  162,25,248,225,121,58,111,184,135,233,253,81,235,176,209,72,171,11,164,249,144,126,188,63,
  234,247,122,157,94,53,246,211,85,28,254,212,58,148,31,238,70,99,243,25,155,128,239,60,
  140,99,111,253,104,53,155,249,113,115,145,188,91,248,151,136,252,249,231,159,42,139,157,69,
  152,164,94,56,241,163,217,206,99,47,245,40,174,154,97,89,197,6,150,105,52,89,93,248,
  97,218,156,196,190,151,250,79,2,159,66,175,78,42,187,243,52,93,14,247,246,46,47,47,
  155,151,157,102,20,159,239,185,131,193,96,239,106,158,94,4,187,206,194,64,248,116,85,169,
  126,161,246,45,70,192,189,59,241,194,207,94,178,91,149,13,216,89,52,147,116,29,248,205,
  233,34,89,6,222,122,180,123,22,68,147,79,64,177,33,42,94,206,71,95,54,206,58,25,
  133,171,32,200,8,55,141,43,205,102,115,33,240,166,163,221,211,95,142,159,60,105,238,214,
  129,108,190,152,165,149,234,225,58,121,176,78,42,187,65,116,142,14,119,56,241,112,18,133,
  73,132,162,0,172,72,88,86,205,71,171,138,198,183,248,208,250,120,8,194,167,235,37,81,
  41,29,97,192,164,241,34,60,223,253,249,231,20,245,245,226,52,249,109,145,206,43,187,167,
  39,47,134,187,85,145,207,71,62,247,227,161,255,243,207,62,72,126,146,122,147,79,167,177,
  55,241,31,16,186,250,104,119,103,183,238,55,207,253,244,69,52,241,168,200,74,117,72,25,
  70,255,201,73,119,82,74,187,19,70,233,142,247,217,91,4,222,25,106,186,243,36,164,223,
  29,217,192,87,209,212,111,78,188,37,8,231,103,248,119,119,210,104,71,66,119,146,12,87,
  210,252,207,141,34,114,214,206,55,169,36,221,98,196,77,62,220,74,66,16,96,157,84,137,
  138,151,94,28,106,50,30,250,65,226,235,6,183,202,26,172,8,77,249,42,220,230,39,113,
  140,81,158,86,179,78,224,56,213,11,89,245,30,127,71,245,124,66,254,61,245,227,140,219,
  42,40,34,139,53,252,180,176,198,222,162,249,223,209,34,172,160,119,171,135,24,227,33,134,
  236,215,175,149,203,57,230,236,232,39,236,35,178,61,198,72,187,70,187,80,89,95,207,178,
  208,191,220,121,19,71,23,139,196,175,168,84,149,208,73,176,52,168,60,49,230,80,114,185,
  72,39,243,202,162,57,9,22,152,133,191,121,139,244,100,29,78,80,197,69,243,228,143,87,
  71,227,167,47,222,158,252,50,62,122,253,242,229,195,87,143,79,198,143,158,159,58,45,140,
  205,137,151,248,152,102,191,61,124,126,58,126,250,240,249,139,39,143,135,9,232,119,134,41,
  253,233,80,70,158,62,127,249,228,245,219,211,241,147,223,223,60,63,166,4,126,122,186,184,
  240,163,85,90,137,81,83,153,120,234,207,188,85,144,14,49,116,55,155,92,146,77,149,39,
  236,167,213,232,203,135,23,222,199,225,99,207,249,240,10,191,79,241,251,22,191,143,240,123,
  149,124,28,190,198,239,99,132,95,224,247,41,126,95,225,247,17,126,223,226,247,53,126,175,
  146,141,115,20,142,38,129,151,36,95,188,233,244,201,103,180,245,197,34,73,253,208,167,238,
  0,217,210,249,34,105,142,3,9,75,70,163,209,231,104,49,221,105,253,252,115,37,31,245,
  101,35,70,80,56,202,197,28,134,212,65,70,78,14,127,248,88,117,232,163,185,8,167,254,
  213,235,89,197,175,34,77,195,253,249,103,134,46,87,201,28,160,205,220,75,74,170,85,94,
  142,234,99,93,214,131,159,220,33,97,251,41,43,60,87,228,79,84,36,166,238,69,244,217,
  191,115,57,152,14,89,25,114,155,224,166,99,237,4,122,138,79,116,145,2,73,60,74,140,
  82,15,227,159,68,83,147,38,86,226,197,196,71,183,186,232,103,90,152,61,12,60,174,9,
  102,136,156,90,37,229,251,165,229,135,35,255,67,218,164,165,148,43,17,26,149,0,216,139,
  49,249,24,153,170,108,51,225,194,91,213,67,181,49,198,216,230,61,212,85,108,142,135,241,
  61,239,48,174,215,171,201,135,248,35,86,195,32,224,94,119,82,204,63,133,143,118,139,205,
  102,227,252,226,143,62,236,182,90,187,206,110,203,165,63,109,250,211,161,63,93,250,211,163,
  63,125,250,179,79,127,14,232,207,128,254,120,244,231,140,254,76,232,207,148,254,248,244,103,
  134,63,46,225,115,9,159,75,248,92,194,231,18,62,151,240,185,132,207,37,124,46,225,115,
  9,159,75,248,92,194,231,18,62,151,240,185,132,207,37,124,109,194,215,38,124,109,194,215,
  38,124,109,194,215,38,124,109,194,215,38,124,109,194,215,38,124,109,194,215,38,124,109,194,
  215,38,124,109,194,215,38,124,29,194,215,33,124,29,194,215,33,124,29,194,215,33,124,29,
  194,215,33,124,29,194,215,33,124,29,194,215,33,124,29,194,215,33,124,29,194,215,33,124,
  93,194,215,37,124,93,194,215,37,124,93,194,215,37,124,93,194,215,37,124,93,194,215,37,
  124,93,194,215,37,124,93,194,215,37,124,93,194,215,37,124,61,194,215,35,124,61,194,215,
  35,124,61,194,215,35,124,61,194,215,35,124,61,194,215,35,124,61,194,215,35,124,61,194,
  215,35,124,61,194,215,35,124,125,194,215,39,124,125,194,215,39,124,125,194,215,39,124,125,
  194,215,39,124,125,194,215,39,124,125,194,215,39,124,125,194,215,39,124,125,194,215,39,124,
  251,132,111,159,240,237,19,190,125,194,183,79,248,246,9,223,62,225,219,39,124,251,132,111,
  159,240,237,19,190,125,194,183,79,248,246,9,223,62,225,219,39,124,7,132,239,128,240,29,
  16,190,3,194,119,64,248,14,8,223,1,225,59,32,124,7,132,239,128,240,29,16,190,3,
  194,119,64,248,14,8,223,1,225,59,32,124,3,194,55,32,124,3,194,55,32,124,3,194,
  55,32,124,3,194,55,32,124,3,194,55,32,124,3,194,55,32,124,3,194,55,32,124,3,
  194,55,32,124,30,225,243,8,159,71,248,60,194,231,17,62,143,240,121,132,207,35,124,30,
  225,243,8,159,71,248,60,194,231,17,62,143,240,121,132,207,35,124,103,132,239,140,240,157,
  17,190,51,194,119,70,248,206,8,223,25,225,59,35,124,103,132,239,140,240,157,17,190,51,
  194,119,70,248,206,8,223,25,225,59,35,124,19,194,55,33,124,19,194,55,33,124,19,194,
  55,33,124,19,194,55,33,124,19,194,55,33,124,19,194,55,33,124,19,194,55,33,124,19,
  194,55,33,124,83,194,55,37,124,83,194,55,37,124,83,194,55,37,124,83,194,55,37,124,
  83,194,55,37,124,83,194,55,37,124,83,194,55,37,124,83,194,55,37,124,62,225,243,9,
  159,79,248,124,194,231,19,62,159,240,249,132,207,39,124,62,225,243,9,159,79,248,124,194,
  231,19,62,159,240,249,132,207,39,124,51,194,55,35,124,51,194,55,35,124,51,194,55,35,
  124,51,194,55,35,124,51,194,55,35,124,51,194,55,35,124,51,194,55,35,124,51,194,55,
  155,237,126,116,30,206,71,110,187,211,237,245,247,157,36,30,189,244,210,121,243,205,243,61,
  247,0,226,8,28,159,15,90,123,18,148,177,215,254,66,179,236,28,23,123,225,52,186,168,
  84,107,221,246,0,167,212,253,246,160,247,181,229,164,55,69,250,55,69,134,55,68,202,13,
  179,242,139,255,97,241,115,187,215,251,88,167,175,251,247,15,204,128,219,55,67,237,174,8,
  237,54,118,9,146,234,184,84,103,83,81,156,213,237,125,237,119,85,130,92,102,255,231,126,
  231,171,219,62,224,104,63,159,223,183,138,246,179,220,8,133,217,151,89,219,208,202,18,234,
  44,213,102,26,189,136,46,253,248,8,28,88,197,100,15,211,28,123,200,180,186,240,174,176,
  251,139,79,176,155,190,99,177,148,47,38,148,71,229,168,44,254,35,173,167,213,255,72,179,
  4,225,76,32,117,152,173,84,60,73,189,178,192,9,179,86,73,26,97,117,175,226,227,59,
  203,145,204,114,213,88,96,227,78,31,32,213,2,105,83,252,29,182,178,212,113,108,167,174,
  184,13,191,90,91,212,253,154,81,137,88,87,66,35,149,249,220,6,183,204,191,90,86,26,
  126,45,52,155,230,113,166,145,171,179,164,34,173,119,150,84,68,179,107,237,170,85,243,168,
  80,243,123,168,120,107,184,184,63,242,31,184,195,202,98,196,205,22,13,118,22,181,69,173,
  210,105,180,107,22,65,131,111,198,1,44,252,95,191,225,246,170,117,183,101,98,155,204,204,
  238,217,89,212,185,5,179,32,2,131,99,207,4,34,108,221,53,243,206,75,243,90,25,204,
  211,187,41,3,64,125,154,189,134,149,222,72,58,229,164,11,131,13,173,96,165,208,39,174,
  135,243,58,22,135,142,219,235,247,32,62,82,108,172,156,243,139,139,21,184,174,191,99,250,
  220,135,76,36,253,234,86,157,244,239,163,180,158,143,220,71,92,31,145,21,149,184,91,197,
  223,86,117,79,207,248,126,86,161,153,93,247,36,206,162,150,118,212,73,146,69,93,88,81,
  247,209,140,87,171,139,51,150,122,60,15,83,255,220,143,89,240,209,174,213,4,33,162,85,
  56,21,68,135,24,160,141,56,226,240,141,243,241,185,137,143,211,45,163,203,74,91,76,189,
  137,191,8,116,102,164,19,75,231,139,87,109,147,176,227,155,48,24,157,126,3,138,43,107,
  186,10,30,88,84,32,74,192,10,243,103,178,8,157,104,132,227,235,94,187,234,4,35,79,
  124,76,0,169,164,117,204,103,4,86,128,234,192,146,99,26,34,48,231,24,25,152,82,76,
  72,195,25,129,49,197,200,192,161,60,114,38,226,16,185,251,251,31,191,239,14,113,2,247,
  211,74,84,91,57,65,109,137,255,231,78,84,155,152,199,201,221,63,222,255,161,146,137,104,
  145,52,159,236,253,239,239,179,100,10,211,170,144,236,247,247,185,66,199,248,127,90,44,244,
  119,163,208,169,163,146,22,10,253,195,40,84,97,178,10,85,135,92,156,220,119,137,208,111,
  211,69,144,12,119,40,203,127,173,32,4,139,67,244,208,83,28,215,113,100,95,250,241,147,
  85,128,33,86,133,104,123,130,161,133,104,127,186,227,97,34,134,159,194,232,18,139,81,60,
  245,227,33,68,63,137,41,67,184,12,197,164,150,244,133,124,13,66,135,52,94,77,210,40,
  150,7,246,167,65,228,165,157,54,203,241,134,106,120,139,227,250,219,69,88,136,201,102,84,
  47,75,228,246,115,137,88,134,152,197,31,136,232,44,124,20,120,23,75,127,154,203,133,237,
  74,228,121,94,40,87,239,77,72,229,118,33,176,134,36,120,223,105,184,85,157,33,87,7,
  35,67,167,189,223,183,211,30,108,75,234,182,69,66,213,55,233,60,142,46,89,110,34,228,
  54,82,60,100,116,215,115,72,25,3,44,107,147,232,98,25,133,56,185,238,208,9,180,185,
  107,246,66,232,255,224,94,48,86,152,133,193,212,84,183,246,136,149,65,8,120,191,177,119,
  44,12,232,168,234,141,61,165,19,234,190,186,185,163,84,122,238,170,27,251,73,165,68,79,
  253,160,110,34,105,210,227,201,232,203,227,39,207,218,199,15,33,146,138,29,252,180,17,28,
  158,36,206,57,73,29,48,27,223,190,125,254,120,232,47,32,158,7,109,134,215,88,51,87,
  16,143,77,125,47,124,25,77,87,65,52,124,49,113,46,188,229,11,150,253,15,195,153,179,
  8,63,67,92,225,191,240,227,229,48,153,57,1,253,198,177,51,165,236,241,204,89,66,202,
  139,154,156,15,189,153,147,92,68,81,58,135,128,99,57,140,84,8,121,41,28,204,28,218,
  82,65,139,225,68,124,242,72,25,206,141,192,201,18,235,201,116,184,66,86,223,159,250,211,
  99,222,131,135,211,153,51,245,207,79,163,99,196,205,40,249,244,52,122,236,159,15,151,168,
  91,242,134,56,194,215,179,211,203,104,120,49,115,104,187,49,32,231,51,135,183,15,3,52,
  38,228,219,215,165,225,213,204,9,163,248,2,52,190,246,135,161,143,146,179,224,101,184,113,
  86,169,144,180,237,44,190,64,114,156,46,38,95,22,205,101,28,165,17,119,3,46,14,124,
  154,12,109,72,50,55,198,228,168,164,35,226,240,91,82,20,119,53,74,29,254,88,143,252,
  13,196,47,59,151,139,41,132,227,25,199,198,137,72,96,40,99,210,44,35,167,159,251,139,
  243,121,154,203,176,230,12,50,74,229,88,35,7,173,221,38,107,86,168,3,127,80,178,19,
  72,135,60,146,234,110,75,154,234,164,191,111,75,69,145,127,228,35,205,156,71,106,232,138,
  74,169,197,68,46,32,173,161,196,230,155,2,87,119,168,234,154,219,116,182,204,24,217,13,
  195,29,22,215,237,44,146,29,136,93,193,232,238,96,176,157,251,180,189,128,1,54,42,72,
  84,53,234,85,82,43,171,169,170,82,86,19,127,72,157,54,147,0,117,168,88,162,110,70,
  111,141,38,174,132,236,147,42,6,218,114,93,210,27,58,5,62,215,130,250,16,15,23,18,
  214,141,148,117,43,233,150,209,80,215,195,65,125,81,98,209,186,164,116,160,33,143,111,213,
  6,65,187,24,95,230,47,201,206,245,171,249,102,13,107,106,196,174,206,10,181,107,24,205,
  105,100,205,65,210,45,205,105,232,230,52,244,32,93,157,221,220,156,134,221,156,134,110,14,
  56,249,116,177,12,138,221,81,51,170,85,203,170,165,210,111,169,91,77,215,77,125,65,186,
  252,25,171,117,33,229,158,129,127,47,195,47,82,151,99,207,149,237,238,97,248,121,75,0,
  176,221,196,139,171,78,78,116,125,229,72,25,250,218,73,80,128,47,174,54,181,188,94,18,
  39,193,5,82,205,175,39,31,58,31,107,33,126,250,31,21,157,18,92,221,113,76,87,196,
  236,127,148,36,192,233,188,48,120,245,185,93,13,245,230,85,85,33,178,227,208,80,204,1,
  137,138,206,252,229,168,40,102,27,42,21,103,162,226,237,177,172,247,33,109,200,16,57,190,
  129,76,197,48,26,196,88,168,84,23,220,136,16,145,37,200,20,84,32,122,193,50,253,194,
  197,134,16,245,87,170,86,119,88,189,31,126,197,169,51,223,233,40,38,228,18,112,150,226,
  131,86,41,245,68,148,128,216,180,51,98,116,115,233,200,87,138,134,99,202,176,100,17,10,
  137,224,142,74,177,136,168,50,52,70,140,133,231,52,122,239,199,81,57,54,44,169,116,35,
  88,130,205,136,81,216,66,255,28,124,67,1,81,195,90,143,101,80,77,192,40,45,206,106,
  90,16,229,180,198,80,217,76,226,40,73,202,82,173,27,58,213,213,70,244,240,201,63,43,
  133,116,98,153,84,73,5,43,160,198,131,117,172,78,254,25,171,193,86,154,171,138,25,20,
  206,189,20,218,14,47,202,242,147,8,73,146,170,110,3,144,85,243,73,185,10,218,75,144,
  57,86,105,64,110,176,247,5,126,190,28,84,160,93,81,116,108,88,69,190,121,46,178,156,
  70,217,250,148,107,92,70,41,16,46,11,84,213,149,155,186,109,219,81,162,221,182,121,229,
  41,122,108,207,63,180,106,4,57,130,152,44,13,215,33,113,19,46,249,88,39,68,84,163,
  156,200,89,154,147,127,174,60,156,115,233,186,124,83,6,181,214,217,70,106,172,181,8,172,
  85,77,124,44,159,97,45,204,58,233,241,214,74,100,61,69,216,10,189,69,88,171,196,139,
  169,213,196,238,50,163,43,155,133,29,106,67,39,128,210,125,186,66,59,163,236,44,99,195,
  174,100,3,89,129,25,135,177,197,26,2,78,131,103,128,162,129,168,126,45,52,121,7,128,
  69,3,36,120,227,131,142,129,53,129,128,96,36,169,73,10,40,107,21,90,111,102,224,247,
  249,48,70,197,18,67,158,43,246,131,175,55,44,124,215,93,185,73,165,145,204,132,27,111,
  59,31,50,168,253,81,100,80,155,100,202,101,9,141,162,135,41,118,212,179,21,214,142,82,
  126,226,156,216,233,108,237,103,192,31,10,128,53,12,7,13,255,161,92,221,178,117,95,201,
  183,40,97,162,69,92,20,138,205,145,228,149,140,36,89,114,92,11,27,94,45,169,27,172,
  98,140,160,135,253,89,115,16,74,16,90,186,20,203,56,123,21,54,129,155,218,135,147,245,
  197,89,20,52,23,41,29,63,163,248,35,80,173,23,126,48,149,152,28,35,176,198,13,244,
  68,169,50,148,28,159,28,82,221,76,72,180,205,201,23,73,118,142,35,213,17,113,195,174,
  79,43,99,117,178,105,142,175,71,114,4,141,47,71,96,239,248,224,182,147,208,32,124,26,
  120,105,69,10,14,157,216,241,156,72,144,55,128,18,64,82,111,125,132,124,144,62,48,14,
  86,252,209,254,8,185,32,125,116,62,66,38,24,127,240,40,205,148,63,144,102,204,31,72,
  179,230,143,14,95,224,47,33,49,94,127,253,26,224,7,154,46,19,252,76,191,126,93,225,
  103,44,138,186,24,65,146,87,159,212,166,245,85,109,92,95,214,214,135,23,247,72,184,60,
  31,53,230,64,221,152,2,109,99,12,148,141,181,115,49,106,92,8,129,243,108,228,54,34,
  66,127,113,175,9,69,178,158,192,117,57,202,150,171,139,170,115,156,13,139,75,168,7,100,
  161,89,237,178,186,119,12,113,168,134,68,2,18,160,50,179,250,188,22,161,225,19,124,77,
  241,181,26,173,240,53,198,215,114,180,196,215,186,22,109,132,206,208,221,18,31,138,154,185,
  123,217,250,24,212,2,180,120,130,22,175,208,226,37,154,84,27,93,58,19,250,179,162,63,
  75,252,217,108,120,114,5,106,94,77,248,163,253,113,180,226,143,206,199,209,82,117,165,90,
  164,178,241,144,228,59,86,208,39,162,190,251,232,4,170,83,39,170,83,87,170,83,151,212,
  115,170,111,93,213,183,109,213,183,232,82,115,230,71,232,177,85,109,89,135,88,180,49,129,
  60,86,86,53,96,48,117,234,178,17,65,98,42,43,62,97,240,180,30,213,230,13,146,224,
  202,102,160,219,145,106,9,208,28,72,144,154,207,248,87,185,57,55,22,2,129,43,125,180,
  55,70,122,20,30,205,233,16,121,4,229,142,51,104,109,225,122,140,80,172,243,40,132,136,
  96,157,161,88,223,134,226,58,143,226,154,81,92,103,40,174,111,67,113,153,71,113,41,36,
  27,25,138,203,155,80,72,233,133,117,17,119,199,153,190,21,169,226,167,239,120,212,30,95,
  169,114,84,41,170,140,242,195,247,216,56,125,143,179,227,55,211,170,169,243,226,251,242,150,
  26,162,237,36,160,18,34,115,218,176,126,106,105,174,159,106,69,7,49,84,42,166,159,107,
  90,233,129,138,228,231,106,98,211,237,71,144,221,126,76,70,81,37,20,247,28,81,37,17,
  119,28,81,37,22,247,27,129,136,154,226,35,17,247,26,1,71,169,59,13,79,223,105,224,
  66,64,53,115,94,163,209,143,65,139,171,1,213,92,10,45,27,20,51,214,205,158,80,8,
  43,197,148,70,189,108,62,193,40,29,242,218,247,18,223,139,191,81,130,191,94,192,255,158,
  238,61,44,252,141,18,252,245,239,174,255,123,186,243,249,46,252,119,171,255,31,239,127,191,
  3,125,238,138,191,88,255,223,223,223,133,62,119,237,95,171,254,230,77,81,182,84,139,171,
  162,108,156,223,126,53,228,105,217,29,177,243,63,97,179,188,211,52,122,120,181,72,30,242,
  113,195,224,162,112,9,104,114,79,161,125,130,22,51,185,150,152,115,89,135,120,54,235,208,
  101,198,144,133,213,187,205,235,99,226,236,64,1,33,112,49,206,1,90,186,226,144,50,32,
  56,140,4,63,221,143,152,232,254,135,131,143,152,232,62,132,41,152,228,254,135,30,109,102,
  254,135,1,109,101,254,7,222,200,124,18,186,44,41,73,139,246,177,176,30,213,151,196,40,
  204,239,203,197,99,58,106,246,140,189,120,142,251,243,67,213,134,102,187,183,55,213,156,84,
  101,213,8,170,181,169,110,125,37,110,76,178,240,245,168,226,53,18,132,153,27,216,33,93,
  197,251,17,148,51,239,47,85,57,237,90,86,140,91,15,27,81,99,153,21,197,200,141,194,
  80,180,89,20,248,208,44,22,69,197,245,9,194,186,168,104,91,41,81,35,180,74,161,42,
  155,77,178,240,174,173,82,81,74,80,95,169,82,74,177,47,129,61,50,176,19,1,76,236,
  162,150,89,43,4,62,141,159,75,219,88,35,236,150,49,242,54,92,164,150,48,82,45,255,
  116,78,245,161,56,161,21,102,239,185,126,227,224,65,133,120,228,236,184,135,211,211,125,35,
  116,93,125,160,182,180,81,35,219,151,214,198,126,117,61,106,233,17,29,86,135,58,121,75,
  167,109,100,155,216,181,177,185,81,114,35,61,203,105,155,215,148,184,230,91,37,81,152,207,
  38,20,111,32,162,48,31,83,106,90,188,202,72,157,252,161,212,148,0,200,214,203,94,98,
  198,87,183,87,9,209,196,145,94,30,221,171,242,64,117,26,65,163,126,154,20,36,105,25,
  110,165,34,172,229,5,134,214,173,22,66,186,142,191,151,91,52,18,121,66,78,100,79,66,
  252,17,166,139,52,207,141,17,99,211,114,232,95,136,65,248,150,45,127,163,3,70,228,191,
  87,66,228,180,49,190,115,188,70,13,74,200,138,186,198,247,181,241,189,109,140,149,72,167,
  198,36,120,26,75,193,208,152,68,79,227,117,93,97,36,54,67,6,46,41,112,185,77,42,
  53,150,2,38,19,145,248,205,112,137,223,12,157,228,13,111,145,92,125,63,102,91,68,37,
  148,118,202,101,167,212,231,15,74,6,126,171,100,138,184,52,228,113,182,73,179,227,167,170,
  102,54,228,85,69,179,193,174,170,154,49,131,170,178,183,237,29,219,174,24,74,206,64,74,
  207,123,131,139,213,111,202,198,37,85,55,229,113,254,109,44,232,37,237,76,20,23,208,207,
  154,118,38,138,91,209,207,101,126,123,13,113,2,244,106,81,61,169,77,26,113,45,208,52,
  75,24,30,212,227,26,22,244,218,68,83,46,102,248,4,130,176,160,145,224,148,169,232,231,
  213,86,72,23,1,22,0,207,228,182,13,56,19,98,221,222,22,99,13,57,140,248,104,30,
  142,26,56,142,140,26,56,89,142,26,56,92,142,26,16,28,140,104,99,16,178,3,40,243,
  209,226,17,153,71,243,137,113,52,143,136,5,215,60,7,52,105,50,38,29,71,99,236,32,
  43,165,15,202,18,29,1,201,143,175,0,52,240,11,99,44,0,37,253,194,56,35,58,250,
  133,177,22,128,146,254,246,147,23,111,181,255,210,82,141,9,105,142,11,209,61,185,81,151,
  151,17,202,131,151,92,109,41,58,147,85,137,201,45,247,132,55,207,107,182,96,202,223,26,
  147,211,179,213,140,33,239,253,80,254,114,98,3,144,95,245,177,156,39,53,221,105,152,199,
  50,68,29,142,80,92,179,68,116,53,67,124,87,45,19,97,142,181,12,115,204,66,204,177,
  150,98,142,215,28,190,214,225,107,14,95,234,240,229,205,98,206,177,41,231,28,91,130,78,
  238,61,22,86,100,221,198,66,138,91,230,211,29,228,163,99,91,64,74,103,105,33,21,209,
  71,106,33,13,145,229,222,85,126,58,46,8,80,199,121,9,170,224,46,0,121,159,65,46,
  5,228,183,12,178,189,105,127,61,121,253,42,183,191,169,246,86,55,58,95,97,35,205,35,
  84,55,184,37,37,125,185,85,78,58,182,4,165,32,158,25,186,182,66,16,156,57,15,239,
  162,165,210,217,162,165,66,98,214,18,77,21,241,1,1,75,38,146,201,100,39,214,203,48,
  209,135,215,146,178,165,56,190,85,229,164,121,253,67,116,79,222,231,35,175,127,180,98,10,
  67,219,67,137,252,155,212,85,58,255,94,117,21,89,83,139,26,63,164,162,223,174,195,34,
  187,248,27,84,89,154,153,40,237,27,180,90,154,215,117,43,219,157,53,92,56,227,159,212,
  117,17,85,174,251,185,242,191,75,245,69,54,228,219,21,97,112,32,203,40,240,45,74,49,
  156,241,79,170,199,8,10,52,52,5,190,81,91,6,237,205,234,254,205,154,51,156,219,206,
  123,115,43,106,118,43,106,118,43,106,89,63,146,206,140,20,209,218,104,56,38,99,99,42,
  71,115,91,210,69,215,193,156,38,39,153,186,27,14,59,211,55,232,238,112,27,152,203,221,
  162,196,19,11,37,158,88,40,241,196,144,39,41,41,23,221,107,185,34,174,43,226,246,117,
  28,120,115,8,160,56,174,39,226,14,84,156,168,219,43,230,246,50,97,87,161,153,89,229,
  237,211,126,22,215,253,198,134,129,53,119,247,42,220,16,95,84,150,234,229,162,9,9,253,
  226,85,81,174,233,21,221,246,110,214,6,74,218,254,88,173,121,138,8,21,77,5,217,210,
  129,76,213,201,82,145,200,74,209,163,47,203,109,201,100,93,157,108,147,239,223,187,181,143,
  175,117,1,140,120,66,4,124,149,0,163,31,181,10,174,115,113,151,20,210,249,2,161,168,
  230,227,48,148,144,156,31,33,113,221,235,231,155,236,227,246,106,2,158,156,174,170,86,170,
  133,33,128,43,92,84,209,161,108,169,26,148,0,184,4,79,191,2,26,113,194,194,201,50,
  250,111,204,162,27,186,19,93,6,21,43,250,250,45,138,3,104,0,179,58,113,53,159,68,
  34,210,210,208,234,102,21,222,13,121,62,231,182,18,140,74,84,55,48,248,16,38,120,47,
  125,241,120,17,139,204,63,104,202,116,115,67,223,156,50,198,96,41,76,25,115,136,20,5,
  94,223,164,245,215,188,222,203,214,201,111,213,0,252,113,234,120,104,157,29,135,26,41,166,
  240,7,170,234,233,98,84,156,89,204,143,83,227,67,49,42,134,11,64,204,245,15,86,240,
  179,139,248,191,75,237,79,245,130,17,115,253,3,21,2,21,250,44,226,250,199,106,10,170,
  2,140,152,235,127,133,14,161,42,199,136,185,254,110,237,66,32,107,168,225,114,71,101,195,
  186,100,133,48,130,191,77,163,80,103,228,159,239,211,47,180,113,252,25,109,195,28,224,250,
  187,213,15,255,191,211,129,35,22,158,110,100,100,15,254,75,84,227,228,145,163,34,239,97,
  180,198,92,169,198,41,3,117,201,82,114,108,3,77,121,169,16,151,10,105,41,9,75,137,
  91,38,185,239,154,197,190,215,121,189,112,22,203,70,153,154,153,7,105,109,160,197,1,44,
  184,245,44,150,226,117,168,206,69,217,181,104,54,98,11,186,156,246,149,138,54,98,35,228,
  182,36,205,206,52,58,45,241,97,174,111,195,106,86,254,155,192,11,205,61,247,56,144,185,
  8,93,179,88,77,209,26,58,139,29,7,116,92,159,5,37,204,139,136,222,86,124,187,102,
  8,155,171,213,255,223,169,186,106,94,139,134,107,78,239,21,34,230,228,71,233,190,230,215,
  15,158,29,234,202,245,100,137,87,107,11,116,72,161,239,114,241,71,216,235,232,6,17,2,
  227,233,98,133,249,210,92,206,241,64,178,137,87,111,169,87,68,167,146,139,73,109,220,35,
  10,153,112,45,45,204,25,67,45,193,222,31,89,108,92,211,162,49,67,210,28,218,215,199,
  71,107,88,241,155,222,212,26,35,69,177,61,220,18,71,105,11,111,73,94,186,72,89,210,
  110,117,168,208,139,146,41,253,182,234,43,216,245,55,81,178,200,241,228,219,24,111,159,142,
  102,90,190,72,39,48,85,136,79,231,172,18,228,44,116,201,141,68,43,193,81,20,172,46,
  200,214,90,171,170,55,11,53,74,203,19,186,70,194,228,166,132,237,106,57,203,118,165,68,
  163,6,145,146,146,186,107,68,185,157,199,144,247,103,103,95,191,214,173,218,217,59,223,152,
  191,83,205,169,160,21,247,162,177,33,35,25,27,2,146,241,181,61,14,163,32,42,205,31,
  27,90,207,89,238,179,111,214,236,166,144,190,17,185,254,78,61,239,166,117,251,241,93,90,
  223,198,157,198,245,15,211,1,111,230,47,48,254,164,90,182,230,118,45,93,109,17,202,14,
  196,234,30,205,54,160,161,110,206,218,5,147,53,109,108,24,161,117,99,230,23,165,13,161,
  117,41,86,184,23,176,46,208,190,67,131,220,12,92,227,34,228,24,170,190,16,63,63,116,
  142,230,252,49,9,157,87,183,60,225,149,83,165,112,57,98,42,137,59,129,51,145,87,36,
  106,186,140,62,184,142,84,231,80,191,24,70,134,181,16,205,161,20,49,109,202,193,76,255,
  213,200,42,70,145,115,5,129,3,228,153,43,50,136,153,224,7,131,14,202,215,16,116,129,
  152,43,72,33,70,49,126,122,164,62,189,130,108,1,171,202,10,210,175,145,135,159,3,86,
  165,190,69,49,165,208,22,117,152,147,2,122,99,237,52,116,213,138,171,52,105,175,65,181,
  26,154,104,164,178,134,47,151,190,218,244,213,166,175,14,125,117,232,171,75,95,93,250,234,
  209,87,143,190,168,226,164,205,230,83,221,67,122,50,72,250,111,248,58,144,51,212,191,34,
  203,157,233,35,47,89,20,54,162,102,249,210,71,140,109,11,151,192,55,68,163,177,225,13,
  209,237,178,45,171,123,235,94,69,100,245,5,37,186,162,33,76,19,110,176,195,42,124,190,
  160,9,55,24,130,160,59,42,124,112,249,176,94,250,45,218,30,89,158,156,170,135,17,97,
  240,250,186,139,161,135,152,5,226,92,255,123,162,163,35,209,163,129,232,186,137,232,243,149,
  232,220,165,232,197,185,232,254,169,232,231,177,232,208,53,63,30,197,19,7,122,60,234,204,
  70,252,120,244,146,223,141,226,33,3,189,27,117,78,70,252,110,244,12,63,200,254,18,63,
  200,126,132,159,3,173,150,79,130,56,40,131,172,33,187,188,132,184,242,204,33,225,47,0,
  23,0,28,3,240,210,33,89,27,0,51,0,78,0,56,114,72,62,7,109,213,53,116,243,
  47,241,26,129,178,116,9,112,1,192,49,0,148,101,255,35,191,112,88,33,203,146,179,96,
  8,207,145,101,138,44,99,206,210,35,192,5,0,199,0,80,150,3,2,208,163,136,19,0,
  142,182,221,90,148,76,36,115,238,208,157,5,205,18,241,219,151,191,174,252,237,202,223,125,
  249,219,150,191,61,249,123,240,49,123,55,236,99,249,132,40,206,11,83,91,251,41,219,238,
  177,247,129,254,232,111,162,56,216,24,162,49,250,152,122,3,210,102,162,63,196,205,66,227,
  52,21,93,155,82,111,96,129,50,232,239,215,72,19,199,175,145,232,56,172,145,182,14,14,
  119,172,31,2,173,28,58,228,213,130,76,217,237,199,87,3,35,108,133,179,37,74,199,32,
  67,185,141,85,13,182,42,208,119,49,100,214,1,134,154,15,9,118,136,167,25,73,109,74,
  135,164,241,246,51,164,249,175,56,79,174,113,153,48,206,222,127,96,160,225,181,142,67,245,
  132,90,41,90,183,130,224,157,1,24,28,144,189,135,212,92,1,232,240,104,113,168,246,208,
  123,133,84,30,231,97,17,211,227,188,49,106,236,11,0,6,231,148,63,48,228,42,164,229,
  52,81,49,24,80,144,239,251,68,86,6,48,139,66,114,236,101,148,104,149,54,103,203,96,
  74,71,114,177,65,101,125,177,226,146,178,133,147,142,228,170,211,254,56,146,75,79,95,193,
  123,114,53,246,197,202,187,255,81,13,38,176,34,55,93,230,20,22,198,166,234,241,166,81,
  223,172,238,176,10,162,56,173,27,39,4,147,156,151,208,84,55,35,213,21,103,42,115,35,
  153,204,188,202,166,186,250,76,88,110,42,83,150,27,199,36,245,245,102,130,106,191,253,124,
  170,46,6,202,95,133,105,230,37,38,211,81,154,89,137,139,186,64,36,222,32,197,53,116,
  23,116,186,60,40,233,71,213,186,87,79,29,244,253,4,186,65,1,125,84,208,187,30,22,
  33,68,69,117,223,218,105,19,113,76,49,120,69,24,82,222,149,219,17,233,204,115,124,149,
  238,224,167,62,150,124,216,200,246,167,205,157,183,184,212,135,44,252,147,127,162,226,193,194,
  192,172,10,76,196,8,38,203,220,29,222,4,70,82,150,7,155,79,14,141,110,181,139,150,
  209,219,203,86,138,237,119,43,94,167,134,17,173,170,49,172,3,47,189,137,0,89,154,237,
  21,57,149,105,238,92,23,51,131,65,144,178,168,140,209,208,102,95,30,88,252,19,9,55,
  136,131,34,185,134,232,217,161,29,207,177,118,175,91,244,176,165,62,146,101,14,71,6,139,
  92,100,48,48,222,128,46,148,104,109,196,39,133,33,101,240,164,34,121,62,91,118,2,187,
  149,227,83,214,135,19,24,226,79,238,13,14,19,152,28,38,49,20,30,220,129,11,166,119,
  119,202,24,191,210,89,7,135,93,56,167,41,44,208,65,58,12,129,37,4,22,171,208,15,
  33,150,161,15,97,221,255,104,169,12,22,207,106,134,160,171,184,144,248,146,43,149,167,183,
  80,44,27,124,118,11,197,34,193,202,104,161,88,98,252,186,226,78,233,91,241,167,244,173,
  56,84,250,86,60,42,125,107,46,245,78,202,48,85,243,0,110,86,24,202,52,206,27,113,
  132,121,149,58,199,115,249,133,12,212,105,205,46,236,148,14,224,216,162,217,233,237,179,167,
  140,38,204,147,118,201,213,69,19,254,82,250,157,129,211,220,119,225,126,225,96,223,105,182,
  246,219,238,0,14,61,154,45,119,208,233,80,18,215,29,184,131,46,62,6,189,86,175,211,
  6,91,251,38,87,64,167,9,63,18,131,62,220,131,52,220,102,175,179,223,57,232,180,157,
  70,179,59,56,232,187,192,208,104,34,174,221,237,244,29,183,121,176,223,131,241,174,30,208,
  119,221,94,175,231,226,163,215,235,147,183,138,70,19,174,95,6,112,254,225,2,212,31,160,
  66,213,204,114,234,231,153,182,156,250,197,103,219,253,211,33,158,242,94,70,241,39,24,150,
  98,169,193,201,18,38,232,135,147,216,73,232,35,25,194,191,1,104,71,251,201,80,27,127,
  151,79,60,205,81,45,177,209,3,33,247,235,215,24,191,222,215,175,63,197,248,31,191,130,
  202,2,33,89,197,22,215,193,62,165,74,97,254,190,146,52,227,81,184,160,31,8,116,154,
  231,226,251,156,190,207,196,247,89,85,137,123,53,142,101,188,184,240,226,133,79,86,195,205,
  40,207,136,98,212,182,174,67,174,34,17,222,215,81,57,91,19,1,29,13,22,74,102,87,
  193,43,111,198,56,201,154,193,223,178,25,252,141,102,32,180,81,244,62,141,12,138,155,196,
  45,188,75,224,237,92,94,84,23,58,11,233,169,143,84,240,52,250,77,164,184,19,202,120,
  11,82,160,4,219,241,70,81,210,192,149,91,205,4,53,18,131,234,156,243,84,210,166,44,
  35,153,196,143,23,15,230,241,48,135,66,17,84,96,192,68,126,137,126,65,181,96,43,205,
  191,67,13,160,28,183,92,165,89,27,142,162,112,182,56,71,39,91,136,190,126,221,37,89,
  246,20,207,81,118,185,160,23,43,102,212,41,189,63,155,45,38,228,189,32,177,136,55,218,
  66,35,221,160,252,130,146,141,176,160,12,57,136,11,205,63,216,124,179,26,245,250,140,110,
  56,154,144,149,44,206,67,19,143,67,25,198,168,169,24,160,219,230,97,146,221,151,20,134,
  120,51,59,162,150,15,110,81,192,227,216,187,68,19,133,8,173,116,112,126,35,237,167,229,
  8,69,105,111,67,124,127,42,45,230,54,138,231,74,47,36,148,197,175,114,37,108,28,106,
  239,111,133,229,174,116,166,16,255,195,105,94,122,161,119,206,59,4,248,160,82,4,224,118,
  224,116,97,231,204,247,113,36,198,82,120,129,167,148,240,122,210,44,153,232,88,235,193,16,
  45,74,163,18,158,202,105,244,167,42,88,146,125,91,245,74,22,13,85,189,178,40,174,30,
  234,55,250,208,236,119,177,11,98,119,195,127,125,108,109,180,17,209,97,6,49,180,19,242,
  62,216,230,77,176,77,103,203,15,205,14,153,200,108,118,218,131,143,153,99,29,49,5,42,
  95,62,76,226,143,195,47,122,249,24,166,206,229,28,210,199,55,240,91,2,95,30,142,90,
  20,134,115,172,85,52,152,135,199,115,71,14,219,225,155,185,83,58,195,134,190,179,101,76,
  12,191,228,7,197,240,21,198,69,249,248,29,126,217,50,128,41,207,6,30,68,252,59,213,
  60,245,191,169,230,223,83,151,77,85,186,36,250,61,29,209,46,159,237,249,216,64,13,11,
  196,247,192,49,180,186,189,7,139,26,122,103,191,51,24,116,192,191,12,181,25,96,128,7,
  221,253,3,178,11,122,208,175,131,131,128,7,184,246,62,249,58,107,55,187,166,17,225,36,
  135,20,206,200,192,229,60,32,11,155,205,65,123,72,204,71,175,150,161,117,192,60,245,251,
  253,106,131,224,92,79,240,179,207,60,105,124,68,218,149,160,5,8,198,136,222,30,191,96,
  246,116,23,100,61,247,247,150,240,107,4,67,208,179,202,222,223,167,136,29,238,45,154,169,
  159,128,135,110,38,241,4,106,5,210,11,210,47,167,47,95,28,177,243,38,233,6,234,254,
  238,106,183,154,73,39,227,137,184,139,101,199,73,166,147,169,66,198,42,216,88,225,165,39,
  181,157,183,36,150,135,40,176,3,88,161,200,96,36,233,3,210,47,1,132,65,72,64,196,
  135,124,191,136,8,214,33,199,43,235,43,188,198,134,71,12,225,141,71,87,226,57,53,149,
  26,255,0,231,164,85,170,131,226,164,80,29,38,188,146,50,88,30,30,116,153,178,36,190,
  51,75,148,218,122,136,101,64,209,18,123,185,178,193,114,252,236,209,105,36,140,141,210,33,
  35,243,33,69,68,96,236,146,6,247,64,60,200,208,243,116,50,147,108,167,252,150,204,86,
  26,157,155,81,62,90,164,176,130,90,146,207,136,85,103,34,203,75,151,159,235,1,191,180,
  3,240,172,187,72,255,240,118,146,106,159,47,200,156,117,73,57,245,201,91,13,141,79,125,
  48,131,191,196,67,239,94,172,220,194,120,56,89,145,173,19,98,104,233,151,204,22,87,201,
  36,174,126,81,108,119,124,194,29,239,248,250,9,118,202,232,51,169,55,133,138,206,104,196,
  65,206,87,165,210,121,206,55,9,154,89,238,253,250,181,24,97,154,240,125,224,211,217,207,
  80,51,67,197,9,164,43,94,29,114,10,9,86,135,227,47,60,73,177,252,18,125,134,138,
  78,130,74,67,69,174,141,104,149,108,57,25,40,224,134,179,213,221,166,53,74,171,195,157,
  183,97,178,90,46,163,24,178,134,29,94,18,132,17,222,157,87,112,43,70,139,224,14,243,
  1,59,130,167,77,216,40,63,184,249,133,47,36,15,88,166,215,51,122,164,157,148,90,58,
  34,87,60,218,192,209,41,70,7,249,46,139,86,241,132,172,127,56,146,37,19,59,149,48,
  92,155,10,22,202,217,93,192,141,5,124,139,6,43,127,184,158,213,235,27,121,60,88,173,
  22,211,17,121,168,16,65,162,134,82,137,167,239,99,72,68,214,218,124,146,172,240,168,69,
  50,190,19,82,127,178,143,254,60,160,212,26,150,205,182,119,184,74,138,172,201,230,231,39,
  155,153,228,65,42,132,21,77,210,205,138,126,147,19,133,3,191,112,119,208,10,35,209,115,
  190,167,49,88,132,18,196,89,164,70,41,253,226,41,164,50,152,161,245,73,24,1,26,235,
  12,246,60,165,44,254,50,133,165,38,170,130,161,81,228,176,33,93,156,223,253,105,242,118,
  57,149,242,177,212,52,64,33,137,7,210,203,71,99,25,237,244,178,173,151,25,195,73,30,
  237,0,63,249,116,131,205,163,41,249,160,251,45,115,122,149,109,28,133,52,114,69,249,66,
  129,161,6,59,171,56,24,238,226,64,145,24,61,39,157,90,137,49,198,102,242,9,196,179,
  11,163,77,156,23,18,184,64,139,33,69,49,215,13,136,220,181,59,41,239,94,196,107,7,
  115,235,139,132,214,6,57,76,31,196,194,227,215,115,152,144,225,72,170,41,108,2,216,224,
  170,124,89,26,143,8,64,203,30,106,58,138,181,73,15,58,157,23,27,73,118,0,194,205,
  38,227,34,144,57,219,240,111,220,52,22,127,102,211,88,124,231,166,177,216,178,105,60,120,
  230,53,13,182,2,94,65,22,220,53,15,196,50,37,186,130,88,178,138,128,87,229,186,181,
  176,215,173,133,26,174,84,190,68,97,138,148,154,196,83,111,134,21,90,201,100,239,208,186,
  197,206,17,193,104,39,208,72,98,237,198,29,25,73,43,211,23,233,146,238,132,86,167,23,
  234,226,252,87,95,93,151,239,32,169,31,78,147,157,163,48,183,102,45,154,143,159,60,125,
  248,246,197,233,248,249,203,135,207,158,128,85,202,32,47,31,190,121,243,252,213,51,48,3,
  159,232,205,52,254,196,163,107,31,87,57,71,11,140,170,113,72,234,130,100,154,40,203,241,
  240,213,243,147,215,167,199,175,223,252,129,75,157,24,157,140,213,150,236,194,56,246,162,248,
  13,203,225,201,246,229,144,232,68,190,80,197,249,77,172,179,212,240,151,153,126,194,197,98,
  137,142,75,72,176,40,194,226,232,174,180,22,38,120,204,25,250,129,178,9,112,25,123,203,
  19,165,197,67,129,83,101,136,10,131,224,233,34,192,53,220,40,86,136,67,9,144,111,70,
  188,112,145,68,41,154,177,150,87,244,77,186,137,240,210,145,84,156,92,144,81,156,208,11,
  158,10,40,77,99,17,65,163,96,36,191,163,217,44,33,151,110,104,196,138,23,47,217,140,
  216,95,250,158,134,187,74,196,11,157,106,194,90,146,94,138,161,85,195,196,251,137,135,171,
  52,18,203,159,222,51,68,132,18,82,50,72,89,155,127,41,73,167,146,26,18,247,135,193,
  114,238,65,54,39,219,25,44,150,127,232,100,226,64,244,48,128,216,129,102,25,156,51,75,
  33,145,58,97,140,228,67,149,21,6,50,77,37,114,122,42,0,92,181,99,122,141,152,117,
  152,222,212,36,129,66,217,128,140,126,56,131,194,182,208,105,230,12,79,141,181,99,35,66,
  143,59,87,197,242,108,213,224,159,42,41,173,225,188,125,232,143,251,138,204,203,11,180,253,
  157,93,19,173,100,60,5,210,173,150,224,197,168,108,170,13,249,69,80,133,57,216,173,102,
  224,75,82,175,57,53,87,231,246,196,215,156,152,215,223,242,196,180,212,240,86,40,210,40,
  43,110,70,44,108,206,139,126,144,215,131,50,133,24,39,205,252,53,91,54,102,149,150,187,
  12,173,205,113,171,226,100,104,109,143,81,115,36,171,148,50,4,213,69,188,180,124,155,141,
  11,195,39,166,57,90,196,30,245,133,157,213,226,220,204,22,168,134,62,150,196,73,0,182,
  207,200,159,84,202,114,139,205,17,124,211,221,228,253,153,34,113,233,155,87,94,149,82,254,
  177,86,166,84,126,216,11,83,170,190,52,247,109,47,84,169,250,178,23,172,84,125,153,11,
  87,42,126,205,229,75,128,78,243,139,88,154,125,231,151,179,52,251,46,44,108,169,17,176,
  214,184,84,126,148,46,117,105,14,96,44,123,194,87,102,97,66,165,70,192,26,86,130,222,
  50,100,173,141,42,74,132,172,229,81,69,137,80,126,133,76,115,3,177,176,84,166,5,144,
  153,80,33,23,161,106,249,18,154,230,33,91,86,212,180,0,50,87,216,84,252,150,47,179,
  105,30,82,88,118,83,35,80,178,114,166,86,240,166,53,52,45,135,151,46,172,105,14,144,
  91,250,137,211,110,46,61,60,249,171,240,167,96,170,23,51,162,168,74,164,174,74,12,238,
  93,109,54,180,148,189,35,46,129,111,57,21,207,75,167,140,157,84,95,34,126,144,190,88,
  51,135,173,95,192,89,253,67,49,79,25,10,156,14,81,21,76,91,140,146,157,255,252,203,
  23,127,243,159,44,233,100,62,132,30,202,227,45,17,51,43,211,230,63,224,31,6,231,255,
  69,184,242,55,82,40,131,234,200,146,146,59,149,36,25,30,85,208,52,242,19,118,136,237,
  227,105,112,106,21,128,155,167,144,60,213,234,171,106,132,179,192,215,175,249,248,142,25,223,
  49,227,229,157,148,136,151,129,7,82,210,31,138,27,110,190,93,221,124,247,33,40,21,109,
  189,229,24,84,146,74,29,132,64,125,143,185,233,47,114,231,31,118,155,251,130,83,86,204,
  240,174,114,136,19,197,26,214,20,21,198,145,41,119,146,162,101,120,168,215,101,135,247,189,
  161,185,233,233,150,138,244,114,181,29,154,139,176,35,215,218,161,185,4,59,98,165,25,126,
  184,97,163,131,186,19,175,84,50,81,249,78,9,45,40,94,152,100,162,242,141,16,106,84,
  114,149,26,218,155,39,45,239,50,103,126,241,135,34,30,175,183,67,99,165,118,236,165,120,
  88,178,94,11,98,235,53,58,243,169,35,201,106,44,205,217,130,50,204,173,54,142,222,68,
  134,246,254,226,232,141,103,104,239,73,78,182,181,12,115,251,142,195,235,222,48,91,10,157,
  220,114,58,44,91,117,157,252,90,58,44,93,116,157,220,186,57,44,91,94,55,74,110,34,
  15,45,159,252,181,124,126,162,87,41,201,69,220,103,43,42,217,10,103,39,114,196,9,185,
  100,252,139,51,50,73,60,132,66,152,122,154,99,184,163,254,34,102,129,76,179,187,49,94,
  66,191,253,172,36,176,198,176,197,196,123,57,209,147,142,69,213,37,87,211,122,227,106,94,
  221,163,201,221,188,2,219,171,108,146,232,97,37,141,147,92,131,136,210,78,132,249,100,20,
  79,117,76,19,42,159,66,149,234,94,11,126,36,93,43,206,27,234,247,59,57,20,255,209,
  38,247,132,238,131,212,126,99,138,24,122,12,116,115,193,27,110,221,90,180,96,93,210,130,
  83,179,5,194,180,133,133,104,93,108,1,167,250,134,22,172,141,22,88,143,97,17,67,47,
  151,110,46,216,178,44,201,163,156,110,9,144,195,109,136,247,210,55,137,176,42,182,12,203,
  228,59,115,251,102,53,67,243,230,229,241,147,151,91,196,97,230,137,7,231,239,205,225,175,
  190,45,39,224,147,150,9,149,178,2,12,57,19,154,201,3,70,238,33,9,40,46,252,187,
  152,55,234,222,96,222,200,176,34,191,221,60,81,147,140,197,111,59,148,93,151,184,231,186,
  190,201,61,215,101,153,123,174,203,204,61,87,209,192,245,205,53,251,110,11,74,205,203,127,
  189,41,165,223,242,145,151,255,30,59,75,12,237,12,101,145,223,100,125,169,251,63,192,250,
  146,172,190,69,184,31,82,251,239,54,201,36,71,204,247,89,102,106,178,141,115,205,187,97,
  73,187,28,186,127,198,102,83,243,178,174,173,166,127,167,253,38,70,241,99,45,57,137,102,
  34,120,249,163,13,59,201,246,254,57,51,79,205,203,70,70,180,239,51,249,196,40,126,172,
  241,39,65,180,134,38,218,159,176,5,5,2,101,13,252,19,118,161,24,143,97,149,231,155,
  76,14,113,126,182,202,179,229,161,170,39,44,196,120,194,66,140,39,12,11,121,244,122,181,
  166,223,64,122,194,82,140,39,44,197,120,194,172,144,71,143,90,85,154,107,164,105,139,52,
  125,145,70,24,21,242,232,177,171,74,4,107,168,194,226,145,39,44,30,121,210,226,145,71,
  22,143,100,162,239,55,41,211,188,220,203,200,253,173,230,101,176,252,107,187,85,244,70,192,
  182,124,36,107,111,26,115,70,73,226,214,219,183,94,22,210,172,224,40,117,107,68,214,175,
  187,210,104,239,213,200,85,20,109,41,178,181,148,105,106,30,164,123,190,49,68,247,124,99,
  128,238,25,15,146,173,154,110,49,214,46,159,8,4,230,43,169,201,40,32,221,226,21,126,
  248,205,83,64,154,192,240,176,240,129,253,137,4,226,205,83,64,47,190,214,248,105,211,155,
  167,128,148,136,103,148,164,197,231,127,205,29,174,26,243,42,180,89,220,159,127,214,160,101,
  99,157,7,141,225,22,134,64,204,191,103,121,235,148,215,202,90,95,231,32,227,250,69,14,
  50,169,79,235,179,70,135,160,213,45,207,2,5,137,14,101,151,188,121,206,29,116,60,66,
  86,23,110,176,241,76,171,50,21,95,103,163,202,76,124,189,28,113,117,246,186,120,175,197,
  213,192,23,204,109,81,241,123,93,213,139,199,247,79,126,254,249,248,254,217,131,99,106,140,
  176,101,158,140,154,251,173,125,183,213,223,63,112,49,209,178,0,58,212,124,109,122,204,230,
  90,247,232,122,233,8,22,185,135,39,64,115,162,208,24,40,160,158,158,71,99,154,121,61,
  97,13,251,61,210,177,188,218,75,170,195,179,82,20,86,149,104,104,153,134,97,207,8,197,
  209,30,20,150,129,66,201,10,249,73,8,15,21,53,192,12,127,61,156,173,114,209,24,195,
  63,61,253,173,115,31,215,248,111,189,50,111,172,240,77,127,171,135,121,67,7,151,212,237,
  52,20,42,100,138,90,25,220,100,36,123,151,218,92,25,225,81,65,216,37,35,84,42,104,
  122,12,146,93,239,178,95,243,127,249,171,124,98,208,104,65,250,209,110,32,111,178,59,165,
  154,171,227,46,73,113,230,223,106,147,74,87,65,197,153,85,248,119,216,171,66,5,84,12,
  23,141,152,203,127,139,37,43,187,224,255,45,246,173,84,127,26,49,151,255,14,203,87,170,
  220,44,226,242,223,101,18,75,21,109,196,92,254,123,141,101,169,26,24,49,151,63,202,140,
  22,112,55,212,32,253,118,171,90,117,201,73,98,98,253,25,3,91,26,141,16,29,252,0,
  115,91,54,198,31,106,124,43,7,184,252,223,99,141,139,14,96,196,130,202,1,241,239,52,
  210,37,79,149,164,160,70,220,238,247,120,187,180,109,162,80,72,219,136,191,252,33,22,82,
  154,150,121,248,31,96,47,197,48,1,127,249,175,179,158,210,204,91,127,255,87,152,83,209,
  139,228,159,240,135,105,89,51,49,3,100,227,253,23,169,172,190,93,45,11,143,75,71,100,
  146,5,138,93,90,135,42,28,217,175,122,190,228,175,65,160,221,146,187,223,97,125,152,236,
  46,6,202,91,172,74,34,58,134,94,235,225,85,109,56,89,4,10,0,158,216,79,162,224,
  179,207,47,2,178,100,18,250,216,206,44,161,39,54,14,66,26,197,254,75,154,235,9,248,
  3,72,83,114,216,10,241,143,243,149,202,197,23,74,224,86,40,229,56,110,164,72,153,12,
  91,82,253,195,21,105,240,203,139,206,231,133,127,73,205,195,85,12,15,244,199,38,2,40,
  240,56,97,181,236,206,91,171,53,73,205,112,17,144,106,225,114,157,225,114,160,215,205,191,
  242,196,48,89,36,73,36,20,179,46,124,161,228,173,89,41,25,121,138,119,7,90,31,137,
  170,71,90,201,229,57,212,125,17,105,118,138,235,230,47,82,35,90,233,20,202,126,29,202,
  90,108,28,81,246,175,120,6,84,37,227,30,77,38,138,86,11,141,160,216,29,221,243,14,
  35,245,66,87,223,72,69,176,62,211,148,18,71,39,31,181,77,169,170,85,76,105,235,22,
  208,137,83,120,56,72,116,182,215,203,148,253,131,40,194,27,3,83,17,83,132,36,213,204,
  33,128,4,86,88,93,187,230,135,46,210,21,129,86,226,199,86,169,69,160,149,248,36,87,
  135,50,176,170,237,246,25,192,149,223,30,189,5,129,93,209,155,162,183,32,56,41,16,240,
  166,4,210,179,134,57,207,12,229,58,27,108,5,101,233,98,54,82,49,226,203,201,196,74,
  52,214,17,161,191,181,78,72,97,98,142,194,82,240,166,100,20,165,188,82,138,163,237,23,
  107,193,43,89,36,197,125,114,249,106,185,57,76,141,11,84,253,94,199,47,106,100,209,157,
  41,95,142,218,233,44,93,44,149,230,180,152,70,43,103,169,52,199,197,52,199,50,205,49,
  165,209,87,229,249,122,149,104,117,113,122,69,133,92,250,50,85,47,74,47,52,4,236,196,
  57,253,46,74,70,23,117,118,34,67,149,139,18,100,23,247,118,178,45,138,100,148,37,83,
  28,176,179,108,81,95,226,218,82,31,230,42,107,170,73,81,154,92,215,219,169,111,85,210,
  34,12,246,0,177,17,220,162,230,150,127,70,99,45,145,230,147,26,123,237,12,63,26,138,
  66,190,112,195,43,35,243,142,108,84,150,214,71,190,159,84,169,148,48,214,136,23,87,149,
  59,230,60,205,92,248,154,80,249,192,64,121,229,52,163,236,37,93,240,162,234,230,249,134,
  132,252,94,129,141,169,73,196,121,60,2,92,73,139,91,70,181,108,17,74,51,133,216,211,
  82,178,88,201,249,146,149,159,194,16,167,63,114,51,37,9,222,64,233,185,63,52,6,178,
  61,29,0,95,2,24,13,85,91,201,182,111,229,0,76,51,26,234,94,33,223,225,201,189,
  152,173,107,216,61,148,200,7,23,118,33,197,104,187,216,98,188,172,72,73,52,191,245,232,
  60,206,186,57,83,27,176,211,217,234,123,55,20,115,95,185,30,213,74,43,27,139,149,209,
  239,111,10,156,143,21,243,253,74,183,102,175,168,23,116,70,223,164,250,153,69,214,69,105,
  9,147,166,52,56,101,176,132,73,75,205,144,205,175,169,204,42,156,227,153,180,106,177,30,
  26,66,137,33,45,140,11,255,94,120,232,99,92,228,166,46,157,189,204,208,22,214,204,191,
  51,107,230,151,177,102,98,204,230,14,23,27,199,46,89,188,8,58,44,160,179,31,94,160,
  127,212,12,51,25,186,244,54,134,46,189,35,67,151,126,11,67,151,126,27,67,151,126,47,
  67,151,254,89,134,46,253,243,12,93,122,43,67,215,44,89,232,171,226,45,232,77,203,188,
  241,72,212,76,38,7,226,161,191,237,1,134,181,106,251,134,71,66,123,57,183,130,182,77,
  36,201,67,166,219,120,200,244,78,60,100,90,10,86,87,160,223,164,111,135,41,17,230,14,
  238,191,120,183,30,220,133,144,73,157,46,127,243,207,158,189,200,29,49,55,100,74,34,135,
  248,87,191,228,113,169,196,237,10,29,40,129,94,110,176,2,59,173,242,246,203,22,185,0,
  240,236,29,137,135,106,169,124,151,230,171,211,99,40,79,143,201,38,255,80,224,181,159,127,
  31,160,32,130,61,253,20,110,121,52,100,191,13,114,203,149,214,37,24,111,60,125,249,74,
  35,225,117,228,196,79,243,74,43,220,86,101,122,221,172,129,100,144,181,250,198,139,12,153,
  222,36,204,2,154,66,125,69,60,13,49,18,235,167,33,86,98,78,133,157,109,195,202,107,
  191,121,63,172,147,244,86,252,63,188,135,190,175,31,4,185,60,255,46,214,122,187,183,90,
  235,117,86,206,210,153,59,83,103,236,172,157,139,173,182,123,51,139,183,214,215,93,45,249,
  230,74,217,220,37,17,47,141,179,114,67,101,51,97,229,119,198,6,13,241,67,102,197,240,
  131,203,85,40,0,206,200,8,98,140,159,30,25,246,157,65,159,0,239,4,103,116,217,138,
  167,128,51,178,139,56,193,15,44,148,173,8,74,214,42,233,23,152,96,243,131,108,37,78,
  241,3,155,101,99,130,2,211,154,126,129,234,226,206,198,129,183,16,75,223,235,21,120,178,
  197,77,150,206,254,231,152,22,38,99,188,248,26,72,59,188,132,90,148,34,138,17,102,52,
  185,32,151,75,114,185,40,151,203,114,185,48,151,75,115,185,56,125,197,77,13,44,185,67,
  191,67,67,111,47,172,120,91,223,249,22,131,196,29,105,119,184,165,76,18,119,5,101,24,
  208,214,54,138,15,62,22,122,249,38,11,204,98,163,214,118,103,31,206,216,188,78,85,186,
  215,206,134,146,180,198,92,225,81,36,109,47,87,140,66,72,117,167,220,160,126,185,49,231,
  155,109,57,151,152,114,174,178,121,196,173,109,224,89,44,174,221,129,236,138,173,109,240,85,
  59,66,107,14,241,245,58,66,215,219,232,99,218,116,148,236,82,41,93,44,141,155,108,10,
  30,222,54,86,104,159,216,243,239,226,200,32,190,41,161,233,200,192,187,41,97,137,35,3,
  61,31,225,192,76,207,72,254,150,115,146,191,49,122,91,217,148,132,50,154,158,148,252,45,
  167,37,127,239,139,164,114,86,194,61,140,158,151,252,173,103,166,8,185,34,53,77,149,150,
  156,38,45,57,69,90,114,46,186,69,27,155,150,111,131,109,68,206,187,186,49,157,109,68,
  35,211,73,135,97,32,54,177,12,196,38,218,227,183,52,30,187,180,140,199,242,144,128,55,
  13,54,221,183,11,107,76,187,162,54,115,242,107,14,157,49,120,231,131,202,24,249,231,91,
  227,239,242,144,105,13,15,125,130,146,13,184,229,19,132,154,8,202,79,97,113,122,34,40,
  59,111,172,249,19,148,131,141,225,64,244,198,186,49,103,32,168,61,134,129,234,137,164,38,
  217,102,206,140,187,232,234,252,241,123,86,157,128,171,19,112,117,38,92,157,137,170,206,188,
  190,134,131,31,174,208,24,254,124,166,162,70,94,77,214,201,227,58,210,86,197,213,230,250,
  136,218,76,145,122,44,106,179,174,207,25,199,13,181,121,255,251,31,119,170,77,67,215,166,
  225,41,242,80,99,163,140,68,81,174,66,68,22,89,167,134,167,8,116,91,117,254,248,253,
  155,250,10,29,163,72,131,62,168,175,69,109,2,69,28,116,86,125,46,106,131,126,97,178,
  80,109,84,93,184,3,111,234,170,247,102,109,2,174,205,132,107,19,112,109,38,185,218,80,
  139,21,109,96,24,123,42,106,83,236,40,254,226,138,240,23,219,168,94,214,199,178,50,68,
  235,101,89,117,126,127,255,199,55,85,167,161,134,49,3,92,34,209,146,73,100,214,6,37,
  43,186,160,206,76,76,38,205,74,214,6,117,169,207,181,53,17,181,224,200,197,228,79,44,
  19,182,170,170,109,188,242,130,79,128,47,103,144,70,157,205,160,144,16,69,159,30,166,182,
  167,159,82,254,206,11,155,57,133,106,60,97,51,28,57,209,150,0,17,27,96,215,164,222,
  135,95,67,19,194,89,45,108,167,93,33,18,48,180,128,64,43,104,0,145,120,94,4,84,
  80,144,32,165,217,33,97,23,159,119,42,128,193,102,170,216,179,83,121,112,54,177,128,54,
  38,117,45,146,98,245,100,127,20,30,127,209,146,16,242,151,203,177,208,162,166,222,69,44,
  125,13,56,150,190,218,28,123,237,144,23,0,138,165,47,30,248,225,109,222,131,255,39,251,
  69,232,10,191,8,7,210,47,66,91,56,70,112,133,99,132,158,112,140,48,16,142,17,136,
  195,27,11,94,118,45,88,213,11,201,136,206,36,207,119,41,248,219,99,193,190,158,72,214,
  244,76,178,156,47,133,59,133,35,225,57,225,138,157,36,56,167,228,78,1,24,159,11,183,
  10,111,133,7,133,167,35,162,188,243,142,128,64,248,88,184,87,120,38,60,48,252,101,196,
  132,119,222,211,47,48,45,82,225,160,225,119,225,139,225,191,8,76,44,35,129,81,110,206,
  15,195,75,120,89,120,14,3,231,143,97,255,124,145,10,207,10,94,237,8,208,183,128,62,
  3,244,119,225,42,193,171,93,1,248,20,192,191,0,248,95,14,187,220,5,244,20,208,119,
  128,190,7,212,79,133,175,134,21,208,46,129,118,14,180,83,129,182,71,208,35,64,223,2,
  250,12,80,66,59,32,224,21,128,79,1,252,11,128,140,182,67,208,83,64,223,1,250,30,
  80,70,203,171,201,75,108,97,207,235,23,64,59,19,104,105,131,4,218,53,208,94,0,237,
  140,209,210,48,28,3,239,26,120,47,128,119,38,240,242,226,126,10,232,59,64,223,3,202,
  120,81,218,37,240,30,3,239,9,240,158,9,188,251,4,61,2,244,45,160,207,0,101,188,
  46,65,175,0,125,10,232,95,0,101,188,61,130,158,2,250,14,208,247,128,250,233,159,114,
  38,209,181,156,67,240,82,104,123,149,80,222,35,6,10,222,201,185,151,208,110,40,20,74,
  183,155,115,84,161,28,82,184,10,167,219,251,78,87,20,93,225,3,226,64,248,128,160,177,
  235,9,207,16,134,19,136,129,112,2,65,99,119,37,252,69,44,133,107,136,57,65,91,52,
  159,82,30,187,99,225,70,98,45,60,70,92,16,212,165,249,148,242,140,185,196,70,52,197,
  14,55,199,148,138,248,107,137,89,21,213,230,204,95,157,97,48,78,121,255,123,137,47,130,
  173,48,185,164,219,228,204,251,69,101,93,187,108,92,192,9,200,172,118,82,37,219,255,99,
  14,159,33,252,178,10,175,19,8,31,99,187,164,240,81,21,46,17,17,62,65,248,37,198,
  204,81,117,83,114,42,248,70,2,9,250,244,4,125,6,130,62,109,65,159,190,32,15,189,
  48,200,170,75,222,58,176,17,115,85,99,254,14,184,154,228,173,3,91,118,181,232,97,34,
  95,21,141,76,20,207,174,25,82,97,43,157,165,22,190,168,1,59,106,224,122,10,183,11,
  12,239,75,199,12,92,85,250,35,225,29,233,206,65,244,56,255,149,49,251,210,129,131,232,
  110,254,43,99,92,81,56,151,206,71,98,158,146,190,62,26,103,71,239,91,183,229,204,164,
  127,231,65,133,87,76,113,10,224,210,216,77,33,227,38,109,189,161,138,151,177,190,140,83,
  126,248,254,181,190,78,152,110,214,40,119,197,40,119,219,98,152,19,145,46,36,65,178,113,
  14,247,44,232,111,15,227,220,135,131,17,242,188,121,130,47,242,245,237,209,254,193,176,8,
  227,60,100,88,68,155,8,127,5,216,71,86,181,117,99,89,27,99,43,89,213,46,192,62,
  142,177,153,172,106,179,198,20,95,111,225,16,133,96,107,108,41,75,134,173,177,171,204,249,
  235,2,251,202,101,237,93,67,172,110,111,177,142,61,111,188,196,186,118,84,187,162,227,207,
  227,187,57,98,41,115,203,242,12,7,213,199,150,143,16,56,95,121,135,9,250,20,123,198,
  219,106,237,153,118,209,242,20,173,126,135,118,72,32,185,105,89,215,142,48,61,95,98,58,
  158,9,32,250,176,50,175,189,68,27,143,80,107,9,36,183,45,1,42,236,33,251,164,118,
  42,128,228,186,197,7,32,193,218,30,43,32,6,115,229,2,83,26,219,70,99,86,59,22,
  192,125,246,250,114,4,202,156,144,131,32,1,20,238,92,158,54,104,131,156,212,174,4,112,
  192,78,95,158,195,129,206,83,212,83,2,105,195,193,58,241,18,235,196,9,42,122,41,161,
  212,166,37,138,194,118,136,154,42,168,240,61,115,138,170,190,5,9,20,134,14,215,245,45,
  218,127,138,217,173,160,93,38,192,49,42,123,6,34,40,12,61,174,237,25,40,112,140,29,
  82,64,13,143,36,119,60,39,219,27,79,40,55,158,68,110,60,177,220,111,66,185,223,36,
  114,191,137,229,54,19,202,109,38,81,219,76,44,119,151,80,238,46,137,218,93,98,237,146,
  230,165,39,124,59,190,14,233,225,214,141,83,174,70,127,234,52,44,106,244,167,78,99,161,
  198,179,79,44,169,53,250,83,167,46,174,209,159,58,245,107,141,103,160,88,107,107,244,167,
  78,221,85,163,63,117,238,163,154,181,182,102,42,218,250,185,135,120,227,93,45,243,38,98,
  57,46,203,22,159,156,200,85,121,20,105,101,62,69,88,248,212,42,245,47,162,60,140,40,
  175,30,184,80,40,113,10,162,142,56,191,127,143,187,145,76,234,43,29,143,152,174,71,202,
  203,249,227,251,220,154,180,100,229,69,137,92,214,205,229,188,255,19,238,83,114,173,184,177,
  28,30,107,198,169,192,244,108,106,249,119,37,201,27,176,123,60,81,34,158,40,1,11,148,
  38,228,14,153,76,29,214,162,66,133,200,61,16,172,35,178,127,100,92,29,176,35,173,8,
  53,1,164,78,144,21,126,113,220,146,171,56,193,197,218,189,226,148,158,131,53,27,95,97,
  105,11,180,59,152,82,121,167,209,179,234,107,11,154,185,239,89,23,46,101,23,6,120,150,
  199,227,209,117,60,70,153,148,93,24,200,99,244,246,253,25,36,244,201,227,40,121,155,134,
  179,81,114,55,13,63,163,228,111,122,124,73,100,172,195,233,225,200,171,123,216,25,163,122,
  132,141,17,92,12,182,69,240,53,216,20,99,48,113,107,22,35,92,176,140,102,70,210,25,
  102,250,38,216,8,137,157,59,25,9,70,143,206,167,216,252,208,65,71,248,171,87,50,62,
  208,226,237,105,101,93,159,85,171,112,250,198,103,88,60,117,60,17,1,90,121,199,141,99,
  17,96,185,2,31,124,43,211,6,18,188,20,231,92,202,62,231,236,47,197,209,182,114,81,
  191,20,129,125,145,131,182,133,113,29,88,142,196,121,24,111,249,46,69,192,109,169,252,235,
  170,132,184,34,207,13,12,138,147,152,50,141,169,127,59,145,15,233,65,129,56,38,210,99,
  2,113,38,164,135,4,140,147,119,92,121,194,45,225,86,105,43,143,205,173,60,148,3,128,
  237,77,250,134,72,219,209,90,35,232,77,49,76,248,192,202,7,83,34,166,33,138,142,178,
  20,196,238,18,67,68,196,51,82,4,89,138,131,143,76,56,65,48,83,76,125,143,69,41,
  16,237,85,157,179,208,80,6,226,74,76,192,68,208,36,116,247,34,140,30,119,47,80,189,
  142,164,218,245,16,109,68,19,199,132,184,5,72,187,0,161,125,111,101,65,122,5,72,191,
  0,161,93,114,105,65,6,5,8,239,140,203,236,250,35,247,70,249,140,236,1,209,211,115,
  186,167,128,32,147,164,72,65,54,113,223,192,52,203,146,124,189,126,246,237,75,202,209,67,
  178,202,250,147,43,134,199,36,55,7,87,120,159,29,239,85,124,120,235,2,165,248,27,158,
  238,170,152,109,21,31,182,38,100,204,20,124,76,29,94,215,69,28,110,58,105,100,4,85,
  76,195,189,138,215,128,0,156,166,162,252,62,84,162,195,8,35,231,97,136,68,141,138,87,
  143,171,58,101,163,93,43,79,252,57,161,196,94,150,48,151,108,139,205,12,121,105,220,204,
  83,1,54,213,158,195,186,111,128,107,222,9,57,183,198,208,78,253,157,100,13,181,162,11,
  178,164,17,233,29,99,194,194,45,103,242,129,229,134,19,22,231,226,71,136,21,39,31,92,
  241,211,35,89,234,132,133,150,4,236,8,168,76,211,151,73,73,154,64,191,36,142,197,175,
  76,180,47,99,129,9,110,117,39,60,137,91,89,223,189,142,211,121,116,142,203,242,249,98,
  242,141,157,151,117,157,238,184,134,221,115,141,45,93,231,102,93,183,181,227,218,89,95,24,
  125,88,210,105,25,178,198,183,118,153,213,248,63,213,103,45,217,103,243,178,62,107,201,62,
  155,126,71,159,181,100,151,185,127,210,141,154,219,255,33,126,212,128,230,255,118,71,106,248,
  86,183,242,92,150,190,151,231,144,190,153,231,144,190,46,231,144,190,48,231,80,118,101,78,
  161,236,134,30,250,83,82,115,232,33,214,88,254,240,124,231,229,76,128,148,17,248,51,21,
  150,59,218,106,33,179,196,158,252,240,66,249,241,124,174,144,188,208,190,167,23,11,173,205,
  178,221,50,85,102,9,251,245,241,227,39,199,218,25,0,223,83,106,85,159,177,182,240,52,
  214,246,161,198,218,64,212,88,220,193,8,251,85,87,121,237,230,43,214,228,190,202,212,183,
  51,92,81,120,52,39,195,65,71,94,16,156,65,141,167,34,180,199,243,106,32,227,53,163,
  200,116,104,199,235,219,80,92,231,81,8,19,90,215,25,138,235,219,80,112,163,242,104,24,
  200,168,68,180,70,39,72,112,3,74,67,59,103,100,100,201,19,235,46,116,222,90,200,86,
  93,152,114,243,75,227,43,85,150,42,201,44,167,220,12,19,213,208,200,72,1,157,155,2,
  22,10,2,240,199,45,53,46,231,40,180,123,89,137,35,36,107,112,138,137,52,217,116,102,
  229,60,113,247,16,137,187,135,64,220,57,76,196,157,195,74,220,57,44,197,93,195,92,220,
  53,76,197,93,195,161,52,168,229,11,131,90,124,31,62,84,173,19,87,89,116,164,194,43,
  255,8,142,9,49,13,171,142,190,224,138,96,16,99,32,254,121,160,72,42,243,224,173,115,
  187,210,192,21,109,85,83,199,140,192,97,169,170,140,196,216,121,176,59,100,89,90,166,145,
  63,190,27,31,218,89,168,106,13,212,109,85,168,219,170,88,183,181,89,78,180,165,106,228,
  11,94,215,204,202,129,11,211,120,91,213,232,162,188,164,106,168,217,188,80,179,249,45,53,
  67,57,91,169,102,213,173,85,94,127,80,214,170,26,46,205,75,58,148,168,182,44,212,109,
  121,75,143,206,183,82,205,234,207,214,246,234,91,29,250,94,87,237,218,166,90,80,168,89,
  112,251,88,211,3,167,216,109,37,117,43,140,6,171,106,116,155,94,82,53,162,154,87,168,
  155,119,43,213,202,171,22,109,157,5,230,204,89,103,99,77,153,167,35,79,19,188,67,145,
  111,226,210,197,163,186,3,85,114,122,66,234,199,112,156,227,133,176,133,252,41,140,46,67,
  177,110,19,175,230,91,118,246,212,138,69,143,110,12,179,146,183,45,89,230,61,189,37,219,
  120,62,111,222,120,167,239,152,206,171,115,117,127,62,103,84,170,12,41,159,203,175,135,229,
  106,99,108,68,139,84,196,168,117,122,143,146,41,95,204,85,137,82,13,41,123,179,84,108,
  207,139,57,221,99,151,88,0,24,107,19,0,99,182,1,48,214,70,0,198,108,5,96,172,
  205,0,140,217,14,192,88,169,104,152,91,168,193,67,22,55,24,230,241,212,6,243,65,89,
  3,160,13,70,112,121,157,143,230,195,54,107,195,65,92,245,150,221,230,14,150,3,198,182,
  233,128,177,109,59,96,108,27,15,80,91,220,70,151,88,104,83,190,42,234,118,178,164,142,
  95,110,125,182,63,182,222,237,143,173,135,251,99,235,229,190,164,245,230,112,145,99,243,132,
  218,23,235,60,159,149,121,134,210,78,20,146,79,208,168,230,177,101,130,42,238,189,123,41,
  124,22,221,191,127,191,181,17,190,111,173,4,95,71,34,129,140,123,24,4,22,198,134,139,
  62,56,63,207,101,250,187,202,132,247,11,5,140,63,143,254,31,85,168,138,207,99,109,109,
  132,99,60,69,250,74,150,57,229,159,42,6,77,107,3,214,86,56,235,45,77,169,10,17,
  105,55,206,19,114,79,243,120,46,185,236,48,81,188,245,123,205,180,123,138,23,255,85,49,
  244,167,146,109,119,46,103,42,249,171,185,230,228,153,179,127,59,215,156,62,107,161,190,158,
  27,156,63,56,253,167,243,145,124,204,1,229,127,159,60,181,62,156,41,8,20,75,162,207,
  12,91,36,10,54,153,47,130,169,72,234,240,183,120,25,236,60,14,172,20,42,171,149,230,
  233,173,62,119,10,78,113,196,195,171,206,227,111,240,138,243,228,142,94,113,248,105,238,174,
  42,64,2,97,229,158,116,251,179,167,58,178,53,161,246,186,178,90,26,39,153,183,111,244,
  99,31,113,43,35,186,67,60,249,90,44,156,80,117,75,98,157,175,50,119,79,212,228,208,
  94,42,125,7,82,142,204,3,164,135,20,126,201,162,25,58,98,89,226,212,126,54,241,73,
  11,51,52,130,16,11,150,17,110,161,52,136,190,44,229,5,242,144,122,2,254,47,87,49,
  13,90,178,36,225,135,171,11,95,135,4,117,211,77,102,110,253,78,25,224,243,243,159,186,
  214,119,203,18,194,187,23,9,241,239,150,26,239,62,46,224,110,56,120,135,183,78,210,113,
  175,28,9,98,222,108,164,153,246,98,220,171,116,179,169,22,156,253,120,150,19,11,248,100,
  13,166,37,112,195,11,134,233,20,234,244,248,249,239,227,135,111,79,95,99,104,60,126,120,
  250,164,128,234,166,124,191,189,62,126,241,248,198,220,175,76,131,213,230,19,33,49,190,206,
  18,245,4,51,89,128,70,250,80,13,62,43,61,153,123,211,232,82,231,138,125,88,216,194,
  115,62,27,58,139,87,73,186,186,56,194,240,39,231,61,45,211,25,198,107,222,246,90,218,
  243,201,5,15,130,204,29,209,4,89,163,11,126,72,246,146,250,27,158,184,228,206,105,197,
  47,132,251,176,210,36,226,41,76,246,226,38,239,6,105,185,248,28,137,201,185,137,194,71,
  62,132,65,178,1,180,153,225,206,106,6,156,38,64,36,17,47,202,140,36,25,32,111,16,
  181,180,131,37,131,102,251,254,177,28,157,216,42,120,86,148,113,53,193,45,144,147,77,164,
  201,38,134,122,89,139,81,95,21,181,218,174,151,153,229,42,43,152,246,80,147,23,212,214,
  62,13,7,65,6,6,185,176,216,169,242,56,180,42,249,150,220,50,222,145,54,212,205,172,
  153,93,209,45,121,11,6,72,243,24,74,108,169,26,88,164,228,96,195,139,146,186,35,55,
  141,41,133,73,121,27,243,61,144,121,238,14,19,109,245,77,224,20,19,247,187,17,155,157,
  148,195,93,176,83,110,53,227,21,243,197,2,244,199,141,41,223,26,41,223,223,152,242,53,
  167,76,229,53,125,25,193,30,207,21,81,155,133,129,104,183,76,185,0,147,99,154,159,15,
  34,119,78,129,207,175,42,110,88,21,90,104,117,190,58,175,236,74,254,113,91,250,183,118,
  250,247,183,165,23,68,8,34,84,144,189,110,7,5,171,221,98,178,115,148,28,152,63,241,
  78,235,88,126,34,186,149,252,2,93,221,92,210,15,220,170,18,242,239,197,250,222,184,199,
  179,176,55,149,222,85,94,25,219,212,174,240,60,213,129,67,124,106,105,160,242,178,107,240,
  57,135,55,215,234,215,220,51,153,76,213,44,95,47,197,174,29,121,180,61,75,43,17,139,
  228,5,61,208,124,128,214,200,202,254,10,173,97,79,177,81,213,97,22,1,232,175,137,142,
  112,238,182,84,188,199,0,36,51,27,64,147,127,142,148,171,93,152,220,128,226,230,233,154,
  81,188,170,172,187,227,214,2,175,147,87,44,19,84,174,77,92,195,199,18,174,38,252,123,
  249,20,108,193,64,108,157,192,162,99,113,14,180,212,53,180,160,64,30,98,31,84,30,67,
  0,161,152,84,202,59,220,137,56,180,3,223,204,255,153,194,215,252,14,115,227,59,240,198,
  228,237,48,183,74,86,242,23,105,226,7,179,38,184,218,236,173,25,123,98,213,28,245,3,
  54,43,66,92,58,209,229,13,143,7,218,214,20,11,44,212,203,77,22,88,184,146,99,67,
  55,246,107,240,167,115,56,69,151,233,180,15,94,43,5,29,254,117,10,126,240,59,220,218,
  46,242,244,4,41,138,242,51,74,173,17,119,101,58,117,214,170,141,104,193,29,122,69,92,
  24,21,122,69,95,31,73,68,89,199,132,185,142,49,110,184,52,65,216,55,193,235,153,161,
  114,67,198,3,26,46,219,108,217,122,148,104,194,117,47,249,179,243,89,69,36,71,169,135,
  179,42,206,82,55,209,242,113,96,164,200,30,79,111,138,157,105,106,138,201,9,175,90,100,
  217,156,17,36,212,18,154,141,124,217,109,111,34,34,85,179,217,180,218,130,73,1,99,155,
  147,249,157,87,186,219,151,54,61,4,127,50,12,224,8,200,141,104,51,70,72,165,54,209,
  151,172,177,4,250,238,25,80,82,19,151,248,159,31,49,57,180,234,159,24,241,143,214,207,
  11,251,147,17,169,79,194,124,8,78,171,102,198,87,88,143,239,148,149,78,198,249,204,217,
  17,155,184,3,105,186,228,67,250,17,75,147,111,170,180,154,198,156,212,149,147,166,152,154,
  103,247,18,158,107,95,132,102,140,149,134,236,58,109,43,150,238,232,189,130,155,52,111,147,
  213,51,177,115,56,116,82,23,108,162,172,42,185,116,19,253,166,181,98,114,149,212,13,160,
  227,141,151,185,132,142,97,6,48,102,151,208,177,81,195,66,129,122,242,135,84,43,30,18,
  230,123,232,59,50,0,119,220,105,117,17,219,207,7,219,11,201,33,51,14,39,180,251,66,
  156,68,190,148,84,1,39,74,47,246,7,224,190,156,241,146,173,113,63,94,196,254,36,53,
  89,250,109,136,205,39,194,38,250,162,110,187,120,247,45,223,185,11,253,218,170,249,160,11,
  214,80,215,116,16,166,115,31,155,35,75,168,51,159,138,19,47,1,193,66,144,155,40,33,
  14,52,70,139,191,101,180,136,225,238,231,135,184,79,227,217,192,165,241,190,19,167,114,211,
  29,153,58,168,143,72,179,69,180,228,240,71,21,157,21,167,107,240,16,251,105,34,222,228,
  89,42,27,114,115,240,141,21,151,52,59,253,102,89,198,234,77,94,112,191,249,176,123,104,
  238,81,116,188,231,71,195,150,51,119,159,149,0,67,161,0,200,202,127,177,57,30,140,161,
  64,202,17,245,145,223,136,217,41,73,35,102,167,36,141,152,157,146,136,55,82,245,17,133,
  93,17,219,19,177,3,25,11,213,232,81,210,136,217,17,73,35,102,71,36,141,88,56,34,
  217,220,40,136,105,89,20,209,231,137,111,16,38,84,110,192,15,110,186,170,238,30,202,5,
  73,166,197,50,185,119,9,250,61,40,76,155,252,222,91,29,22,146,148,62,52,44,217,82,
  205,41,95,173,222,42,170,162,43,245,239,30,211,101,244,221,20,151,12,177,254,43,37,49,
  251,168,67,195,74,82,42,209,227,252,110,11,218,159,235,189,240,127,65,239,177,66,132,35,
  234,42,136,27,111,233,69,15,189,24,193,78,174,236,69,239,94,116,232,161,23,227,15,222,
  199,109,60,19,93,64,126,163,87,85,54,34,117,232,19,217,70,48,245,28,193,53,106,12,
  137,246,16,130,194,11,41,88,228,128,50,198,70,223,108,68,136,191,146,185,183,148,95,159,
  176,120,164,144,98,82,32,147,105,82,40,132,76,153,62,240,213,84,158,87,71,37,158,87,
  5,83,96,57,94,213,7,21,229,121,85,113,29,168,179,188,147,200,188,176,42,119,207,218,
  169,168,114,147,173,29,179,90,242,219,156,60,215,201,75,114,139,194,93,39,219,102,204,61,
  199,201,75,123,139,2,96,199,22,253,230,101,193,142,22,216,26,194,91,167,204,77,116,217,
  84,114,238,230,47,52,217,234,47,84,75,190,13,41,56,95,173,233,42,88,187,132,186,139,
  165,140,184,195,145,243,216,0,103,91,80,182,13,42,145,115,22,151,101,80,147,38,138,151,
  115,97,174,236,241,130,121,26,47,182,172,187,110,73,82,98,83,112,43,186,146,178,158,135,
  51,92,99,208,182,188,181,172,44,201,104,123,110,233,91,189,170,69,56,207,229,185,123,250,
  210,79,230,140,82,92,148,89,240,93,26,148,164,243,32,199,35,125,58,36,43,209,194,125,
  77,126,27,166,124,10,171,226,100,36,155,4,52,201,110,69,140,138,105,53,158,172,226,143,
  232,68,85,168,182,1,165,74,131,109,23,116,127,90,28,251,229,113,52,202,97,189,82,242,
  254,114,168,103,0,68,79,99,239,242,152,238,218,148,186,93,6,224,25,132,17,139,171,80,
  43,133,13,116,232,80,198,107,216,26,29,19,201,52,38,136,236,42,67,201,248,126,229,11,
  14,224,145,115,70,238,72,176,14,62,138,174,134,81,211,8,61,176,66,154,72,67,121,201,
  162,226,78,150,115,40,171,24,89,5,224,65,30,144,71,176,169,86,141,94,54,234,106,130,
  236,186,138,44,222,103,111,17,208,181,157,26,69,207,167,138,20,101,81,106,80,154,57,159,
  41,114,20,115,26,81,70,206,16,107,255,115,18,208,156,164,94,44,7,234,216,6,202,84,
  112,230,154,22,147,25,80,163,131,142,178,81,63,182,96,188,234,92,61,215,131,52,75,150,
  7,139,148,2,125,46,157,1,84,248,166,133,68,25,204,26,56,96,181,133,239,233,194,248,
  209,49,106,101,196,188,183,204,216,142,115,208,204,243,55,119,247,148,79,107,118,142,28,212,
  200,33,226,217,34,117,146,55,157,92,145,142,176,115,197,91,176,12,149,196,101,143,73,19,
  151,29,51,42,73,157,173,19,54,46,204,142,50,68,0,143,242,233,140,165,198,188,183,135,
  205,59,125,52,142,62,4,210,85,181,177,22,103,192,192,108,146,0,106,75,176,139,228,4,
  142,204,125,33,132,36,189,156,115,246,53,164,24,194,12,66,210,117,34,211,3,19,56,202,
  39,210,19,182,152,91,146,87,180,247,86,12,210,233,187,162,155,31,126,94,196,17,27,62,
  148,60,176,1,49,145,151,196,149,24,223,85,70,150,173,164,163,66,222,92,93,244,115,11,
  73,57,90,212,141,187,134,208,215,129,55,209,130,140,239,125,201,230,199,136,28,74,103,156,
  162,50,252,40,34,5,127,38,23,51,5,36,166,25,236,23,9,10,232,240,16,25,251,108,
  212,20,44,100,38,152,98,174,53,24,169,8,202,192,108,130,178,26,13,165,202,170,98,145,
  39,96,145,225,68,81,177,200,147,123,171,195,137,18,141,145,87,197,9,94,46,144,101,56,
  198,228,44,171,194,4,145,1,130,249,2,115,252,124,90,132,161,177,239,157,97,106,190,4,
  243,42,123,86,134,28,249,105,236,204,89,56,207,5,41,182,216,100,45,184,124,197,46,219,
  201,104,149,80,223,35,43,198,26,67,138,51,207,168,86,160,146,149,174,42,72,18,145,107,
  15,69,187,128,158,160,140,172,100,138,142,193,189,201,97,0,58,70,66,218,71,245,213,103,
  1,187,2,31,130,143,152,203,89,120,20,9,18,27,144,27,114,11,179,102,37,66,206,251,
  45,26,112,134,194,80,206,167,72,169,96,148,220,140,20,4,204,150,116,20,30,67,178,121,
  32,238,72,50,67,210,217,153,197,170,132,165,158,81,86,141,66,62,174,136,28,197,185,20,
  168,193,161,149,67,211,55,131,97,76,138,65,233,171,78,243,172,249,70,79,25,61,147,166,
  100,70,142,0,234,132,70,134,228,40,44,78,105,244,142,203,211,3,158,222,114,121,230,240,
  163,215,92,158,85,62,30,116,49,132,79,109,213,195,200,60,66,132,70,61,70,17,173,192,
  86,164,174,210,8,55,41,19,59,78,123,151,129,86,241,202,142,18,21,29,173,80,85,59,
  66,212,121,132,107,204,121,46,66,85,127,132,43,129,169,29,103,116,24,238,43,199,118,36,
  55,106,52,214,90,196,161,28,6,120,159,17,30,26,138,98,145,234,64,163,207,39,184,72,
  219,145,17,171,81,68,139,203,148,170,225,239,172,244,177,22,20,225,46,93,233,18,2,229,
  90,59,189,179,241,121,186,60,16,1,199,103,41,129,26,162,226,52,43,142,178,234,212,37,
  77,195,235,187,94,173,66,32,35,84,88,70,43,141,179,166,122,235,97,3,156,114,165,144,
  162,166,2,139,28,51,163,246,36,127,52,21,139,82,235,248,247,64,6,149,134,223,48,187,
  201,211,194,78,70,36,133,40,206,22,113,77,201,69,121,241,132,92,60,30,223,32,49,74,
  203,225,55,10,113,210,45,17,78,254,244,140,148,230,89,218,18,83,167,90,120,144,87,43,
  75,77,153,68,137,130,89,154,147,73,148,104,155,165,57,225,67,65,245,44,181,132,15,166,
  194,88,170,132,15,121,221,52,115,137,208,71,3,91,183,140,86,86,218,234,33,180,230,79,
  33,97,90,204,120,124,42,97,131,150,125,229,252,150,20,239,185,212,42,74,101,27,23,92,
  135,250,234,63,81,227,201,122,20,240,205,230,214,15,159,250,134,22,170,165,227,107,70,21,
  53,18,209,138,146,4,5,213,67,74,70,74,219,111,253,156,93,239,167,254,205,26,187,207,
  192,70,46,51,231,10,124,8,103,216,46,68,105,71,90,183,152,174,93,161,88,252,228,6,
  157,240,113,202,44,227,177,183,54,46,209,199,231,241,98,105,134,161,229,42,110,77,233,114,
  233,23,4,216,13,78,254,53,29,167,202,110,55,140,156,32,220,91,223,192,85,156,156,74,
  29,81,68,107,221,74,11,250,223,204,112,106,69,69,1,92,132,203,85,122,66,202,87,163,
  47,240,138,132,225,16,158,147,67,181,170,145,136,106,125,170,218,89,90,245,140,10,185,250,
  27,228,49,27,161,193,55,180,36,75,83,104,78,22,53,247,152,169,246,226,119,62,84,165,
  96,221,160,44,81,96,167,16,250,207,37,152,160,31,183,10,110,70,229,229,146,48,174,106,
  62,25,17,236,25,198,64,41,173,120,112,228,200,36,6,140,73,33,130,220,64,28,142,46,
  208,133,161,55,144,132,227,111,160,134,202,191,149,16,156,160,148,6,102,2,159,22,4,245,
  176,96,164,47,42,56,114,99,175,26,233,182,129,148,243,100,148,245,64,62,191,129,59,151,
  135,235,178,37,57,13,235,92,114,158,14,165,201,201,188,125,8,94,70,221,138,146,70,10,
  165,182,238,39,57,255,33,115,151,122,1,38,189,32,145,180,249,89,120,132,170,86,149,224,
  131,215,129,191,210,148,172,240,243,38,219,87,107,201,202,42,43,65,15,22,132,147,1,57,
  71,137,162,70,5,111,197,147,37,47,160,42,163,127,101,251,108,44,165,125,165,116,128,150,
  210,189,82,186,100,201,86,173,164,15,8,211,88,10,47,169,177,248,241,196,143,146,238,233,
  250,57,193,40,171,129,58,135,233,206,65,223,209,67,133,132,46,72,68,153,139,0,99,152,
  23,64,212,107,87,86,163,113,22,172,98,60,149,219,229,14,159,100,29,238,209,158,163,250,
  119,93,214,191,92,213,139,17,57,40,75,185,123,223,208,150,185,38,23,34,179,81,73,231,
  79,28,28,237,47,50,154,204,20,231,150,189,9,187,16,202,161,228,195,77,51,114,179,162,
  242,246,44,187,204,158,105,238,19,159,146,137,156,109,191,27,70,28,111,14,199,222,116,177,
  74,70,23,240,156,74,31,148,71,245,139,170,225,70,48,233,19,185,155,124,216,101,253,178,
  6,94,109,156,251,113,3,215,122,187,244,178,56,139,78,231,171,139,51,9,159,143,86,25,
  23,61,149,74,246,167,81,101,105,240,210,211,81,179,213,198,65,169,217,106,245,14,39,198,
  246,212,84,187,211,207,63,207,239,195,132,244,131,74,105,172,94,171,74,199,62,167,2,139,
  176,235,80,167,249,211,16,35,97,40,122,80,4,28,49,142,88,52,5,177,240,240,167,109,
  85,184,55,154,54,198,232,174,45,149,104,221,86,137,132,100,182,119,173,134,56,250,7,134,
  66,220,185,218,93,72,234,33,6,27,143,51,35,134,70,156,113,95,17,20,199,85,92,50,
  174,130,226,184,10,178,113,21,100,227,42,80,227,42,184,97,92,197,185,221,230,1,144,149,
  236,79,45,58,239,90,48,113,46,201,103,71,135,4,229,251,91,156,223,149,100,73,133,157,
  140,138,202,37,85,101,229,192,170,176,146,205,48,176,183,184,159,127,14,202,123,154,250,66,
  16,67,175,180,185,158,133,24,192,144,241,90,29,153,90,28,23,245,102,162,217,6,243,30,
  106,132,199,84,153,182,64,37,42,246,115,82,210,207,81,177,159,163,172,159,163,172,159,35,
  213,207,209,13,253,156,20,250,57,42,239,231,168,180,159,147,98,63,71,229,253,156,20,251,
  57,218,210,207,81,121,63,39,37,253,28,221,196,244,216,61,123,52,171,102,167,162,200,36,
  187,90,41,85,95,96,102,152,179,79,69,199,58,122,146,69,79,116,180,167,163,197,147,88,
  107,175,208,74,142,106,121,245,197,7,169,80,102,114,125,101,54,70,240,147,135,97,41,27,
  25,90,220,99,41,190,144,212,81,113,26,52,152,147,178,116,56,50,189,91,141,190,120,116,
  114,61,163,103,107,110,111,127,208,238,28,116,160,172,144,46,254,185,242,47,231,120,186,59,
  116,251,221,110,183,179,223,115,60,136,60,134,253,94,175,35,62,47,60,156,101,253,225,65,
  231,224,160,215,135,25,244,107,242,232,76,40,186,46,18,159,193,127,18,229,117,221,65,187,
  143,203,193,69,242,79,42,161,191,191,223,106,119,187,206,89,128,75,1,248,140,198,47,45,
  236,83,47,184,136,194,41,199,183,91,93,100,167,250,180,123,226,227,243,34,2,97,134,131,
  86,175,215,198,30,115,6,99,71,225,208,109,29,180,187,237,14,80,173,226,96,125,25,69,
  200,221,237,13,250,237,14,76,63,121,48,179,198,40,250,237,126,191,215,62,192,75,81,172,
  218,177,143,51,55,87,184,211,107,3,20,225,106,200,163,22,118,246,15,6,221,125,242,96,
  29,123,1,85,162,219,109,239,183,41,136,171,236,232,210,143,5,174,222,192,29,28,184,12,
  78,22,193,39,174,109,15,216,156,73,188,184,72,160,184,225,34,95,199,109,1,209,218,11,
  37,169,166,94,252,73,80,183,51,224,0,199,117,122,251,237,14,7,207,163,0,246,221,98,
  170,126,187,53,104,15,100,42,152,106,90,15,93,252,51,104,185,251,18,226,251,33,104,210,
  7,126,25,206,165,248,52,247,62,45,128,166,219,233,180,123,2,13,201,17,67,44,95,3,
  183,53,232,119,69,137,81,0,81,137,192,214,235,13,246,97,237,64,128,99,186,47,230,214,
  239,131,206,18,134,109,17,53,107,181,186,173,150,219,102,24,184,43,70,215,107,117,57,156,
  112,223,161,231,59,173,131,174,43,242,37,190,39,10,192,96,24,128,106,2,72,196,102,82,
  116,247,59,221,78,119,63,131,114,107,137,114,221,65,207,132,250,54,20,99,249,159,171,104,
  129,78,236,181,7,93,1,83,131,163,63,24,244,136,118,190,79,78,130,185,115,220,254,128,
  10,1,36,249,180,22,5,15,220,30,28,148,47,46,184,192,254,0,99,168,223,19,97,223,
  8,71,211,115,217,231,237,86,171,131,22,56,51,220,71,158,225,22,19,120,137,64,110,183,
  15,103,198,52,90,212,28,193,72,24,128,104,244,36,48,73,101,87,181,251,157,131,110,219,
  153,173,192,53,44,60,174,145,59,192,144,56,247,112,165,125,22,197,17,13,24,140,53,204,
  143,243,121,148,164,10,87,199,237,35,169,67,35,131,50,33,0,204,198,56,233,118,218,3,
  151,64,212,8,148,224,82,87,136,50,59,237,253,254,129,248,94,251,1,198,46,234,219,109,
  117,48,115,28,110,162,74,61,135,148,104,61,133,107,118,49,97,81,131,121,148,42,186,117,
  14,246,187,45,135,46,97,189,144,122,219,237,116,123,7,189,118,151,65,231,17,83,177,211,
  65,138,207,81,188,230,182,163,130,45,71,14,191,222,254,1,170,140,237,26,218,157,36,81,
  3,196,237,180,105,100,40,8,40,155,204,57,95,167,3,114,7,222,101,40,106,127,128,177,
  60,216,239,59,208,185,140,136,33,155,205,104,96,17,109,177,198,56,1,61,248,17,83,9,
  115,9,67,188,43,64,114,214,246,246,251,168,86,95,194,104,146,185,32,46,70,248,64,128,
  52,1,21,97,176,174,29,180,169,90,28,203,243,13,147,185,221,193,196,148,32,49,130,7,
  7,152,116,26,148,79,165,136,214,59,232,246,101,29,213,140,0,16,221,209,150,64,53,37,
  218,110,183,125,48,144,197,170,129,9,64,171,211,149,165,100,83,98,255,160,131,149,183,99,
  129,253,60,56,245,253,64,146,5,149,192,212,18,112,221,76,116,143,123,64,192,11,90,195,
  218,7,45,254,148,227,5,67,137,186,146,182,241,144,73,210,235,99,33,84,203,134,30,178,
  88,236,35,52,137,214,206,126,235,192,185,240,113,204,184,48,118,1,12,154,253,78,187,45,
  35,228,212,233,201,160,90,69,218,109,151,70,182,132,46,87,49,156,75,98,226,98,141,198,
  158,35,128,154,74,157,193,254,1,198,130,2,235,165,227,160,117,176,191,15,234,73,248,146,
  228,169,34,71,191,235,98,68,8,120,182,80,116,49,54,59,45,149,94,44,22,98,76,183,
  186,251,238,62,202,93,76,195,108,96,129,0,152,90,0,134,233,4,166,90,46,104,7,107,
  187,7,61,32,192,177,103,13,151,29,106,19,163,172,209,100,66,230,100,36,164,61,112,66,
  168,141,252,119,164,215,132,254,65,31,227,22,64,12,26,108,66,24,128,216,246,40,6,75,
  113,175,71,0,172,196,60,39,59,24,245,28,130,122,207,217,112,191,213,61,216,199,98,150,
  45,201,88,218,48,225,69,152,171,143,53,97,208,193,70,170,104,219,237,96,2,160,235,151,
  96,249,140,165,162,215,239,237,163,169,2,204,100,194,114,218,198,116,18,160,140,78,24,59,
  237,1,250,130,193,6,153,186,157,3,44,53,29,128,151,222,218,67,203,150,98,226,182,246,
  247,157,165,143,199,57,75,120,57,229,182,226,95,36,243,227,21,173,23,253,3,44,251,142,
  154,27,125,183,133,49,180,132,31,44,218,163,219,221,126,7,153,163,203,169,92,100,81,54,
  246,8,204,68,57,36,104,148,237,99,38,99,201,245,65,97,9,237,247,49,36,176,253,202,
  230,99,40,161,17,232,144,181,228,7,218,216,83,123,216,106,226,104,237,137,249,128,121,214,
  167,109,34,1,51,20,248,34,25,122,23,179,97,223,209,115,20,139,31,166,51,194,225,84,
  97,234,183,58,200,217,117,178,193,216,234,1,180,79,128,100,142,105,197,36,64,43,14,156,
  100,225,135,33,230,9,18,244,247,49,92,193,23,124,166,37,15,75,127,155,86,13,107,126,
  131,51,201,6,50,90,211,106,245,37,68,76,246,14,250,20,93,106,204,115,5,9,229,68,
  238,13,208,151,214,160,239,117,91,40,85,47,1,221,62,152,8,208,37,165,229,175,67,147,
  133,2,62,214,71,52,105,208,103,222,52,5,49,177,6,97,140,129,117,193,171,121,232,149,
  242,170,191,143,61,221,49,102,78,187,135,129,223,119,228,6,139,161,132,173,248,160,239,92,
  194,118,113,202,156,93,135,90,148,109,128,251,216,90,68,48,185,136,62,41,230,15,19,192,
  88,137,250,3,236,12,34,172,134,35,70,68,107,191,187,193,46,60,250,50,7,75,152,224,
  255,96,216,218,56,145,103,3,178,219,200,87,65,101,225,152,207,138,125,182,83,235,215,201,
  225,143,127,159,158,200,249,13,254,190,231,238,245,31,44,234,149,180,177,168,214,250,53,88,
  140,0,164,253,32,197,111,123,175,99,196,84,16,108,248,213,225,98,67,23,19,175,211,146,
  155,131,18,171,207,82,109,38,51,102,160,92,171,158,171,143,51,245,145,61,149,205,108,232,
  241,153,192,55,244,122,194,220,89,0,23,61,135,9,105,146,43,253,28,121,73,74,199,33,
  168,225,8,213,101,156,46,119,97,186,226,204,143,119,181,241,243,95,252,171,92,10,169,220,
  156,121,226,60,73,215,120,192,145,84,13,15,193,244,56,254,217,163,220,195,35,245,238,63,
  115,153,98,61,221,83,207,206,206,213,199,153,178,10,36,171,65,87,182,175,178,174,74,133,
  125,44,112,79,68,82,117,7,59,170,164,247,239,187,253,159,193,242,87,247,136,239,151,56,
  9,124,144,131,158,1,154,129,126,79,133,214,23,31,182,249,193,53,84,86,206,197,173,177,
  111,88,45,80,237,194,97,28,89,46,69,178,35,157,115,107,171,124,85,104,120,99,81,73,
  86,212,47,39,47,110,41,138,159,11,188,152,176,243,64,80,231,90,24,107,39,183,139,244,
  29,138,111,159,77,66,171,234,72,18,203,170,176,178,146,84,149,15,239,141,154,189,7,112,
  69,226,194,22,216,16,182,73,225,176,0,246,124,96,101,24,47,76,14,101,126,204,24,216,
  234,115,210,186,187,215,169,170,166,41,160,82,95,211,128,6,165,82,103,199,59,182,90,12,
  39,213,217,122,170,146,171,190,47,177,161,235,195,183,161,79,131,200,75,17,115,15,83,149,
  140,160,49,113,134,59,15,131,229,28,239,140,73,172,1,118,36,37,41,237,110,61,173,239,
  238,92,46,130,128,94,35,67,153,25,12,246,180,185,91,101,113,38,171,66,37,163,189,191,
  87,254,118,89,175,254,173,82,249,240,247,191,85,63,214,170,127,171,238,225,213,182,63,33,
  157,56,65,37,182,171,232,10,187,138,109,109,44,209,147,198,18,227,243,179,221,161,250,242,
  118,135,108,144,123,239,239,127,131,79,151,191,77,129,56,169,57,91,190,43,15,134,50,88,
  251,91,243,129,4,86,31,252,69,150,31,85,181,77,239,10,61,24,202,172,148,209,120,20,
  78,14,96,142,142,198,49,211,229,57,100,5,244,130,200,113,97,187,137,135,247,182,52,237,
  59,164,233,24,105,124,105,102,220,104,213,127,88,109,41,134,126,76,219,232,56,92,210,54,
  2,111,75,211,190,67,154,142,145,198,183,76,254,205,147,64,118,38,190,242,157,105,180,36,
  223,182,255,216,2,251,110,50,208,58,96,142,118,180,189,186,215,233,203,102,40,32,140,182,
  239,233,182,41,96,231,99,174,101,166,197,64,57,89,222,74,75,128,60,57,119,216,74,17,
  205,22,92,223,43,77,70,154,25,127,251,63,149,15,15,27,79,189,198,236,111,211,143,245,
  234,95,242,19,67,76,11,47,123,84,67,111,98,177,242,116,242,126,101,168,83,51,250,55,
  73,160,2,219,10,120,184,227,246,81,87,115,220,169,56,247,134,184,182,142,147,207,112,81,
  100,63,95,36,237,30,89,86,202,64,169,13,18,40,35,214,115,255,74,146,129,9,160,53,
  57,73,224,174,53,215,114,200,25,133,120,194,76,88,115,155,157,21,203,43,154,16,205,189,
  91,125,32,205,209,23,36,20,58,242,72,165,66,91,130,206,20,26,173,93,24,94,49,170,
  219,187,141,234,251,141,230,110,165,118,202,185,92,180,203,237,219,178,66,139,222,152,117,98,
  124,158,101,142,149,79,208,167,48,38,194,178,218,34,130,112,65,70,28,244,102,193,193,115,
  189,85,112,240,172,154,33,19,104,78,163,19,222,105,243,200,198,137,133,140,131,25,50,14,
  102,200,248,173,190,85,185,188,239,200,124,221,13,243,2,34,183,85,155,98,110,187,178,89,
  238,115,201,178,152,12,75,182,133,159,70,217,38,94,249,205,55,220,33,208,30,202,43,21,
  171,57,147,229,83,68,199,53,90,120,91,14,177,42,96,49,33,13,236,215,11,137,206,173,
  68,56,50,20,147,156,153,73,100,21,79,152,155,179,42,90,217,109,241,63,24,82,242,37,
  62,55,165,138,225,42,83,99,2,85,165,162,82,163,47,48,9,86,101,11,155,114,167,150,
  203,87,182,225,136,90,12,142,135,218,132,171,94,170,183,114,251,75,126,124,216,214,190,118,
  253,75,11,186,132,176,115,119,184,95,170,123,216,168,14,165,185,123,175,42,116,113,91,25,
  163,3,157,205,70,164,54,238,201,104,197,92,207,18,86,240,97,173,126,136,223,118,3,241,
  85,71,110,233,59,225,48,128,247,42,88,200,223,91,214,43,201,189,248,65,127,104,153,250,
  221,73,40,65,220,8,41,65,219,140,136,41,130,204,248,35,162,43,34,54,193,222,168,159,
  201,210,231,228,17,162,9,229,34,252,13,96,29,159,159,158,75,6,243,102,230,242,142,20,
  37,219,17,130,158,152,37,130,162,152,31,130,166,92,152,100,182,184,247,239,56,62,229,243,
  88,198,26,10,156,220,89,103,166,217,142,87,254,131,127,240,194,84,249,203,151,116,179,243,
  151,47,244,198,225,233,226,10,102,38,193,16,34,28,230,194,137,25,174,254,99,248,15,48,
  80,200,107,140,97,159,70,111,117,227,88,192,176,12,152,8,96,245,31,155,104,54,51,249,
  232,130,137,9,138,153,46,236,157,118,186,104,206,235,41,78,145,205,164,238,211,15,92,248,
  104,171,58,214,98,84,55,214,198,186,177,56,214,179,213,17,217,152,148,182,241,170,108,101,
  173,251,214,234,90,247,173,21,182,238,27,120,182,28,155,234,250,132,161,191,206,234,250,228,
  180,58,43,46,161,122,42,169,179,102,195,92,81,243,177,231,13,115,129,205,199,226,98,95,
  175,183,219,252,187,198,53,131,78,53,131,78,181,140,78,69,119,154,121,12,89,254,44,183,
  35,109,222,196,203,18,242,214,71,180,83,52,68,160,90,243,53,149,104,199,16,224,115,13,
  62,99,240,89,67,110,133,18,204,168,179,254,203,15,32,209,131,21,95,82,176,22,154,61,
  9,176,32,157,2,139,30,5,88,208,76,130,185,8,57,64,165,70,161,57,46,85,32,242,
  212,234,24,199,60,66,33,212,192,31,118,115,37,32,9,65,248,164,26,75,72,64,144,32,
  199,147,168,81,46,23,77,211,118,179,182,171,92,108,229,85,214,178,117,214,26,233,11,216,
  48,88,211,201,153,108,160,21,66,50,25,242,61,220,25,59,168,43,216,197,16,5,9,139,
  8,117,98,91,107,97,157,141,27,168,55,48,35,97,15,161,46,172,37,212,201,139,171,138,
  59,27,9,107,8,117,97,43,161,46,45,41,228,124,127,232,69,87,91,94,142,89,141,66,
  133,206,41,116,166,66,103,69,167,30,57,154,192,38,150,38,10,155,70,214,116,97,203,200,
  119,54,173,28,91,150,149,207,45,195,202,180,72,83,61,30,65,52,234,131,219,5,137,207,
  86,169,95,190,150,96,164,252,94,241,171,198,24,244,211,63,52,224,76,0,222,43,128,122,
  202,94,92,18,177,215,87,111,181,185,28,155,102,149,207,205,192,25,110,131,127,19,166,101,
  95,167,135,175,211,230,171,135,47,159,156,128,223,101,69,221,101,92,238,131,3,196,161,247,
  251,174,223,209,30,55,158,70,153,22,141,101,18,151,247,21,89,128,22,252,16,27,166,68,
  44,51,124,134,101,124,240,162,146,33,200,178,233,76,213,60,81,164,18,7,106,130,183,252,
  168,194,48,123,238,206,56,134,25,58,77,58,135,80,14,51,228,51,21,194,199,6,180,185,
  136,191,81,85,153,95,222,229,84,149,25,38,201,97,188,146,203,20,142,205,7,107,25,116,
  6,146,102,161,44,227,35,210,117,91,144,218,145,178,156,154,197,225,200,228,227,150,26,234,
  16,110,62,74,153,169,83,118,132,243,37,23,178,26,113,165,121,161,109,141,122,76,51,179,
  171,162,178,66,246,56,30,179,129,181,241,227,39,239,78,95,191,126,113,50,30,223,219,93,
  65,12,89,132,231,20,54,168,132,35,54,235,42,194,187,209,25,191,107,134,45,102,220,238,
  227,117,174,86,178,82,47,67,244,160,225,174,104,106,48,49,79,186,245,57,37,70,243,177,
  162,249,82,81,233,209,35,175,209,252,92,102,235,125,161,245,184,208,200,142,222,203,101,163,
  254,100,184,145,108,107,207,166,165,224,237,221,157,150,65,183,13,1,245,138,164,24,83,189,
  97,88,164,165,224,173,99,69,21,82,18,69,244,201,143,158,28,177,10,131,171,152,35,71,
  198,187,62,128,41,152,5,17,195,70,67,173,93,206,234,69,95,190,145,18,61,41,163,141,
  215,192,58,190,188,71,183,245,117,89,62,131,234,91,122,188,44,151,158,166,219,250,61,123,
  147,169,115,151,119,245,182,65,80,154,207,46,182,108,40,24,229,146,89,122,229,247,233,87,
  245,241,54,144,31,127,85,144,68,217,163,143,213,199,35,101,199,254,181,74,252,84,125,60,
  82,31,215,226,3,43,254,39,253,245,78,127,165,219,92,74,153,22,214,31,74,3,235,15,
  229,214,230,101,23,28,234,137,18,182,43,241,54,104,7,219,200,43,182,6,166,36,255,244,
  80,18,108,188,96,206,18,150,2,161,185,38,136,87,38,32,193,21,102,82,121,34,45,168,
  198,218,64,220,201,63,43,122,4,198,247,91,120,153,157,99,182,221,189,204,93,46,249,31,
  49,188,63,87,141,106,61,130,137,15,118,242,150,185,188,171,126,177,107,194,236,231,175,97,
  161,190,111,131,124,125,165,239,73,100,159,70,41,85,26,7,122,25,248,53,164,103,152,50,
  240,150,180,228,70,191,102,49,43,21,160,24,28,229,225,26,59,130,235,86,28,247,151,166,
  231,203,216,104,131,67,147,141,11,156,195,139,222,146,188,35,146,167,86,120,142,175,194,221,
  252,120,36,221,176,7,8,29,90,217,221,6,52,124,225,131,111,170,201,128,78,78,73,231,
  230,141,82,198,19,61,148,99,161,138,148,250,107,88,85,230,146,126,114,135,127,133,95,200,
  251,35,92,97,224,99,173,62,174,234,20,186,71,238,37,52,201,105,146,196,203,200,244,84,
  172,60,13,26,239,250,239,84,44,244,15,201,213,77,208,36,55,65,187,215,80,218,222,17,
  90,137,228,185,200,217,189,204,0,151,35,73,50,88,99,13,154,217,125,93,139,20,125,229,
  57,212,159,138,206,132,48,149,170,94,22,227,81,204,186,44,38,162,152,235,42,61,16,47,
  107,170,63,181,24,221,130,99,219,235,92,165,62,229,194,239,114,97,164,223,198,67,83,222,
  242,184,144,241,148,199,65,10,226,217,69,120,133,54,94,227,149,2,209,165,24,243,137,98,
  214,101,49,239,40,6,116,241,20,89,192,3,99,217,75,159,122,19,22,209,229,70,219,147,
  226,60,251,181,184,46,32,149,88,24,48,121,120,222,36,213,123,45,243,226,216,28,68,218,
  36,179,92,159,68,80,29,33,38,34,24,218,167,70,97,196,224,33,237,35,83,97,202,171,
  108,86,40,204,100,73,216,66,158,146,9,91,11,127,74,78,22,173,34,52,241,111,47,101,
  107,79,203,50,183,246,182,172,193,214,30,191,179,104,221,120,108,92,46,81,87,164,104,122,
  57,74,40,65,138,38,68,115,146,73,147,31,66,124,88,217,210,247,34,147,18,227,231,71,
  1,151,169,35,173,241,160,220,243,214,154,61,246,225,190,152,46,197,194,102,28,151,49,74,
  203,81,177,102,176,40,186,90,220,82,58,44,22,86,91,153,198,183,104,26,80,19,157,163,
  26,193,249,222,64,155,215,178,234,169,12,143,30,69,75,68,121,177,24,117,165,40,24,129,
  181,22,218,165,155,81,165,85,240,25,195,150,181,215,198,149,79,84,142,79,230,220,228,118,
  15,3,85,126,95,41,111,87,110,61,48,242,231,98,182,16,54,179,39,10,131,49,22,117,
  179,152,83,240,193,194,73,128,24,192,1,20,213,146,148,235,117,26,189,201,116,208,13,191,
  164,158,37,219,17,165,97,135,58,76,18,155,55,160,215,40,22,40,38,208,235,28,111,32,
  249,23,56,44,76,120,189,122,205,28,64,172,3,236,15,247,30,109,154,147,123,217,126,239,
  171,181,233,240,105,14,159,180,81,186,82,248,158,50,223,16,235,0,225,91,241,38,188,188,
  7,59,9,54,62,153,121,78,190,201,27,171,218,132,18,207,185,240,128,179,172,140,42,120,
  163,96,175,18,52,86,196,9,203,202,20,150,120,176,230,16,228,61,202,85,49,22,165,76,
  85,21,31,5,100,40,34,214,1,42,117,204,229,77,239,193,192,130,93,69,153,121,61,154,
  130,21,10,106,99,74,188,22,244,225,44,99,163,138,209,104,178,87,153,52,198,55,85,49,
  134,32,81,160,188,24,173,106,227,198,180,182,36,148,23,140,18,68,16,245,104,80,125,20,
  218,71,115,187,87,19,226,230,42,72,11,7,196,248,91,135,19,246,113,85,151,153,20,203,
  124,52,87,101,206,200,207,241,69,125,93,159,107,158,213,27,173,107,51,32,156,227,239,45,
  148,45,111,76,153,16,208,107,42,32,143,224,42,11,255,44,216,25,195,38,22,12,115,16,
  214,24,23,101,42,93,202,115,210,30,84,110,196,255,85,197,254,87,26,4,209,127,170,90,
  194,133,89,216,209,130,29,220,100,169,115,1,228,235,35,95,237,209,246,230,129,84,246,30,
  141,180,214,46,173,247,76,229,75,78,166,250,228,63,185,88,146,235,247,67,195,3,0,155,
  171,85,150,255,239,65,25,168,142,235,122,113,228,186,90,66,143,17,54,164,121,198,159,134,
  77,75,28,90,45,92,115,63,45,219,55,239,80,56,219,228,147,101,215,111,40,186,108,79,
  47,173,132,218,24,190,181,225,165,133,167,5,199,7,106,27,242,105,193,4,59,114,66,70,
  155,141,5,241,52,84,189,145,223,18,155,61,91,14,96,244,36,205,30,197,86,101,29,154,
  86,229,219,27,171,95,133,65,63,97,190,196,205,141,141,172,173,142,213,152,44,79,245,199,
  176,48,89,221,233,179,80,113,250,172,102,238,210,101,157,10,24,148,229,75,124,174,179,207,
  235,17,77,20,141,81,155,199,188,210,137,240,121,61,106,168,84,228,192,174,172,0,202,123,
  47,43,75,26,221,98,60,25,120,109,128,175,51,240,53,49,0,162,143,243,77,215,165,61,
  72,141,35,230,176,200,39,1,143,110,69,217,96,224,219,87,30,61,119,44,32,207,211,1,
  175,94,57,176,198,217,3,183,72,109,186,41,55,23,13,186,196,83,42,35,42,179,92,53,
  75,114,139,139,196,44,183,184,14,181,115,151,223,221,81,238,236,246,178,145,67,162,243,216,
  168,114,163,124,155,35,3,87,93,138,105,35,78,107,218,171,194,156,197,51,220,6,16,179,
  150,173,30,187,234,153,227,46,239,173,202,186,177,169,93,152,230,141,140,254,100,89,86,81,
  6,132,197,250,165,236,7,167,210,220,27,227,123,192,23,46,194,86,162,182,148,239,57,167,
  97,117,184,101,81,163,83,46,226,115,190,123,74,44,243,228,151,72,105,115,46,205,25,14,
  148,74,68,21,11,62,202,158,16,147,126,36,202,125,148,69,98,229,8,244,249,196,200,68,
  94,105,195,82,44,225,205,88,66,27,11,193,111,109,221,138,221,5,4,158,84,205,52,76,
  212,220,230,2,161,108,213,35,183,8,121,189,172,109,140,120,74,114,153,108,205,32,26,93,
  221,203,86,29,6,172,141,20,236,222,117,109,164,96,192,181,145,130,29,190,94,27,41,174,
  117,225,57,30,92,151,122,111,148,102,197,139,101,204,174,130,46,93,165,92,171,148,249,170,
  232,90,168,148,215,42,165,93,37,58,113,41,51,130,22,207,193,46,29,43,228,72,183,145,
  85,16,92,93,86,31,19,14,19,227,205,117,6,88,155,9,45,56,37,188,206,0,215,102,
  66,11,126,211,153,133,139,207,119,151,162,160,213,101,92,126,190,219,20,5,173,174,227,10,
  228,187,79,81,48,163,87,86,41,97,191,51,223,143,224,15,47,150,114,112,53,39,188,143,
  208,188,167,185,157,25,61,56,1,99,25,147,219,85,153,162,74,157,36,204,46,212,212,135,
  81,146,62,18,51,11,3,147,106,154,16,194,123,4,104,129,169,206,54,204,100,184,150,145,
  134,249,157,28,156,168,131,105,237,151,199,148,229,16,157,172,193,107,46,176,158,165,91,103,
  233,96,237,162,36,130,200,76,69,110,137,42,205,35,6,140,6,95,231,11,189,206,210,93,
  91,8,174,51,204,215,185,66,205,168,210,60,60,248,240,232,99,212,72,5,103,228,145,69,
  209,240,190,25,222,148,29,154,51,131,114,122,31,151,167,164,159,220,67,37,168,149,156,197,
  95,201,188,160,87,186,175,83,148,103,157,149,193,96,48,52,202,65,207,24,26,228,160,19,
  134,206,22,38,52,194,9,137,76,22,90,64,152,141,140,0,124,109,1,61,24,74,212,174,
  61,62,224,212,2,68,215,132,109,13,94,164,177,164,192,82,6,94,83,224,53,5,56,141,
  72,123,229,44,101,96,73,129,215,50,240,154,2,141,153,76,124,37,227,215,206,82,6,24,
  13,39,106,41,93,216,159,158,5,120,209,224,81,37,81,37,80,171,250,245,43,70,236,7,
  118,27,236,176,33,49,241,251,209,41,38,173,146,172,189,50,247,132,192,75,53,110,182,64,
  129,116,54,251,128,152,43,152,223,64,161,255,47,123,111,222,221,182,145,237,139,254,253,238,
  167,144,181,252,210,4,9,106,140,211,125,73,195,94,142,60,196,221,177,99,75,242,168,232,
  241,64,36,100,210,33,1,6,0,37,209,50,191,251,253,237,154,171,80,32,41,197,233,244,
  185,239,172,196,98,161,230,113,215,174,61,226,207,151,211,176,90,193,194,60,200,22,88,148,
  24,58,203,224,65,248,22,250,156,123,49,50,171,226,99,34,205,106,99,40,156,4,246,163,
  101,33,120,25,150,104,224,216,194,119,188,129,189,74,224,18,74,144,34,205,199,114,228,243,
  216,166,2,146,247,27,181,173,189,88,36,97,141,14,210,207,48,75,141,243,235,190,201,27,
  200,121,151,44,102,62,127,64,10,71,117,107,231,45,170,23,133,235,12,181,102,86,240,217,
  105,124,78,33,80,196,21,166,52,40,52,96,148,113,216,29,180,36,8,81,118,119,117,89,
  6,92,124,101,247,106,202,50,56,183,162,221,253,213,101,107,219,253,222,44,203,0,249,250,
  227,189,183,186,108,109,187,63,212,148,93,103,188,127,95,93,214,223,174,20,104,51,30,254,
  159,83,233,116,79,185,211,172,121,145,212,191,103,170,228,34,42,98,18,130,104,139,74,124,
  193,138,167,163,239,10,17,33,115,71,53,172,249,214,200,219,81,237,171,232,5,19,248,146,
  226,4,110,183,13,42,140,115,76,172,20,117,88,22,152,222,232,68,48,161,215,253,123,10,
  128,36,24,215,99,238,204,189,24,1,46,138,168,76,6,198,50,112,62,18,129,169,12,252,
  34,3,255,148,121,250,210,43,252,80,6,158,138,60,90,25,243,153,80,198,228,124,29,27,
  213,31,9,40,213,222,7,178,31,17,182,15,74,213,245,211,145,49,238,145,164,163,66,53,
  11,40,11,227,105,199,103,69,3,153,174,130,86,1,132,194,140,154,83,212,23,43,138,88,
  146,64,5,24,93,120,68,68,231,68,133,103,81,42,195,244,96,84,82,176,109,21,98,210,
  231,129,214,50,226,223,193,131,76,93,255,66,236,27,126,152,72,16,238,137,148,13,152,241,
  9,153,149,225,225,57,134,250,209,181,100,233,248,158,215,206,131,72,47,71,72,138,57,230,
  169,201,196,28,20,144,25,113,231,24,194,83,79,242,28,37,55,185,127,74,231,209,217,217,
  136,169,208,70,49,204,102,16,225,35,71,157,172,204,96,131,87,138,23,178,164,86,218,5,
  111,224,235,254,208,240,117,111,73,242,177,166,37,213,19,146,134,19,186,147,148,80,4,247,
  39,96,40,195,136,61,176,157,116,164,88,160,244,155,54,136,82,105,48,21,6,7,162,87,
  253,208,240,128,36,124,90,72,15,224,159,166,51,26,96,244,179,40,34,156,229,68,59,112,
  188,253,102,10,189,169,193,65,60,30,147,168,11,185,91,43,136,190,160,77,29,49,194,162,
  32,23,152,165,49,64,228,124,67,173,87,156,209,177,46,149,74,142,251,141,238,148,33,244,
  107,118,149,219,57,190,102,6,194,96,47,138,77,4,60,212,7,220,229,165,81,190,104,248,
  74,243,89,194,120,124,164,187,138,157,99,190,6,140,14,200,195,182,162,16,143,11,156,37,
  42,85,208,90,43,225,98,197,93,155,210,248,48,151,169,228,191,246,178,148,50,164,53,130,
  12,7,194,205,200,234,72,152,54,113,74,229,151,34,15,144,24,99,110,231,236,66,117,163,
  91,40,239,182,52,168,147,178,85,156,162,56,255,72,241,225,16,9,208,178,164,162,91,12,
  92,54,75,236,198,10,252,18,208,234,13,32,231,43,138,246,2,135,252,172,166,77,17,160,
  103,53,236,107,161,128,60,179,72,39,251,230,253,247,30,114,190,148,126,69,127,230,174,7,
  2,221,137,253,213,157,120,146,44,237,4,146,235,59,1,225,98,202,112,69,127,230,244,231,
  139,77,118,113,81,183,111,219,153,239,111,209,25,206,137,214,222,223,191,85,143,156,122,111,
  208,45,101,167,205,114,62,249,13,250,229,173,248,6,29,19,236,40,87,18,222,56,12,74,
  75,158,158,31,82,245,186,202,45,230,71,175,105,237,205,86,98,157,61,3,94,144,105,249,
  232,146,148,188,116,105,208,17,83,174,91,105,52,226,202,178,56,85,128,118,226,84,177,180,
  55,226,62,89,48,161,122,91,167,193,91,100,73,247,19,234,126,98,183,205,184,123,239,171,
  236,61,167,96,154,184,5,151,244,64,220,155,11,38,246,191,70,151,161,115,112,139,78,127,
  248,166,157,38,189,7,221,237,143,107,117,123,239,54,221,254,248,109,187,189,103,118,251,221,
  90,221,222,191,77,183,223,125,219,110,239,171,110,243,75,195,57,54,238,229,186,78,123,225,
  170,163,213,218,145,141,170,152,93,117,188,4,228,169,8,115,125,163,158,132,244,252,104,20,
  183,235,156,138,193,98,23,102,119,223,121,164,129,254,196,30,147,185,99,226,13,125,163,65,
  168,24,108,6,94,169,194,123,93,68,199,197,135,37,18,187,182,134,57,107,202,70,29,45,
  222,178,122,41,115,247,186,215,50,83,199,153,75,210,194,49,110,27,163,29,142,200,178,232,
  14,127,176,208,53,216,176,38,89,45,66,199,89,148,133,65,67,183,157,89,150,18,67,85,
  200,106,88,106,12,213,64,87,65,47,187,177,79,4,122,254,125,114,181,128,62,38,62,19,
  65,252,129,199,204,120,130,186,177,251,131,196,71,185,249,31,94,85,239,54,85,237,239,249,
  170,154,37,55,172,138,153,153,168,212,21,190,58,151,100,131,207,146,0,240,147,20,168,127,
  187,68,124,6,232,69,123,87,137,199,8,127,105,82,64,134,19,50,229,107,81,16,51,107,
  196,100,120,94,91,82,70,150,8,125,226,34,14,146,194,203,51,87,211,226,241,169,228,58,
  58,175,206,183,92,89,19,131,224,154,74,47,170,59,14,179,178,172,248,107,215,202,180,144,
  141,243,49,134,192,185,180,133,92,228,24,180,188,126,97,24,78,168,208,152,205,73,208,116,
  96,163,30,73,19,94,38,72,193,115,64,102,184,78,148,67,52,99,200,219,155,77,180,119,
  195,21,44,87,207,192,117,181,129,224,187,73,190,152,14,47,163,176,91,212,116,163,178,182,
  89,222,199,196,51,85,87,89,174,150,156,34,13,42,196,120,87,117,59,105,38,107,9,92,
  202,198,25,64,244,176,251,68,17,69,168,50,7,39,6,110,180,107,207,87,133,127,81,217,
  226,190,97,168,45,167,216,28,97,250,160,102,21,152,14,19,19,14,49,58,97,250,179,135,
  200,158,171,219,173,75,155,50,196,162,255,192,123,12,230,135,135,71,174,105,252,141,210,102,
  45,16,247,67,19,146,5,219,213,172,156,84,240,28,89,21,163,47,36,33,187,140,185,32,
  38,204,251,226,52,89,43,138,133,75,18,213,241,21,147,71,252,37,125,116,53,42,36,255,
  163,150,50,45,155,240,8,215,168,61,94,199,95,92,9,248,56,24,237,126,182,89,133,214,
  4,9,150,223,103,75,117,136,4,100,234,150,95,238,40,13,144,152,70,58,172,94,152,153,
  192,88,234,58,227,179,164,52,63,195,254,215,118,106,245,22,62,238,45,199,67,21,150,145,
  203,235,169,236,13,101,223,78,76,131,228,141,137,62,72,170,189,28,186,16,217,113,129,44,
  227,62,233,72,197,69,67,109,63,141,29,158,107,117,195,209,110,252,153,115,215,74,189,205,
  170,18,60,159,11,7,74,179,45,240,211,56,88,51,55,29,64,202,45,89,31,30,14,134,
  51,108,163,147,196,199,16,67,142,28,224,113,67,81,65,151,241,193,235,233,152,243,199,219,
  236,152,43,177,6,235,195,119,93,201,226,38,227,195,188,227,128,134,60,39,130,250,132,243,
  50,226,36,124,199,81,144,167,80,222,86,204,140,84,34,42,175,11,25,122,44,41,243,191,
  73,108,104,84,71,140,175,232,106,115,250,139,244,244,123,3,18,249,115,131,68,206,92,175,
  39,163,134,143,98,46,220,69,91,205,108,74,47,213,112,195,97,168,119,75,223,183,158,168,
  95,152,89,20,169,235,29,75,114,145,246,12,197,156,112,63,170,137,231,142,160,224,48,149,
  212,30,46,180,243,33,210,12,157,106,58,187,41,45,102,40,157,219,206,112,117,130,114,71,
  29,9,194,247,142,32,124,67,208,115,225,120,67,83,14,172,20,81,139,121,25,226,106,27,
  3,210,194,183,175,12,138,35,140,143,39,234,205,229,242,77,30,154,19,153,92,54,202,115,
  138,236,229,157,79,121,192,108,44,118,140,12,218,56,229,115,49,173,62,250,152,90,133,210,
  187,4,154,138,160,42,241,151,95,88,2,140,46,69,90,173,212,73,121,74,61,90,98,42,
  194,202,43,219,231,206,22,125,245,11,55,140,213,162,188,32,121,77,88,171,87,10,147,38,
  22,8,243,180,38,88,89,59,210,234,9,219,61,126,214,71,40,189,95,178,5,236,164,146,
  21,194,234,81,76,16,181,255,104,6,30,203,237,100,240,88,212,22,219,42,184,23,109,103,
  231,9,54,70,82,65,3,76,226,142,26,148,242,149,97,60,19,24,58,228,98,8,201,86,
  106,249,202,80,66,171,78,125,28,107,170,17,96,5,68,122,137,189,161,213,167,20,113,185,
  155,122,104,206,0,130,169,211,172,148,166,116,90,45,105,228,160,36,75,139,7,230,88,138,
  26,170,113,225,142,168,222,109,181,184,49,60,34,162,203,188,102,251,74,9,68,57,240,29,
  253,29,131,15,243,90,249,211,52,54,228,36,101,184,162,84,31,167,215,155,149,79,64,66,
  115,229,38,82,242,158,57,66,73,222,215,215,246,126,173,10,62,212,87,240,97,173,10,62,
  214,87,240,113,69,5,6,198,105,145,252,68,53,199,34,89,105,186,45,173,140,121,131,241,
  87,116,100,36,45,171,2,54,123,127,123,100,62,217,222,193,253,143,140,195,69,45,24,154,
  98,51,251,170,122,55,182,60,154,46,248,213,95,177,208,183,106,223,145,231,116,253,120,239,
  227,69,146,38,159,104,158,68,62,61,113,125,8,21,0,113,0,111,4,127,190,4,161,95,
  233,196,128,20,171,36,206,109,87,45,134,51,92,193,189,148,244,2,201,184,188,230,114,231,
  164,30,219,21,46,152,137,227,71,242,107,16,95,251,250,21,202,250,146,155,227,107,151,201,
  118,204,136,220,184,47,220,73,153,184,59,151,188,226,45,134,2,16,6,102,143,208,141,84,
  119,35,231,221,72,36,235,168,32,109,103,210,107,38,13,102,222,21,105,47,83,84,198,205,
  243,218,136,75,103,131,127,3,235,250,130,43,38,203,54,10,64,176,241,6,154,221,96,138,
  168,197,6,185,76,218,218,120,3,118,230,150,162,179,109,0,23,222,32,163,250,37,9,43,
  208,176,148,51,244,205,42,180,93,216,60,221,234,134,184,174,96,43,142,97,23,19,143,97,
  40,34,7,224,101,237,133,16,138,29,224,96,81,250,194,144,38,69,225,135,244,103,135,101,
  23,92,63,174,204,147,119,31,119,54,42,101,55,114,160,247,128,211,5,166,101,18,167,192,
  244,55,100,231,17,184,218,226,136,162,231,4,208,163,217,171,170,22,86,181,218,36,117,128,
  57,247,54,238,41,249,46,117,234,245,43,134,133,137,233,163,54,36,183,87,210,53,237,253,
  66,187,167,133,69,39,242,74,11,115,31,53,53,73,122,150,15,47,125,216,120,156,84,180,
  114,204,222,145,192,102,150,26,114,84,102,162,253,228,122,140,103,237,170,218,32,182,155,165,
  134,24,228,210,218,228,107,180,62,211,154,61,19,77,74,99,189,158,110,105,5,184,198,168,
  120,25,191,244,78,4,68,164,190,126,93,146,60,95,158,12,73,253,239,190,195,198,253,219,
  90,27,247,128,199,14,54,80,116,27,189,223,24,194,159,203,6,42,223,224,62,20,183,54,
  142,135,201,134,6,93,27,234,140,193,36,194,198,120,4,15,229,115,128,139,74,177,191,133,
  82,101,205,139,56,92,251,30,32,254,179,46,95,39,216,253,111,255,242,227,46,251,127,195,
  19,95,176,98,254,67,207,171,212,231,158,157,108,243,96,91,164,66,167,148,32,142,147,133,
  213,116,173,35,206,9,224,137,75,0,191,230,194,122,9,200,220,221,215,69,93,77,217,13,
  142,56,63,51,120,203,243,163,131,207,85,199,152,31,31,86,130,14,174,175,4,142,106,37,
  186,182,5,81,81,0,158,218,122,76,1,46,198,33,167,228,113,141,24,7,153,106,92,201,
  47,160,190,218,238,103,87,79,188,244,149,234,155,93,213,87,152,207,133,4,100,38,250,218,
  191,63,235,246,235,251,154,133,176,90,193,60,218,21,53,99,233,203,69,32,124,107,205,113,
  45,124,27,209,199,13,241,0,41,43,247,90,112,74,159,56,5,170,120,105,130,63,168,255,
  143,2,40,9,161,142,249,219,171,80,28,80,77,94,8,43,239,77,6,70,4,188,250,250,
  53,81,224,69,161,146,20,201,223,144,86,212,236,194,64,54,31,123,144,176,173,74,111,54,
  206,97,21,15,222,17,54,94,140,224,39,23,176,68,128,153,129,30,104,177,209,224,253,220,
  80,96,112,131,55,190,1,236,109,118,17,108,42,120,194,33,137,238,48,187,238,121,222,144,
  60,151,206,46,232,36,84,81,102,241,50,69,69,204,170,187,28,81,204,247,33,189,21,5,
  158,138,205,198,133,104,63,38,85,174,228,247,77,153,45,252,94,203,59,121,154,129,242,165,
  148,31,62,161,115,97,96,228,176,156,212,189,186,47,234,233,94,97,243,103,39,87,167,82,
  14,90,5,187,252,176,8,185,94,41,20,45,5,159,165,152,239,64,6,122,50,32,29,104,
  79,92,137,232,243,198,85,120,28,62,135,37,106,239,81,74,67,128,172,89,77,210,49,84,
  125,106,146,158,7,225,176,70,1,21,21,14,106,146,80,97,175,38,233,57,117,131,136,208,
  125,106,84,4,6,44,48,164,82,60,192,166,231,13,25,120,24,64,62,187,7,149,192,30,
  126,7,36,176,8,251,37,163,20,252,254,198,27,90,76,225,167,115,86,81,155,70,153,170,
  141,135,105,216,166,58,220,188,111,32,97,207,235,153,86,210,208,126,181,158,89,72,253,241,
  213,67,139,205,224,213,156,194,199,70,248,185,10,143,101,158,9,133,143,141,240,115,25,230,
  250,172,151,145,65,172,234,94,74,105,221,136,204,106,52,46,163,19,135,250,41,110,136,197,
  105,96,238,198,240,56,146,37,177,49,143,187,173,214,21,7,33,207,163,75,116,36,124,19,
  61,231,52,174,240,41,66,124,219,202,226,111,163,55,225,227,232,77,235,105,247,237,253,199,
  221,183,36,72,79,254,68,153,164,219,219,214,142,48,58,76,225,93,35,188,39,186,127,40,
  246,234,145,248,61,19,191,47,220,189,123,208,64,151,252,6,151,10,218,102,47,248,226,156,
  241,109,113,28,209,36,119,15,121,36,182,218,33,219,52,103,238,106,156,49,49,252,99,176,
  58,44,38,227,145,173,227,245,2,155,85,236,182,35,86,128,86,7,118,175,30,182,119,59,
  187,221,120,75,202,241,92,161,25,250,55,199,191,47,225,155,96,241,231,77,241,129,57,197,
  65,104,124,238,218,159,123,242,178,115,40,92,242,206,224,10,229,156,216,183,236,226,88,74,
  126,184,227,146,31,60,69,248,252,242,2,230,37,147,42,248,43,233,4,105,45,240,21,57,
  154,112,21,180,239,5,190,162,145,80,170,176,203,25,132,248,58,1,74,49,175,195,251,131,
  238,16,80,55,149,244,134,33,215,251,147,104,149,176,235,40,126,165,134,73,38,1,180,212,
  65,241,67,102,118,175,6,118,195,165,213,48,169,154,80,90,79,90,89,30,210,49,153,235,
  47,28,148,137,254,218,11,186,126,212,39,9,97,121,39,175,73,98,38,225,252,73,19,1,
  94,149,126,38,161,131,83,219,184,82,78,121,184,101,175,41,121,106,246,67,254,30,217,227,
  243,39,161,253,186,91,102,66,21,18,12,155,9,115,126,20,232,203,128,90,148,30,114,225,
  13,140,211,148,17,129,76,197,207,81,232,138,44,15,226,159,25,63,65,29,32,169,17,69,
  13,241,11,207,250,39,238,50,212,205,43,91,144,188,54,113,183,126,110,105,185,110,58,187,
  122,27,182,128,24,147,212,61,198,48,179,198,134,54,107,83,246,172,148,174,45,148,166,14,
  118,149,126,191,168,102,186,246,191,76,5,39,97,185,177,157,218,183,6,127,36,153,224,181,
  212,34,226,143,73,68,252,49,137,136,63,38,17,113,240,124,95,102,41,99,203,0,83,55,
  220,117,129,50,65,38,42,57,70,148,9,73,64,122,68,40,169,190,41,62,12,189,16,142,
  31,245,45,238,242,88,0,96,88,234,196,110,216,1,198,164,31,80,100,199,114,18,201,28,
  221,249,253,73,119,78,111,155,140,25,238,192,171,11,12,34,120,193,116,134,247,112,16,141,
  79,230,167,205,108,139,81,22,201,151,221,32,105,101,91,220,81,68,71,164,206,84,43,231,
  212,34,30,60,96,207,6,195,147,94,171,117,26,245,79,6,248,89,24,92,113,64,190,97,
  56,11,167,193,66,9,103,48,14,161,240,227,45,114,86,233,159,91,246,228,73,114,168,76,
  166,183,68,60,6,157,115,48,7,106,157,182,71,60,35,17,57,153,36,7,95,90,102,63,
  91,42,5,176,44,98,178,43,172,30,53,168,108,3,198,62,133,236,6,236,172,157,100,167,
  128,141,184,36,25,20,182,1,52,189,230,22,130,226,235,161,113,216,85,230,178,202,19,170,
  48,71,189,42,125,134,213,154,70,125,185,90,179,251,211,238,76,190,68,135,152,210,217,41,
  65,93,76,35,61,149,57,113,27,86,87,19,183,61,84,25,141,101,180,203,135,174,127,200,
  10,202,185,129,119,169,126,147,77,212,40,150,253,202,238,143,187,153,236,87,63,138,105,8,
  236,169,202,57,150,125,113,227,247,133,162,83,127,203,98,76,42,79,119,73,69,178,21,11,
  26,211,142,235,92,11,133,177,206,247,91,127,231,130,173,21,110,62,94,35,220,230,126,167,
  186,95,168,214,77,72,107,50,23,240,76,80,64,137,12,224,148,150,74,60,117,42,205,121,
  152,252,188,42,90,193,132,94,220,14,116,148,164,65,88,17,140,21,226,12,191,37,115,65,
  2,149,60,249,64,249,199,34,90,154,102,213,219,153,194,27,116,142,27,220,17,59,202,41,
  167,73,19,180,239,198,193,248,164,127,106,50,46,75,124,71,20,169,197,39,193,145,24,48,
  217,1,125,32,58,215,152,73,131,99,195,78,79,55,49,236,134,115,40,193,79,51,23,229,
  77,214,145,65,158,130,97,151,81,118,238,36,105,11,184,235,184,33,74,194,47,141,159,255,
  171,198,52,166,49,165,114,15,166,39,227,211,174,232,136,193,81,31,3,16,41,11,227,44,
  85,154,203,129,112,4,252,136,238,218,213,249,14,175,108,193,151,134,250,1,173,141,39,240,
  20,7,101,168,15,240,244,62,254,201,131,50,192,1,158,158,118,103,226,220,58,221,10,22,
  51,115,111,176,174,207,168,139,224,18,229,122,146,93,145,19,146,181,210,9,107,159,245,192,
  115,216,99,123,111,178,90,133,176,0,117,116,139,185,134,107,176,32,119,110,58,58,159,195,
  165,163,34,7,120,168,73,114,91,101,149,205,226,144,144,51,195,154,123,121,123,235,107,94,
  9,159,27,8,238,220,86,54,71,28,143,235,69,215,213,30,85,150,183,248,169,73,109,222,
  189,18,182,73,149,77,253,174,180,230,228,217,242,125,227,70,130,165,78,28,220,110,245,177,
  208,39,28,140,85,150,4,234,82,170,189,145,250,198,141,68,59,153,140,126,82,189,54,134,
  57,149,27,90,62,49,196,38,158,158,12,79,117,99,93,239,17,1,136,153,45,234,197,162,
  202,165,119,145,123,17,113,26,171,186,136,36,145,85,122,44,83,51,162,238,162,169,184,139,
  166,226,46,154,186,119,145,216,187,230,90,119,179,59,245,188,205,204,94,169,177,81,212,222,
  243,227,59,75,153,38,174,23,133,138,212,141,27,227,151,194,41,221,24,71,6,76,223,49,
  30,129,144,210,250,20,50,78,55,212,211,8,223,199,126,223,205,74,65,161,130,102,42,93,
  5,75,177,157,35,153,55,80,107,95,67,137,93,41,173,59,34,131,255,191,208,97,191,137,
  202,186,71,51,93,172,71,41,2,117,26,233,53,170,231,162,16,83,60,231,97,159,218,185,
  72,185,177,210,249,13,180,108,249,41,163,181,228,201,124,11,22,145,137,6,217,41,215,11,
  83,113,108,235,140,69,111,245,216,230,49,75,213,100,161,253,69,111,65,179,206,147,154,204,
  167,245,221,168,47,98,14,149,57,151,220,9,68,142,192,120,236,248,21,193,214,109,36,16,
  206,56,42,215,110,98,110,14,133,54,178,103,187,56,18,106,155,144,250,241,77,117,230,180,
  139,152,255,107,150,203,208,183,171,168,149,185,101,36,50,5,84,130,10,119,244,171,133,167,
  119,106,90,89,173,251,199,23,172,99,44,222,66,235,175,84,180,248,200,89,204,93,41,224,
  125,85,235,140,140,185,92,191,179,91,15,233,45,51,40,85,49,109,246,210,168,177,107,194,
  73,13,82,63,211,48,147,193,196,128,55,24,160,114,228,216,24,98,201,249,0,148,37,230,
  242,242,213,44,44,193,11,238,85,22,147,166,84,174,99,145,65,183,173,40,71,119,151,27,
  27,184,187,202,44,195,93,162,33,221,37,26,210,221,111,100,150,97,205,30,45,55,203,176,
  172,91,183,52,203,176,186,95,235,152,101,88,214,177,245,109,45,152,4,47,99,35,254,101,
  166,23,244,142,93,221,67,83,95,252,182,42,241,107,183,103,42,201,127,248,211,91,51,76,
  32,252,17,59,5,235,183,183,103,182,247,238,207,111,111,223,180,149,224,26,211,88,127,81,
  110,106,61,193,99,6,227,38,107,114,139,230,62,222,186,185,189,219,52,247,238,214,205,221,
  202,20,69,213,92,68,180,188,153,63,104,62,194,28,142,97,225,192,138,94,109,72,226,79,
  234,99,189,97,137,27,117,219,142,94,203,206,196,191,111,64,245,118,39,190,197,24,237,104,
  109,139,66,189,106,132,180,147,100,83,15,96,192,174,30,11,147,36,7,8,108,33,64,18,
  75,49,220,226,232,236,27,28,167,52,228,180,46,71,16,52,31,36,109,157,73,230,97,29,
  219,84,230,111,53,197,8,175,109,136,39,27,183,188,146,83,198,228,45,93,21,83,234,15,
  82,120,182,97,50,146,200,75,248,115,217,157,152,162,5,11,0,14,167,105,201,19,40,113,
  204,180,185,155,65,240,88,109,37,116,115,62,157,215,136,39,157,189,73,60,9,39,186,235,
  158,247,196,26,249,13,92,83,236,0,46,124,62,90,171,184,111,220,254,3,17,24,47,177,
  155,109,49,73,199,237,108,28,49,50,219,232,203,255,149,251,236,15,26,97,73,150,217,91,
  249,111,177,255,212,254,8,194,235,101,47,191,14,94,126,53,115,197,56,125,118,11,161,224,
  48,155,91,115,217,76,129,252,40,229,86,126,62,23,129,199,231,66,15,48,28,167,203,124,
  16,113,43,28,137,82,173,28,113,107,18,250,169,202,133,68,165,238,191,176,103,94,103,75,
  69,152,70,183,76,10,232,50,234,194,82,47,130,26,223,124,162,26,237,121,208,169,169,176,
  52,170,248,75,13,14,118,44,135,111,55,236,92,155,27,112,53,50,218,106,91,174,51,57,
  142,222,240,131,246,126,236,122,87,20,242,31,63,159,187,62,22,45,81,52,11,179,90,62,
  24,140,120,137,245,22,107,100,86,247,245,0,181,49,122,94,143,209,15,193,120,222,221,54,
  43,147,150,190,61,232,95,197,74,135,211,86,83,18,178,164,126,156,163,104,39,115,9,123,
  47,178,86,75,155,110,149,81,116,81,134,173,89,208,178,42,54,138,250,13,162,87,170,214,
  54,95,164,61,3,112,101,137,121,189,204,156,187,43,19,106,116,43,108,251,219,49,125,84,
  252,60,74,19,105,161,119,71,238,36,82,254,31,151,113,227,253,56,144,212,97,99,160,5,
  147,175,203,77,223,181,53,227,97,164,116,178,83,177,243,80,118,88,196,117,20,167,46,142,
  218,50,182,178,245,237,9,13,182,115,69,227,20,188,128,70,124,159,196,171,31,236,6,15,
  169,194,142,211,74,213,229,26,36,166,13,139,53,124,240,246,147,164,102,16,138,182,84,73,
  135,149,5,109,126,230,254,14,249,92,32,145,67,10,37,15,118,214,50,168,35,44,231,56,
  246,116,170,187,166,166,72,223,6,56,166,9,13,201,150,149,51,234,158,153,182,61,195,14,
  117,77,211,102,146,175,95,31,159,123,52,185,165,200,142,221,5,236,155,10,61,205,222,71,
  150,69,214,180,30,28,105,192,200,93,11,230,43,237,209,136,2,237,168,172,129,164,30,75,
  35,162,83,166,165,17,81,132,251,28,18,125,136,34,251,128,255,97,107,35,188,17,243,38,
  53,140,173,203,102,58,118,163,181,22,71,68,5,166,113,145,245,64,240,34,124,73,86,71,
  158,165,203,205,120,87,44,135,40,175,244,235,219,12,121,121,35,155,33,178,1,17,121,134,
  187,128,152,182,209,99,241,30,43,136,39,118,57,82,236,69,8,25,31,100,99,220,113,202,
  190,71,54,133,123,81,184,114,223,53,84,163,33,58,1,48,171,178,196,227,233,48,254,41,
  134,7,44,25,195,218,57,202,251,209,121,223,136,120,92,148,209,212,140,120,130,221,194,148,
  89,158,141,236,114,143,168,70,83,70,65,20,247,198,203,90,188,137,108,52,12,69,250,197,
  182,84,198,82,121,17,129,32,13,146,105,57,124,10,81,198,232,170,48,98,142,225,9,85,
  33,81,44,230,93,62,50,88,0,5,150,186,63,26,179,200,23,113,241,91,180,119,239,158,
  149,194,170,124,53,179,226,14,147,243,104,167,146,203,95,28,90,57,209,243,216,138,251,232,
  141,124,133,173,231,70,138,190,138,117,233,143,71,211,41,214,159,129,190,194,152,43,74,120,
  46,33,35,173,136,89,226,104,24,15,178,75,189,37,10,246,125,68,91,199,168,129,38,218,
  158,153,41,104,219,35,198,176,214,217,224,54,119,254,41,75,133,33,20,89,163,21,11,127,
  182,56,44,114,122,172,164,55,80,23,41,212,122,141,74,64,119,218,205,214,70,60,206,14,
  50,236,100,98,1,169,234,243,68,0,237,81,34,214,92,38,225,237,212,79,142,70,228,199,
  135,77,159,174,106,156,93,254,130,122,24,215,88,142,232,2,195,193,198,81,223,37,160,215,
  11,0,98,109,141,162,106,174,198,97,218,247,120,39,105,83,237,112,190,142,252,118,176,43,
  157,145,241,118,116,54,201,217,209,25,30,236,220,137,202,7,21,198,126,165,193,18,12,204,
  31,19,12,57,57,196,230,103,134,19,84,12,97,242,80,255,162,168,254,172,40,97,222,32,
  207,62,65,242,238,32,238,15,147,127,37,46,231,201,41,134,153,56,98,66,84,16,70,64,
  111,223,50,205,55,249,218,214,122,7,138,117,194,164,211,20,215,2,14,55,109,125,131,224,
  26,114,179,255,37,129,87,103,67,201,0,110,252,237,238,117,178,248,27,116,236,10,174,94,
  183,145,157,111,64,16,133,1,205,193,214,127,5,93,50,57,56,74,103,137,97,248,68,84,
  95,212,86,47,42,133,228,109,154,97,166,55,166,2,250,82,229,220,127,194,221,107,5,84,
  23,86,43,176,133,71,128,156,129,153,135,5,215,173,13,58,34,150,163,76,123,192,103,200,
  101,158,250,254,250,85,164,63,153,141,147,92,165,178,47,149,198,243,238,59,101,247,31,22,
  210,25,110,71,12,45,74,23,6,117,67,224,97,134,230,6,245,25,227,64,212,38,151,115,
  219,236,38,244,62,134,104,35,174,169,89,206,36,34,195,209,4,7,134,201,70,74,65,197,
  165,210,171,250,94,49,228,86,101,164,33,177,154,186,226,170,169,33,174,202,132,77,83,125,
  41,89,87,20,201,206,51,129,13,121,77,81,132,6,60,14,32,66,162,117,123,85,238,51,
  100,144,55,153,121,173,81,127,140,27,205,189,226,100,239,232,42,179,46,40,153,64,87,154,
  117,67,201,4,117,181,85,239,41,163,82,14,139,170,87,159,81,189,155,69,70,185,13,185,
  249,172,120,153,153,223,134,206,237,72,56,233,79,100,62,75,230,114,171,146,85,232,11,210,
  190,47,101,18,135,49,214,197,41,147,248,197,224,92,161,97,106,222,26,206,45,66,137,238,
  165,228,189,170,68,70,121,73,185,183,22,109,27,247,134,246,222,219,58,163,30,162,17,161,
  147,233,238,118,238,114,187,108,165,13,25,105,100,163,251,219,189,229,117,242,199,74,250,71,
  39,3,187,172,42,183,191,51,216,234,64,145,193,190,129,171,119,172,155,69,92,199,117,247,
  180,155,157,95,209,53,87,55,211,244,150,183,182,125,137,35,201,184,168,244,109,206,55,145,
  70,49,109,140,83,149,210,151,190,15,19,160,78,86,113,128,26,220,0,153,93,172,192,135,
  42,176,166,77,36,161,138,55,16,84,18,24,131,137,62,16,208,209,136,131,131,72,144,6,
  144,99,150,90,29,12,101,34,139,78,173,184,116,136,217,206,99,140,19,164,142,180,52,175,
  153,205,62,13,129,241,89,118,190,82,29,29,217,185,164,241,5,92,0,227,106,41,21,29,
  217,185,164,0,195,48,73,82,187,4,139,138,116,170,153,243,192,24,152,142,176,70,167,163,
  35,55,155,51,78,150,114,232,31,172,157,22,121,242,75,187,155,19,210,127,191,72,68,167,
  228,167,213,37,25,25,217,89,156,238,200,120,2,87,41,84,49,231,118,143,42,201,145,191,
  148,28,220,52,233,207,64,113,144,147,37,62,237,169,18,145,145,157,197,157,38,17,95,211,
  175,74,114,228,47,229,212,102,173,164,25,231,237,161,185,158,86,230,202,146,66,71,220,183,
  154,34,58,178,115,201,151,58,201,155,246,161,147,106,151,82,209,145,157,203,45,85,179,133,
  170,233,81,77,57,183,66,156,109,121,120,141,40,76,204,49,199,193,236,234,145,20,85,51,
  43,36,143,97,82,181,93,246,52,101,166,213,181,105,230,137,150,20,95,222,11,73,219,170,
  116,65,37,212,181,175,50,68,117,5,237,150,195,74,89,70,166,244,150,102,41,154,46,84,
  1,63,186,191,86,156,211,85,43,45,242,100,247,78,141,13,98,156,150,150,44,76,37,61,
  170,41,230,109,149,68,218,57,210,110,239,96,29,31,57,249,228,77,1,101,48,60,235,206,
  199,132,92,93,84,64,67,37,57,242,151,146,20,46,186,5,11,208,228,251,137,93,143,145,
  16,185,57,171,101,159,255,114,88,91,28,105,145,39,127,181,146,227,225,168,255,27,77,26,
  19,54,175,173,207,206,22,45,175,165,218,138,94,96,59,210,89,93,59,49,242,21,240,174,
  171,175,39,222,38,205,212,250,182,205,92,209,210,42,188,189,137,211,81,145,97,229,167,206,
  46,209,241,145,147,175,82,80,218,178,172,171,64,166,71,53,229,42,21,234,217,176,226,156,
  57,176,210,34,79,118,239,120,39,170,238,73,165,198,137,172,103,82,95,186,236,27,21,208,
  71,165,14,138,140,204,12,254,121,39,52,213,24,168,248,116,199,40,162,35,59,147,183,198,
  241,232,211,208,184,164,228,167,83,163,140,142,236,76,21,192,44,83,28,4,162,18,45,135,
  147,25,99,201,60,3,201,244,40,50,111,131,44,218,105,205,142,147,84,223,217,100,170,27,
  19,95,78,115,34,54,178,178,84,154,164,4,227,198,81,159,129,201,9,213,77,165,53,247,
  95,106,95,123,105,253,109,167,146,180,59,26,43,74,101,49,122,149,46,185,254,8,244,143,
  227,126,50,1,157,67,247,211,137,117,122,235,164,70,222,34,149,158,155,25,140,206,85,162,
  157,172,63,142,226,162,154,147,98,221,87,141,238,126,94,127,161,230,149,187,52,95,121,141,
  170,183,141,110,193,140,114,143,175,145,20,85,51,123,91,144,200,190,110,192,136,113,234,55,
  82,162,74,86,63,242,33,48,107,3,237,208,49,46,194,161,83,162,74,214,165,181,171,67,
  86,109,198,76,170,105,207,204,18,213,23,94,218,3,23,139,115,162,107,90,118,112,57,183,
  144,127,189,210,11,99,169,216,135,187,74,44,50,50,51,56,39,66,90,41,62,195,139,197,
  121,96,240,200,200,204,17,88,45,251,239,75,59,45,242,228,175,28,127,158,92,247,50,181,
  19,35,95,9,133,48,214,227,138,85,52,177,130,33,34,42,143,25,29,239,144,250,89,169,
  193,76,140,124,37,2,169,234,12,113,15,11,146,25,49,206,10,25,41,81,37,171,119,213,
  25,73,152,29,54,119,230,205,148,168,146,215,83,92,119,208,137,117,58,233,164,70,222,34,
  254,206,74,188,205,233,169,140,142,236,92,110,41,163,131,245,56,100,89,65,28,203,213,216,
  98,137,157,195,137,210,143,133,8,136,131,245,85,51,68,117,37,171,117,178,115,91,91,161,
  65,110,112,99,43,20,7,200,80,56,196,6,230,253,88,166,25,217,30,233,170,170,37,140,
  196,200,87,162,226,193,187,202,161,13,212,51,218,140,53,13,27,164,46,79,215,151,31,40,
  41,132,141,30,228,218,42,129,186,65,125,208,36,183,224,72,110,99,218,140,124,255,10,58,
  142,168,222,121,91,26,41,81,37,175,66,52,193,139,31,13,202,161,93,86,69,71,118,46,
  163,20,176,225,106,153,190,70,70,217,135,145,255,51,228,104,170,5,40,54,178,242,40,241,
  246,98,120,84,89,121,25,27,89,121,36,204,137,167,213,18,34,50,50,115,200,29,67,72,
  142,179,79,52,58,84,24,216,227,229,136,65,185,137,147,91,69,71,118,46,183,212,207,254,
  73,174,166,71,53,229,124,21,86,230,223,77,141,188,101,124,85,85,151,166,146,28,249,75,
  137,218,206,33,183,68,252,157,202,30,52,18,34,55,167,44,155,185,101,50,153,55,67,158,
  245,108,221,164,117,182,110,180,41,196,162,145,75,235,253,134,152,183,109,50,137,89,75,18,
  206,86,198,91,146,225,10,179,101,76,96,123,172,180,0,226,5,179,240,43,204,163,147,223,
  41,201,187,133,141,51,246,205,249,183,144,252,178,123,41,179,69,57,25,67,179,146,120,137,
  40,214,170,6,134,92,20,137,173,41,107,67,119,92,189,99,206,211,149,176,157,113,41,42,
  121,12,163,33,148,167,239,64,103,205,171,240,70,18,35,157,96,178,40,72,53,120,121,24,
  54,30,31,149,22,15,195,207,192,176,17,243,168,180,24,24,85,238,133,166,186,69,165,226,
  94,24,132,63,127,94,75,236,40,208,163,209,137,129,170,230,176,118,96,46,203,162,202,175,
  80,79,0,119,22,87,197,235,46,201,24,214,31,129,3,187,165,86,197,27,3,20,49,102,
  109,94,36,211,143,233,71,165,151,205,96,35,231,117,189,88,157,88,237,231,129,220,94,94,
  62,131,205,91,136,74,139,207,224,103,50,216,244,231,168,180,152,12,43,56,12,53,100,247,
  168,244,115,24,106,168,188,14,97,55,42,109,42,239,114,18,175,159,156,27,149,94,18,111,
  29,125,215,165,36,70,165,67,223,93,66,220,245,208,114,163,178,74,220,93,143,178,187,156,
  122,27,149,75,41,187,181,88,127,5,21,143,74,23,235,247,35,225,54,162,28,149,22,18,
  190,10,63,174,195,132,163,178,6,63,94,134,28,175,66,158,253,136,178,58,61,110,10,107,
  205,75,12,118,72,171,81,105,19,131,87,80,130,107,136,190,81,233,167,4,187,119,187,188,
  213,163,146,223,237,101,29,226,224,162,10,81,233,32,14,74,44,169,82,74,75,47,169,32,
  191,239,42,207,123,243,81,31,169,28,12,246,96,19,86,193,142,48,87,67,134,73,24,124,
  146,178,78,30,0,165,197,162,140,15,42,37,196,156,42,69,148,44,148,12,169,253,206,5,
  159,252,219,93,74,73,153,95,108,17,165,164,70,117,237,180,72,135,14,171,34,36,193,225,
  47,194,69,61,116,152,65,59,41,111,84,5,118,90,48,73,135,85,17,111,199,12,129,37,
  29,86,69,152,172,140,191,140,16,172,49,62,20,122,227,47,101,138,56,25,31,252,54,48,
  159,105,158,155,192,126,217,249,158,117,105,244,128,176,140,113,10,44,67,161,111,41,123,224,
  85,36,165,188,13,216,2,86,149,40,89,143,144,166,242,86,161,68,175,204,47,53,151,158,
  183,98,229,113,24,149,238,91,81,201,195,249,15,28,19,200,83,65,149,255,177,103,165,181,
  156,158,10,170,252,82,72,206,95,72,203,240,217,223,102,247,152,196,80,109,31,133,156,145,
  253,109,246,118,73,113,45,251,103,127,87,58,191,164,14,71,56,208,19,169,106,243,222,0,
  235,164,232,91,65,199,169,90,151,244,205,234,147,234,139,43,149,87,5,118,21,97,190,74,
  148,81,143,23,92,88,98,126,230,151,81,16,66,125,117,229,152,252,159,241,225,52,183,172,
  215,90,58,208,141,49,43,129,164,95,109,5,76,46,208,252,50,10,126,92,86,242,163,93,
  244,163,91,150,68,218,106,203,114,33,56,235,211,93,172,165,11,229,44,18,149,245,146,56,
  236,71,127,84,90,36,142,21,244,141,26,82,70,84,250,233,27,75,137,27,94,74,70,84,
  250,136,27,203,41,27,126,42,70,84,122,41,27,94,114,156,69,131,139,74,147,28,231,39,
  166,217,4,180,168,180,136,105,62,74,154,73,63,139,74,131,146,230,37,163,89,180,179,168,
  52,201,104,94,26,154,69,56,139,74,147,134,230,35,160,153,100,179,168,52,8,104,85,234,
  153,166,154,69,165,162,158,217,66,167,149,236,142,240,171,253,93,41,206,37,92,151,87,34,
  197,99,125,177,149,10,153,16,236,242,250,132,252,172,39,146,191,248,132,184,172,231,193,167,
  228,106,117,88,35,103,74,20,182,6,69,51,228,103,221,24,54,138,138,148,108,117,20,30,
  1,91,79,36,71,211,45,73,90,15,202,238,136,223,186,49,124,92,134,164,173,103,84,150,
  104,174,253,77,197,133,44,110,165,160,18,216,149,33,134,25,43,241,220,42,98,108,72,242,
  26,31,84,74,18,5,171,244,179,170,57,83,214,37,67,95,193,42,35,52,56,108,21,135,
  104,51,157,77,206,146,124,243,97,85,121,207,206,250,96,167,179,42,11,127,133,124,241,189,
  66,190,240,87,136,56,132,245,76,15,31,139,35,42,171,76,143,146,240,214,42,65,14,176,
  39,57,97,105,167,80,253,37,11,250,44,103,217,247,102,102,114,50,60,63,5,141,34,82,
  206,197,191,207,95,136,98,242,195,40,40,36,61,170,200,138,16,7,161,98,34,236,148,58,
  242,66,38,67,64,196,146,14,209,82,27,85,154,169,18,6,161,198,212,151,209,156,37,241,
  81,95,158,139,136,216,223,186,184,221,95,165,133,101,166,118,109,214,84,74,26,232,119,118,
  153,97,183,19,104,92,156,90,82,46,124,148,220,173,149,120,134,200,98,202,15,171,43,37,
  226,37,88,153,162,37,52,3,78,156,49,15,21,217,145,165,245,201,101,168,196,185,85,145,
  112,201,210,154,184,76,138,79,32,197,146,38,169,39,81,203,161,153,17,230,134,55,4,70,
  234,137,214,178,18,51,194,168,196,16,11,169,212,97,202,144,80,21,198,183,167,134,122,202,
  173,71,118,221,43,184,110,74,145,212,146,127,101,103,140,111,163,51,62,113,144,213,148,100,
  183,78,51,193,83,185,148,250,168,173,88,9,139,152,149,202,72,115,234,152,148,68,117,190,
  184,84,8,155,114,22,172,20,241,208,191,124,114,28,166,114,186,157,164,171,90,178,108,174,
  96,135,71,170,99,107,5,109,216,33,11,219,20,225,37,242,28,62,233,141,168,244,200,115,
  40,65,65,15,222,41,4,16,105,30,229,135,49,147,21,9,195,218,26,204,41,240,201,37,
  114,241,193,234,37,146,169,27,36,179,155,182,197,13,253,5,205,70,43,226,137,166,20,74,
  21,35,54,68,86,168,117,227,219,232,131,169,46,80,207,87,144,117,152,17,190,74,14,151,
  1,52,191,222,128,85,237,161,31,196,85,37,251,235,43,127,105,93,136,213,232,250,106,253,
  151,130,87,99,192,115,113,249,171,11,28,190,131,175,235,142,96,55,117,219,142,50,186,92,
  35,107,189,22,3,195,83,185,153,102,180,226,200,12,45,101,100,200,90,157,56,179,182,101,
  29,45,221,222,149,254,46,89,162,214,75,232,253,234,172,153,49,38,240,54,213,47,150,48,
  98,21,216,54,99,220,122,150,110,247,170,54,134,170,207,191,205,181,85,195,219,250,253,48,
  24,232,97,29,167,33,116,57,5,225,42,68,63,244,147,255,195,229,68,254,176,142,242,26,
  214,145,88,195,213,100,212,112,53,169,52,92,77,14,13,111,66,237,116,13,131,72,91,71,
  22,229,114,9,133,50,172,103,55,132,245,108,133,112,25,247,32,92,151,170,25,46,167,93,
  134,203,72,148,225,122,68,200,112,57,169,49,92,69,79,12,87,17,13,195,85,148,65,233,
  152,201,97,117,132,220,132,136,244,98,201,206,152,48,180,165,252,92,115,11,255,28,126,23,
  129,109,20,17,101,10,248,242,204,131,20,222,175,153,11,108,105,59,116,97,89,62,114,204,
  163,132,235,114,73,194,229,188,144,112,25,155,46,92,198,37,114,13,169,148,58,28,174,38,
  104,133,55,35,87,133,55,161,70,133,245,84,167,176,158,245,23,214,179,248,194,245,168,81,
  225,77,136,77,225,122,228,164,112,53,201,40,244,19,135,194,101,52,32,199,32,76,189,215,
  41,131,12,20,220,202,109,205,250,190,94,22,225,113,225,24,169,122,102,27,169,42,43,86,
  170,142,166,28,6,105,91,85,134,133,41,59,113,211,216,205,82,252,105,247,135,191,255,253,
  239,123,187,247,180,18,149,233,205,74,82,101,116,148,34,118,239,120,5,109,117,251,166,41,
  170,29,37,220,167,13,51,25,118,104,92,99,139,108,132,174,1,73,227,66,232,27,119,1,
  211,236,162,191,78,143,53,21,41,172,35,210,135,235,17,197,66,91,80,65,218,20,27,98,
  99,74,247,182,51,233,239,118,32,3,231,133,244,36,158,228,50,244,140,123,185,141,201,24,
  169,200,150,202,26,206,101,204,23,229,148,252,247,177,12,253,166,226,46,114,103,123,60,77,
  60,246,70,143,11,38,31,232,221,40,222,13,178,137,209,24,246,119,134,188,243,191,37,134,
  139,23,203,135,240,73,123,235,94,72,255,152,205,174,80,125,236,242,15,17,222,101,177,34,
  215,174,114,243,242,62,134,233,235,123,65,119,104,184,36,59,161,252,123,200,182,23,238,35,
  227,208,245,73,172,124,37,51,3,196,87,57,172,127,238,35,247,29,114,212,92,201,60,187,
  48,178,161,66,150,109,33,220,41,115,135,138,209,176,80,106,127,252,220,72,203,171,204,66,
  165,124,117,176,238,91,94,139,118,13,251,133,79,115,152,98,154,77,106,12,24,242,153,21,
  184,35,38,173,31,23,194,196,37,54,48,208,198,60,142,164,207,174,199,101,227,111,60,123,
  103,99,243,144,103,165,3,192,114,109,114,216,177,81,102,27,103,201,6,193,18,200,168,102,
  57,12,68,81,148,168,120,35,254,20,143,176,13,54,10,86,77,177,245,55,120,199,85,198,
  78,185,29,66,246,74,82,36,90,68,188,203,242,49,72,225,207,102,234,92,177,6,237,84,
  225,76,13,98,176,111,71,201,37,175,73,25,89,100,159,125,58,195,158,178,207,83,2,108,
  73,88,109,113,224,244,236,149,88,221,134,175,49,246,124,228,149,195,104,46,106,156,50,138,
  70,114,192,226,180,194,38,55,110,228,158,102,78,24,197,84,184,118,33,209,135,47,150,215,
  71,85,133,2,15,48,242,206,124,232,209,59,35,143,208,155,33,38,10,134,103,201,40,36,
  251,42,70,41,73,115,152,238,13,249,254,233,78,227,198,128,249,52,111,168,147,66,195,14,
  99,172,10,51,30,31,34,71,202,184,243,141,250,12,231,177,204,224,73,255,50,100,137,204,
  102,222,239,220,136,239,46,133,127,27,138,240,174,242,149,184,197,168,197,114,103,30,99,148,
  116,183,162,135,128,63,128,60,33,153,85,203,153,209,211,76,237,201,134,238,96,219,223,1,
  209,40,78,46,124,81,47,107,228,156,181,195,27,9,101,11,129,48,173,170,28,219,81,241,
  44,31,125,26,165,134,13,210,6,245,107,124,31,15,45,60,248,191,126,29,63,0,8,166,
  128,48,154,125,45,115,118,198,225,148,44,115,118,202,92,226,142,225,236,162,83,142,72,125,
  132,225,130,192,145,98,190,199,242,80,13,252,203,16,163,192,140,133,252,188,7,136,67,93,
  236,178,203,152,164,57,99,205,44,228,5,101,216,167,53,175,168,144,139,11,177,149,175,200,
  75,179,88,117,192,184,253,91,23,244,168,160,184,91,180,152,58,22,1,198,23,181,203,129,
  243,194,180,116,60,34,19,200,194,4,44,246,52,64,149,218,230,108,151,106,135,118,13,244,
  225,42,202,155,40,127,213,46,232,103,142,139,105,107,30,177,240,85,139,165,204,131,142,236,
  234,57,136,230,35,117,3,143,144,35,162,26,16,154,179,208,28,33,203,0,234,179,89,176,
  184,136,243,141,215,2,209,15,63,72,83,217,19,121,179,125,82,129,81,189,165,236,80,56,
  130,98,214,32,219,128,219,28,197,226,27,67,2,233,129,244,203,83,103,31,91,236,35,11,
  131,80,133,120,116,178,196,206,179,85,92,124,249,107,41,117,132,168,47,246,219,49,214,213,
  214,88,52,86,245,192,2,245,98,156,101,191,61,42,171,246,148,237,182,3,218,11,118,213,
  150,135,112,234,14,74,196,69,185,116,128,92,152,20,189,126,157,6,154,54,82,224,61,192,
  140,221,42,67,192,52,170,196,54,180,109,182,45,96,169,97,223,91,79,141,50,107,124,95,
  27,74,54,138,118,110,49,75,224,94,213,155,174,230,208,249,247,92,117,132,103,60,250,221,
  52,17,237,139,21,22,250,94,167,245,227,92,54,62,152,71,126,104,78,176,97,33,27,132,
  142,60,33,215,116,157,198,235,244,230,195,197,118,125,237,175,207,30,200,81,242,105,162,92,
  64,145,189,247,15,99,211,146,54,182,189,123,19,18,130,51,137,229,161,224,91,202,222,72,
  159,98,79,127,41,223,135,49,95,245,156,51,221,36,204,78,130,38,174,11,216,189,118,118,
  45,77,220,36,166,43,0,53,122,102,17,94,191,219,34,133,178,245,41,27,39,31,28,253,
  78,240,156,95,186,241,89,209,216,109,199,77,228,152,134,195,112,16,246,232,226,154,61,216,
  9,240,3,183,172,205,113,59,131,91,228,184,153,181,199,240,78,159,55,225,5,254,65,196,
  82,135,15,162,118,143,5,238,71,61,190,214,48,157,187,61,235,78,155,209,60,28,210,31,
  184,159,109,54,166,173,184,57,108,237,53,179,160,53,108,54,226,230,180,69,95,48,215,221,
  231,222,18,134,240,140,50,229,189,153,196,87,0,85,237,6,21,200,112,124,6,81,123,74,
  249,155,13,89,164,43,138,180,111,94,230,62,186,251,176,225,148,194,208,115,86,108,24,77,
  31,236,60,108,231,29,158,12,92,68,229,67,91,109,50,109,238,171,59,232,208,232,169,90,
  242,30,189,188,172,85,172,81,233,190,217,143,27,119,67,206,75,204,7,113,147,217,145,208,
  132,108,111,222,248,36,65,118,138,25,241,100,5,63,140,171,249,39,113,8,97,172,129,70,
  244,165,157,114,165,118,197,141,215,223,87,214,225,25,245,203,129,25,226,174,247,129,200,215,
  169,119,247,23,50,1,112,184,157,54,83,178,40,46,154,106,202,0,179,148,250,32,183,26,
  230,216,167,6,121,121,187,160,51,150,182,99,28,168,180,21,43,31,193,0,78,204,136,124,
  38,161,20,160,254,24,131,234,200,143,12,31,203,237,179,203,145,63,188,179,219,169,3,173,
  18,203,185,95,237,190,9,178,185,125,119,101,19,213,242,52,224,64,87,34,44,90,214,248,
  171,86,234,141,89,102,230,248,119,180,1,254,148,12,240,155,64,153,90,208,22,248,13,243,
  251,202,157,114,10,128,241,48,101,85,232,249,16,61,118,252,249,85,71,100,218,240,167,26,
  248,164,203,57,78,237,57,174,76,195,178,145,169,107,134,134,8,219,177,75,230,172,137,171,
  72,55,195,44,243,171,142,19,42,9,60,62,11,199,128,177,194,27,133,134,210,87,0,180,
  149,200,57,78,103,37,242,75,40,84,84,121,247,100,239,250,52,121,16,69,193,36,3,26,
  0,223,28,110,93,5,205,62,246,55,197,196,87,42,6,32,37,117,227,68,46,163,28,0,
  63,171,48,151,9,115,36,204,3,128,245,88,22,150,49,168,48,119,227,68,46,163,28,232,
  16,15,226,175,95,243,7,197,215,175,141,70,254,32,253,250,117,84,188,140,153,96,62,147,
  161,1,188,130,223,133,66,70,23,44,154,116,49,217,69,242,16,143,36,81,227,23,212,248,
  37,104,78,113,206,68,171,50,6,61,201,220,56,145,203,40,71,61,25,127,253,154,61,40,
  2,234,74,70,93,97,78,219,121,63,192,109,111,140,169,31,132,200,23,188,19,0,167,4,
  120,156,61,197,183,107,97,111,45,199,29,3,247,86,111,109,8,0,26,65,175,95,84,31,
  110,198,171,195,120,223,242,165,14,179,200,129,170,99,248,42,191,194,118,202,176,85,102,248,
  251,37,36,18,220,85,59,70,44,17,146,231,8,209,237,90,98,240,49,82,123,64,20,121,
  234,28,33,158,58,65,136,167,194,169,142,72,189,140,82,145,122,24,165,34,245,72,35,2,
  152,142,51,253,133,189,242,66,127,97,161,15,194,171,240,56,124,30,190,9,159,134,111,195,
  199,225,179,240,110,248,17,30,130,8,162,28,61,136,206,190,251,14,127,95,60,108,28,71,
  227,240,105,52,13,159,69,61,164,70,231,225,152,45,244,65,212,15,175,162,89,248,60,26,
  134,111,162,65,248,22,8,194,227,104,18,222,141,46,195,143,209,33,22,249,0,169,87,200,
  245,28,169,111,144,235,45,82,31,35,215,221,232,16,57,46,131,160,115,38,26,232,163,129,
  33,26,152,83,3,151,97,95,52,64,197,199,162,248,84,20,239,137,226,231,172,129,177,232,
  194,84,116,161,39,186,112,206,186,128,44,199,72,126,138,164,103,136,70,221,135,226,188,240,
  146,125,81,114,40,74,206,69,201,75,86,119,95,180,62,20,173,207,69,235,151,172,245,32,
  60,54,161,174,2,168,239,163,131,237,227,240,117,116,133,191,9,249,179,57,14,15,203,232,
  121,251,125,243,105,248,168,140,222,180,95,35,144,37,209,91,196,60,11,127,47,163,199,136,
  121,22,254,179,140,238,34,102,4,202,102,244,17,49,8,124,142,254,89,54,127,47,219,191,
  55,179,36,156,163,239,248,106,63,42,155,255,44,195,55,101,148,37,205,71,72,43,155,135,
  108,193,56,81,243,51,57,68,153,151,244,247,77,105,95,193,28,57,67,158,74,38,28,157,
  207,15,88,212,3,22,5,100,177,50,170,171,50,250,220,154,151,173,55,172,177,171,210,59,
  244,183,112,175,84,54,27,159,155,79,145,181,249,12,153,49,12,9,151,81,8,184,204,91,
  52,216,121,75,77,216,135,244,109,185,125,85,226,52,85,28,252,122,30,102,94,95,189,26,
  240,214,59,168,245,57,255,16,117,154,206,63,196,125,66,206,63,116,173,102,6,125,145,220,
  156,79,190,8,15,211,155,50,49,94,36,197,240,199,184,24,245,107,248,24,149,244,27,179,
  50,148,112,82,53,74,11,254,236,26,38,207,76,54,72,230,203,101,202,199,45,101,153,8,
  233,50,55,66,233,75,50,159,122,35,203,240,80,52,233,87,13,248,200,134,93,41,173,173,
  255,253,15,199,140,131,114,148,224,209,184,216,245,91,143,136,54,33,244,152,14,54,107,12,
  66,216,201,255,6,70,142,182,102,167,130,225,250,50,98,230,58,10,105,174,112,29,73,175,
  234,202,90,114,143,43,120,76,210,184,148,8,248,22,91,142,188,34,28,232,85,63,13,87,
  9,245,133,235,137,237,133,245,170,59,225,77,52,115,194,117,85,111,194,245,85,107,106,216,
  107,111,135,146,89,246,227,136,133,62,141,194,30,167,32,190,29,133,207,134,210,95,188,36,
  42,94,200,192,92,6,238,74,10,228,145,140,249,73,150,122,161,50,39,235,240,211,126,75,
  4,89,242,48,245,66,46,15,176,18,7,69,241,155,74,135,230,43,217,34,89,62,29,30,
  199,57,168,212,143,71,108,189,98,100,230,68,219,74,142,231,233,57,206,25,222,6,133,149,
  67,48,166,4,63,155,177,152,95,232,66,69,99,45,234,181,183,153,170,140,179,183,51,53,
  165,183,138,49,120,67,204,112,147,127,156,203,170,55,102,67,216,124,193,42,141,62,165,13,
  114,220,226,207,24,184,116,117,199,128,147,74,8,30,150,6,195,136,119,178,227,16,225,141,
  149,83,65,190,53,125,51,108,58,88,147,185,121,39,21,91,146,164,100,76,235,53,9,123,
  214,166,202,234,139,146,152,57,73,79,118,78,185,75,20,67,11,96,201,244,159,156,46,219,
  75,215,11,83,216,134,236,209,72,145,156,252,126,204,28,144,94,115,46,81,65,114,55,36,
  207,246,245,171,112,22,3,214,75,125,179,156,1,179,19,44,105,27,230,115,162,28,110,51,
  201,145,206,91,38,229,166,25,125,206,43,90,77,49,56,207,91,202,83,43,137,187,240,2,
  228,248,216,157,80,157,24,203,68,177,32,135,9,241,122,46,146,110,194,164,70,29,39,161,
  204,201,163,100,141,213,142,143,185,0,252,238,187,44,184,62,138,37,163,13,44,48,53,153,
  99,76,102,63,146,6,125,192,163,234,119,199,114,50,241,0,58,25,159,226,5,148,227,167,
  59,19,44,196,187,99,111,111,166,228,114,50,126,136,86,92,66,212,221,113,136,39,173,55,
  129,145,95,113,112,103,32,255,38,140,176,123,164,77,5,37,107,243,168,249,134,244,241,168,
  253,107,99,157,47,233,241,206,96,237,118,29,171,118,103,132,51,96,43,113,74,146,226,45,
  50,83,138,83,12,254,71,43,29,64,181,39,72,205,110,81,150,98,97,197,64,27,126,148,
  28,42,226,29,6,91,146,213,193,120,133,65,120,167,193,42,75,75,98,140,115,90,10,10,
  104,26,17,83,152,65,140,75,230,235,197,184,93,2,209,215,175,95,85,25,15,241,29,249,
  30,52,24,71,178,45,90,109,54,247,24,181,224,237,144,119,45,15,80,63,73,120,54,42,
  221,181,70,243,118,72,29,214,163,198,251,252,142,156,45,179,143,236,81,111,101,227,67,145,
  166,232,122,98,98,77,241,55,238,69,245,199,17,204,14,44,234,211,165,151,85,185,170,106,
  213,99,103,213,51,108,250,17,9,109,224,205,159,123,207,106,223,142,159,93,128,34,224,196,
  236,210,225,48,163,132,67,79,80,214,33,61,159,205,166,5,104,5,249,214,32,143,47,153,
  193,23,198,155,150,178,133,8,219,160,29,130,89,242,88,246,112,44,33,221,33,143,101,239,
  254,188,219,147,199,114,18,13,79,122,167,32,46,196,39,19,53,30,38,127,114,10,58,131,
  34,62,79,184,71,204,112,32,61,99,30,106,42,121,198,111,219,80,69,136,204,173,137,72,
  16,133,90,3,254,29,104,128,113,132,87,245,89,116,216,61,186,127,214,61,106,69,251,188,
  79,47,64,43,1,220,121,223,56,2,181,66,133,91,96,169,95,233,175,189,160,91,68,103,
  49,119,55,8,113,81,208,101,250,33,216,26,225,11,162,112,48,122,118,3,40,20,120,215,
  108,52,188,187,231,96,34,230,141,163,237,125,164,179,52,123,200,145,51,5,161,96,169,131,
  218,197,125,66,95,243,233,52,104,242,106,66,230,213,9,113,199,173,134,61,1,65,225,60,
  154,119,39,247,207,187,19,53,236,75,57,184,9,77,175,12,211,176,143,244,151,53,236,216,
  24,246,101,120,24,30,45,29,246,132,134,237,142,136,72,3,90,3,255,175,220,70,227,111,
  191,141,142,176,125,176,111,176,109,176,95,254,99,183,203,248,182,219,101,130,233,195,254,192,
  246,152,88,195,251,131,219,194,148,181,120,115,110,202,90,16,129,92,24,2,236,50,166,15,
  147,227,143,162,127,38,15,241,174,240,8,186,196,40,83,132,120,9,100,65,199,159,131,215,
  170,106,130,99,207,140,120,158,2,170,153,164,166,23,226,26,68,250,11,231,222,27,89,162,
  84,12,141,233,131,110,234,151,158,121,17,43,158,65,255,126,34,132,104,250,15,18,186,178,
  56,117,74,75,208,244,133,4,13,181,45,36,104,132,12,204,8,62,89,229,36,157,197,238,
  36,17,23,1,148,185,173,42,146,151,225,117,70,194,35,213,148,49,158,107,254,148,62,222,
  111,124,84,179,200,88,144,171,24,37,144,132,39,28,195,155,103,124,105,166,252,25,215,229,
  114,62,63,2,231,236,99,153,7,141,159,134,186,4,231,247,53,102,184,117,34,87,30,8,
  130,200,6,86,200,134,130,125,36,132,129,192,184,20,229,118,151,22,204,171,5,99,86,80,
  250,116,95,82,54,182,202,62,2,66,103,242,116,82,131,14,71,118,33,103,53,142,193,73,
  78,134,205,217,48,186,142,59,89,120,214,161,58,133,243,93,41,133,99,28,222,206,206,66,
  76,25,215,159,106,168,201,26,42,151,186,51,118,130,64,25,158,109,157,201,121,141,166,18,
  197,156,49,97,159,223,99,231,245,252,175,202,235,153,104,94,9,30,167,41,254,153,108,167,
  95,18,32,5,248,3,22,190,124,82,179,188,114,66,102,44,55,165,202,135,54,9,96,11,
  35,199,234,189,205,172,100,70,215,204,15,98,25,50,18,69,39,9,135,9,145,131,58,233,
  66,190,228,152,15,196,228,197,104,10,234,146,118,34,122,14,21,128,15,234,107,150,66,215,
  230,183,71,160,36,165,36,64,1,65,207,69,151,198,248,101,36,105,14,191,156,59,98,161,
  79,36,21,225,178,168,21,104,26,167,130,116,128,64,42,3,133,12,228,50,16,139,128,120,
  232,77,185,138,195,137,113,208,78,165,164,147,140,144,239,54,163,128,60,235,25,94,144,138,
  234,150,157,236,158,42,145,167,236,100,239,84,186,168,196,199,190,248,32,134,241,201,247,167,
  18,103,197,199,61,241,17,59,98,82,198,67,87,180,41,33,55,36,194,187,233,253,31,186,
  144,99,15,240,136,149,29,16,217,16,97,57,190,94,8,1,208,87,220,1,60,211,100,227,
  46,183,49,95,143,104,174,128,212,94,107,47,161,162,26,198,19,135,69,86,90,34,0,127,
  60,176,118,78,9,35,197,24,9,29,197,232,8,251,196,184,8,229,196,136,216,27,236,222,
  41,225,149,39,63,156,18,78,121,242,247,83,146,10,57,249,199,41,238,165,252,228,127,159,
  130,237,132,226,59,116,161,227,119,151,238,112,252,162,162,67,250,69,77,71,244,139,170,206,
  232,247,30,127,153,211,4,99,4,228,95,21,160,18,125,105,244,193,110,31,180,103,225,121,
  187,23,158,181,47,109,9,154,130,214,192,201,223,66,254,22,242,183,144,191,85,201,191,87,
  205,159,33,255,20,249,231,200,127,232,230,223,175,246,7,249,219,200,223,70,254,182,147,63,
  13,10,90,110,187,4,176,239,112,130,11,212,169,249,94,181,230,49,106,30,162,230,9,106,
  182,243,119,37,158,229,169,127,89,185,144,46,196,71,212,173,106,115,45,20,107,161,88,11,
  197,90,53,205,81,241,139,194,87,220,55,42,94,172,28,230,217,37,227,103,60,201,97,60,
  179,177,201,157,201,138,23,243,86,221,6,13,58,27,144,108,70,77,131,13,6,20,71,41,
  96,203,70,49,135,200,246,4,226,219,173,196,222,231,213,199,182,16,34,177,31,184,26,45,
  45,235,94,205,101,237,171,249,139,122,89,58,175,102,135,145,100,163,14,10,107,75,12,26,
  23,72,38,53,205,39,43,155,79,110,210,252,194,203,151,150,50,39,95,70,182,32,10,23,
  165,15,174,169,41,46,81,107,144,100,248,24,126,57,183,228,134,165,20,138,92,12,148,228,
  50,40,209,214,223,119,254,190,187,243,195,223,255,177,187,251,143,31,238,125,255,247,31,90,
  9,13,97,73,95,195,245,58,41,165,101,42,48,50,100,90,105,92,30,168,128,88,156,20,
  230,177,180,224,0,58,137,26,71,155,153,40,113,174,28,72,26,220,47,4,142,120,103,87,
  12,234,206,78,149,221,191,30,128,214,196,70,6,207,158,128,213,30,201,71,248,214,21,184,
  152,66,42,163,35,100,49,112,215,145,176,176,204,49,87,57,230,34,199,156,114,124,209,57,
  190,168,28,95,68,142,47,97,81,25,212,147,56,80,44,220,59,187,11,53,42,155,108,179,
  246,168,216,220,165,213,185,43,205,86,244,220,221,156,179,201,48,130,121,126,83,238,38,177,
  58,150,113,55,43,233,183,224,110,58,236,189,177,143,171,55,254,235,152,121,30,131,103,161,
  223,180,89,88,103,195,172,134,81,244,65,241,123,100,224,157,98,29,141,114,201,58,58,86,
  172,163,143,146,45,244,94,114,131,62,198,55,98,2,205,115,239,2,123,214,244,223,200,4,
  186,37,223,231,223,192,47,17,183,6,205,135,52,127,44,57,38,165,77,106,228,239,108,70,
  91,140,76,53,98,97,68,185,66,103,132,181,163,29,109,187,191,192,214,207,163,132,19,20,
  186,197,253,188,91,0,36,124,136,189,68,247,36,44,240,104,194,150,169,75,5,142,116,82,
  192,123,251,9,242,157,178,112,43,66,93,198,13,115,23,239,212,210,209,60,27,27,131,228,
  58,104,179,4,18,127,164,126,198,80,14,56,182,103,27,99,203,63,41,157,141,3,22,207,
  120,194,27,89,58,158,111,96,176,76,149,117,227,18,250,187,27,105,150,182,217,252,36,131,
  13,222,233,103,146,219,180,89,135,118,252,57,28,0,121,55,50,132,28,106,158,241,164,216,
  98,35,3,86,5,246,103,134,20,226,202,88,4,220,91,240,3,142,107,249,1,199,14,93,
  164,96,81,252,98,109,65,62,219,209,71,146,215,243,113,44,8,230,66,251,232,221,80,190,
  128,52,161,126,148,215,19,234,223,13,37,235,40,223,110,24,70,14,161,33,99,124,204,205,
  143,47,1,81,157,32,32,215,204,66,225,185,156,67,12,33,171,95,60,220,235,236,226,189,
  146,10,178,250,208,207,2,99,100,15,75,189,126,96,82,219,98,73,109,235,105,106,219,76,
  80,219,68,90,43,118,169,109,115,136,141,129,226,214,222,237,206,239,79,186,80,237,233,243,
  154,207,163,25,39,194,194,244,205,165,10,19,121,246,48,186,20,148,183,18,243,132,167,250,
  57,200,110,243,160,123,72,8,34,163,171,29,6,228,14,195,24,230,207,89,54,149,194,246,
  162,170,30,29,190,137,252,2,130,117,238,86,11,105,192,144,114,117,207,85,197,231,38,165,
  113,229,216,135,183,31,123,165,43,24,57,141,209,234,74,253,24,157,226,104,129,148,20,104,
  44,115,85,193,156,104,143,255,195,73,190,49,39,217,32,215,94,58,148,72,217,250,72,207,
  91,205,41,170,185,19,50,130,250,119,235,210,114,34,28,251,52,109,62,196,40,4,172,2,
  248,68,240,32,149,112,229,227,120,13,178,109,157,210,227,199,49,91,210,59,141,190,210,124,
  236,115,205,71,41,182,88,37,219,190,31,74,178,237,178,134,67,6,98,58,177,161,232,168,
  8,229,252,83,17,251,44,53,72,144,128,25,193,79,162,76,31,100,224,95,46,238,244,49,
  182,113,39,186,74,56,182,164,49,14,27,252,121,16,39,153,180,249,231,98,15,38,242,176,
  83,65,30,162,189,224,247,97,61,126,240,161,54,141,224,36,195,30,10,169,138,160,176,136,
  214,46,195,41,78,91,168,217,88,240,15,195,219,227,18,114,174,254,124,156,130,63,127,142,
  242,85,68,95,128,17,208,240,70,54,139,0,162,10,122,31,84,146,228,190,56,152,157,37,
  46,145,87,210,105,73,140,100,131,251,69,106,56,98,237,20,201,204,108,136,100,105,165,137,
  211,135,75,188,18,122,163,21,189,174,178,52,106,122,171,251,26,131,12,84,184,189,53,12,
  125,32,78,204,217,213,58,173,71,207,85,59,68,25,31,211,31,204,77,244,54,37,229,11,
  70,53,162,251,255,45,212,158,232,247,112,20,212,80,176,30,115,211,70,172,95,157,13,146,
  19,142,193,88,3,22,72,6,4,18,102,7,102,195,200,249,148,103,200,172,200,35,97,88,
  136,165,109,42,238,2,39,177,151,146,196,14,75,22,148,185,51,93,116,249,108,13,141,153,
  154,17,99,78,147,239,141,78,85,150,118,57,161,158,54,54,228,45,158,10,200,207,21,22,
  86,63,78,139,108,6,59,50,12,78,189,40,26,30,41,50,182,59,130,192,223,72,233,198,
  240,215,140,242,157,40,96,11,111,87,197,118,13,181,95,167,62,79,35,97,130,157,249,79,
  23,130,94,141,220,189,65,59,35,165,19,197,38,151,246,5,223,33,64,39,223,166,82,228,
  200,93,155,82,172,205,238,2,219,231,100,22,170,255,78,187,114,103,215,238,109,118,108,166,
  230,161,244,46,159,123,94,255,208,9,125,145,175,195,76,114,158,222,124,141,101,15,74,217,
  173,39,168,34,79,227,177,209,181,117,247,139,170,203,254,150,212,134,11,121,146,71,170,159,
  191,185,253,220,213,28,47,246,38,221,197,154,237,58,29,231,119,29,104,118,18,216,10,90,
  1,123,74,37,244,104,169,61,108,41,103,116,73,184,15,181,31,158,65,69,228,60,163,250,
  142,23,134,180,91,183,48,25,226,184,199,114,243,155,28,209,153,223,130,23,59,38,152,222,
  167,63,51,250,51,165,63,67,92,153,3,16,223,122,141,205,47,155,225,38,6,176,121,181,
  9,149,125,250,31,59,27,155,139,56,226,64,235,236,12,42,189,205,51,236,178,12,72,144,
  153,200,110,76,201,114,16,119,127,175,154,220,230,233,109,158,97,95,101,152,243,76,34,131,
  220,217,223,187,233,109,149,161,205,115,72,138,154,50,70,51,214,17,181,38,104,112,23,247,
  209,182,55,39,39,129,170,124,179,186,124,210,78,205,140,132,0,247,2,195,59,97,175,65,
  239,159,115,46,209,16,158,113,193,141,240,152,31,245,231,209,209,246,1,20,118,206,182,175,
  160,247,115,180,189,7,197,157,51,252,125,28,189,192,223,103,209,1,94,43,119,163,43,252,
  253,136,37,130,70,208,78,248,94,240,201,37,186,243,26,235,246,250,254,221,238,107,137,180,
  67,141,231,117,243,77,251,173,202,1,133,158,157,238,97,121,255,25,254,200,76,143,152,130,
  206,243,246,211,238,251,147,249,105,4,61,157,203,240,253,201,228,148,84,98,14,17,58,63,
  141,30,135,125,142,206,191,7,205,248,61,168,194,239,241,252,13,89,246,29,158,119,135,103,
  124,1,250,240,110,7,11,49,243,228,159,138,151,100,185,125,160,62,118,219,175,183,33,187,
  242,177,69,156,89,123,28,87,108,28,50,46,161,158,39,229,253,3,252,145,61,199,104,134,
  45,124,62,107,190,38,13,37,17,110,188,38,76,13,138,74,195,86,3,17,187,129,138,251,
  189,52,227,94,119,199,178,67,40,141,68,188,230,121,4,190,160,184,68,17,163,178,21,253,
  176,200,72,20,243,25,201,175,53,6,136,194,138,129,117,21,33,48,108,69,31,23,107,192,
  32,227,248,123,46,44,157,42,184,178,5,161,117,253,13,237,25,210,34,102,143,26,36,110,
  207,40,173,91,28,68,72,175,63,161,72,144,32,66,101,48,34,44,24,34,105,223,103,249,
  205,192,223,254,158,128,127,184,203,51,172,253,152,3,151,87,207,33,39,233,3,136,7,243,
  49,225,162,249,18,168,200,73,60,199,217,20,144,145,135,127,204,202,50,155,24,140,127,22,
  15,179,176,75,32,100,54,77,210,39,104,105,208,33,51,116,176,212,125,68,84,130,78,198,
  63,126,102,239,218,206,120,33,31,106,107,0,78,33,180,162,193,227,9,241,155,79,136,219,
  76,178,100,39,196,104,78,113,64,207,177,61,47,27,4,103,133,12,106,73,162,29,135,141,
  59,0,149,137,8,238,6,46,76,154,173,7,147,166,107,194,164,225,26,48,105,96,195,164,
  75,241,236,58,18,47,191,51,169,75,129,225,29,68,141,164,13,5,234,84,193,143,43,140,
  242,234,126,148,119,175,228,25,60,166,41,120,14,165,193,28,176,235,121,147,21,104,149,170,
  192,83,20,120,122,31,134,32,159,202,2,111,163,167,219,5,160,218,219,230,24,188,239,103,
  218,228,210,99,60,211,181,57,166,199,65,247,12,220,171,55,208,48,60,3,143,170,253,188,
  153,18,167,24,204,168,55,205,187,18,122,32,7,165,82,52,68,12,25,243,240,25,96,234,
  93,155,9,61,228,153,143,144,249,8,153,143,40,243,128,199,189,13,119,219,207,65,98,228,
  95,36,11,184,152,243,240,113,176,176,7,93,176,49,203,184,99,196,29,227,61,121,44,135,
  245,60,154,159,28,159,158,64,88,240,13,133,240,34,164,240,83,21,198,31,210,196,100,121,
  16,238,54,152,206,226,49,9,145,7,76,172,136,181,74,58,173,111,33,29,70,34,114,97,
  35,145,89,242,246,174,145,137,169,189,138,76,139,190,134,77,231,184,80,176,219,206,91,209,
  11,45,217,117,8,17,84,214,193,51,136,224,189,144,6,231,14,164,158,13,86,249,56,58,
  98,102,11,31,18,46,242,92,126,16,20,55,86,113,87,175,162,152,251,157,112,210,124,78,
  237,13,229,55,251,18,19,43,100,105,48,163,236,0,189,137,122,75,182,196,99,218,18,230,
  118,224,27,64,110,7,190,57,186,7,216,14,199,88,251,3,108,7,106,250,0,123,225,24,
  219,67,244,7,201,148,68,209,110,159,94,160,228,51,24,47,105,161,87,47,80,250,46,194,
  205,231,244,37,122,139,12,148,192,250,187,176,251,105,237,220,179,214,83,236,220,55,173,167,
  93,49,75,98,69,30,135,143,113,47,191,13,58,242,27,95,143,105,141,96,96,105,223,94,
  34,236,65,57,193,123,108,173,174,254,146,11,68,129,219,80,134,57,184,53,239,20,27,220,
  250,46,19,5,110,153,145,106,9,110,229,7,7,183,242,142,249,148,122,164,169,196,115,65,
  220,18,179,252,66,50,215,96,7,156,23,127,60,186,96,150,87,139,104,111,199,247,22,151,
  143,201,184,63,76,30,201,66,5,127,76,226,221,194,57,197,193,53,145,85,88,253,157,141,
  45,29,13,42,9,61,93,166,92,242,40,25,128,52,162,10,61,242,49,76,202,55,101,118,
  140,87,44,25,172,53,222,133,50,85,240,244,73,97,95,126,225,77,20,221,147,207,74,131,
  52,197,25,220,81,201,5,171,248,182,113,170,217,38,1,97,165,252,129,248,35,200,178,37,
  131,63,86,237,35,95,197,124,214,92,2,156,136,198,163,79,141,147,60,0,176,72,16,190,
  22,70,14,81,166,186,106,92,42,198,179,66,210,144,154,29,43,106,199,9,1,146,246,221,
  119,119,220,245,14,44,203,193,118,217,174,103,115,116,229,4,209,179,33,178,231,119,135,238,
  122,224,148,146,80,173,230,16,207,186,110,76,115,24,99,14,83,167,84,140,201,11,243,86,
  100,81,118,11,37,126,156,147,193,151,180,187,164,155,96,15,39,130,71,160,35,229,65,176,
  201,76,97,101,33,22,238,22,36,230,181,162,137,166,213,149,11,57,9,84,178,17,194,184,
  155,60,140,163,164,3,245,143,38,236,50,99,25,197,35,146,144,56,124,134,125,54,13,221,
  236,126,52,238,146,228,133,133,30,101,173,6,204,48,5,219,123,100,198,137,8,158,144,142,
  235,67,8,3,204,129,214,174,146,218,234,131,89,49,142,64,27,21,178,72,0,245,103,121,
  18,255,182,96,213,141,57,17,53,130,225,13,9,239,182,27,116,193,9,76,139,82,137,89,
  198,168,170,144,188,27,68,141,184,61,11,182,135,82,29,191,104,13,2,94,130,102,227,152,
  216,144,169,113,86,177,204,237,221,164,253,61,177,48,91,20,0,221,119,135,25,217,160,5,
  127,176,203,172,62,238,90,214,29,213,226,50,11,59,118,84,78,60,62,176,60,26,100,167,
  146,43,106,237,61,228,183,40,151,193,85,135,99,44,133,187,153,22,87,108,227,32,99,163,
  175,183,129,44,114,152,12,182,8,146,240,83,88,225,37,105,54,192,126,166,236,163,100,43,
  165,177,62,198,250,201,233,79,76,127,50,17,61,22,114,20,106,195,211,123,127,64,27,126,
  32,175,186,94,52,216,6,193,252,100,112,26,57,61,64,231,123,66,170,121,65,98,154,162,
  206,88,5,5,110,253,146,249,201,218,122,241,232,125,239,237,163,159,223,60,49,237,124,49,
  57,75,188,250,166,78,212,60,144,166,171,84,20,140,121,206,238,71,125,44,90,31,246,48,
  82,105,15,19,183,58,138,203,132,169,72,96,150,106,201,116,21,75,72,149,48,25,61,7,
  183,250,57,232,227,210,142,19,213,29,58,134,245,114,38,94,91,201,5,97,253,216,159,66,
  5,2,99,18,119,141,73,36,53,67,154,61,252,105,239,42,195,231,168,9,113,177,29,87,
  233,26,165,134,52,247,148,54,22,160,249,129,152,208,39,175,142,158,255,252,203,203,224,58,
  179,68,31,13,53,141,152,176,167,47,165,168,136,73,156,243,202,218,100,60,20,59,11,31,
  54,51,9,146,231,241,111,137,212,90,127,116,53,42,192,25,235,129,35,65,221,173,244,142,
  6,142,234,22,66,62,243,206,142,197,182,150,173,179,217,164,182,243,147,82,183,61,216,6,
  53,175,144,73,206,200,115,62,169,200,206,196,226,27,48,17,54,208,243,219,195,252,246,104,
  126,9,75,207,161,170,179,114,12,5,233,243,12,154,61,146,220,167,252,206,64,122,212,88,
  239,84,138,44,94,151,124,131,211,187,146,207,44,61,40,161,174,47,194,241,226,246,190,37,
  204,195,236,193,108,74,79,164,69,151,150,23,243,53,57,159,98,98,241,215,194,186,120,231,
  251,173,191,135,220,52,185,64,159,4,185,61,203,69,140,160,98,111,2,13,83,130,34,190,
  62,248,251,70,184,28,243,107,38,209,180,176,92,120,208,202,155,143,108,17,62,113,41,195,
  159,28,129,59,220,73,9,254,105,138,235,142,164,98,18,161,1,224,12,200,31,244,117,43,
  98,91,79,198,224,63,20,9,27,189,195,133,52,147,36,166,249,94,18,152,227,15,82,116,
  235,234,144,203,148,10,225,180,185,248,148,54,235,25,150,251,136,148,127,34,97,9,46,6,
  22,204,35,132,249,251,248,96,156,245,127,187,28,21,73,36,204,52,196,202,48,136,129,152,
  150,161,116,133,35,33,119,34,173,16,179,49,10,173,77,85,125,219,237,128,156,17,2,152,
  176,205,102,3,9,126,149,231,247,119,186,1,48,150,66,124,62,40,240,217,198,103,238,100,
  199,145,139,31,98,150,59,121,36,30,18,230,48,184,101,251,59,49,187,63,97,184,10,57,
  219,56,33,17,217,165,51,213,174,141,206,181,202,38,68,37,68,244,251,150,57,181,77,245,
  206,203,2,41,62,19,127,104,153,211,221,84,143,191,140,241,205,237,89,100,15,103,193,35,
  81,117,217,89,212,5,67,117,184,73,195,104,44,38,243,61,80,140,190,8,127,232,142,97,
  29,113,214,30,192,52,161,76,237,35,102,218,26,52,103,34,230,131,148,113,230,23,12,241,
  87,214,120,193,209,54,163,218,228,78,195,199,7,123,179,149,50,100,111,186,82,134,170,155,
  175,180,118,130,179,17,75,29,174,172,101,105,124,184,187,179,212,97,47,12,178,88,99,129,
  1,83,222,203,133,38,103,67,31,228,146,134,165,30,160,49,218,176,212,3,52,70,27,150,
  246,0,221,221,94,154,3,180,198,27,150,214,0,237,1,135,165,57,64,107,188,126,120,198,
  7,105,36,252,119,95,196,69,248,217,101,72,62,201,235,88,229,54,147,92,70,74,240,138,
  87,139,15,180,202,232,77,83,154,231,101,95,236,155,17,57,150,80,0,125,71,231,0,216,
  226,108,74,224,76,4,77,1,46,147,168,189,15,35,164,251,205,184,189,199,204,189,166,209,
  94,51,71,56,110,101,173,177,188,174,71,112,136,114,16,151,208,153,27,31,130,76,44,43,
  108,152,172,126,166,136,215,111,54,178,54,208,120,252,142,219,80,6,94,132,84,244,101,150,
  206,210,17,113,209,151,86,66,26,168,124,4,67,122,135,228,193,118,191,205,170,219,134,6,
  203,44,104,33,28,7,219,236,149,194,67,109,214,200,118,99,214,154,6,226,181,52,237,194,
  6,45,178,208,31,222,37,216,183,69,55,32,70,216,55,154,148,6,249,114,0,205,12,134,
  110,115,121,182,70,4,71,91,9,134,159,54,179,5,23,217,185,171,164,156,149,200,14,71,
  235,95,246,195,127,170,208,103,21,122,84,172,186,108,153,120,199,29,162,240,111,146,78,67,
  62,154,146,163,203,77,92,71,32,228,185,87,172,158,51,182,230,251,206,94,112,147,37,189,
  159,81,46,148,79,6,178,70,61,144,87,110,159,50,50,23,170,226,210,101,166,155,176,149,
  139,234,109,249,200,188,44,141,154,113,93,22,234,161,27,225,137,216,48,26,130,192,14,48,
  208,38,88,59,54,39,20,23,84,220,206,186,102,206,172,21,101,15,72,194,167,97,228,84,
  215,44,86,52,15,192,66,106,230,29,210,242,197,165,72,198,239,241,30,197,205,8,249,209,
  246,30,106,20,239,75,108,31,179,98,178,9,185,243,176,15,73,58,108,150,221,224,255,205,
  79,59,141,143,67,211,190,44,123,87,144,90,25,55,235,76,159,116,61,126,20,226,169,83,
  20,205,80,12,183,23,213,209,98,117,168,187,81,53,210,218,187,159,63,156,241,44,123,188,
  153,187,78,51,57,127,99,160,183,170,41,138,34,115,204,119,165,114,166,94,146,200,222,19,
  176,81,90,77,31,66,184,12,105,242,49,80,159,227,225,214,189,206,214,222,61,41,213,57,
  205,46,27,125,143,21,14,40,248,14,148,146,57,101,154,122,50,13,89,166,137,206,52,244,
  100,154,81,38,136,131,18,49,0,107,4,131,107,32,244,202,175,94,132,87,231,68,126,77,
  232,235,95,227,173,58,8,129,158,94,129,224,12,105,94,50,168,138,127,61,18,105,13,112,
  228,150,21,129,153,85,252,27,146,13,77,252,19,69,62,47,45,2,67,155,100,136,21,255,
  102,100,86,147,21,89,8,253,182,202,196,242,130,184,172,54,49,4,209,251,37,125,54,143,
  151,234,250,146,14,219,249,63,123,242,219,189,181,242,107,187,185,12,91,66,239,8,236,17,
  51,254,159,58,248,89,5,33,98,167,144,41,31,29,156,67,16,131,224,201,47,148,82,164,
  72,209,213,228,126,218,77,180,66,148,76,62,73,78,187,70,61,194,98,128,124,137,187,126,
  201,24,120,42,69,192,61,18,165,14,219,240,170,148,161,117,176,167,178,126,68,70,55,107,
  6,165,115,176,113,57,131,42,51,174,240,97,12,75,141,73,143,47,44,205,49,89,67,12,
  75,61,38,99,128,14,194,84,131,41,253,25,11,197,61,72,152,46,67,139,63,103,205,12,
  4,230,95,67,45,142,204,59,9,83,196,41,120,156,220,22,126,163,104,115,179,248,16,80,
  110,142,0,248,71,205,76,210,41,247,154,9,112,150,180,149,183,226,0,204,173,6,144,154,
  4,72,77,218,38,100,6,81,25,220,115,140,90,137,102,213,61,101,134,17,36,81,127,183,
  61,82,52,250,102,210,44,117,190,31,69,62,145,186,215,132,20,197,40,104,142,204,60,95,
  236,60,35,59,21,198,5,196,168,84,14,209,120,139,85,158,4,45,86,1,236,132,170,50,
  191,45,235,157,213,191,183,117,57,247,89,78,171,39,207,236,126,238,171,177,88,185,126,170,
  142,198,74,207,114,115,149,68,46,209,225,214,91,49,162,103,124,68,45,86,89,33,252,154,
  196,171,208,34,233,238,75,6,82,25,40,212,195,221,197,141,102,103,163,254,143,201,151,81,
  146,251,240,100,55,89,224,70,23,59,18,47,130,93,10,233,249,110,79,34,67,23,251,62,
  60,200,166,26,136,106,36,205,224,98,87,218,120,186,216,147,84,237,139,125,27,26,51,180,
  31,143,151,144,156,192,144,93,230,140,200,178,34,118,30,146,67,24,178,209,12,235,207,22,
  96,174,127,229,94,236,72,29,155,11,41,249,127,177,171,162,118,101,212,158,138,218,147,81,
  251,42,106,63,184,209,187,147,38,78,52,173,64,94,72,141,201,105,176,163,247,228,156,216,
  209,251,114,130,204,232,27,188,12,209,186,233,201,216,28,189,21,111,76,129,21,111,204,131,
  21,47,39,3,108,222,108,157,173,170,60,237,132,154,9,32,80,230,21,219,116,127,197,62,
  221,255,3,27,245,209,191,111,159,202,216,47,136,253,18,146,93,113,88,45,255,159,221,251,
  87,239,222,36,187,33,160,245,42,227,250,224,169,138,223,84,187,178,84,187,50,89,10,52,
  213,50,0,139,222,125,104,58,29,193,136,224,197,192,142,208,110,153,104,30,82,215,90,143,
  240,196,35,51,208,142,115,69,9,188,210,2,165,144,22,48,24,153,170,159,74,236,220,116,
  65,34,142,138,108,200,100,3,121,152,140,53,124,196,146,243,17,87,158,136,117,182,255,218,
  123,253,38,155,250,38,187,247,102,187,20,187,49,189,25,44,173,223,139,251,117,155,113,127,
  205,221,248,232,191,199,102,124,244,63,123,241,207,218,139,163,236,182,40,104,101,91,190,158,
  197,208,204,46,151,97,159,190,44,43,46,246,63,128,119,218,183,120,236,220,226,96,203,58,
  55,248,159,124,79,255,59,47,229,127,255,237,75,110,166,243,91,225,136,107,109,164,253,53,
  118,210,254,141,183,210,163,63,107,39,201,24,133,7,254,207,222,250,99,123,171,200,86,115,
  16,60,94,204,199,53,104,155,145,226,112,7,214,130,55,138,224,223,40,148,68,34,17,247,
  99,71,237,137,8,242,228,48,173,56,137,153,214,110,220,137,137,242,77,52,248,152,212,158,
  240,243,64,213,176,247,80,87,214,137,73,98,123,106,103,216,183,51,236,157,58,196,205,33,
  228,102,198,228,48,73,80,90,167,180,53,69,236,60,236,11,122,234,212,1,116,127,41,157,
  243,63,138,64,249,87,82,22,97,0,114,9,105,17,7,224,221,185,148,189,62,207,147,4,
  40,206,117,175,55,133,87,233,172,215,227,74,245,146,3,218,249,28,135,46,7,172,243,168,
  8,221,23,117,231,117,92,137,219,239,148,89,104,74,169,116,158,228,161,194,42,59,73,166,
  63,246,59,105,22,250,160,112,103,228,143,223,239,28,231,161,113,238,58,69,182,8,184,237,
  171,155,170,63,122,181,124,152,211,187,155,40,62,218,10,143,169,171,206,83,44,164,27,82,
  232,215,64,104,117,123,207,230,221,49,215,162,182,242,14,60,149,181,200,232,202,184,69,166,
  200,203,109,114,25,154,108,143,165,198,206,201,169,84,217,49,182,15,105,238,156,223,159,117,
  207,229,102,185,140,206,155,195,118,172,149,230,72,103,14,142,0,14,101,134,163,232,176,57,
  109,231,221,158,80,44,9,219,151,164,102,48,151,74,7,76,226,112,34,148,203,182,51,21,
  222,109,159,111,143,3,173,231,198,91,30,179,150,101,220,37,233,17,221,207,186,151,74,201,
  45,186,108,245,155,231,80,27,160,223,198,57,41,178,157,33,188,171,190,94,136,175,243,174,
  80,103,32,237,194,23,74,21,131,105,26,6,11,91,241,104,176,158,226,81,111,77,197,163,
  249,26,138,71,19,82,60,250,143,80,150,91,174,36,71,174,174,46,215,80,136,3,213,159,
  159,8,104,195,121,228,211,188,71,228,16,210,189,75,78,200,40,133,236,30,23,20,193,57,
  201,48,131,242,43,17,234,108,250,176,76,135,35,67,19,206,80,118,203,45,101,183,24,175,
  93,109,84,103,63,212,110,238,233,27,135,89,10,111,157,144,169,85,71,31,24,103,85,168,
  128,145,225,126,142,51,246,196,149,108,88,220,217,1,115,21,242,100,115,218,178,218,226,247,
  14,172,125,67,20,126,34,55,50,12,177,182,38,219,105,51,198,54,189,130,63,94,37,181,
  117,78,91,117,46,99,72,88,235,92,169,98,34,43,37,226,31,180,122,250,214,1,235,161,
  22,74,223,78,112,8,0,27,122,168,3,223,115,249,45,116,113,122,196,165,197,141,187,152,
  182,162,225,194,238,180,232,51,239,239,188,217,72,91,187,129,3,23,82,11,46,180,200,96,
  249,37,59,139,200,203,206,97,218,218,227,39,176,155,89,167,47,179,78,159,115,252,178,245,
  142,223,120,205,227,215,95,227,248,205,254,170,227,103,108,106,82,25,210,155,90,41,13,233,
  147,104,108,234,53,84,138,14,138,245,174,173,173,239,233,222,162,83,250,253,63,164,126,39,
  19,175,140,113,100,179,21,71,246,56,131,165,181,149,138,171,56,174,37,140,38,224,156,58,
  74,170,41,197,147,159,49,227,176,66,38,86,158,82,126,100,99,235,200,102,234,200,170,123,
  206,81,82,93,162,189,207,15,169,125,88,31,185,103,53,53,247,125,220,154,227,84,102,206,
  190,39,113,81,99,227,67,63,47,103,231,182,81,66,18,73,29,221,9,36,106,212,199,37,
  63,199,53,57,232,92,179,28,95,162,68,199,76,212,169,182,78,58,68,21,160,160,98,214,
  12,97,4,25,35,106,234,153,148,168,1,76,138,91,210,252,246,233,199,63,173,128,142,193,
  168,48,134,110,92,200,16,13,209,211,163,33,217,46,65,178,194,132,100,13,178,6,212,156,
  183,38,144,107,191,20,95,141,57,158,32,44,230,208,142,1,184,80,249,165,206,57,25,1,
  56,82,80,142,187,56,88,252,103,216,43,248,235,212,17,233,148,227,16,249,84,15,157,67,
  20,50,193,242,53,64,196,163,53,46,114,234,196,113,46,196,11,184,29,9,96,115,234,211,
  248,218,21,10,38,73,244,195,247,2,21,38,128,114,199,143,15,31,99,44,75,0,199,52,
  102,216,176,11,30,18,161,254,238,211,119,231,98,13,157,124,33,181,151,182,124,170,64,168,
  65,24,102,147,42,13,81,172,130,66,133,146,43,53,32,94,132,120,180,210,118,64,130,10,
  11,252,192,212,29,122,68,234,95,156,24,57,19,17,21,237,120,96,216,243,134,171,239,222,
  251,203,245,221,209,167,107,27,177,79,24,98,63,129,133,195,238,132,137,182,223,217,125,152,
  116,176,230,164,206,127,222,48,68,32,40,207,245,140,153,66,85,180,244,195,109,184,60,224,
  0,249,72,79,231,201,33,25,150,55,38,17,17,10,192,66,181,190,251,130,0,202,11,9,
  80,14,96,227,67,200,187,179,123,233,74,203,173,31,144,187,222,182,130,132,7,104,138,233,
  65,67,155,189,117,213,36,5,120,144,22,216,247,156,125,207,241,253,133,125,127,97,223,95,
  240,237,81,134,31,243,130,148,153,80,21,248,34,70,125,105,115,204,120,171,100,208,113,206,
  190,136,167,250,5,95,95,216,215,23,9,53,51,158,141,177,86,13,47,34,231,214,220,238,
  98,110,35,62,185,218,203,205,46,60,220,96,228,71,114,228,103,18,84,30,18,168,108,28,
  181,217,123,134,199,29,138,239,3,245,125,132,153,49,243,31,201,39,24,217,82,185,162,43,
  129,171,115,51,135,56,70,199,46,43,139,94,233,216,142,238,24,168,55,17,45,43,232,53,
  176,195,82,200,119,84,159,17,118,200,196,227,55,7,144,107,210,14,9,98,68,78,253,44,
  82,229,5,109,112,21,176,165,191,239,206,79,74,81,16,144,234,84,16,62,120,118,22,31,
  120,65,174,130,210,21,240,204,1,83,96,74,78,189,27,53,70,74,111,73,219,152,132,134,
  40,4,120,160,238,10,66,140,25,159,178,120,68,74,130,13,133,133,29,243,127,50,183,62,
  68,79,60,76,200,140,8,183,35,41,76,56,61,108,144,110,247,27,46,67,89,188,41,71,
  208,213,218,16,105,197,70,118,190,145,179,50,27,37,183,197,185,209,143,83,210,250,134,205,
  52,70,14,27,108,92,140,98,30,148,117,64,47,28,86,211,38,9,242,235,40,168,134,135,
  37,239,18,215,184,237,200,47,69,87,83,10,176,182,197,101,116,93,140,129,164,121,165,16,
  153,65,128,224,88,168,34,92,197,120,253,199,76,199,45,62,101,100,74,85,191,108,49,23,
  146,160,170,3,194,122,115,215,142,94,24,116,53,185,42,239,19,255,170,144,34,244,253,145,
  162,157,201,227,153,70,180,140,180,42,218,166,34,45,84,26,148,194,142,242,169,167,17,140,
  117,164,133,181,32,147,58,34,121,19,178,35,254,245,43,5,185,206,222,190,249,241,61,255,
  16,10,182,230,199,190,249,33,178,137,229,229,31,32,117,145,89,48,230,213,90,143,243,92,
  143,211,38,21,186,227,44,249,233,166,65,42,2,105,183,58,166,55,125,93,33,243,177,99,
  110,69,227,144,10,35,149,15,71,244,210,193,213,204,134,205,20,247,97,91,123,84,188,63,
  52,203,9,187,27,144,62,100,227,225,182,214,121,230,247,32,83,100,249,111,160,23,232,26,
  152,200,218,79,176,9,199,250,217,121,55,10,217,38,237,188,79,22,225,239,231,209,127,49,
  91,176,19,88,216,199,14,190,254,95,255,207,167,113,79,58,96,218,136,54,166,142,199,139,
  141,230,198,36,27,36,227,183,163,228,82,197,92,36,253,239,27,27,202,152,230,198,238,214,
  206,70,208,253,95,139,255,10,63,120,235,7,198,193,251,135,6,120,89,148,8,55,118,212,
  31,93,193,240,198,222,195,143,134,49,102,170,198,184,190,157,40,208,171,65,114,62,34,15,
  59,215,11,233,253,135,31,223,74,4,179,198,161,45,227,94,48,111,85,188,202,232,247,115,
  97,197,16,99,35,248,38,162,63,156,215,216,228,191,137,123,110,102,148,127,215,240,120,109,
  152,67,4,205,151,148,191,181,35,163,12,118,234,142,70,164,251,243,10,243,166,198,207,230,
  143,171,87,94,83,161,3,108,55,105,25,180,131,194,76,20,224,49,44,116,227,99,161,230,
  37,70,172,66,144,184,51,0,20,167,165,235,156,16,90,11,102,199,236,162,115,2,218,10,
  5,118,121,104,33,93,100,0,119,219,81,165,95,210,88,45,43,245,98,86,95,194,122,129,
  99,151,227,211,184,24,191,229,170,170,194,175,129,225,74,244,54,190,9,156,85,41,157,8,
  207,122,150,214,167,179,49,222,209,235,67,126,5,222,77,242,254,220,200,194,227,2,123,187,
  121,174,120,145,20,124,91,23,221,182,151,4,185,131,132,163,244,194,217,71,165,10,86,246,
  141,167,195,58,53,168,174,92,105,126,45,221,82,254,169,240,228,12,234,183,86,233,139,173,
  221,105,165,39,114,61,163,158,137,61,70,103,208,176,232,97,128,15,251,250,179,250,34,245,
  182,172,72,92,140,91,23,52,208,46,20,102,99,125,101,61,76,204,44,209,53,87,221,46,
  55,67,150,185,19,235,238,109,205,102,163,193,162,35,138,51,32,235,47,220,215,133,113,165,
  252,132,87,86,160,138,73,131,21,222,130,23,123,102,179,130,25,231,20,221,175,41,186,191,
  186,232,247,53,69,191,95,86,84,160,6,254,162,147,253,213,69,107,90,157,248,91,117,242,
  138,28,139,133,105,136,221,60,234,129,50,199,14,172,38,81,0,192,202,130,141,99,195,31,
  23,34,133,73,5,138,85,33,27,50,201,227,173,143,58,34,245,241,54,15,123,151,35,107,
  222,125,106,156,108,231,155,219,97,185,195,156,58,83,24,166,28,36,30,99,78,64,234,140,
  218,0,36,32,77,38,6,211,150,132,188,200,200,139,195,185,229,190,232,229,76,107,163,245,
  22,238,95,58,7,170,48,78,54,189,3,10,152,181,238,11,99,70,70,60,93,237,28,39,
  8,174,225,229,34,193,89,234,184,121,248,73,132,229,252,130,135,78,191,126,101,254,56,153,
  85,154,46,43,213,175,45,37,252,244,4,116,81,209,225,18,117,4,102,105,156,164,101,197,
  93,214,178,175,134,253,165,53,60,90,93,193,247,75,43,152,36,43,107,152,44,239,194,203,
  213,131,152,44,239,67,188,180,15,226,154,168,171,64,228,94,48,15,13,242,172,153,110,201,
  173,27,217,188,128,173,179,87,41,177,4,85,160,178,246,161,172,148,94,129,141,80,13,198,
  149,82,41,94,123,199,6,214,141,92,123,98,150,28,109,156,140,210,254,214,116,4,14,76,
  42,189,113,208,137,32,212,24,68,37,175,7,203,80,146,66,185,43,41,52,172,193,244,75,
  133,234,31,198,151,75,177,253,74,58,180,204,195,195,226,166,47,138,23,240,97,3,98,113,
  58,136,243,65,77,75,190,44,238,235,226,232,248,209,203,199,143,14,31,119,54,55,23,171,
  93,122,1,99,252,52,68,185,66,62,2,200,136,203,152,69,236,248,157,126,97,246,95,120,
  163,200,219,44,214,179,156,203,170,226,204,206,200,190,43,185,146,201,8,248,216,133,130,101,
  82,8,76,198,215,22,176,43,63,155,77,166,213,24,146,56,78,100,65,78,105,180,51,169,
  56,166,131,103,199,242,194,2,70,146,137,32,49,211,163,2,254,224,250,204,42,159,93,153,
  153,98,181,108,38,252,56,138,213,220,170,217,183,235,81,107,224,204,223,120,58,140,237,168,
  36,189,240,69,40,83,11,140,170,54,50,147,42,211,121,147,23,162,149,82,113,245,102,165,
  122,124,190,193,15,49,157,19,251,25,121,107,63,112,171,54,188,207,69,156,222,237,165,14,
  187,251,190,212,225,26,199,114,226,8,148,42,88,119,16,202,106,156,121,52,74,254,235,61,
  29,165,19,97,239,125,57,56,249,29,84,143,70,105,126,213,29,169,178,26,103,31,167,82,
  134,220,51,85,234,176,123,186,74,29,246,29,178,210,254,174,156,56,57,54,35,170,230,228,
  149,110,76,221,49,44,171,113,53,7,179,172,68,121,78,106,105,125,122,206,108,105,125,58,
  167,183,84,65,235,12,151,34,224,59,199,106,185,173,216,192,127,174,75,55,230,219,62,243,
  43,48,160,172,68,213,129,131,178,26,87,5,13,165,249,229,247,191,200,152,167,113,118,155,
  11,150,249,148,88,114,187,90,233,234,106,69,220,43,120,234,166,238,29,204,124,215,162,7,
  52,127,195,107,226,38,32,250,198,80,212,24,91,105,125,122,129,95,237,46,254,11,207,230,
  55,218,217,64,218,178,219,237,41,65,223,92,182,173,156,44,155,255,254,77,116,195,141,241,
  159,191,238,38,63,111,90,152,58,248,119,70,196,124,49,22,142,76,246,62,28,117,104,61,
  192,115,43,183,126,252,112,252,228,168,247,234,201,97,239,201,207,79,94,60,121,121,12,67,
  37,41,51,55,183,201,204,167,150,96,167,116,56,163,140,9,81,83,65,206,199,34,83,32,
  99,164,106,222,203,235,177,201,79,50,222,34,96,255,164,66,139,172,176,162,193,123,169,196,
  51,238,201,209,200,99,133,90,89,14,176,121,171,146,121,162,44,22,245,152,41,223,1,19,
  101,80,248,101,82,224,213,202,61,108,69,186,177,135,5,179,14,155,88,182,33,229,117,82,
  196,100,114,90,80,76,165,122,12,125,29,129,71,47,53,169,177,153,74,128,136,194,216,144,
  143,249,19,249,72,164,244,64,251,88,36,84,48,46,43,30,170,43,131,144,34,251,230,32,
  66,238,43,154,124,164,157,164,100,11,56,237,92,151,29,78,207,236,38,157,235,81,135,249,
  171,43,225,163,90,51,208,33,10,210,218,235,118,57,165,39,210,30,8,233,97,126,63,15,
  216,107,126,67,217,124,128,206,134,52,201,84,157,195,84,34,147,211,249,145,158,147,30,12,
  108,236,50,251,162,41,170,207,120,141,196,7,198,4,179,30,183,90,232,51,117,138,183,85,
  46,98,213,8,39,38,192,170,53,239,248,131,40,15,164,7,193,4,102,149,186,229,253,140,
  168,93,17,153,86,204,52,83,19,98,45,109,57,166,92,143,201,84,88,244,45,126,165,227,
  59,220,105,35,106,24,235,110,23,176,107,70,51,220,110,211,28,135,172,83,186,231,41,179,
  135,38,187,205,127,83,38,145,6,67,222,113,87,246,62,109,197,15,30,60,216,197,0,18,
  184,77,132,13,231,172,147,146,176,59,55,170,108,173,98,248,199,71,80,84,106,184,253,58,
  214,102,103,94,92,177,125,15,134,116,84,7,200,14,59,115,133,199,133,252,52,3,186,146,
  240,244,146,114,144,45,116,121,8,28,151,84,242,216,8,27,85,238,145,89,84,250,104,159,
  27,243,56,203,19,99,30,87,165,165,38,143,43,41,171,53,11,147,151,223,141,1,5,138,
  110,11,102,95,18,98,225,195,204,118,43,62,213,246,214,173,33,17,204,241,122,123,123,46,
  115,193,9,228,198,1,32,226,70,153,109,192,252,88,153,199,125,200,36,38,229,48,99,230,
  234,43,147,24,0,40,132,99,247,150,61,26,249,33,158,229,13,79,128,167,222,37,147,135,
  127,149,39,23,81,91,236,147,94,118,126,142,169,181,227,120,190,151,104,195,205,103,197,85,
  193,86,194,125,254,50,217,215,180,31,242,79,24,58,196,199,162,58,34,97,168,198,84,42,
  170,66,55,44,67,155,4,122,97,243,59,228,190,74,185,214,23,19,32,201,244,118,54,201,
  200,246,54,218,50,122,197,105,201,27,35,200,233,70,100,49,142,108,249,164,6,169,115,163,
  160,20,173,56,134,44,73,139,26,109,227,15,185,247,176,105,154,188,146,148,206,234,248,6,
  93,193,140,232,142,96,104,24,17,12,10,181,19,167,35,176,87,79,114,121,45,178,27,215,
  38,33,19,167,245,152,44,162,147,21,243,5,55,205,141,99,201,141,22,205,156,189,220,173,
  44,127,127,27,226,255,89,80,93,111,36,140,219,242,62,51,55,71,222,156,85,119,66,220,
  156,217,219,222,177,168,84,61,122,177,231,232,101,238,209,131,105,89,230,124,25,86,22,229,
  88,140,174,144,254,130,211,145,80,136,79,25,67,12,7,145,59,56,8,47,178,41,218,102,
  214,157,160,49,212,107,246,66,210,15,232,193,203,79,123,216,156,180,246,154,195,230,28,161,
  30,9,255,238,182,134,1,226,32,42,186,117,15,182,157,134,36,231,219,104,195,219,8,66,
  189,22,147,6,110,239,194,146,53,229,65,22,88,175,71,6,56,32,233,65,46,120,208,156,
  192,214,236,92,129,143,51,186,10,0,62,50,128,143,51,72,0,157,157,66,23,41,62,153,
  181,206,78,91,151,8,244,41,112,136,192,152,2,71,8,76,17,144,176,5,190,116,195,254,
  237,78,254,159,191,60,230,148,194,241,101,123,166,6,13,151,115,221,161,24,244,16,131,30,
  146,137,244,126,107,120,10,179,187,52,82,4,102,230,16,135,223,100,136,246,181,81,189,22,
  232,234,90,132,51,79,91,255,158,157,172,49,91,185,145,13,164,150,249,17,158,125,253,122,
  103,42,237,245,219,59,118,183,173,253,237,48,77,28,49,189,19,76,239,132,79,239,228,20,
  219,144,166,23,129,158,158,94,190,32,25,132,80,7,4,54,12,243,235,240,9,40,106,233,
  73,159,216,84,79,143,212,233,168,30,4,206,177,161,135,173,30,10,95,70,83,248,131,195,
  222,199,15,41,210,30,97,128,60,229,44,154,157,28,157,66,200,19,63,148,114,16,125,60,
  39,183,6,16,136,63,35,109,36,178,201,30,189,155,53,14,96,243,240,16,98,157,19,133,
  16,228,150,120,225,172,106,152,205,48,53,6,35,170,248,175,132,189,53,250,29,49,203,107,
  57,51,27,150,182,184,249,176,66,191,47,238,158,47,173,139,215,193,61,91,253,192,106,97,
  147,141,26,169,30,76,185,41,120,246,209,83,87,99,68,42,84,200,88,6,54,162,112,255,
  31,76,210,79,160,120,24,82,174,138,162,121,172,177,97,116,148,76,84,238,238,8,172,146,
  227,172,232,183,46,96,101,31,219,217,115,211,173,186,242,165,14,149,181,118,6,133,68,99,
  130,233,137,52,75,151,61,145,8,203,215,87,88,13,242,242,175,100,206,94,255,199,64,86,
  126,235,108,16,206,2,164,22,17,27,163,2,245,115,186,242,96,147,245,57,81,181,125,253,
  154,104,255,47,235,214,157,102,16,232,231,49,156,229,173,218,26,108,108,182,74,33,8,159,
  50,210,133,32,25,140,144,53,154,146,156,60,251,62,198,55,63,171,68,43,13,140,231,24,
  203,37,144,85,118,114,43,217,152,96,187,132,6,100,197,215,193,59,173,196,64,202,230,186,
  130,41,165,249,74,12,153,51,115,105,182,31,39,142,119,154,125,5,132,133,27,2,44,228,
  94,5,92,127,26,29,164,11,83,70,147,161,209,117,208,239,146,15,52,100,111,236,128,11,
  63,136,4,30,22,41,92,151,160,224,82,237,246,96,130,46,61,104,75,239,96,232,21,101,
  96,231,204,82,111,16,226,165,94,42,20,60,96,121,212,59,246,90,67,52,222,13,153,98,
  130,58,209,91,3,212,57,121,141,20,57,128,64,27,100,231,190,10,248,98,209,50,49,201,
  161,116,97,32,211,79,99,154,228,249,11,134,64,131,102,212,71,217,196,17,146,30,102,13,
  189,83,204,253,160,28,242,188,149,144,155,200,84,65,109,253,68,253,98,214,120,204,218,251,
  223,170,246,163,73,150,193,53,136,93,251,248,91,213,206,21,199,245,38,101,76,186,27,86,
  222,245,189,204,152,12,137,94,242,200,74,52,247,66,98,46,182,147,205,72,33,17,148,202,
  57,148,221,86,98,35,2,145,30,231,29,241,210,91,181,39,76,36,251,109,188,170,20,95,
  105,179,204,171,149,101,248,250,153,101,146,254,170,50,124,85,180,63,165,196,32,192,112,25,
  160,205,89,10,188,103,154,65,142,97,176,97,29,80,242,25,78,48,81,129,51,121,66,90,
  155,10,134,186,240,83,2,79,109,100,26,45,151,137,209,61,221,1,202,34,225,149,23,254,
  249,65,102,125,126,33,207,238,220,3,169,218,86,36,248,111,95,7,155,161,120,147,216,102,
  97,43,125,22,18,130,85,136,119,109,62,202,42,229,196,38,90,107,255,116,164,91,168,188,
  187,186,16,223,62,178,200,219,120,141,34,124,247,200,34,175,214,41,194,55,143,44,146,224,
  173,109,159,88,11,35,230,7,91,92,200,219,250,216,139,152,69,49,28,157,151,116,174,196,
  178,239,88,116,20,150,213,244,74,71,132,59,73,163,75,5,129,36,13,136,110,213,130,201,
  23,211,210,70,65,116,106,163,226,221,219,86,220,132,179,32,186,144,76,192,65,119,210,231,
  97,163,22,230,128,54,95,73,54,175,156,210,181,158,146,143,38,21,111,94,28,60,22,218,
  235,26,55,13,80,8,116,186,75,114,68,0,131,32,12,157,222,47,187,65,171,149,243,120,
  34,28,145,25,249,20,100,139,7,73,55,104,195,8,5,38,1,148,164,144,138,0,65,98,
  164,37,172,211,131,136,252,176,196,26,173,139,73,118,34,135,37,251,93,203,25,139,189,190,
  93,3,255,73,133,210,136,246,236,32,80,30,115,241,69,22,60,77,226,102,102,15,28,57,
  70,36,106,171,148,150,238,144,115,7,95,163,73,219,208,93,78,130,59,204,100,126,227,113,
  229,236,110,60,79,89,165,27,172,241,141,2,165,21,66,183,197,185,57,232,41,41,91,118,
  61,115,109,94,70,218,223,93,55,143,106,155,99,63,132,144,38,147,105,57,247,180,16,51,
  210,123,87,211,189,119,186,25,57,99,237,102,18,103,7,213,5,196,88,6,27,57,223,99,
  172,185,28,96,67,20,47,227,151,100,211,252,218,215,250,136,99,195,164,139,20,111,240,129,
  243,146,162,39,204,39,6,235,141,134,246,180,252,212,39,8,188,62,24,123,171,253,101,86,
  146,206,19,44,237,67,229,137,164,55,141,218,216,74,235,250,98,56,34,34,82,175,193,56,
  73,206,73,91,201,24,48,209,208,228,84,210,216,199,56,92,25,31,59,153,80,226,99,231,
  227,236,251,199,201,118,194,170,129,246,173,142,105,141,162,108,138,229,229,160,201,240,76,201,
  193,144,208,121,146,155,206,218,177,65,152,250,182,162,225,2,210,1,251,216,37,175,98,162,
  33,42,243,78,33,121,129,212,51,1,235,14,240,246,171,6,63,38,54,49,220,22,97,6,
  232,189,126,146,17,205,143,92,31,98,142,102,228,7,130,224,214,215,175,244,89,50,173,47,
  154,232,128,216,150,194,39,34,115,230,144,53,97,243,38,154,194,97,253,32,154,182,210,202,
  91,59,53,223,218,9,72,61,61,214,200,28,73,201,9,94,211,144,99,229,225,1,194,200,
  71,213,139,41,100,164,70,6,66,169,43,49,233,219,129,20,77,253,237,154,109,199,205,212,
  246,2,40,90,29,4,212,0,124,213,81,171,131,211,5,64,16,171,50,135,107,71,89,85,
  126,106,204,79,142,234,198,84,29,102,101,167,219,23,213,244,81,205,184,213,167,106,50,252,
  16,77,92,46,110,204,94,22,124,182,31,26,88,37,34,249,26,238,184,128,41,209,9,77,
  248,21,232,88,133,236,156,2,7,144,254,218,110,183,119,204,55,25,55,162,44,112,21,254,
  206,2,204,151,120,136,15,195,240,35,16,161,239,62,42,252,111,164,186,59,74,245,211,124,
  36,213,222,88,50,247,2,187,223,164,159,124,30,114,118,174,109,68,75,107,198,177,21,76,
  90,209,94,64,154,113,184,74,23,179,212,224,212,90,248,99,180,185,217,181,82,237,39,117,
  244,116,156,197,229,254,30,123,174,117,171,213,172,149,211,135,32,70,64,148,136,98,241,98,
  228,16,233,102,105,149,120,97,145,3,73,83,245,197,168,126,60,103,89,54,222,244,228,48,
  186,202,251,104,101,241,246,17,248,159,149,105,57,10,40,64,241,122,69,56,10,40,139,208,
  76,156,103,107,204,132,135,52,74,122,3,245,179,193,228,251,54,89,3,211,91,55,48,93,
  210,128,184,50,89,11,147,255,36,2,182,73,85,237,19,141,86,129,188,89,212,111,101,12,
  208,205,186,253,86,244,125,208,39,140,10,61,120,138,30,0,171,2,148,10,251,160,230,226,
  47,238,113,147,124,125,144,223,110,10,111,68,100,152,220,148,12,176,232,30,228,245,11,244,
  187,210,177,221,180,243,173,191,53,207,110,117,72,207,150,28,82,20,5,180,219,244,228,169,
  28,211,179,117,142,233,217,205,143,233,217,237,142,233,167,91,159,162,79,75,78,209,5,83,
  187,226,167,168,151,213,17,113,77,70,42,179,93,34,172,32,237,144,187,51,210,149,230,188,
  134,148,241,65,45,53,207,44,253,57,139,7,242,174,205,210,87,121,246,41,39,113,222,68,
  198,176,7,186,100,239,247,226,51,16,33,14,50,248,141,202,198,56,25,134,32,203,168,76,
  38,188,126,229,254,108,6,143,182,173,86,200,237,111,124,247,157,234,129,129,157,170,56,216,
  178,137,201,47,47,122,191,179,208,53,130,93,106,213,7,210,122,104,246,211,169,74,70,203,
  218,200,64,104,198,188,91,98,78,228,104,157,50,20,69,6,43,141,70,217,144,205,102,213,
  68,56,101,57,245,98,38,11,163,233,108,124,145,188,57,252,217,42,45,206,240,12,70,55,
  180,217,142,205,151,79,15,96,127,96,252,112,140,44,157,217,66,17,159,81,250,69,54,24,
  157,227,89,239,171,101,28,113,102,172,40,0,147,224,63,65,179,97,108,101,38,255,118,34,
  187,176,196,68,81,102,177,60,153,100,23,73,181,100,32,208,216,62,215,215,252,229,28,81,
  18,208,77,197,83,182,191,5,177,50,66,64,200,180,144,89,41,169,42,122,106,148,224,117,
  138,253,56,68,205,2,29,153,222,199,63,66,70,132,119,177,254,201,148,108,204,224,71,32,
  222,3,104,207,100,103,241,152,220,6,111,97,227,151,66,202,133,12,78,149,73,129,45,19,
  72,73,150,158,34,23,99,63,202,153,161,173,170,59,226,248,181,181,247,49,255,150,242,137,
  245,187,156,81,87,54,156,100,167,106,183,52,252,140,215,212,74,74,96,118,92,77,7,112,
  195,188,231,70,121,122,89,120,229,5,3,66,208,109,18,167,241,39,210,90,210,82,107,101,
  231,189,96,223,51,103,201,191,228,163,79,164,99,16,167,89,58,159,100,179,66,169,33,148,
  195,3,184,86,3,130,57,34,107,65,82,102,150,25,40,217,220,212,27,124,6,21,246,87,
  118,228,239,184,132,160,72,199,244,165,72,63,152,63,156,123,61,198,221,233,61,126,242,246,
  248,151,95,126,62,234,245,238,111,206,240,134,174,198,51,73,197,24,52,186,39,23,204,33,
  58,6,122,48,43,202,108,194,191,55,179,179,34,97,70,133,175,7,16,16,31,141,25,106,
  188,192,153,29,179,179,123,205,126,31,21,243,180,239,210,109,186,198,253,9,224,0,169,253,
  164,161,182,4,96,101,112,13,178,2,85,2,25,164,144,140,44,129,243,0,73,148,130,158,
  24,68,129,62,208,83,86,97,47,27,179,89,10,163,88,73,249,206,158,70,183,144,59,203,
  186,32,77,169,155,155,27,135,81,89,14,141,217,119,179,90,43,99,22,49,214,166,90,198,
  92,56,81,72,156,132,107,203,80,110,247,42,219,122,252,228,233,163,55,63,31,247,94,60,
  58,126,114,248,252,209,207,189,151,143,94,60,137,54,123,61,145,192,47,169,67,23,19,122,
  154,184,247,85,197,212,44,57,95,128,156,70,197,239,2,226,54,61,186,88,74,197,76,201,
  238,39,66,50,215,112,72,96,202,230,134,146,181,87,171,225,50,50,212,0,70,150,2,192,
  74,141,115,229,67,33,227,234,181,188,171,70,115,82,129,59,84,89,140,214,172,198,193,244,
  88,132,175,220,9,60,204,151,225,84,169,154,195,159,72,39,101,58,76,242,196,55,155,78,
  170,178,207,205,101,173,248,156,60,77,212,34,191,121,37,13,39,48,229,123,174,135,45,215,
  235,19,211,79,58,48,215,68,121,123,88,181,4,70,89,185,16,70,84,112,187,57,55,59,
  84,105,69,207,63,121,92,234,11,181,213,240,181,180,171,88,74,115,103,23,203,0,107,31,
  232,88,30,75,68,105,228,170,131,157,25,34,227,20,126,9,51,44,21,125,57,75,135,141,
  59,17,150,197,199,179,156,139,170,144,245,57,41,73,206,69,135,185,34,221,61,216,186,196,
  63,45,102,206,48,225,36,245,73,196,227,139,89,23,49,99,104,253,228,208,249,13,56,43,
  51,105,216,67,244,41,133,177,133,194,49,246,209,59,135,169,204,114,54,97,101,47,11,21,
  137,233,120,66,27,180,44,170,170,126,189,11,216,159,153,178,27,108,150,150,114,140,42,22,
  54,90,132,250,50,179,250,138,98,167,140,105,97,22,114,239,84,171,70,202,125,192,214,195,
  201,198,23,137,146,159,242,94,187,213,136,193,44,140,93,221,103,162,253,38,59,130,215,210,
  125,61,36,108,236,41,46,13,190,251,165,92,98,163,20,211,249,46,203,199,3,58,214,246,
  41,122,77,150,182,102,181,101,185,205,42,183,10,16,210,127,35,151,42,51,250,48,207,28,
  203,161,16,19,235,52,38,230,218,218,171,21,44,236,172,90,26,190,175,220,202,232,209,111,
  185,118,132,66,171,127,160,225,67,131,57,33,151,52,98,80,175,156,252,141,146,236,54,246,
  193,12,128,136,35,90,61,154,23,64,163,201,168,88,194,138,14,152,126,80,208,53,94,238,
  214,22,34,70,10,204,240,127,217,134,239,135,14,147,245,196,215,37,190,230,29,18,114,164,
  175,43,150,182,67,86,254,241,53,103,105,59,221,106,163,196,186,36,129,13,187,105,24,144,
  32,99,254,144,208,139,67,218,116,20,104,141,121,32,19,63,173,126,200,247,227,142,248,111,
  55,232,172,95,12,162,151,248,95,22,196,34,202,105,198,220,4,230,246,118,47,97,125,44,
  78,202,83,190,117,245,196,84,247,175,78,91,16,210,148,49,76,69,158,122,97,126,7,161,
  45,149,104,129,4,157,129,190,116,38,87,179,197,2,121,34,32,45,105,173,186,49,57,44,
  44,217,143,5,235,180,177,57,7,254,148,198,71,21,18,149,230,87,5,156,150,198,71,21,
  150,150,230,151,5,87,229,229,35,62,3,7,116,151,42,104,147,157,13,116,210,37,40,67,
  196,87,185,115,10,92,243,127,176,226,161,84,245,107,175,255,82,76,157,236,73,88,90,35,
  181,71,174,108,247,69,198,28,135,165,61,124,103,62,66,53,222,200,154,11,211,61,137,92,
  117,189,3,228,197,11,30,154,184,112,67,216,24,3,59,124,67,109,13,9,131,112,195,62,
  138,197,117,122,192,3,253,52,252,34,253,22,62,95,129,28,122,124,146,179,206,184,158,200,
  41,114,211,132,126,38,156,178,110,57,23,178,45,77,244,85,80,129,47,143,36,41,196,2,
  48,184,47,217,211,208,138,116,79,175,149,184,38,190,84,29,159,222,185,85,8,237,29,150,
  44,224,198,7,75,103,161,174,148,221,86,101,122,124,215,128,16,203,96,93,125,12,45,194,
  190,20,231,177,6,238,203,129,163,255,137,49,164,23,213,59,81,185,19,241,37,85,38,15,
  58,154,100,87,151,160,221,35,114,229,129,77,25,224,31,204,130,130,171,247,221,119,8,205,
  85,232,11,115,215,182,124,250,157,196,0,199,152,236,140,52,130,78,125,57,187,121,118,181,
  48,51,109,129,46,45,198,201,10,154,119,183,182,71,188,85,147,227,191,239,144,111,12,91,
  23,225,100,36,189,94,205,148,7,53,21,250,49,113,160,204,115,231,5,21,221,219,17,110,
  30,182,200,220,243,94,178,95,1,60,175,208,255,41,109,195,139,196,11,131,42,233,210,90,
  67,118,33,223,8,95,178,108,162,108,104,16,125,89,106,109,199,164,243,38,114,247,233,13,
  32,106,142,89,141,74,121,16,104,129,129,193,159,143,198,147,103,241,236,83,18,237,223,211,
  49,191,48,53,8,249,166,224,59,163,130,156,173,251,52,99,125,167,191,70,255,75,246,99,
  12,162,100,63,122,36,37,253,53,71,83,242,95,107,72,165,8,24,35,43,249,143,176,172,
  73,127,58,30,59,115,148,39,112,39,160,212,225,202,76,148,198,135,34,130,60,205,32,126,
  196,109,136,107,84,31,184,155,36,90,62,69,145,159,152,170,72,35,216,46,187,106,50,142,
  138,230,30,183,224,28,67,13,185,145,4,43,166,249,147,221,148,188,251,89,13,84,65,145,
  55,101,171,168,94,61,99,107,123,66,21,62,1,215,131,109,179,167,191,188,213,103,196,233,
  89,93,11,219,106,33,121,231,80,251,59,82,220,118,46,38,53,157,77,37,71,110,172,30,
  14,235,194,233,154,191,244,182,146,92,170,150,38,236,247,71,122,147,23,146,128,49,25,49,
  104,192,109,179,111,221,131,115,203,169,124,148,92,125,223,88,118,51,17,118,77,69,81,195,
  21,32,1,204,55,187,174,50,219,229,54,226,97,5,91,52,66,32,231,134,77,164,107,54,
  33,7,199,184,110,62,143,152,246,200,103,128,83,172,255,218,205,65,58,67,116,64,251,148,
  178,242,173,43,31,108,33,137,114,93,91,71,105,219,132,15,82,116,168,161,163,160,136,23,
  159,141,97,81,30,0,235,28,137,108,193,241,140,162,48,95,63,122,97,177,86,222,227,61,
  197,67,31,16,186,20,25,133,43,167,221,69,160,27,218,18,149,42,40,200,34,85,245,81,
  233,196,242,134,76,80,182,37,154,84,78,165,117,220,7,9,14,89,28,55,55,144,27,49,
  188,67,81,188,10,200,141,1,152,140,41,20,243,70,85,220,169,206,147,30,209,238,170,99,
  93,151,96,202,160,48,160,152,72,111,22,75,143,34,174,29,168,30,74,185,37,190,172,144,
  177,201,35,232,151,53,11,229,30,17,125,84,2,186,214,24,42,67,144,226,75,177,94,16,
  60,147,99,99,33,186,121,11,223,98,5,154,5,156,103,37,109,21,241,161,153,110,247,195,
  162,137,8,54,247,72,77,233,131,79,251,118,127,97,200,30,106,224,202,68,199,72,10,15,
  85,131,135,190,109,66,48,1,99,234,208,209,73,252,91,98,220,159,96,172,231,45,34,190,
  67,29,51,44,213,205,226,199,47,37,199,193,36,43,172,131,198,250,114,24,40,200,77,9,
  142,236,170,20,75,171,233,186,252,198,84,235,172,226,249,197,169,183,137,170,69,70,159,91,
  177,252,26,85,55,170,78,145,32,192,128,174,158,45,174,178,51,112,80,189,84,101,137,192,
  160,72,27,151,171,5,210,237,28,242,142,181,119,2,209,85,25,233,255,85,177,10,239,106,
  155,14,231,218,228,94,107,139,88,213,62,4,236,151,28,108,246,79,121,12,239,65,125,47,
  6,86,205,176,89,197,187,28,36,106,156,156,151,18,86,229,38,132,42,179,169,4,76,103,
  89,9,198,147,4,73,108,233,12,116,39,254,38,152,22,239,7,251,49,59,83,242,95,221,
  165,146,254,90,221,42,69,96,61,140,172,130,194,221,10,245,90,121,53,253,207,85,244,31,
  114,21,53,244,102,106,171,141,6,89,167,189,166,70,4,113,2,27,114,131,181,141,173,229,
  102,75,205,218,90,70,109,228,115,75,213,208,178,106,32,11,28,105,155,60,182,166,45,210,
  142,47,90,36,112,5,73,171,27,221,100,253,186,113,120,150,219,184,86,103,117,227,242,237,
  7,93,142,238,198,126,179,178,57,48,134,188,101,198,115,139,71,89,59,154,85,50,127,192,
  32,179,182,25,207,247,201,162,254,254,51,161,23,201,235,51,89,110,125,69,252,183,184,4,
  235,174,59,14,221,228,178,233,120,1,228,212,210,234,20,6,235,196,218,233,88,9,242,244,
  90,222,240,74,253,131,151,35,153,61,237,59,87,218,69,230,37,88,50,241,130,162,209,190,
  23,210,127,109,198,11,184,183,179,19,168,235,76,17,181,240,58,100,142,41,97,168,45,187,
  36,89,167,69,248,243,106,142,175,33,53,86,91,165,115,63,186,201,127,136,227,203,153,86,
  140,192,242,84,64,210,130,143,128,162,242,190,193,139,224,187,198,97,63,240,204,245,252,6,
  143,37,45,209,164,98,152,89,220,7,209,120,41,43,54,19,111,188,143,101,101,70,71,149,
  239,29,189,69,69,119,116,215,152,49,119,133,252,192,162,124,251,127,239,132,159,136,165,59,
  207,86,202,64,104,22,190,229,94,12,174,131,109,122,82,110,122,22,73,107,104,174,134,45,
  52,134,81,191,24,77,65,217,255,25,240,97,12,193,125,46,26,72,235,244,35,28,182,22,
  232,34,23,116,199,241,140,231,228,205,136,31,85,22,86,50,101,210,33,97,238,41,152,47,
  45,152,43,69,155,74,193,120,105,193,56,48,188,146,217,5,179,165,5,51,233,59,177,90,
  112,188,180,224,152,23,236,123,10,246,151,22,236,203,155,248,192,89,10,251,81,88,129,219,
  146,193,60,28,141,7,88,89,162,112,246,99,18,59,59,17,8,21,93,3,208,172,80,18,
  201,125,82,252,73,2,67,100,15,109,119,133,190,254,163,52,72,113,90,25,161,98,71,184,
  180,147,140,100,198,197,132,122,128,147,94,200,244,182,200,144,235,12,59,33,57,230,202,101,
  14,81,34,54,51,236,210,183,74,231,62,245,50,167,137,76,103,96,5,198,78,250,216,76,
  39,213,54,233,97,137,105,157,22,230,144,218,246,152,218,213,65,181,237,81,249,6,181,106,
  76,237,21,131,106,175,26,85,123,201,176,106,236,29,232,99,190,85,183,143,180,234,156,222,
  69,27,5,75,236,112,35,8,190,61,194,55,39,92,154,121,68,8,196,150,229,215,137,52,
  185,148,164,101,100,161,99,158,130,116,72,174,77,64,4,55,134,21,64,3,207,222,92,224,
  206,187,243,153,98,144,27,169,236,142,175,201,67,170,157,43,214,195,19,137,69,245,195,217,
  169,125,206,200,216,74,213,189,19,25,14,162,216,71,156,138,143,37,129,132,55,93,34,3,
  51,222,24,34,146,122,72,186,202,37,186,218,53,63,72,215,141,107,117,165,202,247,19,180,
  117,176,198,165,168,164,232,214,166,200,194,19,10,144,79,169,119,201,217,179,159,121,127,147,
  156,187,148,162,121,33,99,19,9,76,8,147,40,188,176,66,202,7,198,81,65,108,27,202,
  101,97,134,66,57,130,153,91,176,38,32,197,70,133,110,36,150,129,243,223,15,232,209,34,
  68,181,233,186,197,151,224,98,50,65,14,42,201,220,80,122,171,218,189,69,85,177,191,170,
  189,91,84,149,249,171,218,191,69,85,99,127,85,223,223,162,170,62,1,175,186,21,159,123,
  155,185,119,139,102,102,190,30,79,195,97,72,14,225,205,29,218,51,186,195,100,42,94,189,
  56,124,242,66,9,129,1,21,61,114,49,151,31,93,199,170,39,167,21,82,13,147,29,112,
  104,52,48,241,74,244,114,122,3,203,20,233,19,139,125,226,98,21,136,211,47,253,104,243,
  215,95,79,126,253,245,244,215,95,183,58,191,254,186,189,25,254,235,156,221,200,135,201,167,
  39,87,211,198,230,201,102,235,151,126,107,243,116,51,220,252,4,137,249,167,40,112,242,255,
  169,184,127,158,203,79,204,8,51,119,218,64,125,208,67,221,220,12,88,134,207,231,209,118,
  163,241,176,243,238,160,117,242,235,118,231,52,104,6,219,91,92,88,86,151,120,119,176,137,
  138,97,236,132,50,191,59,248,229,113,43,120,232,203,245,203,99,106,17,67,159,34,223,195,
  206,175,91,200,220,10,40,116,210,216,106,5,191,158,6,15,253,5,121,245,9,138,249,202,
  212,151,72,167,214,92,96,160,159,207,91,175,207,91,229,180,149,76,91,155,119,49,33,35,
  248,105,221,156,40,19,185,42,8,201,110,40,189,193,154,54,139,155,110,158,2,106,175,210,
  32,73,191,126,61,79,182,152,4,52,83,246,37,45,20,48,190,132,21,57,142,248,50,127,
  89,202,126,42,220,161,14,152,158,99,73,76,13,216,73,24,157,65,89,43,17,38,29,153,
  58,139,113,229,80,238,134,165,236,109,86,186,149,30,48,147,146,252,93,86,244,36,145,92,
  53,194,156,198,88,58,24,86,27,139,194,106,209,108,69,214,160,157,233,172,209,188,161,116,
  94,48,45,241,86,11,8,10,249,129,178,26,90,240,81,153,168,95,181,189,100,141,246,82,
  165,62,43,148,53,91,45,92,236,228,197,144,183,0,125,205,63,191,41,217,6,192,193,185,
  228,164,143,124,59,70,139,168,39,202,67,42,110,29,38,129,158,146,41,225,202,54,146,210,
  91,16,176,26,129,130,156,14,72,192,170,81,186,165,89,14,42,162,252,15,100,37,151,202,
  178,53,221,196,48,229,103,15,61,39,70,155,109,184,89,100,42,156,76,210,110,19,87,231,
  149,67,146,188,59,2,183,0,108,233,104,194,52,201,248,172,177,137,100,134,142,71,91,7,
  76,178,97,164,74,118,132,215,107,174,227,38,234,46,226,20,111,237,47,201,75,49,28,83,
  192,78,157,244,237,95,139,237,79,225,102,111,51,80,81,255,58,39,224,37,107,113,102,81,
  27,239,153,194,23,76,210,167,23,173,48,26,195,124,152,214,160,156,160,25,2,104,151,243,
  31,249,102,33,251,163,204,103,42,171,157,27,100,160,234,5,110,41,92,79,201,117,232,36,
  39,123,167,33,127,9,139,239,125,249,205,148,114,16,241,61,220,213,139,54,68,150,123,58,
  70,102,250,1,238,7,201,78,133,172,24,118,49,84,88,171,248,64,235,104,115,139,91,18,
  179,14,59,211,63,146,250,159,70,65,130,59,76,111,144,249,17,238,142,166,74,121,41,15,
  132,206,82,67,103,247,151,100,184,80,186,165,199,8,131,190,204,92,241,150,57,42,49,201,
  240,53,101,69,175,97,226,172,50,255,240,89,187,161,23,192,172,141,249,249,173,44,137,148,
  142,81,206,128,245,241,225,30,186,44,163,107,8,111,110,138,223,45,17,104,239,242,95,110,
  85,76,134,137,138,161,76,10,179,167,229,86,241,27,137,247,101,169,130,160,42,134,142,222,
  143,184,82,126,156,203,139,129,166,232,78,197,176,48,247,232,36,241,111,89,143,82,188,201,
  181,82,24,183,153,151,43,71,185,218,116,94,46,13,189,114,75,111,176,137,240,245,107,198,
  186,75,97,217,84,38,223,254,200,167,218,235,50,75,9,82,97,206,214,13,195,246,51,187,
  198,54,153,204,90,88,89,77,192,18,95,64,237,136,176,40,210,11,50,129,137,157,160,74,
  12,24,245,77,92,124,228,22,217,32,28,113,104,114,34,201,177,106,221,79,141,242,49,147,
  199,116,111,49,161,95,56,144,155,201,184,204,118,252,151,21,218,110,181,132,83,95,167,246,
  39,99,102,24,222,237,163,219,134,221,79,118,62,205,170,164,232,168,190,226,221,10,182,204,
  44,198,220,89,83,180,198,236,68,212,199,74,249,30,51,3,171,228,131,111,88,93,88,201,
  105,43,65,236,120,219,51,94,199,223,180,233,137,183,94,187,23,223,106,99,48,231,128,124,
  119,184,181,123,167,244,91,53,214,189,193,140,171,222,44,155,240,63,177,99,107,175,135,125,
  148,214,61,69,238,134,54,171,170,223,214,107,215,28,222,116,166,141,166,87,238,240,63,210,
  139,117,166,85,187,20,92,2,88,236,76,190,210,235,79,163,83,217,77,38,207,106,237,198,
  51,183,186,225,37,243,229,162,190,149,151,150,141,48,187,243,180,188,212,170,55,14,97,81,
  161,246,77,33,144,247,144,236,153,106,52,138,153,114,51,161,31,185,52,176,55,10,67,58,
  72,75,218,122,20,152,136,63,216,48,206,147,128,189,33,202,96,213,123,224,255,176,247,166,
  93,109,37,73,194,240,103,252,43,212,61,243,204,43,224,130,181,3,198,84,31,22,97,52,
  141,129,17,194,181,29,31,44,64,128,186,132,68,73,128,193,213,254,239,111,44,185,68,46,
  247,74,2,92,229,170,118,207,153,50,186,153,25,25,25,153,25,25,25,25,139,57,157,199,
  221,9,108,197,191,129,128,141,65,8,3,129,109,111,144,227,105,201,97,231,16,150,5,40,
  71,225,23,111,56,42,150,138,172,136,23,162,121,18,95,89,150,32,33,82,63,108,107,202,
  240,184,85,92,194,190,202,114,106,239,237,148,162,131,38,158,62,112,16,174,152,4,79,18,
  36,206,12,102,18,208,213,115,237,17,163,120,54,232,112,140,174,203,246,93,7,2,117,233,
  10,58,46,154,192,80,244,102,254,24,77,217,175,109,168,49,48,95,82,81,129,170,148,47,
  198,195,232,102,45,134,143,204,75,202,138,13,69,40,43,176,78,132,48,181,77,165,146,145,
  117,35,56,153,50,2,33,2,199,20,86,111,95,155,11,245,45,8,177,184,166,127,190,125,
  175,101,215,83,152,99,136,42,195,145,188,100,122,85,208,203,224,32,232,15,140,140,7,251,
  144,19,8,217,8,113,127,228,74,184,158,126,13,92,79,50,251,215,99,102,252,218,203,119,
  64,228,236,191,23,145,81,39,66,203,242,33,124,36,33,82,152,80,213,17,12,160,135,207,
  42,226,155,204,128,243,243,233,216,142,91,195,7,248,151,22,151,211,53,109,116,138,222,103,
  57,98,14,242,159,59,65,179,117,208,188,27,7,151,211,247,159,63,171,44,228,32,165,32,
  147,108,123,129,97,145,159,104,190,184,58,6,45,126,59,49,247,76,151,113,157,34,195,154,
  31,65,204,88,66,237,38,247,177,61,234,255,127,55,204,227,16,59,131,152,176,5,85,105,
  109,1,254,226,30,236,134,80,126,66,117,13,231,240,230,252,94,244,138,17,105,108,207,180,
  87,88,159,27,151,183,116,26,235,176,69,252,140,213,175,192,84,91,81,160,69,177,129,72,
  112,167,100,67,238,196,98,70,156,191,95,13,134,215,151,172,52,111,244,207,225,52,128,124,
  99,163,191,83,249,223,240,29,104,0,73,73,134,15,147,110,131,24,176,220,9,100,247,187,
  133,75,126,116,43,234,30,226,91,209,148,18,228,245,27,24,56,76,81,103,244,59,224,227,
  245,24,108,151,69,1,124,171,203,166,38,195,7,8,33,40,19,11,163,57,98,90,61,8,
  223,17,78,214,186,144,68,147,168,192,164,173,239,28,25,98,109,248,153,30,92,219,86,156,
  18,104,152,252,240,54,42,74,62,210,247,78,123,132,222,205,45,174,155,214,187,78,113,166,
  30,62,192,110,33,10,172,14,17,62,134,157,49,144,130,187,225,154,122,64,117,69,154,55,
  16,4,176,51,220,120,16,240,127,238,189,143,201,52,135,97,205,245,254,153,221,59,208,12,
  226,69,90,61,178,21,166,172,17,201,179,168,87,33,250,92,199,106,73,215,218,167,248,219,
  134,185,18,248,173,253,198,27,20,236,62,5,205,192,250,83,46,133,87,165,196,157,157,87,
  229,207,46,68,59,198,181,223,144,31,1,56,201,89,138,73,156,107,188,42,121,112,34,180,
  94,251,217,169,225,107,121,146,148,82,146,108,50,11,245,58,79,169,163,22,237,123,23,193,
  113,83,188,246,179,135,238,40,19,221,108,125,202,248,202,113,178,190,79,210,144,136,81,37,
  83,255,48,182,238,163,48,136,147,126,146,187,248,164,77,166,69,203,48,174,100,92,133,9,
  17,154,224,94,250,254,61,91,238,179,95,180,140,212,153,7,69,58,22,117,111,181,59,244,
  214,48,237,241,144,2,118,23,95,22,244,13,183,253,64,109,46,186,226,70,155,226,1,169,
  188,205,133,137,62,155,93,97,251,147,81,98,51,131,65,28,85,204,8,250,10,140,52,49,
  92,223,43,204,177,6,191,7,189,51,48,15,79,118,247,183,176,228,0,82,227,66,224,84,
  183,236,240,122,8,28,8,138,63,127,86,230,235,22,207,69,253,229,179,138,108,161,194,138,
  80,165,78,204,15,84,197,181,197,182,3,138,253,148,18,232,163,227,70,248,48,141,206,180,
  89,38,119,189,136,177,54,30,200,57,237,182,175,108,115,65,173,142,15,19,126,71,179,34,
  120,220,172,67,189,14,6,170,136,185,76,68,145,53,157,118,66,211,228,14,47,95,200,58,
  86,121,239,98,148,54,2,101,238,180,8,178,100,127,4,66,229,149,117,223,142,145,192,98,
  140,242,75,179,253,0,183,32,224,102,175,114,71,34,195,1,87,202,225,114,70,233,180,67,
  198,145,102,138,126,104,138,208,107,246,69,173,123,187,216,165,176,90,55,15,152,108,237,158,
  146,234,233,156,207,126,204,150,201,230,240,102,162,57,212,20,112,156,11,187,183,202,24,149,
  66,152,142,160,58,139,181,20,255,234,111,24,150,23,13,41,244,27,198,169,122,3,5,191,
  10,118,61,196,56,33,35,0,209,247,219,143,92,0,174,130,211,92,64,71,16,215,122,4,
  151,80,4,12,119,7,11,219,188,34,201,46,68,4,225,209,173,76,8,11,239,156,103,42,
  3,239,194,141,249,211,230,69,2,240,42,47,146,54,31,128,96,213,24,60,92,237,98,142,
  210,119,163,126,205,98,18,215,33,207,56,109,48,101,216,146,31,145,227,195,72,201,251,125,
  250,87,191,242,117,205,75,141,204,178,4,174,5,246,197,232,245,128,94,141,0,25,124,49,
  34,124,160,253,103,202,123,212,24,197,120,214,26,186,33,33,9,45,199,226,64,26,138,223,
  92,118,141,95,208,37,196,155,131,36,127,198,239,197,11,160,54,166,157,138,29,18,137,173,
  18,13,137,130,0,110,240,191,18,200,13,255,203,160,208,117,224,176,125,238,231,117,192,134,
  159,110,242,250,239,4,242,68,213,56,39,212,65,99,1,127,88,71,30,92,230,236,121,90,
  246,113,50,44,16,54,225,168,219,238,147,161,221,200,176,139,27,244,118,77,171,147,74,27,
  194,98,244,43,198,189,129,212,93,29,72,218,213,159,235,207,58,113,97,224,213,84,71,11,
  167,17,23,44,53,10,38,36,56,149,24,143,231,146,136,124,134,245,184,224,116,48,202,3,
  25,58,47,101,68,148,5,138,54,48,251,200,48,46,108,163,212,59,53,118,16,252,250,250,
  155,140,17,75,87,92,216,245,37,84,201,166,231,32,214,188,117,237,103,14,49,132,105,99,
  197,29,69,79,129,136,126,108,216,153,63,77,249,162,142,51,68,163,114,52,200,184,168,69,
  42,13,72,56,91,89,237,195,254,112,112,64,85,10,104,59,230,59,239,87,157,52,29,162,
  123,25,102,89,55,51,33,143,33,254,61,172,250,33,88,5,192,138,31,66,142,74,88,235,
  67,176,9,88,27,201,92,243,77,54,2,31,117,147,61,223,197,225,159,237,48,42,96,109,
  105,169,182,82,51,73,111,176,233,17,176,191,98,141,199,246,51,197,10,3,243,192,18,216,
  245,149,97,248,21,114,182,168,193,255,45,193,255,85,232,75,17,190,148,224,75,57,89,122,
  63,171,66,188,59,98,77,169,130,49,102,241,243,47,29,176,100,167,204,53,112,127,36,15,
  142,159,58,104,201,56,139,6,195,240,221,92,125,243,127,215,78,19,144,138,134,171,141,146,
  50,84,99,59,184,33,125,124,24,230,127,163,120,127,175,128,5,192,34,131,20,222,215,232,
  249,6,190,107,106,157,14,140,82,196,218,251,111,12,238,119,58,61,128,226,68,179,89,23,
  161,217,138,210,244,213,26,241,178,98,202,130,21,11,169,217,214,59,89,31,57,182,218,108,
  2,165,221,81,29,179,99,228,117,244,212,85,214,233,67,9,120,254,195,68,224,31,237,123,
  155,110,65,233,3,218,86,21,160,233,65,182,28,36,247,174,98,142,82,80,71,65,250,105,
  92,13,29,224,23,35,90,29,139,159,224,15,88,24,55,84,84,209,69,85,93,84,211,69,
  75,248,7,22,45,235,162,21,3,176,160,203,138,69,93,88,44,153,210,178,6,10,66,11,
  122,59,226,95,85,13,182,88,51,165,75,166,116,217,148,174,104,200,165,130,46,45,21,53,
  228,82,201,148,150,185,180,239,189,2,37,46,137,48,222,9,16,136,220,253,49,237,44,197,
  91,204,27,174,169,231,195,221,207,238,186,208,243,43,143,142,113,238,164,26,130,142,206,68,
  77,199,120,228,24,156,131,64,97,74,7,107,125,117,120,51,31,249,91,120,51,136,111,205,
  6,72,174,121,168,59,186,51,8,233,202,188,68,159,148,190,55,39,89,56,163,111,48,253,
  194,108,39,107,218,47,11,131,213,66,24,112,112,4,131,244,237,191,237,214,183,91,175,232,
  222,240,182,177,181,181,91,231,191,155,141,55,59,252,249,179,118,163,189,5,187,55,168,191,
  191,167,170,180,190,223,231,10,200,171,251,106,66,60,236,92,23,69,32,133,174,58,27,140,
  227,230,179,44,254,77,208,221,238,85,41,98,109,4,66,211,232,26,243,95,169,199,160,142,
  202,71,181,126,170,115,58,117,231,110,56,15,84,83,126,122,57,226,232,58,125,228,203,115,
  240,112,247,112,211,225,184,35,170,242,96,138,202,7,93,81,121,174,52,182,250,96,170,234,
  155,18,239,185,242,184,234,199,125,89,189,50,174,122,99,48,85,245,79,195,87,244,239,47,
  38,103,150,200,100,148,239,206,151,103,95,86,102,231,228,183,27,245,109,89,229,141,83,0,
  222,76,13,160,88,99,8,91,3,134,112,52,112,32,96,224,20,16,221,106,170,49,197,81,
  73,150,161,37,183,218,85,173,246,98,173,150,253,70,37,110,180,175,26,109,171,127,63,169,
  127,127,25,60,114,244,27,10,192,142,162,194,187,193,99,169,240,230,209,45,119,82,90,86,
  102,95,86,179,91,126,63,69,75,254,166,91,254,144,210,178,58,251,178,150,221,242,215,41,
  90,242,55,221,242,199,148,150,75,179,47,151,179,251,252,239,41,90,186,125,254,52,69,75,
  254,166,91,254,51,165,229,202,236,75,200,89,155,137,238,255,78,211,212,197,247,95,211,52,
  117,17,254,191,105,154,170,143,186,237,77,47,222,182,88,132,122,165,49,141,59,83,53,214,
  95,117,235,126,143,119,94,87,253,59,114,161,157,118,186,189,124,215,108,33,250,121,35,247,
  192,80,181,107,79,216,78,109,252,239,213,134,31,76,220,221,103,223,18,246,195,17,157,245,
  248,46,5,97,220,59,67,144,54,225,7,251,163,228,144,79,231,248,74,79,111,147,255,253,
  91,231,51,254,1,242,200,226,7,145,139,25,14,203,174,201,232,216,85,135,101,167,207,200,
  157,232,147,230,55,203,246,65,243,111,15,4,208,7,242,112,142,70,220,162,126,202,255,110,
  245,35,45,75,177,150,235,138,3,110,14,198,181,168,168,22,13,133,221,71,213,114,55,214,
  87,37,214,87,75,97,247,241,116,92,139,242,231,144,220,108,120,220,98,2,31,221,116,123,
  35,84,179,253,210,31,124,236,27,178,227,53,4,104,221,253,140,68,126,246,184,253,195,206,
  69,23,213,123,54,112,63,200,177,119,93,124,168,120,245,247,226,114,237,239,159,225,118,180,
  170,186,253,8,239,25,131,143,220,85,158,127,44,234,46,143,255,129,38,57,223,175,55,247,
  26,123,111,94,229,222,114,44,46,204,37,200,58,169,17,62,177,183,64,235,219,89,252,23,
  190,111,226,163,119,247,138,213,137,96,138,243,202,7,183,70,189,207,90,169,236,92,219,54,
  117,149,46,26,111,94,234,81,172,175,114,6,154,37,72,9,22,127,195,204,90,42,130,190,
  113,20,160,216,189,121,76,95,75,117,148,57,49,42,13,134,55,175,68,42,10,20,48,81,
  217,213,49,130,102,215,198,40,200,4,140,110,254,144,110,4,136,124,45,1,218,230,160,49,
  67,138,244,188,166,125,78,199,7,45,59,22,234,238,64,66,1,171,108,176,122,167,26,168,
  97,133,21,226,148,117,161,236,179,221,136,109,218,136,124,117,196,137,255,30,204,53,224,210,
  107,201,212,201,99,182,67,101,168,48,224,107,34,4,233,0,19,238,17,164,168,72,48,199,
  136,93,205,224,140,217,85,169,213,216,97,17,156,132,186,100,141,166,126,246,18,136,113,209,
  85,174,143,91,160,3,202,147,167,39,122,230,14,250,71,215,152,195,97,179,221,235,157,128,
  69,131,242,47,58,163,228,125,102,137,192,2,145,138,128,217,51,232,112,123,119,127,189,101,
  29,144,121,29,82,45,165,116,224,213,24,2,81,197,4,100,103,125,119,251,216,133,228,52,
  16,58,140,217,1,92,190,21,0,30,151,81,50,252,195,5,245,10,127,30,237,29,54,222,
  236,213,183,142,15,119,246,155,41,208,27,125,7,153,140,154,136,135,28,188,129,222,216,75,
  135,45,27,100,213,91,182,213,54,126,108,213,211,49,88,142,32,48,166,197,102,15,162,23,
  119,206,50,27,166,120,93,144,27,173,181,97,112,159,25,120,45,229,224,142,214,86,39,13,
  217,194,104,37,249,111,92,254,234,146,242,153,188,58,75,112,181,226,131,144,126,146,62,165,
  161,30,30,31,212,155,199,245,221,250,219,250,94,43,185,227,103,216,87,131,69,245,87,130,
  57,79,95,93,139,141,211,207,147,151,50,239,140,219,181,158,218,25,215,107,61,117,249,111,
  98,154,191,17,43,210,197,14,56,69,47,215,107,233,106,162,183,195,225,237,9,237,8,140,
  68,126,171,82,200,95,179,134,63,127,150,28,207,174,125,119,182,72,44,104,225,152,255,229,
  13,114,9,218,65,155,40,178,184,122,246,90,67,95,61,211,110,24,199,107,215,63,95,190,
  79,30,224,159,179,247,171,15,220,252,245,154,130,51,127,12,151,45,8,200,63,95,252,135,
  250,203,102,175,85,31,18,213,102,254,129,127,27,20,94,65,14,220,203,4,161,175,65,190,
  119,51,174,203,249,162,204,94,9,30,215,22,169,215,199,22,49,133,81,72,2,213,223,220,
  109,100,118,110,117,105,162,176,1,19,20,118,171,61,18,148,207,227,215,144,169,8,25,4,
  66,78,152,183,27,216,209,148,36,12,160,128,113,135,183,171,201,124,105,176,136,75,12,125,
  113,209,49,119,32,0,13,17,208,228,16,180,129,147,130,179,138,135,5,60,209,80,44,110,
  205,38,21,61,176,55,46,128,138,162,199,54,51,101,114,171,153,24,113,172,250,102,215,171,
  161,215,175,193,38,255,183,219,127,255,251,86,175,251,215,102,7,204,162,51,29,106,150,7,
  137,222,82,3,133,37,239,172,1,41,67,131,237,53,208,106,103,202,219,23,108,172,207,142,
  77,218,169,197,3,121,136,53,148,211,93,243,105,100,163,77,156,70,16,253,141,190,227,134,
  133,211,84,157,88,226,140,154,157,148,201,180,46,85,170,99,224,96,240,112,161,25,141,81,
  160,254,127,202,234,213,112,32,109,130,117,133,178,20,53,225,119,73,48,157,68,56,139,224,
  254,11,255,162,80,227,131,26,233,52,188,134,173,161,229,113,31,134,161,40,76,25,135,205,
  96,215,44,249,180,108,130,241,28,32,246,8,5,23,121,53,76,152,5,189,106,243,251,25,
  196,101,250,240,95,221,115,176,83,204,29,29,214,143,215,119,15,118,214,119,214,15,119,94,
  204,116,207,115,249,28,100,41,59,7,157,28,231,89,105,231,94,231,48,108,66,239,250,178,
  13,182,58,151,45,253,8,159,207,221,233,39,213,220,44,252,31,234,203,218,195,179,213,23,
  255,5,122,196,238,249,135,164,151,218,11,41,24,115,231,120,98,230,232,243,49,126,63,62,
  220,92,223,173,231,214,114,133,197,66,117,245,197,12,151,95,66,159,165,45,232,172,115,90,
  82,249,166,103,115,191,189,152,153,81,59,245,28,31,132,243,185,226,98,161,83,201,205,1,
  97,251,240,99,105,177,0,127,83,237,197,251,220,60,64,44,154,223,15,208,126,14,70,137,
  223,230,33,233,215,40,175,26,149,69,163,7,40,210,205,103,249,255,0,163,207,18,169,50,
  35,85,142,33,37,145,206,219,95,12,16,16,72,212,223,159,124,192,105,148,198,126,174,45,
  181,177,43,110,0,60,121,11,52,187,119,64,54,100,207,240,125,134,249,42,76,227,246,217,
  125,222,180,130,126,169,183,196,171,242,16,86,129,26,136,148,234,225,186,123,143,193,113,59,
  208,3,16,57,247,18,72,23,204,217,156,197,131,90,210,100,233,134,35,104,73,132,192,158,
  59,247,215,64,17,190,6,231,122,131,11,248,97,58,152,53,248,113,45,186,118,70,43,105,
  12,169,159,54,18,76,246,161,103,71,245,98,240,128,185,156,139,211,35,181,197,67,164,133,
  75,30,74,36,74,57,36,1,5,181,24,67,148,109,253,123,168,70,203,53,183,32,219,226,
  154,164,129,208,122,21,5,250,243,131,5,129,131,197,104,206,162,90,18,129,168,8,84,206,
  225,101,83,77,66,153,8,132,116,184,167,153,44,209,154,111,211,134,96,8,109,67,148,60,
  212,89,128,109,82,165,26,179,84,223,212,161,26,252,43,111,190,223,171,173,101,127,101,244,
  225,19,210,88,247,16,129,238,129,237,56,213,177,191,127,80,95,88,132,159,254,193,3,3,
  130,189,82,127,61,112,53,253,243,211,170,221,145,167,40,109,230,69,39,68,49,124,80,167,
  78,120,19,106,206,117,26,225,92,111,215,15,94,204,120,156,113,110,77,223,184,113,123,211,
  60,193,93,9,54,247,186,250,243,8,246,195,226,133,101,137,151,105,128,111,251,93,148,85,
  115,156,229,118,88,218,50,208,108,227,219,72,99,16,133,90,47,102,212,87,222,150,173,253,
  227,205,253,119,245,230,250,155,122,128,240,90,110,68,25,78,225,246,126,173,16,110,193,125,
  52,177,127,194,226,59,167,152,136,193,49,0,76,203,255,178,26,63,48,214,136,133,203,35,
  97,230,191,240,128,78,59,94,108,231,110,19,28,182,25,253,89,234,232,53,237,212,230,208,
  192,44,225,206,189,166,251,68,114,85,253,234,164,11,194,200,254,233,105,239,22,15,80,90,
  124,114,82,7,106,70,7,122,58,135,176,34,105,201,224,162,198,175,13,157,233,5,104,7,
  5,128,250,176,115,222,131,119,163,206,25,133,232,67,183,117,50,45,218,226,129,227,178,241,
  187,93,165,73,204,41,207,129,60,225,185,185,91,95,111,110,194,173,17,186,130,133,76,2,
  237,41,224,124,8,150,107,183,16,172,188,161,192,166,193,99,250,133,112,15,119,234,245,61,
  134,57,186,236,116,250,79,133,87,223,123,7,4,5,128,255,243,63,182,224,176,181,190,183,
  181,222,220,162,29,201,164,62,3,175,175,119,184,4,219,64,92,124,65,195,47,121,99,153,
  190,71,102,112,137,249,141,65,102,192,234,76,115,207,20,154,106,228,17,105,245,78,171,63,
  25,228,243,220,117,18,140,42,177,14,46,16,4,226,226,18,156,127,70,212,155,183,246,174,
  163,11,40,178,103,7,180,97,253,21,233,44,18,187,44,175,92,168,27,235,173,205,29,208,
  131,49,133,255,102,73,249,102,247,120,125,239,205,110,253,152,98,213,31,159,13,219,31,145,
  170,255,197,21,114,23,189,227,45,248,212,216,202,29,155,63,45,10,112,211,22,223,197,52,
  234,10,151,64,206,107,49,134,19,20,86,209,0,153,247,192,170,95,243,54,172,218,56,179,
  149,129,160,21,148,98,54,84,153,10,56,153,99,145,15,92,161,152,38,93,37,196,32,122,
  36,83,27,78,74,17,247,125,44,146,28,236,183,197,251,85,213,226,95,80,29,254,205,19,
  152,185,92,69,127,199,179,245,95,185,255,71,16,245,183,7,250,246,210,124,131,67,176,146,
  187,43,114,135,157,222,118,7,53,206,97,127,93,22,221,238,147,28,137,107,5,115,156,66,
  227,210,164,141,145,33,196,0,148,167,0,80,138,1,168,76,1,160,236,2,80,231,33,206,
  19,200,149,128,222,29,244,112,7,149,0,168,47,138,106,142,192,182,52,118,14,145,174,147,
  206,160,89,28,222,28,226,92,117,35,115,213,21,115,165,69,124,196,39,31,29,176,128,30,
  206,216,226,208,57,215,95,68,54,27,28,149,187,251,205,216,94,214,61,208,49,101,215,55,
  209,95,172,111,42,126,244,242,150,192,51,214,248,52,235,91,219,160,68,136,229,246,22,93,
  225,86,6,186,72,227,78,180,195,79,156,237,13,40,68,182,124,184,128,44,171,34,166,174,
  187,58,134,174,72,80,53,214,198,157,51,45,174,138,91,207,234,139,248,109,210,94,67,163,
  109,84,39,247,186,19,229,178,71,167,141,105,193,54,216,94,31,112,132,189,1,253,210,139,
  25,209,174,133,186,36,32,187,110,120,195,191,249,110,96,123,187,131,222,120,41,188,57,222,
  232,117,251,253,131,203,65,255,226,184,113,133,41,182,187,48,171,180,62,212,92,21,22,75,
  112,223,253,252,130,27,108,137,6,193,194,26,1,129,187,120,76,37,126,9,30,113,59,14,
  216,102,125,179,113,208,220,135,11,218,241,65,131,164,111,211,26,126,161,92,63,111,4,153,
  235,193,71,62,37,119,18,81,107,22,177,162,193,111,52,183,182,227,120,81,113,15,207,100,
  56,172,19,239,251,29,159,225,254,231,190,58,234,221,175,35,117,110,211,50,77,82,71,206,
  99,164,22,151,237,222,57,138,8,107,57,107,67,111,112,193,27,188,22,33,140,54,129,169,
  20,200,32,26,33,13,112,214,109,242,46,210,196,12,205,109,67,120,109,67,245,237,227,195,
  211,75,152,236,95,242,254,192,128,230,137,130,106,123,121,3,77,210,150,138,169,180,5,149,
  220,229,33,214,131,90,0,171,102,254,183,105,206,223,192,127,183,104,38,115,47,95,230,40,
  246,40,12,226,236,67,242,224,110,240,70,179,177,85,63,220,172,239,109,214,181,130,6,118,
  122,57,247,195,143,63,225,149,2,22,211,82,97,133,180,12,55,116,137,204,149,23,75,149,
  66,165,90,129,227,99,161,176,184,82,91,41,213,106,48,48,210,222,84,107,149,114,5,175,
  136,11,197,197,106,121,169,88,94,174,66,73,113,113,121,169,86,40,22,150,169,69,169,80,
  41,148,170,43,84,171,176,88,89,89,174,150,139,21,110,95,41,2,132,2,181,40,84,151,
  74,165,106,233,197,140,37,46,220,224,250,157,94,161,53,104,32,231,165,111,231,234,155,98,
  187,188,156,192,58,121,187,128,19,135,102,202,162,138,224,146,121,181,139,121,27,204,235,54,
  124,111,149,69,11,166,136,143,72,234,1,250,111,13,52,58,10,19,98,97,87,93,240,229,
  58,107,224,100,171,243,160,127,74,102,191,13,186,233,11,69,17,236,187,18,222,105,221,102,
  208,29,119,238,182,99,180,188,170,243,41,85,157,179,220,195,84,93,184,159,9,85,183,69,
  20,69,167,138,36,97,7,22,228,33,9,198,221,59,180,137,86,72,236,31,108,37,138,41,
  92,118,207,111,28,141,23,92,239,70,120,160,178,90,129,56,27,84,135,255,210,125,126,197,
  104,61,238,4,123,175,46,86,150,171,181,206,66,17,228,156,202,98,165,84,40,242,223,85,
  88,195,203,244,183,80,151,192,233,97,26,22,23,107,203,197,66,103,190,64,122,130,165,149,
  106,153,255,46,193,242,93,174,224,223,162,33,170,86,117,195,202,98,185,180,180,12,21,86,
  146,220,202,98,185,80,169,241,223,181,197,90,177,84,196,191,69,67,60,60,214,8,225,57,
  181,90,197,224,16,42,178,104,52,131,39,220,230,20,9,230,13,113,230,192,122,19,238,243,
  11,106,142,184,120,86,183,197,110,160,7,84,44,173,1,46,75,149,37,160,83,177,18,235,
  171,178,88,45,45,151,24,59,221,37,12,181,188,178,66,67,245,122,254,25,132,134,247,178,
  123,209,124,206,195,69,99,145,123,73,106,196,218,114,181,179,176,100,8,48,188,56,1,2,
  184,172,102,14,201,34,246,42,212,241,22,78,99,8,139,106,116,138,222,207,122,225,12,110,
  111,70,240,177,177,223,212,235,25,12,20,74,250,111,24,79,11,61,14,138,250,3,88,125,
  246,49,181,81,11,124,171,126,97,54,74,208,79,0,233,109,135,155,52,172,162,170,107,187,
  133,126,72,23,7,178,141,236,153,251,148,170,150,2,242,124,248,15,172,185,160,79,87,63,
  8,186,104,194,177,116,248,43,128,102,34,90,216,176,189,188,238,165,198,141,107,155,81,186,
  128,245,103,6,204,13,68,103,36,79,162,122,70,214,123,173,20,57,72,6,61,15,130,45,
  98,155,207,97,7,134,231,74,80,2,147,38,114,101,143,37,185,163,74,228,144,101,203,98,
  201,61,90,155,5,117,158,138,65,219,234,45,216,106,102,172,208,86,40,181,47,187,4,170,
  64,250,26,30,184,71,216,215,14,14,166,197,65,195,129,66,29,192,222,89,224,10,102,69,
  227,10,226,213,225,157,87,74,17,201,43,44,209,11,99,5,254,71,211,165,154,55,139,33,
  137,20,200,36,88,1,166,211,102,169,236,145,167,232,145,167,36,249,28,96,95,54,12,171,
  160,39,148,104,161,250,226,29,254,58,92,115,212,150,75,53,73,220,118,197,204,118,197,212,
  118,165,204,118,37,209,206,156,20,230,44,240,90,205,69,182,218,156,165,132,164,131,189,57,
  208,140,162,40,64,61,90,202,22,137,84,106,242,112,25,206,33,181,129,186,29,72,11,98,
  38,208,50,52,174,207,219,128,26,139,89,26,153,141,77,11,116,150,97,69,100,14,175,225,
  38,238,27,236,123,30,96,224,215,6,252,222,44,216,242,43,44,31,65,67,132,75,52,130,
  243,54,79,215,117,44,42,174,194,63,175,129,90,171,185,249,121,248,83,237,107,104,6,202,
  51,196,24,155,48,168,195,43,67,213,248,17,157,167,246,115,124,82,59,95,144,156,132,51,
  160,7,7,14,66,7,112,154,85,24,245,3,176,204,70,34,215,158,119,255,60,244,238,159,
  71,111,15,82,180,110,39,183,87,215,49,189,27,126,167,7,24,150,28,75,185,179,157,179,
  251,135,227,243,143,96,20,166,25,59,124,60,108,157,225,5,150,95,203,238,54,24,216,145,
  124,200,194,42,15,92,229,33,172,194,157,237,244,80,218,48,93,226,226,179,74,100,133,97,
  226,180,229,171,190,186,99,108,220,79,215,122,94,225,13,80,96,186,161,115,9,235,225,49,
  176,30,92,88,150,217,195,50,5,244,18,130,43,69,55,12,110,113,59,60,225,139,244,250,
  240,68,9,192,163,219,225,249,49,136,41,137,248,137,215,171,36,103,231,64,31,191,231,16,
  22,215,56,22,203,211,246,238,176,123,113,213,254,193,189,216,241,12,105,248,250,53,110,213,
  109,243,99,208,230,33,171,205,30,110,82,141,162,216,237,184,215,135,131,17,136,63,10,108,
  130,117,37,167,21,53,246,18,131,176,88,17,231,91,29,84,19,240,101,145,139,19,132,140,
  123,196,25,184,197,230,13,56,55,34,66,152,182,136,219,243,233,78,68,163,215,203,38,190,
  89,243,207,7,98,27,206,117,70,12,156,222,181,13,8,51,68,188,89,80,47,238,102,123,
  203,155,45,183,119,244,22,158,29,26,7,7,168,26,59,216,93,223,171,31,230,190,203,21,
  148,218,11,194,24,247,59,171,89,207,77,122,9,42,159,226,214,224,0,155,36,230,3,118,
  141,26,120,33,149,244,186,215,251,215,237,83,124,62,89,83,207,39,51,255,117,61,108,3,
  189,32,148,18,250,131,31,195,163,236,245,49,25,248,56,44,13,185,118,97,21,254,121,157,
  59,218,107,236,239,249,152,99,17,240,57,197,229,8,121,98,225,221,235,107,208,82,17,102,
  163,159,161,206,123,98,86,30,206,80,115,65,205,221,38,180,208,42,166,132,137,160,22,210,
  188,250,245,209,129,160,7,137,239,193,230,41,205,5,142,215,164,18,143,117,70,82,96,206,
  125,160,91,8,8,151,100,124,49,192,9,44,203,114,146,186,225,171,28,113,228,24,177,97,
  89,96,9,192,136,82,22,40,30,89,41,216,43,79,42,240,225,65,127,51,54,179,153,83,
  235,205,109,234,164,70,123,119,103,123,236,116,63,195,124,63,118,194,103,2,242,204,25,121,
  252,153,38,255,115,26,169,121,98,253,53,199,157,251,104,209,78,84,175,69,145,151,239,83,
  183,230,116,15,193,127,200,22,103,20,199,204,241,119,122,142,191,240,78,57,25,12,122,140,
  41,41,187,193,223,178,243,117,109,16,139,219,116,68,131,87,96,213,116,162,165,104,248,20,
  246,229,144,220,60,56,187,143,177,39,99,207,170,54,135,199,227,51,85,34,45,68,68,58,
  208,252,177,199,64,190,183,74,253,250,83,122,214,64,90,99,129,200,118,196,150,174,204,11,
  199,34,169,68,52,168,143,10,148,107,53,128,15,90,64,200,127,255,59,242,157,159,77,240,
  245,88,238,84,220,205,119,244,151,133,189,254,4,216,130,12,149,0,240,230,163,1,7,165,
  141,61,180,50,216,52,207,120,145,42,238,59,223,88,228,154,127,52,114,60,31,116,27,173,
  24,37,75,248,124,41,186,54,141,200,0,66,141,166,151,50,6,93,121,17,21,110,178,129,
  223,129,143,189,223,80,27,243,155,143,171,19,60,178,90,68,195,7,212,137,31,11,15,112,
  138,148,233,3,232,93,202,139,69,120,45,128,183,135,106,185,186,188,178,180,82,126,97,11,
  75,160,109,45,45,151,139,240,178,80,88,42,130,242,118,185,38,74,143,209,45,3,72,92,
  93,42,44,173,212,202,165,218,210,74,101,121,165,102,171,184,239,103,133,69,0,84,46,172,
  44,47,215,224,223,165,149,194,82,188,98,9,13,75,171,43,197,106,101,165,2,213,139,203,
  160,51,174,154,170,245,131,195,198,238,254,30,170,13,106,72,42,140,21,106,94,150,76,45,
  251,212,132,118,110,74,233,208,86,202,34,90,20,134,218,170,197,199,203,46,166,80,195,43,
  49,133,44,167,134,90,45,232,130,155,85,239,140,90,89,232,188,178,161,205,222,111,57,117,
  157,184,159,187,95,205,169,7,64,175,54,171,173,35,149,13,236,242,88,216,94,131,74,74,
  3,245,55,94,184,168,137,6,80,154,187,47,89,0,160,80,40,251,248,221,201,14,73,227,
  64,255,185,131,40,16,240,31,54,195,69,19,92,11,5,76,230,225,160,234,100,2,226,179,
  208,170,46,202,252,63,90,165,0,136,173,100,24,28,60,131,156,185,176,74,185,219,59,62,
  130,249,171,172,141,230,121,197,210,226,202,242,10,60,144,161,62,124,105,25,84,239,160,99,
  58,133,191,43,229,165,234,242,98,181,82,69,37,141,108,117,102,46,153,183,48,176,135,68,
  93,217,219,201,9,25,114,230,70,120,134,92,13,0,143,51,16,23,97,33,139,23,66,101,
  174,74,230,207,163,62,171,252,249,209,87,237,226,29,136,187,112,124,128,235,251,16,68,13,
  253,154,116,13,187,148,188,253,48,102,209,174,50,37,14,9,165,141,140,239,152,48,74,242,
  155,8,132,52,111,222,212,238,144,108,226,92,86,119,219,59,107,24,230,245,246,210,109,53,
  203,214,201,230,131,180,8,225,48,23,224,233,196,207,83,100,90,102,94,151,21,115,228,31,
  103,226,182,78,162,27,250,59,158,160,110,233,243,170,6,211,116,76,212,12,28,199,248,207,
  133,167,77,213,244,87,207,82,208,255,108,171,127,78,53,134,112,68,16,95,252,120,225,154,
  89,216,160,110,102,169,159,225,147,118,151,237,131,114,28,183,198,49,42,16,42,134,188,46,
  159,83,199,21,181,85,186,60,109,16,241,217,176,168,46,39,173,110,5,157,71,240,217,120,
  80,41,174,209,8,144,237,73,60,212,89,237,20,169,151,15,172,12,244,104,238,76,157,244,
  17,241,64,116,203,130,178,142,144,45,237,192,82,104,153,137,147,67,222,201,17,18,148,77,
  195,134,22,165,19,217,80,247,205,83,233,116,113,69,170,244,159,115,101,212,167,175,177,65,
  171,26,82,41,7,62,170,52,150,163,59,7,111,6,160,46,248,176,23,49,96,23,97,6,
  1,116,232,31,182,254,246,14,67,244,189,176,206,28,232,166,208,38,118,163,14,53,108,7,
  60,107,129,95,41,88,239,29,64,49,64,28,165,228,109,66,156,197,177,80,217,5,11,207,
  14,191,60,73,254,237,8,187,89,38,50,178,162,129,44,222,83,92,176,231,133,192,74,229,
  124,165,16,179,204,121,183,35,169,167,76,17,128,22,236,231,128,119,253,234,98,181,90,89,
  42,35,14,84,125,1,100,151,21,144,93,106,68,14,107,45,162,57,119,65,60,251,105,120,
  168,160,200,35,10,168,97,212,223,172,97,81,108,28,10,161,63,201,64,60,11,150,134,125,
  34,96,27,228,227,214,143,7,32,235,30,109,212,143,143,222,89,11,217,83,72,2,123,244,
  238,24,92,38,32,75,39,229,124,133,151,234,66,172,188,213,237,145,133,94,174,88,195,10,
  198,16,146,82,200,230,220,131,64,234,171,225,68,178,236,108,141,15,40,81,81,40,133,1,
  80,206,108,56,117,251,149,173,97,23,125,231,126,248,164,175,236,19,212,85,14,16,51,170,
  155,51,167,38,242,143,127,208,127,95,129,216,204,186,40,165,141,137,180,120,48,45,138,212,
  162,194,45,62,231,200,213,46,5,161,79,19,35,244,201,128,47,17,248,234,163,17,18,118,
  162,208,196,60,80,148,112,222,142,222,249,179,38,159,30,114,226,25,232,246,206,76,7,247,
  188,38,159,190,111,239,180,127,145,28,66,226,96,135,42,62,119,226,145,35,10,154,57,192,
  139,81,224,11,178,113,226,252,252,20,235,224,33,189,131,210,36,29,140,193,255,83,58,248,
  242,88,240,79,34,79,101,58,236,39,32,78,250,60,78,78,7,187,212,216,43,10,101,110,
  109,68,41,94,198,78,186,96,161,7,206,26,155,196,87,242,226,169,178,211,191,227,215,183,
  232,154,188,234,162,155,128,35,255,170,93,96,88,80,156,169,116,123,224,197,208,208,18,114,
  62,198,240,22,20,244,196,62,244,171,238,84,27,93,28,105,235,241,175,67,54,104,102,150,
  175,113,198,42,124,45,209,183,156,53,189,1,229,56,121,215,205,169,153,38,72,11,106,169,
  206,59,60,145,106,126,231,174,98,244,202,92,51,13,137,93,80,181,133,53,205,207,112,134,
  240,26,164,235,169,167,54,93,221,20,25,130,205,229,216,235,51,60,2,84,125,234,178,178,
  200,167,20,15,25,207,23,168,251,118,253,135,227,183,141,3,122,168,55,131,153,53,221,128,
  150,67,85,108,213,127,168,239,30,127,223,216,106,237,24,160,126,233,78,29,35,220,173,178,
  42,25,15,52,243,102,139,170,254,250,15,45,97,128,227,23,229,205,178,186,189,211,215,63,
  154,100,231,7,154,195,147,5,149,81,193,7,240,36,32,89,157,213,176,159,131,3,115,88,
  192,41,11,207,209,2,207,105,88,191,8,168,44,135,245,139,124,30,134,245,43,80,191,18,
  214,175,196,161,87,241,50,94,168,134,245,171,56,201,97,253,26,26,94,23,195,234,53,150,
  14,212,21,94,251,5,181,6,176,31,180,153,153,240,22,146,151,213,238,181,103,82,100,235,
  125,183,38,168,160,86,52,215,207,11,114,46,56,144,231,108,25,209,200,210,151,205,44,101,
  59,9,124,222,214,244,249,107,20,161,74,26,66,197,12,132,42,2,161,162,135,144,64,150,
  128,91,132,138,19,33,84,77,67,168,146,129,80,85,32,84,241,16,18,200,18,112,139,80,
  101,34,132,106,105,8,85,51,16,170,9,132,170,30,66,2,89,2,110,17,170,250,103,22,
  119,185,160,172,113,216,181,184,184,88,68,107,76,233,180,230,154,215,116,175,205,121,84,209,
  27,124,204,113,196,159,201,164,125,252,34,87,215,55,127,119,136,38,246,36,129,139,132,207,
  50,87,37,184,109,235,56,141,192,221,66,62,161,148,95,182,41,182,250,25,52,137,242,207,
  91,61,44,49,34,121,76,209,52,115,191,107,113,251,70,82,69,34,240,68,216,57,138,41,
  177,221,23,167,234,222,138,10,94,111,87,236,34,199,93,50,224,132,81,156,77,28,83,75,
  107,178,177,27,113,154,49,78,45,210,199,37,195,165,69,52,181,126,45,142,159,75,150,90,
  159,92,129,64,218,185,210,94,1,190,87,16,34,29,98,247,82,219,218,145,210,242,228,138,
  172,8,19,253,7,185,25,235,130,162,46,40,122,5,37,93,80,34,3,228,120,71,107,136,
  218,92,72,31,107,193,226,80,100,38,74,13,31,132,161,139,251,44,25,127,59,81,20,234,
  90,10,233,119,147,41,40,212,213,20,234,122,20,234,106,10,117,61,10,117,53,133,186,99,
  41,212,125,58,133,186,19,82,40,214,61,171,186,222,106,53,98,4,17,133,199,54,188,148,
  30,31,130,135,202,86,124,28,11,209,182,193,212,152,161,68,71,146,71,61,121,167,103,245,
  118,70,179,25,86,119,20,157,246,117,106,203,181,90,220,106,28,194,211,238,38,133,41,74,
  177,94,196,88,123,152,30,30,159,109,98,86,140,178,92,91,51,166,215,216,232,182,71,22,
  155,189,49,216,72,127,187,121,199,110,206,113,146,227,99,77,136,137,30,206,112,128,108,185,
  95,216,190,145,20,106,30,246,104,190,230,225,43,95,247,142,92,140,235,111,27,135,135,141,
  119,117,194,150,206,178,206,85,119,52,194,180,45,234,161,84,10,175,170,136,49,170,219,95,
  202,82,83,193,221,170,111,238,111,213,143,223,193,98,218,71,233,187,117,212,180,29,193,34,
  247,123,24,53,223,108,176,210,26,130,24,237,183,182,243,30,14,194,95,124,6,82,7,180,
  123,186,231,38,216,232,224,94,71,97,223,105,34,223,75,63,36,251,25,67,14,151,139,24,
  164,5,177,125,189,246,119,120,46,133,144,128,23,26,107,62,146,90,131,253,219,27,240,137,
  111,161,55,40,61,169,218,58,179,171,127,79,54,248,16,169,228,118,185,186,26,230,126,29,
  135,201,106,206,138,140,111,163,15,45,252,162,244,163,149,8,129,50,90,218,227,142,92,14,
  57,12,14,61,42,163,17,117,101,105,185,182,84,90,89,174,25,183,38,244,38,43,21,193,
  151,166,0,30,43,179,250,213,173,180,88,225,215,45,183,125,97,105,9,92,85,202,203,232,
  97,198,21,123,40,155,92,182,251,245,95,111,219,61,209,157,176,60,70,39,54,29,227,71,
  3,108,27,197,178,59,188,177,132,73,27,158,237,175,82,172,213,106,74,203,141,238,109,85,
  227,236,69,142,115,254,160,240,65,176,52,197,104,10,229,98,185,176,28,31,207,135,228,147,
  183,214,72,107,106,118,6,252,60,254,126,191,185,187,117,176,127,104,99,199,96,210,30,116,
  67,104,95,88,79,9,39,41,145,150,159,100,85,215,10,151,177,91,16,47,23,116,146,169,
  83,202,253,94,76,249,94,82,223,245,179,159,148,199,50,122,166,36,63,198,154,102,65,141,
  231,64,56,8,179,224,76,131,253,72,121,180,244,153,50,201,91,147,126,41,146,111,50,226,
  22,175,212,210,111,145,225,52,235,219,187,245,205,22,189,163,106,47,1,126,48,132,196,45,
  208,157,250,145,119,72,158,56,56,205,58,215,247,40,8,150,163,83,65,36,186,14,12,190,
  137,193,66,53,76,109,106,197,176,67,208,119,234,113,211,13,4,18,83,189,235,32,5,32,
  6,123,76,26,5,100,43,30,243,191,58,149,19,222,102,108,127,17,26,110,236,214,247,182,
  208,152,228,237,209,110,171,113,176,251,35,146,0,60,116,46,32,57,216,5,63,184,90,79,
  44,251,49,113,127,162,63,131,194,11,15,238,196,248,230,30,222,12,57,74,177,193,131,188,
  29,12,201,165,65,77,128,81,227,7,86,115,79,130,206,243,119,191,190,181,21,235,30,14,
  116,217,23,90,129,103,119,150,97,115,247,75,156,111,184,114,8,79,168,8,116,98,202,73,
  10,118,231,123,53,107,253,120,199,30,174,27,213,92,88,144,70,206,70,91,197,193,254,221,
  36,216,123,148,8,172,192,148,3,74,196,140,107,111,191,249,118,125,55,44,59,216,217,223,
  123,227,125,219,93,127,187,81,111,182,104,190,140,77,144,195,118,195,221,229,240,100,231,121,
  223,97,110,171,130,38,102,72,114,171,11,218,185,96,130,173,173,9,247,38,245,184,248,26,
  105,51,150,60,227,70,63,1,249,28,242,236,76,117,154,186,39,209,26,115,101,215,176,211,
  229,191,154,137,67,14,80,96,160,19,30,190,92,249,143,57,126,99,125,7,163,124,254,51,
  56,184,249,61,230,56,86,171,32,118,14,243,176,50,79,98,167,181,123,4,71,90,143,61,
  132,157,85,246,189,187,202,182,247,65,173,112,183,61,184,216,234,92,3,11,247,236,131,197,
  85,244,135,88,59,181,242,149,53,134,134,98,27,253,26,105,164,175,224,251,111,142,235,63,
  28,148,236,91,208,224,194,4,62,228,247,123,229,6,126,142,96,57,34,218,156,251,195,226,
  45,255,158,21,43,63,4,45,125,34,224,251,30,220,83,18,174,0,255,250,64,152,130,242,
  158,179,200,190,229,116,20,251,223,9,142,138,202,97,187,20,215,209,31,35,228,16,22,228,
  101,211,126,53,131,182,17,2,186,140,198,146,40,114,188,153,42,56,240,213,88,193,54,125,
  247,86,205,127,187,152,191,105,174,111,53,210,149,16,23,202,159,68,220,42,217,246,5,158,
  240,180,175,73,99,56,84,23,218,188,107,95,229,68,128,145,182,17,54,228,202,174,182,81,
  212,109,252,234,218,69,243,20,19,15,218,55,90,106,170,99,213,192,127,237,235,101,218,216,
  92,47,117,161,29,16,67,76,84,55,58,64,148,224,186,37,240,224,17,110,60,170,30,163,
  32,163,103,225,106,210,119,174,37,123,47,45,242,179,155,235,249,191,132,91,226,35,62,51,
  227,223,243,234,111,2,205,209,101,237,236,193,21,237,39,119,226,118,241,93,48,101,214,136,
  136,49,157,145,46,136,4,157,251,39,128,87,86,82,111,85,236,59,19,4,111,245,133,72,
  19,38,44,166,214,92,191,30,210,94,152,154,129,80,185,22,200,153,171,31,146,255,69,69,
  131,115,236,34,7,183,103,179,178,160,244,49,179,38,148,210,46,75,71,74,240,187,65,171,
  72,138,15,13,10,29,94,91,17,131,48,215,214,147,223,166,149,104,238,90,119,233,56,132,
  214,227,37,94,190,23,13,102,228,69,49,76,43,222,212,65,29,3,40,105,147,132,230,131,
  32,232,251,198,166,110,120,196,216,254,27,19,118,81,80,98,81,218,56,153,184,59,93,179,
  255,121,51,211,206,148,173,180,189,172,23,169,209,141,125,9,183,18,1,104,206,51,219,139,
  175,63,214,197,168,137,109,184,38,178,105,38,127,182,147,191,222,188,142,9,47,250,104,18,
  91,127,6,181,125,208,14,43,216,75,178,154,55,25,84,59,101,130,62,36,255,2,30,160,
  249,20,89,171,66,173,14,168,50,15,47,219,144,197,101,245,133,115,176,170,80,157,52,66,
  237,164,227,95,59,136,55,130,121,250,254,6,248,78,205,122,39,51,241,192,131,225,224,164,
  243,115,110,197,122,113,189,80,81,133,222,116,196,129,182,126,19,181,26,86,85,55,7,157,
  243,115,8,197,69,25,86,17,150,220,93,86,220,93,164,72,122,230,23,88,181,126,178,191,
  62,233,93,4,214,140,16,197,19,183,163,7,23,95,118,240,152,1,255,146,82,105,137,182,
  17,213,156,15,171,22,169,42,191,7,195,185,84,4,141,34,70,243,121,200,108,84,138,55,
  250,148,217,168,28,111,116,159,217,168,226,52,170,148,86,10,149,50,5,156,30,135,98,53,
  222,240,97,44,154,53,106,72,103,109,165,92,44,161,105,214,39,250,127,12,95,93,170,44,
  45,161,70,52,19,194,82,58,206,217,93,47,171,105,51,77,242,57,142,174,189,64,120,63,
  72,91,87,134,98,236,138,65,182,218,53,139,84,74,87,145,216,122,118,33,199,99,233,137,
  160,120,207,166,196,12,185,126,100,219,56,215,26,139,170,28,182,5,33,135,190,46,54,120,
  250,224,3,54,32,6,234,160,22,225,23,241,254,141,77,239,150,242,156,134,20,15,157,254,
  45,231,147,247,45,158,149,152,202,21,3,123,232,83,72,232,123,126,158,90,124,214,57,109,
  63,212,239,141,27,138,61,145,85,139,109,72,66,2,0,76,96,125,178,195,163,119,3,175,
  95,15,18,73,193,69,27,126,219,197,67,153,201,178,94,192,239,106,78,71,158,177,210,128,
  9,29,85,241,250,5,140,60,200,34,174,156,118,134,114,225,59,4,62,188,30,220,100,17,
  23,126,194,233,3,222,0,33,233,174,161,209,213,201,176,157,82,12,79,176,61,213,212,121,
  126,145,114,183,132,238,131,115,218,43,119,39,114,200,61,60,218,227,83,69,249,226,42,177,
  244,240,182,111,92,122,66,119,32,215,91,232,179,144,198,77,187,145,250,67,249,24,139,110,
  208,217,154,68,27,36,152,170,213,232,159,15,4,181,2,40,164,211,246,4,217,158,21,15,
  102,122,86,28,163,240,38,125,87,62,83,229,103,194,96,221,212,113,198,197,245,148,163,147,
  245,81,151,241,116,105,56,91,141,102,140,106,230,122,215,238,61,138,122,65,251,51,239,131,
  162,166,232,94,82,211,111,238,81,117,44,244,233,168,236,183,206,162,118,80,247,81,84,63,
  216,135,204,93,49,186,31,192,27,192,141,71,241,107,161,36,118,8,238,177,36,241,5,121,
  142,55,35,2,242,181,249,83,205,130,131,142,156,7,219,200,155,129,40,180,241,210,122,246,
  172,240,145,9,111,72,172,60,178,128,77,118,115,96,118,62,208,248,52,201,152,178,10,160,
  204,251,225,176,202,53,227,127,232,84,117,215,136,64,198,91,29,167,194,49,58,122,46,121,
  7,130,128,116,22,253,136,147,39,81,176,203,73,193,82,93,254,109,45,53,4,151,101,138,
  7,251,209,117,134,252,125,220,50,75,223,233,99,23,222,140,115,74,136,160,123,150,153,251,
  236,214,32,52,210,127,105,134,43,134,224,112,92,93,207,103,185,33,164,103,94,152,6,238,
  115,175,75,125,182,105,141,155,215,62,17,61,71,157,9,70,238,169,205,102,252,193,81,110,
  129,168,249,145,112,197,12,217,179,86,216,94,250,93,72,129,101,154,221,229,31,116,2,39,
  218,78,126,71,65,155,201,55,156,36,90,228,155,221,110,211,239,55,231,41,197,29,145,23,
  153,49,128,125,222,238,145,147,176,180,12,53,219,22,124,27,91,199,235,205,250,122,108,239,
  54,129,140,235,144,151,211,219,191,102,115,198,119,51,134,186,254,30,53,163,206,151,157,14,
  2,241,246,162,208,82,222,156,30,23,87,83,10,74,162,137,139,211,80,254,82,219,56,24,
  210,251,85,127,212,59,96,63,21,27,240,14,26,78,93,95,118,134,157,177,178,200,232,151,
  135,77,135,14,23,96,214,220,63,219,140,136,40,62,212,75,247,183,66,91,226,244,94,95,
  172,96,225,121,173,163,183,160,88,15,81,126,36,239,129,89,154,119,3,32,190,253,177,88,
  105,110,190,239,40,51,6,118,118,98,104,218,21,55,114,5,35,165,184,5,47,168,38,123,
  213,212,77,34,93,73,127,126,121,107,147,139,187,15,185,91,206,58,185,215,180,23,174,241,
  174,57,58,190,110,15,71,199,231,24,98,8,88,239,119,31,146,255,139,63,204,106,186,55,
  54,118,211,111,156,14,29,179,188,75,159,221,102,200,152,205,128,230,62,98,57,35,77,203,
  3,219,25,231,26,30,24,153,147,111,179,0,172,108,203,66,131,141,192,55,200,227,64,194,
  23,72,16,179,153,66,202,73,163,255,199,221,13,38,160,125,220,126,106,193,118,220,23,175,
  181,51,78,109,113,136,210,186,181,133,137,65,144,239,196,18,175,217,24,168,9,67,0,200,
  30,158,115,234,37,92,55,139,145,153,197,103,158,122,177,181,154,245,86,115,223,121,74,151,
  235,2,100,153,193,151,89,28,99,86,135,94,30,208,127,124,186,189,21,146,53,215,51,2,
  204,196,83,205,45,34,19,253,180,153,214,96,253,121,158,106,162,237,76,167,76,181,141,250,
  247,57,176,185,161,112,35,123,141,195,125,152,247,131,31,221,249,134,4,228,163,1,160,120,
  253,240,204,147,238,215,63,233,222,104,43,252,64,51,164,81,152,102,161,156,0,40,195,194,
  85,84,87,209,135,76,35,130,145,251,100,109,177,176,116,67,83,156,88,32,118,53,165,181,
  166,101,41,219,138,149,89,202,171,255,170,124,136,118,144,54,118,130,183,116,157,229,139,115,
  236,179,106,51,21,178,79,127,93,77,181,82,178,249,66,202,66,249,2,44,226,41,171,101,
  220,114,121,204,122,121,226,130,249,253,87,140,183,100,220,41,154,104,221,136,133,147,182,114,
  236,210,225,181,19,55,126,186,185,90,251,208,26,12,250,79,177,85,248,144,116,174,38,178,
  62,112,58,138,155,30,132,22,6,216,232,79,104,94,16,37,234,20,111,208,161,248,159,98,
  42,52,169,157,193,215,96,75,224,205,229,215,109,72,240,212,25,252,131,172,8,16,237,105,
  77,8,176,205,135,164,15,187,216,230,190,122,94,235,37,227,236,101,34,172,133,6,78,38,
  1,217,154,77,187,53,181,33,84,119,50,86,20,25,103,138,45,84,152,58,45,52,144,178,
  216,78,97,58,21,205,246,246,103,97,111,25,235,228,63,211,128,42,14,212,100,207,141,66,
  149,43,32,58,186,36,156,40,159,42,241,157,22,249,126,40,242,251,205,229,82,119,85,22,
  251,78,207,79,248,213,50,241,231,89,167,127,16,43,183,200,79,203,208,109,203,15,201,8,
  56,226,193,229,195,168,123,74,158,224,207,192,212,77,81,31,46,222,39,183,230,213,213,109,
  32,36,97,152,164,118,15,215,158,181,0,55,192,76,25,106,56,221,122,171,108,204,2,201,
  56,116,16,37,142,27,69,121,74,250,131,254,1,103,72,49,110,234,228,144,169,171,60,164,
  85,193,222,181,13,5,47,159,166,145,174,215,68,24,91,74,9,146,228,56,21,200,44,255,
  241,201,65,125,232,183,51,31,24,255,132,189,107,193,231,116,53,210,102,126,45,68,32,13,
  56,70,116,12,75,140,38,82,93,170,26,24,4,218,84,235,210,52,118,137,227,137,155,227,
  225,65,125,243,104,119,189,41,30,164,116,122,114,165,195,176,158,1,126,137,85,225,75,102,
  19,212,55,218,253,72,183,28,171,154,77,201,103,98,96,230,28,159,115,167,6,59,158,31,
  122,159,142,34,177,148,162,61,195,251,121,125,239,176,209,250,209,235,221,31,120,28,3,83,
  203,197,66,126,38,76,218,14,30,1,159,221,94,41,104,85,126,74,255,42,163,88,100,207,
  120,78,28,233,19,167,2,125,101,76,149,151,100,46,5,77,134,163,7,147,42,212,209,242,
  212,233,43,157,245,183,160,226,240,189,244,11,230,69,44,210,8,134,142,189,191,83,201,27,
  236,106,26,90,27,240,162,121,70,217,21,174,84,140,214,232,49,233,51,173,40,221,21,217,
  83,199,111,93,221,137,154,95,30,161,204,249,138,68,179,223,173,175,55,55,247,215,91,162,
  225,169,62,50,41,216,144,250,123,53,86,65,114,199,240,99,180,9,165,133,77,163,138,173,
  37,215,88,4,91,222,166,17,140,221,13,106,190,243,198,220,20,63,77,26,176,136,146,213,
  116,115,220,220,63,122,179,3,233,33,14,211,58,180,20,72,233,217,84,240,80,144,223,9,
  151,135,216,118,146,83,97,101,224,72,49,4,161,202,158,30,117,120,165,86,17,7,82,246,
  76,199,207,166,113,189,59,167,84,172,247,212,124,11,24,57,165,222,228,24,232,66,204,128,
  213,61,28,105,233,66,255,136,181,15,212,161,246,176,68,237,154,227,242,189,150,11,190,69,
  51,52,200,124,205,150,119,217,20,136,120,184,218,95,171,241,74,156,162,210,253,224,46,119,
  209,143,183,254,100,95,238,194,19,37,188,226,26,206,7,62,17,227,203,94,116,119,220,218,
  105,108,254,51,178,240,5,120,155,217,17,44,45,98,223,223,182,239,187,87,183,152,247,44,
  90,10,119,14,40,157,117,147,228,197,225,120,227,144,37,52,160,11,204,115,156,222,135,56,
  30,199,13,36,99,28,89,113,167,14,119,234,117,185,180,70,151,157,78,223,40,54,204,15,
  79,220,194,70,82,232,137,53,247,36,14,83,160,68,13,249,91,72,59,225,185,140,21,93,
  118,77,81,228,220,239,196,4,10,133,162,125,199,14,208,77,227,136,94,7,17,180,61,78,
  120,232,127,51,34,82,42,149,229,227,87,244,179,193,169,36,212,238,111,219,28,104,245,6,
  83,51,152,175,108,204,132,82,124,240,141,130,162,199,190,134,173,69,72,60,91,120,48,232,
  181,189,248,72,18,27,28,252,186,252,45,165,84,242,131,20,253,160,121,189,51,146,57,249,
  12,193,78,19,94,207,0,138,227,218,148,164,144,228,87,58,241,157,47,211,58,229,161,198,
  214,148,120,217,176,86,98,18,12,91,201,11,198,47,91,200,248,127,110,223,6,117,235,114,
  42,236,180,100,221,151,107,49,216,142,212,234,32,25,57,66,229,59,20,155,215,219,162,30,
  100,208,110,105,169,140,133,216,200,29,108,86,73,229,94,5,231,129,203,57,84,109,9,2,
  191,57,233,107,15,36,49,52,12,150,203,69,197,160,232,33,14,108,67,3,43,70,128,45,
  164,245,35,252,83,135,87,214,73,76,4,73,59,191,216,61,106,25,237,104,160,46,200,210,
  141,70,180,1,105,106,211,240,163,146,138,87,131,112,176,230,139,17,127,3,229,42,200,143,
  171,194,229,195,136,7,42,169,236,249,133,238,241,10,188,130,186,16,29,237,6,71,131,25,
  145,174,192,130,114,228,133,73,137,191,170,234,72,21,129,192,144,41,82,138,236,155,86,178,
  246,63,73,193,74,89,7,90,249,57,82,159,135,155,125,164,199,114,168,167,37,86,143,126,
  55,231,161,48,65,51,133,42,85,118,180,168,176,213,237,32,121,134,221,211,120,249,91,156,
  199,30,149,70,198,160,206,86,181,66,228,89,170,39,221,57,73,66,32,164,251,208,227,25,
  164,8,63,173,230,250,222,33,197,127,19,147,75,54,39,20,245,141,109,20,195,175,235,200,
  35,68,145,164,145,122,232,182,166,173,91,194,224,154,207,12,91,102,228,131,49,54,31,254,
  235,185,232,136,216,85,228,60,138,125,219,16,142,239,74,151,102,86,147,214,95,176,170,209,
  183,133,141,215,213,90,198,104,109,154,160,9,160,58,245,124,136,178,162,74,166,113,124,51,
  56,222,46,4,169,65,166,76,168,145,131,255,89,135,82,37,23,177,114,146,42,201,220,95,
  171,178,54,165,197,98,151,76,241,181,106,129,80,97,137,254,227,101,155,231,22,234,121,30,
  98,107,152,64,116,152,120,3,143,235,123,29,181,152,17,185,175,202,108,34,239,142,223,188,
  249,225,248,240,170,123,3,30,145,195,97,167,135,57,57,2,31,47,90,15,177,113,239,237,
  70,191,190,147,15,47,237,146,73,148,78,112,232,4,83,170,209,59,241,134,194,89,214,219,
  37,202,26,162,12,30,74,156,139,156,26,107,192,162,121,79,53,127,55,97,243,93,213,92,
  132,255,87,254,122,23,152,1,224,2,180,245,58,181,155,160,209,22,210,104,10,154,236,76,
  56,122,120,129,27,92,153,82,221,18,245,217,208,72,235,180,116,60,253,148,84,55,80,243,
  165,6,64,224,100,222,47,127,199,103,76,249,177,17,35,187,167,241,161,182,146,232,231,141,
  24,9,90,239,98,95,55,162,95,91,209,37,180,145,178,176,226,139,208,177,151,118,22,85,
  79,36,37,83,51,208,98,43,104,196,145,71,192,191,17,59,185,196,98,107,44,3,218,174,
  7,109,55,113,86,220,132,75,14,133,70,177,232,158,119,86,246,118,162,244,143,126,221,216,
  113,104,74,171,216,140,151,187,112,114,69,148,115,119,134,15,75,58,32,116,73,167,13,252,
  93,82,118,232,59,50,225,132,10,184,83,50,201,214,85,98,40,141,194,71,66,1,151,251,
  93,73,16,52,178,33,120,199,195,54,250,88,10,92,161,34,66,148,205,59,133,20,55,90,
  181,168,3,119,228,117,112,82,243,182,212,199,49,225,88,116,94,200,173,229,98,138,78,145,
  186,99,37,165,206,138,168,228,62,25,165,170,202,188,3,223,176,34,207,14,204,184,136,160,
  53,156,235,184,164,104,130,113,94,165,181,92,230,139,187,31,60,200,221,111,188,213,82,27,
  153,110,252,54,59,233,109,52,234,126,155,119,145,54,102,54,189,70,156,66,12,234,139,244,
  91,152,111,139,228,1,155,14,75,179,88,168,152,114,178,170,99,67,157,157,138,225,216,150,
  91,208,82,157,55,162,230,142,195,67,182,233,140,64,126,180,229,174,111,103,37,255,110,235,
  87,88,146,185,235,55,243,10,230,173,227,216,109,43,186,138,195,171,91,214,218,157,122,233,
  62,102,229,62,98,225,62,98,221,62,102,217,78,184,106,83,111,121,219,90,99,177,45,76,
  61,194,171,90,188,208,137,229,150,125,7,161,3,84,51,254,152,114,67,210,220,217,195,173,
  119,217,205,226,108,169,181,147,221,202,80,209,105,181,145,133,226,70,42,138,27,239,178,155,
  197,81,220,216,201,110,21,65,49,149,235,184,98,132,167,148,74,4,163,105,177,12,164,68,
  161,22,51,40,45,202,188,51,18,77,132,93,77,220,5,10,33,74,46,80,98,70,240,226,
  252,101,184,167,94,137,113,38,170,146,102,238,182,54,143,49,97,166,203,11,247,124,230,248,
  110,156,103,11,151,114,17,168,188,32,231,193,79,117,192,173,198,217,238,130,82,144,94,234,
  228,100,106,234,218,39,109,253,41,210,110,163,177,126,168,28,251,156,122,153,12,9,6,243,
  206,178,7,149,227,139,245,164,194,228,131,111,83,230,234,106,164,98,170,13,255,153,19,136,
  207,27,100,236,61,5,19,241,233,251,19,18,117,179,215,189,190,238,156,29,146,15,228,54,
  48,56,126,102,14,46,220,242,234,212,19,186,224,115,121,111,35,241,25,88,56,74,228,24,
  137,79,37,142,203,211,47,21,188,79,221,241,85,164,26,68,161,126,118,209,97,13,116,122,
  255,119,197,224,120,44,249,113,162,152,225,66,197,187,146,96,204,15,58,97,228,189,248,216,
  166,217,89,174,86,202,43,203,85,186,152,66,136,161,149,90,181,88,229,72,136,16,13,191,
  90,42,212,56,194,144,10,172,196,45,49,216,101,25,162,204,47,85,87,42,5,106,89,129,
  100,65,69,8,171,95,129,95,126,109,74,200,10,20,56,49,95,110,46,65,17,118,12,161,
  81,232,15,90,91,247,198,67,251,31,208,224,149,242,7,85,249,124,121,190,137,176,74,77,
  128,154,131,4,51,156,47,145,218,31,180,10,118,2,148,203,131,38,2,190,187,57,253,185,
  116,199,224,245,180,254,166,216,85,244,225,64,124,160,224,211,87,224,165,229,87,66,197,206,
  38,70,97,84,49,171,172,52,114,87,164,87,88,91,140,218,236,5,231,75,193,122,242,210,
  205,67,150,149,179,106,19,175,247,157,71,196,162,192,183,10,235,184,175,13,6,15,66,128,
  128,239,107,53,45,81,23,11,234,173,5,128,91,120,235,105,21,93,41,230,29,0,220,227,
  219,84,30,233,183,167,182,105,171,68,33,101,21,94,64,233,86,81,63,28,148,145,148,120,
  174,3,45,117,178,22,112,17,231,135,12,232,152,251,178,160,148,71,185,33,48,241,33,139,
  62,137,102,196,75,253,129,45,228,56,69,212,169,160,126,188,118,49,82,187,148,90,187,20,
  169,93,78,173,93,142,212,102,188,165,11,143,156,143,0,229,72,197,162,87,177,148,86,177,
  228,85,44,167,85,44,171,138,188,22,61,54,21,4,17,8,42,128,41,69,22,143,51,195,
  75,130,33,60,14,84,81,130,42,61,9,84,73,130,42,63,9,84,89,130,210,147,169,31,
  87,84,196,190,204,211,40,232,118,214,201,87,93,206,107,56,38,222,148,19,202,144,222,23,
  32,134,161,86,225,108,94,182,135,189,110,39,31,250,182,165,105,11,51,238,50,234,213,161,
  127,183,174,42,113,184,177,182,122,54,208,113,86,70,165,75,165,177,218,209,106,22,251,166,
  213,237,83,169,224,243,212,128,31,235,151,150,139,202,104,199,104,147,241,85,120,222,246,169,
  180,169,121,6,148,216,2,14,238,203,135,48,191,36,131,50,198,81,51,239,117,192,47,182,
  119,147,119,149,121,129,14,79,135,0,115,226,154,189,164,227,143,147,119,234,248,5,44,148,
  44,88,11,124,45,164,248,201,196,201,56,224,153,46,195,222,187,81,32,5,122,150,11,246,
  40,250,43,93,65,89,200,54,75,219,55,251,176,18,183,148,230,205,244,7,55,9,61,227,
  194,78,5,166,121,11,254,251,78,237,50,22,220,25,26,120,50,210,132,226,212,230,199,169,
  44,82,166,52,42,177,63,154,130,67,146,27,172,181,74,228,113,121,136,71,237,154,89,202,
  194,56,27,182,77,209,147,23,23,138,139,43,229,90,137,37,217,218,210,178,147,32,18,27,
  84,170,148,151,125,88,162,184,152,203,149,218,10,254,130,46,28,225,113,161,176,88,195,100,
  75,20,233,163,90,118,193,96,195,74,109,169,96,193,192,206,175,250,96,182,222,112,126,96,
  184,76,153,13,54,15,192,229,172,25,58,65,101,187,241,234,253,187,238,112,208,167,212,96,
  79,152,168,240,33,63,220,113,86,111,244,5,102,152,67,137,183,79,92,11,28,54,99,72,
  194,75,147,225,65,96,55,35,104,228,216,241,98,42,227,19,50,201,144,70,181,252,245,193,
  123,187,145,202,24,242,150,57,5,155,130,219,155,206,91,180,52,24,25,75,3,97,206,230,
  82,154,112,127,62,122,10,221,78,18,120,230,136,55,120,237,242,194,221,117,49,114,148,50,
  139,112,74,164,189,4,205,16,107,1,50,6,250,188,163,123,44,146,99,44,35,88,231,54,
  244,205,239,21,66,62,161,2,117,153,200,28,178,29,243,118,208,253,115,141,209,168,78,234,
  209,237,177,11,75,45,127,174,174,107,122,101,87,35,147,3,162,78,236,139,59,105,223,93,
  112,23,246,101,117,123,168,194,233,23,42,75,181,226,202,170,170,73,0,20,82,115,220,208,
  62,60,35,120,245,17,135,234,208,28,197,59,110,135,151,20,73,115,44,160,134,208,92,198,
  248,140,199,197,242,156,31,117,44,170,99,173,188,22,43,40,35,78,213,87,230,93,150,170,
  121,159,194,183,108,70,70,69,90,243,48,54,15,28,154,251,173,249,72,7,193,196,68,149,
  3,63,190,24,199,15,166,216,117,14,89,23,211,35,145,5,85,179,98,148,69,43,155,240,
  101,2,7,109,53,235,214,246,66,23,142,125,101,152,137,169,25,248,57,198,189,240,174,217,
  145,207,139,161,45,200,240,106,110,171,162,219,106,33,173,149,215,89,41,189,217,124,70,179,
  114,58,142,126,51,171,25,212,26,81,239,128,76,34,143,132,144,162,186,232,158,148,20,52,
  142,243,208,219,58,165,176,78,201,214,49,122,30,157,108,87,7,29,129,52,32,69,202,190,
  145,224,31,232,43,103,75,224,127,104,86,147,208,31,78,9,84,253,164,219,124,164,156,84,
  246,57,239,156,95,47,56,101,108,166,55,13,168,73,74,196,48,243,209,23,43,244,61,204,
  108,63,203,16,30,244,51,222,24,31,94,177,120,231,12,150,115,158,50,45,152,144,107,195,
  157,88,77,102,167,62,163,91,225,92,234,244,154,233,133,57,13,42,164,83,82,58,89,31,
  163,20,163,70,158,157,189,211,83,193,100,60,238,168,163,109,225,34,221,20,142,45,122,181,
  66,99,129,78,150,67,137,8,220,117,83,148,160,98,203,120,83,56,201,216,102,165,140,102,
  165,88,51,179,198,101,67,179,216,205,202,53,165,102,217,219,47,122,253,219,13,96,254,103,
  118,130,253,226,86,150,112,62,5,144,121,151,204,152,1,154,125,226,33,27,58,99,205,73,
  82,248,219,197,241,199,90,136,3,152,245,64,112,156,171,52,99,194,148,141,98,209,244,151,
  169,187,40,220,237,98,90,69,86,169,9,153,166,159,217,93,33,35,34,91,252,89,226,43,
  60,135,100,241,7,69,87,200,52,136,166,134,196,63,82,240,9,72,146,133,152,82,129,159,
  54,2,220,160,139,56,118,89,139,214,1,52,23,181,60,154,54,76,67,48,28,107,155,145,
  101,8,157,123,145,3,255,228,136,69,109,52,212,128,82,164,61,61,132,132,208,159,165,249,
  252,160,85,43,34,39,212,106,235,61,240,175,31,224,213,221,213,1,249,253,4,136,100,245,
  17,233,97,119,108,15,41,20,152,98,44,245,126,103,120,241,128,30,2,230,194,5,42,217,
  114,62,70,36,21,72,227,62,239,144,33,113,81,214,125,200,89,91,243,251,226,58,206,5,
  82,75,43,56,80,64,197,218,14,61,117,150,125,187,146,148,8,147,28,11,209,220,58,68,
  240,207,49,61,137,141,73,48,14,167,28,136,236,120,204,40,102,60,34,57,215,122,252,150,
  132,40,36,49,255,160,208,73,116,214,179,76,152,38,182,139,131,149,16,216,178,221,80,210,
  181,209,113,14,56,239,79,196,120,91,164,96,230,198,219,36,77,28,94,102,59,213,112,41,
  107,129,237,76,51,94,103,73,186,54,57,20,67,47,54,100,119,53,57,125,206,250,43,245,
  169,67,215,72,16,88,105,160,197,208,159,111,229,61,54,238,140,184,40,204,138,232,44,219,
  217,65,219,34,82,212,215,29,243,231,57,100,167,80,249,136,1,197,214,98,38,0,222,182,
  14,42,165,234,35,39,208,25,219,57,60,191,72,166,92,145,49,203,191,20,99,65,225,188,
  149,4,163,78,130,17,74,125,104,154,62,248,41,152,79,138,129,56,46,213,26,207,61,207,
  142,8,38,126,33,142,65,232,191,22,8,45,207,42,21,121,50,161,241,158,242,25,65,212,
  199,220,193,106,206,53,207,247,252,236,166,150,130,4,232,85,202,138,197,179,17,147,116,210,
  88,91,36,168,150,2,19,99,78,154,4,169,220,41,141,55,165,115,45,171,130,248,43,178,
  53,226,106,41,87,179,116,71,191,249,181,144,44,168,241,247,95,17,83,177,79,95,221,66,
  189,144,196,149,17,227,213,67,25,142,164,79,217,40,207,181,97,195,93,230,11,249,30,159,
  177,108,120,162,163,102,92,117,31,188,118,193,157,8,120,106,229,63,251,121,38,203,210,9,
  74,220,246,241,35,146,142,242,207,49,32,61,27,73,234,164,38,169,19,248,123,31,215,79,
  38,240,147,168,250,88,250,100,108,75,115,181,204,24,104,70,183,97,164,195,12,217,145,131,
  131,165,81,45,117,16,217,221,220,64,142,131,94,10,223,200,224,65,243,233,136,172,6,114,
  215,152,119,9,45,90,165,163,34,76,90,49,113,36,167,172,233,156,57,186,61,135,121,251,
  18,12,159,239,158,128,128,186,11,113,53,247,6,139,246,192,253,240,58,239,79,203,92,42,
  78,126,183,91,134,30,70,6,202,106,251,7,72,144,207,40,230,5,164,139,202,123,51,62,
  109,30,35,21,58,202,22,239,219,234,68,1,90,221,79,19,68,95,213,242,86,88,207,88,
  47,200,234,129,69,195,180,49,91,179,218,233,161,58,13,3,25,248,133,182,52,37,166,170,
  139,247,79,33,163,211,40,150,32,54,238,183,173,50,253,154,102,227,172,183,2,131,51,178,
  67,213,70,105,33,52,176,91,43,161,30,177,88,35,195,81,105,251,166,35,25,241,95,97,
  99,82,84,124,72,218,16,195,230,69,84,36,39,219,122,47,226,119,68,54,55,250,38,175,
  212,234,59,33,72,217,104,127,120,115,57,184,24,182,175,47,145,59,161,111,6,203,65,244,
  48,7,198,129,224,169,33,180,86,78,175,38,136,69,138,96,236,11,85,113,193,60,189,241,
  169,255,232,58,38,126,157,152,239,110,186,137,93,160,80,180,41,144,199,132,84,147,113,158,
  82,98,229,21,200,193,74,196,120,74,169,23,81,146,249,182,88,159,51,144,114,146,29,166,
  4,176,113,206,191,14,188,66,58,18,29,71,95,141,199,242,83,150,132,221,36,155,32,105,
  18,148,208,56,134,88,9,129,251,11,227,20,196,46,159,201,112,33,213,50,73,22,25,147,
  140,225,164,74,38,51,227,68,119,232,217,139,1,147,141,3,17,73,173,241,140,14,4,157,
  199,130,183,131,240,129,251,161,11,14,91,235,123,91,235,205,45,103,175,145,213,221,152,135,
  215,248,166,147,2,112,150,141,107,44,40,180,238,218,152,187,254,23,237,150,48,255,118,238,
  223,255,142,228,151,214,95,253,252,199,250,187,151,174,150,140,18,140,9,227,91,199,88,11,
  112,37,107,3,231,131,27,186,45,227,81,68,9,37,243,99,77,128,180,47,2,247,191,96,
  227,26,58,49,6,83,159,254,137,253,42,2,5,131,158,205,253,207,255,88,87,18,115,220,
  227,168,163,249,149,21,185,61,231,147,245,173,253,239,33,110,33,67,243,250,81,197,202,78,
  210,2,61,188,108,159,13,62,10,208,252,65,170,61,174,49,37,99,59,119,219,31,14,122,
  189,227,222,96,112,125,12,193,167,134,55,176,8,97,103,195,130,134,60,71,200,239,11,171,
  240,207,235,96,128,248,117,126,94,241,74,219,143,147,73,25,172,211,186,108,84,23,73,53,
  45,211,74,135,218,49,65,98,99,85,148,73,24,248,184,215,220,223,221,173,111,29,239,238,
  239,31,64,192,238,173,250,15,33,222,134,96,170,145,1,105,192,113,94,169,131,77,120,217,
  192,85,155,86,97,99,253,176,177,137,219,196,25,190,162,251,90,64,121,75,138,192,176,0,
  101,90,247,225,74,39,145,5,12,225,99,167,123,215,81,112,81,144,208,164,228,79,138,142,
  252,3,66,69,82,55,73,208,61,8,227,170,194,33,8,28,169,229,38,64,118,106,141,141,
  110,123,148,90,136,90,190,91,40,190,19,8,146,205,77,54,86,155,237,171,206,176,189,7,
  82,201,152,42,219,237,33,73,78,42,68,185,9,148,110,118,86,222,181,202,9,151,213,216,
  235,207,88,219,139,36,120,242,97,161,34,182,159,0,63,153,2,55,31,103,129,233,76,34,
  146,233,154,239,140,21,250,224,69,70,84,53,136,224,240,245,100,48,232,193,38,54,48,96,
  238,39,228,47,22,67,151,189,24,80,106,53,142,220,223,79,100,46,130,46,14,111,49,189,
  200,44,210,14,103,241,18,133,139,172,224,19,242,149,44,222,17,18,227,248,251,70,107,231,
  24,40,118,72,155,95,223,184,68,69,100,18,12,35,2,151,179,121,78,221,235,248,190,188,
  118,136,160,200,28,154,217,52,142,202,66,26,42,243,19,82,70,36,84,128,46,162,61,191,
  142,161,205,182,138,238,138,198,91,207,161,243,133,150,192,226,253,195,39,12,30,21,43,250,
  72,134,134,238,22,160,128,87,240,224,211,3,105,167,117,217,238,171,180,32,94,95,115,224,
  238,72,178,0,218,183,186,217,4,216,130,210,108,191,156,151,239,193,246,244,115,124,192,239,
  19,175,51,24,129,2,26,158,12,107,222,6,6,246,31,57,62,44,51,160,212,41,175,226,
  182,107,118,54,32,243,51,10,157,17,236,158,120,206,166,172,90,143,79,200,125,252,5,14,
  71,125,46,98,31,254,177,232,245,235,159,138,241,98,113,40,198,43,240,153,24,47,51,71,
  98,100,129,254,5,206,50,247,74,144,117,148,221,246,213,73,166,254,152,244,44,210,61,120,
  71,145,130,162,79,34,231,231,83,15,34,51,40,247,28,210,35,88,51,221,185,167,144,250,
  168,14,33,245,235,153,37,217,144,28,179,2,53,187,191,156,15,207,187,189,110,251,102,135,
  233,63,197,6,115,58,78,162,35,249,211,175,122,239,202,155,177,234,77,38,234,118,79,222,
  28,237,135,9,119,129,233,209,221,5,62,120,53,83,103,209,207,79,220,21,118,208,206,174,
  240,251,162,199,11,247,147,179,75,124,148,121,183,248,77,158,121,215,132,228,155,141,160,110,
  118,79,188,224,139,28,82,162,43,127,43,197,177,240,143,172,204,90,226,228,202,172,199,7,
  88,102,21,115,142,109,249,24,255,149,206,179,152,251,105,218,254,54,143,54,184,152,50,220,
  78,87,31,183,223,124,84,156,93,231,116,224,187,66,218,133,26,226,154,247,93,98,191,178,
  105,144,100,246,222,184,144,204,172,253,62,233,57,239,186,17,115,20,63,247,241,58,191,5,
  49,199,17,249,143,219,226,187,213,104,7,172,134,121,7,60,23,111,212,153,117,8,248,148,
  237,136,33,28,12,7,39,29,9,190,103,190,70,13,221,133,5,146,90,127,59,245,183,13,
  95,61,154,177,120,210,87,143,0,228,44,156,16,243,157,14,4,170,167,136,57,1,117,46,
  221,162,145,98,76,49,147,253,207,41,136,226,244,198,44,173,36,69,143,223,192,107,147,241,
  68,70,106,125,63,24,246,206,216,213,24,35,194,161,194,35,31,89,169,124,37,91,32,231,
  47,48,108,27,118,239,85,164,33,184,18,217,111,152,197,235,225,211,106,216,129,121,16,163,
  96,89,152,212,130,191,108,60,52,56,122,218,59,3,35,212,188,75,248,171,217,43,226,13,
  188,12,72,178,58,67,76,66,132,102,99,25,110,162,59,195,60,108,235,173,145,189,43,66,
  107,60,255,37,81,229,249,24,192,27,233,4,187,209,159,79,78,174,67,234,169,158,186,176,
  182,58,247,244,10,36,253,39,85,17,208,80,95,107,143,238,196,227,150,46,119,246,184,3,
  79,37,8,53,21,101,134,73,119,30,66,88,222,182,115,182,121,125,239,157,17,39,76,1,
  127,100,157,239,230,209,70,253,248,232,29,111,75,217,90,63,224,120,42,227,221,245,183,27,
  245,102,203,251,122,176,179,191,247,134,85,28,46,43,227,133,3,246,26,233,41,218,93,223,
  197,140,85,146,58,158,148,229,147,22,201,85,115,220,166,195,79,1,71,155,160,168,25,224,
  58,222,91,76,188,56,197,3,161,166,165,223,113,66,105,196,240,122,18,54,105,134,161,161,
  27,74,26,189,132,75,136,64,38,70,53,172,242,187,146,206,208,110,12,142,79,199,204,93,
  165,51,238,52,241,131,176,253,148,4,152,60,206,217,69,236,34,1,125,117,188,225,114,51,
  216,127,19,12,62,85,248,25,99,110,172,89,108,111,98,22,59,57,59,113,121,159,195,91,
  4,161,194,142,242,142,97,251,239,37,17,78,113,182,69,62,231,133,145,190,51,210,36,102,
  139,255,251,142,233,67,114,202,179,155,46,237,232,44,90,156,36,65,229,210,42,111,177,48,
  48,58,132,216,126,186,134,21,90,48,89,97,252,123,251,62,250,189,217,25,13,122,183,142,
  33,83,134,88,66,53,62,26,177,196,254,52,217,175,181,107,19,152,55,224,11,103,208,7,
  151,94,0,220,102,187,127,209,49,117,0,63,144,210,196,24,12,24,248,117,59,218,239,119,
  232,10,163,94,223,117,49,213,63,188,110,159,178,109,171,5,251,82,182,52,207,95,68,66,
  150,25,245,32,224,157,66,14,96,206,5,73,193,13,117,243,219,187,143,54,43,79,94,64,
  19,120,83,32,68,131,134,155,244,103,134,1,224,127,231,156,129,17,178,28,174,141,255,54,
  126,150,253,79,54,148,3,13,127,241,147,41,187,110,159,129,117,194,33,24,143,16,169,161,
  234,60,6,96,180,17,229,110,122,237,209,86,231,26,98,188,64,241,18,217,194,201,54,166,
  226,237,221,79,27,109,96,37,220,9,160,7,65,149,231,24,158,33,53,60,38,22,172,136,
  150,55,107,80,191,124,96,171,123,80,33,228,29,104,225,255,144,60,2,49,227,23,10,240,
  139,211,193,159,87,16,29,50,100,118,80,154,182,131,82,64,179,236,14,202,211,118,80,158,
  178,131,202,180,29,84,166,236,160,58,109,7,213,41,59,168,77,219,65,109,154,14,224,250,
  130,171,116,84,80,183,57,254,84,52,87,24,40,0,125,243,168,168,159,211,184,188,100,203,
  33,100,16,86,40,169,156,156,92,142,211,10,159,30,62,125,52,159,112,34,70,101,167,151,
  170,133,82,166,94,42,110,47,53,91,94,225,94,170,78,47,75,8,178,234,244,178,140,159,
  106,170,23,27,133,92,240,43,12,15,243,224,125,2,2,126,242,62,125,18,236,148,67,225,
  158,22,136,183,45,47,215,74,165,37,178,111,166,2,116,199,42,210,203,38,151,87,139,197,
  90,173,162,2,142,139,58,165,88,157,79,110,157,114,172,206,189,91,167,34,234,84,74,43,
  133,10,54,186,15,251,171,198,234,61,132,125,214,200,88,171,176,184,84,41,99,64,91,40,
  167,255,199,48,151,165,202,210,82,97,89,69,176,52,13,150,210,16,240,0,47,187,53,84,
  86,54,0,252,192,17,220,189,8,245,220,50,145,55,103,25,209,244,67,114,233,139,118,36,
  2,236,191,89,111,194,235,248,219,198,230,241,86,253,0,158,201,55,142,182,183,235,77,20,
  111,46,122,199,219,160,58,225,5,15,43,169,49,58,0,245,195,53,9,186,29,109,31,11,
  106,99,174,198,143,198,159,64,213,218,27,92,128,21,223,157,109,139,138,15,248,72,63,54,
  110,207,183,55,245,33,167,81,187,157,22,53,45,85,168,208,253,18,54,174,187,246,240,1,
  143,82,21,174,222,32,18,22,57,67,178,248,156,121,114,82,10,38,207,208,211,249,196,61,
  201,169,96,19,66,32,188,22,28,201,166,192,159,32,238,26,173,190,197,103,173,56,2,70,
  248,47,86,152,107,117,145,20,20,175,93,180,72,131,194,220,148,36,144,179,45,105,109,235,
  168,81,174,72,131,98,149,39,10,202,86,125,115,127,171,126,252,14,236,183,247,143,91,245,
  31,90,71,77,116,175,139,131,27,53,223,108,180,72,249,213,25,214,247,91,219,249,104,183,
  242,150,229,88,255,162,47,70,88,223,14,238,42,50,184,48,125,236,21,218,37,233,38,23,
  177,5,202,74,12,121,231,161,107,247,238,193,206,58,21,69,84,57,100,241,119,168,117,53,
  34,35,198,209,157,159,214,152,190,227,177,212,210,106,64,84,31,210,230,166,137,7,237,170,
  178,211,72,148,159,139,251,249,129,237,249,81,203,152,149,154,154,70,239,147,207,159,79,14,
  151,23,201,107,173,198,234,66,88,108,123,48,40,104,248,91,13,104,241,194,210,245,56,70,
  87,135,72,122,27,17,77,136,76,138,74,143,152,14,51,203,20,18,77,80,118,44,129,50,
  151,71,156,34,97,19,77,6,59,252,123,24,190,151,17,216,132,224,23,57,130,37,54,245,
  214,250,174,201,98,206,17,225,80,247,248,214,152,161,187,179,167,63,179,98,243,173,248,169,
  246,167,223,49,207,156,0,136,9,183,53,190,119,222,206,113,144,137,208,72,116,103,129,60,
  184,64,26,123,168,165,220,108,236,189,57,126,187,223,60,216,49,57,146,7,195,235,203,86,
  123,136,170,143,254,121,239,22,45,216,65,213,79,117,90,235,205,55,117,88,34,155,251,71,
  123,45,122,83,10,218,160,44,103,218,229,212,160,182,59,55,167,144,222,133,171,49,149,224,
  154,206,86,231,112,105,130,13,212,232,115,166,219,198,22,165,117,65,107,243,213,148,215,176,
  16,19,231,73,35,142,127,23,131,98,142,71,166,139,23,161,12,132,236,169,126,24,229,76,
  136,218,230,254,238,126,147,222,133,238,204,198,78,35,208,35,199,24,116,76,125,242,38,80,
  154,100,242,105,137,79,37,62,205,254,77,123,217,40,28,89,215,245,22,27,16,159,123,7,
  65,46,58,247,141,45,160,76,146,227,228,47,233,208,86,141,13,99,136,212,151,65,71,169,
  252,199,161,228,134,244,251,144,188,245,246,17,82,121,111,191,249,118,125,23,76,35,7,39,
  120,48,107,229,192,179,207,217,196,36,112,240,200,36,68,145,223,178,198,17,194,142,255,36,
  50,126,133,49,49,246,126,58,115,240,68,191,12,226,164,215,156,128,159,232,57,11,152,218,
  58,168,166,30,36,176,145,218,187,171,182,50,109,226,88,29,180,141,208,247,82,75,78,227,
  138,136,19,121,199,132,5,10,220,39,110,137,51,128,176,120,112,126,62,234,232,248,203,248,
  129,152,12,213,164,107,160,1,11,211,228,12,90,73,99,199,135,45,244,174,3,198,195,144,
  86,21,152,7,205,60,185,241,203,180,113,45,222,235,22,247,110,11,190,176,100,183,98,199,
  113,172,114,132,49,247,186,44,231,224,77,51,9,198,237,100,167,12,57,169,211,69,162,129,
  18,235,116,214,96,61,107,13,154,167,87,244,197,249,227,182,160,68,35,115,7,22,166,221,
  129,45,35,121,156,183,79,59,198,120,6,21,154,120,157,3,63,247,109,86,71,254,131,132,
  203,87,90,7,170,40,182,189,187,206,70,75,245,45,157,140,243,140,178,149,109,159,221,71,
  220,70,117,149,7,174,242,144,90,165,239,186,178,114,206,34,78,43,5,29,36,4,66,93,
  84,72,8,76,109,116,39,31,173,213,21,100,255,104,99,23,150,121,131,144,158,233,27,6,
  235,80,96,117,236,243,37,51,106,122,124,93,223,123,83,7,81,245,96,125,179,30,17,61,
  205,171,142,109,18,19,80,205,51,157,247,222,169,160,235,40,213,55,39,125,29,183,215,25,
  104,11,181,190,176,252,64,58,144,159,55,186,55,126,129,243,80,41,96,194,82,81,64,224,
  142,121,213,201,251,238,198,186,125,18,59,241,197,208,232,9,143,233,78,242,101,202,105,28,
  163,10,53,245,158,52,4,12,245,60,168,126,170,231,187,200,155,168,55,197,48,188,159,11,
  239,99,83,76,69,197,247,19,205,126,240,72,103,17,31,51,95,165,47,49,97,165,105,102,
  44,23,37,106,110,34,210,149,50,104,87,154,136,120,106,119,246,65,253,0,204,24,34,44,
  248,158,234,31,146,143,46,23,182,123,107,127,227,127,225,81,155,182,214,139,25,179,195,197,
  213,166,175,199,147,228,238,156,177,41,70,88,162,59,49,191,38,24,174,213,56,240,247,63,
  58,215,107,191,249,49,52,241,248,12,101,187,136,14,127,38,198,145,52,190,164,108,153,19,
  51,28,217,35,105,44,70,71,88,105,95,239,61,137,24,222,125,123,125,243,159,96,158,234,
  110,71,213,7,31,196,248,131,212,228,156,184,209,205,151,181,160,19,199,170,58,234,15,246,
  74,113,150,154,46,153,211,132,1,31,217,30,30,162,49,122,33,111,154,227,161,198,137,180,
  113,244,246,64,97,107,218,95,243,74,99,18,172,15,79,50,118,198,217,206,217,253,195,241,
  249,199,179,60,236,55,247,16,20,138,176,117,94,159,253,240,208,179,122,137,178,166,249,106,
  10,63,112,235,170,157,187,26,20,24,14,16,62,203,111,126,29,104,52,83,209,216,139,205,
  160,149,94,194,211,216,67,77,51,197,52,0,134,105,18,190,150,87,198,36,133,59,99,12,
  103,90,97,168,119,254,27,35,206,139,108,0,14,75,112,1,47,184,164,136,91,55,125,72,
  14,82,56,88,76,33,98,182,169,184,44,208,93,193,217,14,225,209,147,194,22,29,117,150,
  228,47,142,21,195,223,244,182,201,229,37,213,61,79,227,220,196,194,77,238,177,210,13,238,
  84,66,53,56,190,104,225,117,30,58,199,215,230,137,127,116,59,60,63,198,49,113,84,0,
  210,28,218,248,23,191,22,140,176,169,154,17,163,179,214,122,191,22,141,168,25,171,80,202,
  141,110,44,136,219,187,197,209,141,83,102,91,187,101,144,40,130,220,88,20,110,162,59,224,
  60,215,54,55,235,175,69,74,105,106,203,11,110,57,228,72,253,181,32,202,91,80,164,96,
  204,33,106,20,97,64,53,194,15,69,190,39,113,158,65,191,238,131,95,247,65,196,17,238,
  220,232,12,140,196,165,33,200,66,11,5,12,250,1,70,89,27,218,117,80,133,80,194,21,
  72,122,103,106,184,102,82,248,22,232,18,224,164,239,197,26,242,42,166,18,186,34,14,8,
  5,97,155,63,247,188,203,87,195,221,52,194,254,201,181,8,21,108,197,151,34,44,143,222,
  77,1,38,247,162,11,246,109,112,128,158,6,98,82,170,236,20,57,82,29,192,234,132,243,
  32,234,163,46,54,52,121,230,225,51,167,139,167,56,141,182,82,70,154,194,110,36,156,213,
  73,37,218,12,48,123,105,236,43,62,214,172,14,155,251,71,111,118,50,84,199,161,181,154,
  163,67,222,243,116,200,54,46,80,10,60,17,6,37,133,24,2,198,113,107,167,177,249,207,
  12,236,98,177,104,28,252,142,44,126,251,7,235,255,119,84,127,225,61,144,172,169,235,116,
  128,70,171,185,190,119,248,182,113,120,136,246,156,225,171,138,49,231,163,179,17,236,224,49,
  132,212,58,39,101,85,192,204,123,108,79,167,242,5,43,117,136,165,122,1,111,67,23,198,
  141,201,1,60,11,146,248,62,160,204,6,78,237,211,95,120,30,91,3,120,136,139,167,83,
  116,194,100,5,82,174,74,215,198,105,199,171,38,81,227,109,31,97,227,227,222,128,59,8,
  2,220,130,54,85,2,230,103,114,248,74,123,78,237,182,207,47,100,102,250,3,128,120,116,
  173,249,86,169,90,91,4,21,21,164,151,92,92,149,181,142,168,231,173,193,199,190,173,89,
  229,154,53,183,230,225,101,247,252,166,137,84,90,166,73,210,117,156,78,193,16,31,51,88,
  154,242,170,41,39,213,30,226,196,207,41,35,51,1,20,107,7,33,233,127,144,133,68,126,
  154,127,103,5,200,146,26,128,2,170,172,71,224,157,192,27,87,162,130,198,8,4,22,47,
  28,72,101,23,82,217,136,248,62,133,92,32,195,139,24,232,19,7,116,197,5,93,49,67,
  31,7,250,36,6,187,173,194,141,85,104,61,210,195,55,45,199,245,32,226,155,18,12,48,
  7,250,93,238,181,58,178,236,177,196,56,20,22,19,241,255,38,105,250,93,238,59,126,77,
  15,26,20,23,19,241,255,34,15,193,221,237,185,181,119,59,199,195,117,112,70,144,230,92,
  252,19,172,41,218,157,136,186,80,48,39,87,154,95,247,98,162,186,14,190,92,143,23,102,
  130,0,230,228,214,72,176,123,239,75,251,92,228,80,117,137,252,8,26,151,243,83,208,183,
  156,31,79,219,147,116,218,158,60,149,94,229,137,232,165,169,83,114,169,243,8,226,148,242,
  19,18,166,148,207,38,202,133,75,20,228,20,177,241,149,34,227,19,249,177,13,31,94,111,
  13,104,88,46,31,174,232,17,41,128,36,43,66,138,55,111,123,199,0,70,225,149,39,129,
  87,142,193,139,130,43,121,224,238,22,49,214,132,203,32,23,41,205,245,226,69,88,112,161,
  38,149,249,74,9,19,103,4,124,69,118,66,85,237,57,122,135,230,22,231,195,54,250,141,
  194,223,52,7,85,206,55,119,135,38,23,166,232,193,22,133,155,117,184,136,207,34,67,168,
  196,167,7,28,2,67,108,61,92,252,68,223,63,202,239,31,197,66,148,51,71,184,103,207,
  156,90,10,42,59,218,157,237,143,241,253,164,62,127,180,159,229,44,160,47,217,79,173,129,
  12,25,233,79,135,168,24,132,212,236,83,224,36,239,227,121,123,232,96,152,231,198,128,8,
  86,87,57,221,233,207,5,174,107,209,25,248,120,180,40,9,202,79,97,8,80,44,156,18,
  29,199,169,230,93,189,121,8,10,49,215,0,203,238,83,130,79,22,121,231,132,167,194,156,
  16,22,10,219,160,182,28,150,106,102,85,44,30,209,133,197,214,23,160,185,194,101,94,17,
  127,150,84,153,154,250,222,176,230,116,37,59,19,215,30,114,207,57,17,147,76,133,29,8,
  225,232,34,239,80,121,78,145,95,143,38,156,158,40,12,143,0,26,198,121,219,177,58,131,
  168,173,219,86,206,63,104,214,223,30,237,182,26,7,187,13,64,152,76,32,140,9,165,13,
  130,51,183,150,115,190,181,237,157,97,131,5,240,74,238,234,78,132,126,101,134,33,52,97,
  198,182,95,208,9,216,193,230,14,60,146,131,126,69,182,61,105,195,187,40,6,212,214,74,
  103,91,26,189,1,153,199,118,15,78,87,217,160,100,193,113,26,192,9,213,233,89,55,85,
  175,133,176,91,100,95,12,215,16,209,169,252,33,249,100,73,188,5,150,145,245,38,225,23,
  16,22,195,35,220,92,170,80,235,65,169,184,67,255,18,133,167,2,127,27,8,42,104,55,
  7,137,254,205,232,71,208,205,226,88,100,37,134,105,57,203,187,22,176,58,108,146,128,120,
  60,66,113,228,24,216,182,240,108,45,85,45,147,95,112,127,202,95,12,44,2,135,252,213,
  248,130,228,151,2,192,180,2,119,0,82,125,195,131,157,15,218,56,122,155,119,230,217,216,
  56,213,25,131,53,145,197,88,110,97,231,162,111,13,214,154,34,17,178,208,195,12,197,117,
  31,142,40,121,251,87,239,86,126,199,218,96,205,84,149,246,133,111,92,61,193,56,173,195,
  48,170,108,216,81,38,86,94,40,169,205,253,253,230,150,206,4,110,53,232,149,48,166,82,
  188,225,123,71,75,27,11,55,70,160,67,44,221,104,94,177,134,239,99,219,219,4,38,225,
  39,160,140,40,70,54,40,155,41,223,92,63,220,4,85,255,97,174,20,248,24,71,2,84,
  162,46,61,192,219,198,68,18,33,69,226,88,188,119,220,66,35,20,152,28,134,122,44,22,
  170,242,138,108,206,222,249,113,8,115,177,241,191,151,150,69,108,18,173,161,109,182,225,238,
  112,214,153,30,156,183,122,52,60,237,139,111,179,152,59,239,52,110,61,155,169,120,116,51,
  188,133,48,63,94,40,42,138,178,160,227,228,59,65,88,86,253,18,12,187,18,124,84,49,
  8,98,69,28,131,197,38,73,118,66,193,80,20,6,73,179,204,16,89,163,172,217,148,14,
  242,25,209,135,158,182,60,227,145,111,226,221,141,91,166,143,128,165,150,171,187,34,210,98,
  219,100,64,82,139,32,37,18,211,87,179,24,166,138,20,53,202,26,177,191,56,210,130,133,
  62,145,121,185,193,251,82,58,26,203,190,166,129,162,86,132,222,213,94,204,194,175,103,91,
  103,7,97,29,101,14,211,159,188,212,72,210,143,154,189,205,219,147,142,12,60,237,145,62,
  214,89,204,148,56,30,104,57,165,191,105,123,138,238,251,48,92,113,38,20,181,68,130,104,
  219,127,228,26,113,106,219,128,202,41,69,219,92,226,44,172,177,193,195,71,217,100,137,134,
  85,73,89,57,140,17,128,239,12,225,253,233,174,115,246,6,253,208,225,157,122,111,208,29,
  241,59,117,41,103,165,103,166,172,146,159,149,230,167,90,90,92,89,46,173,20,151,87,40,
  73,60,125,35,133,151,205,0,175,21,130,133,218,82,177,88,168,214,200,101,187,80,93,46,
  47,21,171,194,106,134,130,23,177,42,106,112,129,89,1,70,191,28,210,2,99,115,83,94,
  108,202,18,216,126,24,109,14,110,251,240,194,163,110,234,151,93,141,166,124,191,184,24,244,
  32,106,252,58,230,13,194,167,144,197,242,202,202,74,173,92,42,173,44,149,150,107,213,178,
  152,29,114,199,162,231,222,188,118,39,19,29,3,108,118,33,199,139,179,83,204,104,152,184,
  180,12,12,46,22,55,109,235,151,230,2,154,115,176,154,71,212,87,5,121,153,104,144,118,
  40,175,192,204,82,126,42,243,139,32,12,21,217,166,159,115,25,160,213,231,249,122,77,43,
  75,4,47,210,93,116,99,185,159,57,142,93,184,111,18,37,64,218,29,174,39,75,214,213,
  111,133,24,222,215,214,228,0,195,107,178,177,10,44,44,191,124,66,179,97,111,107,171,0,
  220,219,195,219,209,205,237,85,206,5,113,143,122,105,84,96,131,109,136,251,253,53,235,171,
  189,239,15,41,245,31,84,125,219,227,57,247,215,234,140,110,72,165,160,251,247,26,126,146,
  13,201,80,90,54,84,228,225,109,65,215,62,156,4,243,28,198,145,183,94,70,216,144,94,
  206,68,119,51,102,158,6,52,14,210,160,216,206,66,87,199,237,179,150,193,15,130,139,63,
  190,34,52,74,12,194,204,94,158,126,206,24,31,116,119,65,149,243,46,161,209,154,35,216,
  240,160,20,0,45,62,239,230,57,53,138,196,163,27,110,129,249,167,119,85,252,253,186,42,
  253,126,93,149,127,191,174,42,147,117,69,29,241,59,57,47,152,207,130,219,145,114,135,158,
  140,71,42,98,173,199,96,204,9,145,45,164,188,59,124,59,134,201,253,233,216,219,4,234,
  97,159,7,46,4,60,208,8,231,99,185,165,17,205,254,58,124,243,172,59,178,89,12,221,
  56,241,118,45,120,235,92,101,221,49,115,119,213,105,247,73,231,106,65,185,172,19,36,89,
  29,89,203,169,244,64,106,73,249,129,91,77,52,173,10,246,101,123,120,118,108,86,207,232,
  166,115,157,39,132,130,93,166,64,155,201,206,104,239,180,76,120,124,166,189,94,2,51,244,
  31,162,174,3,67,61,229,42,34,207,4,11,91,166,8,155,33,41,223,68,29,67,203,60,
  253,155,5,65,252,95,81,247,173,205,248,188,229,134,207,132,136,162,83,233,250,24,160,225,
  89,168,193,227,99,134,249,1,154,93,36,189,1,172,107,155,224,66,252,1,227,86,148,233,
  16,45,44,214,170,94,52,33,103,112,132,186,32,67,162,32,168,138,159,159,196,211,104,194,
  190,113,173,111,92,75,236,1,21,252,98,82,126,53,13,95,241,152,129,122,184,28,199,77,
  178,89,8,227,27,240,144,199,110,9,79,189,149,162,185,183,91,198,196,195,87,203,111,188,
  218,100,38,253,230,227,173,190,200,54,116,87,93,166,146,55,225,244,39,118,127,194,5,16,
  253,185,120,182,197,238,115,20,224,214,4,35,174,32,167,189,55,31,87,139,163,33,178,131,
  194,162,175,217,144,60,206,62,176,155,208,55,41,93,234,76,46,144,159,148,117,255,251,236,
  132,187,38,135,22,85,252,175,102,48,20,215,165,52,242,104,0,134,144,232,79,250,157,242,
  55,93,88,144,231,123,69,227,194,249,22,188,167,9,23,207,121,237,162,174,54,159,29,51,
  192,86,85,129,25,192,166,181,37,175,77,193,131,61,241,84,146,86,46,48,1,235,45,231,
  230,90,51,238,114,153,241,51,68,120,65,228,179,234,88,166,158,85,139,102,54,171,130,98,
  249,182,138,251,46,20,161,22,101,12,14,86,2,3,8,143,72,220,223,14,85,196,78,191,
  26,12,110,46,153,115,104,138,2,215,48,196,77,4,205,103,83,15,213,145,206,161,16,99,
  18,169,106,212,9,216,129,97,36,78,82,183,80,163,250,123,30,196,110,45,153,158,45,170,
  74,116,94,238,253,93,102,99,58,131,177,141,101,49,222,97,110,106,158,156,65,20,76,199,
  86,223,111,107,31,253,33,115,211,187,14,166,226,164,20,78,209,122,150,197,96,216,199,206,
  79,90,156,162,255,112,123,52,52,83,127,61,160,178,75,253,173,206,33,179,93,85,251,133,
  96,240,175,205,73,157,90,13,105,167,79,116,181,149,39,58,46,213,121,124,77,142,34,1,
  192,57,239,227,54,25,211,8,36,102,149,157,141,248,20,111,19,128,54,251,0,250,206,18,
  143,82,17,220,86,248,101,147,228,121,16,204,146,202,148,62,84,40,176,28,133,148,167,193,
  82,55,43,189,184,56,155,51,45,46,90,150,179,182,212,122,175,113,61,148,240,244,159,159,
  108,182,103,37,211,39,106,230,95,217,92,98,74,224,47,152,195,48,203,209,14,123,79,76,
  159,179,18,145,19,225,72,23,175,45,78,192,167,168,217,152,233,16,91,66,135,174,44,205,
  153,95,191,24,171,95,76,175,95,138,213,47,165,215,47,199,234,151,211,235,87,98,245,43,
  94,125,79,171,152,162,168,170,168,165,129,246,157,154,226,115,154,76,100,14,106,39,200,126,
  103,202,154,85,153,224,14,178,74,177,71,116,85,76,233,170,248,252,93,149,82,186,42,61,
  127,87,229,148,174,202,207,223,85,37,165,171,74,90,87,47,92,165,226,231,23,83,222,53,
  198,106,20,205,179,231,88,1,225,63,69,52,248,51,31,247,127,248,65,25,61,39,167,145,
  184,226,202,1,92,127,206,62,227,131,82,41,5,38,18,114,206,68,172,205,5,134,239,29,
  226,190,210,224,218,185,243,63,110,235,69,93,204,191,31,111,128,232,218,183,89,3,65,97,
  223,22,51,64,124,186,225,226,35,172,11,159,213,220,109,82,163,48,135,64,17,195,44,75,
  168,20,139,170,111,38,89,95,204,36,235,155,109,83,184,68,29,251,33,187,56,83,141,127,
  190,89,14,125,41,203,33,205,133,127,80,92,56,63,38,223,99,42,23,194,224,24,169,124,
  81,23,166,44,15,58,106,161,74,62,253,16,16,225,195,118,214,15,149,75,191,22,82,70,
  129,74,116,178,252,98,65,12,151,32,197,152,147,249,40,214,141,155,241,75,111,135,52,53,
  239,106,106,220,53,65,232,9,206,152,248,177,98,148,200,31,125,213,113,130,47,106,198,239,
  68,203,93,113,109,242,90,56,206,105,204,147,31,147,56,47,0,230,166,207,139,235,203,157,
  65,162,110,156,134,62,138,104,199,179,82,139,70,116,229,90,57,144,153,117,115,45,253,152,
  77,213,163,78,147,179,111,156,165,232,99,200,28,1,247,188,132,142,176,160,49,36,14,89,
  57,19,55,114,64,60,153,172,233,254,40,82,214,124,84,242,106,31,148,155,195,122,60,85,
  117,174,221,108,238,59,117,14,248,88,215,252,146,180,22,157,191,104,134,248,197,216,105,103,
  109,122,99,41,214,215,66,33,61,107,246,198,102,41,253,144,252,106,92,179,204,179,203,219,
  246,232,151,60,81,56,122,209,125,148,244,158,242,178,183,250,196,156,230,209,221,22,73,109,
  238,231,60,86,67,154,91,243,242,25,79,145,19,124,92,54,240,12,218,79,202,243,39,148,
  169,87,159,152,2,59,74,196,9,50,97,79,71,212,233,83,68,143,75,14,61,62,45,116,
  102,66,232,199,165,130,158,102,90,211,110,46,169,23,131,213,39,115,201,232,126,208,224,37,
  251,120,212,228,121,30,40,188,31,52,68,127,186,252,2,49,79,126,17,79,144,255,213,204,
  76,140,19,62,110,70,210,196,100,25,210,46,254,180,233,132,168,139,43,55,241,100,200,184,
  60,60,114,114,199,201,22,182,3,125,186,79,63,191,142,46,214,119,65,161,89,182,128,253,
  105,14,74,196,60,7,101,60,209,193,103,51,211,81,169,37,210,64,106,106,83,10,89,93,
  59,213,34,209,185,55,221,55,114,240,215,255,209,245,7,62,252,103,99,111,143,253,221,241,
  206,125,50,232,163,143,251,15,108,186,176,193,191,232,26,52,250,165,219,39,99,14,149,63,
  75,86,255,49,171,250,67,80,253,167,172,234,159,130,234,223,103,85,255,40,29,219,255,59,
  109,108,142,94,225,164,219,63,211,65,34,211,74,212,61,80,84,112,242,65,130,221,15,34,
  103,227,222,83,115,15,73,47,190,67,87,4,166,31,241,139,231,141,141,2,159,151,0,85,
  52,115,29,77,254,95,252,52,152,207,177,61,117,69,70,153,255,87,238,255,17,52,25,171,
  254,95,248,122,170,190,177,50,164,232,165,255,112,250,82,9,55,40,169,217,172,17,187,185,
  97,105,146,134,156,169,35,108,92,158,176,113,41,214,184,50,97,227,178,219,216,70,98,68,
  59,41,64,235,14,160,223,65,165,187,138,23,128,241,167,180,197,194,247,114,88,97,28,95,
  30,163,72,152,117,193,9,136,82,194,81,216,150,24,35,116,205,196,221,226,66,253,29,196,
  106,179,203,230,100,63,252,227,251,14,109,255,251,104,139,31,51,90,60,68,91,252,148,209,
  226,83,180,197,247,25,45,208,204,70,70,228,199,55,147,96,211,168,38,8,85,37,120,215,
  36,255,103,38,239,193,70,138,202,107,106,2,37,237,84,17,222,74,4,153,160,51,77,205,
  172,122,15,182,222,143,89,245,62,217,122,63,101,213,251,104,235,125,239,214,91,75,37,136,
  89,65,14,3,114,114,140,232,53,227,84,87,225,11,69,61,109,181,160,200,27,15,40,204,
  13,108,84,225,49,144,85,197,0,180,171,1,252,95,115,197,26,169,116,191,135,55,195,78,
  255,2,95,134,228,204,30,212,55,143,118,215,155,94,196,11,147,57,216,53,97,85,95,249,
  153,244,208,254,82,225,46,252,158,52,99,208,53,23,135,38,25,66,164,170,8,50,249,33,
  249,151,183,0,37,154,49,39,101,131,138,5,241,127,126,182,161,214,254,30,165,200,58,104,
  112,178,231,72,92,150,27,90,41,215,215,99,35,179,220,92,216,232,210,58,196,248,11,29,
  138,194,198,28,71,103,63,101,180,222,150,246,233,90,123,225,166,125,17,189,215,239,193,7,
  147,78,44,210,85,238,118,49,192,79,75,98,231,71,125,209,2,132,233,59,2,13,227,181,
  234,124,116,42,152,94,179,211,237,163,97,124,38,236,83,147,108,45,134,97,216,53,215,167,
  215,86,109,172,67,238,151,26,164,237,126,19,6,54,232,63,165,243,83,21,59,148,189,19,
  172,254,54,81,48,22,200,81,161,34,3,155,93,163,196,169,177,196,71,224,218,98,201,144,
  70,59,137,242,107,113,90,157,226,226,146,114,40,45,212,40,64,25,117,92,90,44,57,163,
  107,54,91,235,253,179,253,173,214,118,247,38,239,132,151,227,215,120,220,236,252,116,205,160,
  74,149,234,210,114,141,34,110,177,119,197,74,161,90,94,210,9,71,78,76,245,2,248,237,
  150,151,74,232,182,203,45,43,229,210,74,181,88,80,40,149,202,203,133,229,162,29,111,27,
  134,114,98,144,130,24,217,135,219,221,222,85,247,116,12,217,81,40,162,24,216,216,162,209,
  191,190,69,245,143,78,19,161,180,232,136,75,117,101,169,184,66,171,123,169,86,96,131,172,
  210,114,5,167,192,214,41,87,43,213,101,44,90,41,44,151,43,248,71,177,92,94,6,231,
  141,68,0,42,84,150,75,101,106,95,172,214,200,213,24,70,185,180,132,123,117,118,53,192,
  104,255,246,38,5,37,152,157,90,161,178,84,165,96,69,197,66,169,176,76,127,21,10,229,
  210,146,192,10,62,85,203,69,44,132,6,240,111,177,204,213,150,74,75,53,137,24,125,43,
  215,150,20,144,90,161,74,45,112,180,37,139,91,250,34,101,255,20,177,84,29,122,170,53,
  37,138,221,85,99,54,172,211,220,14,222,182,79,217,134,179,54,12,45,209,110,183,177,7,
  145,141,193,154,96,179,4,164,57,110,237,31,171,47,135,42,88,19,211,82,239,220,26,141,
  23,23,100,177,84,169,241,95,133,226,114,137,40,164,8,132,139,96,121,169,134,204,173,8,
  43,81,85,47,20,106,94,37,32,237,178,130,0,203,128,170,23,151,113,126,77,108,86,137,
  33,226,35,208,83,8,251,24,2,105,75,75,180,162,10,181,149,34,175,30,200,165,107,59,
  134,197,87,90,161,101,181,82,92,97,47,161,229,229,130,83,1,178,214,170,117,87,164,127,
  151,87,170,53,198,138,247,233,197,253,86,231,188,13,41,107,55,33,217,208,176,61,186,89,
  191,134,80,96,138,223,228,238,197,150,190,71,57,252,94,101,242,229,47,40,31,223,35,239,
  184,47,217,73,2,14,82,93,196,12,188,247,148,246,23,35,21,45,228,42,64,180,138,249,
  6,159,64,104,46,46,174,212,232,19,213,168,45,46,215,150,169,129,169,65,105,119,151,53,
  16,252,93,44,174,20,85,49,147,186,84,46,217,221,127,241,195,196,219,254,226,7,200,232,
  215,185,145,162,158,179,241,151,97,151,150,32,16,64,185,92,172,174,44,151,121,87,47,149,
  33,158,0,240,38,64,170,178,68,159,96,142,33,204,64,177,4,142,251,43,85,111,199,3,
  215,130,146,10,176,185,229,98,117,153,56,196,82,13,62,20,87,86,96,163,149,170,106,70,
  151,106,75,43,43,149,226,114,173,80,46,174,20,34,108,163,90,172,21,106,69,224,49,85,
  133,71,161,88,41,67,205,90,165,182,84,173,149,104,82,97,114,129,20,181,229,242,74,109,
  185,186,18,227,42,23,63,192,190,74,31,49,44,87,24,46,134,63,40,46,23,43,21,0,
  165,86,122,5,87,253,82,173,92,129,49,151,17,129,200,215,146,203,83,136,48,48,68,200,
  251,92,46,172,192,38,43,151,120,67,192,9,80,42,47,21,74,165,34,208,182,148,68,106,
  150,86,42,62,40,92,242,43,229,149,242,242,82,113,9,248,107,117,169,156,196,190,151,170,
  75,216,7,80,107,5,40,83,168,85,87,170,5,160,189,75,10,150,69,214,47,238,223,118,
  251,245,59,74,46,81,44,45,86,150,0,204,106,80,165,125,79,85,32,145,122,9,48,92,
  89,157,248,184,206,222,227,33,83,244,22,99,88,129,206,127,250,1,67,236,44,224,97,184,
  106,10,57,205,115,200,74,243,70,72,48,227,229,115,223,140,77,150,136,134,74,168,83,253,
  57,142,135,186,74,58,215,136,176,116,111,229,133,195,35,153,37,77,198,9,5,16,159,206,
  41,220,62,236,39,99,96,50,248,158,225,39,123,157,91,24,91,111,2,158,162,98,150,163,
  230,113,115,112,117,13,57,192,71,252,118,6,91,147,23,107,197,91,131,91,29,117,150,233,
  122,197,234,248,5,102,19,193,95,97,200,141,83,150,222,19,249,235,66,209,13,131,130,139,
  224,197,3,237,16,5,46,133,116,74,128,158,18,157,76,107,139,148,43,157,83,154,191,178,
  136,210,194,89,179,41,8,117,192,209,246,47,206,114,164,206,237,175,160,115,122,40,164,86,
  175,67,234,204,230,92,170,11,31,91,8,188,188,16,52,48,53,250,144,71,136,49,161,122,
  236,79,251,82,247,132,14,182,11,145,222,36,121,53,132,151,212,196,134,117,214,32,41,158,
  124,222,157,164,57,221,193,130,105,62,75,210,178,147,2,190,107,119,42,47,91,83,23,130,
  62,202,203,1,184,87,14,174,50,150,150,75,156,28,40,77,59,23,206,189,209,201,80,48,
  19,77,72,160,141,75,212,207,213,148,106,148,183,192,60,71,218,42,58,165,2,130,209,127,
  203,10,237,27,80,76,223,18,117,192,84,254,70,185,82,71,190,166,52,210,169,17,252,79,
  158,50,65,140,147,238,200,41,99,117,115,93,203,34,190,211,183,220,47,148,66,100,24,38,
  27,163,46,157,188,19,49,138,120,157,137,228,19,216,147,248,121,164,50,109,75,131,155,50,
  6,33,194,11,143,255,188,172,110,81,174,143,7,41,223,205,115,252,2,181,181,137,26,39,
  180,31,234,199,141,134,88,43,194,132,129,73,56,99,37,119,99,99,247,221,160,119,123,213,
  105,118,40,114,18,244,139,130,2,134,76,74,108,230,11,19,164,83,124,51,249,44,250,38,
  176,128,40,213,170,12,154,228,141,30,16,4,245,150,65,241,246,74,33,161,119,24,132,140,
  17,108,25,97,137,124,18,132,171,117,144,24,97,100,100,183,239,238,96,40,126,153,9,75,
  228,244,250,235,80,52,136,45,245,217,113,251,137,152,65,122,141,68,82,30,19,21,196,215,
  53,118,115,51,128,12,222,91,76,92,13,88,126,115,65,81,42,135,84,96,90,205,211,207,
  98,39,158,242,198,101,34,94,161,96,13,110,73,156,19,136,160,157,229,169,183,126,168,31,
  243,54,250,68,27,58,2,197,73,31,19,164,29,230,56,71,162,39,118,11,26,42,35,201,
  108,172,84,93,55,95,15,41,155,197,234,246,139,252,245,189,234,231,152,243,121,7,147,252,
  99,33,175,137,175,94,122,76,124,109,206,237,65,41,77,232,44,107,139,255,46,192,191,112,
  33,163,178,5,245,239,188,17,142,62,27,232,197,169,160,43,224,101,138,129,220,38,105,131,
  1,87,2,192,37,1,120,10,172,53,232,121,131,115,58,238,229,169,112,231,255,119,32,92,
  164,209,22,137,222,166,94,145,64,126,171,52,154,225,152,85,171,114,208,234,50,173,47,74,
  190,99,123,34,49,37,232,223,131,149,134,129,130,84,22,144,124,156,52,36,58,40,78,186,
  167,183,240,255,50,254,199,13,70,189,83,137,225,148,55,144,240,133,226,94,123,3,29,186,
  227,22,207,182,219,59,39,208,215,167,143,58,59,145,178,64,238,82,45,104,58,24,230,41,
  221,156,41,57,231,18,14,230,167,75,244,220,160,84,76,51,116,123,167,30,135,205,4,80,
  81,49,82,116,73,173,46,99,173,46,169,213,101,188,213,131,104,245,224,182,122,16,173,30,
  4,238,215,5,186,146,241,147,97,247,150,115,65,64,255,9,253,120,160,31,15,74,59,90,
  117,125,202,224,17,196,130,41,198,192,20,167,6,83,26,139,77,113,18,48,229,177,216,140,
  1,163,86,226,133,37,38,237,64,156,206,57,45,97,237,14,224,129,131,22,218,53,220,216,
  120,53,205,211,180,198,170,20,117,21,229,238,119,81,156,12,116,105,60,232,178,1,45,54,
  133,170,182,17,238,13,245,151,216,31,254,126,224,85,13,61,56,209,243,156,7,121,3,132,
  222,222,245,200,236,186,58,157,178,181,226,140,14,12,141,1,200,141,202,209,234,165,249,22,
  116,36,43,157,186,149,42,185,115,62,235,232,253,209,165,71,98,24,68,94,246,151,216,225,
  207,38,122,211,91,34,171,64,26,19,65,61,149,80,79,5,84,176,206,233,57,64,229,93,
  77,97,156,232,78,76,234,24,127,166,41,93,38,139,197,242,38,209,108,63,4,153,217,18,
  63,237,78,144,102,195,202,159,129,129,198,64,38,229,240,165,4,153,123,115,104,132,115,112,
  80,84,161,255,59,10,249,5,236,83,166,131,67,58,240,156,117,149,122,70,165,48,70,216,
  58,39,226,140,253,181,136,220,175,71,207,151,90,43,35,208,248,25,12,29,222,115,34,79,
  134,37,26,62,100,55,44,166,54,252,148,221,176,228,54,12,83,222,5,244,32,126,99,111,
  108,238,88,205,233,8,183,239,222,67,99,0,15,144,38,115,65,96,53,35,174,58,225,116,
  185,103,170,169,74,249,35,73,223,212,165,87,54,206,83,89,50,174,253,66,64,161,69,142,
  201,88,3,161,209,79,134,116,174,221,239,147,199,160,104,217,143,214,26,166,136,180,139,28,
  8,54,74,26,209,145,89,74,106,236,62,43,140,11,193,137,29,5,229,169,166,141,38,182,
  217,29,237,177,117,123,49,8,166,67,194,213,23,11,127,207,133,119,57,15,72,244,70,199,
  182,81,168,180,234,66,80,91,200,234,20,175,229,70,28,22,239,192,100,224,47,98,204,133,
  168,116,206,207,187,167,93,149,217,24,38,33,31,96,74,162,88,252,234,164,163,75,232,187,
  158,210,184,116,238,175,73,132,143,247,51,23,165,23,106,144,237,196,73,128,202,73,65,172,
  203,152,62,96,122,158,23,91,161,42,57,139,77,130,137,247,113,183,212,81,26,4,64,165,
  206,192,3,107,3,61,167,178,83,167,55,42,148,42,6,183,4,47,100,65,137,210,88,10,
  133,67,38,59,247,153,127,48,216,39,172,90,95,149,163,77,100,35,75,70,228,221,198,203,
  241,86,3,76,80,154,124,239,183,161,25,123,231,135,160,59,109,179,121,85,151,222,17,138,
  234,134,132,143,224,85,142,31,169,198,109,215,102,215,36,219,44,235,102,22,86,194,244,232,
  146,153,129,232,98,54,18,111,75,89,236,150,93,191,31,103,52,250,0,102,173,85,202,217,
  204,90,43,113,224,34,134,202,12,214,57,89,87,45,124,117,148,116,206,0,66,253,190,203,
  134,192,198,171,200,235,125,213,70,252,234,159,157,30,144,102,207,46,22,149,183,203,179,118,
  242,59,112,66,59,18,167,183,167,25,113,74,4,202,208,49,240,202,75,253,55,7,62,155,
  9,42,207,139,152,151,65,225,75,12,53,94,16,104,135,124,154,41,26,59,143,124,104,137,
  119,32,140,148,53,57,131,247,215,163,118,186,9,123,252,217,134,64,243,27,65,14,210,249,
  88,19,76,218,229,212,199,181,109,125,222,44,83,209,62,61,145,147,69,11,28,254,138,194,
  200,15,193,94,140,110,62,139,56,197,3,139,32,15,244,46,51,189,77,100,164,103,89,197,
  177,245,251,200,229,251,124,171,119,234,197,155,181,118,179,150,174,79,232,199,47,89,29,97,
  201,59,85,229,10,250,82,139,199,117,80,147,39,64,231,76,191,142,184,120,205,5,60,30,
  213,189,134,207,111,67,131,122,255,174,59,28,244,175,224,240,223,104,110,109,235,197,227,29,
  164,206,201,105,165,86,161,204,112,58,54,233,197,242,238,119,202,240,233,126,185,8,190,156,
  144,76,163,54,129,147,129,51,175,226,122,108,179,192,233,14,62,81,133,186,82,100,111,205,
  74,130,8,60,93,43,235,238,133,103,46,137,199,222,209,59,207,9,5,63,174,239,53,14,
  247,91,205,253,131,31,209,112,64,104,92,33,251,195,209,93,44,224,6,91,151,58,21,233,
  217,39,86,151,178,17,70,26,208,115,64,106,43,114,85,137,180,210,185,199,82,250,218,143,
  117,52,72,173,191,113,244,246,32,210,98,227,246,234,58,181,141,72,87,239,182,50,25,234,
  227,237,234,164,211,127,87,143,180,172,211,62,186,235,164,182,125,91,111,173,239,218,68,118,
  46,225,33,5,69,207,60,188,197,90,123,105,240,156,214,110,146,187,40,77,205,234,136,209,
  182,223,29,13,110,134,131,235,135,212,246,155,187,96,157,176,185,191,30,155,205,77,8,185,
  54,60,133,125,55,190,117,58,221,13,144,49,19,96,65,101,17,196,64,27,79,153,70,179,
  1,17,81,55,235,123,155,177,73,109,64,182,195,206,232,180,67,185,43,199,67,240,158,107,
  82,97,185,239,172,241,104,56,245,250,30,120,58,239,238,55,35,160,14,47,59,29,102,207,
  99,0,100,17,137,128,140,39,144,107,140,238,66,144,198,230,89,109,211,7,34,89,251,120,
  48,224,255,86,223,59,108,180,126,204,0,101,156,206,82,193,5,143,115,226,253,170,236,63,
  203,181,244,27,245,170,223,97,240,44,31,237,203,89,16,110,71,98,17,100,244,18,93,42,
  31,146,209,151,62,25,28,92,175,178,80,156,224,212,112,128,181,213,161,145,14,113,210,99,
  197,1,219,83,167,74,58,216,137,206,29,23,213,65,54,158,19,156,74,14,188,19,62,148,
  210,33,78,122,106,57,80,251,154,103,166,195,29,195,86,241,38,189,11,38,204,111,193,249,
  37,132,143,215,229,30,4,103,67,193,44,179,151,45,183,226,4,103,168,211,79,199,30,161,
  233,125,76,121,206,186,203,88,28,179,25,235,121,202,195,216,233,66,102,128,77,239,98,234,
  19,219,93,149,242,192,206,88,157,211,158,235,78,39,167,226,88,79,239,227,145,135,127,188,
  167,189,241,235,248,169,114,66,188,227,230,68,147,54,133,84,225,31,223,78,183,35,121,122,
  167,119,247,200,67,62,236,106,178,209,77,40,14,248,242,146,211,93,215,17,151,210,251,122,
  154,88,149,214,101,107,162,3,117,58,49,44,230,226,198,148,181,178,79,6,77,31,33,32,
  69,123,153,96,181,60,69,146,138,246,41,5,169,241,253,254,135,136,93,195,199,138,93,71,
  119,70,185,140,47,191,69,114,17,77,191,150,179,1,38,42,45,164,232,197,138,44,0,129,
  1,39,142,222,17,152,84,64,226,206,110,229,41,2,25,8,96,6,174,110,51,22,184,184,
  218,91,169,138,128,7,98,152,1,174,219,140,199,92,105,0,148,120,197,56,15,226,8,239,
  79,2,208,170,8,140,124,197,206,215,158,56,102,192,170,6,99,1,203,251,172,56,145,8,
  120,40,149,25,240,166,217,216,14,2,193,44,148,177,168,179,52,17,205,116,233,1,26,219,
  177,171,233,112,132,46,234,48,38,171,153,206,68,227,177,29,121,106,17,87,248,226,29,16,
  147,218,236,86,16,237,199,118,230,221,135,221,227,142,58,139,202,111,166,51,217,126,252,50,
  246,148,46,158,60,198,203,58,42,202,217,229,45,65,140,237,208,211,210,184,178,25,59,214,
  196,132,58,211,155,108,63,121,103,206,38,136,201,103,110,207,123,233,219,34,2,114,10,52,
  188,185,77,145,214,92,100,154,153,179,29,135,61,22,165,64,165,228,139,59,252,50,25,151,
  149,76,231,46,148,105,58,245,181,80,169,18,143,143,71,84,128,138,33,228,244,48,22,181,
  64,151,229,73,182,42,118,120,76,40,54,157,187,48,38,236,210,91,17,17,9,215,118,157,
  189,18,66,120,227,81,112,244,102,110,92,6,234,53,34,64,218,254,108,227,137,59,114,40,
  28,72,131,78,159,25,116,246,129,77,222,189,167,156,139,11,134,14,26,81,9,51,68,69,
  2,30,139,78,32,101,70,220,103,196,91,80,32,111,154,238,61,64,227,59,118,55,93,100,
  167,221,100,110,175,177,91,234,67,210,142,9,159,245,189,119,28,189,82,10,160,112,218,67,
  40,147,205,186,243,57,23,70,188,12,10,229,168,185,60,35,142,39,61,190,126,140,134,199,
  141,6,248,145,18,217,122,107,115,135,130,215,204,248,0,78,218,16,177,8,36,113,243,118,
  235,135,241,140,184,73,52,246,104,188,113,128,221,62,191,90,142,5,232,55,148,175,212,65,
  43,61,41,3,152,148,80,169,234,92,57,110,239,196,69,227,110,208,61,131,207,224,248,200,
  14,234,106,113,136,58,102,73,168,139,130,90,5,24,26,37,160,243,181,13,57,170,50,242,
  40,98,131,219,93,15,48,139,56,120,148,182,86,189,16,39,39,237,211,95,46,64,240,233,
  159,137,240,220,225,152,124,204,149,17,177,121,238,21,110,101,37,72,212,115,167,2,208,168,
  105,218,170,111,238,111,65,66,97,56,68,246,143,91,245,31,90,71,205,250,11,202,105,162,
  1,240,136,200,184,149,28,107,117,17,133,121,65,227,157,149,10,4,226,0,47,118,136,198,
  49,111,61,110,171,37,112,233,94,2,215,114,233,113,91,65,178,37,33,136,194,18,184,76,
  175,96,40,14,93,23,82,205,143,90,151,237,126,253,215,219,118,207,237,52,177,157,84,10,
  21,142,63,226,130,253,232,132,164,118,123,91,139,211,213,137,112,195,100,179,14,68,253,211,
  222,45,36,222,123,141,94,180,87,236,94,121,140,70,135,40,214,127,39,43,144,115,229,8,
  51,102,136,114,152,243,83,119,53,106,119,27,19,209,18,227,12,89,24,87,87,131,254,119,
  193,188,186,45,164,159,158,249,152,23,70,106,190,45,135,237,224,164,115,209,237,31,223,81,
  36,44,7,123,229,35,100,139,196,210,38,115,90,249,155,3,239,93,90,167,47,230,120,28,
  112,113,243,104,163,30,184,49,81,242,152,78,255,78,69,30,114,178,209,248,141,233,242,30,
  115,132,178,237,35,65,129,236,204,110,244,110,135,195,46,123,145,77,176,171,220,215,0,83,
  163,9,46,113,60,61,19,79,30,140,241,248,246,238,24,108,68,122,92,42,22,130,55,163,
  169,116,75,219,195,156,253,132,41,144,68,208,68,14,229,173,18,21,201,125,2,82,103,117,
  122,244,110,186,110,147,232,76,248,97,229,157,222,76,88,185,68,252,167,232,5,151,255,10,
  118,242,237,95,117,39,159,133,199,18,109,216,27,252,175,191,137,110,182,123,221,107,255,227,
  0,168,229,159,81,17,250,76,118,94,241,90,167,206,53,191,167,78,195,197,134,25,150,252,
  79,198,174,63,125,45,56,17,203,218,184,154,12,254,207,177,78,206,137,49,250,43,65,76,
  157,18,167,142,175,219,195,145,153,39,91,14,60,36,165,68,234,148,0,181,180,106,87,131,
  225,245,37,4,45,0,35,182,180,42,20,208,48,3,7,48,39,167,196,71,39,183,231,105,
  85,78,97,74,136,56,128,18,236,113,183,154,43,171,236,128,6,242,0,38,168,139,178,253,
  79,223,175,134,236,80,140,61,178,166,13,197,34,101,56,146,147,246,168,19,43,35,66,104,
  97,83,86,72,215,230,249,91,140,117,134,182,173,15,61,163,156,104,236,21,91,73,121,236,
  70,150,179,152,50,238,126,10,77,252,117,50,9,155,136,207,125,164,216,159,119,83,197,159,
  102,143,215,124,98,102,115,173,174,76,156,157,235,96,125,243,159,24,103,112,13,12,91,75,
  133,130,239,246,109,246,165,185,221,165,111,43,168,10,36,185,136,237,36,187,61,5,121,245,
  254,137,21,146,62,126,124,141,27,72,105,62,166,202,101,123,116,153,94,37,216,102,177,74,
  209,141,38,68,139,169,182,26,241,92,199,30,85,31,192,246,78,152,214,177,195,5,83,103,
  112,70,66,95,196,224,5,130,187,6,203,31,137,28,229,174,102,10,210,75,137,252,233,197,
  68,250,104,177,36,187,59,168,113,73,228,120,89,26,175,35,140,252,236,147,156,60,218,192,
  70,53,252,94,84,57,136,88,12,138,128,42,80,172,179,169,0,26,143,103,25,78,61,109,
  102,188,35,81,37,95,55,62,71,11,18,27,184,84,169,121,19,146,100,12,112,49,4,140,
  91,145,210,92,131,187,215,155,141,245,188,3,55,19,88,41,13,75,23,100,222,195,212,74,
  139,105,128,203,19,1,246,225,198,68,81,224,97,87,200,195,84,20,83,173,216,121,145,25,
  83,225,207,46,15,100,31,246,223,78,243,175,247,52,39,37,25,220,40,166,59,202,51,19,
  247,112,40,101,216,7,23,177,125,224,4,101,129,203,112,103,136,47,25,118,43,184,39,60,
  6,236,181,110,137,110,217,185,44,122,244,246,250,234,133,128,113,231,251,23,58,192,255,128,
  19,216,248,56,222,72,7,109,119,173,45,132,107,134,70,165,90,229,185,249,130,179,112,84,
  200,17,177,94,130,10,22,130,13,252,74,31,34,87,69,38,39,150,70,52,18,176,236,143,
  255,148,138,0,64,252,62,174,127,6,69,43,245,57,133,174,43,101,84,236,131,107,7,37,
  124,234,99,234,41,145,41,29,52,81,224,125,172,16,57,186,19,201,136,82,46,243,70,179,
  173,177,79,44,164,217,231,185,200,223,9,114,41,31,97,118,249,7,231,34,138,73,214,209,
  97,56,128,1,134,156,74,165,136,222,117,10,39,225,84,145,227,23,49,76,43,60,31,92,
  60,225,228,127,242,77,223,91,2,114,184,184,219,144,98,32,212,186,36,26,43,35,240,128,
  167,17,2,156,10,169,205,31,123,62,127,129,235,178,59,129,98,151,62,136,101,39,189,218,
  83,149,110,238,231,51,96,186,28,73,197,15,155,15,70,59,92,240,232,21,42,214,97,236,
  36,123,244,81,107,150,240,23,187,36,79,122,136,158,233,40,120,206,229,103,146,19,149,98,
  61,92,97,136,31,135,160,137,37,60,28,83,223,153,233,49,185,220,96,135,12,207,68,208,
  138,193,237,205,5,164,77,186,208,174,168,94,178,213,241,151,216,177,7,59,207,162,40,242,
  187,116,110,240,236,19,106,91,3,97,126,189,237,196,33,63,142,213,122,91,33,90,0,222,
  253,87,16,35,184,123,221,235,118,206,142,73,186,112,153,245,225,23,211,186,194,243,71,198,
  229,234,11,178,230,47,174,164,157,238,222,246,197,120,114,228,62,120,238,89,65,68,108,42,
  180,241,132,74,186,67,15,89,207,123,221,115,47,162,99,238,130,242,194,198,241,172,255,152,
  219,226,23,61,169,178,46,147,106,155,140,63,223,222,78,123,190,233,12,50,219,187,235,156,
  103,175,190,229,135,176,212,41,155,39,208,19,159,117,111,46,33,156,105,63,227,196,249,82,
  199,220,239,117,163,108,15,50,59,33,131,240,204,26,106,46,153,120,99,171,61,238,88,215,
  54,103,217,168,254,57,206,254,47,114,42,255,62,186,112,57,13,162,66,147,173,24,116,68,
  140,161,251,19,178,175,56,31,188,112,251,19,253,240,13,208,172,3,3,79,145,113,91,192,
  56,143,238,157,79,23,37,210,131,84,199,15,147,120,45,66,102,50,186,28,234,128,199,16,
  17,196,129,171,204,144,244,55,99,75,0,223,32,23,64,227,160,185,191,185,190,123,124,208,
  16,122,251,241,61,184,81,188,66,182,207,219,83,16,123,12,200,185,168,68,22,21,22,179,
  33,173,70,248,117,116,81,124,141,50,222,76,140,133,59,34,224,137,80,71,238,174,191,221,
  168,55,91,158,70,3,227,152,127,37,74,249,47,35,85,42,153,227,203,201,156,156,121,53,
  3,243,191,176,80,154,41,91,142,17,45,39,123,226,136,62,50,140,149,43,253,217,127,70,
  189,199,87,104,101,32,247,48,165,221,185,186,115,31,38,158,44,175,218,53,62,94,152,173,
  71,120,78,182,112,75,95,181,179,86,134,204,251,77,130,125,146,4,171,8,156,89,41,211,
  100,243,217,165,97,93,126,249,48,234,158,106,38,61,181,216,124,50,58,59,31,5,180,80,
  76,148,182,121,218,121,144,74,200,209,49,68,151,61,129,53,157,33,172,187,108,63,138,24,
  184,110,102,86,96,92,50,171,252,149,238,4,191,147,4,205,241,255,80,197,168,93,66,49,
  99,187,14,231,106,216,204,95,240,138,18,172,114,93,166,183,65,70,13,0,53,114,79,3,
  193,47,226,23,59,119,167,100,213,201,192,195,175,17,224,225,87,128,203,195,119,153,119,135,
  73,174,1,222,13,37,55,238,10,19,95,79,127,241,219,67,75,156,228,111,193,25,10,110,
  162,191,255,229,33,83,238,159,240,102,241,71,94,15,190,73,255,223,164,255,223,255,209,116,
  130,75,1,108,240,143,225,6,127,212,51,171,181,156,0,159,161,83,244,72,122,44,159,248,
  15,16,228,51,101,217,113,210,233,115,136,148,223,52,200,95,90,60,123,180,244,197,219,5,
  118,11,88,5,121,198,66,206,126,54,98,238,189,87,139,68,97,5,96,241,147,178,211,90,
  48,95,238,165,136,140,65,177,79,135,3,204,59,161,202,147,220,189,181,66,186,189,51,105,
  111,206,6,32,141,223,235,164,39,40,115,211,151,7,251,69,133,174,175,64,158,102,99,126,
  45,227,5,17,111,81,106,108,230,17,17,195,37,46,72,116,246,41,199,59,206,109,36,237,
  180,201,25,23,114,215,83,170,102,108,76,217,136,102,67,135,185,168,92,232,171,145,49,149,
  137,237,234,79,241,212,63,94,144,91,23,124,158,195,150,188,112,124,244,197,51,98,36,74,
  148,10,52,20,41,177,33,80,192,208,245,13,152,24,31,130,137,121,221,11,225,25,242,255,
  241,47,146,95,92,215,252,87,19,251,158,42,158,141,21,12,191,201,111,95,70,126,251,210,
  251,112,172,24,104,125,56,54,67,46,145,110,126,240,213,48,143,44,121,239,47,39,77,121,
  46,225,95,82,166,122,180,12,19,53,29,151,233,210,180,200,48,71,222,93,36,46,36,57,
  207,69,78,188,132,239,31,172,255,223,81,221,115,86,90,180,41,181,237,2,110,138,5,124,
  176,179,15,129,86,190,189,117,254,39,191,117,126,49,101,198,183,211,242,219,91,39,9,214,
  7,1,199,121,212,75,167,147,139,205,215,182,140,96,185,69,131,183,124,123,25,253,246,50,
  250,7,188,140,94,95,14,178,86,211,183,119,209,111,239,162,223,222,69,205,62,249,107,191,
  138,70,219,235,168,154,233,0,68,141,255,196,119,213,134,144,26,208,69,124,107,189,185,53,
  230,170,18,141,29,234,223,148,191,143,69,127,252,99,239,57,223,110,50,223,222,109,191,221,
  100,190,232,77,102,226,251,74,58,23,153,32,182,133,213,178,236,198,184,151,130,125,176,243,
  227,97,3,252,3,0,5,85,163,177,223,180,63,100,44,102,63,104,229,35,76,67,77,28,
  124,191,192,68,227,207,114,160,66,124,9,59,183,74,23,3,226,165,135,144,246,171,7,113,
  162,87,109,141,48,197,182,251,44,22,137,138,29,9,242,233,7,196,142,71,23,78,9,113,
  157,1,79,70,182,182,48,51,162,215,251,67,55,17,234,87,83,75,154,118,134,226,121,35,
  116,106,108,207,97,91,228,190,142,36,72,168,67,182,129,102,125,123,183,190,217,138,52,134,
  20,168,195,129,190,165,221,185,177,210,98,161,232,131,5,96,131,205,175,102,148,193,210,201,
  42,182,177,181,225,246,126,117,123,53,89,221,246,61,215,77,137,22,239,47,45,19,143,62,
  128,238,198,139,247,150,157,23,235,62,182,70,100,164,251,148,5,23,9,95,159,6,73,6,
  174,207,90,106,54,149,132,51,210,146,72,65,241,174,131,41,104,221,1,121,73,44,34,88,
  56,41,44,2,12,190,25,171,252,193,186,21,177,23,126,71,221,203,211,116,43,207,163,74,
  25,135,130,76,1,240,135,235,92,12,91,159,108,38,83,43,153,115,59,179,55,115,136,127,
  83,241,124,83,241,164,44,158,56,198,114,225,124,41,45,144,221,11,227,129,165,215,125,130,
  114,73,113,142,175,75,191,68,75,79,171,135,158,168,94,114,150,179,209,24,173,61,85,231,
  228,6,194,146,236,117,156,178,204,25,220,188,135,87,150,38,203,19,226,114,32,162,248,176,
  221,223,243,44,54,105,224,28,34,204,255,218,80,3,91,5,128,185,152,132,38,4,119,29,
  103,110,112,179,247,238,244,212,13,249,134,150,124,23,157,1,236,154,225,131,151,26,43,49,
  5,239,148,77,34,179,25,166,207,54,65,218,62,62,60,189,236,129,0,75,246,123,32,14,
  181,123,139,102,193,111,131,145,66,228,235,74,33,49,184,16,188,108,114,204,229,116,52,214,
  16,22,20,34,30,179,64,29,145,51,43,160,92,80,162,169,71,118,8,33,216,168,151,250,
  159,83,219,184,37,238,235,173,125,184,47,125,181,70,17,223,148,133,223,148,133,223,148,133,
  95,135,178,16,24,199,158,207,56,190,121,113,255,73,238,211,23,67,16,63,36,175,253,58,
  44,7,110,6,95,193,37,246,219,133,241,63,236,194,248,59,62,249,211,2,255,79,247,131,
  254,115,74,201,71,97,100,227,72,196,88,21,237,120,194,128,176,95,91,96,98,113,55,60,
  216,135,119,162,67,200,118,230,39,149,199,36,138,51,233,153,33,181,198,60,154,188,205,3,
  60,77,222,200,240,178,245,213,199,59,166,76,62,221,254,13,133,174,93,83,203,197,185,237,
  55,126,170,175,183,224,53,238,104,157,223,169,102,78,6,131,94,174,59,58,128,183,173,107,
  122,157,194,134,206,111,14,104,14,113,206,185,55,74,252,106,66,156,115,64,93,183,253,108,
  206,193,99,142,114,215,82,132,233,151,174,148,248,41,37,244,218,151,13,129,233,137,148,251,
  211,7,186,124,116,232,101,37,5,220,116,225,82,255,135,250,244,254,73,36,142,231,15,181,
  108,168,255,40,113,96,202,3,255,175,16,177,121,251,137,17,155,255,240,224,202,41,138,146,
  84,245,198,95,211,17,227,235,10,98,252,92,234,128,13,159,119,159,178,229,193,35,56,247,
  51,94,61,39,98,174,19,92,45,117,149,209,47,99,88,239,88,30,24,245,127,35,90,89,
  166,108,53,218,176,20,14,169,235,183,208,117,126,214,4,129,253,10,184,209,167,64,36,30,
  154,20,192,142,117,6,60,247,130,254,252,177,153,60,50,152,214,239,162,86,229,240,2,119,
  126,78,117,212,184,177,236,245,115,174,76,105,234,56,55,11,201,86,58,48,130,206,216,35,
  146,209,80,110,58,212,201,225,117,59,90,94,212,229,218,203,177,255,59,75,140,42,3,137,
  167,70,252,228,134,74,0,163,27,216,120,224,173,43,8,227,228,114,135,214,121,53,245,24,
  97,130,232,129,14,157,179,42,26,132,186,43,49,44,90,57,29,153,208,222,251,178,136,145,
  44,78,7,16,139,66,47,50,2,226,225,0,181,22,64,210,238,143,169,245,16,129,255,64,
  50,122,127,44,252,249,9,176,64,248,142,2,22,99,28,135,67,20,185,62,49,5,160,63,
  65,115,130,250,171,95,52,127,202,47,207,41,118,255,185,3,236,252,199,10,227,191,147,222,
  237,107,21,195,113,23,220,172,253,22,226,252,106,112,157,164,44,170,87,61,85,36,41,244,
  234,84,124,116,171,95,170,18,135,100,175,110,229,87,183,193,25,20,57,90,187,87,231,250,
  139,91,241,250,58,137,201,254,175,174,196,119,245,233,2,62,9,249,243,213,177,250,237,136,
  171,175,238,225,43,202,89,175,238,174,147,152,245,221,171,7,168,16,211,182,191,58,188,78,
  210,214,237,171,183,97,153,219,248,36,165,130,194,170,30,22,171,146,22,148,56,247,197,87,
  31,245,23,183,135,117,231,179,106,189,169,63,170,223,77,252,141,124,237,213,1,252,149,110,
  92,248,170,241,255,179,247,230,125,109,36,201,218,232,223,221,159,66,102,124,252,74,80,200,
  2,220,158,238,146,203,252,48,155,233,102,179,4,152,54,205,149,133,84,32,217,218,172,197,
  128,65,223,253,62,145,107,100,86,149,160,61,221,51,115,238,125,207,156,54,170,140,200,61,
  51,50,50,50,150,65,144,198,237,135,187,72,207,126,211,14,55,146,96,5,217,31,4,105,
  194,228,240,216,77,119,251,116,160,154,239,46,246,112,203,73,118,179,188,65,121,174,221,84,
  248,205,36,165,41,72,134,159,13,216,77,63,113,211,85,63,182,109,106,170,14,99,248,206,
  32,168,28,111,7,129,61,149,194,247,242,139,23,121,42,147,76,9,95,24,138,73,252,125,
  16,100,62,110,133,79,7,65,250,3,90,248,65,65,146,222,74,195,223,18,32,55,235,175,
  6,110,47,32,225,39,147,232,136,252,195,113,55,200,124,235,10,99,3,116,77,3,195,158,
  7,112,179,181,25,212,83,250,10,71,73,152,155,121,104,16,220,167,135,176,158,4,208,139,
  67,216,79,166,131,39,12,59,42,25,76,204,69,98,87,55,0,76,57,130,194,150,155,238,
  102,154,164,0,213,74,104,186,32,149,122,217,13,156,245,60,144,223,110,169,93,147,232,74,
  158,194,43,15,224,102,171,1,154,166,53,24,222,120,0,55,219,87,64,83,196,16,225,173,
  74,119,136,78,85,37,186,148,100,79,165,38,69,51,225,133,11,210,52,178,27,164,190,104,
  133,71,73,128,152,209,107,147,238,81,74,55,93,147,74,147,170,105,165,78,72,118,255,176,
  27,60,160,6,25,238,204,66,17,205,219,229,24,110,249,27,221,32,83,159,56,220,239,6,
  30,251,16,30,119,3,21,242,62,60,192,207,25,247,219,112,139,224,92,162,18,190,233,6,
  201,167,161,240,27,79,245,8,102,55,72,213,69,13,79,60,128,155,109,187,27,100,136,35,
  194,183,9,144,166,150,28,160,201,165,77,243,101,22,225,23,192,92,241,86,248,187,76,242,
  229,123,225,83,150,174,146,62,168,36,103,25,252,134,196,20,227,234,240,87,55,221,109,199,
  167,110,144,198,198,133,239,220,116,55,211,248,42,72,85,203,12,99,15,224,102,235,93,5,
  254,205,36,108,219,52,213,141,145,72,81,31,195,171,192,19,142,133,245,43,48,84,141,207,
  87,152,192,94,83,36,134,125,39,137,202,14,59,60,105,157,248,7,129,217,72,36,11,236,
  214,149,228,49,4,206,68,125,8,72,243,42,16,84,78,130,46,245,151,128,13,174,136,135,
  176,100,37,236,178,4,129,113,117,21,232,240,176,18,163,198,18,4,198,205,85,64,81,70,
  41,192,98,172,122,243,213,73,18,88,183,87,160,113,163,22,86,75,187,33,145,170,60,69,
  224,236,201,20,125,68,10,172,11,55,77,224,109,202,52,233,66,81,162,29,57,73,2,235,
  90,38,177,21,22,174,57,73,2,107,93,38,201,51,81,32,85,120,138,192,57,212,41,234,
  244,19,104,59,94,162,192,220,149,137,226,92,22,88,27,44,65,96,236,95,5,3,122,72,
  147,139,33,60,54,159,2,122,112,165,246,155,132,110,153,79,1,125,131,207,193,176,61,86,
  115,245,205,124,10,232,231,171,105,112,137,219,135,98,58,239,212,165,40,188,251,90,239,76,
  226,176,23,95,231,14,198,249,165,151,255,252,231,63,151,151,126,42,76,3,117,95,212,8,
  75,83,58,188,12,250,164,211,17,9,230,85,149,23,180,63,158,202,251,198,158,159,67,167,
  206,206,118,132,107,138,6,148,166,83,190,187,195,59,253,145,40,155,1,178,138,159,42,134,
  48,188,195,223,68,1,50,173,162,164,46,126,203,184,57,31,27,20,216,106,154,175,226,79,
  2,109,88,23,44,124,133,138,209,176,226,47,63,79,131,230,229,213,238,241,145,83,233,84,
  222,181,194,187,122,63,57,88,125,30,252,138,213,41,0,217,125,212,12,104,120,167,35,104,
  185,229,38,226,106,177,162,77,28,174,204,210,213,149,44,188,163,31,137,178,85,98,230,252,
  18,188,74,82,57,91,231,212,30,238,225,157,252,153,40,214,36,103,22,44,49,156,162,9,
  52,193,154,14,150,10,168,196,187,13,209,14,176,9,137,10,61,96,102,181,28,207,235,151,
  3,123,211,174,143,248,146,102,55,45,44,70,245,145,92,145,22,144,61,33,156,57,12,239,
  204,87,162,48,14,201,46,141,179,13,225,221,144,89,77,186,165,113,72,118,105,236,162,20,
  222,233,15,191,172,41,93,178,194,59,252,179,225,174,199,229,159,226,197,159,4,116,31,220,
  25,27,89,164,108,217,132,229,120,69,36,9,1,79,38,65,83,107,27,179,128,195,130,154,
  33,196,67,78,158,179,115,133,116,72,87,12,158,58,154,244,118,85,110,157,72,204,27,44,
  132,199,237,120,68,107,73,197,69,15,239,166,242,70,140,31,83,155,79,190,175,100,229,150,
  84,220,110,199,37,69,215,229,162,153,170,47,105,15,225,166,145,246,211,132,125,99,104,73,
  7,195,84,174,223,117,72,216,235,245,71,130,214,235,144,145,55,157,190,154,190,212,59,223,
  213,103,63,255,127,164,239,172,17,153,99,48,232,143,103,118,80,247,41,208,47,14,244,219,
  233,181,102,134,228,8,244,32,98,20,237,25,196,189,73,247,98,88,87,159,205,184,81,191,
  85,83,162,235,252,207,44,8,93,59,219,129,206,72,36,7,73,176,31,223,49,74,170,203,
  124,132,166,188,180,127,127,255,85,194,122,189,27,15,235,146,158,184,105,130,162,152,86,102,
  173,154,22,209,228,1,110,99,241,227,55,199,232,243,237,186,30,37,197,153,219,237,66,120,
  107,195,184,254,231,7,249,186,221,28,183,232,71,43,166,188,178,184,206,184,81,91,242,206,
  123,36,45,187,73,82,134,82,125,155,150,10,111,1,156,136,174,153,244,250,77,106,122,37,
  30,245,59,19,159,101,90,211,3,57,250,46,94,115,36,38,205,126,122,71,107,55,139,177,
  252,215,217,205,128,169,10,38,206,51,201,81,127,87,143,228,51,165,199,155,192,237,108,145,
  208,135,30,211,89,250,207,113,219,211,224,180,23,221,137,123,87,120,167,222,230,70,225,105,
  156,63,187,28,23,229,237,1,23,137,34,227,201,233,83,242,213,244,75,240,179,244,67,179,
  161,244,27,167,243,121,33,144,23,92,218,89,24,136,207,227,162,123,229,11,244,157,57,13,
  129,96,88,201,242,158,247,215,181,139,177,97,244,169,120,91,250,105,120,82,250,240,120,71,
  213,35,83,216,40,48,44,156,183,34,74,5,125,173,72,97,182,167,25,67,194,111,184,25,
  131,194,47,188,216,103,116,25,253,223,53,40,186,69,254,14,90,90,250,249,197,207,75,132,
  160,93,142,106,140,149,210,119,12,165,189,184,103,12,164,189,199,163,74,156,84,205,250,176,
  57,107,36,255,182,161,227,124,55,125,115,174,254,207,13,173,41,137,211,75,93,26,35,48,
  223,49,152,76,194,145,57,158,76,224,49,13,72,178,49,107,56,255,226,65,100,215,141,63,
  53,102,89,61,54,146,154,140,222,26,193,13,81,103,146,45,205,234,235,159,220,71,119,186,
  68,126,83,202,104,39,147,117,101,180,148,137,190,236,185,236,183,85,166,103,211,107,38,154,
  74,169,134,73,170,192,247,9,225,222,172,225,16,125,244,207,117,202,86,117,143,126,105,227,
  206,210,150,83,135,193,19,50,166,180,207,147,57,78,165,164,115,86,19,189,153,73,169,213,
  138,78,83,42,180,146,84,45,158,248,43,150,199,93,130,181,200,90,21,76,182,153,177,42,
  152,168,211,242,54,126,19,101,122,246,170,96,66,199,148,106,152,12,114,202,68,211,172,154,
  187,108,134,43,24,47,111,120,242,37,83,66,26,225,74,180,205,19,164,167,180,207,147,171,
  79,61,241,57,111,103,154,204,208,98,191,233,76,134,195,182,71,99,103,54,151,65,51,132,
  142,179,58,100,100,254,51,59,101,158,0,166,66,230,207,187,51,150,253,227,189,25,111,65,
  245,65,39,45,46,165,176,177,201,22,53,102,180,163,97,107,215,175,2,78,11,54,77,162,
  35,14,242,171,112,158,24,82,170,113,94,28,248,157,243,209,91,59,184,131,220,22,119,58,
  228,58,52,55,44,231,174,211,139,201,149,131,46,216,74,162,82,82,33,142,74,219,146,206,
  83,74,26,185,224,47,43,250,106,154,236,131,58,205,52,5,109,164,8,189,74,105,23,144,
  180,38,177,215,132,180,173,107,31,23,166,211,242,105,175,168,15,247,200,109,20,32,154,117,
  42,106,0,26,166,95,84,217,118,48,105,137,125,196,33,153,212,160,225,186,6,201,46,100,
  255,65,113,177,135,154,41,55,14,146,126,4,211,250,83,201,148,147,166,162,204,20,39,75,
  175,131,172,150,132,55,65,6,99,143,211,233,169,137,6,185,176,204,150,184,110,6,237,19,
  199,138,3,242,93,12,26,188,82,41,3,79,186,23,212,120,47,178,241,178,155,205,145,50,
  219,47,252,212,176,33,177,238,4,211,110,35,220,215,160,247,170,196,65,179,107,171,164,48,
  222,9,223,131,41,165,63,106,101,240,103,103,214,45,158,156,40,221,3,62,170,236,170,116,
  88,200,25,46,185,31,82,209,146,85,234,185,225,109,204,156,212,241,99,102,178,62,198,201,
  57,17,199,163,79,106,75,14,52,115,118,185,247,82,255,194,25,200,109,238,123,56,77,127,
  90,124,120,29,248,142,77,249,82,72,113,122,154,94,13,199,200,30,22,207,31,165,63,91,
  142,191,73,79,46,196,65,153,194,174,191,236,22,88,134,121,111,110,130,83,99,24,150,130,
  11,252,119,69,19,119,114,21,81,77,245,56,24,52,35,89,103,121,208,44,142,226,113,126,
  113,41,40,137,255,153,191,133,242,229,164,39,228,169,185,237,171,124,59,24,7,113,208,11,
  70,193,176,112,215,137,199,185,122,100,166,59,232,71,163,40,138,158,148,86,75,16,27,119,
  130,70,48,137,168,215,193,32,42,5,45,241,211,150,213,204,95,203,2,42,209,117,177,61,
  170,130,188,196,50,243,53,227,161,196,168,149,219,151,249,202,179,103,21,160,29,201,112,145,
  50,103,53,226,168,150,9,124,93,42,87,34,220,72,209,157,74,80,45,76,65,196,39,195,
  94,174,50,53,149,215,108,229,79,150,130,106,68,141,41,87,81,61,85,183,218,205,215,131,
  126,33,172,62,123,86,69,149,98,225,61,123,150,239,230,171,24,140,0,57,74,133,50,229,
  189,136,218,197,155,33,85,179,217,251,218,30,246,123,226,177,17,54,31,205,61,216,123,228,
  11,229,11,20,56,87,111,66,131,8,55,223,185,213,24,247,140,75,112,58,163,162,96,28,
  104,184,133,195,173,188,30,240,97,33,148,89,72,52,184,120,65,37,205,61,123,246,64,182,
  18,178,5,249,118,177,62,25,247,69,250,253,125,165,128,230,218,108,226,46,68,217,72,218,
  152,71,235,131,20,152,176,63,114,97,166,58,3,107,75,15,89,188,54,49,58,1,75,216,
  160,18,121,66,21,27,170,209,238,20,10,118,248,111,243,215,65,69,207,97,51,95,193,216,
  163,197,98,176,193,181,170,57,190,191,175,22,149,138,16,6,101,107,88,88,205,55,240,67,
  104,223,151,128,222,16,11,239,54,206,211,159,175,109,69,78,2,250,106,245,242,119,61,60,
  44,132,115,111,28,110,124,79,57,250,154,11,12,19,245,190,157,7,15,229,50,237,134,147,
  242,118,97,18,145,131,253,253,152,196,118,17,32,103,199,227,223,175,177,188,11,11,73,48,
  214,162,248,120,47,110,131,248,162,199,89,252,169,119,58,253,235,3,84,70,7,49,18,166,
  133,66,208,40,106,231,108,152,68,140,100,188,54,198,123,201,197,4,78,221,230,228,221,114,
  110,54,210,228,171,64,232,247,222,196,232,108,92,193,106,139,135,145,158,163,252,69,176,23,
  172,23,238,112,76,140,48,13,244,20,35,220,132,99,85,12,110,53,159,158,95,231,32,144,
  242,131,11,210,169,43,74,175,65,135,242,17,229,54,223,40,106,15,107,193,156,188,200,205,
  5,119,216,54,161,169,172,112,167,246,168,168,78,15,127,81,34,23,5,109,156,78,49,183,
  197,201,160,73,110,235,26,98,0,140,223,182,212,12,81,53,21,37,141,96,168,12,149,84,
  224,3,165,152,3,35,165,16,3,123,160,12,125,5,149,69,208,150,219,26,246,187,242,1,
  236,69,254,228,10,153,63,199,26,137,64,155,19,48,0,249,74,74,17,133,66,81,240,10,
  120,172,2,9,10,188,61,37,41,154,156,234,35,161,87,170,0,68,121,151,158,61,251,83,
  205,180,26,150,183,249,65,211,153,16,82,239,195,68,12,226,102,116,42,8,177,60,233,208,
  230,170,36,43,85,82,90,47,60,137,34,72,55,242,19,252,173,222,223,15,232,15,109,41,
  98,114,238,239,91,248,108,155,146,64,5,136,170,177,58,122,113,220,28,29,139,245,0,146,
  140,163,166,138,115,198,228,199,121,227,102,70,243,58,245,91,162,107,113,175,14,218,186,214,
  233,96,124,174,209,79,136,187,47,199,249,6,219,44,172,43,130,194,210,169,80,48,7,130,
  25,203,124,135,211,163,14,167,71,183,237,252,114,176,156,77,141,30,65,137,30,69,133,30,
  79,129,82,169,207,117,251,59,169,79,231,49,212,39,157,26,116,24,53,232,62,154,20,64,
  26,149,66,7,58,162,41,201,53,107,176,177,40,58,127,193,246,237,124,231,210,174,42,250,
  184,134,195,80,45,85,98,113,104,29,201,30,40,243,205,244,94,48,225,156,218,114,68,124,
  243,186,208,194,159,218,57,157,239,223,57,157,217,59,167,195,86,3,235,7,219,57,246,228,
  239,202,147,255,154,6,174,178,253,38,63,233,4,199,141,124,187,144,202,119,72,54,103,210,
  41,14,3,252,115,69,255,92,4,21,240,59,182,188,75,172,151,198,19,206,21,176,133,217,
  214,84,144,81,13,150,168,50,161,127,188,128,78,90,1,157,180,2,58,186,0,197,96,210,
  58,182,60,81,202,138,174,227,154,145,142,130,65,137,150,10,119,117,193,130,95,19,35,93,
  9,36,23,10,173,5,149,101,141,184,194,148,82,251,182,84,15,5,60,110,63,186,54,5,
  13,5,217,15,107,1,88,210,163,190,60,4,118,113,171,11,111,3,213,169,240,114,106,135,
  246,173,96,246,37,151,22,99,73,160,29,135,245,33,232,24,6,2,28,224,222,218,105,237,
  100,179,114,180,121,90,131,205,114,101,231,77,21,251,50,34,141,139,168,149,23,211,30,12,
  163,81,80,199,185,98,153,254,126,254,56,216,10,78,130,141,96,91,22,252,148,248,239,15,
  209,0,233,27,72,223,42,148,135,152,140,15,152,135,97,244,33,104,228,135,197,190,32,34,
  88,32,79,193,41,74,172,109,124,60,123,86,179,95,219,79,36,231,254,236,217,88,83,134,
  109,176,160,155,187,155,123,8,44,92,91,171,84,214,126,71,40,226,173,173,205,10,54,205,
  211,251,251,58,237,136,186,224,253,117,139,120,41,237,226,5,92,95,189,17,235,49,159,94,
  78,32,239,23,219,5,181,108,249,34,239,216,201,1,199,12,253,146,113,124,34,200,244,218,
  112,88,191,205,51,204,70,254,152,161,82,165,28,241,152,97,78,28,76,73,115,179,112,7,
  166,79,98,140,183,163,141,226,117,91,40,199,118,37,253,193,88,246,206,182,138,237,230,121,
  249,41,63,194,158,210,252,41,72,244,84,222,114,62,68,199,56,243,118,148,137,75,115,15,
  87,76,121,77,67,114,19,23,202,246,56,122,122,246,225,188,140,191,172,36,124,161,40,2,
  68,237,177,44,232,20,191,206,78,68,165,167,28,245,148,48,53,40,58,149,200,239,162,211,
  179,237,243,178,234,240,59,142,255,14,235,11,35,92,8,8,35,122,87,8,222,217,158,183,
  104,148,40,255,86,4,45,158,19,250,103,3,255,148,65,66,243,114,40,74,229,237,87,113,
  121,123,97,161,176,69,217,75,193,137,252,179,33,254,148,205,78,150,36,32,148,215,215,97,
  31,175,155,93,249,97,70,146,142,70,28,239,230,224,27,133,91,129,36,144,77,150,118,66,
  82,18,249,177,1,1,226,8,246,122,225,70,32,23,117,120,108,129,66,111,10,243,15,35,
  4,41,137,103,23,101,111,54,17,253,216,228,194,76,110,241,207,15,145,152,145,19,90,156,
  182,21,184,139,234,17,56,205,181,177,128,198,5,92,169,49,230,167,231,197,78,191,33,152,
  186,215,81,73,237,245,113,180,13,64,80,161,137,61,61,23,151,239,177,59,97,184,163,106,
  155,39,121,128,225,154,138,229,224,36,1,17,217,252,84,204,26,207,45,200,160,147,89,223,
  182,157,188,34,145,206,8,219,142,251,251,120,108,59,142,157,91,25,227,226,59,198,117,25,
  12,71,125,92,23,41,226,87,65,206,233,147,82,249,195,194,130,150,5,240,49,220,159,116,
  137,230,220,223,15,139,98,2,240,177,193,69,5,222,240,211,186,254,59,6,93,12,118,252,
  125,35,29,255,11,35,29,167,140,180,216,131,21,218,195,229,10,27,230,40,134,240,199,142,
  177,152,38,241,43,82,41,32,162,232,5,70,62,16,99,205,6,41,218,14,188,49,7,125,
  87,3,206,135,27,228,81,12,200,49,150,185,179,187,204,96,110,209,166,69,147,165,115,141,
  242,214,171,147,242,22,182,243,241,217,22,54,48,231,52,64,10,46,49,121,37,135,93,192,
  100,202,10,78,252,10,64,41,134,197,196,6,14,156,253,166,183,112,249,228,236,248,60,90,
  2,213,192,159,40,18,20,79,229,85,68,89,228,208,164,89,226,45,209,232,28,159,99,121,
  109,9,252,175,12,83,21,44,154,39,176,162,45,214,236,235,172,65,9,182,210,218,108,134,
  234,132,8,27,214,170,26,170,147,87,27,229,19,65,249,78,168,21,199,248,35,26,130,211,
  63,189,229,39,133,128,112,35,62,132,21,123,132,7,79,131,15,133,187,15,242,56,112,251,
  179,35,124,210,197,67,133,12,212,66,232,98,184,8,162,52,86,139,61,150,239,176,34,202,
  250,24,115,8,95,98,191,97,35,110,21,149,193,182,73,63,33,198,217,14,73,123,76,123,
  241,169,28,207,83,108,186,246,88,144,184,211,228,158,124,7,58,168,160,239,188,179,237,145,
  219,242,93,202,174,116,115,103,111,203,119,41,244,239,157,97,84,13,213,120,167,52,22,240,
  102,208,36,146,253,174,136,203,91,151,158,16,130,181,177,18,130,190,43,80,31,214,44,105,
  41,64,97,122,220,238,77,98,49,176,253,56,90,27,43,54,38,248,50,166,143,241,237,32,
  14,126,21,63,47,110,49,182,112,32,179,217,17,15,182,193,151,232,11,21,212,46,194,209,
  227,253,189,250,125,140,144,99,219,251,155,27,53,145,248,174,120,53,152,28,161,8,192,174,
  251,98,252,4,11,129,233,6,151,250,53,86,156,149,153,34,217,153,79,104,59,209,144,224,
  118,28,125,194,171,38,221,54,131,99,234,81,255,242,18,28,46,149,243,137,179,34,137,2,
  177,223,213,44,223,140,113,150,223,140,95,217,89,165,17,65,10,86,63,159,235,133,155,113,
  240,73,136,205,15,121,123,202,9,150,231,137,188,182,109,20,97,181,122,179,99,166,101,210,
  115,232,117,26,56,89,252,252,39,92,113,0,42,76,227,14,156,170,62,166,205,93,183,205,
  152,79,151,67,117,56,211,126,108,79,158,89,133,86,188,129,168,140,159,187,104,88,12,32,
  247,152,143,249,95,199,65,254,120,188,144,192,152,71,91,8,248,69,246,229,206,76,181,26,
  186,196,68,235,118,125,66,179,62,249,173,250,228,207,206,167,224,221,223,59,57,201,226,231,
  223,165,78,78,102,123,157,153,249,244,248,137,201,44,176,226,14,64,214,164,84,196,164,36,
  39,228,147,154,14,217,124,204,199,135,36,205,248,160,201,90,60,182,192,209,117,27,158,68,
  144,164,78,11,220,171,97,63,156,91,246,200,246,242,229,87,214,64,180,4,207,28,184,225,
  124,46,11,236,21,15,123,101,38,246,11,15,251,69,22,182,162,233,30,250,82,2,29,26,
  212,211,107,126,191,186,192,193,121,196,24,177,99,34,254,61,125,69,232,225,156,181,103,37,
  129,20,111,128,51,19,199,158,189,53,16,104,67,95,88,233,166,96,64,31,228,105,50,201,
  211,85,71,223,85,3,121,61,203,137,123,145,250,77,185,166,83,245,65,21,51,14,127,143,
  88,21,76,7,146,197,29,200,204,137,100,86,203,166,181,226,242,244,31,106,48,213,109,155,
  188,46,184,43,205,145,177,81,61,161,155,165,173,110,131,64,39,154,117,62,57,219,16,235,
  110,219,239,168,115,30,61,141,182,189,174,62,186,209,42,227,148,13,238,205,236,150,130,63,
  154,125,191,101,173,102,59,73,23,248,148,10,84,178,140,15,104,247,211,115,143,211,248,128,
  86,139,253,230,55,91,110,66,211,238,167,231,122,172,169,58,45,58,253,28,223,142,192,133,
  169,45,41,57,77,51,37,91,78,63,177,204,239,118,192,3,213,233,126,79,66,148,145,16,
  162,140,28,33,138,69,7,234,221,200,136,186,228,51,237,168,168,110,186,250,211,74,13,32,
  245,85,87,99,156,196,147,65,216,135,52,9,191,194,35,249,119,67,110,208,42,185,64,11,
  119,140,56,233,2,80,28,208,163,88,0,70,7,151,219,250,94,189,231,67,100,135,195,27,
  63,253,80,221,189,215,113,57,110,51,110,15,66,43,37,141,212,73,33,249,106,16,140,236,
  113,15,246,33,252,18,126,205,198,233,189,122,195,150,115,214,179,178,169,17,164,201,119,189,
  168,99,81,49,143,65,3,59,179,216,28,214,175,5,71,60,202,247,232,93,155,164,149,250,
  37,10,111,225,75,108,92,235,121,241,238,93,184,35,209,172,188,23,216,220,102,149,201,98,
  128,230,22,52,225,51,212,215,37,97,233,77,104,234,53,57,144,76,221,220,251,205,55,219,
  187,53,241,2,83,163,42,230,10,69,241,177,97,170,19,24,162,170,18,74,130,160,87,50,
  210,45,28,61,122,145,54,241,187,249,106,82,110,226,228,105,45,68,141,51,236,58,211,164,
  150,236,155,16,196,211,51,48,94,181,177,158,196,167,148,37,70,67,254,165,123,55,138,234,
  60,121,79,55,42,234,219,206,157,26,77,2,57,15,163,50,27,117,209,229,209,19,143,12,
  2,7,201,227,34,28,117,229,231,54,79,143,106,99,249,14,83,187,108,119,192,8,214,140,
  102,69,187,49,87,16,91,88,22,189,174,184,224,7,243,148,71,190,148,115,93,72,57,33,
  226,60,58,174,108,214,232,183,141,53,90,67,170,98,18,70,70,112,148,27,241,149,176,174,
  37,118,79,242,235,232,77,173,247,236,89,15,220,69,143,14,49,0,133,148,222,147,170,238,
  236,29,74,97,163,240,0,41,163,180,34,226,237,218,70,109,139,194,181,30,185,11,100,93,
  246,240,38,90,71,127,55,80,60,31,30,233,125,70,178,245,181,86,189,3,207,60,228,229,
  111,174,112,127,159,133,165,16,10,101,222,236,24,229,210,223,93,252,125,114,243,175,116,225,
  232,247,195,77,87,78,186,46,166,154,90,63,215,130,110,229,96,78,124,139,34,229,51,213,
  33,116,75,219,244,42,177,69,183,156,49,64,74,230,92,125,187,182,1,150,170,93,124,187,
  179,253,182,182,181,139,56,103,5,122,145,148,216,175,75,36,196,157,81,202,86,101,109,91,
  200,116,31,42,71,45,62,213,188,242,122,52,215,141,97,230,216,29,204,105,81,146,104,189,
  78,124,160,90,191,241,123,155,27,59,199,123,127,69,243,51,75,90,53,109,11,231,240,142,
  135,118,211,146,105,68,177,69,50,59,109,149,37,134,170,203,120,30,234,224,189,189,76,244,
  172,129,21,118,72,164,39,190,216,222,149,15,8,120,80,152,3,113,193,163,31,54,241,100,
  48,232,131,63,107,6,185,9,124,109,94,33,111,32,110,186,113,189,89,36,101,131,72,81,
  160,1,106,135,139,167,58,30,26,91,221,118,67,168,140,72,150,89,138,167,91,128,15,99,
  122,143,138,155,62,144,100,252,118,245,66,103,186,70,204,195,176,143,7,199,242,172,92,45,
  245,196,157,108,127,238,88,28,36,136,109,145,163,32,23,186,136,156,120,12,205,201,173,145,
  107,78,4,130,80,193,67,180,4,191,242,28,232,10,189,21,226,113,60,183,133,7,83,122,
  68,36,124,197,185,58,101,97,40,202,146,246,166,62,170,104,114,179,179,183,182,189,89,59,
  222,223,57,194,203,74,109,230,3,76,106,150,219,217,197,147,179,217,66,208,77,71,90,63,
  126,67,244,238,208,195,190,124,220,51,208,245,76,52,52,143,104,25,62,215,143,14,42,85,
  210,168,74,71,95,171,252,190,179,191,109,241,170,233,120,102,47,36,10,190,72,207,80,93,
  35,10,5,248,94,18,174,97,250,33,160,61,18,107,101,57,196,170,188,34,77,168,155,53,
  163,192,23,14,85,146,217,159,97,39,80,7,140,220,169,21,44,124,90,90,97,93,167,147,
  84,196,164,246,3,187,219,26,65,250,134,8,7,65,202,154,14,91,48,122,186,81,138,8,
  163,176,73,95,82,124,103,210,106,12,67,104,148,222,82,2,233,130,116,149,93,54,185,68,
  187,97,92,210,165,45,229,88,43,37,92,139,52,25,34,100,20,86,232,107,75,233,17,24,
  156,42,165,74,165,212,17,88,190,145,250,181,199,152,174,47,56,238,229,89,5,153,20,88,
  131,32,150,44,102,15,2,202,17,61,195,13,233,31,169,74,216,233,225,249,83,42,37,226,
  137,149,41,79,6,236,221,154,212,16,202,92,67,32,234,72,158,3,214,255,135,194,117,36,
  74,214,9,66,80,52,146,54,225,58,153,184,73,171,236,52,8,90,178,121,205,104,160,152,
  109,98,223,240,148,126,127,223,147,191,70,230,136,143,192,23,25,180,160,57,149,5,10,135,
  103,202,180,62,226,175,180,226,137,93,190,136,42,84,16,158,116,196,37,133,0,102,107,187,
  211,191,128,110,60,113,193,94,51,227,104,66,63,72,174,109,176,147,120,65,83,118,168,134,
  150,106,167,154,114,96,64,24,100,18,31,22,16,2,153,168,26,166,183,122,126,32,100,135,
  79,70,247,247,53,165,30,137,95,236,62,130,87,19,176,5,221,194,112,85,245,49,108,224,
  190,45,132,65,84,253,117,52,132,50,104,15,123,252,122,254,5,118,240,165,105,140,104,50,
  70,151,244,59,59,70,89,163,138,206,213,208,250,10,218,111,152,212,11,240,87,23,244,160,
  83,94,88,184,40,84,207,46,206,163,24,255,148,189,194,144,61,117,198,111,87,221,149,17,
  150,188,165,178,16,225,130,80,102,143,178,24,58,217,34,98,127,132,78,128,108,95,28,184,
  202,19,196,31,224,33,216,174,185,158,252,114,23,28,127,196,21,51,19,212,228,220,96,34,
  212,147,243,170,94,78,104,155,188,125,209,168,223,210,202,19,28,17,150,183,108,66,80,19,
  194,174,251,251,174,154,13,89,210,101,212,92,184,197,0,95,71,45,174,145,183,211,19,148,
  163,220,167,169,212,134,25,66,225,4,58,6,249,174,153,208,174,170,253,213,37,189,139,119,
  197,238,219,34,94,112,101,89,62,4,92,22,236,108,84,176,133,160,181,89,174,160,37,183,
  152,17,168,216,46,68,47,10,117,169,156,50,56,171,156,23,138,208,28,233,220,106,253,181,
  107,104,31,4,117,37,167,134,102,137,44,178,11,205,220,160,123,86,93,88,57,143,40,175,
  184,60,140,167,122,168,187,129,175,167,162,249,45,62,222,183,105,227,29,116,167,83,82,123,
  126,51,138,94,4,191,95,69,47,131,167,87,209,114,41,248,128,127,127,122,25,156,14,69,
  255,14,71,193,151,137,210,93,14,190,53,36,61,250,220,160,199,37,250,103,187,65,20,233,
  55,169,40,189,22,156,182,213,143,203,78,212,232,212,71,163,59,209,226,225,132,212,191,243,
  99,165,39,89,27,42,158,34,26,203,37,86,19,219,14,102,206,92,229,78,86,37,225,157,
  62,84,193,110,52,81,170,145,209,22,209,102,147,64,190,24,118,251,205,17,189,100,219,28,
  144,81,196,44,137,105,45,246,111,120,233,13,73,237,181,178,25,7,105,187,173,52,216,5,
  116,31,211,210,175,174,110,156,228,233,37,84,17,133,226,118,30,87,57,180,185,23,21,225,
  46,36,130,5,10,8,250,221,84,172,205,59,225,78,162,46,134,222,120,208,232,71,191,93,
  77,163,97,25,227,238,14,156,208,11,98,99,5,129,198,231,52,156,53,225,204,158,78,179,
  45,104,91,1,235,36,27,107,175,77,198,157,187,56,69,73,111,105,59,129,120,99,158,208,
  130,76,8,45,6,53,35,177,8,181,148,175,75,38,174,163,74,35,37,57,8,34,99,217,
  108,122,252,85,203,181,35,117,172,21,47,250,196,76,44,13,219,81,159,122,112,124,130,225,
  35,141,250,14,109,148,152,152,127,59,15,121,169,71,21,23,84,62,177,179,14,247,42,155,
  123,16,87,232,105,134,196,164,55,25,80,130,156,19,109,219,87,239,93,145,77,131,152,29,
  73,44,184,110,93,141,80,21,135,64,40,5,145,87,113,8,143,206,3,187,190,65,187,19,
  107,198,66,92,88,242,122,63,248,11,80,43,216,228,211,193,31,38,121,211,39,89,172,6,
  165,103,40,152,234,189,30,123,205,72,44,118,175,29,9,248,211,135,26,226,231,64,75,140,
  98,152,170,212,42,138,165,182,221,170,44,165,65,139,126,110,191,66,47,187,15,78,228,119,
  104,196,19,175,255,14,48,77,255,45,13,45,169,17,87,152,154,221,97,40,162,34,112,104,
  87,171,8,241,2,136,165,248,9,102,119,57,79,90,93,30,221,19,192,65,255,26,218,179,
  60,63,74,246,135,151,19,41,111,48,56,40,209,9,70,196,188,108,12,146,200,149,70,199,
  189,236,105,40,182,28,115,124,210,131,220,248,149,71,203,245,11,61,61,203,121,160,51,136,
  142,147,51,50,53,155,62,113,246,16,63,232,144,208,111,13,208,80,16,200,194,12,242,182,
  221,192,73,58,194,93,4,74,8,164,12,76,244,110,107,132,29,78,196,103,92,20,238,142,
  240,87,122,59,66,237,62,25,184,27,51,91,141,181,54,137,152,236,247,219,246,170,75,59,
  17,89,178,91,191,138,25,27,185,186,244,50,84,169,103,165,115,89,31,21,98,82,100,6,
  145,94,8,211,75,19,192,231,47,10,193,255,166,83,69,28,34,189,8,156,240,3,231,136,
  4,235,107,36,59,56,82,78,134,158,127,50,32,161,55,77,150,173,110,100,43,243,98,219,
  225,22,151,119,183,99,176,180,4,29,246,56,122,49,239,165,67,209,19,67,190,37,132,168,
  33,190,187,237,158,253,184,194,201,54,36,221,102,49,66,35,210,147,35,125,130,112,3,214,
  223,226,90,28,214,122,129,85,149,14,27,195,128,29,145,116,179,3,15,241,251,36,47,197,
  195,66,232,155,205,75,105,22,118,198,30,20,235,130,244,177,103,98,201,165,77,12,191,222,
  80,143,216,243,143,33,19,172,43,196,14,41,138,22,14,167,98,197,148,243,119,102,171,135,
  222,214,15,52,247,23,186,204,224,52,250,245,42,63,52,196,211,97,215,222,1,34,158,55,
  82,120,182,79,26,54,53,210,234,228,241,166,21,126,153,49,195,231,24,122,192,101,111,21,
  171,140,249,24,12,53,17,126,143,159,113,108,4,165,105,196,155,56,255,11,153,185,209,80,
  64,22,120,182,20,44,10,155,45,250,223,57,196,140,103,242,247,162,250,127,120,97,243,246,
  20,228,130,3,107,87,22,208,93,157,233,169,151,7,69,71,1,59,255,5,207,43,14,70,
  116,216,11,88,1,68,227,6,100,62,63,246,237,225,4,185,144,66,151,60,221,136,6,9,
  154,58,162,162,27,198,222,45,79,159,62,142,212,125,79,229,209,103,157,191,174,89,155,176,
  32,169,24,11,18,177,191,139,214,142,100,206,181,34,51,166,27,220,170,3,246,26,146,87,
  189,141,82,170,195,125,243,214,234,236,95,210,160,92,71,220,141,69,249,122,245,154,155,65,
  42,253,124,113,219,187,166,235,175,69,149,151,133,75,122,122,9,93,68,154,11,145,206,175,
  145,229,202,171,151,229,10,142,60,101,9,88,249,159,21,97,132,89,90,197,141,123,34,6,
  20,70,142,13,220,40,3,210,210,47,154,232,83,4,0,29,133,254,224,45,254,251,38,44,
  20,250,253,207,107,34,117,97,66,25,52,8,86,59,40,113,201,41,81,150,249,248,18,41,
  85,151,74,37,254,43,173,211,169,162,188,130,54,36,117,201,107,25,39,239,40,168,206,195,
  216,225,245,242,42,89,237,94,4,23,105,43,12,171,240,242,217,179,129,122,104,203,195,250,
  130,176,212,23,164,103,133,169,187,254,155,206,242,111,185,115,119,61,77,57,93,244,51,169,
  191,19,71,209,236,3,191,60,90,205,127,247,21,32,131,99,182,134,49,151,16,251,108,114,
  83,191,241,12,171,182,213,197,165,112,73,243,12,223,117,19,144,179,132,183,244,213,212,118,
  101,20,13,113,166,207,202,149,206,203,117,179,215,112,253,21,218,119,208,224,212,29,131,160,
  198,177,96,28,59,87,76,190,60,98,193,152,173,204,119,130,229,249,14,25,68,249,75,35,
  166,68,181,18,234,146,68,51,6,193,80,121,111,90,193,136,216,5,82,142,29,90,41,218,
  50,138,50,56,87,189,173,135,209,82,121,248,10,194,82,195,201,138,106,183,183,79,37,131,
  128,53,53,92,36,83,103,167,244,222,52,137,102,223,134,253,86,14,163,236,211,214,140,58,
  59,248,32,79,246,103,162,119,142,177,54,51,81,87,227,92,183,62,84,32,16,122,158,79,
  239,235,34,20,121,39,81,60,11,60,144,23,153,209,23,188,146,54,230,27,139,147,121,208,
  191,86,212,152,95,42,46,255,68,7,215,124,43,48,156,64,77,114,2,129,38,207,250,152,
  71,35,65,156,87,230,111,231,243,189,215,181,197,55,163,213,222,98,109,225,13,228,151,68,
  75,95,204,123,204,218,226,45,214,169,183,126,138,106,71,131,4,25,215,116,10,214,36,163,
  38,33,4,86,9,181,197,152,88,254,97,208,13,46,177,178,110,177,178,110,11,193,40,177,
  178,134,148,168,86,86,159,86,86,224,213,58,156,81,107,41,89,107,79,94,52,30,168,117,
  236,215,58,149,226,17,197,105,200,149,242,192,194,144,92,46,226,182,141,2,241,243,112,167,
  240,92,252,168,190,171,28,45,151,45,55,117,8,1,31,45,84,81,118,157,243,89,2,66,
  76,84,79,64,166,28,221,115,139,224,45,90,189,6,57,183,22,116,18,11,115,116,94,182,
  198,102,81,191,44,31,107,251,150,68,52,178,166,184,81,28,181,175,186,117,61,9,100,244,
  198,7,154,95,167,23,165,78,217,196,95,112,35,226,185,86,230,39,243,249,209,107,7,31,
  107,111,180,200,83,212,50,108,165,44,67,188,241,10,2,69,162,110,148,133,9,157,144,248,
  55,133,64,213,245,132,118,196,132,50,17,252,175,236,189,8,98,206,152,254,233,69,109,28,
  59,109,180,101,97,105,225,247,43,70,113,74,134,226,168,161,103,82,4,112,221,112,143,55,
  25,181,180,192,174,31,45,61,207,215,23,151,201,126,111,177,143,125,190,180,208,39,230,179,
  35,52,114,232,63,250,87,255,109,208,136,188,68,63,95,98,215,174,224,25,54,33,26,111,
  206,183,230,7,244,218,154,1,49,237,188,36,181,155,87,131,242,165,110,231,117,116,249,63,
  43,243,203,207,87,64,15,43,209,37,142,250,18,28,127,65,182,126,6,115,64,80,247,235,
  5,192,248,175,5,242,201,113,157,150,66,127,207,203,53,193,122,84,3,170,248,210,123,62,
  121,213,42,95,232,138,247,162,201,217,197,252,242,249,252,50,170,91,151,31,11,75,226,179,
  124,41,248,175,211,182,40,10,208,96,175,16,94,10,14,74,165,45,238,17,143,190,46,83,
  151,89,234,58,116,133,68,226,138,73,164,252,139,170,128,23,12,149,0,133,80,125,83,78,
  162,153,167,109,243,54,112,27,228,47,231,91,120,235,153,135,83,1,161,175,32,223,36,62,
  199,229,46,229,97,198,198,154,235,154,19,140,242,135,24,175,71,77,48,16,62,26,226,18,
  14,38,227,13,237,25,218,96,223,10,236,88,174,16,197,120,119,149,213,106,208,123,253,6,
  74,116,189,197,69,173,251,102,111,106,49,187,155,177,119,78,92,245,184,114,153,104,244,184,
  167,210,244,77,190,167,183,172,225,152,182,112,242,154,68,98,244,35,197,232,211,166,58,254,
  58,71,231,59,151,205,64,210,110,235,196,102,51,78,89,72,89,237,43,34,215,146,26,134,
  24,92,157,14,13,17,85,130,147,108,75,249,100,212,226,116,43,29,203,117,209,28,156,206,
  235,80,253,81,30,167,231,2,105,216,13,203,53,0,244,171,122,248,225,42,32,237,129,227,
  19,210,29,216,220,173,189,223,217,56,122,27,46,61,31,187,201,111,55,161,111,115,132,244,
  88,167,211,211,252,222,206,97,248,241,233,93,123,90,44,125,156,6,179,221,255,13,83,156,
  130,73,138,199,131,61,56,166,243,93,18,213,120,70,242,31,127,68,156,220,31,204,67,124,
  78,104,189,200,128,168,229,52,8,172,86,202,34,203,87,249,42,46,35,170,126,61,112,23,
  152,68,209,225,85,229,147,248,112,121,35,39,59,82,230,64,29,91,89,245,38,5,38,123,
  37,139,252,135,28,244,220,230,254,137,208,207,128,74,149,212,214,56,62,17,96,27,31,51,
  59,174,225,107,167,164,195,157,220,74,113,233,197,210,79,191,44,191,252,105,229,167,95,4,
  240,249,243,220,73,29,182,135,208,124,89,71,16,178,9,26,8,247,174,112,43,132,254,139,
  139,49,33,169,150,75,128,122,107,172,157,52,215,243,19,12,82,238,162,61,30,137,0,169,
  248,63,250,77,1,132,197,223,87,175,114,75,47,39,133,220,189,250,126,253,90,124,151,29,
  76,9,122,150,43,221,252,164,254,15,57,40,167,204,104,193,107,234,255,144,78,5,101,151,
  179,162,254,79,150,179,236,151,179,174,254,79,150,179,156,93,78,105,75,254,79,150,243,194,
  47,71,65,75,178,156,23,51,202,41,109,109,209,127,178,156,159,19,229,108,73,4,89,206,
  207,186,28,181,59,197,208,231,229,16,207,231,150,139,43,203,63,175,148,94,190,88,193,12,
  254,252,242,151,151,43,241,226,82,169,156,195,44,62,71,81,120,143,147,255,71,37,76,245,
  252,190,173,119,225,122,127,212,137,111,115,35,220,93,200,187,155,88,214,20,175,185,101,96,
  114,46,219,208,232,162,191,251,122,66,85,51,68,216,103,217,150,118,1,117,201,159,251,8,
  125,157,178,42,96,229,95,118,90,0,178,145,59,217,223,128,82,118,151,8,22,233,142,202,
  173,66,219,42,191,57,108,55,114,111,227,246,248,91,110,185,180,244,115,65,101,154,171,106,
  12,196,101,19,69,144,103,50,65,231,133,82,228,101,238,4,219,149,244,185,228,131,247,104,
  78,101,108,141,199,16,70,62,127,254,169,113,53,46,246,135,87,207,7,147,139,78,155,124,
  210,62,199,208,252,243,121,233,197,243,210,210,115,53,4,43,172,77,82,181,133,104,28,53,
  54,47,6,232,20,35,34,208,78,2,127,3,235,33,146,201,194,151,83,46,178,96,76,23,
  219,235,2,19,141,171,202,45,154,123,81,92,10,115,7,195,49,92,99,139,214,231,200,5,
  251,72,96,137,234,142,150,116,32,96,196,143,15,40,24,176,248,71,45,14,137,178,76,161,
  180,135,125,240,164,104,220,17,252,119,165,212,178,28,230,140,218,83,251,91,93,15,157,10,
  138,7,229,183,58,12,47,88,55,40,168,177,184,205,224,148,190,81,181,73,8,194,31,2,
  182,140,168,196,243,68,74,230,49,52,34,64,182,65,24,83,147,135,0,80,96,109,96,59,
  185,199,203,10,72,177,185,125,32,109,24,10,41,62,159,163,222,34,52,247,9,164,41,18,
  65,228,19,169,8,8,46,194,141,83,227,228,55,42,156,199,63,5,100,160,209,30,47,167,
  13,192,74,152,171,196,54,38,119,14,106,124,253,156,13,48,97,135,115,191,133,154,68,145,
  24,251,5,106,240,60,13,241,130,172,145,100,230,98,18,156,170,233,215,178,168,186,64,109,
  59,73,54,96,165,248,34,204,25,239,30,57,173,46,24,119,32,217,24,145,46,38,212,11,
  46,219,87,147,161,152,26,190,229,140,185,95,94,44,3,185,190,230,209,76,200,150,114,236,
  235,54,200,153,198,225,243,91,193,217,125,94,12,107,211,89,244,149,85,224,29,106,124,149,
  157,0,115,95,80,152,181,209,104,210,141,115,196,118,228,76,232,141,28,40,10,246,158,42,
  140,204,168,176,182,226,69,169,118,141,205,251,163,45,9,233,50,57,110,178,240,217,43,121,
  187,172,213,82,33,103,212,239,99,21,88,26,80,59,170,208,182,67,158,225,109,14,10,174,
  118,119,5,185,79,147,209,88,29,190,130,90,196,214,79,155,106,106,231,86,148,209,190,204,
  229,237,14,125,69,165,151,150,244,200,80,32,119,82,122,115,130,123,95,180,201,167,53,68,
  205,82,82,38,207,117,140,116,160,78,235,130,88,18,170,3,106,238,228,199,212,52,251,8,
  175,213,212,20,17,193,87,238,116,49,82,130,38,106,66,104,7,106,50,64,229,245,139,81,
  126,31,179,41,26,249,203,47,191,228,86,205,104,41,114,64,213,230,194,7,136,196,88,85,
  205,167,91,210,140,9,245,162,192,113,113,202,24,108,137,131,94,170,36,77,92,232,174,33,
  79,10,154,155,73,25,127,95,137,35,35,207,120,195,2,146,113,7,209,195,170,8,41,50,
  176,227,70,157,52,78,54,93,137,158,106,193,140,6,114,9,6,185,81,159,236,108,198,206,
  88,194,54,106,132,205,112,13,35,136,92,158,122,143,97,41,152,90,87,114,111,107,182,75,
  153,132,94,211,120,111,108,3,70,234,89,195,236,102,126,107,182,179,8,47,41,155,196,43,
  119,70,93,55,100,222,54,170,120,3,242,98,71,157,67,110,1,217,119,82,52,69,148,101,
  239,58,101,75,170,220,236,143,233,36,120,75,164,232,45,72,211,137,105,182,220,88,251,64,
  160,124,68,46,8,23,99,186,91,208,43,70,34,226,13,78,98,189,22,201,122,6,5,69,
  147,219,139,111,45,148,121,217,190,193,33,130,173,144,235,208,11,169,69,207,60,239,91,240,
  98,12,181,81,177,79,155,252,68,183,52,227,7,219,79,185,169,245,142,204,216,140,187,102,
  51,234,126,80,11,20,9,185,184,85,29,167,29,71,149,194,41,117,123,188,8,90,150,195,
  173,108,216,135,175,92,75,123,109,211,15,241,95,175,142,221,140,199,212,219,92,189,33,140,
  69,71,166,144,175,138,247,128,18,239,176,127,137,121,31,59,157,81,133,37,72,222,66,228,
  244,104,94,54,77,77,236,15,156,242,1,147,195,166,156,164,16,21,227,184,238,100,165,208,
  217,68,210,115,78,101,29,114,149,70,3,253,236,140,228,81,187,62,6,194,133,37,197,227,
  125,219,155,225,103,108,202,174,161,239,102,95,67,171,116,56,19,95,185,93,159,64,17,191,
  222,35,55,125,236,34,170,47,161,79,255,93,151,80,33,114,251,255,196,5,84,244,228,63,
  127,249,100,176,237,131,221,141,205,125,152,92,109,239,110,210,21,7,167,221,203,149,229,229,
  95,254,185,146,194,194,152,13,32,59,146,139,4,139,144,51,160,212,5,236,19,142,156,166,
  28,254,104,106,66,146,147,199,122,206,63,215,249,78,17,211,225,201,155,28,170,156,40,61,
  167,233,147,123,204,251,133,20,191,229,18,199,126,206,57,155,114,230,224,207,57,39,127,238,
  161,163,95,157,235,57,58,252,253,166,23,114,179,153,1,63,131,225,13,114,236,230,113,52,
  196,14,135,84,91,16,201,207,241,176,23,119,232,152,24,183,134,49,40,175,242,96,15,113,
  195,215,182,32,186,163,28,38,73,192,227,28,180,130,219,3,152,30,22,57,47,216,66,80,
  29,136,161,233,212,194,2,192,26,42,137,59,4,102,62,160,53,229,50,142,178,110,57,13,
  146,79,143,111,6,121,252,145,151,11,83,24,251,249,220,44,36,85,172,28,6,59,14,160,
  252,147,174,195,175,242,129,86,119,63,194,73,225,88,233,176,200,231,12,183,36,153,37,69,
  186,232,99,97,129,173,91,58,98,199,116,13,184,68,28,115,45,125,89,92,103,28,34,93,
  220,196,93,88,209,196,32,39,194,43,52,115,100,132,152,187,234,119,154,113,111,17,83,210,
  161,51,174,141,131,171,200,79,255,17,21,142,67,15,215,41,117,125,71,11,232,238,68,131,
  99,239,244,186,125,102,233,179,137,160,187,161,30,43,186,20,97,100,37,231,1,45,191,188,
  26,113,93,205,60,159,142,130,87,154,188,75,178,86,204,59,68,64,159,226,114,135,9,167,
  32,180,10,113,173,20,57,9,93,47,189,5,113,161,52,201,102,213,114,94,73,18,69,190,
  71,69,81,178,75,148,203,223,9,170,80,134,32,29,147,216,153,130,172,140,144,197,116,96,
  18,122,208,66,87,215,143,166,244,36,73,92,33,66,246,129,114,137,9,113,102,226,90,175,
  20,167,150,231,242,151,174,132,173,59,176,2,42,203,124,38,33,243,250,104,233,88,153,21,
  103,89,139,107,117,240,63,116,242,243,229,255,156,175,115,67,30,255,21,38,128,222,220,211,
  207,127,79,205,87,43,42,204,205,62,165,191,227,16,86,54,147,169,199,176,134,253,53,7,
  177,119,112,138,40,41,175,179,14,184,239,59,91,204,101,107,242,21,200,90,79,225,248,107,
  62,73,235,203,51,230,92,61,80,160,237,118,125,161,196,66,113,120,117,241,151,204,251,135,
  204,121,87,218,229,143,156,239,192,234,135,176,128,58,127,247,42,112,217,38,219,132,239,93,
  35,212,85,103,149,164,172,7,111,158,212,12,81,78,198,203,136,195,201,182,7,212,194,175,
  159,68,71,137,180,219,111,246,204,251,222,25,237,90,87,152,98,108,103,12,109,246,200,26,
  167,95,185,180,229,47,80,30,30,221,228,224,37,118,74,148,44,90,14,177,118,119,110,118,
  130,126,116,228,171,30,67,244,81,152,29,13,148,133,144,52,9,110,226,70,219,115,141,133,
  224,55,46,22,214,49,176,151,22,154,46,86,93,83,153,155,210,4,58,42,164,112,94,40,
  31,20,239,100,224,217,177,142,59,59,86,145,212,132,206,236,89,47,48,255,59,151,234,12,
  106,69,136,135,200,234,144,20,198,140,238,180,210,145,58,24,8,134,43,175,155,160,159,33,
  211,21,170,96,134,149,98,96,226,106,130,243,82,72,241,55,146,127,220,226,173,254,111,196,
  63,92,36,79,147,56,74,164,184,232,70,11,57,98,191,61,20,173,181,28,177,223,122,108,
  255,100,116,172,143,138,38,59,107,79,216,221,121,27,91,241,221,90,64,100,160,196,3,202,
  108,16,70,6,244,1,145,195,139,156,52,224,99,44,96,66,242,11,2,172,144,230,213,138,
  20,5,200,11,79,161,120,67,91,215,187,148,36,105,199,15,94,91,137,126,164,52,209,174,
  245,46,152,112,101,63,104,138,103,71,151,48,128,173,201,17,122,237,3,149,164,221,128,127,
  52,194,139,143,233,68,56,229,196,52,147,82,126,244,192,167,31,172,233,163,161,230,33,245,
  80,245,70,170,192,56,209,101,213,194,227,19,239,108,109,38,209,211,201,245,242,70,222,246,
  45,176,197,217,41,164,97,162,13,174,34,93,252,20,224,127,228,94,57,245,144,220,98,251,
  211,117,47,223,203,114,41,223,155,233,70,190,151,21,184,130,157,7,83,104,55,48,247,235,
  186,126,109,55,91,182,145,98,160,96,65,74,82,208,135,98,155,84,171,70,240,61,28,69,
  235,109,17,199,196,38,125,139,165,71,253,219,62,69,251,144,166,116,5,237,173,102,76,154,
  90,28,187,15,5,163,20,91,166,122,154,47,111,42,105,42,131,155,144,225,27,105,88,208,
  63,35,227,69,102,40,189,200,184,86,36,70,197,166,14,21,155,58,148,153,235,164,245,152,
  212,93,35,237,62,82,35,83,225,83,164,238,69,121,156,162,90,199,149,160,198,37,171,4,
  69,189,126,15,87,104,116,162,199,206,87,207,139,171,51,130,219,30,50,44,49,156,84,75,
  153,24,211,63,97,115,181,158,135,1,205,16,255,112,63,71,45,97,101,220,130,151,12,63,
  198,78,83,88,20,75,5,123,160,52,49,45,23,253,251,123,250,187,217,47,24,135,60,173,
  130,54,55,151,206,118,90,5,77,114,245,212,246,201,174,91,23,165,220,5,202,28,45,105,
  191,67,197,215,224,214,91,89,98,188,46,105,35,105,234,237,160,147,215,0,163,71,115,91,
  156,113,20,181,97,42,31,72,29,152,86,0,141,194,86,17,94,207,55,191,98,17,147,203,
  115,58,69,242,115,106,250,231,200,161,83,63,127,107,52,233,188,102,230,52,253,21,103,128,
  30,85,238,232,231,81,195,7,229,49,119,244,160,51,214,84,202,211,77,169,52,77,35,112,
  127,127,43,179,118,197,113,71,67,9,109,207,174,245,209,210,53,135,217,160,139,8,28,39,
  42,102,93,137,114,183,210,143,109,106,24,71,70,97,151,218,171,82,207,170,64,75,125,165,
  203,142,240,199,223,141,106,171,189,180,17,198,120,118,11,161,4,105,99,81,74,10,210,27,
  22,185,85,7,177,154,19,158,129,154,222,77,248,123,178,96,165,46,167,23,138,130,99,177,
  92,227,255,205,122,193,200,137,148,14,204,18,86,243,223,209,175,68,175,190,163,79,51,215,
  217,132,21,88,80,60,69,202,130,234,231,133,251,6,5,144,139,102,213,172,35,44,152,80,
  46,32,116,205,166,190,109,163,114,238,91,201,120,180,40,97,229,189,52,164,234,22,164,234,
  246,85,173,124,75,30,191,206,110,207,89,236,3,56,1,43,179,74,107,220,79,125,203,174,
  231,177,88,90,229,38,180,55,187,253,175,113,246,166,42,115,138,0,127,14,53,30,103,97,
  172,156,216,3,16,212,184,13,41,243,150,240,231,43,157,232,74,227,212,74,227,7,43,29,
  144,129,233,44,114,107,109,103,123,236,252,144,84,152,71,130,8,71,198,13,222,128,105,8,
  198,140,168,223,49,98,15,123,31,65,68,198,208,250,78,236,4,74,84,234,247,194,43,199,
  166,118,54,132,76,101,134,3,239,108,35,221,4,80,101,27,9,162,103,86,19,213,163,186,
  48,21,46,245,120,68,137,56,219,39,88,16,107,143,115,210,237,17,242,215,116,212,84,9,
  61,216,172,90,247,106,148,169,38,69,82,18,58,211,35,153,45,92,120,176,147,28,80,83,
  233,78,215,198,125,93,46,199,84,64,4,74,197,243,83,13,2,93,216,185,244,80,150,8,
  152,225,244,92,14,92,204,198,106,100,104,195,231,118,194,27,212,220,66,111,97,206,250,115,
  202,57,158,173,200,141,213,136,187,93,236,149,60,71,118,228,31,126,200,151,76,153,29,20,
  3,137,68,118,108,106,65,183,140,207,121,29,192,66,46,240,188,2,88,30,163,70,23,132,
  22,115,129,93,96,184,54,245,172,118,94,64,169,179,183,73,221,184,161,28,157,181,132,171,
  77,185,207,134,234,196,41,55,197,14,85,197,99,179,12,245,198,105,21,164,97,73,170,91,
  71,162,152,173,164,255,95,13,86,158,184,84,205,173,164,63,94,98,223,80,37,216,53,197,
  183,181,227,17,180,111,25,97,20,30,112,244,44,202,166,71,210,33,74,126,38,229,69,135,
  53,58,216,186,148,106,64,13,93,242,201,166,170,158,244,172,222,20,147,81,48,225,70,90,
  112,156,24,184,238,126,157,48,31,166,52,40,178,147,221,162,152,219,160,230,20,110,44,200,
  2,162,209,196,11,36,60,193,18,7,246,132,59,126,185,142,154,197,58,233,76,151,193,76,
  232,8,62,158,171,150,107,109,163,83,121,85,133,185,93,180,34,179,94,68,215,103,149,133,
  210,57,220,96,209,15,216,91,174,139,31,203,231,88,62,66,31,154,66,192,145,118,246,58,
  172,207,166,83,230,211,167,102,234,172,61,80,39,41,183,39,234,69,173,168,148,148,214,215,
  241,239,114,122,117,70,235,27,140,159,120,100,127,29,189,252,9,79,132,171,181,97,120,53,
  44,224,136,132,102,91,87,87,15,79,52,82,199,126,168,189,22,93,178,189,116,73,11,152,
  142,232,1,142,104,199,29,143,158,150,33,115,118,196,92,65,137,105,42,55,205,238,108,233,
  250,94,153,193,70,208,52,228,147,140,34,253,42,155,8,19,178,68,126,36,192,208,64,122,
  175,234,16,145,122,175,189,171,90,23,166,19,70,89,218,165,76,71,165,3,114,84,58,16,
  67,52,12,234,60,188,206,128,252,72,13,164,188,165,142,31,158,247,118,103,129,211,86,146,
  222,73,21,148,188,155,182,96,245,50,152,175,51,199,164,45,207,195,105,67,187,151,106,58,
  30,78,117,25,220,199,169,42,12,200,110,113,77,247,152,87,229,169,235,197,159,241,115,170,
  43,213,158,78,91,20,14,19,150,39,37,114,31,165,185,29,189,50,187,248,221,125,213,44,
  119,177,215,111,23,162,214,89,151,249,58,189,205,246,117,138,207,29,17,166,162,239,248,62,
  237,164,251,62,109,164,251,62,157,216,14,143,56,11,96,41,16,185,165,210,206,219,160,73,
  0,17,161,12,51,3,27,83,232,154,8,48,46,173,244,106,68,191,7,20,62,129,126,116,
  132,230,67,137,177,18,61,24,40,81,196,167,59,237,210,187,40,10,88,88,0,21,148,78,
  189,219,69,120,234,19,79,106,213,16,194,57,93,236,66,212,159,207,15,159,175,56,78,186,
  219,197,221,157,125,129,39,106,82,56,203,41,56,181,42,74,61,116,17,97,216,145,130,184,
  123,112,224,224,13,93,156,195,3,132,17,160,26,101,39,57,134,246,4,190,161,189,56,238,
  244,46,251,228,193,241,115,175,127,13,182,21,67,45,228,84,33,145,124,153,135,109,41,114,
  121,160,70,35,162,83,192,244,92,124,201,218,196,79,209,178,72,187,156,186,147,167,5,132,
  173,42,132,86,172,67,2,141,164,99,58,50,100,172,8,127,200,228,123,89,252,24,233,173,
  222,99,245,15,75,73,243,16,205,88,74,233,78,55,230,142,112,49,139,129,34,244,13,24,
  39,162,17,131,150,188,216,161,219,29,161,178,141,122,96,29,38,32,107,201,147,228,254,62,
  9,147,114,173,52,136,224,207,96,239,52,177,151,205,137,245,75,134,0,190,98,67,246,37,
  145,100,145,120,90,146,64,35,151,162,167,71,220,175,221,58,103,147,53,27,1,71,71,253,
  7,120,20,120,215,45,183,24,243,222,50,87,49,235,84,69,210,233,236,254,155,236,56,106,
  179,70,194,226,220,70,25,99,98,81,186,51,7,27,135,251,101,148,61,226,0,95,103,85,
  33,160,100,19,223,84,92,82,158,34,194,5,53,251,181,76,70,102,230,107,69,118,190,138,
  242,82,56,8,57,33,243,21,184,192,92,42,87,95,147,109,54,247,5,137,18,46,164,145,
  92,35,110,119,242,213,231,62,2,57,219,76,164,149,165,237,88,194,206,13,134,234,243,47,
  200,6,110,93,222,179,225,226,39,168,194,100,29,103,225,186,124,0,216,237,5,235,158,55,
  183,178,116,92,92,153,127,97,232,243,14,122,191,3,3,185,29,109,167,118,28,117,207,118,
  206,17,227,230,146,254,156,128,59,217,33,127,237,178,190,29,47,168,215,177,236,179,8,237,
  165,28,223,111,207,223,216,225,28,137,171,189,23,144,2,81,102,16,62,110,239,108,99,225,
  41,56,161,104,132,135,48,249,177,68,31,183,234,99,153,62,190,169,143,21,10,135,99,231,
  37,189,216,45,91,236,11,94,236,79,188,216,151,188,216,127,138,98,111,103,23,123,98,139,
  253,153,23,251,11,47,118,169,196,203,93,66,79,78,76,76,24,97,135,7,191,238,48,136,
  159,78,91,209,157,24,50,184,24,85,59,11,94,214,133,183,54,25,199,28,150,132,96,196,
  164,109,57,108,110,91,180,101,103,176,216,216,175,160,11,245,84,103,250,207,158,105,234,37,
  43,210,60,44,5,91,52,142,68,243,5,170,73,196,235,1,153,156,227,248,115,129,155,159,
  172,233,12,63,218,100,103,188,148,110,52,52,199,75,66,142,38,185,53,191,61,87,114,129,
  62,167,162,144,149,117,234,228,204,103,21,46,2,22,155,229,135,155,35,178,189,193,49,101,
  8,240,28,188,57,6,143,204,104,169,54,137,71,166,143,204,53,50,131,96,72,32,61,24,
  254,201,220,52,253,84,2,77,176,97,74,213,25,53,100,103,84,189,196,205,9,165,232,59,
  227,54,219,144,240,73,164,217,157,162,224,88,112,130,176,80,186,45,37,255,105,4,146,187,
  214,87,76,44,129,137,184,100,106,190,80,243,232,45,114,131,15,235,101,111,29,137,216,158,
  16,106,100,45,191,78,193,196,48,158,177,72,41,42,165,108,142,110,128,117,192,239,133,106,
  242,175,115,65,195,141,199,100,46,5,41,37,232,168,232,110,1,178,127,228,211,95,117,176,
  250,185,221,235,201,238,233,59,7,172,184,63,211,17,137,27,213,80,137,176,84,67,155,186,
  26,93,80,83,196,11,72,19,31,210,37,128,205,25,231,250,205,156,53,180,4,98,242,192,
  57,220,73,185,240,203,104,13,121,17,191,64,221,177,38,137,72,87,147,204,241,74,100,145,
  1,174,188,85,89,55,2,180,190,116,232,217,47,69,119,103,87,141,243,112,142,184,200,181,
  74,237,232,96,95,184,169,62,132,159,232,185,224,172,70,160,202,230,206,254,219,181,202,134,
  15,188,33,224,58,242,193,39,189,7,218,31,1,180,182,14,249,213,214,206,238,222,206,186,
  15,191,165,172,107,48,11,240,210,171,148,190,191,121,124,84,89,219,245,97,95,69,117,199,
  213,163,131,61,23,196,24,245,14,219,106,214,111,128,178,25,166,244,59,225,55,171,237,184,
  199,130,136,15,51,212,104,119,212,247,208,56,92,238,173,190,0,155,54,130,142,65,188,199,
  196,104,27,142,111,173,36,188,234,20,71,190,152,99,104,168,125,141,189,124,42,213,199,158,
  210,75,158,96,130,149,171,208,134,182,212,110,204,180,212,6,157,58,131,25,248,10,174,106,
  210,225,19,188,153,136,63,231,193,138,216,29,110,230,201,87,155,173,20,44,11,239,39,203,
  132,188,172,188,179,72,87,174,195,126,158,191,226,111,180,47,47,225,210,125,246,35,126,182,
  242,53,87,202,145,47,243,244,2,126,2,67,39,185,196,203,9,4,107,80,166,49,8,197,
  83,96,209,3,81,78,192,72,67,43,161,46,180,156,251,122,252,181,156,101,181,117,76,42,
  93,148,235,135,132,174,138,223,24,40,11,120,29,48,234,3,9,133,22,122,118,78,121,153,
  127,220,88,177,39,123,53,3,179,58,101,223,232,201,85,145,122,206,168,13,234,195,17,211,
  25,247,222,242,65,45,132,85,141,143,150,62,72,51,158,221,85,251,2,106,142,121,112,255,
  71,251,18,247,204,92,10,145,73,62,227,147,242,27,202,220,21,162,238,35,235,107,41,159,
  75,96,41,77,188,127,196,29,232,169,75,29,247,102,62,151,74,175,114,133,204,154,42,113,
  187,7,153,119,243,123,234,74,33,127,51,106,90,71,166,126,239,123,234,201,162,165,51,42,
  163,44,120,183,135,221,204,119,85,232,17,231,89,21,93,157,126,79,13,105,100,126,70,45,
  251,241,4,202,52,157,239,154,164,228,161,49,107,146,96,226,8,87,156,143,171,7,218,26,
  151,206,18,175,34,6,123,13,253,218,175,130,65,73,213,81,25,1,67,71,184,63,216,60,
  218,114,139,247,138,150,84,99,150,106,32,120,67,165,4,34,162,31,181,148,43,238,252,162,
  241,253,7,15,129,120,156,144,103,73,77,254,185,37,167,116,240,15,36,191,174,35,113,83,
  134,67,42,227,253,158,110,54,70,226,64,183,22,25,73,93,120,7,165,207,160,111,152,143,
  190,11,176,62,96,59,14,192,92,40,246,112,161,216,123,101,68,232,123,250,118,73,178,242,
  189,115,92,112,85,46,196,200,113,10,152,90,231,252,155,56,42,27,99,230,235,191,90,184,
  187,134,179,250,138,145,146,147,203,231,107,225,229,84,189,222,147,71,33,113,113,82,254,225,
  234,202,3,235,30,126,201,231,238,50,207,219,183,111,221,125,205,63,144,52,253,78,251,221,
  108,250,60,64,226,16,239,124,95,62,59,80,235,24,168,117,59,80,235,122,160,110,48,80,
  235,231,229,27,59,80,230,39,213,101,6,74,168,143,121,179,72,62,240,239,239,171,142,15,
  187,8,94,28,233,213,223,250,142,213,1,158,151,232,122,113,25,193,87,158,243,68,178,23,
  93,168,209,91,199,47,53,122,249,186,113,11,186,135,112,240,214,251,231,122,65,249,246,212,
  109,196,139,132,97,178,43,234,138,81,77,104,238,212,73,177,192,105,104,80,245,93,79,66,
  119,82,118,21,183,24,54,205,60,4,132,170,198,198,137,240,6,196,45,179,75,59,163,164,
  196,50,117,116,175,111,227,126,82,216,79,59,27,55,122,54,142,48,27,55,231,229,35,237,
  129,246,137,236,80,254,72,187,98,170,146,203,31,218,25,71,82,96,83,189,174,15,52,210,
  94,132,199,25,17,107,170,191,218,9,251,5,113,225,167,34,170,69,169,57,187,110,244,56,
  17,36,66,164,179,230,226,193,32,5,17,123,220,29,182,73,81,89,243,209,67,234,169,184,
  59,106,250,131,187,16,234,30,147,248,202,96,21,29,18,22,205,169,216,58,16,214,151,240,
  18,90,190,225,184,232,57,33,160,10,47,178,192,132,169,174,41,142,64,105,174,237,153,27,
  119,114,206,241,170,84,213,195,70,143,41,154,64,221,218,64,30,228,54,179,43,153,43,204,
  87,114,166,111,21,158,186,232,112,140,58,19,155,114,242,101,83,57,237,178,169,13,246,123,
  194,28,70,139,27,84,183,41,182,249,111,112,218,42,249,244,155,54,105,210,21,130,171,166,
  146,221,5,53,249,235,125,61,184,105,42,181,225,224,183,9,209,220,95,197,191,159,38,73,
  9,224,210,75,196,38,78,73,255,5,42,88,205,100,242,139,130,189,3,125,30,185,34,241,
  54,57,73,164,88,152,175,40,182,136,13,192,149,107,107,255,131,243,49,222,215,127,155,144,
  167,52,18,39,240,16,175,195,100,93,80,117,20,184,17,84,14,199,50,158,70,207,248,153,
  26,6,37,174,195,183,132,171,12,244,248,200,85,48,226,90,212,11,253,5,68,252,104,159,
  213,207,89,134,190,33,6,67,123,173,94,35,161,203,88,197,46,51,225,99,76,36,85,67,
  159,116,85,50,86,130,70,45,199,175,122,229,24,59,148,178,159,197,164,254,49,198,31,155,
  77,7,176,103,225,55,85,133,110,129,227,68,129,40,70,148,101,115,94,117,100,78,153,235,
  215,9,220,139,151,99,62,134,82,221,5,54,66,122,4,225,202,64,160,69,44,122,45,162,
  108,148,123,106,156,122,133,152,84,79,218,69,227,94,90,238,24,136,166,198,214,125,117,204,
  30,16,75,188,9,98,253,55,234,141,86,92,38,71,223,84,168,120,83,84,59,114,73,121,
  127,134,76,7,170,220,144,55,0,39,26,179,231,195,86,86,105,66,55,241,198,234,210,228,
  117,241,197,155,251,251,248,108,73,254,190,45,240,218,150,157,218,32,95,37,4,85,39,190,
  40,151,200,83,214,209,142,215,200,1,50,252,216,235,247,121,91,208,87,183,221,235,18,145,
  9,218,38,255,98,195,233,247,178,252,253,205,233,196,74,74,39,2,66,74,118,36,160,34,
  68,1,101,29,47,24,143,230,105,85,15,89,213,87,172,234,139,89,85,211,127,87,228,101,
  214,84,61,212,85,95,233,170,47,30,30,204,149,135,7,179,249,23,14,38,253,94,145,191,
  175,157,222,189,200,24,88,114,144,63,107,112,3,42,79,148,246,96,87,95,60,220,213,203,
  140,174,10,2,16,171,7,113,65,66,173,250,200,172,42,165,40,192,93,176,228,178,221,214,
  237,52,186,103,74,24,55,133,24,178,71,206,229,30,40,171,169,10,235,241,142,12,254,150,
  142,172,252,249,142,188,155,100,116,36,81,214,187,73,90,71,186,127,75,71,94,252,249,142,
  124,202,234,72,162,172,79,169,29,185,250,83,148,185,253,0,101,174,253,165,148,185,253,87,
  81,230,246,131,59,236,230,111,163,204,237,239,165,204,51,201,227,195,61,250,250,111,33,143,
  237,191,159,60,62,220,213,219,63,181,134,39,15,45,226,234,95,186,136,39,127,217,42,158,
  60,60,18,123,127,219,50,158,252,61,235,248,17,125,186,248,183,44,228,201,191,97,37,63,
  162,179,155,158,186,16,59,89,200,22,50,157,223,238,201,126,143,50,73,53,110,68,132,19,
  141,148,91,123,101,40,74,122,26,20,205,89,250,121,168,212,150,55,68,112,225,131,247,171,
  249,211,134,136,246,1,69,235,45,213,180,72,218,135,38,226,146,230,11,171,173,78,216,232,
  224,134,118,218,128,153,81,212,109,74,227,136,35,35,115,71,20,150,161,227,121,246,232,111,
  238,165,211,128,21,209,128,90,211,105,193,245,191,179,5,194,56,27,109,184,113,219,176,246,
  239,108,195,242,134,186,218,221,223,95,185,205,88,23,186,137,74,113,176,173,212,5,127,90,
  90,126,25,170,123,92,163,36,181,244,86,126,122,249,242,133,78,108,177,196,159,116,226,132,
  37,154,236,77,155,248,79,147,253,146,37,154,236,3,150,104,178,119,85,34,90,244,34,52,
  224,146,6,95,177,42,255,105,225,75,26,94,99,240,159,45,124,89,195,111,24,252,23,11,
  95,209,240,175,182,122,211,206,91,157,231,229,242,47,166,71,85,150,104,48,247,88,162,233,
  209,5,235,166,110,210,203,165,95,204,207,101,251,19,14,94,77,155,126,54,109,222,100,5,
  252,98,115,253,98,115,253,83,163,30,89,212,159,75,22,206,126,178,106,77,167,175,109,179,
  127,102,165,174,152,159,75,75,54,151,105,214,90,137,17,178,138,162,218,236,86,239,18,63,
  139,122,232,16,248,207,228,87,95,138,99,41,236,20,180,78,51,110,216,49,43,98,103,86,
  17,43,133,172,123,37,47,98,119,86,17,47,10,89,247,53,94,196,198,99,139,200,184,51,
  241,178,246,103,149,245,75,225,193,43,16,47,235,120,86,89,16,228,61,120,13,225,133,29,
  36,38,182,157,53,177,91,62,234,114,38,234,27,31,117,37,19,245,155,143,250,34,19,245,
  115,162,173,147,76,220,147,68,99,179,113,183,19,173,205,198,125,155,104,110,54,238,251,89,
  39,130,22,245,225,160,133,68,47,38,115,98,112,26,61,104,145,192,18,38,99,54,134,130,
  199,32,28,121,242,215,31,60,249,235,56,197,17,186,181,219,52,130,63,18,144,146,52,122,
  4,193,95,191,224,31,237,103,253,115,60,166,4,67,252,101,29,57,253,59,59,226,88,96,
  171,134,213,11,254,145,15,25,174,56,245,135,248,193,90,246,229,63,219,50,201,10,136,182,
  221,36,218,246,251,127,182,109,134,69,16,205,187,74,52,239,233,67,140,66,37,141,81,56,
  76,99,20,118,210,24,133,221,52,70,97,35,141,81,216,79,99,20,142,103,51,10,7,15,
  48,10,91,15,48,10,111,30,96,20,190,165,48,10,159,211,24,133,147,52,70,97,59,141,
  81,120,251,175,50,10,239,31,207,40,156,126,23,163,240,229,187,24,133,223,75,242,105,234,
  75,35,45,176,182,52,172,148,111,105,77,29,85,155,150,51,133,90,55,59,194,132,194,78,
  184,155,209,74,177,17,24,91,153,78,15,206,191,255,61,117,145,26,66,44,15,82,183,242,
  167,188,242,167,141,25,17,196,109,197,240,201,111,170,130,82,20,94,67,167,70,195,55,25,
  28,11,216,60,32,14,108,191,76,100,46,18,231,215,177,191,149,130,97,63,26,157,13,41,
  250,21,47,237,172,79,166,145,66,24,56,13,222,54,162,231,249,63,174,23,10,249,63,206,
  97,72,255,199,217,253,31,197,194,234,243,43,102,172,220,212,199,24,234,149,198,123,99,146,
  57,162,157,103,99,97,100,201,44,205,62,120,132,172,45,34,156,144,189,11,143,29,246,22,
  58,219,245,145,50,180,42,149,203,90,245,24,233,241,77,220,32,153,102,61,226,72,120,171,
  27,226,254,13,197,133,33,221,190,163,104,238,28,74,213,248,88,17,239,130,82,23,162,127,
  95,162,80,147,214,84,133,126,207,157,205,65,31,125,97,25,63,161,225,140,190,196,12,101,
  149,94,191,190,52,160,239,78,61,20,154,240,191,235,47,109,80,100,52,207,7,194,139,208,
  0,199,93,121,192,223,208,164,146,203,211,6,153,185,136,242,7,20,100,6,22,123,114,169,
  127,27,165,47,63,181,6,146,51,175,28,20,137,199,240,67,105,121,100,188,247,147,208,160,
  184,182,126,180,115,2,55,172,251,59,91,7,149,189,170,239,152,164,39,200,187,154,254,177,
  141,97,171,180,199,81,68,157,52,64,198,76,163,124,183,47,125,52,2,214,23,19,86,40,
  99,34,97,142,36,157,174,76,229,218,67,51,135,248,199,86,39,125,81,202,78,20,234,154,
  183,24,39,121,11,82,186,72,64,133,47,217,25,112,228,150,26,213,138,63,25,233,200,78,
  225,208,196,120,26,49,189,152,188,25,79,178,178,33,175,147,20,54,213,219,71,94,240,48,
  154,207,248,188,60,100,150,71,67,190,87,68,216,28,124,75,191,89,20,49,149,111,70,122,
  10,45,143,88,94,135,16,152,236,80,163,70,112,139,201,0,47,213,77,219,10,119,3,199,
  153,27,56,198,6,38,133,29,185,107,97,181,192,212,25,148,174,134,179,189,59,82,153,129,
  106,214,85,99,76,222,183,199,45,51,12,122,115,178,185,36,75,184,161,125,83,166,78,13,
  209,138,145,137,111,70,47,240,48,247,16,166,208,208,201,232,233,41,208,15,228,61,238,204,
  166,215,244,169,64,3,123,105,28,171,72,237,214,171,11,104,138,72,170,246,39,195,6,241,
  43,49,81,22,21,109,86,97,139,112,198,180,147,126,43,69,43,255,132,71,224,224,215,18,
  25,129,152,96,70,206,29,7,37,146,107,239,252,199,31,63,146,163,6,172,217,81,100,3,
  30,47,190,164,240,157,67,27,28,110,188,240,50,208,99,207,55,210,8,27,105,40,60,252,
  168,105,168,47,44,149,85,167,225,46,154,116,176,198,171,115,175,231,194,185,220,220,52,135,
  132,105,136,127,99,176,77,211,143,118,76,138,159,96,101,40,154,34,58,208,150,234,17,251,
  99,219,248,119,130,179,130,230,75,13,187,81,222,193,242,237,38,169,194,192,157,60,12,6,
  174,152,242,76,91,114,242,227,232,35,212,143,225,229,15,94,171,155,230,197,137,150,50,158,
  243,95,227,232,233,111,145,63,118,40,94,20,166,185,194,199,178,98,219,60,237,26,120,71,
  145,108,92,107,168,14,231,179,113,48,167,52,91,153,10,224,220,185,60,231,201,112,196,160,
  249,122,130,64,210,182,154,154,247,211,38,155,138,130,145,213,166,241,171,144,19,58,189,210,
  85,62,92,48,160,99,65,86,229,220,158,51,177,168,208,31,185,70,44,125,28,99,249,172,
  31,236,29,238,236,146,133,234,218,209,113,149,166,59,207,112,201,134,116,23,174,106,199,224,
  158,161,31,68,102,161,221,188,48,86,129,166,25,41,152,32,81,118,98,110,78,121,178,122,
  190,89,169,28,84,194,92,41,204,255,209,92,40,60,151,135,212,80,100,50,148,150,148,147,
  97,40,4,237,48,28,84,86,19,2,211,113,60,128,231,191,245,58,41,230,44,192,223,217,
  199,133,161,252,67,11,215,182,75,109,1,28,173,125,215,19,17,83,61,25,215,248,82,199,
  218,49,59,233,236,35,233,118,211,146,152,230,133,154,119,78,80,1,242,43,247,49,248,168,
  157,202,209,10,93,58,87,24,121,137,34,148,70,231,5,168,116,62,13,84,162,116,129,252,
  49,152,155,206,157,123,171,56,174,25,195,12,233,242,67,219,98,40,93,101,99,126,33,52,
  138,185,197,133,84,249,53,54,22,87,167,198,172,66,42,208,26,75,10,161,230,202,141,39,
  122,78,199,227,26,41,178,232,17,182,167,121,126,246,170,99,138,103,176,24,198,64,207,9,
  13,253,185,133,246,194,92,94,106,235,203,133,137,65,211,131,159,162,234,173,112,202,185,233,
  92,33,124,68,17,115,11,241,194,92,86,9,98,68,155,82,31,115,205,246,183,93,203,11,
  170,128,213,177,59,1,157,146,102,51,241,229,101,187,209,22,182,251,205,142,36,6,237,168,
  217,41,222,176,61,31,140,41,229,150,167,196,148,242,141,165,232,53,51,39,189,151,118,116,
  13,212,50,240,40,198,81,161,80,39,206,221,205,97,1,73,128,72,149,110,131,71,198,19,
  54,173,185,0,255,142,197,191,241,84,174,27,189,228,40,198,132,206,18,200,18,203,115,254,
  178,178,123,188,70,228,80,53,175,93,52,126,89,214,225,134,102,29,218,111,27,202,9,205,
  234,220,63,172,207,22,56,12,16,230,237,41,206,106,224,33,125,40,221,244,129,82,131,208,
  216,18,141,177,126,122,81,214,3,129,91,196,121,81,6,65,200,255,62,44,164,117,96,88,
  227,177,44,173,14,24,141,105,219,170,189,129,231,232,169,195,91,5,172,156,211,142,240,197,
  114,33,191,52,230,24,25,167,85,84,119,118,4,180,39,21,65,76,112,140,68,17,21,199,
  184,118,4,155,253,55,199,71,155,140,103,196,193,95,30,129,103,28,233,163,110,40,203,145,
  60,163,52,202,65,25,35,226,200,135,146,153,239,83,212,95,43,73,218,218,61,88,59,130,
  242,250,209,178,224,194,151,201,98,45,9,92,17,192,149,116,224,11,1,164,165,138,3,52,
  146,10,201,18,47,232,40,214,52,148,173,18,237,49,236,170,240,2,168,49,72,129,151,140,
  199,146,58,104,191,15,237,170,202,181,159,16,137,103,51,214,116,30,25,139,189,73,183,58,
  64,204,13,90,177,68,153,251,215,163,5,55,21,251,24,241,81,211,16,137,203,34,168,101,
  111,134,177,112,207,158,127,190,127,188,87,171,30,239,215,118,41,8,69,245,57,41,74,81,
  1,147,158,200,15,199,134,14,230,198,78,197,195,132,63,202,84,204,234,225,193,145,95,168,
  110,85,54,46,217,26,36,241,169,233,217,89,214,15,14,42,27,148,41,246,112,42,155,235,
  71,96,213,55,215,188,118,84,160,8,191,6,190,47,181,45,194,237,131,151,225,144,124,50,
  164,98,191,221,220,219,241,144,223,34,124,87,122,47,245,40,171,139,67,98,176,213,100,101,
  13,185,159,77,143,124,122,54,54,64,42,31,226,143,28,189,77,31,96,111,153,60,88,84,
  86,1,217,99,233,231,180,67,170,179,50,18,162,86,126,250,98,93,223,221,17,230,40,181,
  195,221,53,184,4,209,5,18,37,22,17,151,225,240,62,230,13,193,117,20,214,70,143,202,
  181,104,210,192,47,209,171,178,12,56,38,205,62,107,209,243,255,231,44,247,199,248,124,222,
  216,128,45,188,202,159,253,113,253,71,179,248,252,124,161,240,250,249,85,215,30,148,31,26,
  124,107,155,214,244,107,65,163,38,11,236,200,48,190,142,53,115,195,33,158,159,199,196,78,
  128,151,139,153,198,150,164,212,157,90,81,198,163,22,252,161,85,56,16,153,16,177,27,12,
  199,255,241,60,138,73,110,46,215,104,77,122,159,115,115,255,51,154,67,176,163,81,238,34,
  142,41,192,38,217,211,213,201,179,88,238,24,12,158,128,146,85,108,92,111,22,255,15,197,
  153,86,154,153,136,148,129,216,102,212,236,205,225,16,132,122,238,232,109,101,115,179,232,178,
  54,235,117,233,174,76,217,106,230,172,201,220,220,194,120,1,119,19,115,128,96,144,98,57,
  24,45,140,238,63,6,100,65,87,207,77,122,195,62,142,202,78,191,63,168,225,180,28,142,
  255,24,45,224,84,248,99,52,255,71,30,255,96,221,32,161,141,95,17,254,19,140,47,254,
  150,9,130,255,94,165,164,253,177,128,255,225,15,165,221,97,198,70,127,84,207,23,86,11,
  83,20,147,86,39,70,140,139,153,250,205,212,153,108,213,130,73,141,59,16,170,249,46,216,
  192,166,219,187,180,225,194,137,51,126,101,190,40,30,47,14,56,120,151,233,217,21,251,199,
  153,110,248,57,214,233,220,25,14,221,33,142,222,243,57,103,85,87,14,118,119,55,55,132,
  23,155,218,206,254,198,230,41,112,135,214,185,28,51,183,110,218,163,255,163,53,156,4,123,
  84,52,95,83,227,2,61,19,65,56,66,207,6,27,115,203,71,32,209,147,195,35,208,86,
  54,30,85,161,120,34,120,20,166,36,53,143,108,224,163,145,85,11,30,198,111,63,106,140,
  218,143,234,125,251,145,67,217,126,252,48,77,30,213,188,201,163,154,55,121,100,243,38,137,
  230,125,180,60,138,193,163,203,175,176,243,157,91,29,47,68,31,127,212,44,233,91,156,44,
  181,67,28,243,59,85,208,248,143,161,151,69,185,240,247,50,237,109,110,236,208,217,148,153,
  13,225,27,7,115,36,182,227,185,118,15,222,179,44,184,205,200,11,18,93,57,15,134,184,
  26,202,227,205,196,163,58,92,223,194,165,113,119,148,132,156,84,247,248,5,242,146,93,44,
  80,220,153,20,60,245,97,26,61,56,2,155,137,87,47,191,128,55,107,213,157,117,121,63,
  27,80,245,107,109,84,226,199,194,66,229,111,51,210,183,134,41,233,136,157,197,91,213,173,
  57,52,79,6,113,16,102,103,171,201,34,195,1,53,91,226,144,199,50,106,115,2,73,180,
  247,138,218,203,219,181,119,176,177,9,54,109,171,66,23,129,131,125,222,132,218,195,77,208,
  185,119,55,101,238,240,42,179,33,62,170,104,206,13,53,167,219,176,205,121,179,187,185,191,
  65,76,194,222,241,238,209,206,225,238,239,24,174,163,73,26,124,135,68,4,215,105,160,181,
  141,13,222,141,175,15,119,195,100,221,135,177,237,92,120,67,157,128,164,241,2,203,142,245,
  192,197,98,154,166,236,98,167,203,151,241,73,222,74,35,67,210,73,84,150,153,5,230,67,
  186,44,121,12,33,112,68,132,164,101,28,76,139,203,184,172,33,60,157,218,129,119,48,58,
  139,59,239,69,168,135,165,231,249,149,121,35,174,20,63,6,253,235,252,50,201,69,151,150,
  224,254,32,16,200,178,202,176,23,0,11,193,17,194,152,137,201,170,137,3,82,220,156,16,
  203,157,140,219,200,159,72,20,107,27,57,33,120,118,188,193,147,163,118,207,1,124,39,194,
  230,137,233,101,165,43,254,78,162,154,248,59,136,190,138,191,173,232,86,252,109,70,35,241,
  183,22,225,18,60,36,247,70,35,37,246,85,44,11,234,134,93,47,76,122,225,174,183,51,
  234,40,127,202,184,127,43,231,138,116,245,229,16,72,201,62,226,202,93,22,234,135,245,107,
  217,156,61,229,206,125,53,223,141,206,204,165,153,246,46,158,12,104,31,136,82,164,84,153,
  118,118,224,227,236,175,237,113,156,125,186,209,214,210,175,245,240,223,204,158,23,186,68,167,
  62,146,225,223,223,94,239,37,175,247,82,214,91,8,169,199,96,50,48,196,127,89,253,144,
  125,207,146,177,168,18,142,171,155,130,177,199,29,9,50,213,253,245,77,33,76,137,139,23,
  117,8,150,193,213,187,152,111,214,142,214,223,146,243,19,23,73,8,178,211,49,113,181,220,
  61,168,40,124,229,37,38,81,236,206,190,168,219,22,108,17,83,138,182,216,25,133,239,145,
  211,164,204,60,123,7,149,195,183,42,15,236,67,183,250,87,228,201,230,178,239,181,105,235,
  96,59,13,105,243,102,176,108,17,129,84,219,60,61,92,86,152,144,211,187,133,128,234,40,
  144,164,42,46,84,146,165,12,132,185,133,137,4,116,212,245,221,205,43,110,132,54,119,189,
  159,192,88,59,176,224,11,156,225,9,132,55,199,123,135,22,69,250,182,75,32,237,227,25,
  114,109,55,5,77,122,14,18,175,23,25,57,106,7,111,126,197,89,81,61,92,51,171,202,
  228,86,17,147,103,102,199,132,109,111,238,59,249,201,10,150,152,119,34,97,137,166,98,5,
  227,102,186,190,185,135,76,108,92,17,9,124,4,113,87,114,244,247,118,170,85,136,206,216,
  32,246,218,163,254,120,216,31,220,122,35,185,191,83,61,56,170,28,28,254,158,64,76,142,
  186,193,181,229,138,96,17,13,220,13,18,216,235,187,112,76,178,14,73,89,10,114,69,7,
  39,206,206,85,171,28,28,111,191,197,93,188,154,146,127,63,125,66,109,102,127,106,219,67,
  132,3,25,53,98,225,249,201,203,181,83,217,217,216,172,174,111,130,64,164,102,56,106,181,
  27,159,83,27,203,114,214,142,222,238,172,255,230,54,119,52,136,27,228,180,63,145,175,122,
  184,185,126,188,187,86,73,162,10,146,144,137,47,105,66,50,23,137,35,68,228,190,236,156,
  144,180,108,238,87,119,142,216,212,13,51,39,33,101,232,33,148,173,119,82,145,247,54,143,
  214,118,93,100,17,225,61,185,124,118,15,223,174,121,72,111,235,163,86,10,214,219,181,170,
  38,99,34,220,143,88,231,226,204,101,152,194,158,94,44,116,98,238,18,200,137,250,57,190,
  109,198,56,115,118,211,102,180,5,121,72,198,28,189,221,68,8,72,127,130,8,63,123,177,
  203,60,105,163,93,31,28,127,181,184,68,49,16,107,135,78,65,1,113,71,153,99,234,49,
  214,232,22,199,37,184,60,143,38,183,58,143,197,97,244,215,169,228,128,215,208,103,168,140,
  32,241,12,140,28,233,108,14,166,67,200,121,70,69,198,117,38,131,225,81,92,158,197,210,
  88,149,137,97,165,18,90,158,217,35,179,186,136,68,142,228,158,112,166,139,237,8,51,111,
  14,110,114,3,242,252,124,65,232,252,46,110,10,169,118,38,136,19,106,51,81,46,118,146,
  124,243,18,56,241,214,5,184,184,89,20,57,181,148,90,98,86,210,50,206,56,36,210,75,
  77,27,168,140,236,105,199,0,47,212,61,4,116,97,62,254,204,179,33,163,56,135,142,164,
  20,236,22,145,66,103,120,185,46,149,209,165,121,232,25,180,39,89,78,218,248,165,100,75,
  156,102,78,73,246,44,51,69,112,204,244,243,45,173,132,100,183,18,153,178,143,189,212,2,
  249,161,231,23,234,102,78,61,62,120,153,222,225,161,75,75,228,72,30,42,78,41,41,75,
  97,156,50,255,242,110,171,24,201,145,96,207,225,3,150,110,146,210,187,16,137,7,156,163,
  74,178,147,115,60,183,220,88,35,139,136,67,85,237,67,7,79,140,238,200,99,164,216,221,
  67,98,173,209,65,146,134,37,15,108,7,247,248,235,146,135,121,124,178,228,97,44,39,48,
  150,61,140,149,4,198,138,194,144,62,208,143,191,122,8,210,35,59,137,170,36,26,27,50,
  118,171,217,93,147,143,77,155,27,250,140,38,255,166,137,107,91,245,183,157,253,125,123,105,
  227,190,106,61,246,135,174,92,71,107,149,237,205,163,42,71,86,195,255,240,220,137,2,228,
  188,56,5,164,205,139,192,21,195,94,77,105,152,8,79,242,186,196,14,34,214,182,218,17,
  162,219,28,87,164,3,252,13,121,165,230,222,138,17,191,155,34,230,253,201,66,215,15,142,
  247,143,88,89,44,143,58,108,251,19,184,112,170,162,232,38,59,102,15,142,223,144,162,211,
  142,157,5,10,253,234,97,109,209,213,157,227,24,209,231,166,244,11,229,51,83,74,14,250,
  32,246,220,66,71,161,224,233,29,79,243,113,111,34,94,226,189,242,118,62,108,66,243,96,
  115,255,120,237,200,114,152,120,0,20,207,146,144,13,93,196,35,62,46,230,230,10,65,240,
  193,155,77,61,65,16,158,213,225,89,174,5,13,34,102,131,234,101,59,216,94,171,224,245,
  149,92,17,110,108,30,226,25,86,122,227,213,140,122,210,134,213,227,215,55,79,54,43,85,
  188,246,36,50,207,37,93,131,42,167,159,169,48,230,54,212,135,39,188,134,250,8,95,179,
  242,174,228,52,11,230,3,165,226,17,36,58,195,186,118,9,202,193,23,253,126,7,47,20,
  7,195,113,139,4,113,136,147,222,32,176,114,11,232,9,87,160,186,99,221,148,138,6,185,
  174,126,69,78,225,1,48,171,8,69,245,156,130,68,11,29,7,192,143,40,71,74,97,80,
  78,210,215,40,223,113,78,73,89,238,87,147,32,57,152,9,128,112,203,234,54,137,168,174,
  223,27,66,91,202,234,3,17,225,180,12,203,217,25,86,82,51,172,100,101,208,7,149,159,
  233,69,110,44,15,59,63,163,117,57,233,157,56,185,66,74,33,13,59,67,157,212,188,41,
  185,86,120,174,100,139,205,65,144,172,141,78,14,161,230,95,206,0,202,112,243,188,100,72,
  68,179,100,167,127,135,144,244,47,150,1,138,219,36,226,155,195,47,54,100,237,177,119,237,
  132,55,80,12,50,40,209,218,246,230,195,66,67,108,209,70,18,122,180,254,175,72,21,27,
  15,136,27,147,128,1,1,152,4,130,30,163,142,79,232,172,220,220,133,66,204,198,209,91,
  32,9,143,246,250,169,99,6,254,219,77,34,255,54,131,122,238,72,205,177,183,118,138,103,
  162,67,129,44,95,67,22,230,138,165,185,255,43,22,245,197,162,64,255,28,55,51,4,126,
  192,251,13,199,158,223,226,255,38,185,104,134,132,242,127,151,248,148,4,32,230,217,203,19,
  75,131,243,176,236,17,158,6,161,248,19,95,118,36,171,224,179,41,24,36,254,32,235,223,
  166,51,165,171,255,87,138,251,95,45,197,37,63,202,41,88,80,216,61,250,115,194,94,33,
  124,72,145,145,254,155,197,175,255,85,50,231,191,66,22,32,239,146,240,37,52,251,245,49,
  235,218,79,25,103,188,135,254,151,203,1,112,117,104,182,211,30,213,182,43,107,27,59,206,
  131,218,35,69,6,255,213,151,90,40,43,9,43,0,152,147,197,77,49,127,22,9,42,74,
  74,119,101,7,199,38,159,177,63,125,175,117,50,108,131,206,206,204,132,177,222,49,163,23,
  55,112,215,60,193,184,244,213,77,136,13,226,230,58,169,226,156,96,128,14,180,196,34,51,
  219,166,58,230,103,103,55,135,254,127,225,133,60,253,206,252,93,215,226,152,59,168,126,66,
  14,200,153,216,145,199,118,81,36,200,71,254,60,46,102,134,182,200,200,2,235,166,57,150,
  232,225,20,52,235,48,110,65,243,133,111,168,13,140,236,102,197,182,165,63,168,127,153,176,
  89,60,56,92,123,119,44,167,11,173,202,10,164,17,192,170,108,174,163,236,156,14,132,199,
  238,35,226,186,69,137,158,3,111,88,103,194,50,73,222,117,196,164,29,130,171,116,219,36,
  38,138,248,73,10,101,32,30,125,24,30,53,37,243,234,70,38,38,80,85,174,11,83,147,
  38,226,33,198,244,171,174,126,245,9,70,145,4,9,214,151,41,117,245,171,30,65,123,184,
  78,41,248,75,102,216,105,26,67,79,84,180,181,235,232,163,209,56,130,155,131,92,60,250,
  241,35,252,185,159,53,237,109,208,94,65,219,189,57,155,172,67,154,96,84,88,170,137,48,
  162,127,113,211,38,210,97,90,232,58,58,67,186,20,42,218,209,119,194,25,180,211,88,157,
  195,124,117,234,183,168,35,175,45,91,96,106,85,42,80,173,42,10,139,184,23,15,26,98,
  6,181,44,37,163,36,93,169,19,199,129,103,101,29,81,56,98,94,177,229,157,239,153,253,
  77,192,72,49,46,27,186,188,1,90,247,73,127,209,239,52,156,221,126,147,26,161,190,241,
  149,85,144,139,168,82,210,219,244,168,50,183,113,204,49,44,250,204,170,218,67,213,73,233,
  149,167,149,235,175,148,75,105,213,135,96,17,11,221,133,58,133,86,94,184,92,232,35,176,
  35,44,170,17,11,170,8,106,8,106,44,15,209,74,128,40,99,123,26,2,221,206,109,122,
  109,213,176,42,25,199,99,29,195,139,133,178,163,190,37,71,255,137,180,61,177,91,72,248,
  82,50,17,152,72,236,97,163,130,34,90,3,224,158,229,213,109,80,74,207,87,64,36,87,
  112,58,154,222,26,152,138,12,152,89,152,141,24,69,141,4,57,250,172,181,6,111,153,3,
  249,245,252,177,114,188,222,140,47,38,87,69,184,232,104,124,150,125,17,150,18,202,94,125,
  11,170,135,214,22,78,91,252,222,10,139,95,132,150,28,37,173,129,47,36,108,35,13,182,
  39,97,219,209,150,50,22,14,158,34,202,162,250,249,33,218,208,63,219,99,10,55,126,74,
  81,26,208,196,81,170,53,222,45,58,135,80,67,191,105,227,100,193,119,10,95,240,99,138,
  123,66,38,111,112,170,160,187,7,163,54,219,55,168,76,235,113,152,43,164,98,96,33,96,
  146,49,165,54,92,226,187,104,68,235,227,130,108,93,137,13,196,232,198,99,153,182,23,204,
  233,35,0,113,20,54,18,166,179,202,144,69,148,12,66,46,250,35,171,41,192,74,98,49,
  119,178,182,187,179,129,187,137,234,138,70,73,237,178,135,43,236,160,53,93,206,209,178,9,
  115,31,23,142,133,121,33,64,6,66,50,58,9,161,129,161,76,170,244,28,205,77,14,147,
  67,208,109,177,117,222,137,127,99,229,47,57,183,45,204,252,86,147,22,193,137,18,48,179,
  208,173,124,42,44,191,239,239,63,72,11,112,156,19,167,52,51,229,83,252,58,198,225,91,
  191,234,245,71,112,166,128,72,21,195,73,79,48,141,97,123,172,67,3,83,57,219,110,172,
  177,59,240,71,225,83,32,128,16,220,132,221,169,31,95,75,192,63,104,248,229,116,10,79,
  17,42,96,174,218,156,98,187,58,41,216,172,55,194,154,233,219,136,230,26,113,59,34,216,
  129,210,47,225,162,227,70,250,248,98,145,34,83,162,80,220,48,223,37,235,194,103,6,10,
  149,78,71,142,76,126,27,176,54,165,132,163,148,18,142,100,9,59,81,172,130,100,96,73,
  106,197,82,90,10,157,78,220,145,157,88,151,206,29,196,194,215,54,8,42,124,70,5,118,
  80,183,41,21,238,168,72,44,249,157,40,107,137,253,86,2,251,176,163,3,108,224,26,61,
  236,59,37,37,130,26,106,234,34,155,175,199,217,210,28,89,146,154,94,213,221,169,227,11,
  136,137,145,69,50,45,222,200,145,28,107,39,63,191,150,16,142,219,58,21,250,45,190,213,
  126,127,192,70,53,143,218,93,12,243,146,91,225,173,252,228,43,10,177,110,68,154,187,142,
  162,61,145,42,20,237,247,106,240,226,241,91,154,199,33,237,108,70,206,128,240,108,164,140,
  226,180,231,25,185,227,28,208,84,133,160,76,58,32,170,25,26,41,50,108,245,13,147,197,
  124,123,136,208,55,16,196,235,201,19,238,149,72,48,31,219,110,195,38,43,144,120,61,15,
  143,188,126,184,120,162,155,42,152,164,235,20,216,105,189,54,216,51,209,43,200,75,77,92,
  96,133,45,46,242,162,35,17,215,222,31,30,29,186,154,130,43,55,227,130,179,80,221,234,
  20,226,88,53,16,181,159,176,73,195,114,187,18,205,229,5,216,225,83,96,103,158,11,84,
  198,150,51,201,143,44,197,93,25,133,169,137,7,147,152,125,41,194,204,23,82,102,95,131,
  166,51,39,57,115,248,3,114,23,172,166,64,155,65,112,111,73,50,14,122,53,30,75,31,
  181,228,36,167,64,14,94,18,189,113,170,96,77,127,92,5,191,54,104,70,220,42,166,193,
  175,15,120,227,218,171,153,141,138,89,79,110,210,18,119,114,115,225,152,156,80,168,170,246,
  253,61,253,125,59,148,127,223,51,207,29,155,181,172,232,159,23,163,64,134,241,250,173,161,
  226,114,209,224,52,200,115,205,196,108,210,1,249,207,74,189,111,139,72,237,198,174,10,70,
  24,119,20,96,86,32,232,233,10,231,154,146,133,23,144,182,121,204,213,64,149,34,225,90,
  164,173,161,242,217,65,194,16,10,185,221,176,160,11,250,148,144,221,122,247,2,75,216,194,
  58,50,65,66,15,91,125,40,220,27,216,128,62,37,228,168,47,158,193,21,96,220,39,127,
  33,148,142,53,208,107,194,151,8,207,116,139,218,116,75,14,213,87,22,124,79,60,138,89,
  168,124,36,155,11,200,157,71,122,47,8,178,1,185,106,204,234,108,138,239,185,64,24,81,
  143,88,93,226,123,46,168,42,33,146,78,151,66,37,164,15,40,4,30,75,23,223,142,213,
  85,254,198,44,157,142,160,119,55,116,180,131,20,173,82,244,211,240,227,228,235,211,187,155,
  233,71,102,127,148,191,9,142,130,157,224,56,216,10,78,228,250,217,136,142,233,165,81,48,
  166,38,2,243,211,232,6,39,105,218,24,194,239,164,130,120,179,101,1,206,68,173,30,211,
  59,95,123,216,239,9,209,133,136,118,245,225,207,151,254,236,217,147,27,245,96,152,81,17,
  195,32,30,90,70,144,182,121,158,6,31,10,224,170,219,240,229,223,166,48,245,58,50,220,
  214,112,21,223,237,46,168,133,138,232,38,219,248,46,106,158,221,136,67,154,2,209,153,173,
  97,226,103,229,105,199,8,111,75,55,135,26,152,103,136,176,104,2,46,75,120,246,204,231,
  34,5,19,162,185,143,17,56,72,134,29,204,145,81,182,241,58,19,192,246,82,200,116,90,
  193,156,54,245,158,83,62,89,193,137,111,75,125,31,203,111,21,245,61,232,254,62,9,147,
  155,49,13,34,132,59,65,101,28,197,99,123,115,139,181,63,47,68,10,94,27,195,209,71,
  118,109,204,137,89,30,168,8,18,150,85,187,135,185,156,134,217,80,33,160,57,226,138,236,
  115,63,14,190,140,131,95,199,193,23,186,36,189,147,43,185,17,71,167,189,179,119,112,84,
  24,71,13,207,34,237,203,152,146,220,163,77,58,229,3,242,77,2,247,198,67,45,171,242,
  251,197,212,147,25,219,238,211,88,2,211,142,220,155,2,124,39,42,54,232,38,128,15,174,
  79,20,32,75,180,8,94,186,190,68,159,200,1,162,224,189,63,73,51,59,39,102,28,108,
  224,200,88,144,188,176,209,59,50,145,235,145,20,135,73,84,41,252,4,218,241,24,91,216,
  11,129,46,46,204,193,141,132,188,161,71,3,158,126,130,235,34,118,13,118,67,112,29,171,
  159,68,226,130,109,2,224,98,242,65,34,136,199,237,160,35,81,244,115,120,240,94,2,213,
  211,54,5,105,188,134,127,27,17,207,93,112,97,193,64,102,48,111,206,193,174,76,240,20,
  184,131,119,50,153,189,19,7,53,213,28,246,66,6,146,44,210,248,19,91,176,143,169,178,
  111,193,175,209,37,154,78,243,180,138,132,152,18,236,139,41,82,54,145,224,188,140,126,197,
  43,31,210,175,136,44,217,39,74,164,28,32,69,188,90,225,247,103,252,230,143,75,72,122,
  31,237,131,234,60,185,113,95,163,131,225,56,58,137,37,128,63,40,7,245,148,116,243,246,
  27,252,158,4,242,183,178,224,215,232,74,130,221,103,215,160,63,78,1,240,231,213,224,104,
  28,29,72,12,231,197,46,104,186,233,78,109,29,57,183,236,181,53,184,118,147,76,49,187,
  110,58,127,55,13,182,198,209,103,89,133,247,46,23,236,26,0,111,106,67,150,197,222,136,
  130,223,228,10,84,175,159,65,139,54,167,121,230,164,89,27,91,4,122,210,12,198,50,193,
  88,36,142,130,245,49,228,227,229,27,35,14,143,155,32,38,159,148,153,237,253,253,39,108,
  141,211,10,223,115,98,119,208,237,125,157,118,30,151,162,203,224,170,227,232,78,242,146,59,
  27,225,187,192,94,225,66,121,98,4,246,246,134,20,225,237,200,185,212,131,122,121,215,120,
  16,51,101,76,139,12,218,172,182,33,28,152,113,106,131,234,64,243,100,186,75,104,0,249,
  18,164,136,171,195,155,52,33,182,220,253,76,212,11,52,246,21,152,99,40,108,5,250,165,
  49,188,25,7,206,171,35,18,158,61,219,42,214,228,139,128,122,4,82,231,99,96,31,54,
  195,227,113,224,61,115,34,137,114,58,138,124,201,140,194,182,82,161,114,69,61,141,233,191,
  42,132,122,70,87,219,197,36,44,125,142,87,63,233,96,157,242,180,145,200,169,126,19,61,
  109,171,208,44,58,155,6,67,234,65,120,50,14,36,17,13,175,227,64,114,31,225,246,56,
  176,134,238,248,226,60,72,144,180,2,15,79,3,65,113,195,15,112,196,169,168,109,216,137,
  3,69,105,195,247,184,139,104,186,17,98,185,121,244,52,220,69,197,150,150,134,239,226,32,
  77,105,8,57,137,102,91,141,32,233,80,171,50,9,82,149,132,50,208,49,17,158,78,80,
  38,226,179,103,184,236,48,64,145,132,75,117,28,133,156,202,135,53,196,234,100,148,40,220,
  195,208,27,242,26,238,7,14,173,13,223,7,134,90,134,39,113,192,233,109,56,28,7,73,
  58,27,214,89,42,39,121,225,239,129,61,37,194,24,173,112,149,103,194,205,128,17,216,240,
  42,112,233,112,248,107,144,65,127,195,62,28,159,17,129,13,15,2,135,0,135,71,10,224,
  52,163,137,68,75,117,195,142,253,52,249,174,109,26,167,182,225,46,174,154,140,204,134,159,
  3,143,234,134,91,226,50,106,155,182,27,48,82,27,54,198,129,124,252,11,21,189,166,248,
  61,189,177,18,169,224,164,239,144,222,164,96,155,55,70,148,224,173,127,121,234,107,82,29,
  254,22,24,58,29,182,198,129,161,208,225,21,166,64,186,76,64,69,234,87,32,77,233,78,
  176,55,104,145,224,11,98,249,122,175,23,119,10,129,54,106,251,160,128,226,219,130,153,133,
  92,39,150,24,58,201,34,89,99,181,247,170,20,149,98,81,184,105,218,64,21,100,23,171,
  65,75,154,159,237,42,100,15,98,179,184,230,117,239,20,58,75,181,168,158,101,90,77,225,
  242,100,139,236,153,161,237,41,100,158,204,70,209,179,56,123,175,6,147,39,91,108,207,188,
  108,168,6,141,39,167,32,115,243,177,186,159,101,63,57,150,89,230,97,191,123,89,43,169,
  61,74,216,131,253,42,179,185,233,169,248,158,153,87,127,156,200,201,49,108,17,190,109,215,
  145,202,232,164,123,232,94,223,154,60,75,122,199,92,147,173,142,206,96,83,147,168,172,77,
  215,30,126,74,179,210,173,174,118,189,140,28,108,51,39,141,172,182,84,62,15,194,178,184,
  163,189,171,208,83,71,152,217,200,254,166,86,168,74,177,56,174,18,22,142,226,237,98,221,
  222,27,149,238,58,57,85,71,80,241,125,141,174,76,128,124,108,185,195,3,174,153,21,222,
  56,138,90,1,87,190,242,96,234,65,210,43,83,240,18,207,158,37,211,138,144,224,116,201,
  197,36,242,189,8,140,198,84,72,183,51,41,31,74,47,112,242,21,189,57,65,236,173,223,
  160,154,142,23,159,39,79,54,2,169,40,142,246,32,65,50,116,74,97,156,160,207,158,109,
  160,200,45,153,16,48,69,170,208,191,165,161,224,27,95,135,173,68,194,129,196,24,49,249,
  232,128,229,253,51,210,32,155,156,45,249,241,165,114,144,186,122,150,65,232,131,151,34,251,
  159,46,219,12,113,35,75,106,8,133,183,56,68,149,173,153,24,254,42,125,240,203,113,218,
  251,115,232,12,75,66,236,17,112,179,171,240,17,114,146,128,27,167,133,15,10,75,2,102,
  138,22,62,36,48,9,146,54,96,149,113,144,98,100,182,6,118,146,57,17,13,143,138,163,
  137,14,180,16,112,167,161,128,52,177,118,26,210,139,61,195,96,46,55,129,35,150,53,131,
  90,79,162,84,52,62,210,96,228,201,82,129,245,55,67,115,157,128,2,111,168,18,24,142,
  117,229,9,120,11,31,188,26,215,105,167,236,162,22,188,14,82,250,106,17,89,143,211,50,
  36,92,99,234,1,72,67,246,61,112,170,14,63,6,85,123,251,68,150,25,208,192,85,28,
  148,200,44,33,72,234,9,134,39,172,86,215,195,38,188,231,82,15,197,239,32,197,215,166,
  132,243,148,192,232,151,209,61,86,255,14,124,61,201,144,57,78,131,132,86,164,61,123,182,
  99,252,21,5,142,91,53,7,91,198,78,97,62,184,215,233,234,236,235,34,10,62,82,114,
  145,237,17,7,40,242,234,185,177,87,252,166,85,80,35,199,252,113,144,173,227,40,120,56,
  151,131,123,92,61,14,207,231,215,151,212,21,13,111,82,20,72,3,166,240,42,72,97,147,
  170,107,246,2,163,224,106,83,127,141,3,79,201,78,8,24,236,231,107,60,246,54,179,193,
  247,247,165,32,69,63,135,196,10,201,212,32,211,11,84,56,22,243,97,101,50,197,134,135,
  161,6,76,168,253,64,76,190,249,6,78,180,147,158,184,73,241,36,225,123,59,204,39,74,
  239,106,152,58,205,110,198,5,175,108,235,154,27,101,62,78,217,32,84,5,252,246,182,66,
  186,143,2,161,38,69,61,53,21,108,2,101,73,233,140,18,241,175,171,151,122,186,231,164,
  165,227,129,84,63,63,94,140,153,158,118,212,17,85,65,128,206,146,151,117,242,178,147,188,
  162,147,225,22,187,99,158,100,47,88,56,157,46,189,19,9,213,12,242,38,14,129,249,77,
  81,139,175,86,143,164,227,112,155,2,45,22,147,150,38,129,130,150,134,11,246,5,81,56,
  175,141,12,203,250,186,213,15,233,59,228,195,220,192,11,170,172,29,86,170,2,157,237,216,
  248,7,153,66,44,226,64,46,243,71,1,4,240,215,242,143,42,38,41,2,42,36,26,238,
  205,5,193,133,30,29,243,19,123,73,239,101,133,187,27,153,241,136,63,239,152,180,164,118,
  171,1,89,161,79,34,145,203,123,24,80,220,130,217,183,229,134,89,162,189,240,114,204,190,
  159,98,174,188,44,141,221,113,89,106,226,74,203,155,203,239,174,188,165,206,69,149,1,220,
  75,41,111,161,123,1,101,16,247,178,153,6,216,79,109,119,198,245,145,97,248,87,196,116,
  144,123,27,100,56,222,117,207,135,100,86,235,92,224,82,210,211,139,76,189,149,49,132,196,
  205,139,195,178,186,160,228,43,44,69,93,19,120,205,46,55,205,103,217,231,33,125,152,57,
  0,124,168,227,151,222,73,182,78,232,157,100,238,183,221,45,198,122,162,79,79,23,94,208,
  29,16,115,235,238,164,123,206,228,83,91,171,93,156,167,182,57,21,152,116,143,158,222,206,
  71,65,141,91,119,7,139,241,112,206,34,100,108,18,95,12,236,197,194,41,198,243,187,158,
  128,57,158,212,25,125,96,28,1,15,130,45,169,99,157,232,7,241,112,107,157,78,158,168,
  168,149,225,35,104,154,226,239,242,37,23,178,46,175,200,6,188,228,130,133,240,159,129,151,
  9,44,229,234,44,117,133,82,37,61,101,169,47,40,53,77,226,205,112,126,114,112,184,156,
  155,33,189,36,36,67,99,24,224,159,162,177,150,126,48,208,207,4,50,114,79,6,248,133,
  0,92,116,192,59,95,178,64,41,102,224,192,37,11,36,222,128,131,150,57,104,217,1,173,
  112,208,138,3,122,97,65,214,144,205,130,197,216,88,98,205,65,47,77,239,72,144,203,33,
  98,72,244,251,16,7,136,1,177,146,117,14,18,67,226,137,217,249,156,191,224,101,250,11,
  102,89,140,25,19,95,115,152,24,50,239,93,130,195,217,184,25,207,48,22,186,98,150,126,
  29,107,110,244,25,66,41,127,137,95,246,19,107,91,27,238,187,75,154,73,86,252,229,156,
  46,174,240,151,119,138,236,194,95,235,90,142,225,175,111,78,188,253,101,237,122,197,113,87,
  54,147,50,248,43,59,121,27,241,151,184,127,215,75,44,115,118,123,73,172,114,115,131,73,
  44,114,239,22,147,88,233,230,178,153,88,232,252,212,76,44,115,113,140,39,86,184,124,5,
  73,44,111,35,175,75,174,239,196,77,49,177,206,179,47,147,137,69,237,61,171,36,22,118,
  154,177,95,98,117,167,137,175,102,46,114,22,123,221,222,23,140,18,84,176,67,23,135,35,
  153,126,76,138,54,71,231,229,157,232,237,4,4,18,167,13,180,219,149,65,220,72,169,206,
  239,224,157,94,39,105,62,126,135,57,152,150,103,135,84,247,158,8,101,173,35,195,239,239,
  88,237,163,133,133,29,171,58,25,66,119,91,232,54,146,30,36,216,125,10,40,212,48,55,
  136,137,208,211,132,122,155,80,226,182,26,150,212,27,180,125,113,241,198,213,215,213,125,108,
  200,187,236,193,37,41,234,52,208,175,168,113,214,80,178,136,69,132,11,69,13,253,65,158,
  202,87,90,186,55,70,3,91,94,117,132,158,56,174,13,182,206,61,170,179,95,84,122,198,
  55,108,108,215,243,4,48,58,181,42,184,198,157,171,10,118,27,88,253,116,115,133,236,6,
  76,37,63,172,4,245,134,136,39,165,77,18,170,129,210,77,215,9,23,58,129,169,223,134,
  123,218,214,96,20,54,2,213,138,112,157,181,252,136,98,134,201,152,96,52,210,239,17,188,
  212,9,122,50,166,88,117,198,67,58,93,59,235,172,119,177,141,100,39,20,153,40,186,166,
  66,238,115,245,218,62,197,154,106,139,249,170,35,82,29,108,238,108,25,61,42,163,173,199,
  154,151,62,34,228,160,67,80,89,54,162,168,70,29,22,132,9,77,119,154,173,71,151,228,
  246,99,26,190,144,158,117,105,78,224,245,92,42,100,133,35,51,12,67,54,12,215,53,47,
  170,205,21,174,53,131,131,33,6,242,9,133,23,181,159,171,28,182,200,33,97,91,89,49,
  216,108,236,123,213,129,46,58,176,176,109,212,161,161,25,38,114,178,239,85,7,186,232,192,
  88,206,19,28,46,245,222,216,201,173,210,86,19,88,139,9,28,148,244,77,228,253,6,236,
  111,128,127,67,138,168,14,186,106,54,6,78,243,63,60,78,186,141,248,111,177,157,222,198,
  86,211,44,105,138,145,11,129,151,136,200,172,194,122,82,124,53,190,128,4,92,237,126,252,
  236,217,159,35,243,147,69,63,202,183,100,217,77,40,70,170,65,104,249,74,120,88,240,205,
  5,210,113,108,185,239,12,50,29,36,157,53,182,159,111,5,77,120,11,186,37,47,247,178,
  232,107,132,122,179,145,10,175,89,164,194,235,8,193,59,195,22,105,17,246,5,155,11,125,
  29,173,195,27,54,3,61,163,97,45,240,39,151,218,29,176,41,184,13,248,176,182,248,32,
  7,223,136,244,16,42,236,139,2,106,74,116,13,33,209,53,169,186,139,170,175,139,178,242,
  168,133,159,186,250,136,210,117,173,81,141,125,168,38,68,162,9,215,108,29,192,92,229,154,
  87,28,185,205,184,46,126,139,186,58,67,116,137,87,69,168,217,95,179,200,57,124,228,130,
  235,194,221,181,203,59,41,9,35,252,223,47,118,181,129,164,59,218,229,154,167,225,183,170,
  98,181,86,10,97,205,215,136,40,233,80,194,21,50,86,84,191,216,198,72,155,199,135,171,
  155,244,70,173,246,229,56,127,157,81,35,131,199,236,131,69,54,162,42,16,162,90,203,210,
  151,72,238,57,130,18,113,190,117,127,127,93,43,152,5,77,144,158,132,52,17,228,186,89,
  48,235,155,32,35,14,177,165,227,24,52,33,135,91,176,108,104,70,250,149,165,220,122,213,
  44,183,116,248,191,26,150,108,75,136,26,107,180,76,84,64,11,17,10,187,44,82,132,6,
  87,77,47,28,245,101,214,142,250,54,203,71,195,197,204,211,135,142,207,119,167,244,85,98,
  246,42,45,136,59,27,58,80,248,118,175,141,152,178,1,77,82,216,9,212,176,225,0,132,
  160,177,141,164,65,64,157,13,39,140,254,175,61,120,12,178,32,208,242,56,162,112,227,122,
  147,14,217,38,149,6,26,173,166,58,239,122,1,162,19,34,14,195,232,53,34,33,202,177,
  99,56,38,40,117,33,132,249,58,194,37,7,117,126,186,102,156,111,116,178,141,205,57,198,
  227,120,172,219,126,220,105,81,179,64,55,86,109,99,105,10,43,131,176,91,121,173,38,232,
  50,93,42,154,235,32,192,99,25,152,94,68,254,157,211,98,147,185,80,124,110,216,231,50,
  149,140,184,146,230,13,77,4,71,95,11,196,203,135,248,125,48,158,202,8,233,101,89,152,
  22,134,136,108,250,185,84,229,154,85,74,160,95,8,160,164,14,179,28,72,163,70,248,53,
  128,68,171,123,49,172,203,47,112,224,245,219,176,228,84,104,69,55,105,53,102,213,144,86,
  18,137,156,70,3,220,65,226,236,126,143,62,223,174,243,34,105,65,247,154,235,89,163,225,
  72,171,68,129,78,131,188,198,182,234,157,75,233,20,205,126,235,16,47,148,160,138,158,186,
  51,27,225,213,102,202,22,76,229,191,98,193,200,75,164,17,132,134,75,234,69,240,77,187,
  78,51,41,63,228,237,213,73,170,224,162,61,25,25,116,108,17,17,207,147,250,63,153,181,
  210,254,254,250,188,133,246,183,84,168,18,215,133,175,146,125,136,173,12,158,76,218,162,148,
  120,101,230,66,32,11,208,195,26,143,140,190,227,176,194,152,75,244,71,9,43,87,151,195,
  82,97,17,143,45,94,210,66,94,168,183,174,46,41,176,254,109,151,217,46,11,132,68,141,
  95,135,127,196,136,214,30,216,178,59,229,87,35,20,58,24,173,240,14,111,245,187,210,2,
  101,113,41,96,239,241,54,81,220,204,237,167,208,34,48,95,90,87,192,166,144,118,128,253,
  146,178,95,253,36,47,19,54,252,87,127,13,56,180,111,251,58,169,106,158,240,121,138,120,
  172,151,159,252,69,126,113,105,26,64,85,134,100,86,225,89,9,46,11,74,231,116,37,187,
  136,67,226,70,39,61,245,71,150,231,124,144,74,165,251,77,158,106,156,36,132,7,111,224,
  174,71,105,108,152,188,79,91,116,154,106,67,70,186,169,10,61,211,127,181,236,93,127,179,
  54,115,165,6,15,93,151,164,103,133,255,222,61,90,95,146,230,87,44,101,89,166,136,41,
  38,100,166,93,225,125,170,202,156,20,93,29,77,57,253,157,165,90,81,242,39,11,20,94,
  115,59,13,108,137,198,171,95,202,13,176,56,100,42,9,184,60,170,5,109,149,252,228,72,
  6,224,70,108,44,250,91,135,226,178,250,193,226,83,230,27,114,217,79,112,161,24,224,191,
  22,109,53,85,199,22,126,111,161,142,45,91,199,217,214,185,224,26,196,74,145,181,224,166,
  17,144,137,246,45,254,235,226,191,75,252,119,141,255,42,248,175,138,255,46,240,223,30,254,
  91,199,127,55,248,239,8,255,237,80,235,37,75,183,83,43,240,10,225,200,66,203,58,80,
  247,137,168,91,217,5,54,80,57,204,2,55,148,89,216,83,252,106,107,130,37,60,86,232,
  19,145,236,237,68,148,50,156,11,27,74,0,72,10,104,242,23,237,126,176,221,252,211,40,
  221,75,13,112,97,245,10,51,188,40,13,39,244,211,197,99,132,18,161,221,223,167,101,41,
  4,164,252,182,38,119,153,152,207,194,100,1,70,114,195,121,120,82,160,31,87,248,209,162,
  31,23,243,79,101,172,84,209,242,246,200,78,190,229,115,79,49,118,167,152,149,83,54,43,
  167,231,100,110,89,133,150,90,220,60,137,133,1,48,53,4,35,101,163,171,3,41,120,90,
  40,239,44,44,76,121,21,250,248,147,163,124,170,12,21,55,68,108,216,83,165,37,216,128,
  216,61,175,198,189,80,84,2,215,91,170,174,78,21,153,89,160,142,90,210,91,80,190,58,
  244,144,144,143,142,88,151,14,43,62,239,204,137,222,177,233,52,96,58,109,0,185,192,31,
  155,104,15,34,128,122,230,195,34,200,99,9,192,161,248,97,1,234,128,146,61,122,71,147,
  68,159,182,79,72,147,70,115,221,88,40,95,32,40,61,89,89,91,245,172,179,26,78,166,
  49,79,65,145,148,216,30,203,59,220,56,18,69,144,175,108,178,149,20,207,144,121,187,196,
  133,197,226,218,248,85,101,140,127,197,12,122,4,244,236,118,97,13,87,218,119,210,152,147,
  82,96,103,232,52,65,81,85,131,88,107,200,132,141,250,184,126,134,164,242,237,66,4,205,
  58,88,125,79,69,174,51,28,166,167,65,211,155,119,159,207,249,255,213,252,71,102,234,49,
  176,137,227,229,172,171,230,56,237,64,34,96,59,3,40,230,15,112,70,4,40,41,184,16,
  83,193,50,16,210,105,208,245,183,162,62,8,18,115,113,106,84,53,137,250,110,13,251,93,
  89,153,150,168,35,187,172,234,125,127,216,193,213,217,153,184,237,196,148,61,37,12,77,45,
  163,15,2,93,220,81,100,68,199,70,127,132,2,241,18,215,137,9,145,221,90,18,240,249,
  252,210,226,134,193,40,136,114,233,50,66,212,152,254,210,186,69,175,206,174,209,223,178,187,
  28,36,117,150,196,184,231,40,90,158,221,208,16,146,21,232,13,68,42,239,148,169,170,232,
  113,35,70,213,238,50,123,246,236,136,28,105,56,69,136,137,184,62,23,211,44,102,32,185,
  46,255,75,23,163,101,85,168,253,146,210,112,238,133,82,177,252,214,177,116,174,189,229,227,
  92,207,82,150,208,195,75,194,92,217,196,25,191,1,205,108,252,158,47,254,36,14,123,5,
  151,87,56,197,4,108,40,27,117,129,67,115,160,249,164,179,10,45,240,138,215,66,123,233,
  248,107,168,13,91,196,236,244,247,151,224,255,62,154,100,64,246,234,4,168,244,249,88,36,
  151,134,62,198,22,71,184,4,188,199,149,126,207,46,213,74,114,121,83,74,110,39,146,197,
  214,185,76,210,176,61,65,195,4,42,129,79,131,75,111,114,61,129,67,202,2,212,226,134,
  217,51,44,86,34,147,68,104,108,150,148,150,167,39,116,172,207,170,212,180,42,154,54,173,
  136,56,163,242,77,104,238,0,78,70,21,55,86,19,193,202,107,210,55,228,92,65,74,50,
  243,118,233,18,211,31,93,142,139,248,91,219,218,37,127,215,75,129,3,93,118,161,203,16,
  62,167,231,126,187,182,187,149,153,89,0,151,5,115,161,238,95,103,165,243,104,194,62,151,
  206,163,1,251,92,62,143,90,101,249,218,217,163,78,181,202,120,228,52,87,81,8,62,32,
  28,61,46,38,174,163,0,116,9,192,174,164,72,186,164,36,123,45,69,202,53,165,184,87,
  83,164,86,40,213,94,79,145,82,165,20,231,138,138,196,154,74,76,94,83,1,188,80,64,
  126,85,69,242,158,46,200,94,87,145,186,206,82,233,18,68,78,43,84,18,187,9,33,117,
  167,32,207,13,99,26,0,9,127,47,197,30,0,50,250,30,215,242,143,174,217,132,232,180,
  138,222,5,58,225,82,173,39,253,93,229,12,152,78,172,121,108,96,70,58,109,31,13,186,
  77,97,228,56,44,193,84,104,224,69,6,59,242,0,156,215,125,225,110,117,157,188,151,32,
  12,153,16,94,218,158,115,50,233,212,117,255,188,242,1,236,128,54,160,133,155,197,35,23,
  108,178,221,32,125,198,21,57,58,146,112,182,52,34,120,147,177,219,2,139,34,101,79,96,
  77,56,27,2,211,205,119,3,150,136,191,21,176,66,248,62,192,122,240,54,1,38,61,99,
  7,96,220,19,203,31,163,231,175,125,140,144,179,240,209,245,227,100,215,122,69,37,140,138,
  14,193,223,23,248,107,83,35,152,72,154,171,174,241,65,242,98,62,225,92,226,78,79,60,
  70,153,155,137,188,179,155,203,247,245,171,74,249,90,95,190,171,184,124,95,139,247,147,106,
  226,202,8,55,158,226,150,49,56,47,95,216,161,206,224,83,171,46,159,202,51,136,39,18,
  186,128,155,33,204,227,81,109,192,78,153,106,230,181,133,218,192,217,235,214,119,180,101,148,
  137,54,22,42,75,217,45,31,77,46,242,163,199,116,166,229,117,198,99,248,197,72,18,183,
  92,163,230,63,192,241,39,70,210,224,67,209,178,115,43,177,95,80,173,255,141,35,81,243,
  70,34,133,119,165,209,48,236,228,237,95,58,34,117,8,152,113,164,130,169,163,200,241,146,
  193,240,114,15,153,106,217,173,204,3,246,97,88,111,64,183,123,44,29,188,14,169,14,151,
  101,174,122,44,243,69,146,101,174,186,44,51,47,193,105,102,221,203,237,3,111,189,17,244,
  121,107,26,62,201,173,53,255,210,177,107,122,245,166,178,125,84,185,96,199,186,127,11,81,
  160,139,179,121,114,69,153,80,5,232,7,226,47,137,93,240,164,42,156,19,133,61,246,142,
  52,105,186,2,126,33,240,159,161,120,1,53,138,129,98,168,161,71,240,39,21,48,148,40,
  182,85,112,244,40,200,95,103,34,189,67,233,163,100,122,131,84,63,138,162,79,240,46,233,
  188,167,27,0,117,54,31,7,45,233,39,117,16,221,9,67,140,209,218,112,136,215,192,88,
  61,181,168,207,158,116,75,96,84,244,100,42,156,186,136,46,74,145,182,204,30,186,94,27,
  184,143,146,16,90,82,138,139,134,226,215,88,8,164,213,44,168,39,109,57,240,3,57,25,
  202,250,177,193,191,196,4,77,196,211,183,72,8,235,226,183,146,159,247,45,192,180,52,236,
  176,105,220,240,222,105,18,15,225,49,28,199,14,181,46,93,93,221,62,64,146,250,250,41,
  188,206,158,194,165,139,66,177,52,2,185,63,71,1,116,184,240,216,61,124,29,213,205,107,
  56,71,171,203,169,130,142,88,8,79,237,103,195,115,87,85,140,244,117,50,30,196,99,243,
  32,222,147,207,90,251,181,232,163,104,73,183,78,86,62,185,187,31,127,128,115,114,189,43,
  224,30,157,60,162,231,77,176,183,32,183,84,44,229,10,229,31,167,31,131,99,100,77,134,
  143,147,19,14,91,176,209,168,252,35,11,23,176,156,27,198,163,126,103,34,66,5,24,128,
  184,6,229,228,101,180,252,163,215,16,225,74,82,161,200,10,70,104,144,248,206,231,78,170,
  123,181,234,218,222,225,238,102,149,218,243,131,68,235,198,117,225,211,189,88,50,73,163,47,
  19,168,60,52,107,41,160,201,87,105,237,139,100,93,254,171,72,244,112,149,240,114,97,110,
  25,255,62,207,229,13,120,81,247,159,149,80,31,142,179,11,16,25,8,29,158,158,243,170,
  47,109,213,12,252,120,165,243,209,199,194,66,78,244,219,148,125,112,121,137,245,0,108,93,
  205,2,176,230,77,179,81,236,15,42,12,220,219,131,202,206,135,131,125,4,200,65,128,128,
  106,21,128,31,196,152,147,60,66,168,185,202,201,52,46,214,243,124,158,2,180,204,120,177,
  239,15,155,197,155,91,212,68,249,243,166,17,129,232,79,1,181,203,201,194,207,231,108,70,
  115,133,226,240,138,218,243,131,24,230,133,200,169,185,120,35,64,206,76,248,40,183,40,218,
  75,88,240,10,241,49,68,169,255,160,115,128,74,151,131,38,30,66,190,171,167,232,95,96,
  199,124,102,79,221,142,82,141,233,253,19,109,153,183,24,50,196,222,143,63,76,127,252,65,
  173,70,241,231,185,89,4,63,254,224,173,86,231,147,227,169,181,61,110,214,154,241,87,129,
  136,103,44,236,157,27,213,17,39,227,162,172,103,94,254,41,136,245,235,132,45,208,219,156,
  224,129,46,53,144,37,217,13,127,80,139,206,196,99,94,126,73,114,22,242,99,209,249,42,
  5,75,252,99,209,249,2,144,127,44,46,21,206,131,45,83,170,143,253,80,222,96,70,54,
  60,216,54,245,139,227,151,161,122,131,124,223,144,63,44,165,126,163,156,199,74,82,253,255,
  82,247,238,109,105,44,205,222,240,223,207,254,20,46,175,236,108,88,153,184,196,179,16,150,
  151,7,60,5,197,8,106,18,223,188,108,20,68,20,65,97,68,141,241,187,191,191,170,234,
  227,204,0,154,117,239,251,217,239,149,181,100,166,15,53,221,213,213,213,213,213,213,85,226,
  224,246,1,78,255,243,202,120,160,167,31,228,212,18,215,121,133,15,215,186,202,163,108,183,
  11,135,178,88,145,238,243,20,47,240,81,59,17,32,5,26,214,194,211,135,214,143,44,174,
  27,159,238,54,126,100,31,90,193,105,189,243,35,91,239,188,144,107,89,178,109,234,164,158,
  181,139,179,103,135,161,101,151,0,80,27,53,63,59,244,155,125,30,212,218,48,237,98,115,
  175,192,18,165,73,23,27,139,64,40,87,167,206,193,63,175,231,107,109,191,26,245,181,118,
  84,125,129,84,5,227,66,49,92,79,231,234,250,202,233,84,132,181,228,51,57,49,102,163,
  111,93,55,96,185,214,119,156,154,167,108,176,1,30,143,239,13,62,11,222,36,106,157,157,
  225,245,62,117,138,1,194,127,16,61,103,213,47,254,206,226,247,71,58,152,85,254,59,159,
  24,252,83,35,85,133,100,129,141,35,121,11,22,23,234,234,102,60,187,244,167,247,218,125,
  216,61,98,221,60,249,167,16,119,225,141,70,189,175,211,50,142,103,241,146,248,172,188,200,
  155,36,129,41,22,149,214,169,249,94,176,30,136,125,252,141,249,30,93,172,253,245,235,198,
  253,158,220,181,189,241,190,39,197,246,180,80,70,22,245,178,252,230,108,51,160,118,234,66,
  111,100,92,159,218,131,250,137,131,245,205,114,247,194,106,46,38,160,99,155,56,195,117,140,
  9,49,209,174,79,77,28,145,211,83,46,104,10,89,239,167,94,103,5,149,149,68,247,153,
  59,146,186,74,158,30,57,134,198,38,174,121,145,191,76,55,29,33,31,161,13,46,194,96,
  148,46,248,108,106,135,155,185,77,26,243,53,229,248,42,181,13,35,191,205,81,78,56,69,
  187,105,139,136,134,29,32,214,249,94,54,31,227,99,202,102,135,150,200,4,252,47,254,25,
  148,160,27,101,41,56,38,12,184,81,101,220,68,198,137,190,36,102,4,1,199,249,11,178,
  135,54,67,126,12,223,250,36,211,115,243,82,102,208,55,210,207,27,198,186,146,28,171,18,
  177,210,145,57,19,173,205,74,175,216,103,58,171,47,224,14,65,106,43,255,247,150,79,119,
  232,143,83,46,146,245,98,207,98,55,32,195,111,229,53,193,228,54,62,109,229,54,180,198,
  227,93,126,239,116,227,7,204,10,222,57,199,85,223,141,236,152,126,78,32,161,201,224,93,
  48,73,84,211,233,170,149,15,100,145,131,40,21,182,58,247,141,23,2,16,167,225,239,49,
  26,78,235,26,185,190,108,77,191,155,115,106,70,42,12,16,190,199,79,169,115,125,123,136,
  221,10,205,182,214,214,13,224,131,254,241,239,251,95,191,250,83,79,127,223,167,217,39,61,
  222,241,219,155,122,148,131,61,172,108,64,205,253,95,240,6,248,72,106,128,71,152,132,62,
  254,73,111,129,129,35,137,148,251,164,42,63,197,42,63,113,54,202,61,81,229,39,167,50,
  39,42,70,243,245,53,94,100,25,237,250,100,163,234,27,80,127,21,192,198,113,230,177,248,
  201,124,230,42,200,176,206,145,191,199,44,53,70,229,217,235,48,65,60,83,76,135,117,134,
  46,232,206,254,124,190,216,231,70,188,139,108,203,147,184,14,150,30,69,43,144,32,110,251,
  19,144,28,38,60,87,203,19,36,190,58,119,145,137,11,53,136,7,77,32,157,189,126,39,
  49,35,75,117,130,34,98,233,97,135,70,156,70,38,120,22,67,151,236,65,139,131,179,100,
  55,58,193,77,171,179,201,225,171,178,63,201,249,93,211,190,224,66,38,144,31,42,150,4,
  135,85,153,23,221,127,109,55,195,81,34,222,73,164,147,73,123,172,53,153,136,63,52,229,
  177,101,154,82,236,36,97,121,24,68,21,182,41,161,130,50,221,57,238,36,101,146,59,12,
  96,118,83,49,28,49,224,78,40,103,144,144,47,53,18,243,53,94,144,47,90,16,127,140,
  87,82,22,221,183,109,234,99,122,24,10,118,107,140,130,29,120,241,207,166,146,6,41,253,
  10,228,81,237,55,96,111,12,226,92,18,46,245,86,82,175,64,228,215,149,203,118,246,124,
  12,46,127,142,193,37,152,83,54,245,111,26,181,116,206,112,19,49,43,56,136,68,174,199,
  29,184,239,202,81,15,207,85,90,160,221,5,92,7,92,147,66,172,238,67,18,6,4,81,
  118,56,73,212,123,156,246,148,38,254,78,137,180,62,130,255,153,177,85,182,16,35,191,180,
  178,144,253,158,100,74,228,147,156,106,207,247,168,149,196,59,136,81,102,165,107,144,213,81,
  35,252,244,5,127,244,10,119,168,150,17,57,65,78,33,198,80,46,129,103,41,155,37,141,
  180,0,238,201,191,235,35,97,56,38,127,103,142,221,127,253,90,13,233,232,57,119,71,77,
  146,103,246,145,78,15,121,56,20,198,211,48,140,167,177,79,24,162,27,124,231,235,6,1,
  196,104,38,121,125,187,131,22,246,228,92,158,157,76,78,164,8,4,165,234,105,35,252,145,
  150,175,75,177,77,155,132,37,235,122,53,76,157,156,167,109,243,246,236,247,176,0,116,137,
  124,174,27,236,50,169,45,122,223,143,104,236,99,64,127,159,248,239,79,136,239,117,179,248,
  154,1,160,214,68,122,74,31,137,159,192,208,12,70,232,58,248,128,185,191,209,88,136,225,
  168,94,167,202,231,180,115,110,117,208,202,242,19,88,253,13,165,121,107,98,250,69,47,126,
  195,168,42,205,215,62,60,145,148,203,195,164,0,30,41,181,191,30,137,181,197,94,137,229,
  20,61,177,142,83,65,40,122,85,209,148,38,89,166,170,26,107,214,72,146,88,37,73,130,
  164,130,85,146,10,84,202,79,157,242,64,130,228,64,215,196,69,203,142,150,115,24,53,4,
  43,40,167,176,61,64,68,0,72,90,134,91,209,132,117,137,86,214,67,53,47,252,101,249,
  253,251,135,212,247,128,153,178,191,83,121,113,54,38,193,77,116,27,19,239,62,7,173,72,
  59,177,47,30,104,231,34,211,229,49,175,73,137,162,188,93,154,221,156,179,203,164,227,111,
  184,175,189,239,149,69,171,64,241,27,146,202,249,165,130,250,43,202,92,70,4,94,84,138,
  72,192,193,30,141,221,1,246,180,218,65,181,77,24,43,39,188,128,85,155,226,134,253,237,
  89,102,72,59,48,157,109,88,225,158,195,25,193,19,109,9,205,23,29,0,129,87,24,253,
  209,219,241,41,103,55,62,197,219,107,5,215,101,246,110,121,187,73,151,226,76,135,175,250,
  146,108,226,205,71,148,197,78,156,12,76,71,156,153,16,232,11,162,226,28,65,14,46,64,
  182,188,122,61,34,98,198,83,32,34,100,125,108,207,24,69,74,198,114,139,191,177,99,245,
  223,236,216,235,59,85,215,157,114,47,237,243,86,222,94,170,231,162,71,28,106,197,206,85,
  217,159,238,41,15,88,177,48,62,38,195,141,252,67,203,212,145,189,232,179,147,63,50,54,
  211,59,137,224,219,56,87,128,37,7,2,51,182,181,195,25,227,12,98,157,61,191,233,51,
  120,225,25,254,246,83,74,56,94,106,116,37,155,164,118,144,104,19,124,187,173,71,221,87,
  81,121,55,137,205,180,117,89,237,80,139,10,57,97,4,40,235,38,49,53,193,199,245,180,
  142,15,9,215,4,247,184,0,123,156,95,151,7,177,153,207,109,184,87,221,55,72,101,70,
  201,249,13,225,217,176,168,63,61,254,145,219,114,11,109,1,148,82,71,5,148,155,223,10,
  214,105,57,45,12,216,130,29,75,15,182,4,169,73,181,253,153,68,12,78,82,107,108,209,
  226,179,3,22,222,111,1,185,104,132,122,10,118,28,31,175,235,246,57,168,48,83,94,217,
  17,239,128,235,106,26,144,195,13,181,61,91,113,211,178,235,92,46,251,218,226,183,167,82,
  1,190,34,12,154,243,22,227,58,149,80,155,79,198,235,202,212,124,214,25,0,212,184,97,
  16,55,92,219,37,28,143,140,84,158,165,143,124,148,96,84,9,215,189,145,42,227,121,175,
  220,137,146,82,62,70,92,145,50,76,91,249,4,122,139,148,99,59,197,245,88,146,59,82,
  20,59,138,185,137,59,100,38,17,37,219,78,1,243,28,36,76,64,114,165,41,190,116,163,
  19,220,163,222,22,201,75,112,203,19,182,26,28,16,17,62,52,160,238,226,67,201,252,227,
  75,146,203,14,197,97,224,97,131,182,219,123,134,242,88,139,162,116,127,156,129,208,195,164,
  95,8,73,55,181,174,222,72,3,178,167,218,69,139,22,221,182,232,52,228,73,124,31,75,
  9,107,60,74,121,224,123,13,168,231,180,233,241,142,236,246,81,240,143,189,41,37,195,145,
  27,201,70,157,1,233,193,236,107,25,6,251,181,244,51,152,43,92,175,180,143,77,120,239,
  184,240,248,152,32,39,50,87,183,178,176,82,176,25,89,3,97,52,55,120,217,176,92,50,
  162,71,75,11,166,183,16,231,149,175,46,247,205,254,224,29,52,97,223,113,255,68,105,194,
  222,125,250,158,123,167,247,9,80,55,109,157,190,251,129,96,85,27,167,45,235,150,97,135,
  124,148,176,73,16,130,137,126,213,168,215,246,181,196,253,191,242,200,228,246,16,206,117,173,
  129,47,41,164,65,235,47,227,118,28,124,9,90,225,144,165,229,81,150,22,42,179,199,165,
  8,204,234,5,190,157,12,5,246,0,214,24,213,107,204,22,55,102,99,108,99,182,212,178,
  60,186,57,91,40,47,229,134,55,72,65,122,121,17,186,6,9,93,182,218,117,64,53,248,
  62,6,190,55,16,3,73,225,251,248,211,70,238,24,248,46,167,54,193,108,13,81,187,62,
  99,246,136,108,148,249,141,232,163,135,243,98,243,153,71,114,110,121,110,252,202,192,174,30,
  92,218,128,161,37,34,199,254,47,43,160,223,10,28,92,58,138,47,113,119,50,65,169,105,
  247,62,238,79,117,251,210,187,4,78,240,139,36,40,159,135,250,172,230,179,172,247,151,58,
  193,168,154,115,214,108,99,15,110,126,236,61,222,38,160,126,6,35,111,194,75,217,31,69,
  222,116,156,139,91,194,254,53,50,3,253,95,26,176,241,247,133,172,10,16,108,25,46,181,
  124,24,69,228,114,38,43,176,221,60,132,226,65,236,29,184,34,69,204,43,92,254,213,62,
  94,145,243,103,254,140,114,249,103,157,127,224,200,71,44,2,156,106,16,17,195,169,6,78,
  247,218,253,212,121,104,66,123,42,33,137,143,243,98,197,121,199,137,194,105,62,51,114,239,
  45,83,75,129,50,131,40,254,154,28,232,1,77,46,206,59,17,20,187,181,228,183,169,46,
  188,89,220,106,13,170,115,79,90,46,74,159,179,7,22,181,179,95,215,129,233,38,17,195,
  91,249,183,133,226,176,215,109,67,133,24,134,43,235,161,56,198,149,52,148,65,4,162,169,
  98,233,164,112,88,45,22,54,43,85,164,160,175,83,223,11,135,165,106,165,84,45,237,23,
  40,41,157,125,101,189,253,194,214,106,101,231,184,192,21,157,250,212,201,48,212,177,141,154,
  97,78,245,79,246,124,230,108,226,140,166,125,208,76,234,172,241,0,115,46,180,192,65,63,
  60,84,160,119,87,24,186,141,194,65,101,187,90,41,148,209,234,167,208,79,224,154,62,145,
  134,138,72,67,75,164,117,145,77,65,164,33,211,102,168,104,147,212,86,9,216,71,29,68,
  132,186,190,63,13,73,7,113,169,71,67,223,76,15,229,86,250,68,177,150,85,160,9,14,
  62,179,95,64,152,244,180,115,133,123,98,35,90,100,181,120,178,250,173,236,149,217,143,150,
  193,134,209,47,241,216,143,149,248,114,180,90,244,202,28,69,161,196,139,148,162,69,182,226,
  101,54,99,101,14,11,8,159,237,247,106,45,214,241,82,197,3,133,45,112,13,203,101,114,
  171,95,46,137,114,94,18,153,67,200,204,33,76,100,14,148,215,12,213,192,98,18,227,55,
  208,99,149,249,24,90,213,8,239,68,168,248,248,9,237,77,77,153,183,238,172,238,71,25,
  39,149,26,82,57,84,191,235,234,247,76,3,109,68,231,189,79,232,87,212,229,95,191,240,
  203,196,94,174,20,246,215,119,138,46,185,123,73,73,20,79,32,152,103,92,89,146,167,53,
  231,188,213,102,162,191,98,86,137,220,36,170,191,10,131,189,78,176,137,56,203,169,207,12,
  226,215,47,209,7,32,16,36,163,27,89,46,68,30,76,83,9,216,192,51,112,129,87,96,
  2,41,252,137,210,109,226,7,194,80,125,97,93,127,225,44,225,11,165,91,7,126,72,237,
  38,164,226,253,204,126,32,74,56,140,69,148,76,34,28,70,79,67,161,71,47,5,101,249,
  150,224,166,33,184,73,38,150,127,54,238,47,34,98,136,165,132,190,222,221,81,118,18,125,
  101,38,161,76,209,130,115,239,237,158,182,129,183,244,231,146,254,212,189,204,42,89,68,42,
  183,55,55,212,204,11,121,126,144,159,67,249,41,171,54,201,207,158,106,169,252,72,64,247,
  146,190,40,174,46,125,3,210,145,228,111,202,207,177,252,108,200,207,150,252,188,147,115,112,
  27,8,189,53,181,183,250,181,186,94,218,91,219,217,47,108,128,84,191,86,142,14,11,213,
  157,189,213,173,66,245,104,127,167,82,134,66,143,245,116,228,44,235,107,188,58,56,103,121,
  167,180,79,119,144,180,107,61,57,136,155,76,99,228,62,102,86,112,98,154,199,81,3,60,
  102,178,133,219,95,255,47,231,78,164,254,159,122,250,47,216,253,54,206,83,95,211,184,143,
  67,159,105,133,127,195,69,86,214,129,84,194,245,71,20,46,148,21,52,162,131,8,56,83,
  100,40,200,25,125,36,192,40,128,226,21,131,114,24,198,187,82,94,223,41,151,75,135,213,
  181,210,87,210,89,39,244,117,167,112,114,80,58,172,144,234,90,203,94,105,108,77,186,55,
  178,11,56,4,77,222,133,73,57,171,20,95,92,115,167,221,48,85,4,117,6,159,3,125,
  10,208,148,74,71,216,212,44,73,133,57,158,65,160,121,240,229,176,161,20,112,116,100,139,
  171,113,157,186,126,47,6,194,62,161,199,50,237,108,33,181,53,165,7,114,111,103,191,186,
  185,83,196,74,16,208,58,183,122,72,188,104,116,141,213,173,120,13,35,246,174,211,105,199,
  122,248,233,51,254,64,170,46,66,80,179,85,103,55,126,253,242,83,102,54,170,171,135,135,
  171,223,86,248,139,59,20,122,119,118,3,43,53,40,183,53,117,184,181,182,202,38,16,151,
  206,123,107,234,104,191,188,179,69,196,184,246,173,82,32,161,52,107,43,207,80,229,15,235,
  145,250,163,42,155,64,236,33,207,231,59,114,117,115,119,234,182,240,71,126,151,184,181,77,
  8,188,23,216,103,184,229,215,143,214,8,71,7,145,90,58,57,136,39,85,15,74,229,29,
  22,197,190,6,11,62,48,141,158,88,19,36,57,72,72,202,68,27,52,27,237,192,172,219,
  1,188,112,141,152,97,74,134,236,109,173,41,10,72,218,41,129,24,183,190,184,38,101,121,
  9,121,132,66,246,132,205,80,224,115,41,117,126,46,101,215,143,138,197,234,230,234,122,1,
  147,46,36,219,25,75,238,87,169,98,250,249,254,180,248,67,31,239,181,180,99,210,34,28,
  109,34,157,77,71,108,220,234,208,45,47,155,0,229,145,215,86,200,56,21,142,100,58,25,
  177,244,82,170,158,227,196,88,102,11,27,115,136,29,132,148,12,168,8,10,4,66,172,27,
  135,171,39,213,205,195,213,189,194,218,209,230,102,225,144,78,12,128,96,39,133,10,167,85,
  233,120,193,104,125,41,77,166,50,116,248,161,155,249,168,155,73,100,248,25,119,154,46,73,
  106,161,77,62,186,251,57,95,231,189,194,57,139,185,142,182,240,51,45,23,117,222,191,16,
  199,80,103,65,224,23,69,173,188,238,19,136,207,86,77,218,212,23,237,16,65,10,215,30,
  255,160,54,175,151,138,224,107,171,149,202,234,250,246,94,97,191,50,109,93,94,132,196,215,
  215,67,91,13,123,147,79,235,33,254,98,122,127,38,73,58,169,254,7,108,33,244,55,109,
  85,238,210,180,210,22,152,143,175,173,174,127,166,158,208,29,76,121,147,114,56,191,161,232,
  152,83,20,152,67,20,2,253,212,103,103,92,143,153,16,212,160,62,1,84,145,6,244,222,
  120,61,37,106,120,202,23,53,162,249,158,21,220,45,157,110,193,56,17,195,116,180,191,94,
  93,221,216,8,78,219,247,230,189,124,180,86,57,92,93,175,4,167,231,54,241,144,164,255,
  114,193,100,190,228,30,26,167,151,247,212,88,112,79,68,75,62,189,151,151,85,185,102,143,
  128,201,207,167,117,174,79,155,179,224,244,130,159,177,193,10,78,111,249,177,124,184,94,101,
  148,33,239,92,39,172,22,15,182,87,131,211,167,123,47,1,39,77,152,167,144,212,131,211,
  71,206,217,40,87,116,221,166,73,80,117,111,244,135,136,173,31,149,171,206,119,110,207,227,
  89,170,214,32,82,203,249,66,53,33,75,213,42,115,214,122,105,191,92,89,221,55,21,246,
  34,21,162,249,103,126,45,5,171,48,172,22,231,59,39,125,223,157,149,49,240,149,10,20,
  72,155,118,121,52,9,193,94,158,111,180,134,129,69,237,181,98,97,127,35,205,2,149,230,
  248,164,48,191,209,122,132,43,175,12,248,91,17,244,212,189,23,136,114,103,247,42,228,155,
  175,148,146,162,19,206,173,22,164,92,254,21,33,151,35,98,22,238,36,108,66,202,18,24,
  28,91,162,12,68,53,20,164,70,170,221,102,81,109,54,55,104,15,200,117,137,123,150,27,
  20,77,6,26,69,161,151,228,17,27,145,231,109,231,46,207,93,208,46,72,175,216,253,249,
  144,22,48,245,38,211,147,155,233,1,171,15,3,102,169,106,88,167,226,16,245,118,115,195,
  24,111,241,21,151,9,40,101,107,109,48,64,29,132,52,59,49,25,232,61,170,48,151,215,
  34,57,130,212,127,17,170,135,127,32,24,49,8,209,94,150,239,207,248,210,27,84,222,218,
  232,20,230,176,236,232,185,63,97,76,44,227,94,216,233,26,64,239,30,241,135,34,227,18,
  133,191,167,20,223,255,8,248,111,13,145,191,137,137,236,94,28,31,87,188,117,185,0,11,
  223,161,29,160,154,182,77,90,139,224,139,148,102,63,84,5,88,197,104,121,90,231,237,109,
  32,42,182,7,181,195,61,75,152,153,102,108,192,183,207,195,31,196,190,155,164,13,122,160,
  245,254,140,180,155,1,239,151,15,213,118,25,119,238,121,119,187,167,54,183,235,14,76,111,
  172,183,194,211,207,240,145,21,158,94,134,252,19,202,207,58,1,63,204,127,70,111,209,190,
  61,82,110,172,163,181,248,202,153,81,104,62,166,149,1,53,239,102,43,206,23,68,177,137,
  146,61,10,243,212,164,63,103,129,196,21,98,237,38,105,58,161,93,70,130,160,201,21,40,
  72,30,98,129,162,104,227,130,173,48,55,180,194,88,214,151,205,114,34,123,20,109,200,176,
  28,107,98,16,120,253,51,75,117,248,91,140,198,254,45,198,2,4,175,136,136,151,37,118,
  109,74,235,138,122,32,244,123,185,119,174,31,55,112,206,23,41,37,81,206,108,81,239,29,
  229,189,119,198,150,126,209,57,113,26,118,165,213,162,213,100,169,100,54,162,46,106,195,134,
  126,168,146,89,225,162,146,79,16,170,161,161,100,102,149,206,234,114,73,103,36,94,146,4,
  166,244,30,156,156,235,26,224,108,240,64,104,237,58,0,220,178,123,28,104,162,235,52,210,
  209,209,4,230,237,176,113,17,120,89,78,61,232,90,108,86,173,213,182,5,191,71,94,217,
  132,34,29,124,1,181,192,132,171,253,212,236,118,212,221,167,200,59,76,229,67,70,175,151,
  202,151,17,137,42,18,15,113,89,243,197,182,50,74,156,129,166,121,189,4,121,10,234,3,
  173,5,27,150,109,41,249,132,37,61,178,59,32,29,88,17,155,69,236,156,59,33,219,238,
  131,124,79,104,7,232,165,32,9,218,142,162,235,173,152,54,7,207,180,148,247,238,87,82,
  145,29,9,37,111,18,100,52,250,252,28,224,41,208,156,130,69,242,104,58,75,57,181,123,
  63,103,243,16,215,67,232,211,209,180,234,234,62,246,151,84,47,173,250,232,124,107,51,95,
  116,92,95,154,86,29,147,89,35,73,187,116,154,203,87,146,73,124,61,246,250,240,165,161,
  164,31,84,145,62,28,148,138,223,182,74,251,213,210,230,102,185,80,161,109,57,42,165,54,
  120,119,131,184,173,248,253,156,102,187,3,212,218,2,15,170,249,6,222,200,130,18,236,227,
  57,235,8,188,81,85,155,9,213,252,164,207,216,102,85,165,19,50,214,74,69,226,106,57,
  221,36,39,44,130,212,114,247,50,69,171,24,152,254,240,142,110,58,125,81,67,222,154,146,
  117,208,232,53,144,229,161,102,223,96,198,219,29,125,209,86,86,159,35,160,179,159,243,95,
  204,68,109,16,231,206,241,129,185,169,137,183,103,54,187,82,1,84,181,7,66,121,125,9,
  184,78,158,102,50,138,178,233,26,53,149,214,12,189,239,226,65,32,20,83,39,62,39,116,
  226,51,117,226,51,161,222,87,217,208,208,221,97,3,202,103,88,108,62,87,12,44,88,2,
  234,236,129,236,17,32,90,244,229,71,174,248,135,237,68,81,183,203,244,42,242,41,49,185,
  147,35,85,121,209,241,98,205,238,81,91,250,56,158,167,233,147,240,204,253,220,98,35,97,
  200,11,20,72,195,234,95,166,166,166,112,198,121,79,102,13,136,255,113,94,83,114,88,68,
  64,32,105,192,81,200,23,70,192,156,253,77,152,77,3,147,174,128,221,159,253,163,6,150,
  146,128,253,110,203,174,147,123,251,79,219,120,50,26,236,239,182,22,6,161,110,223,177,6,
  252,131,54,214,146,128,253,110,203,190,185,176,254,17,234,118,99,144,126,183,77,93,87,43,
  113,43,170,38,117,231,157,222,178,17,165,177,203,196,42,90,3,116,171,53,84,60,101,111,
  91,143,136,62,10,76,53,90,74,61,117,43,234,41,167,106,157,191,10,131,105,37,81,22,
  157,19,242,190,92,14,195,116,127,196,188,198,173,15,152,253,22,201,226,183,171,124,113,22,
  211,174,83,4,134,116,23,38,65,50,6,194,49,80,119,97,2,168,7,95,163,117,174,53,
  88,185,136,6,203,59,41,137,170,178,192,136,63,115,197,98,58,206,162,25,153,42,14,206,
  26,140,12,175,217,50,134,170,23,249,2,6,148,101,12,143,24,168,219,178,162,219,50,192,
  49,77,211,95,106,235,4,12,195,165,12,195,189,243,153,181,150,220,1,60,167,35,50,124,
  171,90,61,147,20,54,94,226,70,80,0,86,213,21,255,219,155,52,3,172,202,210,232,29,
  220,36,71,94,112,147,93,133,171,155,158,40,6,184,5,188,53,216,207,113,207,32,253,156,
  97,114,89,48,74,215,145,164,112,160,141,189,151,19,211,114,216,173,127,180,184,108,128,180,
  1,75,224,218,165,192,190,90,255,103,142,136,99,197,173,133,192,31,246,221,61,143,23,233,
  222,200,68,164,182,246,142,155,249,221,61,116,157,155,89,158,91,94,88,156,89,158,119,114,
  60,43,0,124,59,177,16,100,242,214,212,231,66,129,14,1,156,31,243,65,125,116,41,221,
  137,136,160,65,130,136,27,21,218,84,159,125,49,195,10,62,90,212,112,245,221,158,190,218,
  88,97,197,75,69,117,216,35,138,194,176,96,35,177,168,163,154,213,73,86,218,21,60,43,
  86,53,205,71,54,231,53,40,17,148,161,187,125,213,150,213,14,59,122,93,113,151,143,98,
  206,0,169,32,110,28,6,145,194,58,152,139,151,56,218,127,117,153,205,226,206,65,245,91,
  149,195,73,7,66,66,137,229,14,14,11,123,71,197,202,206,65,241,155,154,88,99,171,176,
  206,172,140,199,2,169,69,213,105,170,170,6,210,56,44,157,148,97,217,179,81,216,92,5,
  96,73,31,210,89,20,133,5,208,254,86,101,91,200,36,161,72,249,51,250,113,176,243,181,
  80,44,143,46,67,159,77,42,161,26,61,250,83,170,144,156,33,111,23,118,182,182,43,35,
  138,141,105,147,91,106,76,171,184,12,127,149,75,217,99,120,239,228,247,127,221,153,124,183,
  97,84,98,227,136,252,46,124,117,209,222,20,91,69,164,136,1,234,167,174,126,210,190,115,
  212,229,224,172,10,54,210,147,240,240,240,27,164,56,90,182,139,157,16,159,213,101,175,2,
  181,116,96,215,23,68,152,66,246,8,33,83,236,41,78,246,49,12,44,47,200,30,135,129,
  115,175,30,74,35,49,132,81,215,16,96,62,77,186,7,19,206,254,132,179,215,21,119,204,
  222,114,118,81,179,145,108,145,223,15,92,182,152,253,194,105,206,29,249,108,21,6,27,46,
  147,204,238,53,2,103,103,148,221,135,31,10,247,253,184,17,36,237,117,178,141,164,244,217,
  141,108,33,176,2,105,246,91,96,101,202,236,110,224,144,102,182,194,145,232,140,52,152,237,
  134,42,20,221,209,90,73,5,237,205,62,132,65,130,232,145,45,134,129,43,141,103,123,238,
  59,62,84,147,119,179,165,200,54,3,111,231,146,45,5,67,54,31,217,235,96,200,254,33,
  123,18,40,14,157,173,135,129,102,191,217,118,168,44,108,54,67,119,147,163,124,140,4,136,
  142,20,244,2,19,12,48,20,175,176,204,169,170,172,153,19,247,46,245,170,216,254,86,195,
  174,118,23,59,153,94,81,38,146,175,43,173,92,105,229,105,39,219,189,152,232,212,6,173,
  102,13,24,249,123,242,126,114,229,143,76,246,175,18,86,213,251,254,90,15,23,5,26,189,
  191,154,98,148,110,138,209,2,213,91,165,24,184,105,101,177,3,15,40,247,30,39,144,139,
  194,101,40,199,46,99,60,2,135,181,180,133,168,234,207,19,245,193,60,3,65,195,121,230,
  125,66,35,16,102,139,216,128,159,65,126,30,88,240,36,147,77,116,37,53,57,83,103,115,
  22,54,51,226,109,199,179,115,252,157,130,217,130,217,96,84,87,146,0,82,137,236,61,12,
  147,101,206,79,58,210,231,13,101,6,37,25,140,235,124,38,56,201,99,67,95,224,251,167,
  169,19,97,22,127,151,126,253,58,81,140,226,239,18,169,48,174,243,165,191,216,209,0,121,
  218,81,165,2,93,4,202,195,235,79,153,52,0,168,142,111,87,246,138,76,49,133,54,223,
  110,144,158,23,38,84,72,231,164,34,80,178,219,186,210,139,209,149,189,50,166,54,131,92,
  107,133,184,36,146,80,207,201,53,53,56,8,43,179,170,132,10,54,83,133,247,10,93,119,
  11,215,127,42,76,128,133,198,50,20,106,114,151,222,142,37,255,148,194,60,173,133,106,111,
  243,45,223,92,209,41,217,75,109,137,242,77,160,230,145,254,77,193,201,215,232,57,74,34,
  124,50,206,157,194,160,18,207,23,72,129,241,117,32,215,202,192,87,38,20,35,115,253,170,
  80,160,119,120,56,128,9,210,68,106,242,131,234,202,135,201,71,122,150,175,126,152,76,79,
  132,93,202,237,133,156,81,163,36,114,183,242,77,78,217,164,193,147,96,88,181,73,152,178,
  23,222,191,79,248,52,55,144,44,221,41,70,130,110,71,171,15,200,221,137,179,86,115,196,
  199,233,75,5,141,149,130,165,225,11,208,171,158,0,133,169,136,139,4,55,36,121,65,194,
  131,186,249,169,130,119,79,206,5,52,236,42,120,220,98,39,107,10,207,110,12,41,74,139,
  128,46,196,22,91,110,185,95,191,40,111,221,48,89,46,160,80,179,18,183,233,201,186,73,
  238,29,28,158,202,193,117,112,130,145,167,147,109,58,155,46,40,198,161,34,115,21,146,194,
  114,21,126,228,18,6,10,222,140,26,55,183,33,13,57,24,33,60,97,116,62,54,30,113,
  199,129,142,6,197,38,143,239,212,192,199,235,132,92,77,157,248,175,201,15,133,15,147,255,
  53,41,78,16,107,97,238,26,68,94,243,236,218,181,227,111,242,202,158,89,152,164,169,242,
  235,87,194,199,143,88,138,176,223,38,31,238,76,160,218,138,101,2,39,186,151,221,251,112,
  34,14,117,162,241,200,126,226,225,125,201,78,44,98,104,77,54,200,57,44,108,160,89,37,
  49,206,33,183,225,120,251,70,233,179,51,155,233,64,210,217,35,184,151,153,89,48,153,158,
  205,152,206,95,138,229,150,225,50,10,213,41,58,60,138,212,66,130,33,38,245,82,48,49,
  191,188,95,58,220,227,82,136,193,169,155,91,221,217,175,20,182,216,126,104,84,11,142,118,
  134,181,65,119,33,161,4,64,219,254,219,124,15,240,78,164,201,26,156,73,247,128,236,216,
  166,111,37,35,122,107,36,166,183,198,161,122,235,53,184,222,26,135,236,173,100,108,111,189,
  14,217,91,99,177,189,53,14,221,91,195,240,189,53,4,225,91,201,24,223,242,81,190,246,
  202,14,172,141,239,193,218,216,46,172,13,237,195,218,176,78,172,13,233,197,90,164,27,171,
  175,237,199,234,43,58,178,58,190,39,171,195,187,178,58,180,47,171,195,58,179,26,233,77,
  188,19,9,20,185,54,150,102,215,60,162,77,232,79,117,190,186,204,255,96,165,102,91,179,
  92,45,204,39,150,206,76,35,42,66,70,254,119,106,208,235,22,253,89,67,1,111,80,68,
  240,217,133,48,178,114,217,203,126,101,206,206,14,57,72,215,115,146,206,37,205,118,66,198,
  168,249,78,88,28,49,227,119,145,30,210,58,88,38,170,18,61,201,82,86,13,204,107,56,
  1,193,31,135,215,213,145,136,229,26,213,57,249,231,180,123,110,72,201,121,254,151,177,37,
  231,171,171,25,189,139,70,90,94,173,40,191,126,169,103,32,200,60,111,185,25,91,94,14,
  99,202,125,37,196,194,177,134,115,111,140,54,230,85,217,102,75,116,13,22,207,156,219,139,
  188,93,160,65,44,25,81,106,165,105,252,118,209,211,78,71,126,75,253,149,82,94,105,153,
  103,230,170,74,53,188,148,165,188,98,199,230,161,17,126,230,81,159,201,61,94,149,69,209,
  13,199,69,68,118,2,203,245,89,75,59,44,173,133,33,220,200,145,8,79,18,161,239,123,
  139,214,251,9,165,103,208,238,255,102,230,62,82,101,91,13,34,98,58,59,190,55,116,129,
  225,0,218,229,253,202,204,92,172,59,54,19,29,75,232,143,205,207,192,60,187,228,28,97,
  123,91,49,146,74,217,232,65,164,59,71,253,161,250,142,205,133,117,151,4,169,172,20,75,
  249,217,88,225,109,68,187,219,156,73,153,253,86,83,237,183,154,102,191,245,33,147,165,138,
  44,239,58,199,187,38,77,7,111,158,94,137,38,101,125,193,211,180,204,191,173,140,66,36,
  180,167,87,154,209,234,142,121,209,58,9,207,108,108,156,47,168,107,173,185,230,152,251,177,
  235,208,124,165,154,224,47,248,20,111,174,76,3,238,167,228,214,171,206,165,61,158,201,188,
  181,153,142,189,244,111,124,31,167,50,71,30,144,138,5,210,225,73,37,59,225,38,78,116,
  30,26,103,77,220,180,110,217,51,39,125,173,157,39,19,62,218,239,222,247,96,120,122,173,
  108,179,75,92,245,90,224,157,228,175,79,9,202,57,8,181,241,185,1,31,250,39,164,94,
  168,87,90,55,141,254,199,143,129,243,38,126,126,118,240,233,160,196,49,176,167,174,27,79,
  125,0,114,188,105,190,127,95,215,56,40,193,57,143,234,166,183,153,217,73,234,138,170,164,
  207,2,76,191,84,66,122,68,95,212,45,228,72,55,160,45,188,193,183,123,79,198,188,252,
  227,71,199,212,62,25,157,5,207,77,12,230,86,97,152,207,63,219,51,175,4,150,164,161,
  251,179,180,182,84,191,198,205,147,235,79,11,185,107,186,61,31,187,129,111,186,238,76,204,
  211,107,248,27,215,213,79,80,253,228,211,144,98,218,246,253,4,176,53,82,221,83,143,33,
  213,78,79,126,164,149,131,148,215,87,74,231,76,6,179,78,201,96,75,120,6,33,157,143,
  194,112,138,18,12,222,160,191,14,11,233,8,6,147,202,232,254,95,191,173,255,111,236,188,
  63,247,126,175,243,196,62,84,234,158,163,46,116,190,98,129,37,182,103,72,45,7,46,31,
  50,186,13,25,134,192,88,65,23,141,195,75,1,113,227,58,156,84,39,74,55,110,246,171,
  16,232,117,233,69,115,6,115,141,196,233,36,180,134,37,211,151,79,39,50,229,148,134,76,
  38,126,137,27,212,11,35,12,71,221,218,118,121,82,172,76,58,137,199,56,172,129,65,123,
  60,80,7,204,181,102,81,56,98,223,116,67,67,108,216,91,233,155,54,117,139,216,213,102,
  222,81,45,189,83,230,84,133,252,166,145,150,254,134,55,65,235,227,186,239,40,185,116,18,
  244,107,189,39,146,79,148,250,98,242,67,170,240,33,147,254,48,169,181,23,19,247,100,37,
  57,241,0,95,20,208,113,224,34,253,196,214,193,145,150,117,250,19,221,78,251,9,149,188,
  207,192,82,240,3,212,179,78,219,190,91,230,138,120,25,170,117,77,137,137,80,152,122,232,
  193,205,43,17,169,243,94,137,188,195,166,121,218,73,50,190,25,221,52,45,142,56,105,181,
  78,171,223,133,51,129,219,39,39,81,235,130,54,89,21,228,100,92,68,19,216,79,156,125,
  141,232,235,156,28,39,234,139,178,211,181,64,113,254,243,205,121,191,239,220,214,206,175,87,
  225,160,166,115,195,250,122,147,195,2,113,25,185,252,205,171,46,69,85,176,56,108,133,142,
  60,28,89,163,162,34,201,62,173,200,5,14,144,99,87,27,237,12,84,108,98,40,151,188,
  0,19,34,172,115,219,63,88,22,83,193,162,40,18,94,9,20,174,94,145,109,178,180,18,
  94,137,91,44,61,40,81,54,61,66,135,123,83,235,93,43,223,180,114,88,52,1,23,228,
  228,247,152,161,76,144,54,22,153,136,215,71,190,104,181,99,150,107,182,9,163,121,39,30,
  122,222,12,95,128,131,114,91,29,13,73,129,127,134,153,103,41,32,180,234,155,53,162,29,
  78,64,14,137,213,145,217,110,100,15,173,255,140,188,243,153,14,62,228,89,46,122,23,48,
  163,16,3,199,212,211,149,245,190,142,26,250,81,99,252,154,145,140,225,224,223,137,2,117,
  245,243,181,136,248,242,239,69,196,144,102,207,190,126,228,26,35,103,45,9,132,27,190,123,
  233,215,207,192,163,215,54,215,92,224,29,211,104,229,222,245,249,244,103,141,140,253,14,11,
  7,240,179,17,156,94,119,248,226,91,17,70,85,100,76,85,216,216,194,141,190,107,46,178,
  183,115,120,88,34,173,175,148,125,161,171,229,207,167,165,6,229,169,251,213,193,233,234,189,
  243,138,171,72,7,116,113,216,228,174,245,18,114,139,59,148,16,156,254,100,72,250,173,210,
  181,111,49,56,235,173,120,166,188,189,208,149,246,231,211,29,213,12,216,161,4,167,37,126,
  81,54,79,167,69,126,35,155,42,220,158,108,203,51,249,5,9,78,55,56,71,189,92,114,
  214,150,122,219,231,44,229,139,36,56,61,18,240,202,235,136,115,241,239,78,81,0,203,135,
  218,165,104,135,52,33,175,8,49,74,122,153,166,227,137,152,54,218,208,17,120,41,149,110,
  52,101,173,23,77,89,111,113,138,241,120,108,224,56,41,10,142,147,162,224,56,41,235,116,
  99,112,236,25,136,244,96,226,130,107,145,112,193,10,17,238,29,189,113,124,49,115,58,50,
  53,49,12,5,17,173,10,112,201,242,7,2,157,192,217,25,157,176,69,239,250,23,156,219,
  225,39,135,32,128,50,156,33,96,19,200,2,198,143,87,148,175,216,242,21,148,79,21,98,
  62,0,10,137,62,0,210,36,164,142,129,125,104,97,31,142,105,139,227,167,96,149,234,152,
  145,28,87,207,122,68,144,122,122,228,126,144,64,17,241,139,205,242,236,112,88,164,57,90,
  37,152,165,13,74,214,175,135,133,77,98,2,170,212,232,230,232,58,100,98,137,57,120,26,
  107,194,15,114,128,47,211,192,61,26,19,186,169,26,217,173,117,174,226,237,170,73,228,18,
  118,201,39,227,63,136,104,223,191,247,83,132,248,223,60,245,28,87,128,77,71,144,252,27,
  151,221,132,143,55,211,164,94,184,239,209,229,177,85,43,104,42,94,159,112,146,152,212,177,
  156,143,65,172,11,96,211,150,16,190,226,70,206,78,185,84,57,44,29,124,99,255,87,162,
  91,131,104,232,182,41,96,207,135,123,181,71,219,12,248,86,166,141,199,240,118,230,93,0,
  174,135,161,93,111,201,130,105,72,33,81,165,196,186,16,55,3,230,172,205,81,222,62,215,
  213,9,231,53,190,172,116,54,39,74,103,115,157,206,157,184,128,79,216,144,141,13,194,112,
  68,172,234,97,131,246,29,125,161,241,232,133,188,20,90,229,78,250,249,228,180,135,251,252,
  46,12,78,120,214,183,95,98,254,70,2,163,197,66,60,186,248,190,13,33,228,75,124,157,
  154,225,88,149,215,135,15,57,57,49,206,159,156,22,60,45,89,45,252,195,251,188,151,235,
  105,208,160,199,143,169,208,154,172,40,178,53,200,134,162,16,149,182,164,45,170,137,218,183,
  165,163,214,189,211,38,50,42,203,177,236,112,30,11,127,149,210,127,185,66,202,149,62,142,
  55,59,225,130,114,15,121,88,235,52,197,13,67,47,116,35,228,52,34,23,83,60,169,86,
  108,152,163,186,95,62,238,111,78,145,140,175,132,111,128,236,147,13,108,10,182,100,117,56,
  198,251,187,18,82,100,143,94,248,177,174,30,210,26,215,211,102,11,95,129,103,173,92,37,
  252,100,218,131,23,189,133,175,135,192,218,105,13,183,87,219,252,84,161,75,178,97,94,67,
  251,80,39,83,127,120,195,15,138,97,254,46,213,86,201,166,165,56,34,217,164,140,122,60,
  35,167,11,127,202,63,132,31,176,42,23,105,34,108,226,88,198,194,249,208,86,224,225,141,
  207,214,36,150,19,174,232,47,231,141,106,220,180,37,94,221,244,62,155,250,0,195,149,64,
  250,148,111,227,214,132,29,5,24,180,100,148,193,64,35,234,204,39,102,202,154,14,118,135,
  150,114,140,84,97,70,25,142,46,71,102,170,36,206,143,51,157,85,253,79,187,195,6,159,
  226,52,42,118,216,62,213,67,103,240,188,33,115,168,85,227,231,47,12,79,81,229,192,153,
  106,59,165,241,245,151,140,219,67,248,159,26,235,69,183,254,67,248,151,110,13,249,205,130,
  241,225,103,80,80,99,188,197,238,38,246,230,141,113,38,187,69,42,51,114,46,108,130,220,
  196,241,131,51,1,94,10,98,55,127,228,204,178,212,208,207,57,136,253,54,186,77,170,233,
  187,175,104,121,55,116,239,254,60,133,190,117,157,219,137,92,138,142,53,200,6,202,181,246,
  161,53,117,152,37,144,24,222,37,200,73,129,134,52,187,97,55,149,215,158,140,37,115,254,
  36,175,150,33,178,15,210,235,69,100,31,121,29,99,144,238,134,166,100,152,135,172,130,189,
  80,45,29,83,118,55,85,11,237,230,10,134,131,86,200,104,12,189,137,0,184,65,106,152,
  205,94,115,74,233,26,18,45,247,148,88,161,238,20,201,129,244,65,15,53,122,112,93,156,
  194,251,67,183,119,13,1,121,221,209,3,93,82,239,173,98,8,16,122,173,21,218,84,103,
  163,245,155,190,254,40,161,30,252,13,228,233,78,208,10,237,83,246,11,217,33,54,248,195,
  230,134,119,83,160,169,245,90,141,183,93,23,104,38,168,202,26,111,191,62,64,158,92,27,
  227,46,61,52,99,186,182,156,88,4,220,164,212,64,225,246,66,208,143,68,1,76,231,118,
  243,184,155,218,196,36,226,226,96,137,20,20,139,156,41,67,148,82,58,194,192,199,118,197,
  47,35,106,67,112,187,50,125,200,211,54,98,218,5,100,58,61,101,237,196,60,88,177,243,
  197,52,34,163,128,210,155,210,24,216,43,63,208,200,170,179,77,98,136,209,10,172,67,32,
  134,232,18,183,17,140,52,149,131,71,98,38,16,47,58,68,232,167,39,226,140,123,210,103,
  22,119,49,73,221,51,44,116,229,204,244,29,245,15,91,129,238,37,173,129,41,172,112,13,
  255,134,165,239,103,12,184,8,118,21,115,222,213,103,193,217,134,123,247,49,194,53,19,42,
  32,85,144,199,58,37,171,31,212,44,69,55,22,73,15,161,57,71,198,69,109,180,144,90,
  57,170,133,231,36,129,0,181,112,228,164,190,42,207,218,22,86,47,99,159,177,138,93,210,
  90,163,87,177,207,159,46,225,46,10,107,88,155,82,201,141,6,112,81,212,31,27,178,40,
  124,102,1,9,235,151,124,10,15,170,135,210,191,182,12,204,40,4,125,166,230,38,0,152,
  142,128,192,185,74,68,105,77,126,54,24,115,104,103,234,117,104,137,141,92,128,14,94,17,
  181,200,215,210,191,63,148,187,142,60,168,199,50,118,198,159,142,101,120,107,77,226,0,207,
  110,36,169,25,71,15,51,181,134,163,225,188,98,184,209,36,51,226,122,98,96,230,85,59,
  148,211,13,181,37,43,249,96,82,206,157,164,23,236,68,94,86,125,184,36,198,244,255,123,
  218,248,73,92,59,79,37,12,169,195,114,120,198,229,172,123,179,9,172,47,62,72,227,159,
  89,17,192,84,255,254,172,198,199,167,112,83,221,12,255,210,201,100,177,84,174,30,128,245,
  23,138,5,102,150,40,128,115,159,145,101,72,240,27,118,61,59,1,217,66,229,97,34,157,
  102,104,252,215,73,2,225,145,255,13,184,137,243,71,13,32,1,215,51,32,241,3,35,161,
  39,207,44,13,122,90,67,22,183,228,12,125,180,69,50,52,11,117,156,162,89,69,150,109,
  138,57,100,83,86,201,48,53,199,198,139,42,152,109,170,62,8,73,96,43,255,4,61,201,
  108,230,119,81,18,99,58,9,100,78,210,17,203,188,69,39,47,165,78,251,255,205,60,218,
  155,177,43,102,186,174,104,36,15,243,109,240,106,46,238,224,54,209,211,196,43,153,185,1,
  243,63,75,97,217,255,85,107,214,75,116,97,247,184,189,229,168,111,97,249,177,5,72,179,
  250,96,28,95,254,76,108,57,86,125,40,83,190,28,202,148,193,224,119,35,60,249,50,252,
  243,243,95,187,67,57,242,37,115,228,225,37,136,31,143,101,7,194,12,46,19,80,144,209,
  178,40,216,240,240,169,57,241,234,111,76,15,197,178,191,216,59,96,71,194,28,49,108,35,
  229,7,111,123,153,78,18,115,188,47,206,110,140,167,145,81,140,119,118,227,141,253,31,202,
  112,103,55,254,65,175,227,70,155,106,146,168,25,243,27,98,158,104,233,100,18,232,204,75,
  162,98,201,55,52,223,36,253,92,51,252,116,78,17,23,192,102,135,179,128,38,127,232,115,
  16,21,230,131,207,127,195,61,54,146,233,231,197,99,0,142,53,37,119,104,18,176,213,221,
  57,245,9,186,191,213,210,147,85,95,22,22,207,177,80,243,59,129,206,49,39,113,11,167,
  79,190,9,27,176,0,192,118,56,18,9,221,47,16,76,138,187,64,32,68,28,196,237,119,
  57,100,20,121,106,154,194,13,87,114,241,64,17,90,82,208,182,220,114,212,200,38,57,237,
  232,98,207,137,61,31,246,217,249,191,245,196,167,136,30,151,164,228,169,171,166,247,35,82,
  220,109,26,158,102,97,158,208,190,175,99,230,133,161,178,98,229,72,2,177,80,212,248,8,
  249,59,196,5,208,3,250,82,202,117,11,202,167,9,62,126,172,222,118,54,157,144,29,245,
  15,205,182,234,180,17,52,163,223,12,149,9,183,248,237,230,167,245,48,106,0,159,27,11,
  123,218,250,61,221,69,196,201,200,201,209,16,55,213,158,151,111,57,208,141,159,58,13,169,
  171,206,0,35,135,215,111,171,94,137,85,183,244,233,109,51,121,154,145,20,163,73,17,151,
  83,89,64,209,108,122,212,220,251,172,166,215,103,61,247,94,254,39,118,156,122,157,29,187,
  74,155,130,35,246,143,238,66,236,116,121,247,247,186,59,174,3,211,78,7,118,99,75,72,
  226,166,83,23,206,129,145,164,41,132,231,53,93,225,115,116,34,86,21,136,101,188,219,145,
  25,70,98,170,126,38,123,235,130,83,161,169,203,123,30,186,149,222,84,248,21,181,198,58,
  172,94,112,237,174,175,141,74,243,100,152,70,51,193,102,227,21,42,78,99,185,120,194,218,
  155,19,71,193,217,243,20,156,215,74,193,57,82,187,169,181,166,175,211,79,126,251,61,245,
  228,110,146,118,146,85,75,223,254,127,162,157,28,165,102,252,29,141,166,213,54,38,234,64,
  88,219,78,196,69,59,158,132,124,82,65,218,18,70,19,45,165,29,237,24,105,37,97,130,
  233,186,72,39,175,232,11,226,20,253,143,46,69,107,169,208,121,21,187,71,55,138,82,118,
  9,11,173,33,43,75,201,106,9,172,128,149,165,89,85,18,117,156,162,242,148,117,43,171,
  114,172,89,149,103,173,209,196,249,28,154,73,122,205,177,122,214,98,146,158,117,51,81,207,
  250,64,167,124,195,245,172,228,242,62,89,115,122,30,122,179,198,209,156,202,244,193,241,209,
  137,163,56,189,20,197,41,241,202,187,196,168,9,77,237,215,62,199,154,33,168,171,222,191,
  63,31,181,213,53,53,33,37,109,38,237,152,210,195,134,15,177,136,242,130,91,173,32,142,
  5,183,176,190,240,215,205,201,27,7,203,34,111,191,57,127,95,252,96,247,197,43,159,95,
  183,45,78,8,12,241,65,86,124,98,223,103,186,43,103,102,243,247,64,62,208,95,187,87,
  30,14,125,51,76,2,62,109,97,255,75,55,208,125,99,95,72,83,65,118,208,130,161,240,
  95,136,151,162,135,154,240,95,131,16,31,174,10,127,200,107,151,119,172,64,36,74,228,100,
  228,155,247,239,177,27,21,211,7,16,29,214,249,250,24,209,38,66,197,161,110,76,24,198,
  68,156,8,21,163,49,21,154,38,191,133,81,145,19,212,36,144,47,170,23,15,177,42,237,
  119,144,203,39,186,35,62,48,157,240,137,87,77,67,172,131,50,13,163,124,52,247,187,196,
  245,65,194,2,194,88,78,26,138,7,15,11,120,255,61,242,2,224,205,48,9,238,116,4,
  178,162,175,127,50,150,46,54,127,107,172,60,0,111,100,136,255,16,243,102,182,153,17,253,
  93,92,39,64,34,155,45,37,217,198,193,208,185,185,179,136,157,252,115,73,247,49,244,156,
  118,8,166,106,238,130,92,210,139,118,201,91,104,191,121,69,100,205,222,197,146,93,138,46,
  217,228,41,6,117,157,21,219,7,212,213,114,110,147,79,92,59,206,213,197,10,117,182,231,
  216,127,231,155,1,132,25,36,66,5,16,49,99,239,27,99,33,99,140,99,12,118,254,254,
  27,61,35,235,33,47,75,249,19,162,60,182,83,243,77,66,79,146,195,66,121,74,30,194,
  24,142,203,88,1,74,54,32,74,157,195,61,102,13,136,79,22,94,113,183,152,178,241,30,
  238,6,17,187,219,61,26,205,149,238,212,69,76,35,52,179,225,220,16,163,240,154,126,93,
  26,216,74,244,74,19,62,95,37,128,48,70,138,118,20,93,255,59,63,146,130,223,191,63,
  249,148,84,194,68,240,252,206,150,179,73,45,125,85,219,122,44,107,191,194,49,164,23,122,
  199,110,216,196,7,164,119,175,140,236,222,247,55,10,135,22,159,106,184,196,3,156,182,143,
  105,122,151,60,177,163,131,255,152,235,200,177,249,202,53,83,187,184,246,234,201,17,186,186,
  21,46,192,96,218,72,254,151,34,169,43,250,242,182,118,112,106,131,19,101,117,150,77,202,
  233,225,238,57,189,80,11,114,108,180,189,174,241,176,6,142,153,155,185,165,157,45,173,180,
  198,192,123,3,176,68,80,209,250,73,53,3,143,50,34,163,228,14,49,230,71,108,216,94,
  140,250,138,6,43,118,19,80,174,204,94,187,151,99,181,237,227,53,174,190,6,30,99,3,
  43,85,156,173,23,14,101,109,189,208,225,109,189,48,145,185,33,217,225,110,62,176,127,58,
  144,187,255,194,113,220,253,221,97,76,168,248,242,50,126,150,69,230,232,67,195,55,70,107,
  14,187,64,205,27,178,28,219,107,141,101,139,127,164,154,145,187,220,254,123,212,230,37,29,
  94,194,217,222,4,57,169,43,244,122,32,155,201,202,246,97,161,48,21,185,62,233,46,58,
  30,188,137,27,196,110,135,199,178,137,90,199,216,131,145,170,87,160,184,159,162,203,110,66,
  147,106,121,243,47,144,139,78,39,182,188,165,254,56,137,176,67,210,17,248,61,98,109,20,
  143,7,69,79,147,167,33,165,100,180,184,152,26,56,190,122,50,12,94,222,142,243,80,96,
  6,84,180,80,68,161,77,118,199,170,143,137,102,230,39,113,51,115,15,220,104,155,243,32,
  138,38,235,20,225,57,150,21,183,18,31,171,160,59,137,221,1,30,162,7,136,140,106,130,
  109,153,215,43,125,25,181,50,162,140,54,52,203,37,214,69,71,143,59,80,229,36,187,18,
  25,82,227,176,5,140,219,58,142,71,20,43,59,183,73,118,110,211,134,173,205,27,182,215,
  75,180,44,206,212,227,188,222,63,14,210,26,246,104,127,201,224,66,49,233,40,214,137,97,
  11,239,250,150,191,94,25,221,136,146,231,149,142,117,145,195,112,241,198,181,152,117,192,195,
  70,34,253,219,130,217,46,47,29,44,8,74,188,152,177,226,146,174,225,158,17,14,235,227,
  255,197,118,189,142,197,30,117,174,59,221,135,206,132,199,93,165,3,83,174,131,208,173,48,
  238,195,35,32,63,0,227,86,15,242,37,112,70,215,142,35,119,49,35,158,60,204,157,103,
  55,85,215,231,180,13,225,59,235,181,118,251,12,26,97,90,98,146,115,82,233,192,120,91,
  73,225,230,195,179,114,89,146,216,146,192,201,76,2,22,92,143,241,29,115,146,126,201,93,
  143,98,147,39,226,34,34,9,120,254,228,37,177,81,249,235,151,4,247,40,127,80,217,218,
  125,216,93,109,195,227,47,24,233,134,35,58,163,124,41,209,229,201,67,99,152,23,14,220,
  111,189,118,78,34,175,173,83,7,163,225,100,193,219,120,13,26,6,106,154,64,33,154,230,
  144,124,206,52,231,90,16,60,146,252,115,184,138,115,183,249,175,144,63,134,57,25,9,134,
  56,65,177,107,212,176,2,122,165,242,196,42,132,93,10,135,249,85,65,23,41,130,164,193,
  229,9,221,221,254,253,125,135,216,229,39,126,41,55,94,224,235,189,65,186,63,9,226,149,
  95,94,70,210,4,121,13,240,200,226,183,198,103,154,181,53,111,175,152,60,170,163,135,244,
  141,227,249,111,26,204,127,207,72,190,109,3,255,61,114,83,197,220,169,111,58,215,239,160,
  172,186,78,158,230,134,88,130,132,216,191,126,120,108,72,166,37,7,38,47,47,94,140,66,
  215,139,151,2,106,239,248,7,215,70,91,133,27,148,35,152,239,163,150,254,173,195,154,128,
  175,224,141,90,182,72,222,57,209,244,205,161,150,201,59,173,237,116,84,214,165,107,54,99,
  133,93,180,57,73,7,152,116,57,50,205,42,192,82,2,142,93,46,249,13,92,242,27,184,
  228,55,225,146,230,128,65,110,232,250,14,223,146,161,157,34,128,184,3,112,23,0,119,63,
  69,43,231,118,1,127,72,237,211,93,203,43,93,26,83,220,126,216,55,135,87,121,254,189,
  142,196,209,18,235,197,183,225,189,120,107,23,146,203,11,153,164,157,102,64,248,61,49,159,
  255,180,203,77,120,86,59,19,101,83,128,175,167,115,221,112,4,97,197,51,227,219,168,36,
  18,18,1,66,132,203,62,29,49,237,53,148,35,194,140,131,195,33,78,173,146,123,104,93,
  92,196,220,76,81,32,140,113,172,102,220,71,211,145,33,60,113,135,78,185,27,37,132,229,
  134,55,195,29,204,8,175,31,207,108,71,130,77,216,82,238,106,189,213,238,240,235,74,187,
  238,117,165,221,228,235,74,187,174,218,202,133,197,78,134,190,30,198,216,19,235,208,171,226,
  157,255,141,26,168,54,91,8,21,212,6,177,240,102,101,96,82,72,247,111,193,155,16,249,
  90,141,85,80,112,85,195,174,123,156,152,83,178,17,235,251,240,74,180,202,79,167,95,171,
  226,126,225,219,141,100,221,51,70,99,113,253,90,141,69,250,247,24,249,43,152,245,163,211,
  237,24,191,38,167,54,99,86,231,164,109,253,55,109,180,56,49,28,250,111,131,158,182,102,
  100,73,135,109,141,41,47,234,74,202,238,37,192,111,159,95,201,112,137,115,216,131,45,152,
  138,4,172,135,113,47,226,14,241,222,47,126,91,147,61,246,167,69,159,243,42,191,255,73,
  55,118,125,114,170,135,241,99,24,166,31,54,184,67,112,231,100,212,3,239,221,112,200,212,
  172,147,62,32,96,219,24,66,111,157,175,118,38,161,243,89,112,232,97,132,98,127,252,38,
  82,190,253,171,112,242,45,121,70,125,11,216,33,236,255,208,76,25,49,77,198,78,132,17,
  53,29,58,255,150,68,214,17,142,23,145,137,79,194,4,153,216,30,186,148,64,255,164,205,
  87,221,43,125,186,206,149,52,253,195,38,242,180,244,131,198,19,154,224,180,57,140,161,56,
  23,65,205,26,56,70,16,29,177,159,84,81,68,30,82,114,48,24,109,252,11,187,184,130,
  27,24,136,2,197,6,75,100,78,128,226,130,4,161,176,2,9,191,187,50,73,180,99,172,
  92,146,117,138,118,161,74,149,121,146,215,168,21,246,92,93,219,169,136,64,255,79,118,104,
  6,7,5,49,186,116,69,255,111,134,144,186,164,153,237,194,170,65,227,184,43,6,249,163,
  215,143,90,56,214,131,232,63,89,125,187,177,227,57,89,61,95,223,172,225,77,113,21,141,
  177,64,127,67,27,227,108,242,248,112,118,90,187,168,27,19,14,112,60,162,212,13,243,184,
  74,98,247,253,251,221,209,250,136,88,243,19,251,63,84,43,241,186,234,118,73,79,38,21,
  153,2,160,237,110,123,224,42,239,172,231,99,243,126,242,203,28,44,88,66,39,153,200,35,
  115,242,102,167,192,149,253,116,1,160,169,223,1,1,2,167,232,55,195,9,46,54,46,201,
  43,186,71,114,22,27,113,145,175,171,141,81,205,226,219,228,180,40,179,249,71,148,231,147,
  93,133,22,61,146,51,219,173,208,29,71,178,26,34,19,27,253,75,42,19,229,227,14,50,
  117,94,252,3,166,110,181,141,57,24,106,209,121,70,58,251,18,77,38,252,152,188,74,67,
  5,11,101,151,150,157,33,215,254,231,52,80,226,169,69,231,185,133,189,194,0,123,130,122,
  100,19,150,128,142,34,185,193,30,94,62,54,158,216,143,240,238,176,241,250,24,157,141,215,
  71,254,28,199,46,159,255,215,241,203,127,5,241,254,175,224,182,66,246,111,100,95,195,144,
  108,100,236,223,33,235,182,93,206,255,137,245,205,27,38,193,105,83,108,246,180,200,193,219,
  99,207,83,23,185,117,99,11,251,178,136,32,129,17,70,28,25,107,175,145,224,169,222,68,
  2,115,180,41,111,138,86,168,56,11,41,129,225,154,76,73,195,174,97,188,19,62,97,223,
  54,160,166,54,247,66,36,185,123,213,30,50,35,0,231,184,103,15,110,124,229,69,59,20,
  75,185,241,164,143,27,174,167,57,215,1,50,203,83,74,119,193,138,81,168,40,114,78,184,
  179,216,253,7,39,124,133,107,210,175,146,73,141,123,14,202,160,223,30,29,112,71,162,193,
  148,210,18,184,37,117,205,150,239,48,108,195,111,163,147,78,116,152,221,199,5,184,9,80,
  12,110,3,58,49,190,46,107,3,227,255,146,110,200,137,254,4,198,31,117,156,95,246,113,
  51,164,81,95,123,130,70,12,29,193,161,101,118,35,14,247,200,177,63,215,70,231,140,145,
  137,62,161,4,209,218,75,20,223,198,241,45,235,18,208,239,7,48,92,73,157,43,155,142,
  2,34,143,3,88,173,125,34,134,34,90,184,61,215,6,29,166,192,54,191,83,9,109,30,
  244,182,160,132,238,55,73,17,142,155,151,39,209,79,169,244,109,245,1,167,70,172,85,90,
  71,116,254,66,30,66,167,106,234,240,81,225,246,136,204,70,222,5,156,197,209,62,157,244,
  126,254,88,50,154,145,228,13,73,142,150,222,138,38,207,108,228,91,97,60,145,119,156,249,
  175,209,140,217,141,252,151,104,26,233,247,243,141,80,183,207,217,178,244,243,223,45,232,251,
  91,79,203,134,224,178,156,163,252,244,57,89,114,81,47,127,18,186,5,28,102,232,129,249,
  210,112,62,16,215,90,109,185,223,231,145,91,115,23,130,252,163,254,72,223,99,183,135,149,
  74,126,79,65,38,191,204,18,161,220,101,194,154,136,173,115,123,44,10,18,99,87,152,57,
  13,136,9,109,238,122,42,59,230,224,172,80,170,152,105,144,66,152,86,76,107,181,103,12,
  106,249,200,236,238,179,226,155,110,242,96,74,235,72,130,209,203,171,82,96,181,155,80,192,
  11,169,164,75,174,15,47,169,66,42,233,146,149,243,132,146,145,248,87,186,236,195,176,178,
  145,232,87,186,252,153,83,222,237,71,193,73,231,70,233,140,163,254,176,102,155,38,56,29,
  219,217,55,201,59,157,228,150,233,252,162,147,207,49,179,116,198,134,147,97,35,106,25,132,
  59,13,229,155,120,6,191,78,6,248,169,78,174,118,188,100,83,252,216,73,143,24,59,233,
  34,176,116,137,20,81,75,188,41,224,126,178,176,97,146,187,94,178,142,245,166,179,15,90,
  110,139,76,170,91,105,43,90,103,167,235,247,34,154,255,19,142,151,233,247,90,253,30,171,
  223,45,54,162,168,241,98,69,79,189,188,23,100,216,222,90,50,46,103,251,179,225,121,181,
  223,107,158,225,30,123,207,141,175,41,159,209,205,232,177,131,95,236,50,202,68,14,20,153,
  175,60,91,89,175,110,124,173,100,200,235,172,110,215,245,136,10,114,143,50,177,218,241,171,
  171,205,186,213,182,94,93,109,158,170,185,129,93,39,168,159,198,246,233,117,104,122,3,134,
  222,138,32,30,227,183,160,198,171,240,26,164,120,21,134,160,67,207,212,174,16,211,134,250,
  221,87,191,71,221,215,145,212,237,160,151,140,172,98,119,40,178,14,142,15,209,180,185,181,
  131,131,227,12,226,216,155,105,178,49,174,202,76,172,202,126,119,56,2,134,125,230,104,108,
  29,251,157,161,120,43,41,60,109,170,223,53,245,251,83,253,94,171,223,109,61,105,95,137,
  207,70,50,54,237,231,116,219,101,222,175,196,38,195,82,181,80,89,159,201,70,187,38,201,
  186,245,107,175,0,35,241,10,185,90,181,176,186,30,131,232,228,25,78,53,4,179,153,140,
  91,234,58,185,148,90,69,34,133,183,135,81,184,95,236,120,52,76,85,122,232,104,110,233,
  209,82,191,39,234,247,171,250,189,83,191,223,212,239,59,245,251,93,253,126,86,191,187,234,
  247,74,253,126,81,191,97,91,126,27,237,215,81,65,173,159,76,6,91,175,31,183,213,50,
  17,255,227,92,245,243,246,97,124,236,188,108,131,235,55,66,159,31,13,125,222,135,126,242,
  102,232,243,163,161,207,187,208,191,190,17,250,194,104,232,11,62,244,187,55,67,95,24,13,
  125,193,133,254,237,141,208,151,70,183,125,201,111,251,187,55,67,95,24,13,221,107,251,247,
  55,67,95,26,13,125,201,133,254,249,141,208,51,211,163,81,163,243,53,252,221,183,195,95,
  24,3,223,195,206,213,219,225,47,141,129,239,225,231,203,219,225,103,166,199,124,64,10,232,
  47,132,237,55,126,97,102,220,23,102,34,95,104,252,198,23,102,198,125,129,11,12,101,247,
  29,197,142,91,234,183,239,179,101,55,74,131,102,206,216,91,86,207,110,147,185,114,103,108,
  15,148,132,186,118,128,230,29,233,104,189,9,237,247,11,232,230,182,218,67,229,34,174,160,
  86,57,222,74,185,245,250,99,234,153,157,155,169,57,20,99,61,133,169,154,250,61,81,98,
  77,247,117,152,235,53,147,49,215,27,210,66,110,101,101,221,19,142,107,237,145,75,124,82,
  149,147,222,80,240,8,213,83,216,231,26,51,110,141,238,216,143,68,42,198,49,166,159,37,
  116,111,100,231,142,27,52,8,1,125,218,249,97,76,73,87,232,141,111,133,170,16,14,207,
  202,22,43,219,120,121,25,32,238,205,86,53,255,223,255,193,101,111,106,20,4,110,226,249,
  63,254,227,255,52,219,213,131,110,191,197,42,143,252,196,160,113,62,151,66,36,29,73,8,
  38,50,83,211,19,233,220,127,252,199,203,127,7,219,84,27,81,251,72,125,58,33,10,153,
  158,82,72,201,21,10,62,67,200,153,34,28,250,68,114,88,7,151,152,35,90,56,124,32,
  218,44,52,100,6,106,202,110,175,46,173,154,73,77,160,165,80,21,193,77,16,18,167,30,
  39,254,114,96,7,126,230,147,206,20,240,220,131,255,211,186,152,72,9,68,84,254,59,47,
  93,227,111,253,31,85,153,21,73,248,156,162,187,148,211,173,128,218,48,107,235,127,164,234,
  129,122,125,2,158,0,42,61,213,163,239,188,76,240,64,254,30,96,7,228,180,3,146,7,
  224,234,60,127,222,174,245,251,52,172,125,248,144,59,199,105,4,244,92,172,13,83,144,121,
  78,136,126,12,49,64,46,157,87,254,226,62,124,49,226,56,207,38,108,210,251,75,11,42,
  200,84,24,136,35,60,15,156,158,100,164,1,235,228,233,66,205,94,47,21,234,236,116,46,
  21,90,192,164,218,182,111,191,126,133,230,19,54,7,47,236,124,206,111,145,3,36,210,54,
  11,35,29,248,253,124,121,33,149,36,250,152,10,99,205,86,188,1,71,21,6,17,110,71,
  26,0,123,14,149,99,175,214,39,63,70,131,86,227,129,212,227,129,116,240,178,147,122,166,
  73,3,215,12,151,53,104,44,179,91,213,0,167,16,77,82,105,171,148,237,106,160,72,185,
  159,125,182,67,153,125,198,137,205,125,35,235,54,5,225,102,12,145,234,252,198,212,79,149,
  44,228,105,211,31,112,142,147,206,57,195,135,246,60,65,15,73,63,173,212,204,116,48,3,
  139,166,142,142,162,62,97,10,190,176,250,249,21,180,64,72,115,111,219,88,53,169,215,232,
  151,224,139,162,181,9,164,64,113,219,159,88,239,120,100,199,212,130,115,5,54,255,21,234,
  32,8,80,152,202,173,116,120,93,172,201,99,55,63,73,90,243,246,71,14,155,49,137,195,
  230,76,112,46,89,247,242,115,43,63,138,90,235,242,83,149,159,167,188,58,0,248,122,200,
  199,26,107,208,98,195,85,24,29,2,4,55,140,159,171,243,224,130,194,250,60,72,132,145,
  245,110,135,122,97,188,47,82,248,139,67,129,85,150,159,51,50,211,217,163,63,235,12,224,
  62,12,30,37,167,34,63,59,156,188,214,200,237,24,218,224,20,28,68,81,79,143,116,246,
  81,98,246,102,254,116,39,56,250,17,28,115,98,185,27,108,8,212,45,254,145,193,21,234,
  91,197,125,38,115,91,85,6,170,193,241,206,234,56,22,211,74,238,3,26,218,14,197,55,
  51,137,170,151,189,110,187,237,106,188,239,132,186,175,242,103,167,119,38,24,234,149,107,80,
  125,197,77,42,244,3,42,145,191,74,7,87,172,207,102,141,253,97,237,137,79,200,160,19,
  79,248,202,86,175,117,251,207,191,68,80,98,31,217,198,81,214,63,7,77,80,52,232,156,
  19,187,214,192,219,195,129,106,189,241,88,186,72,221,225,233,246,62,44,179,163,60,86,225,
  19,252,143,25,215,167,222,19,188,42,156,94,253,200,61,121,177,151,158,66,125,218,232,193,
  8,238,228,152,50,56,135,175,185,116,128,82,116,196,84,11,207,47,249,106,70,234,153,189,
  53,220,241,97,99,64,238,107,178,94,245,23,247,232,242,59,102,100,63,249,218,93,31,183,
  245,206,195,201,224,93,58,24,89,130,163,218,140,47,134,89,61,178,16,28,129,54,126,54,
  94,81,100,252,247,164,220,152,15,114,246,247,161,217,140,48,113,110,216,23,159,167,147,65,
  203,113,89,118,7,139,139,187,79,103,218,224,226,78,219,255,97,228,137,146,174,204,154,144,
  162,119,197,9,240,68,99,5,198,214,1,70,82,87,24,9,111,182,6,55,83,138,179,218,
  239,144,167,174,139,180,186,58,121,65,176,105,245,161,147,44,247,0,44,5,111,162,117,143,
  175,41,46,215,247,120,209,46,5,33,234,222,114,152,247,216,92,39,152,7,228,239,239,16,
  241,13,187,169,199,180,36,145,127,188,212,186,58,172,92,215,214,129,176,35,12,42,127,184,
  203,220,93,190,162,24,77,14,4,218,29,224,21,127,65,172,63,187,221,27,188,208,15,222,
  132,162,15,122,221,171,6,147,32,44,6,122,173,71,180,72,184,33,162,74,39,81,51,136,
  136,229,113,26,181,151,244,139,62,204,115,76,21,202,224,249,141,205,26,173,22,222,244,238,
  229,239,162,157,21,27,1,117,36,254,245,112,175,214,193,41,50,60,169,173,215,58,20,170,
  81,134,123,194,177,23,153,232,19,112,21,60,250,214,0,162,91,187,230,92,241,176,129,130,
  56,78,111,48,91,160,243,113,175,25,221,223,109,70,79,195,149,227,115,62,32,31,209,146,
  102,180,37,9,71,148,196,55,134,53,220,107,244,121,254,206,130,93,171,245,27,236,88,59,
  1,226,165,246,176,119,153,173,59,53,100,241,76,40,127,155,215,211,227,137,44,179,152,197,
  250,43,110,170,31,144,177,213,173,133,198,99,157,0,171,106,139,148,133,72,18,10,245,109,
  127,117,161,90,255,169,115,62,225,118,23,19,171,143,97,234,187,123,189,67,217,32,122,115,
  141,152,70,252,34,156,199,47,135,101,187,204,107,88,25,203,184,18,74,120,108,114,104,254,
  152,207,68,25,100,188,136,229,142,241,188,33,172,49,120,152,122,236,145,65,11,184,7,132,
  10,21,3,184,246,80,107,65,4,134,53,208,117,227,235,161,205,6,18,31,5,181,14,203,
  33,142,211,212,28,7,43,219,251,247,147,114,219,196,178,11,166,64,114,149,237,147,11,28,
  158,118,195,46,223,0,122,86,11,42,51,188,35,245,251,40,191,185,7,17,240,65,116,72,
  121,208,38,83,176,99,141,122,137,200,54,226,206,38,176,212,58,117,14,91,217,227,14,125,
  192,38,149,250,89,156,47,243,138,126,140,8,194,108,245,34,6,52,0,39,14,169,101,127,
  33,105,176,58,232,91,174,149,237,189,228,110,243,145,185,3,148,92,230,111,167,18,177,144,
  58,14,105,128,92,203,137,114,72,210,194,51,123,237,239,103,79,47,127,188,164,99,124,61,
  227,242,245,75,45,131,203,182,214,188,110,91,46,95,230,217,25,118,198,149,125,22,59,167,
  108,181,19,48,203,110,116,2,247,82,125,150,160,60,182,198,65,57,10,3,145,127,94,243,
  243,68,8,112,173,222,178,102,44,2,107,132,149,13,167,224,14,29,52,107,29,254,6,202,
  190,12,229,107,96,160,48,124,171,245,87,230,178,211,65,220,98,56,123,57,5,171,39,216,
  225,113,218,49,237,156,250,108,123,23,36,153,3,15,47,61,202,150,239,13,181,94,247,181,
  23,231,166,9,136,246,217,116,210,237,112,80,35,207,192,89,236,5,120,156,178,106,110,104,
  140,58,184,188,72,92,102,137,96,235,46,235,22,170,4,227,150,129,73,162,204,51,189,136,
  96,157,24,67,154,117,215,98,83,136,197,75,74,34,209,87,214,73,32,213,49,228,50,132,
  200,18,200,165,254,38,114,169,255,22,185,212,127,147,92,234,195,201,165,28,191,0,169,55,
  137,36,103,65,64,174,241,50,9,187,99,181,155,174,229,133,183,247,181,23,127,95,144,72,
  117,211,44,110,154,45,50,236,151,68,254,196,210,148,32,128,78,7,163,37,63,89,210,208,
  82,187,220,23,58,131,86,175,203,190,170,215,32,135,215,247,96,202,232,174,253,180,158,235,
  181,92,203,1,216,242,198,43,89,144,158,107,144,184,24,113,51,21,83,104,56,219,63,104,
  181,238,236,45,185,43,236,16,174,62,221,169,237,69,93,239,20,174,244,78,1,243,210,100,
  98,239,71,43,137,221,52,98,2,229,142,194,191,41,166,46,54,16,71,161,217,65,28,133,
  222,22,2,229,172,3,90,253,69,44,217,195,190,199,89,137,95,3,178,142,66,217,152,154,
  46,28,147,93,249,113,104,183,57,199,202,177,237,49,154,182,167,18,211,207,123,98,76,79,
  147,30,32,142,195,220,25,150,171,107,99,214,188,119,122,204,33,141,69,166,146,183,167,208,
  43,234,124,92,18,232,235,143,180,41,70,127,115,143,33,121,66,152,114,250,44,55,160,190,
  242,180,95,13,190,200,175,29,8,200,104,119,193,21,113,161,231,175,178,77,232,222,200,62,
  67,171,158,83,87,144,72,40,225,164,219,107,215,211,193,151,33,197,176,185,118,203,137,66,
  6,78,229,105,12,216,28,180,210,77,125,161,235,130,249,43,18,65,188,61,205,84,67,5,
  201,192,237,88,116,120,68,254,3,89,32,158,102,230,126,252,149,162,223,233,31,192,67,176,
  21,70,18,17,69,39,248,30,230,233,109,153,94,254,194,195,60,98,4,55,84,210,71,147,
  116,34,165,150,116,18,188,198,96,59,130,81,67,18,85,60,230,36,92,29,123,104,252,9,
  251,202,47,252,0,101,65,181,145,63,10,255,74,125,60,9,63,208,85,234,189,6,238,62,
  255,137,55,86,90,184,136,192,74,65,135,36,240,251,0,213,132,209,222,223,77,221,221,215,
  232,218,181,188,176,108,131,43,132,83,33,89,48,182,145,243,21,215,208,188,132,239,169,42,
  39,184,160,223,0,216,169,182,211,97,27,75,212,70,204,112,47,39,77,166,237,13,230,57,
  130,71,161,179,187,248,120,112,221,248,56,210,151,162,105,222,231,174,134,101,59,158,77,246,
  129,226,15,213,70,112,220,128,49,42,61,52,26,249,98,227,35,172,75,11,48,94,253,0,
  242,255,72,184,105,194,84,246,207,173,240,175,227,198,159,251,184,150,215,110,232,151,92,66,
  123,73,166,62,192,119,110,27,28,6,33,213,104,4,226,218,118,31,159,25,219,234,120,182,
  197,148,99,167,122,200,115,41,253,124,165,102,240,74,116,180,28,124,167,179,126,166,14,65,
  192,224,177,91,240,137,200,12,224,111,12,229,139,99,19,188,206,74,135,124,108,39,231,113,
  254,156,48,200,59,56,237,131,214,159,121,225,69,173,151,187,137,233,240,83,55,246,116,224,
  111,81,0,58,41,233,224,198,28,17,252,45,58,186,188,77,193,134,245,152,63,144,63,146,
  159,29,249,185,66,242,5,167,94,112,34,253,5,7,76,109,224,171,82,225,215,175,45,126,
  190,144,3,139,68,217,201,52,34,43,117,2,253,217,44,215,131,60,181,161,160,65,157,36,
  160,240,93,217,11,0,139,253,107,244,217,121,251,181,16,236,120,185,94,217,247,31,231,131,
  163,81,217,179,154,23,222,169,176,66,196,4,143,245,65,71,14,100,115,12,22,111,21,89,
  122,45,121,12,189,197,4,229,48,43,177,36,112,225,71,39,58,251,204,74,131,96,64,185,
  157,70,7,147,39,235,78,194,100,173,24,221,194,29,137,24,118,126,8,157,0,187,21,108,
  205,248,57,123,23,64,69,149,101,117,85,64,250,169,172,104,171,128,202,85,34,123,238,129,
  179,192,175,218,117,229,41,140,206,7,205,11,92,122,205,166,252,220,200,114,98,232,222,178,
  40,147,162,231,78,4,162,83,226,77,60,88,153,207,91,72,41,242,110,241,111,97,130,193,
  176,113,16,53,97,185,255,231,204,159,124,111,8,170,234,78,42,243,215,221,240,149,18,139,
  91,90,107,20,51,138,9,208,193,65,132,3,24,121,237,216,81,31,105,17,54,34,33,254,
  145,186,52,4,83,215,60,67,139,139,237,151,184,8,236,31,30,64,95,116,105,121,7,130,
  181,99,55,83,183,101,239,160,150,53,217,245,72,174,171,235,175,199,107,170,111,227,246,19,
  75,156,229,6,238,223,36,106,211,162,76,44,34,206,170,138,116,120,57,76,164,229,131,205,
  99,247,40,134,49,26,19,132,239,76,53,210,68,191,136,167,22,57,249,179,179,228,78,173,
  24,64,238,125,158,79,75,142,113,110,213,232,29,16,169,202,153,69,21,44,241,222,211,30,
  99,62,221,243,249,86,63,103,17,22,215,115,187,119,211,202,254,222,78,237,29,61,77,93,
  57,109,68,182,63,50,56,93,177,33,141,12,167,82,73,248,88,52,9,39,215,84,111,218,
  242,176,45,226,97,91,225,39,3,8,47,90,188,254,78,50,222,233,86,200,2,153,54,64,
  209,93,73,35,173,174,49,65,39,120,169,239,110,92,58,8,103,183,58,87,59,232,79,93,
  6,223,57,210,119,30,55,173,204,153,241,22,177,157,233,36,204,232,187,54,64,203,109,67,
  174,160,105,191,139,183,234,52,92,109,6,141,115,135,68,124,177,100,13,1,114,147,250,146,
  59,241,61,216,134,234,44,18,18,166,94,13,228,8,49,181,5,225,31,137,145,19,202,128,
  161,224,14,15,103,42,190,117,1,49,155,205,56,128,4,17,5,105,59,110,100,1,91,208,
  50,56,164,25,14,135,103,135,197,225,77,241,184,147,4,9,219,251,84,156,165,37,84,241,
  120,90,66,190,35,77,58,221,229,11,130,237,6,236,41,240,231,137,254,168,88,55,13,115,
  167,203,140,220,177,183,34,152,222,210,90,29,235,247,177,237,246,177,219,235,99,213,105,222,
  112,137,162,213,82,47,111,198,128,114,189,137,50,199,188,155,141,154,56,178,0,101,170,61,
  149,137,213,55,201,20,242,177,47,188,98,18,66,136,50,139,56,234,131,24,243,249,201,230,
  237,253,199,238,109,216,186,33,231,73,184,17,247,148,126,190,149,139,155,70,85,153,83,115,
  228,214,176,158,157,142,104,90,100,43,197,145,86,182,66,114,183,65,247,27,233,186,41,63,
  135,218,237,230,205,20,155,134,224,182,86,95,93,200,100,209,135,239,80,199,26,44,221,253,
  88,59,135,96,217,167,6,163,69,172,94,8,99,30,51,130,88,75,223,52,163,245,177,22,
  90,241,93,5,140,192,180,188,56,253,142,9,210,134,187,234,20,205,120,182,85,9,56,17,
  98,187,242,133,167,112,33,220,84,166,181,76,106,21,55,77,51,216,147,240,133,174,212,170,
  70,61,81,163,158,156,109,247,147,105,19,109,219,79,159,192,100,120,115,140,7,104,9,12,
  187,124,12,61,215,129,250,240,24,42,213,43,57,43,126,233,2,201,221,6,179,103,156,99,
  215,113,164,136,227,134,250,65,187,214,105,192,179,203,16,5,204,45,103,235,194,147,114,164,
  124,245,146,86,54,19,76,102,187,50,227,47,234,57,209,249,172,118,90,50,236,197,46,142,
  27,239,194,180,89,71,189,28,255,136,172,97,142,155,148,83,65,119,185,2,126,130,147,42,
  127,165,214,8,6,162,120,220,15,115,131,58,79,190,143,25,21,103,197,252,166,237,138,244,
  53,126,215,238,38,184,72,63,107,166,227,88,70,200,100,186,241,228,37,16,208,197,20,219,
  204,200,156,53,188,202,238,146,58,2,240,66,24,47,159,24,109,173,161,224,69,87,194,204,
  73,245,224,232,60,213,74,19,52,4,58,237,54,87,164,0,137,244,146,159,191,16,241,157,
  147,55,157,84,146,229,179,170,86,225,241,118,134,183,41,40,179,65,211,53,124,50,229,234,
  242,238,52,172,79,13,11,30,130,195,160,76,205,107,245,41,250,231,30,49,18,40,129,87,
  46,166,180,97,209,62,124,153,107,227,144,12,127,138,100,2,156,249,181,206,77,233,30,119,
  210,100,22,107,55,103,224,132,38,59,37,249,232,29,116,108,123,181,91,110,164,60,238,64,
  3,232,183,52,146,158,182,96,43,93,230,181,62,204,91,254,177,133,14,46,187,144,104,162,
  165,238,255,37,13,0,207,233,212,107,189,122,12,252,165,6,175,219,240,4,236,212,218,186,
  28,4,60,70,118,217,129,133,188,243,218,109,12,82,53,210,29,102,150,195,240,188,161,84,
  77,49,40,79,17,40,251,236,220,46,9,76,177,213,105,248,99,153,170,217,206,80,238,70,
  173,127,217,168,219,174,116,29,216,7,93,120,211,235,155,170,109,77,82,146,91,190,237,181,
  66,219,184,115,251,85,50,95,235,62,216,79,222,168,217,225,76,37,53,97,104,91,221,133,
  214,216,29,32,245,110,65,53,122,182,121,169,33,148,235,80,126,79,207,241,33,128,3,245,
  109,154,237,245,214,197,5,238,254,38,182,12,164,114,211,130,10,122,192,11,148,126,246,139,
  234,212,180,217,182,209,57,73,173,231,100,89,74,3,196,27,69,156,248,53,205,194,115,208,
  72,201,47,231,84,180,104,196,28,131,15,107,52,81,235,23,83,89,39,48,4,243,98,11,
  250,176,206,238,111,110,53,40,245,108,32,169,119,6,164,159,77,41,11,70,165,241,113,144,
  87,151,83,240,141,62,46,201,211,61,133,134,249,138,83,246,79,210,192,81,75,58,138,100,
  165,45,230,205,178,67,157,194,237,177,111,78,89,175,77,146,234,124,73,143,144,147,145,246,
  91,151,84,169,211,104,138,147,7,20,149,251,251,231,188,1,213,13,141,164,89,254,235,167,
  115,163,163,105,177,218,94,7,220,60,31,185,177,156,72,233,53,28,233,37,22,166,12,151,
  140,13,103,180,239,150,43,218,52,110,188,251,238,213,240,9,138,182,246,247,160,119,13,218,
  121,55,160,157,52,6,237,190,123,53,18,232,190,130,227,37,86,183,221,216,119,159,246,41,
  69,132,174,7,101,210,127,65,134,152,15,138,207,227,136,80,63,30,118,67,150,63,114,135,
  206,250,160,128,29,6,55,145,82,138,32,188,211,129,185,212,73,149,149,175,186,16,101,21,
  238,219,180,39,77,167,101,75,195,226,59,90,64,78,62,224,24,193,120,150,63,100,71,2,
  177,109,155,242,230,50,228,235,78,52,215,212,160,158,102,51,177,139,54,107,83,92,198,230,
  38,162,76,171,107,5,8,60,75,173,94,141,185,35,159,183,186,21,221,116,66,123,155,118,
  45,122,56,245,139,169,160,19,2,155,23,95,97,99,57,60,236,9,85,35,3,222,53,92,
  174,235,177,184,174,98,105,221,196,175,249,201,194,7,221,26,206,71,236,26,81,211,107,196,
  168,5,96,212,10,242,86,70,110,63,221,53,159,198,202,75,7,222,118,238,170,4,84,134,
  73,11,24,83,82,230,135,139,169,166,196,174,165,201,227,49,10,126,115,28,124,155,229,250,
  183,251,137,47,184,109,160,151,63,31,34,223,61,252,115,106,254,85,8,185,31,252,207,45,
  108,111,97,22,22,67,231,255,140,10,48,175,188,233,74,19,74,37,252,223,95,234,127,15,
  35,247,26,35,154,45,251,40,209,169,132,149,254,37,246,236,216,31,234,181,199,68,28,188,
  176,89,65,166,241,113,206,1,127,171,119,75,205,94,173,222,114,214,85,231,221,180,207,73,
  115,64,92,154,29,92,3,83,196,249,254,133,77,9,156,103,253,5,55,33,94,67,163,213,
  75,240,43,185,232,165,145,191,111,94,122,95,55,41,129,243,172,191,238,38,196,107,232,175,
  123,9,126,37,127,112,127,99,155,99,49,40,155,149,7,194,97,116,165,160,161,131,121,160,
  34,25,126,94,143,75,238,54,61,65,236,229,76,161,15,60,28,198,208,228,39,7,46,52,
  35,68,184,41,126,69,157,42,130,132,151,18,169,23,145,83,188,175,122,31,58,76,26,154,
  88,142,253,224,161,63,72,177,52,231,195,244,229,243,54,54,246,231,184,135,166,144,106,222,
  205,183,76,74,224,228,198,17,23,207,114,161,235,46,185,9,241,170,186,35,94,130,95,201,
  199,91,252,163,177,15,37,226,47,49,215,255,116,4,143,137,233,67,26,179,239,109,30,226,
  201,241,102,236,123,219,137,132,228,36,48,158,124,30,201,78,216,105,36,149,136,109,57,70,
  128,49,123,15,189,249,144,232,21,138,110,108,130,39,235,75,18,213,192,249,12,66,80,57,
  146,160,170,24,75,119,164,191,72,14,107,5,122,104,110,255,156,204,185,20,0,39,197,50,
  11,155,22,120,37,118,74,135,73,133,144,236,151,171,92,182,206,175,121,148,177,84,220,220,
  223,36,85,50,101,14,201,214,152,44,87,134,128,168,61,190,18,68,230,135,223,67,77,66,
  126,82,18,32,77,58,145,164,104,85,159,96,147,27,27,251,164,155,55,178,19,9,141,240,
  242,134,66,141,240,36,222,168,240,150,206,144,151,155,100,218,224,38,6,126,25,241,240,232,
  48,232,7,19,66,38,177,160,149,101,89,107,251,160,206,73,30,204,49,137,255,53,141,165,
  72,90,98,203,52,86,162,105,177,218,254,10,30,106,252,88,160,58,37,112,158,77,75,146,
  6,41,140,142,76,232,15,71,152,60,6,36,180,133,88,152,239,89,86,212,106,62,43,152,
  197,243,252,26,9,107,114,52,87,100,192,78,171,223,197,28,191,213,188,192,38,28,55,200,
  146,215,25,17,183,180,156,202,159,119,251,94,170,222,155,250,144,165,44,206,142,146,203,250,
  133,141,108,235,166,216,126,187,169,34,229,122,41,145,122,30,81,91,137,53,46,10,197,114,
  156,210,73,226,141,155,229,42,57,124,33,37,146,24,251,154,47,170,68,19,227,0,146,21,
  43,166,205,209,239,186,25,195,123,26,253,190,151,145,12,44,113,211,90,213,130,251,13,235,
  181,213,198,134,30,157,189,13,189,58,117,68,75,253,236,171,102,100,239,159,99,149,132,24,
  13,107,147,203,36,133,139,49,199,124,240,205,103,110,248,184,36,50,113,96,157,205,122,103,
  117,106,103,78,84,198,22,36,27,42,229,200,128,20,34,80,168,227,180,229,72,95,188,238,
  4,42,81,43,161,77,78,223,177,150,187,227,147,38,88,192,119,164,199,125,186,42,220,163,
  63,53,186,1,220,133,167,112,186,242,82,35,179,133,144,253,235,238,173,126,133,59,139,157,
  77,242,156,97,156,135,239,111,236,236,111,149,157,19,172,54,142,245,207,4,228,94,254,140,
  78,165,177,45,186,201,117,180,22,124,13,119,158,175,245,241,102,57,216,243,118,183,78,213,
  254,41,12,189,235,63,114,123,238,177,254,13,212,86,184,161,124,79,63,170,64,126,47,40,
  143,10,246,245,160,12,43,214,253,198,176,26,254,104,173,4,250,185,149,150,172,75,57,186,
  235,227,185,216,237,201,119,112,96,249,72,118,51,244,105,149,148,127,244,54,162,101,105,250,
  89,30,183,22,115,101,10,225,40,189,228,67,137,29,50,96,206,159,229,164,115,58,54,205,
  154,142,74,179,158,167,10,164,174,192,101,163,50,188,123,226,0,54,103,60,22,18,160,53,
  237,230,216,31,2,32,144,194,24,113,230,6,142,59,227,5,214,17,239,44,24,13,67,188,
  116,187,101,232,10,93,188,220,25,125,109,207,221,29,91,11,236,50,78,130,203,159,186,185,
  178,216,93,215,140,201,118,57,237,94,34,158,168,201,169,63,176,88,214,29,52,174,121,69,
  221,72,55,11,149,132,4,111,31,116,114,71,81,156,251,45,218,185,225,124,183,123,223,111,
  63,193,235,47,25,10,76,232,163,149,137,38,246,158,183,125,220,65,172,157,227,116,8,183,
  12,131,105,119,11,110,198,70,81,13,136,168,108,206,101,20,246,207,169,106,110,52,166,206,
  236,129,252,35,172,110,42,198,182,60,247,248,169,146,123,212,135,223,59,56,251,126,228,251,
  175,108,211,1,77,170,216,118,236,164,141,39,244,35,84,223,132,133,165,170,126,244,105,51,
  119,132,234,245,212,206,41,238,200,63,6,71,68,144,108,152,142,36,188,79,227,253,229,21,
  195,232,110,155,203,52,98,168,71,6,78,79,230,77,226,69,61,75,31,168,227,221,139,11,
  48,49,116,166,44,252,38,222,238,74,90,247,107,58,231,116,0,173,174,152,14,232,190,111,
  230,43,167,124,203,255,34,181,153,206,85,83,155,1,125,131,142,226,131,157,116,160,156,22,
  108,254,145,159,148,177,133,129,134,147,118,214,237,98,191,209,65,226,31,155,116,86,200,60,
  116,22,111,220,14,233,56,123,156,110,60,0,58,230,227,206,7,24,72,245,37,198,213,95,
  155,228,62,101,118,70,218,78,222,89,203,213,3,112,169,66,177,64,174,195,117,148,227,106,
  170,98,155,52,77,241,178,212,133,161,251,179,228,25,244,104,138,187,230,198,85,65,104,250,
  89,53,191,156,55,93,130,139,15,155,166,187,180,114,134,13,65,190,156,45,219,126,173,164,
  36,205,90,13,98,203,112,6,161,223,77,202,80,210,140,151,52,67,73,179,63,64,65,103,
  167,115,94,206,44,229,204,123,73,115,148,180,224,37,205,83,210,162,212,95,242,114,22,40,
  103,217,75,90,228,38,249,205,92,226,52,180,19,1,94,19,70,166,156,94,57,99,25,141,
  29,61,76,185,238,49,202,10,217,64,233,25,188,115,151,132,244,180,253,8,142,113,203,80,
  37,11,209,157,121,139,131,165,95,67,185,98,169,80,201,159,125,152,172,78,126,216,35,178,
  93,63,173,56,161,92,125,79,221,143,9,3,244,232,14,16,87,126,76,234,208,99,90,101,
  78,245,219,45,114,165,144,85,175,231,109,220,244,6,255,70,88,99,53,65,40,131,47,190,
  191,238,163,60,55,119,104,113,209,173,21,200,128,104,130,31,38,54,72,21,151,120,202,59,
  83,184,216,84,107,247,145,46,145,115,20,172,29,17,17,31,169,129,47,74,100,112,29,203,
  223,88,198,232,176,195,61,16,198,122,62,179,96,166,122,5,9,59,121,99,228,83,249,180,
  147,171,24,59,159,188,207,42,206,208,124,12,63,254,102,79,233,175,13,194,184,9,40,199,
  176,44,87,80,54,63,29,231,54,53,148,141,252,209,233,230,15,88,131,251,192,54,100,140,
  211,43,234,33,123,170,30,44,212,119,128,250,61,191,165,161,190,251,244,61,247,78,67,133,
  23,242,173,211,119,63,130,175,224,69,116,239,247,75,126,239,63,215,3,114,194,253,159,95,
  167,56,192,114,173,247,20,28,226,253,67,35,204,237,125,32,167,224,135,100,154,4,89,99,
  253,227,97,248,233,171,102,45,116,185,233,67,158,210,96,187,174,152,1,219,245,184,44,39,
  245,245,181,156,136,97,8,219,133,252,2,200,166,166,10,153,244,72,45,213,235,227,35,111,
  134,248,251,24,73,45,40,176,224,163,86,45,18,219,200,28,201,142,236,133,29,217,103,221,
  83,220,26,85,95,201,78,191,228,252,201,49,150,123,165,206,12,198,242,115,152,176,10,82,
  126,46,205,28,77,182,103,51,94,177,37,167,216,146,91,108,246,215,175,50,199,20,192,70,
  194,171,145,89,112,170,100,102,220,58,115,35,10,46,164,35,92,213,105,170,219,136,185,37,
  183,160,15,113,193,237,212,130,234,149,58,243,92,57,136,75,40,58,44,186,242,247,213,159,
  56,175,193,184,10,222,16,206,224,238,0,23,0,73,102,65,138,47,160,80,32,130,100,126,
  57,180,115,194,39,139,194,30,179,9,13,113,163,24,104,63,98,60,71,120,104,167,38,97,
  229,19,156,89,194,120,112,167,124,200,39,187,185,179,49,225,204,31,210,74,110,181,82,221,
  89,146,124,155,206,213,176,67,99,14,185,7,179,54,72,148,226,13,68,9,44,253,211,51,
  146,189,210,58,188,186,122,215,175,61,121,117,46,244,184,226,37,121,23,233,167,99,16,203,
  48,212,228,13,139,217,191,232,157,17,181,46,11,87,74,44,233,103,207,3,213,155,236,161,
  184,124,251,38,22,122,71,104,123,102,65,166,239,105,102,102,110,97,57,200,204,79,207,47,
  6,153,153,5,184,149,202,204,45,207,204,7,153,217,153,133,5,60,115,202,236,210,52,114,
  231,102,23,41,101,118,102,22,41,203,203,84,114,126,142,74,46,80,249,185,197,12,165,207,
  206,44,225,121,105,14,185,51,75,75,51,4,109,54,131,231,121,78,95,94,6,132,153,153,
  217,89,250,226,44,190,155,89,154,225,175,211,183,50,243,4,103,126,122,9,117,51,51,203,
  242,140,191,211,203,244,93,60,3,218,244,226,52,167,47,2,194,244,28,149,153,93,90,226,
  150,128,152,241,188,188,196,173,154,161,22,46,82,155,231,102,41,55,51,79,233,232,43,181,
  127,153,218,185,56,75,109,155,159,7,228,217,185,12,149,89,156,70,202,12,183,112,110,41,
  51,71,109,150,231,101,106,255,12,253,69,95,22,169,205,179,4,103,153,48,70,217,252,12,
  248,153,204,50,167,207,81,219,150,166,103,25,147,168,53,141,238,49,28,74,159,89,100,252,
  160,60,190,68,159,5,174,240,151,145,129,103,106,242,52,163,19,184,162,78,204,209,103,231,
  169,225,179,139,115,220,209,69,122,158,91,32,48,11,243,244,156,97,240,139,244,217,153,5,
  1,159,193,223,153,217,57,130,188,180,192,77,166,143,207,45,81,99,129,189,101,221,28,100,
  76,115,58,53,115,113,62,195,77,35,52,207,201,48,102,232,57,195,207,139,11,243,193,242,
  34,125,16,35,61,19,44,207,49,121,44,50,218,231,24,138,122,94,100,98,88,96,196,206,
  77,155,103,100,240,51,245,10,163,72,207,75,68,12,76,54,248,18,61,207,114,255,151,168,
  221,25,70,47,90,79,196,48,63,199,45,166,62,103,102,50,243,220,202,121,219,98,194,215,
  244,28,35,124,145,137,100,154,123,190,8,210,90,94,224,34,11,104,194,242,236,60,15,57,
  240,186,60,61,195,163,70,168,153,89,154,157,231,231,5,126,230,81,94,32,84,46,77,203,
  40,19,37,44,206,240,104,206,82,25,166,180,185,229,204,34,163,152,26,185,60,61,205,13,
  102,148,45,49,37,47,112,131,23,185,193,92,102,105,126,153,26,188,192,141,39,132,162,193,
  25,110,240,28,53,56,195,207,51,139,104,48,1,67,131,167,131,229,25,166,173,121,64,92,
  146,241,195,28,8,150,230,121,246,100,50,76,149,68,137,120,158,182,207,212,234,153,153,12,
  207,152,101,162,232,204,60,63,115,95,167,169,228,252,52,125,63,179,200,179,121,153,134,30,
  73,60,239,105,222,160,16,227,99,154,103,27,163,108,105,129,200,99,86,72,136,198,126,153,
  39,3,101,98,148,184,185,64,59,184,6,21,94,192,56,96,74,242,60,204,44,7,24,76,
  166,102,32,22,147,138,230,238,12,145,31,209,55,63,83,75,230,104,140,240,60,99,158,51,
  196,93,48,85,151,232,153,70,3,83,91,122,74,196,60,67,83,0,28,75,120,3,143,36,
  205,188,233,133,57,230,49,52,179,167,103,231,184,181,139,25,180,118,150,167,31,218,181,60,
  207,179,158,9,99,38,195,51,8,77,90,90,156,211,188,96,105,150,208,49,55,135,14,47,
  77,51,47,156,197,136,45,82,159,208,64,102,58,250,121,134,159,23,233,153,80,59,189,144,
  225,14,17,178,167,23,102,185,67,244,23,233,220,112,166,203,121,34,53,116,130,134,127,118,
  137,25,223,28,207,176,133,69,69,118,203,139,50,5,150,240,56,39,132,131,161,89,206,100,
  132,195,78,7,75,11,76,66,11,72,0,177,10,150,231,130,197,101,38,160,57,252,93,156,
  103,94,61,3,188,44,102,136,29,206,207,162,0,72,125,158,31,1,119,97,137,122,48,139,
  97,88,166,153,140,134,162,24,166,54,183,31,76,19,40,229,110,17,150,22,23,153,216,104,
  250,44,208,244,3,5,2,152,240,169,101,176,223,101,30,139,57,226,187,224,147,211,170,189,
  160,81,26,149,5,192,89,98,178,196,172,155,14,22,23,121,205,152,163,150,205,49,15,154,
  69,191,23,153,50,231,50,152,43,11,11,196,46,65,223,168,38,232,148,199,89,73,5,93,
  45,16,79,199,32,210,160,241,178,53,11,98,93,162,81,166,97,152,67,27,136,227,48,41,
  45,45,211,39,128,108,208,224,34,119,126,122,6,169,243,140,190,229,12,141,181,240,56,76,
  24,180,153,219,187,180,132,177,206,112,123,1,125,113,134,187,9,174,28,32,103,137,151,190,
  76,0,34,35,202,154,70,194,66,134,63,60,59,63,77,29,154,49,143,210,116,140,222,34,
  51,70,60,78,19,92,198,58,218,176,200,156,101,158,160,3,211,179,10,213,75,188,2,99,
  48,129,201,105,30,150,233,185,5,148,157,227,89,138,206,3,2,115,59,96,121,113,150,153,
  35,205,58,160,143,153,4,230,30,40,131,202,206,81,35,103,137,1,204,205,96,50,205,47,
  243,172,152,6,85,204,243,34,140,70,46,80,234,140,121,20,164,206,207,162,67,170,233,232,
  219,12,77,41,96,29,112,231,121,78,206,162,135,248,34,55,29,147,119,145,153,236,60,161,
  0,84,196,237,5,43,192,242,44,156,100,25,101,23,50,106,45,90,144,166,47,96,228,23,
  102,153,151,207,99,106,47,200,200,207,130,118,230,133,77,99,153,10,64,163,25,90,248,48,
  85,100,89,69,203,150,3,172,221,179,230,113,126,86,53,125,110,134,86,104,110,58,102,50,
  63,2,46,200,149,177,14,56,243,196,49,137,52,102,131,121,145,52,50,52,110,60,235,230,
  121,8,167,101,205,6,129,131,241,202,106,2,148,44,242,202,79,139,233,252,60,51,18,76,
  140,96,94,232,97,22,232,2,219,36,50,194,170,22,48,127,6,123,194,48,0,223,140,73,
  254,252,194,172,126,92,80,168,94,130,199,206,69,105,239,92,128,38,241,35,214,247,89,162,
  26,106,47,224,78,11,23,1,85,96,28,133,67,226,145,133,21,176,118,66,9,99,146,184,
  182,48,58,80,35,240,176,36,204,12,248,157,227,57,4,166,0,96,179,75,44,175,9,206,
  22,152,104,103,131,217,69,145,154,128,24,240,72,211,94,30,65,126,146,52,106,237,18,179,
  12,176,89,221,86,94,10,231,20,25,207,100,152,110,102,208,148,25,94,215,230,105,105,70,
  14,115,12,48,207,89,230,229,204,38,102,213,130,136,49,194,163,208,46,58,46,50,4,230,
  40,112,192,171,14,125,98,54,195,61,152,70,59,102,136,153,160,65,24,95,136,169,186,173,
  211,186,165,138,32,132,105,160,153,75,66,188,11,193,140,80,8,216,4,49,27,61,213,22,
  120,166,205,179,208,70,77,156,21,9,141,40,129,168,100,81,132,12,66,148,8,174,11,212,
  127,94,207,231,88,130,91,228,137,63,75,51,96,129,42,204,178,172,72,228,139,38,242,226,
  181,52,11,73,255,164,19,185,114,249,174,106,47,116,158,116,204,125,214,212,137,248,152,189,
  171,165,190,85,3,154,175,11,193,65,43,64,40,35,128,64,12,52,248,83,155,220,216,220,
  170,22,143,42,147,148,130,192,125,155,173,54,206,37,242,208,152,211,123,173,233,189,63,244,
  96,114,152,191,238,232,231,138,122,110,210,206,134,174,203,72,144,88,242,68,67,240,97,152,
  223,55,14,71,233,147,188,69,120,215,75,112,43,12,31,69,47,188,129,194,205,108,4,34,
  236,103,27,249,205,123,104,128,206,197,119,76,182,163,156,181,178,163,162,62,121,139,209,94,
  138,122,244,53,241,100,84,227,71,227,234,168,75,175,214,116,182,213,168,175,114,177,54,85,
  103,223,121,61,184,238,233,213,30,176,219,82,14,114,206,185,74,151,46,171,154,131,169,236,
  125,30,151,227,46,106,128,50,25,92,212,90,237,157,139,189,218,85,183,135,27,205,124,185,
  13,69,214,107,116,107,55,123,43,110,126,98,81,194,178,151,148,33,78,133,84,220,49,186,
  88,85,71,28,175,151,124,152,83,158,87,189,13,40,154,200,155,195,42,187,247,118,29,210,
  41,45,130,83,26,205,87,46,118,36,86,93,199,141,85,151,88,46,29,94,246,186,15,19,
  236,216,180,7,219,143,212,100,101,27,206,193,167,34,123,96,126,133,171,233,86,159,183,224,
  118,67,140,99,94,120,34,236,65,108,194,214,59,87,149,203,117,9,158,112,197,182,80,233,
  135,243,53,113,117,154,175,43,79,186,101,168,50,79,119,186,193,65,55,56,236,130,170,47,
  108,42,28,50,237,192,183,26,34,83,247,131,213,110,176,78,217,15,102,103,169,21,67,115,
  236,106,23,137,59,94,90,89,185,133,57,19,154,217,147,159,117,218,206,62,210,159,138,227,
  25,183,14,195,109,81,191,230,27,218,23,244,217,125,19,14,2,46,27,231,215,114,235,132,
  81,212,103,63,89,173,26,252,39,245,195,214,57,28,49,95,55,158,30,224,58,27,25,153,
  151,0,6,14,182,108,214,185,132,93,195,213,178,117,50,246,49,238,148,76,10,43,106,226,
  201,76,56,241,100,117,121,215,122,101,194,80,148,206,232,110,106,223,164,157,183,91,124,238,
  38,23,250,168,175,156,204,254,144,215,85,94,33,226,242,55,132,170,85,29,215,229,15,58,
  177,52,220,54,235,246,201,64,93,23,119,108,41,14,201,163,213,61,49,32,182,95,202,103,
  114,162,172,101,207,204,71,244,133,77,193,253,177,252,36,120,38,174,70,93,109,229,247,197,
  173,177,40,32,17,109,88,57,240,251,138,195,46,114,244,67,207,13,115,189,248,208,60,173,
  170,114,93,185,138,89,10,83,228,222,0,23,41,225,115,42,175,175,227,222,229,245,125,220,
  224,10,29,74,114,18,200,208,56,124,241,110,24,224,246,254,113,66,218,3,221,52,162,11,
  189,148,243,208,39,167,60,120,111,115,170,186,34,93,163,91,215,138,14,139,13,221,72,56,
  218,129,7,180,243,107,210,90,65,141,194,31,197,45,62,121,112,28,83,73,2,84,71,61,
  178,227,209,7,206,146,138,187,80,176,235,105,128,28,95,200,95,15,174,181,187,33,85,205,
  42,208,50,254,41,174,178,153,23,241,64,211,201,185,33,75,7,193,129,19,180,208,78,96,
  206,224,26,198,159,12,197,111,230,152,244,193,183,96,55,64,48,92,196,191,173,135,65,59,
  12,30,194,160,24,6,155,248,27,156,135,193,231,0,152,109,34,150,34,152,90,239,137,245,
  97,3,56,159,243,221,204,245,45,235,118,153,117,34,167,30,198,166,227,60,122,12,103,126,
  33,62,58,73,215,79,53,123,34,31,146,80,64,179,165,129,73,132,118,14,42,232,143,80,
  11,194,110,121,50,248,111,240,201,70,99,234,10,39,160,239,158,39,51,75,11,147,47,255,
  77,33,154,227,7,227,28,224,87,45,86,109,112,136,201,0,198,58,228,154,110,92,97,244,
  143,124,182,225,90,237,85,248,170,10,124,196,77,158,112,137,211,76,6,123,29,174,181,239,
  121,156,63,200,75,149,153,73,234,246,126,30,163,125,16,12,156,82,194,255,41,57,189,50,
  110,21,224,188,9,249,108,167,169,22,5,213,152,137,135,22,2,13,60,225,46,243,132,242,
  93,90,159,168,153,5,128,244,178,255,4,58,121,181,93,39,103,63,231,116,39,57,53,32,
  127,243,212,238,70,178,98,117,196,24,140,45,159,48,12,99,235,36,143,68,194,177,251,228,
  135,1,185,196,39,235,131,116,48,176,154,88,234,219,115,67,184,67,99,58,181,143,175,54,
  228,6,60,238,154,10,31,65,148,208,125,164,34,240,176,56,90,108,242,43,44,76,200,86,
  174,201,105,63,77,145,169,4,217,227,253,251,75,10,135,236,199,36,237,59,49,73,73,50,
  59,206,239,43,211,9,215,191,6,249,19,74,78,223,26,146,94,226,6,245,185,43,215,252,
  92,169,34,238,49,61,92,75,43,193,78,174,193,84,192,38,74,48,248,144,62,134,211,56,
  193,7,115,17,198,121,75,149,155,242,178,77,221,173,161,167,223,248,181,51,205,175,96,72,
  77,164,237,74,121,74,251,70,169,77,134,89,228,212,30,165,22,130,19,240,106,129,116,215,
  76,93,167,193,185,196,153,123,21,103,255,224,103,194,223,136,137,193,107,144,100,125,165,44,
  148,172,203,235,106,21,76,142,159,54,170,41,194,241,166,188,109,53,5,66,19,236,16,69,
  240,89,229,25,131,170,239,6,5,156,200,171,239,82,183,75,1,199,142,62,151,164,19,53,
  134,104,236,103,78,104,77,155,132,146,54,166,233,231,187,161,121,134,159,38,152,84,213,206,
  90,109,88,65,97,93,47,32,129,227,36,176,95,196,60,170,178,31,36,24,1,115,246,53,
  94,197,214,134,232,182,159,7,167,222,81,166,78,88,212,243,69,126,37,55,9,249,38,158,
  90,112,209,144,47,189,60,113,164,104,118,142,68,109,106,79,167,112,135,74,45,155,102,209,
  236,6,253,160,167,44,127,206,164,55,95,206,209,227,125,21,68,226,177,151,63,11,61,119,
  254,104,102,130,207,153,253,151,104,33,43,53,38,21,31,34,94,42,40,224,245,96,244,146,
  93,236,246,61,8,178,4,53,26,110,212,58,240,6,138,241,195,229,33,190,14,222,191,31,
  76,81,154,94,255,146,224,30,10,127,248,29,208,138,181,68,161,123,158,143,19,58,125,101,
  93,15,37,149,3,51,28,252,225,133,41,24,88,15,215,228,62,149,68,21,98,72,206,247,
  40,221,131,160,62,53,96,171,2,150,109,94,60,32,78,225,224,32,216,102,3,23,44,40,
  103,161,231,187,51,253,156,112,126,6,39,230,255,101,124,152,211,193,170,242,89,126,124,136,
  96,52,3,156,101,209,174,194,115,96,174,142,78,95,32,175,193,107,124,254,64,147,159,220,
  40,226,104,30,169,193,159,87,105,67,142,110,198,1,101,108,43,239,13,41,44,234,225,147,
  118,194,146,31,124,152,188,125,156,12,116,162,170,124,192,169,214,137,253,251,247,21,131,187,
  40,225,91,239,21,198,127,15,73,131,44,40,89,95,75,174,136,50,14,213,127,94,5,119,
  104,178,106,189,131,246,81,80,104,12,210,207,6,63,87,249,237,100,28,109,15,195,209,246,
  248,142,32,171,128,79,211,174,194,109,62,153,58,73,224,233,231,132,5,206,86,66,52,37,
  242,6,219,162,80,242,209,13,47,21,163,208,242,219,181,246,5,159,228,115,34,86,127,243,
  226,80,1,62,56,176,231,148,7,176,138,58,248,52,208,230,8,7,98,13,55,56,61,248,
  1,74,44,241,119,14,160,82,80,54,88,9,228,104,203,232,221,44,105,39,32,173,208,249,
  167,211,250,169,137,10,22,116,4,134,226,205,15,135,189,119,98,215,79,72,128,171,62,95,
  29,197,41,53,74,145,176,58,65,155,52,114,106,130,93,80,251,137,58,33,78,76,95,42,
  14,58,83,131,95,191,78,127,184,46,182,238,123,228,54,79,15,68,50,181,176,161,73,35,
  116,170,189,162,252,99,232,140,102,66,121,162,163,96,13,12,196,49,3,120,20,31,70,3,
  184,48,26,192,131,209,96,234,39,254,127,72,103,117,186,170,19,52,141,207,35,52,203,124,
  46,122,255,11,148,205,91,156,148,199,126,206,177,109,116,163,51,196,26,126,236,54,60,94,
  60,169,221,199,67,218,125,28,107,119,95,224,165,14,67,243,177,183,52,155,110,72,38,176,
  233,135,70,172,197,126,73,116,178,25,201,76,97,3,57,112,122,90,186,173,193,129,114,57,
  58,168,216,158,14,108,33,182,199,22,79,139,177,146,71,182,36,17,150,85,46,12,71,245,
  102,232,23,181,93,238,39,67,72,63,111,134,126,94,106,106,106,10,150,14,247,108,188,150,
  142,124,159,55,112,9,248,114,190,203,69,82,209,207,198,42,58,159,149,26,73,159,229,251,
  84,78,119,73,37,114,64,127,182,141,85,230,90,158,141,186,6,242,246,147,118,206,45,50,
  96,242,220,208,221,132,249,150,113,72,53,37,206,171,114,63,225,226,19,94,248,82,55,33,
  123,162,250,153,84,148,131,237,12,48,44,186,100,112,11,31,106,49,36,7,101,47,85,161,
  32,40,192,81,20,12,164,131,53,254,109,6,219,252,123,150,27,132,43,169,7,50,102,44,
  64,74,37,11,198,53,250,133,217,226,54,253,194,86,177,28,6,251,210,121,225,179,247,173,
  65,106,31,241,255,138,184,104,53,77,86,218,217,212,161,170,127,168,234,31,170,250,135,73,
  245,189,234,135,105,113,35,63,177,246,43,175,82,173,137,122,229,229,0,43,45,231,72,160,
  4,155,163,6,148,61,113,197,54,30,123,240,30,74,155,14,44,215,186,190,138,185,48,6,
  130,210,26,24,24,56,188,160,227,117,88,113,0,214,154,152,151,169,190,164,214,60,186,136,
  81,178,205,66,75,32,36,145,156,228,86,16,21,220,144,10,40,61,29,173,160,149,115,195,
  171,80,45,135,212,201,11,83,159,130,72,121,33,181,72,168,179,62,249,176,165,162,226,216,
  116,152,217,157,224,23,235,127,245,102,120,51,212,77,78,209,158,202,121,105,187,47,215,206,
  115,207,205,216,117,158,155,110,70,232,190,116,221,151,179,200,203,144,144,84,142,79,249,160,
  122,57,182,36,199,67,121,68,185,163,150,138,160,228,184,133,133,97,22,143,220,45,237,191,
  59,240,124,199,135,5,212,227,94,92,46,86,138,19,108,84,72,191,65,106,82,199,164,254,
  138,52,2,35,106,169,109,72,93,213,20,133,235,32,95,98,53,241,33,133,142,2,215,43,
  134,218,225,31,184,31,94,106,198,187,89,176,70,239,206,185,76,240,147,18,136,119,229,72,
  25,17,56,128,32,100,90,64,144,54,61,64,144,60,125,64,249,181,64,1,202,255,180,221,
  217,99,146,78,16,24,87,35,106,164,243,238,125,187,174,237,252,68,179,80,159,66,111,107,
  253,110,7,250,19,44,233,196,11,238,201,203,21,107,81,236,39,54,59,154,163,31,228,7,
  218,242,238,96,140,229,221,38,14,192,190,213,161,252,178,112,240,10,56,239,232,47,200,81,
  170,227,217,22,224,44,245,161,107,222,243,13,210,102,135,158,59,112,247,98,7,180,112,20,
  96,65,154,50,243,20,114,123,151,40,12,252,160,79,113,92,168,22,18,225,68,111,144,224,
  221,202,22,149,156,117,178,70,197,247,148,199,91,217,220,11,195,222,128,168,125,30,23,238,
  130,159,1,214,160,231,3,123,20,120,0,55,227,178,115,199,42,245,83,121,12,123,255,254,
  103,196,173,59,154,128,83,64,232,101,87,47,46,90,100,132,253,105,154,86,178,221,186,133,
  156,206,53,133,9,75,115,83,107,88,247,4,50,22,183,109,177,101,164,21,141,215,215,53,
  108,80,216,189,204,77,67,73,231,72,68,185,111,132,194,19,157,101,21,176,219,188,68,70,
  108,205,115,128,54,195,42,233,53,250,66,29,123,36,190,200,75,235,228,246,148,163,112,52,
  222,50,247,66,44,117,18,107,226,79,172,125,95,224,16,95,191,127,88,35,1,240,190,19,
  166,145,147,187,9,157,72,109,161,245,94,177,135,107,248,22,64,154,32,72,30,110,56,126,
  129,235,110,157,249,225,198,129,150,166,198,235,80,88,81,112,211,49,32,101,93,53,157,221,
  70,181,228,86,196,171,109,155,106,162,136,105,228,191,192,121,60,59,233,63,107,124,154,254,
  245,235,140,80,157,249,203,96,175,201,18,212,253,109,234,39,134,239,22,0,240,101,169,91,
  111,224,188,50,127,206,117,203,14,38,112,83,189,38,87,245,80,146,138,124,198,31,2,194,
  6,169,200,70,79,53,13,165,163,67,188,146,98,250,32,175,114,28,130,197,165,1,74,228,
  29,235,159,123,236,94,75,160,82,12,14,200,29,197,157,253,2,57,207,243,18,43,135,59,
  171,251,91,69,100,228,116,64,137,159,202,103,157,76,199,45,48,31,92,39,84,112,115,91,
  13,247,106,27,222,50,44,252,187,237,217,106,168,175,107,64,229,6,135,91,237,175,36,52,
  39,171,11,145,191,202,120,129,106,177,84,58,136,52,153,211,203,104,248,129,146,159,126,26,
  39,122,62,128,131,18,34,43,171,79,136,27,189,247,239,135,117,94,245,123,141,84,225,112,
  212,71,136,71,146,175,126,226,237,76,149,166,199,36,221,100,80,140,130,227,193,144,94,1,
  0,164,8,189,148,137,122,251,129,155,180,78,84,149,144,228,120,42,6,62,19,128,84,195,
  124,2,156,111,141,120,106,240,46,132,204,185,98,137,139,237,176,201,59,184,58,134,205,226,
  252,189,163,24,236,90,122,234,92,54,201,138,99,82,170,190,118,233,56,113,93,235,64,180,
  95,235,124,250,214,192,95,168,6,110,59,188,241,37,107,109,104,86,39,171,136,138,76,13,
  216,217,152,12,214,58,76,115,130,24,16,194,233,90,231,199,95,239,66,244,128,158,210,47,
  46,137,237,168,83,116,65,182,169,165,147,251,52,59,207,176,132,170,217,104,200,115,219,173,
  43,92,122,171,209,197,125,207,222,147,161,216,109,32,166,246,168,75,49,106,108,92,237,132,
  204,44,166,51,161,217,240,1,226,179,78,126,176,213,32,213,201,144,38,86,195,180,142,246,
  173,187,206,25,174,16,211,188,180,27,236,77,195,9,6,17,143,161,239,223,111,242,142,149,
  15,158,137,243,115,72,18,81,187,61,72,236,38,14,86,48,0,32,18,0,7,202,27,26,
  239,91,85,185,129,118,133,81,239,208,11,171,89,203,208,182,180,27,90,135,147,89,73,169,
  66,187,216,84,69,12,73,16,187,139,91,186,70,208,185,208,67,235,21,133,234,157,116,214,
  36,202,58,74,94,150,91,237,152,150,83,118,131,219,118,233,220,198,110,61,176,56,217,180,
  190,137,123,74,195,64,215,103,219,66,211,244,44,135,56,7,8,56,34,215,50,113,191,115,
  155,16,193,71,46,199,173,62,7,192,51,95,197,126,82,120,12,244,117,180,32,43,199,218,
  33,105,10,14,212,27,93,202,147,48,59,92,12,117,136,238,106,253,80,124,108,190,127,47,
  153,242,134,92,150,44,208,226,109,194,240,191,253,203,123,178,222,112,5,10,136,236,226,78,
  110,14,168,172,61,181,199,227,219,224,114,59,41,77,7,233,67,205,22,136,218,30,66,193,
  111,130,201,67,64,174,86,34,4,185,213,238,158,193,195,10,147,101,82,149,3,183,125,197,
  208,76,16,213,52,57,56,145,182,129,168,15,100,225,92,211,182,42,57,163,86,209,88,246,
  208,203,1,6,244,74,249,235,151,93,8,228,153,214,21,121,18,246,159,118,163,149,220,16,
  87,189,81,19,143,248,63,36,186,216,29,82,164,153,203,175,3,242,27,61,8,63,221,24,
  103,214,3,227,56,26,50,220,77,120,58,128,195,104,76,116,150,1,14,32,201,5,107,116,
  218,140,119,197,31,144,119,19,201,35,165,7,145,55,72,153,3,199,198,231,65,129,20,102,
  193,154,222,15,203,164,90,165,232,154,201,51,75,240,231,22,86,147,72,227,146,80,11,142,
  15,43,147,70,234,103,254,111,235,182,249,38,148,152,13,107,113,1,123,160,28,115,67,120,
  84,210,121,24,93,61,114,41,71,178,196,93,169,144,93,49,214,234,79,16,5,222,191,95,
  83,119,96,168,226,11,117,158,111,131,65,171,128,57,2,209,91,235,165,73,19,215,186,193,
  117,233,144,48,149,193,158,94,47,193,159,183,15,171,224,115,80,1,55,218,213,62,203,237,
  85,213,191,201,180,22,10,169,7,217,56,140,180,132,82,88,109,71,236,250,190,243,166,99,
  21,200,94,109,123,187,145,234,37,48,97,247,162,38,253,209,164,115,148,53,150,153,143,90,
  218,55,55,101,68,125,115,127,175,155,235,195,56,194,191,16,19,50,41,104,2,183,33,125,
  172,3,111,110,39,54,141,103,9,254,191,193,62,7,218,28,197,52,58,235,52,147,106,37,
  133,67,77,216,165,15,45,102,182,232,206,30,201,35,64,38,29,119,163,118,64,183,229,216,
  247,195,31,178,53,137,237,84,21,152,236,132,184,136,208,103,7,53,107,98,71,23,210,196,
  176,65,32,249,71,24,71,178,231,81,147,122,196,18,34,92,101,27,199,167,102,203,45,60,
  44,114,214,198,169,216,198,219,83,171,148,177,244,249,245,107,59,77,167,88,103,13,88,175,
  224,84,182,37,81,221,6,238,14,47,230,233,124,144,16,25,38,29,28,76,153,21,91,190,
  114,48,18,200,65,34,144,87,245,133,79,156,117,251,43,124,125,241,70,118,113,228,167,95,
  174,221,162,12,170,198,130,216,27,160,110,24,40,90,117,15,8,141,198,237,62,9,250,188,
  201,38,179,41,211,235,110,103,173,1,38,162,34,44,1,91,196,130,232,54,171,89,200,7,
  88,189,213,253,105,103,69,215,203,130,210,251,66,26,197,73,217,201,148,245,228,202,41,41,
  119,233,167,16,15,209,64,88,7,177,192,18,193,65,66,12,44,10,27,161,93,161,196,66,
  102,35,114,219,106,39,56,240,173,62,254,249,242,121,6,131,1,133,128,117,131,128,51,109,
  151,178,46,221,58,123,229,240,170,99,242,16,46,25,30,123,73,17,105,32,198,15,236,198,
  115,29,92,14,42,172,224,35,137,187,59,174,141,35,44,129,218,188,76,68,211,209,52,40,
  42,90,4,233,141,114,134,7,71,247,129,211,82,28,158,48,77,246,117,126,47,51,216,92,
  199,123,41,169,145,56,61,138,120,1,3,173,97,182,85,233,30,26,171,12,92,195,31,40,
  78,69,6,24,158,111,148,15,31,156,12,171,143,147,6,186,85,56,82,188,39,238,240,212,
  23,81,172,175,226,113,252,204,39,73,50,196,24,172,172,243,147,79,128,61,72,200,48,112,
  130,212,26,241,21,116,80,58,64,98,58,102,149,76,78,61,192,103,228,15,21,199,88,164,
  45,58,115,204,68,7,236,248,34,42,17,30,104,1,70,230,168,89,187,15,76,0,49,84,
  186,213,114,204,223,211,70,198,41,144,73,231,26,109,34,181,140,83,8,63,173,133,248,171,
  37,157,109,202,60,45,64,210,121,186,36,106,130,176,131,221,8,22,117,53,18,170,207,3,
  187,127,124,11,204,193,37,13,29,224,145,26,70,31,133,106,31,24,78,131,17,142,196,126,
  253,32,29,68,63,30,40,64,80,17,180,44,253,127,87,49,98,78,52,67,181,161,92,189,
  56,61,196,168,78,188,24,113,146,33,6,248,148,13,9,134,198,12,121,100,92,149,196,255,
  86,47,32,89,186,236,143,85,236,76,85,74,131,45,50,115,218,179,189,213,98,224,163,233,
  41,20,93,249,199,83,253,254,17,142,53,78,248,188,210,101,135,73,92,115,164,160,190,19,
  229,76,26,130,144,7,148,68,218,172,91,181,103,221,182,231,12,222,34,214,109,115,178,202,
  14,124,136,224,234,108,129,21,135,225,45,48,47,156,3,217,48,185,206,31,100,69,77,220,
  40,169,74,173,254,22,93,74,79,99,215,168,190,83,234,225,143,81,15,80,137,98,105,35,
  61,112,245,233,254,98,140,133,198,47,78,211,6,252,255,172,177,5,131,224,180,179,29,51,
  137,68,207,177,42,110,73,166,185,225,27,183,72,117,181,29,225,253,11,182,232,189,251,126,
  120,127,179,14,236,53,234,191,126,161,0,4,67,44,57,224,155,155,146,69,241,159,210,207,
  32,185,226,48,215,93,3,63,144,35,153,78,60,25,119,234,161,209,76,239,106,20,12,248,
  4,117,96,55,64,183,161,30,15,98,211,188,10,13,2,153,97,56,141,104,224,224,159,135,
  249,64,79,70,52,157,187,34,59,175,129,217,109,13,204,30,140,4,139,215,247,206,112,186,
  225,77,164,173,9,96,14,196,233,0,136,183,124,123,217,224,176,108,74,179,19,203,202,91,
  69,11,237,21,160,196,94,243,242,65,217,69,229,145,46,90,117,10,243,25,223,165,35,222,
  65,56,20,108,56,30,110,56,12,48,21,241,198,105,204,24,34,106,162,183,55,189,13,211,
  134,177,227,43,226,76,234,119,184,46,169,243,111,195,211,237,208,160,154,245,208,112,99,6,
  181,197,222,16,186,216,51,116,1,102,125,224,176,232,215,82,145,218,131,99,100,96,185,86,
  199,84,206,185,27,237,128,119,212,118,163,253,233,54,148,237,54,184,136,236,180,21,43,177,
  155,179,129,163,98,163,171,78,178,96,102,127,6,238,106,153,197,150,208,209,153,101,7,225,
  75,126,144,243,214,79,246,110,177,253,54,254,185,205,11,66,130,245,206,26,235,193,237,178,
  213,170,65,28,96,149,214,77,232,39,223,132,146,62,136,164,15,84,122,130,149,49,91,187,
  144,133,113,115,184,33,128,147,39,225,97,252,60,178,127,236,182,159,154,221,142,120,53,74,
  121,65,94,158,46,61,150,205,186,87,187,208,65,147,154,120,137,34,237,71,5,87,2,138,
  172,80,222,253,22,187,178,158,174,177,111,58,115,48,37,254,244,66,178,255,36,131,143,201,
  194,215,74,149,155,95,149,206,84,47,97,229,86,189,32,203,182,201,244,175,95,67,139,169,
  18,185,87,126,95,197,185,199,117,196,224,57,114,235,142,110,88,112,192,174,189,112,101,163,
  67,49,237,205,117,190,236,122,43,16,41,162,159,53,103,75,115,48,24,87,137,233,72,128,
  251,94,82,96,251,97,33,236,199,5,171,31,31,150,30,37,206,205,5,160,236,215,112,10,
  247,170,174,249,182,154,78,124,73,235,217,248,58,60,145,37,206,154,161,117,160,159,14,250,
  140,121,39,8,248,231,159,59,163,174,50,17,141,63,140,46,34,43,214,45,237,108,154,145,
  40,138,124,132,201,201,171,18,240,20,1,64,54,107,228,224,138,142,69,157,12,25,185,34,
  246,111,109,72,235,59,177,112,140,100,70,180,19,49,33,234,54,248,94,211,78,204,134,232,
  46,252,132,8,34,59,17,27,45,220,251,95,92,164,91,179,83,243,4,75,12,82,162,226,
  232,182,82,106,18,88,231,238,87,110,39,114,59,44,167,184,178,69,173,28,241,234,55,247,
  16,220,166,234,41,19,196,153,216,218,219,152,24,138,19,191,17,139,190,113,50,242,205,104,
  25,153,178,245,172,116,78,207,20,129,86,5,51,213,176,91,85,50,235,164,187,223,217,227,
  120,162,122,57,248,66,203,1,14,93,15,244,106,240,5,177,251,26,248,171,23,50,28,63,
  28,156,126,193,138,240,220,229,221,38,142,11,131,166,58,18,202,110,193,171,175,102,78,85,
  92,41,160,85,50,251,173,241,146,175,243,206,169,26,186,199,37,88,157,93,217,115,205,202,
  158,124,105,46,204,171,226,57,93,109,151,78,127,162,135,35,229,203,20,90,64,135,249,91,
  148,141,147,186,116,160,43,240,97,88,180,2,245,23,174,206,246,244,88,165,254,57,242,211,
  47,113,122,191,165,51,241,160,192,84,223,143,80,125,64,65,4,183,135,145,216,54,215,113,
  169,117,45,180,171,68,171,166,141,167,69,247,124,224,175,18,7,201,171,132,25,223,159,24,
  94,22,4,212,240,254,132,134,61,247,83,15,46,88,205,224,244,167,29,91,116,194,140,109,
  89,143,103,129,86,241,144,196,157,129,21,98,114,176,68,128,186,184,251,80,82,159,87,216,
  93,179,202,61,84,88,35,41,211,27,245,109,123,52,83,102,101,254,1,31,233,3,58,225,
  206,113,166,88,190,140,216,131,216,61,208,207,145,7,123,3,210,255,39,40,201,28,88,200,
  190,65,253,54,205,227,61,63,100,181,209,114,109,39,168,180,2,95,136,164,147,59,29,182,
  142,96,52,27,198,241,62,43,185,98,95,33,105,101,72,187,6,220,174,159,9,71,142,63,
  221,57,244,115,200,145,227,79,51,99,126,70,233,127,39,193,230,38,69,199,36,132,12,253,
  89,125,22,249,15,106,211,33,229,43,74,199,183,239,206,192,56,177,31,13,217,27,130,255,
  67,205,95,107,13,180,102,172,153,2,171,42,18,61,89,224,172,183,222,49,24,91,16,73,
  186,50,135,39,105,184,27,122,190,141,251,68,69,82,42,16,209,49,240,192,155,173,171,82,
  199,149,13,4,57,174,97,163,167,207,13,222,70,208,186,185,102,173,173,214,166,156,203,178,
  249,193,144,184,141,178,213,75,136,84,105,51,252,48,146,7,83,177,59,184,116,210,132,149,
  143,254,234,37,242,205,159,131,159,213,129,138,195,50,228,203,78,9,233,27,221,211,234,41,
  45,172,169,234,245,26,147,157,14,169,252,112,97,121,175,136,115,136,175,64,170,35,24,175,
  159,186,110,118,16,1,70,56,119,152,237,96,148,59,104,50,168,43,200,133,48,242,40,110,
  135,10,134,199,105,45,53,20,140,209,7,173,107,14,43,87,103,122,254,105,29,190,189,29,
  210,177,156,208,34,75,4,199,66,108,212,172,80,59,227,60,35,30,119,203,107,131,217,225,
  105,247,128,138,160,140,201,136,119,140,159,100,218,112,118,143,157,30,11,24,183,161,203,0,
  215,213,57,37,120,237,14,125,137,0,215,206,249,158,139,54,230,227,181,139,176,192,247,14,
  202,164,31,164,1,50,77,113,154,149,83,34,132,205,85,71,70,164,141,136,91,3,114,226,
  33,140,109,188,116,34,37,45,30,137,170,157,109,199,162,62,0,30,204,103,161,41,215,168,
  90,19,14,37,130,88,254,11,27,61,38,34,154,150,44,183,172,124,2,228,221,98,58,64,
  146,227,56,223,240,4,149,15,169,193,153,233,145,50,183,148,70,37,250,247,29,129,29,41,
  128,116,55,91,41,163,227,133,36,131,138,214,153,85,162,217,181,118,34,68,39,63,169,120,
  242,23,156,82,246,75,160,250,48,185,209,200,240,10,12,105,54,178,45,52,250,192,42,108,
  94,19,33,234,76,70,102,120,94,205,12,201,47,86,214,51,186,204,204,240,50,51,84,230,
  150,180,95,137,95,227,28,191,72,114,15,56,223,118,225,146,34,67,178,242,40,17,44,101,
  171,193,52,49,97,105,177,31,50,152,234,64,204,173,176,94,67,172,185,122,99,88,13,149,
  29,25,214,17,223,26,82,202,27,187,228,70,250,185,145,10,183,195,75,223,26,180,142,104,
  86,44,223,204,75,179,90,230,71,44,162,70,47,19,196,184,233,118,232,48,35,226,224,162,
  20,223,118,164,228,189,75,117,211,111,224,23,244,61,11,12,70,219,228,69,234,254,36,147,
  129,187,19,120,9,16,115,188,3,122,13,108,161,244,139,177,159,113,235,217,70,49,207,58,
  80,250,64,35,170,228,160,216,137,58,14,249,255,218,123,247,182,54,142,38,111,248,255,253,
  20,242,36,183,87,130,65,232,12,8,4,47,39,219,220,241,129,0,78,226,120,185,240,32,
  13,104,98,49,34,26,65,32,54,223,253,253,85,85,119,79,247,204,72,224,36,187,207,62,
  215,245,236,222,193,154,62,31,170,171,171,170,235,112,152,75,130,210,213,57,169,75,50,243,
  106,126,90,169,98,24,114,232,126,251,70,181,78,234,165,31,78,142,174,155,73,113,202,188,
  25,79,174,135,78,25,78,65,153,4,242,141,88,218,215,63,145,122,69,185,194,11,37,200,
  177,63,117,174,16,199,38,87,125,234,92,30,130,201,148,175,76,187,172,46,152,105,92,84,
  8,95,85,225,170,120,215,189,62,14,243,105,82,238,64,203,200,233,154,48,165,236,84,148,
  3,113,8,238,153,5,20,212,148,253,105,114,79,72,145,59,158,166,249,58,129,84,230,108,
  233,131,253,149,194,199,15,3,163,218,161,249,51,86,218,209,170,68,196,199,185,121,198,141,
  247,45,12,163,244,139,148,214,207,161,52,97,254,142,103,188,99,56,207,244,233,27,226,43,
  18,102,167,44,226,171,141,157,245,87,154,67,252,19,12,226,171,83,209,224,117,251,35,102,
  68,11,223,119,198,119,228,211,125,26,0,90,248,125,130,130,201,170,161,254,249,96,205,39,
  157,188,163,149,63,131,200,7,27,206,111,122,174,122,130,86,48,99,202,86,164,101,179,168,
  218,157,89,68,244,206,83,137,232,107,178,148,211,196,231,65,238,152,118,35,122,82,255,229,
  200,22,5,8,99,110,217,215,61,34,32,36,238,225,155,167,0,210,106,199,162,174,139,9,
  115,93,130,232,91,69,146,167,149,110,133,232,219,97,145,24,67,175,58,129,178,3,207,158,
  57,214,9,42,220,120,62,173,138,183,182,171,99,209,55,107,17,121,153,169,56,149,35,65,
  207,85,24,143,137,124,77,131,182,99,59,85,88,78,243,76,157,247,237,2,179,136,31,139,
  243,165,69,18,97,21,229,242,16,125,146,97,197,224,78,244,41,12,7,174,238,209,172,77,
  172,136,57,129,35,156,17,240,155,144,26,244,172,161,66,153,105,198,48,139,114,100,136,208,
  171,182,31,219,72,86,198,167,177,203,170,211,90,167,156,244,210,93,62,151,78,166,17,67,
  150,229,189,23,10,130,164,63,251,163,146,172,193,25,60,105,176,253,72,12,73,68,170,32,
  191,172,219,234,206,36,123,24,134,34,164,254,158,101,131,59,154,51,70,89,200,211,206,206,
  212,231,86,89,75,215,52,69,125,54,45,160,188,209,245,135,208,101,177,191,126,69,201,236,
  217,65,185,235,169,104,144,90,150,1,220,168,185,250,68,67,229,217,99,101,106,243,155,145,
  251,78,139,83,228,17,35,73,163,87,11,16,60,169,129,122,190,129,103,118,3,142,222,61,
  55,97,221,199,214,100,30,43,167,38,116,76,247,171,85,200,92,189,86,83,243,202,212,30,
  31,149,187,54,169,98,190,164,62,109,102,238,242,56,109,60,121,117,132,222,208,227,144,27,
  190,96,135,158,212,68,61,219,132,25,197,217,84,225,63,164,236,79,9,9,145,140,68,122,
  69,30,62,144,113,53,229,130,57,234,193,150,223,206,200,255,131,147,229,83,55,98,19,22,
  166,136,157,88,225,146,54,133,129,98,59,83,43,85,211,21,164,21,47,233,54,9,132,212,
  55,86,170,162,171,8,3,88,169,130,220,145,120,30,114,162,133,210,144,56,8,115,173,106,
  195,142,151,200,122,246,236,108,154,33,218,159,61,137,152,39,12,74,72,5,81,97,228,95,
  223,70,40,61,131,105,212,139,84,76,248,38,163,249,252,189,193,110,200,134,88,112,71,52,
  188,95,48,58,203,73,65,198,177,200,46,181,104,3,70,99,49,217,144,40,129,97,204,222,
  238,34,250,251,27,255,133,65,242,117,236,242,1,254,109,72,195,48,242,13,96,216,75,196,
  223,74,165,37,177,22,17,209,236,168,201,26,55,89,227,38,107,196,246,208,242,244,126,65,
  238,47,116,179,15,36,187,226,187,232,211,96,103,208,129,115,214,146,140,22,11,86,191,55,
  12,5,12,51,44,214,48,84,189,145,14,239,14,48,239,143,4,182,228,200,193,125,197,190,
  180,252,104,85,196,215,143,165,138,169,201,176,219,234,153,147,78,179,212,154,62,57,165,78,
  188,198,39,161,99,190,148,85,20,37,107,216,108,90,174,210,173,145,151,123,174,192,93,43,
  150,242,86,14,163,30,234,81,100,114,81,171,210,68,238,250,48,178,206,233,48,178,155,190,
  126,154,130,15,185,35,27,141,47,3,168,17,13,175,162,190,227,145,44,51,86,20,211,217,
  47,118,61,191,177,92,230,55,106,36,163,77,4,229,91,172,87,150,57,229,245,219,70,133,
  148,17,139,40,181,148,128,59,25,243,170,60,149,122,69,76,182,168,159,79,46,38,34,93,
  17,92,37,55,21,56,163,153,76,135,4,215,215,8,249,233,177,222,157,157,34,218,142,66,
  23,249,12,84,128,143,31,161,53,111,195,254,131,188,250,185,98,55,67,13,88,82,146,107,
  75,15,35,51,16,187,148,231,23,86,38,239,104,38,163,64,30,50,167,245,162,210,222,252,
  198,156,222,82,249,215,252,73,216,197,188,25,213,157,134,29,249,201,156,150,221,114,222,172,
  6,208,182,24,137,90,244,65,229,139,180,245,238,90,102,136,230,254,244,61,10,139,162,206,
  155,62,139,179,10,168,19,232,169,35,200,194,159,207,100,122,66,231,142,240,209,16,62,47,
  112,177,184,55,55,142,97,152,170,114,153,220,114,238,228,91,85,61,223,109,138,166,243,144,
  35,209,202,133,195,85,68,155,110,41,223,77,166,0,91,141,170,119,191,180,59,127,94,219,
  7,131,71,91,79,139,80,251,20,24,135,32,235,137,237,243,109,157,118,49,131,230,204,193,
  70,97,245,92,109,90,75,222,192,126,148,231,98,232,186,43,247,35,195,204,60,75,173,142,
  144,42,108,140,155,214,87,116,158,122,16,129,30,165,86,50,252,147,30,35,240,188,82,30,
  69,76,94,96,1,66,168,142,8,140,162,198,159,110,74,69,46,52,39,45,91,38,187,218,
  78,38,77,213,109,209,66,185,127,87,58,192,118,86,206,99,20,25,143,20,189,89,149,111,
  67,85,48,27,200,215,41,111,50,137,226,168,14,46,46,225,202,220,38,49,77,162,170,76,
  142,210,97,34,17,177,192,40,179,12,5,94,143,61,87,169,64,39,103,169,144,231,207,127,
  131,36,38,4,254,38,173,57,154,99,74,17,159,208,102,100,163,201,82,225,43,246,58,89,
  157,17,85,150,74,236,248,240,18,231,63,170,241,116,43,225,159,102,241,149,169,222,176,166,
  149,114,57,235,183,234,65,38,57,126,165,22,106,104,148,181,125,147,137,24,237,118,136,100,
  70,46,169,236,10,218,102,21,171,108,112,55,175,108,112,103,149,77,53,171,50,85,38,38,
  3,120,43,193,137,128,178,220,0,91,5,73,245,25,189,207,209,41,100,18,173,224,213,44,
  149,118,191,197,170,56,106,214,229,89,109,249,133,149,200,142,121,199,232,68,167,61,100,192,
  71,180,102,233,248,40,253,217,236,49,203,40,50,80,201,156,110,67,166,138,173,26,65,229,
  237,239,226,246,173,182,109,45,139,116,98,172,156,158,60,115,21,25,135,97,47,91,194,72,
  54,65,36,214,252,31,35,130,8,37,220,28,70,27,63,70,248,171,229,155,39,67,100,126,
  28,70,167,235,83,109,140,85,62,25,50,214,66,2,221,124,234,83,71,17,3,147,98,249,
  188,81,18,220,219,130,119,69,91,145,2,30,140,237,231,196,108,86,250,142,56,43,71,191,
  100,101,243,243,239,134,143,149,152,213,146,253,176,54,59,111,86,109,235,85,113,102,214,172,
  186,153,55,196,108,118,238,125,206,41,144,238,5,63,5,167,158,226,30,209,158,112,41,237,
  219,98,178,124,190,186,132,121,235,202,164,217,39,89,109,187,8,85,31,180,151,57,87,181,
  178,192,211,220,247,169,71,186,156,182,101,65,241,95,211,226,142,12,51,95,50,154,166,126,
  188,236,162,138,50,72,114,30,59,93,53,27,136,6,207,206,200,244,99,123,68,38,122,83,
  91,201,150,77,70,178,154,183,98,71,54,175,150,136,107,202,84,4,60,182,26,211,88,19,
  144,132,191,84,231,26,167,87,80,146,189,119,233,50,135,166,4,115,182,39,51,138,205,25,
  196,150,224,147,238,43,30,41,228,47,251,240,80,60,193,137,49,203,66,30,235,139,214,205,
  242,33,157,177,215,205,61,250,169,225,216,53,176,194,50,109,101,180,228,228,25,5,150,226,
  158,157,238,122,228,44,144,116,81,200,245,213,247,104,247,215,222,43,109,235,79,175,24,228,
  49,144,52,162,196,111,224,173,49,171,55,195,19,139,181,25,99,177,16,238,37,163,68,219,
  115,246,219,234,139,163,237,55,251,42,48,50,55,146,157,39,148,85,167,218,14,196,152,157,
  249,71,38,77,121,183,172,80,32,4,243,69,234,247,174,199,78,215,17,166,210,175,183,93,
  83,110,79,197,222,75,27,49,107,83,154,194,65,165,234,240,63,139,106,175,163,205,105,153,
  20,113,237,2,152,168,208,227,20,173,134,78,192,70,60,2,170,79,2,84,219,152,105,144,
  194,168,58,138,180,66,118,109,179,115,76,162,236,89,57,44,67,20,245,164,148,52,253,204,
  74,195,72,103,217,14,123,29,226,114,85,152,188,95,42,135,188,132,201,196,3,175,157,165,
  188,9,63,49,252,10,248,9,14,187,94,178,71,68,38,232,100,138,75,97,19,254,68,30,
  252,233,78,135,72,141,251,227,9,93,2,236,107,153,140,208,213,142,112,93,105,83,1,219,
  173,50,109,33,163,28,189,200,228,26,1,152,151,2,117,55,247,84,79,202,67,2,165,177,
  12,205,77,38,141,40,236,23,100,90,118,38,45,201,21,139,42,215,149,137,143,113,149,150,
  135,158,245,91,29,245,134,176,184,13,58,91,25,39,23,251,83,120,250,173,108,237,244,248,
  7,222,82,187,234,39,29,74,136,69,111,181,33,3,177,250,63,147,156,209,54,54,56,58,
  57,193,0,68,201,115,39,55,32,187,164,53,184,110,118,4,170,123,213,247,95,61,141,114,
  158,230,123,206,85,62,161,253,249,142,106,117,41,244,240,7,67,240,43,113,129,89,222,233,
  253,196,54,53,115,113,204,78,133,140,129,200,83,148,156,12,58,120,89,239,190,79,194,21,
  127,102,241,160,57,183,235,111,197,156,89,250,87,0,210,216,203,12,68,251,21,221,62,57,
  217,222,125,69,97,156,107,72,59,129,157,204,251,163,253,179,221,247,59,251,103,111,182,15,
  207,14,223,29,31,156,28,252,180,127,246,203,226,161,133,32,53,78,120,149,122,110,186,154,
  154,17,29,26,26,246,152,60,163,28,79,55,204,232,18,77,203,30,27,37,253,253,220,20,
  16,7,119,122,90,60,143,215,164,128,253,248,84,22,89,157,61,55,90,82,194,75,173,20,
  111,13,102,225,45,252,111,93,208,198,94,241,234,17,202,183,44,97,247,135,121,103,135,90,
  71,241,16,213,65,105,14,94,176,115,92,186,215,148,159,220,175,95,117,22,249,242,230,12,
  178,71,170,176,71,68,187,142,169,225,219,21,84,113,78,147,108,114,206,66,166,247,189,125,
  189,6,47,156,244,178,110,167,194,149,168,118,190,202,137,149,90,86,35,66,121,237,73,49,
  24,216,136,135,61,237,39,197,222,20,197,74,176,87,83,78,125,110,69,131,212,194,217,210,
  68,165,216,141,72,81,63,221,210,196,74,211,206,69,114,119,129,100,91,142,69,92,63,55,
  133,168,85,153,241,22,98,87,178,10,181,101,36,199,108,102,9,131,69,210,193,126,156,60,
  33,253,93,29,221,103,223,186,67,146,143,215,176,112,217,97,61,95,181,189,162,244,203,238,
  69,115,39,111,179,78,238,122,105,97,118,116,47,249,227,115,173,180,135,161,215,0,144,220,
  23,205,225,55,211,60,132,136,105,206,223,91,120,184,130,63,122,185,179,77,30,233,35,186,
  14,72,190,196,154,208,176,223,33,95,152,131,146,116,234,186,120,225,209,56,160,247,15,141,
  5,17,200,163,75,244,186,3,7,121,218,83,254,140,113,77,93,223,249,135,155,116,17,28,
  110,244,20,117,178,4,203,207,87,156,246,138,210,132,10,89,250,83,175,191,140,163,172,33,
  125,72,215,14,201,168,167,68,227,216,159,164,209,204,6,6,112,16,10,71,247,26,2,140,
  255,130,45,129,72,120,31,40,0,73,209,170,122,12,184,216,126,229,193,159,115,56,197,177,
  84,64,127,75,127,239,156,62,133,10,251,167,78,238,127,235,145,37,183,70,79,219,242,167,
  157,238,255,125,39,251,111,108,21,195,203,223,63,240,179,79,250,127,219,216,254,2,2,80,
  214,136,58,58,147,90,127,162,28,104,215,205,118,28,30,252,178,255,250,236,112,123,247,7,
  229,115,29,138,97,136,14,166,94,183,137,226,47,44,4,19,90,242,214,249,154,119,217,39,
  191,237,71,251,219,111,206,240,103,143,106,127,51,46,169,113,159,243,7,198,170,199,218,207,
  237,63,137,105,206,67,237,2,23,203,117,65,65,228,142,177,25,40,116,252,225,237,238,217,
  203,195,247,103,187,239,222,188,217,126,187,119,76,63,14,95,239,159,236,99,192,235,38,36,
  209,197,232,134,125,14,5,127,4,17,84,60,111,32,129,133,2,122,235,9,83,146,181,198,
  248,85,144,151,155,243,153,43,94,227,87,130,167,45,18,138,137,7,59,85,80,186,145,36,
  158,26,188,246,162,57,33,59,255,9,160,133,239,130,132,2,190,81,177,18,179,206,9,71,
  69,129,142,26,121,69,155,144,190,13,133,112,211,126,1,175,239,173,221,72,197,83,142,12,
  70,148,210,123,38,100,3,191,251,35,216,95,185,225,47,189,98,11,53,43,208,141,205,112,
  47,236,84,216,92,45,159,45,184,143,242,161,230,122,248,204,216,64,221,117,217,135,131,149,
  114,223,173,173,219,190,107,64,91,223,10,144,210,224,145,136,173,58,160,54,153,232,182,72,
  234,87,62,71,209,97,47,18,98,28,9,101,151,216,146,107,148,237,85,80,105,197,43,160,
  188,36,250,70,232,196,166,167,198,143,150,175,77,100,201,216,243,21,59,185,248,145,93,188,
  14,66,54,76,75,217,114,213,248,22,212,50,196,63,192,199,171,233,105,87,45,138,230,23,
  25,112,110,201,137,54,94,100,170,119,75,175,232,21,7,161,84,174,117,210,189,74,186,247,
  197,183,119,130,215,155,230,150,228,253,169,242,254,132,207,222,125,174,192,117,119,244,207,123,
  113,207,157,214,225,178,181,212,153,48,116,132,236,253,253,147,92,129,217,59,56,208,123,123,
  27,178,167,21,55,75,237,43,229,221,22,200,41,182,48,96,148,146,152,147,183,89,225,6,
  229,186,205,113,65,106,13,70,187,61,158,16,123,39,169,209,36,106,15,59,182,87,239,29,
  204,242,71,250,231,158,84,102,119,170,127,66,49,235,13,21,213,134,224,53,163,224,106,97,
  188,67,195,172,188,204,164,51,79,130,247,187,245,195,220,48,203,54,60,54,247,128,88,89,
  5,170,151,130,95,115,175,210,61,44,148,211,28,206,20,211,184,205,54,246,68,198,145,111,
  187,177,119,182,125,116,180,253,1,243,203,156,139,162,178,4,245,1,75,218,53,212,155,92,
  246,228,113,77,232,227,152,92,65,68,200,121,255,150,49,216,139,215,7,135,103,31,206,216,
  2,31,92,220,5,148,1,63,204,44,124,120,180,255,230,253,235,147,131,195,215,31,206,182,
  95,31,190,218,54,245,210,224,161,247,172,253,55,179,137,237,215,7,47,223,18,9,130,74,
  55,49,84,104,63,111,227,117,129,223,146,101,211,160,29,124,233,216,177,166,117,143,222,253,
  124,246,122,255,237,203,19,96,34,104,212,205,42,118,240,102,251,229,254,217,171,253,131,151,
  175,78,42,228,205,122,86,193,227,31,48,119,198,230,199,120,161,123,164,28,58,63,166,151,
  235,249,165,184,239,99,242,208,95,56,251,116,6,190,62,92,51,87,202,158,134,63,72,67,
  161,205,40,110,77,134,45,248,231,21,163,185,176,1,235,188,66,50,21,50,163,228,125,129,
  110,212,109,33,152,231,206,54,233,35,206,62,17,78,81,77,116,219,18,214,138,198,77,198,
  46,26,42,30,242,27,46,210,134,38,61,100,217,133,197,80,248,67,93,14,10,29,153,188,
  66,114,132,8,167,51,155,38,25,134,133,146,255,162,186,123,71,219,63,187,117,163,162,186,
  169,143,10,188,226,174,255,24,109,28,79,241,23,34,175,223,34,176,17,243,36,91,185,193,
  21,9,150,178,252,140,222,1,242,185,180,248,99,68,87,231,236,30,114,83,152,221,195,97,
  174,135,43,186,245,168,11,38,143,16,24,211,94,30,185,30,229,182,148,27,82,126,23,132,
  42,122,91,125,187,191,125,180,127,124,242,196,45,18,82,235,73,59,194,69,83,143,242,36,
  219,19,104,45,120,52,252,250,85,94,21,110,43,115,193,239,105,99,220,123,226,0,95,166,
  224,1,176,174,65,251,140,192,99,24,50,120,108,253,109,224,184,13,11,193,2,118,18,221,
  153,146,204,39,53,236,208,94,5,189,144,102,196,214,223,134,188,126,84,12,115,115,199,255,
  164,150,157,241,23,117,3,66,151,160,101,235,91,32,59,27,158,203,130,236,46,47,71,134,
  134,5,49,113,70,49,78,12,9,137,137,249,86,243,52,201,60,217,107,87,177,11,255,119,
  30,158,132,136,88,65,220,179,145,62,38,56,157,51,57,223,144,206,88,224,151,68,46,87,
  41,210,183,144,78,51,136,164,183,172,111,105,168,233,167,181,109,26,254,214,241,84,186,153,
  73,186,51,202,178,28,87,238,254,103,102,85,204,10,204,152,208,204,166,53,137,144,222,254,
  179,103,248,196,241,61,133,50,33,111,68,79,161,73,190,127,26,49,66,106,72,143,19,35,
  241,147,136,145,17,43,248,137,147,214,195,106,198,1,27,73,189,220,36,214,44,155,197,10,
  210,43,238,12,101,132,202,151,57,130,66,203,41,71,241,171,187,213,126,158,195,148,232,153,
  36,95,212,80,97,83,246,148,206,60,111,1,199,148,225,68,156,82,121,138,108,198,153,42,
  226,59,184,165,34,190,123,214,202,177,61,43,91,203,217,218,57,223,131,247,250,21,255,69,
  202,180,253,210,120,72,214,110,109,185,1,113,117,127,118,198,2,143,179,189,253,159,78,222,
  189,123,125,124,118,38,142,239,243,233,28,77,142,84,130,217,247,73,153,68,38,187,112,10,
  58,190,146,111,111,124,158,132,147,91,104,138,126,65,240,172,32,26,117,105,136,15,80,180,
  38,97,94,31,204,37,116,30,3,146,192,64,86,114,149,42,17,109,199,92,32,107,207,151,
  22,224,153,158,101,243,41,254,64,190,210,84,133,27,204,21,239,77,153,118,86,30,193,211,
  40,218,16,9,134,252,250,107,66,13,91,117,96,95,123,150,141,103,236,116,134,120,128,194,
  58,229,43,189,207,164,99,201,31,214,111,131,73,233,126,208,251,194,222,9,61,137,9,237,
  61,248,225,80,39,137,135,255,7,255,141,41,68,206,252,31,252,179,17,187,148,185,140,252,
  227,129,132,37,143,253,31,206,132,113,239,143,147,242,74,109,97,175,15,154,238,101,227,136,
  68,144,71,161,4,109,247,227,184,215,88,224,82,135,112,131,31,246,190,188,125,247,118,191,
  11,39,198,71,239,78,182,79,246,33,248,217,123,247,250,245,7,200,45,14,183,223,118,27,
  254,201,187,247,187,175,206,84,102,83,125,82,86,75,253,230,226,156,210,118,82,84,149,14,
  1,102,175,30,46,117,252,187,81,175,63,162,48,195,28,160,29,18,177,247,147,47,125,138,
  97,48,185,161,128,181,101,104,14,40,119,10,201,205,117,200,223,21,43,218,101,47,10,171,
  52,88,73,146,112,118,106,82,34,63,186,153,80,76,92,43,5,82,149,189,72,236,249,0,
  252,146,20,220,153,36,114,164,174,203,253,58,30,95,89,101,248,211,206,23,12,114,20,12,
  162,155,196,42,231,36,219,229,225,151,19,238,7,201,139,150,85,218,74,212,59,160,203,111,
  255,25,93,221,76,135,146,185,148,182,20,220,57,57,38,67,28,175,239,65,55,131,76,49,
  161,125,37,49,48,229,27,90,127,88,207,94,181,214,182,11,243,156,158,169,250,127,226,227,
  248,26,154,142,189,186,93,134,29,28,133,166,212,132,63,157,114,159,195,251,163,124,170,212,
  62,12,98,83,245,58,136,157,18,9,196,238,97,204,128,143,98,108,64,170,139,162,201,67,
  93,122,37,29,222,201,120,87,118,84,207,142,253,189,171,1,230,146,164,122,195,180,152,244,
  190,188,222,127,113,210,245,128,72,199,127,188,14,47,16,38,227,253,161,250,124,15,187,150,
  35,186,40,213,247,17,93,224,136,128,245,238,228,228,221,27,149,182,55,254,35,246,20,86,
  189,26,67,111,102,231,102,10,125,119,221,238,126,84,21,8,247,223,28,236,237,189,222,167,
  4,6,124,213,48,62,113,38,84,253,233,248,6,218,74,168,74,103,237,196,212,60,249,249,
  29,125,153,19,244,96,131,118,173,103,125,192,117,209,56,230,224,158,188,182,202,118,66,21,
  17,231,122,38,213,45,75,43,233,150,163,20,201,59,147,35,115,76,177,223,123,30,173,165,
  167,50,6,192,225,242,152,2,47,99,140,205,19,185,57,36,27,167,120,170,141,219,236,3,
  199,25,63,222,144,122,106,172,179,250,177,149,167,132,229,69,85,127,71,53,85,1,177,198,
  149,29,29,185,100,144,112,214,73,153,244,167,125,174,129,192,236,117,159,194,228,166,53,149,
  229,80,47,77,209,203,128,203,158,101,137,186,52,235,250,82,56,114,238,236,32,201,38,239,
  133,163,105,144,201,35,167,166,26,144,17,200,70,185,219,117,134,175,142,10,97,107,78,191,
  153,58,25,240,167,94,148,156,118,102,50,232,224,228,91,65,106,174,9,164,21,212,31,140,
  241,254,92,208,2,167,231,218,224,212,89,173,236,105,221,110,103,166,124,24,50,35,9,39,
  36,192,149,243,42,120,198,172,214,88,92,134,247,62,158,186,41,26,8,112,42,30,52,56,
  226,6,158,140,71,162,146,156,54,49,142,15,165,202,27,248,162,236,253,118,38,106,243,18,
  255,55,83,130,14,109,239,223,243,74,188,191,238,253,88,152,175,174,255,55,97,124,211,131,
  4,191,160,196,27,154,248,207,195,16,58,210,113,97,1,156,21,30,64,84,152,123,66,72,
  64,54,38,153,93,128,231,56,153,221,63,119,48,157,157,207,245,195,130,124,158,125,31,194,
  188,93,89,101,110,104,252,132,130,88,177,81,190,88,138,33,140,138,150,122,197,129,53,96,
  95,133,80,73,11,169,90,202,2,162,194,180,154,133,125,82,50,205,70,73,0,47,80,244,
  30,12,68,207,189,173,76,131,160,9,80,68,95,250,82,166,59,191,12,35,56,161,61,173,
  158,51,84,165,149,245,96,230,162,40,146,116,114,185,69,40,240,0,168,224,124,64,247,72,
  17,156,126,67,27,125,162,87,70,94,30,150,159,210,134,10,36,140,220,27,175,8,218,159,
  210,198,31,4,245,94,193,73,240,191,92,7,226,155,253,89,253,33,223,18,17,71,227,49,
  135,244,6,38,206,183,139,139,218,94,159,34,24,181,122,128,224,41,184,38,118,7,63,11,
  58,147,221,230,187,118,91,208,22,140,132,98,144,211,96,83,244,206,41,48,43,32,40,173,
  134,10,35,36,127,251,118,34,145,178,250,55,143,54,74,57,94,17,182,251,59,141,222,92,
  63,9,96,230,53,241,13,112,87,28,5,125,22,216,60,177,254,19,65,151,130,166,73,165,
  147,177,161,87,148,35,167,105,111,46,76,174,207,232,249,73,128,233,66,227,204,134,210,141,
  200,163,214,108,35,243,238,65,125,149,106,127,93,181,199,46,212,249,103,195,243,159,130,46,
  77,192,120,57,56,246,81,122,96,82,78,179,52,89,68,106,8,170,42,172,251,169,168,98,
  100,130,209,35,197,161,179,63,13,168,130,102,212,50,69,179,228,238,64,21,59,25,151,45,
  138,25,14,135,114,16,65,151,204,227,72,200,220,227,149,57,196,240,244,97,6,208,125,153,
  89,39,181,223,157,89,228,73,176,248,164,241,137,140,56,9,40,34,1,223,184,95,108,214,
  66,148,231,237,213,202,48,22,86,129,204,114,207,101,43,30,148,44,233,139,195,186,100,250,
  170,169,38,114,108,139,41,103,134,225,150,164,30,122,105,239,78,222,44,151,33,190,134,90,
  75,78,117,63,200,80,35,5,66,134,7,16,215,44,128,248,98,104,109,75,30,97,200,24,
  38,147,15,226,148,124,73,19,138,74,190,187,153,102,138,74,74,166,172,240,6,196,179,166,
  165,157,180,194,242,239,175,179,165,57,37,83,86,253,152,90,142,43,195,66,54,114,253,72,
  25,27,195,130,40,185,57,119,161,229,72,69,191,73,121,189,114,202,121,229,56,45,205,205,
  9,39,215,44,31,233,117,76,217,119,69,61,202,30,244,244,46,168,84,103,242,156,64,232,
  68,215,69,231,130,81,166,154,35,116,132,35,91,229,66,4,179,216,43,226,251,36,111,33,
  47,76,241,11,144,218,140,38,144,83,208,0,244,68,190,121,28,223,214,169,220,119,113,175,
  72,164,228,39,189,34,121,210,122,148,188,32,89,120,88,142,97,138,96,62,56,250,83,188,
  177,164,132,84,91,241,98,47,142,187,241,166,250,166,204,37,164,224,45,49,45,147,112,153,
  196,42,147,72,153,120,163,151,108,21,78,189,103,226,173,192,157,177,14,222,157,227,199,165,
  44,66,57,21,183,81,152,186,89,142,23,147,202,114,99,203,234,161,184,217,238,227,29,23,
  237,66,58,244,188,192,47,157,75,94,238,87,212,86,65,15,87,193,231,240,56,184,48,168,
  201,149,246,177,255,71,27,195,226,74,227,184,43,3,57,96,229,140,156,194,47,130,198,76,
  253,108,157,138,45,138,74,207,191,208,6,110,38,100,186,87,215,162,248,90,46,18,151,22,
  10,75,221,38,204,0,84,251,162,34,166,204,73,115,2,65,141,21,114,82,135,175,95,109,
  68,86,228,94,168,146,93,233,137,136,110,21,221,69,51,49,84,71,113,209,74,170,163,151,
  7,62,41,178,254,183,59,89,176,228,78,21,127,210,67,16,222,226,130,100,101,113,100,188,
  78,29,235,220,108,179,143,32,108,19,81,53,204,19,6,188,55,6,97,171,165,133,98,224,
  231,237,105,1,1,81,0,168,229,57,120,182,87,95,122,28,213,166,88,117,94,113,3,185,
  89,123,192,162,58,149,60,54,150,94,136,122,33,125,85,77,121,88,205,166,89,21,255,233,
  32,89,81,144,162,157,237,186,0,122,136,101,191,14,197,90,223,10,246,57,238,29,105,135,
  18,224,80,130,66,192,25,59,48,194,7,102,212,27,47,5,235,133,180,85,49,134,112,133,
  124,254,168,82,64,82,185,113,139,113,38,159,141,140,222,206,163,167,77,205,70,132,182,150,
  244,16,74,162,246,215,61,105,143,143,233,225,76,232,55,187,97,61,181,44,33,184,158,163,
  12,115,40,153,214,63,143,140,127,53,226,111,171,242,178,189,152,149,167,81,150,147,30,57,
  42,202,141,139,29,31,61,109,202,253,25,83,46,166,143,9,9,247,229,60,142,31,223,169,
  192,134,33,217,49,122,107,27,131,193,251,35,128,169,162,247,243,246,209,219,131,183,47,187,
  165,119,147,243,72,51,164,73,245,55,60,207,193,96,250,134,56,74,40,176,35,156,247,77,
  252,57,6,183,161,227,124,211,35,100,105,169,68,115,37,11,107,193,216,37,80,216,28,9,
  24,74,237,133,47,56,235,65,134,253,201,191,7,57,87,154,62,109,75,245,138,120,247,161,
  99,101,64,213,129,60,113,230,146,179,3,14,42,233,181,146,229,97,186,229,179,81,117,60,
  137,16,17,120,30,167,131,66,198,189,201,95,24,145,192,94,112,158,148,221,205,2,203,61,
  45,219,109,87,42,27,63,156,109,61,130,96,187,229,227,129,70,244,226,130,115,59,30,236,
  142,175,225,11,52,152,136,83,108,183,27,223,65,207,232,207,196,238,100,255,161,104,206,41,
  129,255,251,198,131,29,252,207,30,202,160,224,180,105,104,122,252,172,146,243,64,87,224,224,
  62,234,20,189,96,76,20,65,225,188,119,89,242,134,99,92,158,56,36,197,208,179,73,158,
  14,86,23,244,253,147,121,26,99,24,176,235,253,110,178,42,82,117,214,163,217,172,254,213,
  54,162,174,186,114,103,48,189,238,100,30,227,243,179,195,206,149,183,198,61,243,157,175,64,
  220,64,174,9,32,49,158,197,200,25,129,143,182,45,136,227,229,78,109,161,232,205,119,1,
  158,212,41,115,86,62,247,65,59,202,23,96,121,170,121,222,244,112,46,84,107,117,99,189,
  100,172,46,170,107,237,204,83,249,2,188,88,159,21,241,230,69,180,205,18,68,68,103,121,
  198,60,79,213,112,65,144,25,210,36,73,28,142,50,110,68,161,50,114,115,21,151,67,162,
  74,144,151,193,115,75,211,60,173,162,104,54,110,151,122,55,114,140,130,87,120,38,210,102,
  118,89,7,222,121,100,60,253,201,56,73,204,35,173,139,130,142,232,224,230,199,252,200,144,
  101,192,22,55,155,74,184,158,76,67,37,243,133,26,73,94,168,161,152,14,155,244,154,44,
  8,152,76,3,23,193,95,140,111,151,141,22,205,114,125,213,38,23,121,31,27,11,211,133,
  201,50,61,197,147,215,174,87,162,32,88,116,69,152,106,216,166,198,66,248,164,74,218,76,
  109,62,142,222,42,187,67,154,46,56,83,152,176,213,171,115,231,160,84,101,57,135,132,245,
  120,126,102,117,199,199,230,16,186,189,64,70,234,244,113,62,134,14,197,213,156,94,102,207,
  26,128,248,143,18,48,24,111,158,106,177,244,88,16,74,247,44,39,180,155,13,117,143,243,
  158,91,214,189,179,220,155,254,195,211,225,161,46,51,85,54,99,86,234,86,211,211,178,164,
  150,255,212,172,22,254,143,206,74,46,127,250,182,226,236,49,38,33,99,247,28,81,170,3,
  28,207,188,255,107,235,133,24,136,173,83,149,191,198,93,134,217,35,126,13,33,17,219,82,
  204,167,8,228,70,136,159,0,126,144,225,177,82,20,6,27,164,52,133,215,29,182,160,151,
  44,7,11,141,165,186,203,30,244,150,202,147,229,113,101,161,177,88,47,212,200,96,114,116,
  30,123,1,42,181,152,185,72,241,94,246,198,87,46,20,41,12,48,150,51,35,167,48,87,
  114,142,162,211,101,242,84,157,201,161,144,166,240,113,21,15,70,161,81,98,144,43,58,43,
  178,102,253,8,153,156,194,8,191,248,250,215,135,124,35,123,180,40,105,27,197,32,144,107,
  232,151,74,78,87,230,233,93,30,6,150,184,95,43,235,60,173,58,189,232,22,79,27,122,
  57,51,155,200,43,12,209,22,58,23,110,218,74,94,27,41,199,24,101,53,251,42,182,30,
  172,117,211,230,68,239,113,188,144,31,202,221,114,232,160,110,119,184,184,17,10,107,221,207,
  173,37,75,154,18,141,233,244,178,175,25,217,213,205,192,131,214,121,122,108,109,83,45,168,
  252,210,234,54,114,32,83,80,25,129,102,182,178,143,59,250,189,194,34,65,115,213,140,116,
  219,78,220,168,105,169,146,193,215,79,107,43,15,220,214,82,234,233,60,182,146,89,48,127,
  194,42,106,125,180,252,26,74,253,140,130,91,49,96,106,189,81,171,209,114,166,249,187,108,
  127,247,149,76,211,246,132,165,239,121,211,101,141,132,111,70,34,52,109,114,33,48,13,62,
  108,56,187,62,99,163,84,89,218,106,245,115,211,221,224,89,208,98,42,22,79,65,61,6,
  167,140,13,132,45,201,31,17,24,63,26,52,148,28,42,80,49,208,244,34,41,198,86,161,
  6,139,156,233,100,132,170,184,224,171,152,98,160,126,38,195,232,130,94,145,183,114,58,193,
  238,83,156,117,174,93,133,224,229,236,109,233,156,242,174,75,100,25,169,41,246,56,175,11,
  12,113,52,93,194,231,240,161,240,121,61,51,5,165,169,251,15,76,99,233,31,159,199,210,
  183,76,132,117,137,255,238,52,28,236,252,15,205,35,59,9,98,246,102,79,67,116,158,255,
  145,121,252,211,27,178,52,127,38,15,20,86,96,74,70,242,36,38,81,206,73,203,153,179,
  102,14,91,170,195,153,222,227,154,35,205,233,231,80,60,189,89,180,205,53,236,194,8,151,
  208,191,31,172,55,37,173,193,12,20,112,28,130,148,30,28,186,202,61,196,58,199,189,106,
  123,65,183,177,8,210,143,104,207,52,233,3,146,238,43,235,197,61,199,62,226,74,230,103,
  163,176,253,19,166,146,161,120,254,167,230,225,116,59,99,18,230,242,127,250,0,84,231,75,
  68,63,39,234,235,195,18,209,207,19,225,252,147,223,161,58,30,47,224,57,121,33,209,99,
  201,80,141,53,127,82,153,49,24,251,18,77,217,21,13,157,179,198,239,23,3,115,241,158,
  205,232,57,67,102,254,181,206,221,51,58,243,4,56,67,112,105,220,217,0,229,30,141,148,
  186,152,5,80,241,19,246,51,113,0,42,38,128,154,56,0,21,103,15,134,238,55,161,61,
  252,127,116,118,74,103,231,246,244,155,48,196,227,219,249,223,128,31,116,167,140,29,254,47,
  160,72,115,75,252,63,138,191,244,106,17,246,42,224,130,56,199,200,194,51,245,238,151,115,
  136,240,62,35,43,119,185,144,148,180,117,56,149,167,49,42,235,242,214,228,0,194,66,181,
  13,129,138,3,8,72,90,159,67,192,7,254,120,198,138,63,25,73,219,123,244,56,142,78,
  15,77,113,183,223,130,161,103,245,60,19,65,59,88,248,225,12,194,117,5,62,22,79,167,
  15,240,53,57,107,83,241,183,195,201,1,1,166,168,161,90,85,196,85,90,169,88,223,248,
  163,85,249,212,184,205,32,175,25,225,70,33,178,88,39,55,26,57,76,242,49,60,237,245,
  236,113,100,7,154,92,143,16,66,137,31,37,180,59,207,135,179,40,57,153,192,172,150,130,
  36,166,195,253,231,198,32,253,128,88,84,63,240,100,53,165,254,172,206,236,211,58,127,105,
  66,203,62,189,156,53,161,154,87,179,199,186,57,69,24,245,97,30,130,208,67,75,91,234,
  245,122,153,9,215,78,183,50,41,245,211,110,174,204,186,243,116,154,27,107,120,10,121,33,
  91,126,51,47,45,47,142,86,255,204,193,190,1,47,10,148,245,69,49,210,221,148,165,86,
  12,181,73,249,224,11,203,107,56,229,7,205,209,106,102,182,222,141,85,22,148,130,58,54,
  79,210,176,115,106,154,200,215,195,215,12,10,226,15,23,168,218,147,98,163,85,25,8,246,
  193,114,252,252,239,179,114,228,28,86,138,146,200,193,37,102,221,136,53,157,103,235,216,211,
  179,40,23,220,21,197,255,114,100,129,219,35,230,30,51,13,132,254,178,1,201,204,22,139,
  173,71,240,112,171,44,25,114,199,46,170,152,149,176,208,13,156,66,152,249,177,39,107,216,
  115,177,21,130,50,232,114,236,226,80,186,155,51,118,163,38,242,166,97,218,44,172,104,133,
  115,134,97,176,93,187,244,72,191,33,13,172,51,115,47,159,52,90,90,217,220,96,85,162,
  213,203,143,220,139,22,198,8,133,229,96,86,51,181,12,236,40,40,175,117,243,86,50,163,
  16,57,127,29,130,254,119,89,33,57,26,10,111,6,179,173,253,255,250,222,195,243,171,133,
  29,234,93,203,38,200,70,112,254,108,12,126,186,94,0,169,95,204,138,119,225,196,133,80,
  114,151,104,48,70,202,248,117,255,160,186,125,72,161,97,122,71,208,192,253,107,116,22,193,
  173,44,25,149,167,251,221,203,217,155,179,136,200,157,67,81,41,177,66,119,49,97,81,57,
  150,212,168,98,3,145,118,160,28,60,220,107,48,85,99,209,230,236,93,125,71,90,207,109,
  236,219,215,121,62,43,124,150,137,178,251,201,13,218,67,52,70,244,212,75,148,74,143,162,
  84,122,20,25,233,81,202,124,164,175,180,143,142,132,40,176,220,56,200,212,158,57,17,183,
  65,229,91,224,209,54,21,97,149,107,86,166,242,224,206,143,28,99,252,133,201,253,67,99,
  41,152,229,223,91,182,12,224,228,237,102,210,164,103,89,51,14,247,184,135,67,11,83,134,
  119,89,76,201,77,40,88,52,243,233,126,251,34,89,148,112,228,160,2,13,143,223,12,224,
  41,49,158,107,80,109,246,183,172,182,102,19,162,60,194,136,239,178,151,148,138,203,94,52,
  92,149,154,89,249,175,95,233,114,40,148,109,230,118,195,207,141,77,61,140,40,4,156,33,
  239,140,221,66,14,139,91,251,26,221,205,184,103,237,190,244,11,70,100,85,76,114,0,225,
  210,221,143,220,156,245,174,93,85,185,223,168,98,65,84,254,73,244,151,32,42,39,252,202,
  157,17,219,93,142,13,28,39,209,55,2,135,43,231,155,209,15,154,92,127,244,68,58,87,
  66,209,178,192,13,73,186,44,169,59,159,25,231,66,237,221,183,204,192,176,217,51,166,97,
  250,204,172,152,227,72,232,41,195,121,242,246,217,28,248,220,65,57,59,249,232,34,255,227,
  120,113,242,196,99,144,193,150,142,211,166,111,135,112,7,103,186,47,33,25,132,151,186,131,
  250,70,200,78,209,222,19,218,255,135,97,210,145,252,124,195,0,254,97,64,204,74,130,102,
  140,100,14,76,165,112,18,204,194,179,121,236,159,86,26,115,165,136,222,198,136,166,86,202,
  89,134,172,206,154,161,215,254,138,119,135,249,182,239,51,60,59,88,99,28,125,219,24,235,
  143,140,241,175,153,232,207,28,38,121,111,251,29,34,141,253,55,135,39,31,186,222,119,141,
  181,32,232,180,61,255,229,187,119,123,248,108,53,86,219,225,192,243,181,18,156,247,93,56,
  56,95,105,182,60,127,247,245,187,227,125,250,14,59,245,118,199,243,183,15,172,226,251,71,
  71,239,142,240,29,180,218,97,184,234,249,239,94,188,192,87,103,117,165,217,65,213,247,111,
  119,247,143,78,182,15,222,34,237,162,182,26,52,251,40,177,187,75,141,181,91,171,205,115,
  248,172,250,61,50,67,58,153,252,215,77,125,127,175,14,6,88,141,234,191,110,106,245,122,
  141,83,87,74,83,201,157,90,99,220,165,164,237,21,240,43,17,253,218,121,65,82,159,255,
  186,219,223,190,50,195,254,241,6,223,245,210,165,42,41,195,223,85,138,123,67,78,109,223,
  151,238,66,51,149,215,210,93,84,218,223,125,245,78,77,72,141,227,110,191,41,163,216,126,
  49,117,38,247,3,58,125,209,138,47,75,125,105,241,133,158,231,14,125,238,238,32,93,13,
  240,202,179,164,52,125,6,24,29,142,116,179,81,219,242,120,41,188,110,180,177,178,229,241,
  12,232,119,175,222,216,242,104,69,188,174,86,82,244,82,176,27,162,21,31,186,131,62,61,
  38,124,17,197,96,110,25,10,125,168,219,168,145,0,73,203,4,227,175,95,41,0,107,109,
  235,203,184,223,191,185,142,194,65,55,240,249,164,118,39,15,93,202,170,91,89,161,202,10,
  183,130,173,9,124,141,29,96,0,50,194,135,174,213,192,215,175,186,32,21,11,183,156,130,
  22,141,56,52,222,17,190,76,163,171,144,2,55,140,255,64,160,15,192,106,152,92,211,63,
  163,254,64,32,151,246,39,253,245,26,106,250,40,157,140,111,0,238,112,244,119,29,128,226,
  67,201,186,159,144,0,31,14,0,147,209,120,154,168,96,127,23,208,254,46,127,17,26,171,
  219,122,240,105,121,42,189,205,242,23,133,110,168,93,176,194,188,12,245,26,255,104,108,213,
  59,221,22,117,55,145,12,144,168,148,236,7,81,246,251,77,16,223,4,35,234,59,164,144,
  27,244,131,218,84,46,201,204,162,56,181,224,155,100,16,17,166,116,147,19,132,123,164,85,
  144,181,147,1,169,125,150,65,153,221,54,171,9,66,246,18,133,147,238,71,248,21,59,173,
  146,115,210,50,207,45,32,107,4,180,37,10,226,248,113,149,142,51,157,118,196,227,133,71,
  72,94,187,41,165,211,47,194,187,244,175,248,116,28,160,246,72,86,28,2,198,224,190,91,
  199,11,6,144,215,69,52,185,234,86,235,109,194,38,233,174,158,15,4,252,122,52,142,47,
  15,204,28,2,147,243,14,233,251,36,170,210,126,195,28,28,2,54,217,81,198,246,180,249,
  44,198,74,119,185,39,69,181,125,184,41,20,85,1,33,120,54,146,236,37,187,202,70,147,
  20,237,159,197,186,51,45,80,15,32,80,15,54,90,235,129,142,12,56,70,109,134,147,143,
  193,169,63,234,225,220,140,245,13,180,53,174,14,186,107,107,107,62,82,104,87,241,111,16,
  249,9,134,43,48,71,38,127,114,159,169,2,56,169,132,46,188,238,72,210,125,216,104,169,
  173,199,109,54,174,154,29,199,252,172,175,94,90,12,93,240,254,235,25,251,86,49,110,66,
  23,164,201,243,164,85,133,205,94,157,27,213,249,118,111,126,88,254,244,95,119,123,173,210,
  247,95,130,197,250,67,23,255,166,5,183,4,143,213,25,143,221,17,90,236,227,76,50,194,
  218,3,50,2,10,107,18,34,236,206,45,54,77,145,244,195,39,0,130,224,27,181,176,213,
  139,104,68,164,94,208,219,124,22,152,126,43,250,25,195,221,153,134,187,51,12,214,178,51,
  102,91,158,63,31,87,163,201,250,136,215,3,80,203,243,198,191,189,17,173,150,0,171,89,
  63,179,74,42,99,147,22,70,160,150,235,9,184,247,70,180,210,114,56,192,113,234,228,45,
  252,26,165,240,135,34,116,38,80,32,16,217,248,132,15,103,88,6,70,199,178,68,37,181,
  68,124,169,220,227,171,81,171,183,176,132,180,52,109,220,3,183,40,85,27,195,227,41,165,
  236,191,208,197,177,194,36,221,237,162,51,229,19,116,173,70,91,134,221,146,195,191,155,105,
  193,235,90,73,147,0,104,183,180,95,95,45,93,211,37,87,167,152,238,124,191,224,2,188,
  11,105,16,245,181,70,233,138,203,191,248,68,70,114,93,221,207,102,171,109,129,17,77,148,
  86,135,79,55,175,141,26,205,55,15,102,40,183,218,212,234,223,158,42,13,66,45,101,79,
  175,52,29,45,194,85,139,234,209,23,81,59,204,0,150,84,94,101,193,232,103,27,211,167,
  92,25,127,186,128,71,19,160,34,162,108,238,128,43,122,155,3,45,229,69,57,69,84,237,
  220,31,12,72,96,65,133,246,7,189,143,112,237,167,255,119,234,223,142,196,83,228,205,157,
  38,66,62,58,84,136,161,146,78,249,58,255,88,116,159,251,134,152,57,21,170,229,227,44,
  178,197,251,174,25,172,156,15,168,53,77,193,124,44,32,97,168,224,96,109,173,93,15,80,
  80,168,152,143,121,50,198,234,21,20,13,181,163,142,175,221,77,74,162,124,44,164,81,172,
  142,132,250,249,104,200,31,202,92,13,219,193,160,230,157,90,36,203,9,236,117,249,200,70,
  61,179,216,8,32,53,185,63,134,180,159,148,22,16,82,188,236,97,171,206,207,167,49,89,
  63,244,238,96,89,113,53,130,30,1,188,107,145,50,219,4,183,68,68,49,116,246,17,21,
  153,48,69,80,101,79,187,68,227,130,205,191,36,163,58,111,140,25,6,236,159,156,236,173,
  208,26,32,18,150,240,254,188,62,255,24,79,62,39,100,45,230,195,36,21,151,134,87,113,
  123,17,121,59,49,176,128,250,30,29,104,140,203,219,18,191,107,93,143,6,91,29,70,131,
  65,24,247,40,204,55,101,74,128,76,132,72,103,111,226,20,133,10,195,57,32,7,208,30,
  198,228,145,34,78,159,184,225,47,15,186,57,156,50,178,97,94,79,103,136,249,78,138,60,
  221,225,125,248,179,231,211,165,25,150,39,246,84,233,204,192,126,5,142,171,38,175,78,222,
  188,238,125,250,143,82,105,99,16,221,150,120,153,168,151,243,96,226,109,34,53,147,14,182,
  33,9,47,189,205,13,17,212,219,233,180,25,165,104,32,31,103,23,55,127,254,121,239,109,
  190,160,127,54,150,165,244,227,181,232,207,200,219,164,167,209,81,90,107,25,35,80,99,73,
  200,76,73,85,199,1,185,86,117,71,103,163,40,254,140,97,45,83,129,205,217,197,46,96,
  239,103,138,209,164,117,219,238,236,47,39,209,192,212,1,193,54,72,60,53,140,79,138,106,
  144,168,146,4,104,180,127,18,217,237,34,36,177,132,183,28,92,71,203,52,147,173,171,158,
  183,56,33,151,102,144,43,117,1,4,75,9,57,140,7,133,5,172,64,17,193,225,217,4,
  197,20,156,240,212,183,234,93,188,179,242,254,170,93,127,88,103,232,78,215,180,50,127,167,
  81,88,149,171,248,166,166,52,254,120,77,85,78,72,35,172,68,115,32,240,153,244,28,8,
  189,180,33,180,242,245,43,149,211,227,133,187,28,3,246,248,211,165,60,139,129,253,131,48,
  229,23,32,197,200,55,23,186,130,98,54,78,194,130,202,2,239,15,240,140,190,30,139,58,
  198,151,1,189,125,43,243,153,77,220,104,181,45,66,170,86,162,127,141,143,171,209,134,201,
  184,26,109,225,79,191,11,123,99,252,131,37,143,117,204,201,14,238,218,88,94,26,176,204,
  106,149,52,246,80,199,19,111,17,67,91,54,64,95,10,39,221,142,12,198,81,160,193,33,
  31,159,97,90,42,149,97,177,194,177,46,217,57,95,60,37,239,114,42,147,33,48,151,105,
  31,198,255,116,97,145,64,203,219,220,29,18,198,223,169,5,138,136,26,168,91,127,164,174,
  231,27,185,4,246,119,74,251,199,135,205,70,85,160,245,63,237,56,192,68,6,50,188,129,
  1,203,3,85,49,130,124,22,22,64,81,113,81,93,210,204,223,66,47,27,145,158,207,8,
  134,222,160,24,49,146,209,187,120,203,187,4,128,76,188,7,58,93,209,230,155,215,86,142,
  220,241,184,134,83,194,208,35,143,240,206,50,128,62,156,179,174,159,118,135,250,130,52,23,
  46,247,64,101,31,74,203,165,214,39,103,225,53,64,50,199,67,156,174,2,198,73,239,230,
  238,99,92,189,72,78,191,126,229,127,113,111,117,130,149,96,165,142,75,141,44,218,64,174,
  11,47,101,110,182,110,156,1,87,79,232,55,59,3,171,247,2,81,36,6,229,58,8,24,
  161,84,38,100,72,135,190,1,177,95,191,214,42,76,124,128,88,181,242,104,121,78,134,19,
  201,129,95,13,129,249,255,116,48,222,237,24,186,73,124,229,108,102,214,75,33,190,255,236,
  82,189,162,90,160,167,189,205,60,5,98,234,21,84,9,175,174,189,205,148,162,209,69,21,
  228,125,114,129,153,14,140,87,74,196,195,246,210,82,191,251,253,151,9,52,108,30,212,93,
  147,197,195,67,196,139,164,203,102,83,49,28,9,24,14,220,11,46,130,191,26,113,148,97,
  211,236,57,4,194,151,188,90,105,235,244,163,118,250,160,239,135,244,70,201,221,123,209,37,
  149,14,30,240,31,221,208,102,71,159,63,231,111,181,141,91,222,70,2,219,192,209,102,159,
  214,134,127,209,253,254,48,167,101,248,80,79,204,52,37,115,115,99,20,156,135,163,205,55,
  68,54,149,136,224,222,141,135,165,55,175,55,150,37,29,19,255,254,139,218,98,3,64,227,
  69,239,95,222,3,22,163,207,75,97,26,116,251,35,174,132,239,112,28,61,181,46,108,119,
  217,53,13,214,186,227,135,127,173,59,139,69,112,183,217,83,32,182,229,221,146,57,253,82,
  208,239,147,195,98,76,79,125,95,157,35,90,152,62,179,27,55,186,121,178,244,68,27,163,
  135,127,81,206,141,90,100,119,65,156,15,112,148,183,65,98,110,217,33,182,247,193,43,241,
  40,113,243,212,106,94,73,236,67,123,30,228,124,104,72,202,43,66,64,46,227,135,74,245,
  55,136,254,203,68,89,229,174,19,57,193,131,59,24,183,91,152,124,144,170,29,24,44,62,
  244,22,35,65,226,83,205,225,107,85,45,43,16,134,215,192,77,198,74,166,98,193,74,10,
  166,58,214,13,93,87,17,33,3,46,127,133,200,26,225,64,252,41,27,114,82,255,48,30,
  160,199,189,128,227,243,77,198,176,182,157,222,255,20,140,110,128,74,151,150,212,237,49,137,
  16,3,4,119,235,119,245,86,163,94,7,37,56,154,81,30,22,195,78,121,69,180,175,147,
  170,115,24,76,216,28,151,60,181,208,122,40,119,60,215,189,205,235,229,246,218,66,188,62,
  73,215,235,218,31,18,198,187,174,94,63,211,174,105,112,213,143,198,231,112,173,66,129,18,
  17,64,97,117,177,218,92,64,1,104,223,129,13,31,137,254,201,72,125,113,71,253,242,176,
  178,196,254,39,26,212,37,126,248,180,252,164,175,103,55,85,39,253,61,200,147,225,215,141,
  219,24,227,27,215,70,200,86,245,189,122,181,131,239,243,16,254,104,14,3,246,60,64,195,
  190,33,83,174,252,120,177,109,112,33,163,2,148,126,145,66,233,165,55,232,37,75,134,209,
  195,96,80,180,66,174,49,146,245,155,45,233,18,126,82,105,208,254,160,2,69,21,18,135,
  167,9,62,90,171,241,216,101,172,101,17,113,19,11,63,143,17,244,127,155,244,18,192,203,
  109,20,254,113,61,70,184,18,64,13,28,76,82,8,224,3,28,20,2,211,154,231,39,128,
  114,244,54,161,82,158,255,125,68,210,250,223,36,226,135,255,227,4,124,242,253,136,146,222,
  139,183,249,171,137,255,126,208,187,34,26,235,77,56,136,130,178,87,6,203,139,112,43,201,
  82,159,66,168,44,37,160,49,33,239,44,13,130,201,231,10,224,129,139,134,201,22,224,167,
  195,240,195,226,245,139,70,120,225,173,191,143,171,233,185,231,230,223,77,203,239,161,90,140,
  140,139,241,37,167,92,79,144,226,119,218,126,189,89,19,214,246,173,140,100,39,44,55,215,
  16,112,160,90,247,27,184,132,214,223,198,150,79,38,168,100,46,53,86,252,70,203,111,172,
  72,173,147,144,41,200,19,209,239,252,126,66,98,196,105,4,179,234,32,33,105,33,69,254,
  161,160,56,78,8,25,126,78,16,130,82,222,113,40,2,49,173,232,104,28,12,88,121,205,
  165,159,12,223,249,89,46,252,23,145,146,161,215,199,70,138,66,183,32,83,72,205,126,137,
  99,23,87,75,175,72,206,114,95,58,23,242,98,74,117,106,245,26,100,10,154,161,238,91,
  50,152,189,166,72,34,118,247,32,212,159,140,169,208,68,33,236,193,205,189,16,97,83,145,
  98,168,206,33,194,25,201,40,162,170,231,71,15,39,204,218,113,100,228,35,242,238,82,54,
  112,57,8,111,161,201,155,230,248,13,144,224,84,124,24,192,211,237,155,224,218,40,60,64,
  243,214,73,39,191,0,189,55,227,117,105,123,151,206,59,71,212,41,211,219,202,69,45,236,
  120,220,16,216,40,248,195,186,102,47,116,111,147,76,202,254,29,246,143,66,63,213,33,120,
  93,255,109,66,158,241,16,143,102,119,24,193,115,22,138,90,94,235,215,221,77,144,231,163,
  178,108,244,239,178,195,119,163,242,219,216,207,84,251,61,116,29,89,65,216,91,227,212,76,
  220,150,26,165,217,97,106,234,146,98,69,169,233,180,85,74,62,122,204,114,3,24,138,114,
  179,97,80,170,237,54,129,54,249,84,161,33,30,194,11,94,103,101,101,165,81,7,116,215,
  234,107,107,157,181,134,223,168,182,42,106,34,9,79,228,181,148,106,53,59,43,126,19,131,
  77,50,96,94,71,157,142,95,95,225,44,188,143,78,143,121,87,120,18,137,218,34,34,37,
  143,225,64,128,107,52,106,173,85,159,254,84,214,223,137,171,1,122,71,131,104,42,45,46,
  18,103,255,11,223,165,75,141,134,207,110,73,186,248,1,231,9,244,143,56,12,225,172,24,
  123,141,87,137,11,252,93,193,113,177,58,21,191,5,59,56,97,64,217,45,61,243,223,19,
  153,222,165,236,211,251,112,29,156,92,140,14,123,222,78,16,193,190,224,236,46,60,131,59,
  139,248,242,12,59,48,244,116,189,203,80,234,189,76,116,61,149,241,82,53,248,58,210,25,
  175,35,213,224,222,13,53,51,141,226,179,97,20,222,112,123,103,195,113,224,81,151,84,245,
  53,174,91,20,190,141,146,136,228,165,64,217,212,208,249,136,92,117,95,220,81,108,140,95,
  163,222,199,165,149,234,170,191,212,192,101,64,255,225,227,212,127,197,129,51,222,13,232,239,
  11,254,187,195,127,255,228,191,215,92,245,24,239,4,120,46,32,73,99,175,218,105,19,103,
  139,225,29,37,229,47,140,41,187,145,79,10,103,163,24,33,199,160,172,8,28,120,57,228,
  223,33,57,28,135,97,69,89,211,253,150,252,9,126,130,200,18,72,224,25,92,51,147,34,
  196,248,9,45,208,107,3,26,34,69,13,208,135,242,181,30,229,232,7,56,88,79,111,78,
  239,187,115,0,95,103,133,248,63,115,131,210,85,77,205,225,63,109,34,181,178,2,178,131,
  223,96,16,203,98,161,222,89,173,173,252,171,81,111,173,224,158,239,180,86,252,112,57,253,
  72,227,116,226,10,89,159,108,0,123,227,31,45,4,7,179,82,174,44,208,248,166,206,245,
  251,105,114,121,30,148,65,0,150,43,155,213,246,150,215,89,245,241,82,85,111,227,210,104,
  180,219,126,3,8,31,184,30,79,169,92,100,161,90,71,112,3,226,157,210,43,155,146,27,
  160,14,144,104,221,219,248,82,55,106,205,15,42,156,247,103,4,69,221,155,9,39,194,83,
  144,31,44,42,25,109,12,23,117,11,109,191,137,142,131,165,22,47,65,176,216,164,74,230,
  242,77,167,182,218,198,220,104,30,147,69,252,174,184,139,218,89,107,173,52,90,173,86,102,
  89,39,220,102,67,11,55,8,40,206,34,146,219,170,215,81,10,214,97,130,148,189,13,125,
  13,53,56,195,221,196,130,147,234,74,7,183,83,5,179,251,253,184,119,12,18,236,187,198,
  106,179,223,108,227,154,191,143,37,225,188,117,62,56,135,240,19,224,231,87,155,157,138,127,
  172,50,26,181,198,74,131,74,222,12,117,66,39,108,55,144,48,209,9,120,17,15,168,196,
  231,129,36,244,59,65,173,141,71,116,32,49,180,213,174,248,23,170,45,160,247,198,249,5,
  74,254,164,75,174,182,250,173,38,96,204,16,187,7,211,244,221,22,243,39,163,159,47,226,
  63,146,38,119,31,50,58,188,141,202,92,164,162,35,222,141,193,231,96,205,9,199,111,161,
  225,49,100,252,102,145,70,46,22,228,86,193,162,186,8,16,223,240,203,18,66,71,32,77,
  138,248,224,227,101,100,100,169,243,196,217,225,129,200,197,11,169,14,98,127,99,15,243,124,
  34,195,4,113,241,164,145,222,20,142,244,70,76,16,117,234,8,135,173,79,169,238,248,111,
  102,141,31,33,22,110,210,241,239,69,50,126,237,156,75,161,65,53,128,88,144,225,20,146,
  39,60,84,78,40,248,33,222,44,17,238,38,242,51,36,83,181,90,165,64,150,130,27,177,
  15,231,35,48,19,189,24,246,22,169,114,74,100,45,213,151,139,203,46,244,185,191,107,174,
  182,154,13,136,227,207,47,187,226,82,21,80,58,237,142,123,75,230,66,244,47,128,124,186,
  163,94,171,241,208,163,135,83,33,255,31,69,107,125,11,173,245,53,90,171,55,86,21,29,
  222,207,163,53,188,182,150,111,172,83,24,248,55,121,148,134,22,42,21,223,46,54,161,175,
  49,201,106,58,181,90,137,184,199,235,187,210,246,4,244,225,39,228,80,251,28,115,29,252,
  108,72,138,40,158,74,220,129,32,155,112,15,56,71,136,235,70,161,167,26,165,240,143,128,
  146,70,187,227,119,90,114,208,175,245,65,239,87,214,175,221,19,206,249,67,27,198,238,163,
  50,31,4,62,250,177,28,125,56,142,36,247,150,216,57,12,161,251,12,170,12,209,32,236,
  14,98,60,92,35,18,248,207,19,120,8,239,62,123,22,224,197,90,111,252,48,183,187,240,
  239,60,76,1,239,14,236,142,128,19,184,140,97,186,199,31,18,86,35,80,102,66,143,110,
  82,104,109,82,104,54,169,166,253,52,133,249,77,138,29,52,121,33,255,215,240,0,142,54,
  206,142,69,212,196,59,87,199,45,80,243,91,160,143,128,170,235,171,148,73,77,112,41,251,
  2,1,242,25,12,240,34,228,153,244,178,46,171,11,180,113,61,181,86,168,64,193,182,198,
  51,182,53,22,232,240,218,128,142,198,154,134,13,79,53,205,219,29,241,118,183,235,25,188,
  14,170,37,41,216,110,9,136,120,59,225,237,62,49,216,125,32,81,221,161,144,64,58,83,
  246,126,215,172,141,157,228,54,22,108,252,164,202,206,190,56,165,9,140,15,115,60,255,101,
  194,219,11,83,210,139,59,145,96,227,231,36,221,233,253,81,30,113,108,39,229,143,226,38,
  23,237,70,21,21,218,45,34,99,140,122,21,212,197,199,198,169,78,156,234,196,169,149,200,
  163,57,37,155,96,11,160,183,49,81,191,9,130,187,214,104,250,109,31,234,119,32,143,176,
  54,76,126,209,16,97,27,140,75,226,50,36,150,141,198,190,66,46,93,113,141,147,150,71,
  101,61,205,226,255,136,165,175,214,155,254,210,42,46,34,220,67,51,11,100,242,41,131,10,
  160,245,165,122,189,186,218,230,66,181,226,18,217,2,68,25,94,221,245,80,238,125,76,84,
  136,98,60,121,156,29,26,46,223,125,97,59,92,27,92,208,211,197,213,157,131,204,235,134,
  32,162,176,246,209,70,123,157,98,218,171,94,235,171,184,82,253,14,254,91,170,215,170,173,
  197,104,161,141,85,5,27,228,47,53,171,206,12,235,180,58,52,194,26,213,88,234,208,205,
  78,43,148,54,142,22,208,124,29,62,218,23,73,128,161,187,160,185,212,234,244,167,213,246,
  35,218,8,191,85,109,65,130,124,1,109,182,193,26,78,37,80,59,10,122,234,109,187,84,
  18,145,99,237,128,126,212,234,219,47,222,151,78,94,145,252,243,237,75,100,74,41,207,95,
  243,171,43,109,255,35,239,108,195,111,158,242,157,224,125,183,54,104,93,64,3,238,161,146,
  153,117,139,103,173,196,94,191,70,184,94,84,183,199,124,123,150,163,69,64,6,248,63,30,
  239,199,169,192,203,82,93,55,27,116,64,93,226,112,243,109,210,129,118,142,34,76,45,72,
  251,99,82,174,182,1,255,56,139,173,213,20,131,10,201,157,182,160,144,39,29,170,208,198,
  136,214,149,21,186,167,140,7,211,33,63,195,250,134,52,170,59,184,127,89,154,2,241,157,
  239,189,218,61,59,62,170,181,206,222,157,121,139,50,31,154,71,135,246,170,121,202,251,24,
  99,134,43,72,194,166,215,25,82,233,207,100,104,145,204,165,241,197,199,165,106,11,139,219,
  90,59,133,34,89,140,58,216,110,250,95,163,13,2,162,138,85,169,2,70,239,85,132,6,
  26,47,14,54,23,163,44,254,175,189,106,74,174,130,240,179,74,42,211,109,107,213,118,147,
  114,181,65,68,98,13,237,175,50,105,115,15,160,11,220,37,80,205,53,27,132,83,105,5,
  2,155,22,38,50,191,197,52,62,207,17,128,134,226,171,252,239,82,181,177,56,89,224,35,
  75,19,168,129,168,180,209,36,104,149,36,183,216,171,56,164,109,120,123,53,203,157,100,121,
  138,166,195,80,100,38,67,29,19,247,226,243,104,51,80,176,2,130,21,175,253,25,12,235,
  143,129,161,163,233,125,183,74,122,95,235,137,158,226,187,129,224,78,220,158,184,26,177,193,
  75,180,193,165,255,186,219,89,41,241,131,128,181,209,13,138,238,74,91,221,6,10,4,122,
  165,189,151,132,83,62,37,109,95,126,130,54,31,172,174,118,154,164,123,208,89,89,107,157,
  55,232,87,176,182,26,4,23,244,235,188,29,244,219,45,239,20,39,36,93,99,251,12,169,
  211,10,29,157,221,230,129,25,65,131,64,164,213,241,63,242,217,162,237,92,163,237,55,199,
  114,181,21,54,3,117,126,218,196,173,171,102,78,88,36,181,93,18,165,23,58,226,63,161,
  225,218,187,110,169,84,215,218,46,13,253,163,169,127,128,179,193,173,92,133,56,65,16,0,
  196,113,186,167,213,70,251,194,244,212,164,147,74,248,243,157,161,77,223,197,134,231,39,14,
  157,217,253,243,96,124,246,122,119,239,236,54,56,107,33,178,250,158,71,165,92,97,71,27,
  7,9,52,28,177,211,148,105,14,238,189,117,112,41,195,92,133,202,99,68,117,5,210,14,
  5,71,239,4,155,190,139,129,0,215,232,40,54,52,66,133,82,92,205,197,166,124,16,27,
  132,136,233,207,105,69,234,85,213,37,129,197,142,212,25,6,138,246,85,102,157,228,4,109,
  198,222,17,221,142,216,126,58,171,132,196,177,218,40,225,237,190,226,101,238,148,44,237,153,
  78,169,206,47,43,164,134,92,175,2,71,211,85,68,154,143,4,80,56,57,6,181,174,212,
  90,23,13,240,116,68,98,215,100,125,91,171,243,17,45,225,39,72,237,24,61,97,93,207,
  126,62,110,172,214,27,59,41,130,250,184,132,203,142,14,231,26,48,47,254,163,175,83,6,
  160,53,156,35,220,237,180,166,236,182,157,103,9,152,94,161,153,97,25,106,43,116,156,233,
  255,111,134,58,143,6,79,55,26,95,226,140,197,234,117,189,64,217,236,37,43,223,76,33,
  22,252,215,104,50,114,108,242,178,79,213,213,142,46,219,109,126,69,224,190,9,75,201,29,
  160,202,52,164,241,90,91,234,82,219,109,186,154,127,143,171,172,246,196,8,255,138,156,99,
  131,128,171,134,87,17,235,144,107,225,179,41,229,23,20,58,32,49,47,160,241,190,135,233,
  191,80,72,1,12,211,181,162,173,232,45,133,120,88,251,46,19,130,194,255,72,255,46,41,
  116,216,208,91,9,170,215,217,199,78,235,33,191,8,116,221,119,78,43,58,57,145,228,102,
  135,24,250,83,173,4,61,137,249,126,130,16,70,253,105,48,131,134,101,104,186,151,196,195,
  3,161,176,29,17,12,67,120,60,16,89,114,95,176,25,78,95,105,177,212,42,209,225,243,
  63,242,105,107,18,28,211,129,59,149,35,28,68,12,78,80,81,5,154,226,55,254,179,227,
  38,29,89,126,152,63,35,137,180,199,232,0,187,208,97,208,193,198,64,193,180,81,149,45,
  111,200,5,160,32,70,101,242,141,79,153,250,54,60,142,117,22,221,3,52,4,130,79,162,
  121,232,52,53,101,223,177,216,40,32,131,88,58,6,42,165,46,26,66,141,180,154,117,46,
  168,151,26,68,127,167,85,87,248,168,69,235,172,154,239,96,33,107,171,44,9,70,235,24,
  6,86,204,234,189,205,227,146,49,87,9,193,113,207,238,89,131,248,135,14,155,78,156,242,
  14,225,16,97,119,84,43,205,6,19,146,4,231,208,228,227,254,234,107,116,220,64,233,17,
  190,56,182,218,20,231,24,13,118,132,161,170,3,100,100,24,53,204,28,14,163,171,107,43,
  139,33,42,174,84,184,233,180,29,92,171,252,50,116,201,116,42,85,173,251,130,63,80,16,
  245,155,24,195,74,71,40,213,181,122,191,223,26,16,165,74,16,161,23,17,219,159,190,53,
  236,188,208,42,150,37,121,87,95,83,155,75,15,99,43,10,32,254,45,0,193,202,203,0,
  137,183,151,144,185,158,77,199,103,132,209,235,141,159,80,158,72,88,190,248,26,109,1,135,
  127,71,0,43,76,105,117,149,47,44,129,134,130,117,173,185,235,138,196,233,198,202,250,20,
  105,128,247,127,211,194,116,212,127,68,89,0,29,19,229,204,135,172,213,166,237,95,109,47,
  194,139,124,163,163,55,244,223,145,92,144,88,169,42,35,252,90,147,144,120,195,89,127,233,
  123,69,147,231,212,77,67,248,0,106,25,107,205,157,16,164,212,85,109,220,219,237,224,98,
  173,238,49,9,198,227,106,181,245,159,166,83,165,81,151,42,10,124,81,214,171,55,74,63,
  149,246,118,61,190,78,8,57,126,164,229,106,209,249,78,161,183,214,108,53,90,44,96,33,
  22,163,127,14,210,64,93,169,103,188,213,255,86,91,77,128,69,43,81,231,22,234,178,211,
  231,245,65,179,189,162,119,26,27,36,239,72,177,122,94,106,151,232,21,137,134,161,246,170,
  78,76,5,109,47,209,47,233,230,9,1,211,148,159,96,180,155,157,102,27,115,230,81,132,
  129,92,50,125,144,214,30,221,225,245,78,173,113,118,208,216,101,112,193,155,6,80,119,189,
  149,222,35,161,48,117,97,192,128,80,167,19,45,247,168,133,23,144,201,72,131,174,32,76,
  172,198,60,157,187,85,114,210,120,170,248,115,154,57,130,213,54,122,109,163,87,236,10,26,
  195,125,161,254,107,208,205,76,248,114,181,237,98,71,121,41,28,61,46,243,56,25,165,66,
  15,250,208,82,143,78,141,155,248,41,233,33,49,247,102,63,24,106,169,192,201,168,178,62,
  24,102,228,2,252,186,49,200,200,129,152,148,88,89,117,101,65,131,33,17,168,47,7,46,
  117,36,43,68,18,136,48,144,151,145,129,236,33,225,67,181,131,140,211,177,88,130,210,137,
  248,236,175,157,175,173,120,89,248,111,216,244,67,196,74,210,75,107,213,78,119,141,158,196,
  225,81,27,20,2,174,24,77,205,41,98,78,202,225,25,244,6,103,191,79,8,224,54,32,
  85,106,43,1,186,212,169,128,209,244,217,167,253,162,70,41,184,195,34,253,115,170,152,136,
  6,177,169,171,124,247,247,9,142,240,252,178,162,69,2,68,192,53,228,116,66,222,79,229,
  218,156,125,17,107,230,130,96,146,148,237,153,242,57,198,251,238,88,70,65,132,143,140,85,
  6,136,81,129,10,10,125,126,222,33,200,79,108,210,50,101,9,15,72,126,203,11,136,126,
  59,26,94,39,66,249,36,180,79,4,230,116,15,213,248,40,75,222,132,162,216,85,141,112,
  130,175,146,206,170,66,121,169,48,136,36,182,138,40,62,129,90,255,240,140,148,251,207,104,
  232,147,236,62,179,192,1,34,22,45,221,57,96,222,172,46,68,103,179,218,150,27,182,174,
  214,194,172,50,118,182,15,193,67,95,48,219,68,16,33,253,215,80,199,171,185,216,95,160,
  27,241,39,0,206,206,192,72,140,72,32,40,203,197,107,40,180,66,129,202,124,97,46,237,
  56,51,70,29,136,99,112,39,250,194,114,168,6,153,147,168,43,110,131,237,49,240,221,44,
  29,109,123,130,134,232,63,145,3,172,96,107,214,12,70,92,59,111,173,52,215,188,7,205,
  203,74,115,4,59,157,118,151,32,169,67,158,200,104,255,163,9,239,62,108,9,120,45,63,
  2,17,180,27,252,138,71,150,46,214,54,43,40,55,187,221,77,247,29,59,56,150,27,152,
  254,91,83,251,126,156,229,190,169,12,203,140,232,79,75,54,129,80,62,99,225,1,134,204,
  143,51,249,58,237,182,250,83,19,144,34,132,150,103,216,71,132,232,199,66,4,240,31,185,
  57,27,57,238,54,92,235,212,90,231,179,185,219,22,33,143,63,213,246,142,132,77,77,49,
  59,29,3,62,102,132,29,6,231,65,187,141,167,28,166,42,46,134,188,160,242,56,12,60,
  47,166,133,103,151,81,112,117,150,4,83,162,24,87,4,246,228,16,97,184,23,67,6,248,
  118,85,3,126,163,186,146,202,244,144,219,96,202,175,193,194,44,64,72,187,106,139,219,190,
  55,226,129,239,115,175,222,56,129,109,110,178,66,153,206,99,202,18,173,61,181,134,188,139,
  33,159,145,239,19,238,240,251,36,133,170,106,167,105,246,17,146,213,152,50,25,151,240,127,
  43,190,72,246,90,205,204,78,168,130,26,22,96,27,170,75,182,101,159,91,29,60,129,174,
  122,44,167,171,219,187,205,183,175,50,199,76,207,200,118,29,154,27,89,107,26,179,148,36,
  18,148,197,100,203,15,188,43,3,99,119,86,177,195,108,22,209,60,239,116,6,44,46,192,
  99,108,155,211,86,251,107,172,16,154,62,190,221,90,154,102,106,57,167,10,207,252,18,158,
  141,13,23,56,205,60,15,25,145,150,194,215,83,205,105,97,154,119,119,204,29,18,115,3,
  249,202,58,243,94,64,219,252,56,216,20,90,154,1,33,84,76,95,157,48,22,9,10,88,
  168,202,92,109,139,41,211,180,64,107,85,104,212,90,83,242,153,214,146,37,109,214,91,107,
  45,122,198,100,50,186,226,86,169,219,85,234,78,133,74,150,155,50,68,249,148,43,17,217,
  1,218,99,213,143,23,170,171,76,68,212,132,117,204,247,218,214,189,50,251,73,117,136,240,
  164,122,186,26,189,126,164,253,37,138,52,169,81,15,181,21,166,65,166,138,8,95,225,139,
  0,117,215,176,6,120,210,78,8,196,106,25,188,48,21,41,3,131,35,21,109,171,162,247,
  110,209,117,139,167,110,243,216,81,24,178,79,18,56,214,233,62,2,151,217,9,251,3,156,
  97,205,126,203,21,101,151,228,130,192,249,218,31,155,90,94,197,180,48,237,69,163,166,6,
  215,232,210,82,45,233,62,91,178,223,62,211,4,196,231,79,221,195,170,101,81,216,209,206,
  34,121,131,98,195,196,232,180,58,0,211,178,88,103,234,230,85,98,120,232,105,161,188,139,
  161,152,79,193,207,137,214,255,90,105,138,254,87,189,129,147,254,115,22,69,172,146,100,186,
  77,123,188,130,69,228,2,42,126,27,51,153,75,138,220,251,53,41,212,8,195,205,242,107,
  146,81,148,170,115,90,70,137,201,77,177,149,152,88,75,9,216,146,53,235,42,142,58,211,
  175,137,163,151,68,227,248,32,212,212,219,73,249,124,196,75,84,243,215,218,173,206,106,19,
  186,20,31,226,84,26,97,97,117,210,241,177,179,20,134,135,182,137,214,141,249,16,91,175,
  240,215,169,241,0,180,171,156,240,65,105,130,56,164,85,138,92,164,51,196,111,129,62,180,
  235,2,142,65,211,139,150,167,244,53,43,166,155,194,17,238,212,237,190,226,194,60,213,173,
  44,57,119,203,15,160,216,51,213,109,184,28,211,215,172,110,31,248,18,12,19,212,124,119,
  206,154,124,147,242,245,176,130,96,42,252,81,254,13,209,107,31,41,226,12,11,143,180,67,
  165,80,246,22,102,251,15,133,226,181,183,209,71,141,36,79,123,5,114,97,75,107,111,69,
  139,104,103,54,36,2,186,211,94,106,129,191,26,151,148,164,46,47,107,206,168,110,189,141,
  252,47,68,232,214,186,115,73,52,54,168,174,207,33,212,162,9,90,32,179,203,25,245,163,
  73,189,48,159,234,146,93,251,183,200,17,196,32,126,62,63,74,70,242,196,79,150,136,159,
  44,49,63,169,12,230,245,77,10,187,83,88,11,94,149,18,54,19,37,130,144,247,17,186,
  207,56,148,241,212,177,255,80,107,134,228,73,20,38,88,180,138,216,88,124,4,136,159,66,
  173,233,211,198,248,90,174,76,210,105,238,121,176,209,96,157,253,41,52,220,37,103,243,83,
  170,233,45,36,10,30,143,71,17,57,232,251,32,177,219,253,15,145,252,251,3,54,216,15,
  134,164,59,123,60,234,125,196,63,250,127,167,254,27,36,228,109,78,125,215,230,244,212,191,
  167,251,94,129,21,114,85,12,38,120,28,240,225,118,0,43,172,179,160,107,70,37,5,112,
  198,80,36,21,125,208,233,48,85,55,221,219,99,35,207,212,50,146,173,119,110,201,174,165,
  252,57,147,168,244,80,43,36,224,131,99,136,210,15,188,135,171,39,165,55,252,74,176,186,
  91,218,221,126,39,134,192,205,210,103,49,252,105,34,234,40,198,54,64,215,141,123,152,180,
  192,102,131,168,156,253,58,137,9,51,22,157,165,242,6,30,19,250,87,21,50,232,28,144,
  14,51,238,133,72,223,5,254,191,39,148,20,145,77,245,51,237,152,117,235,217,51,78,232,
  194,104,38,33,251,7,241,17,224,239,14,82,244,150,196,236,148,7,187,63,29,227,225,53,
  163,156,11,19,119,43,39,53,29,34,44,233,37,195,49,206,188,207,122,234,39,48,28,30,
  195,49,242,46,244,174,119,161,182,29,78,117,10,105,190,21,55,162,20,80,117,59,205,54,
  25,235,166,58,51,169,51,8,130,151,158,130,152,45,194,71,123,236,83,6,55,8,117,162,
  4,199,136,5,197,30,117,107,254,106,165,11,27,101,190,237,47,112,119,225,17,125,202,134,
  205,208,28,167,58,166,252,117,48,16,255,135,16,70,213,64,173,119,139,42,253,235,145,74,
  159,210,17,223,133,41,201,56,75,22,2,195,7,86,35,228,85,120,203,228,228,104,124,73,
  106,19,222,250,124,205,17,26,12,235,141,216,251,51,214,234,245,241,204,122,100,51,163,4,
  21,113,102,111,167,234,118,197,173,49,171,118,68,234,239,21,95,52,135,73,107,24,42,223,
  92,197,168,19,83,98,159,174,103,220,173,218,72,175,93,91,175,164,185,100,77,206,23,184,
  81,57,102,198,232,54,238,41,61,10,194,33,108,51,99,217,191,144,187,26,40,142,43,68,
  18,197,48,202,208,185,226,65,202,227,176,106,100,33,26,246,63,159,143,239,196,226,147,145,
  15,84,145,224,99,132,51,112,61,144,101,207,166,54,202,249,228,31,138,130,41,119,234,154,
  220,152,46,217,64,105,147,154,210,38,71,231,170,241,41,48,155,152,242,176,33,147,50,42,
  53,16,240,195,84,107,134,36,228,83,110,43,111,178,73,153,150,195,147,128,32,6,102,5,
  114,206,108,252,43,200,52,242,127,73,212,14,79,123,172,88,182,62,133,174,214,135,88,7,
  177,20,228,76,4,32,210,140,70,46,164,28,87,100,1,41,118,174,166,187,87,3,81,81,
  98,163,101,49,132,37,251,196,178,109,0,27,21,89,188,10,82,40,87,82,229,184,132,206,
  64,242,115,4,157,35,239,153,87,217,2,26,137,213,1,172,83,84,32,120,145,137,73,25,
  46,86,150,165,90,75,19,119,128,193,166,140,217,246,247,162,66,45,255,145,114,92,48,84,
  247,219,78,36,182,145,128,71,236,39,3,208,175,19,5,64,176,193,196,118,98,118,159,216,
  122,151,174,235,173,203,30,237,223,243,254,213,160,71,219,6,136,128,55,10,255,135,137,222,
  124,148,22,91,95,44,36,84,157,47,97,107,1,166,237,25,29,246,97,8,201,39,180,136,
  225,154,227,89,152,157,233,243,231,152,3,166,103,187,252,25,201,208,111,180,87,7,30,40,
  161,250,21,152,124,65,217,116,61,179,15,63,147,141,60,44,95,44,180,86,209,123,34,234,
  216,219,231,176,60,81,78,155,70,33,194,194,103,49,42,244,127,168,8,244,180,26,132,54,
  177,6,121,43,102,242,59,114,147,120,249,13,245,137,250,129,235,151,168,42,63,48,66,7,
  137,3,154,222,220,41,168,8,17,222,16,254,183,193,200,209,213,172,118,243,7,144,95,244,
  221,235,53,201,205,244,148,124,175,208,149,13,12,232,189,17,151,73,211,210,103,237,250,33,
  22,123,140,40,93,151,252,21,248,115,4,179,143,157,237,131,179,95,246,207,184,216,49,241,
  94,1,66,7,151,29,152,201,55,90,4,60,166,35,122,111,239,15,149,133,7,104,165,36,
  48,254,13,118,94,216,148,230,26,174,105,34,20,15,249,145,121,119,31,46,117,42,254,31,
  240,26,144,68,87,236,109,217,90,255,159,7,190,123,85,189,17,191,94,180,161,207,204,134,
  42,83,52,54,153,38,242,230,195,148,136,157,72,157,229,103,52,181,103,31,34,178,91,35,
  114,7,79,179,83,113,101,163,126,243,138,242,47,80,115,61,253,56,74,31,21,7,36,223,
  253,192,6,74,202,248,64,85,17,159,52,100,18,59,233,231,77,235,136,71,80,74,242,224,
  226,52,103,25,159,174,39,150,165,10,196,164,236,199,6,154,136,202,64,22,41,131,94,152,
  177,140,109,213,186,38,134,98,35,13,154,8,53,240,180,100,133,100,174,226,28,39,36,251,
  91,112,39,17,126,5,4,63,112,50,148,250,169,249,247,132,30,134,201,30,3,38,88,38,
  25,206,163,170,195,62,206,162,26,17,80,11,251,222,169,210,64,197,171,210,3,25,16,178,
  127,152,217,179,20,247,49,241,41,197,179,172,142,3,196,149,232,7,48,249,27,247,54,141,
  7,145,73,101,163,14,187,50,68,6,197,21,71,198,252,233,220,106,233,220,234,126,121,204,
  181,43,203,19,86,120,161,253,22,83,150,0,54,119,128,176,202,194,26,173,159,242,153,66,
  137,42,42,175,164,139,111,153,94,168,126,248,214,178,63,35,163,26,36,68,52,200,65,56,
  149,133,35,156,174,63,149,195,26,124,247,47,150,235,97,147,118,133,188,180,80,194,128,18,
  200,14,2,198,236,56,137,22,134,122,228,44,166,230,218,202,97,76,51,86,190,207,96,72,
  156,55,86,63,225,67,178,189,123,2,208,3,76,146,27,156,143,167,154,254,34,195,162,91,
  10,205,94,78,221,107,0,155,98,64,159,96,163,128,245,185,28,111,116,224,41,135,126,44,
  122,194,137,52,238,189,174,69,122,113,30,81,107,200,39,79,54,47,182,193,162,176,25,184,
  33,204,119,250,149,210,47,33,93,244,224,84,182,60,35,9,63,226,220,221,3,148,103,179,
  92,42,144,60,8,227,5,196,62,70,194,253,221,71,152,39,222,159,146,119,34,185,94,149,
  197,225,252,131,242,33,74,15,10,0,53,169,224,168,51,192,62,127,126,151,58,85,138,149,
  83,37,206,217,242,224,245,70,134,230,174,244,163,14,150,80,239,104,219,212,161,137,184,110,
  149,228,116,98,4,141,231,207,205,1,45,24,200,200,246,85,167,160,96,109,42,124,229,254,
  90,31,1,106,199,104,12,39,235,234,138,26,162,191,185,54,12,3,165,236,227,52,199,78,
  12,19,76,244,62,15,245,60,130,155,79,149,71,79,226,135,200,62,137,129,117,250,232,72,
  142,131,202,134,147,128,83,182,78,16,78,177,211,249,8,200,0,137,153,6,181,62,66,43,
  188,220,156,181,229,21,185,61,194,106,90,78,136,212,226,201,121,228,54,229,39,205,158,174,
  192,152,124,74,221,177,147,35,221,122,74,137,192,14,61,61,203,91,114,211,237,237,123,169,
  251,191,189,38,57,222,43,49,156,203,225,222,242,228,28,237,175,240,246,226,10,67,113,166,
  104,94,212,157,251,135,246,181,194,99,147,138,60,54,249,153,29,144,125,161,217,61,201,177,
  220,239,64,194,192,10,167,232,137,53,212,94,212,108,149,169,85,26,34,111,20,182,34,136,
  222,209,149,37,63,184,31,249,185,165,5,5,112,29,100,195,108,14,143,120,93,187,228,213,
  236,187,95,93,198,96,117,95,96,174,134,9,231,81,144,71,7,30,5,253,32,55,84,244,
  175,184,213,186,19,143,90,164,173,151,206,97,187,243,65,159,141,167,122,212,74,47,107,58,
  241,89,226,75,81,167,231,36,137,59,185,179,93,135,80,146,162,203,14,16,250,183,247,246,
  134,12,232,203,145,66,118,208,107,37,218,203,225,36,232,25,96,26,77,71,228,170,102,216,
  4,27,241,150,36,185,96,37,240,225,184,62,184,134,18,124,137,7,176,121,114,36,27,181,
  91,58,57,224,165,219,63,116,188,29,124,98,251,246,15,83,225,212,1,235,65,52,74,28,
  41,77,184,248,159,27,215,186,101,248,249,233,79,34,150,189,16,191,66,235,150,18,62,123,
  123,143,249,31,161,181,109,116,54,206,39,170,174,57,77,208,56,84,54,177,16,86,92,209,
  89,187,71,205,187,253,61,28,142,235,161,182,155,205,111,62,83,119,240,135,224,18,120,196,
  66,193,213,201,117,234,232,132,61,3,218,212,13,203,9,43,21,248,2,251,52,99,118,123,
  249,169,12,181,73,112,95,78,94,80,74,133,140,124,37,224,193,243,193,151,220,237,189,107,
  131,129,21,94,36,116,9,109,73,125,51,241,8,191,255,114,8,64,20,63,25,219,228,136,
  132,189,136,214,201,9,214,232,246,140,105,27,192,152,13,5,96,163,7,32,22,21,51,201,
  236,35,74,18,73,148,114,144,188,193,165,156,47,10,217,114,233,242,23,232,0,40,71,163,
  84,31,228,124,229,65,114,228,29,12,228,243,185,229,23,12,69,112,25,152,50,122,222,174,
  31,83,42,53,236,167,13,89,2,50,228,4,145,201,249,65,111,228,239,55,210,89,169,127,
  163,183,180,108,14,90,189,148,84,164,42,249,31,113,87,1,194,253,196,211,147,231,15,51,
  121,1,107,242,233,95,202,109,59,73,117,231,238,186,45,237,29,104,134,75,163,213,41,111,
  51,129,103,237,70,46,253,105,110,227,108,80,168,150,78,8,134,175,52,32,180,25,36,24,
  184,123,238,13,202,176,80,180,203,122,138,52,112,103,134,185,197,200,3,108,183,244,242,240,
  224,221,247,95,234,245,69,72,58,112,232,158,143,166,235,184,46,174,172,251,97,247,133,144,
  48,43,130,246,32,230,189,11,200,118,93,83,68,176,52,160,201,113,153,231,151,211,245,122,
  67,10,54,106,130,23,237,60,164,113,229,17,139,23,57,25,119,187,13,34,221,194,214,171,
  243,247,140,53,70,42,218,38,230,131,38,184,167,167,100,211,98,92,23,173,66,77,111,246,
  182,138,132,221,28,224,106,233,45,19,127,202,200,191,125,149,138,12,102,137,7,170,165,29,
  93,86,93,207,203,246,229,188,124,73,23,66,223,176,134,230,246,176,239,212,236,61,186,158,
  246,95,124,159,218,27,213,100,255,139,146,32,59,92,191,1,141,100,200,106,227,84,32,69,
  43,187,69,252,168,156,168,75,90,55,115,30,45,130,126,168,53,80,101,66,82,154,9,24,
  83,154,213,97,140,51,63,133,29,46,83,4,242,82,150,34,225,53,79,41,38,229,123,65,
  161,182,128,47,79,167,130,90,87,220,0,246,202,218,84,130,150,135,104,172,96,90,160,165,
  201,144,241,233,30,72,105,220,109,40,158,243,93,7,79,115,215,65,52,201,249,174,67,58,
  156,93,68,87,193,228,94,14,33,220,171,196,47,9,20,233,24,238,212,58,106,184,47,102,
  185,177,51,46,236,250,163,113,18,90,53,87,204,244,136,130,120,180,58,36,43,215,166,118,
  123,187,86,146,115,190,99,87,21,140,144,159,0,148,226,66,105,133,184,89,221,202,151,250,
  139,118,173,245,0,6,130,161,104,22,236,149,177,168,21,211,135,44,220,76,97,170,174,43,
  94,165,100,235,205,1,82,103,67,9,59,217,135,38,220,212,120,236,197,72,136,222,7,96,
  139,154,72,62,149,143,35,17,204,154,242,94,41,192,155,234,18,103,246,28,8,211,226,218,
  9,185,94,245,74,96,164,73,158,154,192,145,209,18,144,2,100,182,224,178,123,30,126,146,
  39,168,240,26,63,189,244,65,201,244,79,162,221,24,76,1,168,93,80,158,184,77,153,111,
  134,124,247,145,121,239,106,202,199,166,10,33,13,226,163,73,190,73,127,191,9,244,196,153,
  157,86,19,135,80,83,184,234,138,229,239,235,161,148,20,172,129,61,241,19,213,29,214,52,
  34,122,175,168,119,11,234,246,168,199,162,5,170,54,244,186,152,101,169,186,235,162,120,126,
  237,237,49,200,131,213,16,142,102,122,30,252,47,145,255,95,5,87,205,181,230,67,233,149,
  70,33,41,22,124,14,87,30,235,196,168,236,215,108,15,182,59,43,68,203,129,24,34,68,
  25,140,21,246,220,88,14,10,174,55,65,225,180,179,180,153,4,47,228,69,176,35,191,30,
  244,93,167,124,107,175,118,155,107,138,57,7,20,103,50,87,224,111,155,239,195,236,147,95,
  169,152,127,42,253,160,13,237,244,179,41,237,237,121,129,59,94,125,147,61,20,223,100,64,
  51,69,247,216,188,139,11,163,95,218,91,173,189,253,193,162,62,179,136,142,25,184,233,41,
  0,233,53,201,249,118,3,18,145,16,59,231,188,31,234,171,233,245,187,159,65,151,12,131,
  123,123,150,237,153,178,114,145,63,199,151,40,111,11,161,31,37,88,8,165,218,244,202,247,
  95,110,99,90,129,125,145,72,121,190,178,122,208,212,180,67,112,210,166,145,53,111,24,19,
  130,151,138,7,241,173,231,91,14,110,183,155,99,197,34,152,59,230,134,234,68,241,45,72,
  197,153,7,246,151,156,91,233,76,27,250,180,42,121,152,65,84,253,139,135,210,213,55,158,
  207,172,36,70,86,39,130,51,30,110,187,232,100,54,106,230,100,214,82,148,85,115,207,230,
  5,185,89,203,157,144,60,144,43,241,148,140,34,3,212,70,84,163,9,180,235,42,159,9,
  33,0,214,230,80,26,130,221,233,12,164,7,92,29,140,98,9,120,241,177,202,28,22,98,
  154,149,46,227,214,156,3,225,6,80,0,244,44,18,65,110,249,201,251,240,238,245,59,60,
  105,159,67,48,120,118,14,211,237,234,245,148,54,205,39,240,101,9,65,75,203,189,72,175,
  1,229,68,54,71,143,17,84,174,162,9,27,226,64,201,149,211,118,76,133,142,160,247,2,
  254,124,114,95,58,140,208,66,249,248,138,98,14,253,80,145,166,13,245,248,57,199,216,108,
  244,65,104,109,110,31,248,9,36,210,16,117,67,174,138,19,193,137,124,55,188,223,62,58,
  65,151,250,88,149,88,57,219,76,230,92,6,48,176,15,124,164,70,136,61,26,177,220,76,
  157,31,20,111,238,17,16,13,141,75,42,139,22,252,33,199,59,247,179,236,217,56,37,241,
  142,165,144,220,220,17,145,241,22,21,150,130,179,170,216,167,135,197,57,103,206,210,41,185,
  203,194,226,238,142,161,37,247,86,28,21,29,115,204,18,118,182,172,27,190,136,194,145,242,
  194,43,175,17,116,60,63,122,6,110,83,5,13,147,100,207,212,172,131,72,207,154,215,37,
  87,119,195,118,243,153,87,64,73,152,80,32,87,182,192,223,120,3,217,242,100,112,250,53,
  24,167,179,80,57,133,222,123,185,32,141,21,202,145,62,25,103,115,87,113,111,211,226,149,
  149,232,147,1,20,170,69,49,41,130,42,222,121,49,174,216,237,229,78,191,121,30,203,203,
  96,48,195,223,69,231,163,169,133,15,250,54,112,25,220,140,16,77,159,208,46,31,78,178,
  49,155,119,50,141,174,145,181,185,32,2,90,186,53,63,61,29,102,237,205,206,248,115,240,
  77,43,5,190,213,216,103,171,56,17,24,55,4,21,225,58,155,176,71,88,74,188,39,182,
  189,63,84,178,35,153,158,146,222,206,59,13,2,202,0,218,244,12,24,55,177,116,143,54,
  156,248,3,92,88,92,203,22,203,102,176,180,208,228,95,173,182,170,245,89,236,58,137,174,
  186,153,151,73,176,62,245,70,179,213,238,172,172,18,46,86,11,215,45,181,150,219,130,202,
  59,203,43,242,163,190,220,144,31,173,21,184,121,162,194,48,18,236,150,234,138,239,174,171,
  92,22,242,211,191,45,99,81,216,45,29,239,109,151,148,202,216,241,238,235,210,26,213,102,
  234,10,245,85,122,115,13,67,97,202,169,174,58,108,213,28,96,32,211,162,121,192,240,86,
  176,124,4,47,120,124,59,40,242,116,91,57,238,37,108,86,50,138,100,41,50,159,247,94,
  84,45,253,210,53,79,25,50,40,24,68,91,62,124,57,233,167,229,163,110,78,120,187,12,
  24,43,191,209,228,155,95,114,9,102,191,180,96,117,108,208,191,95,218,42,165,142,114,53,
  95,92,177,160,136,38,192,74,112,204,35,247,201,163,245,6,212,88,132,4,234,15,246,88,
  154,234,185,34,43,229,22,149,13,31,235,205,235,59,236,227,213,56,30,179,251,120,114,103,
  138,250,69,196,239,222,182,16,177,233,190,201,247,154,179,43,98,245,71,30,58,231,236,204,
  108,5,63,117,147,64,76,69,23,210,116,204,2,30,247,122,173,170,35,229,156,137,237,166,
  146,102,225,6,140,47,121,201,16,189,64,221,78,178,112,43,106,27,101,163,210,197,230,237,
  140,205,144,158,32,19,189,254,163,16,253,25,17,6,112,155,116,60,213,79,251,208,194,112,
  175,69,191,100,116,236,100,40,99,7,251,64,86,193,170,114,150,188,72,173,178,168,38,21,
  74,201,125,168,232,164,50,2,223,246,160,254,43,105,231,112,30,121,95,167,114,169,72,160,
  160,32,103,234,146,134,251,47,40,72,121,186,156,225,239,11,202,241,75,182,46,199,204,180,
  239,49,233,10,114,89,84,232,12,75,238,42,233,105,238,88,116,134,22,61,102,181,220,118,
  36,226,9,55,36,157,93,142,41,24,145,247,156,11,192,209,189,219,134,61,127,97,78,179,
  67,49,76,178,59,148,114,182,33,139,109,198,99,110,226,21,52,108,141,237,135,73,249,83,
  127,64,250,58,61,165,139,39,126,179,221,70,23,136,31,135,239,240,66,21,156,233,83,88,
  111,53,63,155,205,201,140,34,140,213,40,76,215,74,157,140,212,2,248,229,88,26,16,118,
  39,83,25,188,205,211,106,107,6,35,187,186,22,83,51,127,171,193,234,120,249,198,178,43,
  122,145,29,13,215,54,227,80,244,89,182,158,7,194,41,7,25,206,162,187,156,105,252,151,
  200,71,207,118,239,188,127,39,111,115,244,210,101,185,114,142,244,179,219,65,104,191,187,1,
  220,36,166,153,241,244,142,181,131,53,9,133,140,154,184,203,230,39,70,41,150,51,85,104,
  109,50,253,234,145,135,99,122,94,59,8,139,158,158,148,63,175,15,169,1,198,58,41,54,
  234,71,31,63,204,251,143,151,247,154,238,239,209,71,82,119,57,245,127,143,63,90,165,84,
  88,40,206,194,28,202,230,89,200,15,103,121,157,15,139,188,206,235,186,164,225,21,194,8,
  105,147,116,42,110,18,192,4,206,127,59,17,171,64,170,174,11,210,195,16,84,158,101,44,
  167,164,85,131,127,237,241,127,215,175,53,215,26,231,42,212,73,89,61,19,145,119,178,62,
  197,250,129,152,183,244,11,189,118,107,172,108,74,129,236,245,73,159,135,28,157,167,87,113,
  193,91,49,21,114,154,178,136,181,197,50,233,31,108,121,74,107,255,166,209,129,16,85,43,
  23,188,218,229,81,233,14,233,161,201,87,42,22,86,107,242,138,186,87,99,229,45,41,201,
  183,142,255,233,4,145,194,181,120,9,139,120,173,133,78,216,168,52,57,188,166,119,241,60,
  36,200,107,84,30,16,252,184,87,188,175,10,66,24,177,0,10,98,6,129,248,180,168,109,
  251,213,36,76,165,77,6,202,228,1,192,255,43,143,252,122,9,228,85,192,55,250,13,91,
  230,8,227,177,66,132,128,187,180,188,228,95,133,60,86,109,167,137,118,37,114,65,173,0,
  68,183,124,153,216,37,216,255,54,180,212,116,251,68,56,128,117,190,151,215,100,90,25,165,
  88,165,242,183,33,169,203,170,80,108,235,135,4,173,76,209,245,118,115,180,157,209,32,204,
  4,44,170,223,167,16,73,212,63,235,98,153,235,80,67,14,160,129,54,123,28,176,48,27,
  244,51,125,245,229,235,147,57,144,33,159,71,12,86,105,150,20,128,191,173,175,83,32,27,
  227,93,217,222,135,142,148,82,208,54,55,56,249,242,123,254,220,104,92,7,28,238,83,105,
  92,3,77,197,164,82,169,244,127,181,182,137,111,223,254,190,78,213,115,43,2,172,84,136,
  89,4,86,200,117,166,7,105,163,54,78,49,115,122,117,240,242,149,36,26,237,203,52,248,
  27,239,110,58,197,135,131,208,18,12,81,64,211,178,17,86,64,237,81,233,148,164,123,44,
  140,221,254,73,233,173,114,151,230,192,95,62,55,109,227,187,70,184,122,14,191,12,121,104,
  20,1,135,148,220,37,23,253,250,168,102,53,145,12,210,90,140,13,218,210,168,182,60,15,
  69,85,30,193,81,34,155,96,68,85,97,111,241,138,29,55,235,193,140,43,20,103,55,154,
  246,33,73,133,156,57,153,0,26,19,129,65,235,164,104,85,164,165,217,75,194,188,47,173,
  3,253,192,205,128,160,41,158,25,25,113,66,136,243,91,214,92,17,149,35,245,82,149,173,
  153,20,133,71,255,208,189,237,102,120,1,133,73,149,58,97,142,61,243,172,224,167,90,69,
  36,150,163,188,215,140,108,197,156,147,39,169,218,24,125,213,41,225,223,216,121,102,254,235,
  106,56,36,151,113,213,127,179,234,55,5,180,193,86,50,155,125,123,25,217,28,23,105,153,
  124,86,186,51,77,193,28,128,152,161,209,186,177,95,39,110,36,84,94,106,5,22,23,72,
  222,171,37,91,3,66,196,44,196,230,22,232,180,176,2,196,149,72,4,179,12,28,137,236,
  19,120,103,26,79,222,197,243,69,253,130,46,152,88,118,217,186,188,221,196,15,5,19,85,
  252,155,19,225,139,216,111,218,36,33,164,48,26,38,152,248,43,19,235,229,17,189,26,77,
  28,169,55,0,160,180,135,71,116,107,10,158,4,10,71,157,234,136,120,78,79,133,47,2,
  234,65,160,85,248,130,73,131,242,212,138,135,96,219,247,73,147,25,75,158,135,147,25,42,
  155,120,119,83,196,230,120,226,238,64,238,105,156,70,42,158,89,139,94,201,55,23,75,187,
  16,27,0,200,68,232,146,123,217,102,177,72,24,64,45,205,126,228,222,84,149,200,12,159,
  100,146,43,217,103,237,28,171,111,211,92,31,225,73,5,226,212,214,202,41,78,106,1,241,
  245,17,230,198,16,182,54,234,156,79,146,174,22,4,230,87,221,82,238,178,85,15,2,250,
  149,32,167,241,153,10,67,243,52,220,220,3,171,212,36,172,163,183,191,166,227,68,64,182,
  127,110,160,227,82,113,49,25,105,63,142,95,181,180,91,252,176,34,130,212,61,173,147,100,
  30,6,138,159,124,44,49,170,18,150,202,51,222,35,175,219,217,94,173,102,178,50,122,217,
  226,1,69,234,243,92,129,61,9,190,63,170,80,197,138,176,204,189,179,166,24,197,35,159,
  140,202,168,208,251,197,40,21,113,50,135,41,86,169,25,77,34,206,215,145,139,125,82,222,
  38,13,37,78,149,144,214,246,3,226,238,11,233,133,201,107,26,19,169,106,113,18,34,111,
  235,65,82,188,237,83,245,40,0,95,37,227,98,187,212,64,76,195,216,112,160,215,11,242,
  175,2,227,71,94,5,10,95,82,149,168,16,135,11,94,152,151,56,212,67,237,250,110,93,
  252,138,0,171,197,41,86,203,99,185,191,170,35,86,188,53,221,146,232,142,253,15,40,142,
  205,56,103,194,207,204,59,104,25,181,36,37,183,20,210,96,247,5,250,99,85,1,87,81,
  99,185,180,38,169,74,52,92,248,50,174,212,87,108,77,174,84,192,172,30,29,216,254,39,
  171,145,109,93,134,52,252,109,1,126,91,245,213,125,232,152,161,126,192,24,98,135,86,147,
  159,197,159,197,138,103,123,236,190,84,90,50,83,119,93,92,203,67,91,25,199,145,139,73,
  164,91,75,47,199,186,222,254,162,86,142,126,217,94,171,21,106,224,108,254,77,181,44,165,
  143,165,118,244,219,212,178,82,125,172,210,211,106,167,90,89,51,213,177,254,162,206,144,29,
  216,184,64,113,40,22,171,156,255,77,170,66,50,32,111,150,206,206,127,171,106,78,161,86,
  126,170,155,0,250,71,61,247,91,239,215,133,42,59,221,188,170,206,60,100,99,244,114,102,
  169,217,228,212,74,72,239,38,197,7,223,172,8,163,40,100,93,27,111,75,184,55,10,233,
  60,139,160,153,169,16,9,53,153,73,241,211,165,210,80,57,10,37,154,19,67,156,10,84,
  254,13,204,252,195,99,103,224,47,170,225,56,40,74,101,137,212,254,127,141,110,78,58,168,
  111,82,210,177,20,155,48,121,210,112,121,178,214,142,253,2,168,53,104,230,18,164,140,166,
  99,121,209,19,209,192,139,186,14,111,6,166,209,214,39,169,150,242,186,250,80,39,40,214,
  75,81,239,110,217,157,212,84,170,5,254,50,204,12,111,40,186,0,217,107,219,75,141,80,
  255,47,208,49,201,143,190,64,227,68,91,79,44,170,90,90,235,132,2,241,20,145,151,99,
  38,47,17,213,166,103,76,112,243,36,102,240,168,226,137,150,112,216,157,209,102,144,192,106,
  236,207,80,65,25,47,242,3,155,17,73,34,56,122,16,57,154,40,22,119,119,1,163,242,
  241,31,187,193,36,113,89,60,75,128,220,212,4,205,222,90,17,164,132,204,11,54,247,210,
  203,52,119,130,118,245,74,207,212,119,153,165,236,50,87,189,69,196,82,115,9,76,197,16,
  216,50,170,132,55,87,27,62,237,24,38,171,128,188,243,29,10,210,61,215,190,195,155,137,
  216,229,74,158,223,181,42,152,177,57,114,223,227,211,51,197,19,224,35,181,107,172,77,172,
  183,254,177,110,46,251,28,239,25,11,241,25,202,44,12,57,92,0,170,183,240,168,15,136,
  171,225,239,131,22,130,144,191,92,87,12,98,107,12,37,143,26,219,24,69,161,130,251,47,
  161,29,220,190,161,7,249,198,213,28,13,34,89,178,39,234,16,49,52,19,32,104,5,162,
  116,17,49,100,23,43,157,59,76,111,65,211,158,50,170,159,177,120,90,43,106,22,71,246,
  79,234,248,112,145,127,82,195,7,242,229,57,219,247,42,136,248,72,145,246,118,189,131,189,
  185,123,177,103,232,180,89,2,195,2,139,41,209,164,118,212,123,214,93,213,158,234,76,181,
  30,107,235,48,96,119,235,50,7,8,43,227,41,183,7,255,144,158,206,39,241,95,83,164,
  15,146,200,3,120,42,2,213,79,224,1,249,255,48,118,249,65,230,25,159,28,93,124,42,
  48,42,20,83,100,85,107,203,211,177,73,181,201,47,73,41,196,244,246,7,118,5,164,132,
  137,230,245,63,144,128,184,191,133,91,85,218,91,178,246,140,141,130,219,30,97,220,161,66,
  15,188,39,53,214,149,145,127,77,56,211,44,53,201,52,182,186,253,144,93,32,133,38,63,
  27,206,27,63,25,90,107,139,76,252,236,45,186,217,126,200,14,26,144,179,193,110,30,130,
  232,13,243,185,112,213,17,138,103,7,100,147,61,245,96,163,215,168,169,181,183,68,180,174,
  70,3,9,70,101,61,108,177,104,102,27,216,17,69,225,38,40,75,117,107,241,169,172,177,
  186,222,115,197,169,100,89,79,160,27,100,146,211,77,81,17,173,108,237,28,60,28,77,105,
  139,180,214,136,200,70,11,138,16,34,49,101,68,184,150,157,137,3,76,178,154,60,11,75,
  152,159,78,38,72,195,174,10,13,241,142,79,116,130,248,81,246,18,242,224,149,71,100,234,
  69,203,29,216,23,136,113,66,193,219,96,196,27,21,189,136,228,181,197,122,152,84,0,16,
  60,172,255,48,117,164,33,246,52,210,14,114,91,18,147,127,168,91,106,84,23,2,84,152,
  65,212,244,42,103,212,155,208,232,164,156,129,50,181,144,197,170,90,147,242,90,109,190,142,
  214,164,92,155,171,154,53,49,178,147,217,170,78,50,87,241,174,145,61,3,115,116,160,84,
  21,86,119,122,152,173,104,132,214,227,153,8,70,87,51,252,163,187,254,209,140,211,32,110,
  17,166,202,73,183,118,48,13,202,149,225,137,234,65,65,35,101,10,51,126,17,30,102,170,
  37,73,175,218,245,72,102,45,196,11,201,60,173,165,160,64,107,41,141,217,193,242,90,248,
  125,253,104,145,70,66,229,145,144,87,223,244,68,164,208,183,190,62,232,222,163,111,139,69,
  209,84,250,105,5,211,8,210,53,27,209,232,167,31,199,167,189,81,209,162,241,203,224,88,
  252,72,112,177,153,136,27,142,95,82,18,244,235,215,177,38,158,224,2,132,188,10,96,74,
  133,234,84,180,211,198,39,79,22,204,141,103,203,0,158,45,3,120,182,12,224,217,146,207,
  8,200,125,107,18,99,158,132,122,251,12,78,9,209,142,115,167,207,201,127,163,177,128,26,
  151,69,245,59,71,33,101,56,244,59,57,109,119,96,181,80,103,110,2,219,200,136,61,16,
  196,254,32,206,83,16,76,53,235,186,108,28,203,160,201,33,33,121,56,203,76,216,40,62,
  252,126,19,78,238,143,25,183,141,39,219,136,75,232,125,132,137,90,176,68,143,130,167,94,
  234,61,70,252,26,198,60,100,14,164,203,141,82,81,56,101,98,175,203,21,209,248,164,89,
  96,136,3,247,142,119,249,41,237,250,205,21,24,234,154,74,210,134,95,236,235,205,17,76,
  228,157,147,170,167,65,135,92,164,150,200,77,52,200,102,83,94,199,42,170,73,133,141,72,
  196,133,227,169,56,148,219,88,142,180,48,50,225,215,173,204,219,166,235,118,110,230,195,166,
  83,87,143,134,223,86,149,40,20,63,165,169,19,67,177,153,103,86,197,198,217,142,58,139,
  92,174,138,183,215,9,249,92,173,100,183,68,59,117,21,23,159,163,33,188,178,165,158,241,
  254,184,203,121,221,120,254,220,104,249,125,20,213,131,250,22,212,140,148,26,66,131,126,159,
  42,245,191,20,66,163,17,238,103,242,39,4,79,73,218,65,84,69,185,29,132,206,64,104,
  249,147,9,5,193,47,25,143,45,28,28,121,129,181,208,40,99,179,133,207,55,56,226,8,
  153,165,134,161,29,246,67,221,191,180,183,253,161,228,117,63,1,135,117,201,147,220,114,171,
  244,169,178,248,233,39,124,76,203,70,237,160,6,165,174,210,145,155,6,103,217,80,78,117,
  161,78,20,79,62,41,151,58,172,127,164,6,191,229,253,130,33,48,9,162,150,190,228,85,
  78,83,173,136,163,129,241,45,230,174,92,129,110,143,114,99,242,184,74,150,173,140,165,228,
  241,150,23,27,22,57,23,41,94,121,15,202,31,122,190,79,179,234,83,181,234,83,103,213,
  179,221,79,237,45,40,30,64,186,4,87,169,7,102,107,17,124,30,7,227,226,231,207,133,
  183,131,122,72,68,107,179,165,52,94,186,89,184,193,77,240,108,148,131,27,198,29,98,38,
  226,92,91,228,105,241,89,244,245,43,22,123,43,52,234,148,114,213,5,226,52,122,54,162,
  25,17,146,137,244,57,68,108,92,107,199,225,49,94,228,65,114,246,173,67,199,141,99,38,
  3,210,47,81,90,170,231,65,255,243,37,11,83,123,208,212,250,110,117,237,124,165,77,177,
  189,191,27,212,218,43,20,177,128,81,223,61,196,156,87,20,82,53,235,193,21,139,177,53,
  125,162,186,145,109,226,193,188,106,106,31,51,87,19,73,185,102,180,117,195,102,120,28,211,
  202,128,70,84,33,59,71,255,92,7,180,46,4,15,153,7,193,84,171,72,249,38,184,74,
  229,56,78,159,57,169,94,87,184,204,121,114,1,251,197,193,220,245,142,127,229,108,247,200,
  55,196,180,114,177,232,46,57,142,44,187,46,91,134,96,238,136,84,117,149,2,32,225,64,
  104,41,77,25,92,129,226,82,240,130,39,172,100,43,22,40,248,244,31,159,42,93,6,99,
  184,188,147,130,91,108,194,98,84,33,82,87,29,101,152,181,84,60,241,245,92,52,15,113,
  209,218,31,64,44,30,101,6,57,241,85,158,98,171,43,36,68,112,83,220,242,58,102,72,
  178,168,6,90,97,223,133,80,88,28,13,81,247,167,196,138,23,12,227,239,239,106,171,173,
  213,62,195,105,189,5,99,41,196,141,84,69,220,160,210,29,226,16,179,245,41,206,99,24,
  80,156,55,74,230,24,194,231,227,209,160,212,2,135,111,241,247,186,69,142,36,28,115,88,
  221,154,223,65,36,0,55,189,206,233,245,70,167,66,193,132,170,49,34,183,38,239,217,35,
  58,71,59,31,246,130,138,159,163,129,16,92,188,175,21,203,111,112,240,70,154,179,84,122,
  190,35,121,195,239,138,22,194,53,173,69,148,226,229,97,170,39,220,63,93,191,166,61,31,
  26,37,238,13,82,238,30,22,233,116,27,36,35,198,182,92,111,164,153,4,128,131,30,196,
  168,74,10,12,45,75,111,17,41,166,242,186,230,184,189,197,190,187,137,215,130,43,152,40,
  200,230,65,47,225,230,84,248,154,177,212,204,161,31,168,50,168,34,66,3,244,109,111,206,
  211,241,37,48,58,232,86,86,103,245,148,22,161,208,4,125,246,11,87,249,178,207,39,64,
  60,198,143,177,189,186,15,12,246,154,184,44,184,40,141,67,141,97,175,10,15,23,29,11,
  75,161,180,209,134,134,67,234,190,198,177,77,211,230,190,162,10,223,134,43,195,244,74,19,
  46,136,76,77,169,179,201,77,28,211,51,90,182,43,65,156,220,71,129,135,46,75,243,145,
  2,233,117,31,117,125,250,13,8,82,214,96,52,6,145,94,201,123,124,86,194,25,42,147,
  106,208,233,19,108,165,100,55,216,48,8,69,246,20,167,74,43,165,226,187,182,70,170,217,
  25,198,71,246,83,159,38,11,138,91,23,118,219,232,99,39,204,106,234,7,76,221,139,149,
  226,50,213,243,219,254,246,167,207,2,185,148,26,194,108,89,213,35,203,55,208,147,82,92,
  123,58,39,157,96,164,46,143,76,39,82,216,118,220,51,143,106,124,105,240,149,185,62,231,
  0,141,183,228,8,117,189,139,81,120,55,255,36,141,13,108,191,201,223,155,234,192,88,198,
  62,238,229,55,11,52,231,28,167,57,87,126,122,42,138,110,120,200,67,63,177,175,207,223,
  64,181,130,126,182,142,56,167,241,11,48,112,13,68,116,90,234,161,132,213,44,252,208,191,
  31,62,117,191,143,236,190,82,15,2,142,217,33,31,228,12,9,34,239,71,237,152,3,38,
  212,120,133,108,141,231,71,169,135,130,21,20,73,163,134,17,245,85,213,46,109,224,10,246,
  183,80,249,93,103,121,163,185,159,229,43,83,48,37,150,217,133,247,180,34,114,228,138,229,
  118,91,200,236,181,105,170,94,28,164,207,96,236,227,96,100,92,14,144,115,131,42,204,64,
  48,234,251,84,37,37,43,124,174,226,16,33,66,59,217,107,233,189,133,79,99,217,221,175,
  95,205,246,218,131,48,66,255,244,201,203,87,186,227,242,52,52,85,79,74,215,234,153,187,
  221,207,43,136,57,222,182,250,162,150,106,246,82,13,74,69,56,183,172,182,48,204,169,130,
  18,178,188,37,89,6,217,64,25,102,192,30,38,179,238,74,101,204,218,64,45,58,179,150,
  70,235,196,102,196,238,233,210,112,159,36,25,102,103,226,232,116,86,63,253,204,221,196,239,
  171,214,108,226,158,30,62,124,38,213,141,39,246,212,177,177,241,22,106,167,49,210,221,88,
  173,109,177,27,237,93,203,17,136,33,106,179,218,112,218,99,150,245,142,35,143,6,59,43,
  253,210,185,121,37,154,161,27,90,180,14,149,238,51,123,76,41,61,147,31,189,140,211,128,
  9,9,52,211,206,175,114,3,203,14,222,133,13,3,233,12,223,122,44,56,23,189,47,56,
  35,192,26,221,169,127,61,132,20,9,70,153,234,85,3,4,161,3,37,3,80,89,8,179,
  68,113,151,220,56,73,20,210,145,99,103,33,212,100,69,149,176,194,226,129,170,37,217,226,
  47,161,126,30,138,88,168,127,174,223,166,93,141,127,145,217,62,142,192,42,228,160,62,61,
  235,7,145,248,191,103,35,72,101,2,57,93,138,210,145,222,65,246,24,218,9,127,66,142,
  200,55,246,240,254,122,76,193,12,65,250,142,122,241,2,162,143,17,176,142,55,122,35,13,
  159,118,51,189,169,111,55,2,51,106,196,80,178,11,44,246,38,203,227,133,145,83,106,177,
  23,80,218,186,4,81,76,48,140,116,125,52,248,246,101,52,16,229,197,141,50,255,76,192,
  89,244,85,0,184,254,56,193,111,242,40,156,214,92,236,245,23,44,87,220,241,66,135,118,
  53,93,147,109,237,139,29,104,81,176,145,139,152,108,187,82,4,112,10,29,147,146,42,27,
  245,189,82,191,146,30,98,112,209,47,62,199,246,53,163,23,219,146,3,73,25,134,38,241,
  92,139,205,137,125,6,19,48,31,17,162,234,177,225,105,36,32,166,10,114,252,56,93,167,
  254,252,249,68,137,69,122,171,13,178,25,82,89,13,187,84,131,39,103,69,120,255,115,163,
  141,168,113,166,113,52,105,103,110,246,36,119,98,48,129,94,128,116,124,20,93,80,13,80,
  247,211,100,147,178,180,215,166,154,79,162,10,167,241,30,237,90,45,123,160,45,89,164,160,
  167,42,46,53,41,240,29,245,179,78,199,202,120,86,175,215,28,183,241,22,61,233,204,146,
  227,178,45,118,42,11,245,26,25,87,21,61,59,170,247,198,64,29,220,58,61,127,217,71,
  210,87,43,228,3,1,112,176,161,31,121,7,41,176,202,98,195,28,215,34,5,196,189,21,
  227,22,209,60,247,42,82,132,232,74,109,47,82,160,24,81,39,51,6,24,90,63,48,71,
  151,5,155,122,22,108,50,107,166,119,198,90,177,111,88,165,199,22,41,133,66,119,179,235,
  106,179,83,216,40,222,110,23,46,173,74,28,62,46,173,85,4,249,77,187,110,115,22,228,
  103,1,42,3,249,155,107,136,139,56,11,242,55,122,146,59,3,242,249,96,82,152,96,119,
  156,234,22,168,251,10,245,231,151,144,250,121,18,32,185,112,146,130,21,16,250,103,69,204,
  104,165,10,6,144,20,135,33,238,4,112,24,100,157,145,79,116,51,145,213,186,167,239,105,
  0,62,11,215,243,1,178,94,165,226,139,41,204,28,51,247,81,100,201,143,203,133,113,183,
  114,66,16,180,242,101,170,87,100,154,6,106,224,47,132,117,30,68,34,69,161,79,9,240,
  160,172,65,30,88,162,226,250,129,231,33,165,177,18,8,64,205,83,208,249,120,112,95,32,
  75,16,65,111,58,112,33,136,201,36,122,7,250,106,25,65,103,26,34,74,169,76,48,101,
  159,103,109,180,63,77,209,91,121,1,21,68,67,116,122,166,253,147,224,210,145,237,218,173,
  111,68,44,182,45,157,20,52,208,53,185,57,166,74,189,25,95,141,51,3,103,94,107,167,
  83,218,157,193,142,248,194,212,223,36,97,65,197,122,167,174,4,192,215,33,153,56,169,7,
  124,79,165,74,0,166,116,26,128,26,242,241,158,91,170,172,237,80,129,180,163,251,180,216,
  16,5,222,183,181,236,99,247,73,113,87,186,114,114,234,138,33,72,169,238,204,178,168,231,
  71,23,32,236,39,41,156,34,59,110,24,71,114,233,42,56,74,31,166,118,153,94,72,134,
  56,107,143,62,76,82,144,67,251,97,82,29,143,28,216,26,157,79,28,11,253,76,73,117,
  49,152,136,48,90,90,131,84,99,3,40,50,129,103,134,200,149,148,48,129,132,166,115,164,
  106,5,13,226,172,253,14,204,4,61,141,163,241,84,29,71,230,194,199,74,8,148,143,184,
  70,46,228,228,88,81,84,63,86,66,218,42,191,141,179,209,130,9,61,174,80,164,249,53,
  238,194,104,163,32,207,14,136,142,120,79,142,82,179,211,82,127,124,125,95,182,162,140,102,
  91,226,16,202,136,42,170,218,32,95,57,249,161,212,124,14,29,219,200,87,198,255,163,106,
  126,236,8,69,140,104,177,141,149,130,238,40,250,49,39,75,84,76,192,208,55,111,124,238,
  69,154,160,200,217,25,245,34,205,79,199,73,6,48,191,220,227,77,253,126,228,59,5,242,
  91,78,166,243,247,176,30,226,134,254,136,38,97,174,157,215,81,26,242,43,253,237,219,229,
  139,155,77,11,87,172,223,28,218,202,219,19,93,220,156,210,175,115,250,196,5,67,43,125,
  147,72,189,255,39,98,84,102,31,114,199,150,163,125,99,219,33,123,106,126,6,88,157,9,
  102,224,218,249,156,5,227,246,60,157,10,170,39,141,40,29,45,5,0,7,37,247,22,93,
  101,133,117,27,245,58,35,51,210,141,222,51,243,115,38,162,118,36,93,132,235,85,200,118,
  194,221,28,167,109,221,186,26,156,110,10,101,43,162,22,88,32,74,81,40,211,145,76,105,
  41,73,225,221,130,229,215,178,100,34,44,190,143,124,107,94,245,167,93,60,5,55,154,136,
  217,92,47,216,57,25,219,99,183,30,26,41,231,104,136,136,214,196,168,159,213,193,25,166,
  17,171,128,7,83,58,140,67,145,165,109,67,162,149,170,72,26,211,103,237,194,92,211,248,
  203,98,208,81,24,156,5,88,134,154,76,231,99,55,185,238,10,213,6,145,27,116,76,41,
  159,27,249,153,126,38,200,42,111,234,59,77,78,68,152,132,211,252,149,38,1,201,68,6,
  76,207,252,114,195,219,133,138,239,253,244,198,59,186,19,9,161,136,200,222,31,189,86,49,
  31,37,148,32,190,41,96,217,236,120,146,1,185,47,169,178,87,105,8,30,16,144,249,143,
  120,52,14,240,54,141,15,30,4,9,129,221,96,117,212,7,212,2,199,159,173,62,128,121,
  218,100,16,196,71,50,14,174,17,4,52,59,219,47,8,167,140,160,146,3,200,199,223,199,
  136,158,92,241,145,144,6,128,198,73,223,25,141,207,5,38,158,63,231,105,121,59,65,116,
  54,24,159,193,169,44,132,103,215,236,188,30,235,73,10,41,135,3,142,172,183,55,241,15,
  228,215,205,212,31,14,123,112,243,9,230,221,109,56,23,181,17,42,95,17,9,104,105,178,
  68,143,110,82,197,72,5,97,254,197,215,191,62,156,90,132,197,47,131,52,26,168,219,60,
  208,203,14,177,110,16,145,239,114,61,126,17,69,80,242,1,95,81,101,211,48,148,40,70,
  225,197,180,178,60,173,254,65,81,160,23,26,136,197,190,100,242,63,144,146,197,248,154,178,
  135,28,8,122,161,65,142,72,15,7,58,56,164,152,96,148,15,6,180,120,70,159,39,68,
  144,117,172,70,149,103,148,160,107,21,67,242,146,164,58,198,241,53,36,163,156,12,41,8,
  100,61,186,46,206,215,250,100,125,210,155,84,37,150,118,133,16,212,196,92,38,112,185,10,
  105,229,57,0,230,51,177,148,73,133,234,145,179,27,10,50,116,25,174,43,14,14,24,101,
  130,16,174,129,232,110,153,152,146,78,242,58,156,123,168,78,30,116,132,117,226,84,30,158,
  184,85,55,215,188,81,212,161,37,240,74,151,118,56,164,247,226,116,41,241,13,197,157,205,
  182,43,31,226,77,164,144,155,208,111,154,146,130,223,19,123,39,50,75,250,183,218,241,37,
  192,247,116,60,30,77,35,104,13,50,239,175,215,27,161,196,103,193,133,27,242,21,202,138,
  112,118,18,102,95,78,207,249,65,198,164,19,220,164,18,131,116,218,49,231,44,226,229,61,
  22,152,90,170,175,144,178,194,245,93,90,23,48,213,75,23,38,166,239,165,102,77,202,184,
  243,87,110,212,110,38,80,159,239,233,185,171,199,235,176,248,109,119,94,3,151,147,224,220,
  123,250,34,43,69,236,52,192,176,90,215,194,142,69,43,237,245,160,119,29,78,0,149,87,
  252,242,30,143,255,0,182,218,27,128,247,125,139,63,233,225,253,157,15,239,36,4,65,152,
  76,183,99,24,50,83,242,139,9,14,84,249,247,129,14,200,106,214,183,28,45,189,30,84,
  88,17,182,90,235,84,214,209,77,164,158,32,108,141,37,156,170,173,105,119,186,160,201,13,
  2,128,176,162,37,7,240,82,151,187,239,38,128,32,37,192,88,236,149,33,232,48,252,243,
  214,86,173,178,164,178,42,89,137,40,214,80,187,182,79,201,19,112,253,231,244,72,11,105,
  241,29,176,47,4,165,192,168,223,3,115,66,82,202,143,6,52,138,205,31,39,233,75,163,
  214,171,170,88,242,78,173,79,21,15,14,128,153,239,160,17,187,249,108,44,138,161,248,215,
  188,166,64,240,60,217,132,42,221,164,91,92,135,171,32,48,234,38,137,74,159,145,206,169,
  207,229,73,186,218,21,77,122,140,215,146,171,224,170,48,161,28,157,193,209,34,90,130,15,
  150,105,171,115,55,238,89,26,177,84,206,96,170,30,247,79,129,122,105,137,100,48,95,142,
  71,40,70,8,76,145,68,212,196,168,183,61,40,143,217,212,97,11,255,116,33,87,91,231,
  114,16,7,77,92,158,3,194,226,224,148,223,4,32,52,171,248,170,181,26,73,139,44,73,
  186,148,154,100,5,147,182,216,188,60,90,178,19,50,187,75,239,13,114,194,216,78,4,136,
  53,96,17,216,140,177,200,240,43,149,76,151,196,53,238,12,178,171,102,149,185,235,45,25,
  49,101,160,222,145,22,180,24,16,145,29,42,254,187,92,245,47,233,250,38,188,61,70,27,
  217,121,242,73,83,73,153,5,3,51,225,163,77,123,99,127,164,55,177,223,211,161,184,23,
  170,171,139,163,229,102,229,95,245,245,177,253,26,209,167,231,11,127,92,77,16,23,36,164,
  233,31,227,71,48,41,215,23,251,11,96,25,199,160,23,129,45,162,0,42,120,162,181,215,
  43,215,151,250,149,133,42,98,101,98,25,254,124,124,30,102,25,212,60,48,230,92,155,118,
  49,232,217,87,87,186,88,126,52,255,98,240,56,108,226,13,38,1,120,137,95,18,241,190,
  130,79,87,225,104,172,20,142,184,152,168,29,173,171,50,207,159,231,227,150,99,230,255,34,
  81,42,20,151,169,104,197,30,50,187,38,97,56,129,70,207,8,160,252,123,92,69,33,167,
  76,120,21,37,9,144,13,23,27,209,176,168,25,210,235,130,77,117,151,235,21,150,63,160,
  91,42,161,5,73,43,213,186,85,90,137,203,187,162,33,144,174,216,218,218,160,217,38,149,
  200,239,154,173,86,155,131,60,159,21,22,78,45,28,191,235,175,14,6,157,134,83,231,101,
  98,246,237,30,239,160,201,240,89,42,149,240,115,49,179,17,78,251,128,180,93,172,192,217,
  110,13,75,68,224,231,72,209,104,233,237,96,179,81,171,49,124,100,85,52,52,190,76,7,
  4,124,152,246,179,110,13,20,76,140,29,185,187,238,255,154,88,61,253,156,56,179,154,216,
  101,3,186,179,162,135,104,105,79,13,164,76,188,36,221,102,144,9,17,167,160,68,93,90,
  53,27,74,55,235,20,121,122,125,246,181,118,66,217,15,21,252,249,143,229,133,133,255,128,
  195,237,255,143,180,82,226,36,164,223,187,16,229,76,136,206,44,129,247,175,45,145,158,18,
  252,176,64,111,22,81,25,74,176,19,26,142,39,9,149,59,62,220,251,101,233,181,212,91,
  58,24,224,234,142,96,169,61,233,150,222,28,156,32,127,249,63,254,127,40,42,120,143,179,
  183,9,0
};
const size_t  WEB3D_APP_GZ_LEN = 163779;

// ===================== (hết web3d.h) =====================

                        //   web3d.h do giao_dien_3d/build_web3d.py tự sinh – sửa giao diện ở thư mục đó

// ================================================================================
// PHẦN B – CÁC HÀM XỬ LÝ
// B1. LƯU / ĐỌC CÀI ĐẶT (bộ nhớ flash, vùng "parking2")
//   Tắt nguồn không mất: góc cổng, tốc độ servo, cài đặt cảm biến, nguồn xác định chỗ đỗ
// ================================================================================

// gkey(i, k): tạo tên khoá lưu flash cho cổng i, vd gkey(0, "oa") = "g0oa" (góc mở cổng vào)
String gkey(int i, const char* k)
{
  return String("g") + String(i) + k;   // vd: "g0open"
}

// isCalibrated(i): cổng i đã đo tốc độ chưa (servo 180 luôn = đã đo)
bool isCalibrated(int i)
{
  if (gates[i].mode == 180) return true;   // servo 180 khong can do toc do
  return gates[i].ms90R >= 50 && gates[i].ms90L >= 50;
}

// updateLearned(i): cổng "đã học" khi: đã đo tốc độ + có góc MỞ + góc ĐÓNG cách nhau >= 10°
void updateLearned(int i)
{
  Gate& g = gates[i];
  g.learned = isCalibrated(i) && g.openSet && g.closeSet && fabs(g.openAng - g.closeAng) >= 10;
}

// loadSettings(): đọc toàn bộ cài đặt 2 cổng + nguồn xác định chỗ đỗ từ flash (gọi trong setup)
void loadSettings()
{
  prefs.begin("parking2", true);
  slotSource = prefs.getInt("src", 2);
  if (slotSource < 0 || slotSource > 2) slotSource = 2;
  opMode = constrain(prefs.getInt("mode", 0), 0, 1);
  { int m = prefs.getInt("off", 0); for (int i = 0; i < 4; i++) slotOff[i] = (m >> i) & 1; }
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    g.ms90R      = prefs.getULong(gkey(i, "m90R").c_str(),  0);
    g.ms90L      = prefs.getULong(gkey(i, "m90L").c_str(),  0);
    g.speedUs    = prefs.getInt  (gkey(i, "spd").c_str(),   300);
    g.mode       = prefs.getInt  (gkey(i, "mode").c_str(),  180);
    if (g.mode != 360) g.mode = 180;
#if MO_HINH == 2
    // Mô hình 2: góc servo lấy từ 4 hằng số GATE_IN_ / GATE_OUT_ (giống baixe_MH2.ino)
    float defOpen  = (i == 0) ? GATE_IN_OPEN  : GATE_OUT_OPEN;
    float defClose = (i == 0) ? GATE_IN_CLOSE : GATE_OUT_CLOSE;
    g.openSet    = prefs.getBool (gkey(i, "os").c_str(),    true);
    g.closeSet   = prefs.getBool (gkey(i, "cs").c_str(),    true);
    g.openAng    = prefs.getFloat(gkey(i, "oa").c_str(),    defOpen);
    g.closeAng   = prefs.getFloat(gkey(i, "ca").c_str(),    defClose);
    if (GATE_USE_CODE_ANGLES && g.mode == 180)     // luôn dùng góc trong code
    {
      g.openAng = defOpen;  g.closeAng = defClose;
      g.openSet = g.closeSet = true;
    }
#else
    // Mô hình 1: góc đã học và lưu trên trang web
    g.openSet    = prefs.getBool (gkey(i, "os").c_str(),    false);
    g.closeSet   = prefs.getBool (gkey(i, "cs").c_str(),    false);
    g.openAng    = prefs.getFloat(gkey(i, "oa").c_str(),    90);
    g.closeAng   = prefs.getFloat(gkey(i, "ca").c_str(),    0);
#endif
    g.savedOpen  = prefs.getBool (gkey(i, "isOpen").c_str(), false);
    g.sensorEn   = prefs.getBool (gkey(i, "en").c_str(),    true);
    g.sensorInv  = prefs.getBool (gkey(i, "inv").c_str(),   false);
    g.confirmMs  = prefs.getULong(gkey(i, "cf").c_str(),    150);
    g.closeDelay = prefs.getULong(gkey(i, "cd").c_str(),    1500);
    g.zeroSet    = g.closeSet;
    updateLearned(i);
  }
  prefs.end();
}

// saveGate(i): ghi toàn bộ cài đặt cổng i vào flash
void saveGate(int i)
{
  Gate& g = gates[i];
  prefs.begin("parking2", false);
  prefs.putULong(gkey(i, "m90R").c_str(), g.ms90R);
  prefs.putULong(gkey(i, "m90L").c_str(), g.ms90L);
  prefs.putInt  (gkey(i, "spd").c_str(),  g.speedUs);
  prefs.putInt  (gkey(i, "mode").c_str(), g.mode);
  prefs.putBool (gkey(i, "os").c_str(),   g.openSet);
  prefs.putBool (gkey(i, "cs").c_str(),   g.closeSet);
  prefs.putFloat(gkey(i, "oa").c_str(),   g.openAng);
  prefs.putFloat(gkey(i, "ca").c_str(),   g.closeAng);
  prefs.putBool (gkey(i, "en").c_str(),   g.sensorEn);
  prefs.putBool (gkey(i, "inv").c_str(),  g.sensorInv);
  prefs.putULong(gkey(i, "cf").c_str(),   g.confirmMs);
  prefs.putULong(gkey(i, "cd").c_str(),   g.closeDelay);
  prefs.end();
}

// loadHcSwap / saveHcSwap: đọc / ghi ô nào đã tự đảo TRIG-ECHO (mỗi bit = 1 ô)
void loadHcSwap()
{
  prefs.begin("parking2", true);
  uint8_t m = prefs.getUChar("hcSwap", 0);
  prefs.end();
  for (int i = 0; i < 4; i++) hcSwap[i] = (m >> i) & 1;
}

// saveHcSwap(): ghi vào flash các ô đã tự đảo TRIG-ECHO (mỗi bit là 1 ô) để lần khởi động sau dùng luôn
void saveHcSwap()
{
  uint8_t m = 0;
  for (int i = 0; i < 4; i++) if (hcSwap[i]) m |= (1 << i);
  prefs.begin("parking2", false);
  if (prefs.getUChar("hcSwap", 0) != m) prefs.putUChar("hcSwap", m);
  prefs.end();
}

// saveGateOpenState(i, mở?): nhớ cổng đang MỞ hay ĐÓNG – mất điện lúc mở thì khởi động lại sẽ tự đóng
//   (chỉ ghi khi trạng thái đổi để đỡ hao flash)
void saveGateOpenState(int i, bool isOpen)
{
  Gate& g = gates[i];
  if (g.savedOpen == isOpen) return;
  g.savedOpen = isOpen;
  prefs.begin("parking2", false);
  prefs.putBool(gkey(i, "isOpen").c_str(), isOpen);
  prefs.end();
}

// ================================================================================
// B2. HC-SR04: ĐO KHOẢNG CÁCH – PHÂN LOẠI – TỰ NHẬN TRIG/ECHO CẮM NGƯỢC
// ================================================================================

// [CẢM BIẾN]
// --------------------------------------------------------------------------------
// readDistanceCM(idx): ĐO KHOẢNG CÁCH ô idx (cm)
//   Bước 1: nếu ECHO còn kẹt cao thì chờ tối đa 40 ms; kẹt 5 lần liền → lỗi 2
//   Bước 2: phát xung TRIG 10 µs
//   Bước 3: đo độ dài xung ECHO (pulseIn, tối đa 30 ms ≈ 5 m) → cm = µs × 0,0343 / 2
//   Trả về: cm, hoặc 999 khi lỗi. Mã lỗi lưu ở sensorErr[idx]:
//           0 OK | 1 không có vang | 2 ECHO kẹt cao | 3 quá gần < 2 cm
//           (mã 4 = mất vang liên tục quá 3 s không lưu ở đây, do hcErrCode() tính)
// --------------------------------------------------------------------------------
float readDistanceCM(int idx)
{
  uint8_t trigPin = trigOf(idx);
  uint8_t echoPin = echoOf(idx);

  // Mot so HC-SR04 (ban clone) giu ECHO o muc cao ~38 ms khi khong co vang,
  // co con con ket luon. Cho toi da 40 ms de ECHO ve LOW.
  if (digitalRead(echoPin) == HIGH)
  {
    unsigned long t0 = millis();
    while (digitalRead(echoPin) == HIGH && millis() - t0 < 40) { delayMicroseconds(200); }

    if (digitalRead(echoPin) == HIGH)
    {
      // Van ket: phat TRIG de "danh thuc" cam bien, chi bao loi khi ket 5 lan lien tiep
      digitalWrite(trigPin, HIGH);
      delayMicroseconds(10);
      digitalWrite(trigPin, LOW);

      if (echoStuckCount[idx] < 255) echoStuckCount[idx]++;
      sensorErr[idx] = (echoStuckCount[idx] >= 5) ? 2 : 1;
      sensorRawUs[idx] = 0;
      return 999.0;
    }
  }
  echoStuckCount[idx] = 0;

  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, 30000);   // toi da ~5 m
  sensorRawUs[idx] = duration;

  if (duration == 0) { sensorErr[idx] = 1; return 999.0; }

  float cm = duration * 0.0343 / 2.0;
  if (cm < 2.0)      { sensorErr[idx] = 3; return 999.0; }

  sensorErr[idx] = 0;
  return cm;
}

// [PHÂN LOẠI] (ngưỡng cứng – phần "giống fuzzy", xem sơ đồ ở đầu file)
// getParkingState(d): phân loại khoảng cách → EMPTY / CLOSE / GOOD / WARNING (theo mục A3)
const char* getParkingState(float d)
{
  if (d > PARKING_LIMIT) return "EMPTY";
  if (d < TOO_CLOSE)     return "CLOSE";
  if (d >= GOOD_MIN && d <= GOOD_MAX) return "GOOD";
  return "WARNING";
}

// hcErrCode(i): mã lỗi để hiển thị và gửi đi. Giống sensorErr, thêm 4 = mất tiếng vang liên tục quá 3 s
uint8_t hcErrCode(int i)
{
  if (sensorErr[i] == 1 && noEchoSince[i] && millis() - noEchoSince[i] >= NO_ECHO_FAULT_MS) return 4;
  return sensorErr[i];
}

// hcBroken(i): HC-SR04 ô i đang hỏng: ECHO kẹt mức cao (mã 2) hoặc mất tiếng vang quá 3 s (mã 4).
// Ô có cảm biến hỏng không bao giờ được tính là chỗ trống (xem updateFusion).
bool hcBroken(int i)
{
  uint8_t e = hcErrCode(i);
  return e == 2 || e == 4;
}

// [CẢM BIẾN]
// --------------------------------------------------------------------------------
// updateOneParkingSensor(): đo 1 ô mỗi lần gọi (lần lượt ô 1→4), cập nhật hcOccupied
//   + TỰ NHẬN TRIG/ECHO cắm ngược: chưa đo được lần nào mà lỗi 6 lần liền → đảo vai trò 2 chân;
//     đo được 1 lần → khoá lại và lưu flash
// --------------------------------------------------------------------------------
void updateOneParkingSensor()
{
  int i = currentSensor;
  parkingDistance[i] = readDistanceCM(i);
  hcOccupied[i] = parkingDistance[i] <= PARKING_LIMIT;

  bool gotPulse = (sensorErr[i] == 0 || sensorErr[i] == 3);
  // Đếm thời gian mất tiếng vang liên tục (| 1 để mốc thời gian không bao giờ bằng 0)
  if (sensorErr[i] == 1) { if (!noEchoSince[i]) noEchoSince[i] = millis() | 1; }
  else noEchoSince[i] = 0;
  if (gotPulse)
  {
    hcFail[i] = 0;
    if (!hcLocked[i])
    {
      hcLocked[i] = true;
      saveHcSwap();
      Serial.printf(">>> O %d: nhan dung day TRIG=GPIO%d ECHO=GPIO%d\n", i + 1, trigOf(i), echoOf(i));
    }
  }
  else if (!hcLocked[i] && sensorErr[i] != 3)
  {
    if (++hcFail[i] >= 6)
    {
      hcFail[i] = 0;
      hcSwap[i] = !hcSwap[i];
      echoStuckCount[i] = 0;
      sensorErr[i] = 1;
      applyHcPins(i);
      Serial.printf(">>> O %d: thu dao TRIG/ECHO -> TRIG=GPIO%d ECHO=GPIO%d\n", i + 1, trigOf(i), echoOf(i));
    }
  }

  currentSensor = (currentSensor + 1) % 4;
}

// ================================================================================
// B3. CAMERA AI: NHẬN LỆNH UART TỪ PC + BÁO PC + KẾT HỢP AI VÀ HC-SR04 (xử lý tại biên)
// ================================================================================

// [XỬ LÝ AI]
// aiOnline(): camera AI còn kết nối không (có dữ liệu trong 3 s gần nhất)
bool aiOnline()
{
  return aiLastMs != 0 && millis() - aiLastMs < AI_TIMEOUT;
}

// [XỬ LÝ AI] (kênh truyền ESP32 → PC)
// sendToPC(dòng): gửi 1 dòng lên máy tính qua cổng USB (Serial) và cả UART0 (Serial0) nếu đang dùng
void sendToPC(const String& line)
{
  Serial.println(line);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.println(line);
#endif
}

// [XỬ LÝ AI] giải mã kết quả camera "AI,..." → aiSlot[]  (+ lệnh PC: GATE, SRC)
// --------------------------------------------------------------------------------
// handlePCLine(line, port): XỬ LÝ 1 DÒNG LỆNH TỪ PC
//   "AI,1,0,1,0"    → kết quả camera 4 ô       "PING"  → trả "PONG,ESP32S3,BAI_XE"
//   "GATE,g,lệnh"   → điều khiển cổng           "SRC,n" → đổi nguồn xác định chỗ đỗ
// --------------------------------------------------------------------------------
void handlePCLine(String line, Stream& port)
{
  line.trim();
  if (line.length() == 0) return;

  // "AI,1,0,1,0"
  if (line.startsWith("AI,"))
  {
    int v[4] = {0, 0, 0, 0};
    int idx = 0, start = 3;
    for (int k = 3; k <= (int)line.length() && idx < 4; k++)
    {
      if (k == (int)line.length() || line[k] == ',')
      {
        v[idx++] = line.substring(start, k).toInt();
        start = k + 1;
      }
    }
    if (idx == 4)
    {
      for (int i = 0; i < 4; i++) aiSlot[i] = v[i] != 0;
      if (!aiOnline()) Serial.println(">>> CAMERA AI: DA KET NOI");
      aiLastMs = millis();
      aiMsgCount++;
    }
  }
  else if (line.startsWith("ML,"))
  {
    String f[8]; int idx = 0, start = 3; bool ok = true;
    for (int k = 3; k <= (int)line.length(); k++)
      if (k == (int)line.length() || line[k] == ',') { if (idx < 8) f[idx] = line.substring(start, k); idx++; start = k + 1; }
    if (idx != 8) ok = false;
    int lab[4]; float cf[4];
    for (int k = 0; ok && k < 8; k++)            // moi truong phai la SO (toInt/toFloat bien chu thanh 0 => nhan nham)
    {
      if (f[k].length() == 0) { ok = false; break; }
      for (int c = 0; c < (int)f[k].length(); c++)
        if (!isdigit(f[k][c]) && f[k][c] != '.') { ok = false; break; }
    }
    for (int i = 0; ok && i < 4; i++)
    {
      lab[i] = f[i].toInt(); cf[i] = f[4 + i].toFloat();
      if ((lab[i] != 0 && lab[i] != 1) || cf[i] < 0 || cf[i] > 1) ok = false;
    }
    if (!ok) { port.println("ERR,ML,FORMAT"); return; }
    for (int i = 0; i < 4; i++) { mlSlot[i] = lab[i]; mlConf[i] = cf[i]; }
    if (!mlFresh()) Serial.println(">>> ML: DA KET NOI");
    mlLastMs = millis();
    mlMsgCount++;
  }
  else if (line == "PING")
  {
    port.println("PONG,ESP32S3,BAI_XE");
  }
  // Dieu khien cong tu PC:  GATE,0,OPEN | GATE,1,CLOSE | GATE,0,AUTO
  else if (line.startsWith("GATE,") && line.length() >= 8)
  {
    int g = line.substring(5, 6).toInt();
    String c = line.substring(7);
    if (g < 0 || g > 1) { port.println("ERR,GATE,SAI_CONG"); return; }
    Gate& gt = gates[g];
    if (c == "AUTO")
    {
      gt.manual = false;
      gt.detPrev = false;
      if (gt.learned) gateClose(g);
    }
    else if (c == "OPEN" || c == "CLOSE")
    {
      if (!gt.learned) { port.printf("ERR,GATE,%d,CHUA_HOC\n", g); return; }
      if (c == "OPEN" && entryBlocked(g)) { port.println("ERR,GATE,0,BAI_DAY"); return; }   // bãi đầy: không mở cổng vào
      gt.manual = true;
      if (c == "OPEN") gateOpen(g); else gateClose(g);
    }
    else { port.println("ERR,GATE,SAI_LENH"); return; }
    port.printf("OK,GATE,%d,%s\n", g, c.c_str());
    Serial.printf("[PC] %s: %s\n", gt.name, c.c_str());
  }
  // Doi nguon xac dinh cho do:  SRC,0 (HC) | SRC,1 (AI) | SRC,2 (ket hop)
  else if (line.startsWith("SRC,"))
  {
    slotSource = constrain(line.substring(4).toInt(), 0, 2);
    prefs.begin("parking2", false);
    prefs.putInt("src", slotSource);
    prefs.end();
    port.printf("OK,SRC,%d\n", slotSource);
  }
  // Doi che do:  MODE,FUZZY | MODE,MODEL | MODE,?
  // Kết quả đọc biển số từ máy tính: PLATE,<biển>,<1 đã đăng ký | 0 xe lạ>,<tên chủ xe không dấu>
  // LCD hiện 5 s: dòng 1 "BIEN 51F-123.45", dòng 2 tên chủ xe (hoặc DA DANG KY) hay "XE LA CHUA DK"
  else if (line.startsWith("PLATE,"))
  {
    int c1 = line.indexOf(',', 6);
    int c2 = (c1 > 0) ? line.indexOf(',', c1 + 1) : -1;
    if (c1 < 0) { port.println("ERR,PLATE,SAI_DINH_DANG"); return; }
    String bien = line.substring(6, c1); bien.trim();
    String dk   = (c2 > 0) ? line.substring(c1 + 1, c2) : line.substring(c1 + 1); dk.trim();
    String ten  = (c2 > 0) ? line.substring(c2 + 1) : ""; ten.trim();
    if (bien.length() == 0 || (dk != "0" && dk != "1")) { port.println("ERR,PLATE,SAI_DINH_DANG"); return; }
    bool daDK = (dk == "1");
    plateL1 = ("BIEN " + bien).substring(0, 16);
    plateL2 = daDK ? (ten.length() ? ten.substring(0, 16) : String("DA DANG KY")) : String("XE LA CHUA DK");
    plateUntil = millis() + 5000;
    Serial.printf(">>> BIEN SO: %s - %s\n", bien.c_str(), daDK ? "DA DANG KY" : "XE LA");
    port.printf("OK,PLATE,%s,%d\n", bien.c_str(), daDK ? 1 : 0);
  }
  else if (line.startsWith("MODE,"))
  {
    String v = line.substring(5); v.trim(); v.toUpperCase();
    if (v == "?") { port.printf("OK,MODE,%s\n", opMode ? "MODEL" : "FUZZY"); return; }
    if (v != "FUZZY" && v != "MODEL") { port.println("ERR,MODE"); return; }
    opMode = (v == "MODEL") ? 1 : 0;
    prefs.begin("parking2", false); prefs.putInt("mode", opMode); prefs.end();
    port.printf("OK,MODE,%s\n", opMode ? "MODEL" : "FUZZY");
    Serial.printf(">>> CHE DO: %s\n", opMode ? "MODEL (ML, den xanh/do)" : "FUZZY (nhu binh thuong)");
  }
  // Doi ten WiFi:  WIFI,<ten 1-31 ky tu> | WIFI,DEFAULT | WIFI,?   (ESP32 tu khoi dong lai de ap dung)
  else if (line.startsWith("WIFI,"))
  {
    String v = line.substring(5); v.trim();
    if (v == "?") { port.printf("OK,WIFI,%s\n", wifiSsidNow.c_str()); return; }
    if (v == "DEFAULT") v = "";
    else if (v.length() < 1 || v.length() > 31) { port.println("ERR,WIFI,DO_DAI_1_31"); return; }
    prefs.begin("parking2", false);
    if (v.length()) prefs.putString("ssid", v); else prefs.remove("ssid");
    prefs.end();
    port.printf("OK,WIFI,%s (khoi dong lai de ap dung)\n", v.length() ? v.c_str() : WIFI_SSID);
    delay(300);
    ESP.restart();
  }
  // Tat / bat tung o:  SLOT,1,OFF | SLOT,1,ON | SLOT,?
  else if (line.startsWith("SLOT,"))
  {
    if (line.charAt(5) == '?')
    {
      port.printf("OK,SLOT,%d%d%d%d\n", slotOff[0], slotOff[1], slotOff[2], slotOff[3]);
      return;
    }
    int n = line.substring(5, 6).toInt();
    String c = line.substring(7);
    if (n < 1 || n > 4 || (c != "OFF" && c != "ON")) { port.println("ERR,SLOT"); return; }
    slotOff[n - 1] = (c == "OFF");
    int m = 0; for (int i = 0; i < 4; i++) if (slotOff[i]) m |= (1 << i);
    prefs.begin("parking2", false);
    prefs.putInt("off", m);
    prefs.end();
    port.printf("OK,SLOT,%d,%s\n", n, c.c_str());
  }
}

// [XỬ LÝ AI] (gom dòng UART)
// pollPort(port, buf): gom từng ký tự thành 1 dòng; gặp '\n' thì gọi handlePCLine
void pollPort(Stream& port, String& buf)
{
  while (port.available())
  {
    char c = port.read();
    if (c == '\n') { handlePCLine(buf, port); buf = ""; }
    else if (c != '\r' && buf.length() < 80) buf += c;
  }
}

// Gui trang thai gon cho PC moi 1 giay: ST,{json}
unsigned long lastStatusToPC = 0;

// [XỬ LÝ AI] (báo trạng thái về PC)
// sendStatusToPC(): mỗi 1 s gửi PC dòng "ST,{json}" – chỗ trống, 4 ô, 2 cổng, LCD, thời gian chạy
void sendStatusToPC()
{
  if (millis() - lastStatusToPC < 1000) return;
  lastStatusToPC = millis();

  String j = "ST,{\"free\":" + String(freeSlots) + ",\"src\":" + String(slotSource);
  j += ",\"ai\":" + String(aiOnline() ? 1 : 0);
  j += ",\"ml\":" + String(mlFresh() ? 1 : 0);
  j += ",\"mode\":" + String(opMode);
  j += ",\"s\":[";
  for (int i = 0; i < 4; i++)
  {
    j += "{\"d\":" + String(parkingDistance[i] >= 900 ? -1.0f : parkingDistance[i], 1);
    j += ",\"f\":\"" + String(finalState[i]) + "\"";
    j += ",\"o\":" + String(parkingOccupied[i] ? 1 : 0);
    j += ",\"e\":" + String(hcErrCode(i));
    j += ",\"w\":\"" + String(whySrc[i]) + "\"";       // ly do quyet dinh: ML | CONFLICT | LOWCONF | FALLBACK | NOSRC
    j += ",\"mc\":" + String(mlConf[i], 2) + "}";
    if (i < 3) j += ",";
  }
  j += "],\"g\":[";
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    j += "{\"det\":" + String(g.stable ? 1 : 0);
    j += ",\"open\":" + String(gateIsOpen(i) ? 1 : 0);
    j += ",\"mv\":" + String(g.motion);
    j += ",\"ln\":" + String(g.learned ? 1 : 0);
    j += ",\"man\":" + String(g.manual ? 1 : 0);
    j += ",\"en\":" + String(g.sensorEn ? 1 : 0) + "}";
    if (i < 1) j += ",";
  }
  j += "],\"lcd\":" + String(lcdOK ? 1 : 0);
  j += ",\"up\":" + String(millis() / 1000) + "}";
  sendToPC(j);
}

// [XỬ LÝ AI] điểm vào – gọi trong loop()
// handlePCSerial(): đọc lệnh từ USB (Serial) và UART0 (Serial0, khi bật USB CDC)
void handlePCSerial()
{
  pollPort(Serial, serialBuf0);
#if ARDUINO_USB_CDC_ON_BOOT
  pollPort(Serial0, serialBuf1);
#endif
}

// [KẾT HỢP] (ghi sự kiện sau khi đã quyết định)
// addEvent(ô, vào?, nguồn): ghi nhật ký xe VÀO / RA (giữ 10 sự kiện mới nhất),
//   báo PC "EV,IN,n" / "EV,OUT,n", hiện thông báo trên LCD 3 s.  nguồn: 0 HC | 1 AI | 2 cả hai
void addEvent(int slot, bool in, uint8_t by)
{
  for (int k = 9; k > 0; k--) events[k] = events[k - 1];
  events[0] = { millis(), (uint8_t)slot, in, by };
  if (eventCount < 10) eventCount++;

  const char* src = by == 2 ? "AI+HC" : (by == 1 ? "AI" : "HC");
  Serial.printf(">>> XE %s O %d (%s)\n", in ? "VAO" : "RA KHOI", slot + 1, src);
  sendToPC(String("EV,") + (in ? "IN," : "OUT,") + String(slot + 1));

  lcdMsg = in ? ("XE VAO O " + String(slot + 1)) : ("XE RA O " + String(slot + 1));
  lcdMsgUntil = millis() + 3000;
}

// [KẾT HỢP] ★ BẢNG LUẬT NẾU–THÌ (hc = HC-SR04 thấy xe, ai = camera thấy xe)
//   ┌────────────┬─────┬─────┬──────────┬───────────────────────────┐
//   │ slotSource │ hc  │ ai  │ có xe?   │ finalState (màu đèn)      │
//   ├────────────┼─────┼─────┼──────────┼───────────────────────────┤
//   │ 0 hoặc     │  0  │  -  │ KHÔNG    │ EMPTY                     │
//   │ mất AI     │  1  │  -  │ CÓ       │ theo cm (GOOD/WARN/CLOSE) │
//   ├────────────┼─────┼─────┼──────────┼───────────────────────────┤
//   │ 1 chỉ AI   │  -  │  0  │ KHÔNG    │ EMPTY                     │
//   │            │  0  │  1  │ CÓ       │ AI                        │
//   │            │  1  │  1  │ CÓ       │ theo cm                   │
//   ├────────────┼─────┼─────┼──────────┼───────────────────────────┤
//   │ 2 kết hợp  │  0  │  0  │ KHÔNG    │ EMPTY                     │
//   │ (mặc định) │  0  │  1  │ CÓ       │ AI                        │
//   │            │  1  │  x  │ CÓ       │ theo cm                   │
//   └────────────┴─────┴─────┴──────────┴───────────────────────────┘
// Bảng trên dùng cho chế độ FUZZY. Ở chế độ MODEL (opMode = 1): mô hình ML quyết định khi gói ML còn mới,
// độ tin cậy từ 0,80 trở lên và cảm biến không lỗi; ngược lại tạm dùng HC-SR04 (FALLBACK).
// Ô bị tắt hoặc cảm biến lỗi luôn coi là bị chiếm, không bao giờ tính là chỗ trống. ML không bao giờ trực tiếp mở cổng.
// Ở chế độ FUZZY, HC-SR04 hỏng (mã 2 hoặc 4) cũng tính là bị chiếm, trừ khi SRC 1 và camera đang chạy.
// --------------------------------------------------------------------------------
// updateFusion(): KẾT HỢP HC-SR04 + CAMERA AI → trạng thái cuối cùng của từng ô
//   slotSource 0: chỉ HC-SR04 | 1: chỉ camera AI | 2: kết hợp (1 trong 2 thấy xe là có xe)
//   Mất camera AI → tự dùng HC-SR04.  AI ≠ HC-SR04 liên tục > 2 s → bật cảnh báo mismatch
//   Chống nhiễu: trạng thái phải giữ 1 s mới đổi và ghi sự kiện
// --------------------------------------------------------------------------------
void updateFusion()
{
  unsigned long now = millis();
  bool on = aiOnline();
  int freeCount = 0;

  for (int i = 0; i < 4; i++)
  {
    const char* hs = getParkingState(parkingDistance[i]);
    bool hcValid = !hcBroken(i);                     // mã lỗi 2 hoặc 4: cảm biến hỏng
    bool hc = hcOccupied[i] && hcValid;
    bool ai = on && aiSlot[i];
    bool occ; const char* st;

    if (opMode == 0)
    {
      // ===== CHE DO FUZZY: giu nhu binh thuong (HC-SR04, tuy chon camera AI); ML khong tham gia =====
      if (slotSource == 0 || !on)      { occ = hc;       st = hs; }
      else if (slotSource == 1)        { occ = ai;       st = ai ? (hc ? hs : "AI") : "EMPTY"; }
      else                             { occ = hc || ai; st = hc ? hs : (ai ? "AI" : "EMPTY"); }
      whySrc[i] = "HC";
      if (slotOff[i]) { occ = true; st = "ERROR"; whySrc[i] = "OFF"; }
      // Cảm biến hỏng (mã 2 ECHO kẹt mức cao, mã 4 mất vang quá 3 s) trong khi nguồn quyết định có dùng HC-SR04
      // (SRC 0, SRC 2 hoặc mất camera): không biết ô có xe hay không nên coi là bị chiếm,
      // không bao giờ tính là chỗ trống. SRC 1 và camera đang chạy thì camera quyết định như cũ.
      else if (!hcValid && (slotSource != 1 || !on)) { occ = true; st = "ERROR"; whySrc[i] = "NOSRC"; }
    }
    else
    {
      // ===== CHE DO MODEL: mo hinh ML quyet dinh, CHI 2 ket qua EMPTY / OCC =====
      bool mlUse = mlFresh() && hcValid && mlConf[i] >= CONF_THRESHOLD;
      if (slotOff[i])        { occ = true;       whySrc[i] = "OFF"; }       // o tat (cam bien hong): coi la bi chiem, khong tinh cho trong
      else if (!hcValid)     { occ = true;       whySrc[i] = "NOSRC"; }     // cam bien loi: coi la bi chiem
      else if (mlUse)
      {
        occ = mlSlot[i];                                                    // KET QUA CUA MO HINH
        whySrc[i] = (mlSlot[i] != hc) ? "CONFLICT" : "ML";                  // CONFLICT: mo hinh khac luat HC-SR04 (mo hinh van quyet dinh)
        static unsigned long lastCf[4] = {0, 0, 0, 0};
        if (mlSlot[i] != hc && now - lastCf[i] > 2000) { lastCf[i] = now; Serial.printf(">>> O %d: ML=%d(%.2f) khac HC-SR04=%d -> theo ML\n", i + 1, mlSlot[i], mlConf[i], hc); }
      }
      else                   { occ = hc;         whySrc[i] = "FALLBACK"; }  // ML chua co / het han / do tin cay thap: tam dung HC-SR04 (van chi 2 ket qua)
      st = occ ? "OCC" : "EMPTY";
    }
    finalState[i] = st;

    // AI va HC-SR04 bao khac nhau lien tuc > 2 s -> canh bao
    if (on && hc != aiSlot[i])
    {
      if (!mismatchSince[i]) mismatchSince[i] = now;
      mismatchFlag[i] = now - mismatchSince[i] > 2000;
    }
    else { mismatchSince[i] = 0; mismatchFlag[i] = false; }

    // Chong nhieu: trang thai phai giu 1 s moi ghi su kien
    if (occ != candOcc[i]) { candOcc[i] = occ; candSince[i] = now; }
    if (candOcc[i] != parkingOccupied[i] && now - candSince[i] >= 1000)
    {
      parkingOccupied[i] = candOcc[i];
      if (now > 5000) addEvent(i, parkingOccupied[i], (hc && ai) ? 2 : (ai ? 1 : 0));
    }
    if (!parkingOccupied[i]) freeCount++;
  }
  freeSlots = freeCount;
}

// ================================================================================
// B4. ĐÈN WS2812B – tô màu theo trạng thái ô
//   xanh lá = trống | xanh dương = đỗ tốt / AI thấy xe | vàng = cần tiến thêm | đỏ = quá gần
//   tím nhấp nháy = ECHO kẹt mức cao (lỗi dây / chia áp)
// ================================================================================

// [ĐIỀU KHIỂN LOGIC] (đầu ra: đèn)
// setSlotColor(i, màu): đổi màu đèn ô i (chỉ gửi khi màu khác lần trước)
void setSlotColor(int i, uint32_t color)
{
  if (lastLedColor[i] == color) return;
  lastLedColor[i] = color;
  slotPixel[i].setPixelColor(0, color);
  slotPixel[i].show();
}

// [ĐIỀU KHIỂN LOGIC] trạng thái → màu đèn
// updateSlotLEDs(): mỗi 50 ms tô màu 4 đèn theo finalState
void updateSlotLEDs()
{
  if (millis() - previousLEDMillis < 50) return;
  previousLEDMillis = millis();

  bool blink = (millis() / 500) % 2;

  for (int i = 0; i < 4; i++)
  {
    if (opMode == 1)                                   // che do MODEL: CHI 2 mau - xanh la = trong, do = bi chiem
    {
      setSlotColor(i, strcmp(finalState[i], "EMPTY") == 0 ? Adafruit_NeoPixel::Color(0, 160, 0) : Adafruit_NeoPixel::Color(255, 0, 0));
      continue;
    }
    if (hcBroken(i)) { setSlotColor(i, blink ? Adafruit_NeoPixel::Color(150, 0, 255) : 0); continue; }   // tím nhấp nháy = cảm biến hỏng

    const char* s = finalState[i];
    if      (strcmp(s, "EMPTY") == 0) setSlotColor(i, Adafruit_NeoPixel::Color(0, 160, 0));
    else if (strcmp(s, "AI")    == 0) setSlotColor(i, Adafruit_NeoPixel::Color(0, 0, 255));
    else if (strcmp(s, "GOOD")  == 0) setSlotColor(i, Adafruit_NeoPixel::Color(0, 0, 255));
    else if (strcmp(s, "CLOSE") == 0) setSlotColor(i, Adafruit_NeoPixel::Color(255, 0, 0));
    else                              setSlotColor(i, Adafruit_NeoPixel::Color(255, 150, 0));
  }
}

// startupLEDTest(): lúc khởi động bật lần lượt từng đèn xanh dương để kiểm tra thứ tự
void startupLEDTest()
{
  for (int i = 0; i < 4; i++)
  {
    for (int k = 0; k < 4; k++) setSlotColor(k, 0);
    setSlotColor(i, Adafruit_NeoPixel::Color(0, 0, 255));
    Serial.printf("    Test den o %d (GPIO%d)\n", i + 1, ledPins[i]);
    delay(500);
  }
  for (int k = 0; k < 4; k++) setSlotColor(k, 0);
}

// ================================================================================
// B5. CẢM BIẾN CỔNG E18-D80NK – lọc nhiễu theo thời gian xác nhận
// ================================================================================

// [CẢM BIẾN]
// updateIRSensors(): đọc 2 cảm biến E18. Tín hiệu phải giữ ổn định confirmMs mới được tính (g.stable)
void updateIRSensors()
{
  unsigned long now = millis();
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    bool raw = (digitalRead(g.irPin) == (IR_ACTIVE_LOW ? LOW : HIGH));
    if (g.sensorInv) raw = !raw;
    if (!g.sensorEn) raw = false;

    if (raw != g.rawPrev) { g.rawPrev = raw; g.changeMs = now; }
    if (now - g.changeMs >= g.confirmMs) g.stable = raw;
  }
}

// ================================================================================
// B6. SERVO: CHUYỂN ĐỘNG CỔNG (mở / đóng / dừng, quay mượt tới góc đích)
// ================================================================================

// [ĐIỀU KHIỂN LOGIC] (cơ cấu chấp hành: servo)
// gateStopAll(i): dừng cổng i ngay (servo 180 giữ góc hiện tại, servo 360 ngắt xung)
void gateStopAll(int i)
{
  Gate& g = gates[i];
  if (g.mode == 180)
  {
    // Servo 180: dung tai cho va GIU goc
    g.motion = 0;
    g.target = g.ang;
    if (g.outputOn) g.sv.writeAngle(g.ang);
    return;
  }
  g.sv.stop();
  g.motion = 0;
  g.calDir = 0;
  g.nudgeUntil = 0;
  g.target = g.ang;
}

// [ĐIỀU KHIỂN LOGIC] (cơ cấu chấp hành: servo)
// gateIsOpen(i): góc hiện tại gần góc MỞ hơn góc ĐÓNG → đang mở
bool gateIsOpen(int i)
{
  Gate& g = gates[i];
  return fabs(g.ang - g.openAng) < fabs(g.ang - g.closeAng);
}

// [ĐIỀU KHIỂN LOGIC] (cơ cấu chấp hành: servo)
// gateOpen / gateClose(i): đặt góc đích = góc MỞ / ĐÓNG (updateGateMotion sẽ quay tới)
void gateOpen(int i)
{
  Gate& g = gates[i];
  if (!g.learned) return;
  g.outputOn = true;
  g.target = g.openAng;
  Serial.printf(">>> MO %s\n", g.name);
}

// [ĐIỀU KHIỂN LOGIC] (cơ cấu chấp hành: servo)
void gateClose(int i)
{
  Gate& g = gates[i];
  if (!g.learned) return;
  g.outputOn = true;
  g.target = g.closeAng;
  Serial.printf(">>> DONG %s\n", g.name);
}

// [ĐIỀU KHIỂN LOGIC] (cơ cấu chấp hành: servo)
// --------------------------------------------------------------------------------
// updateGateMotion(i): QUAY CỔNG i TỚI GÓC ĐÍCH (gọi liên tục trong loop)
//   Servo 180: mỗi 15 ms tiến 3° cho mượt
//   Servo 360: quay liên tục, ước tính góc = tốc độ (90° / ms90) × thời gian
// --------------------------------------------------------------------------------
void updateGateMotion(int i)
{
  Gate& g = gates[i];
  unsigned long now = millis();
  unsigned long dt = now - g.lastMotionMs;
  g.lastMotionMs = now;

  // ===== SERVO 180: quay muot toi goc, 3 do moi 15 ms =====
  if (g.mode == 180)
  {
    if (!g.outputOn) return;           // chua hoc / chua bam -> khong xuat xung
    float diff = g.target - g.ang;
    if (fabs(diff) < 0.5)
    {
      if (g.motion)
      {
        g.motion = 0;
        g.ang = g.target;
        g.sv.writeAngle(g.ang);
        if (g.learned) saveGateOpenState(i, gateIsOpen(i));
      }
      return;
    }
    if (now - g.lastStepMs < 15) return;
    g.lastStepMs = now;
    int dir = diff > 0 ? 1 : -1;
    g.motion = dir;
    if (fabs(diff) <= 3.0f) g.ang = g.target;
    else g.ang += dir * 3.0f;
    g.sv.writeAngle(g.ang);
    return;
  }

  // Dang do 90 do: servo quay tu do, qua 20 s thi tu dung
  if (g.calDir)
  {
    if (now - g.calStart > CAL_TIMEOUT)
    {
      Serial.printf(">>> %s: do qua lau -> dung\n", g.name);
      gateStopAll(i);
    }
    return;
  }

  // Nhich theo thoi gian (chua do toc do)
  if (g.nudgeUntil)
  {
    if ((long)(now - g.nudgeUntil) >= 0) { g.nudgeUntil = 0; g.sv.stop(); }
    return;
  }

  // Chua do toc do -> khong the quay theo goc
  if (!isCalibrated(i))
  {
    if (g.motion) { g.motion = 0; g.sv.stop(); }
    return;
  }

  float diff = g.target - g.ang;
  if (fabs(diff) < 0.5)
  {
    if (g.motion)
    {
      g.motion = 0;
      g.sv.stop();
      g.ang = g.target;
      if (g.learned) saveGateOpenState(i, gateIsOpen(i));
    }
    return;
  }

  int dir = (diff > 0) ? 1 : -1;
  if (dir != g.motion)
  {
    g.motion = dir;
    g.sv.run(dir, g.speedUs);
    dt = 0;
  }

  float rate = 90.0f / (float)(dir > 0 ? g.ms90R : g.ms90L);   // do / ms
  g.ang += dir * rate * dt;

  if ((dir > 0 && g.ang >= g.target) || (dir < 0 && g.ang <= g.target))
  {
    g.ang = g.target;
    g.motion = 0;
    g.sv.stop();
    if (g.learned) saveGateOpenState(i, gateIsOpen(i));
  }
}

// ================================================================================
// B7. LOGIC TỰ ĐỘNG MỞ / ĐÓNG CỔNG
//   E18 thấy xe → mở (cổng vào: bãi đầy thì KHÔNG mở) | xe qua + chờ closeDelay → đóng
// ================================================================================

// entryBlocked(i): true khi i là cổng vào (0) và bãi đã đầy (freeSlots = 0).
// Mọi lệnh mở cổng vào (tự động, web, UART) đều phải hỏi hàm này trước. Cổng ra không bị khoá.
// Ô lỗi, ô tắt, ô chưa chắc chắn đều đã được tính là bị chiếm, nên khi nghi ngờ thì cổng vào vẫn đóng.
bool entryBlocked(int i)
{
  return i == 0 && freeSlots == 0;
}

// fullWait[i]: true khi có xe đứng chờ ở cổng i lúc bãi đầy. Khi có ô trống trở lại mà xe vẫn
// đứng đó thì cổng tự mở, không bắt xe phải lùi ra rồi vào lại.
bool fullWait[2] = {false, false};

// [ĐIỀU KHIỂN LOGIC] ★ luật mở / đóng cổng
// updateGateLogic(i): quyết định mở / đóng cổng i theo cảm biến E18 (bỏ qua khi đang thủ công / đang học)
void updateGateLogic(int i)
{
  Gate& g = gates[i];
  bool det = g.stable;

  if (g.manual || !g.learned || g.calDir || g.nudgeUntil)
  {
    g.detPrev = det;
    return;
  }

  if (det)
  {
    g.lastDetect = millis();
    if (!g.detPrev || fullWait[i])            // xe vừa tới, hoặc xe đang chờ vì bãi đầy
    {
      if (entryBlocked(i))
      {
        if (!fullWait[i]) Serial.println(">>> BAI XE DAY - KHONG MO CONG VAO, XE CHO");
        fullWait[i] = true;
      }
      else
      {
        if (fullWait[i]) Serial.println(">>> DA CO CHO TRONG - MO CONG CHO XE DANG CHO");
        fullWait[i] = false;
        gateOpen(i);
      }
    }
  }
  else if (fabs(g.target - g.openAng) < fabs(g.target - g.closeAng) &&
           millis() - g.lastDetect >= g.closeDelay)
  {
    gateClose(i);
  }
  if (!det) fullWait[i] = false;              // xe bỏ đi thì thôi chờ

  g.detPrev = det;
}

// ================================================================================
// B8. LCD 1602 – dòng 1: chỗ trống + cổng (V/R: M mở, D đóng, * đang chạy, ? chưa học)
//                dòng 2: 1:X 2:O ... (X = có xe, O = trống)
// ================================================================================

// scanI2CBus(): dò mọi thiết bị trên I2C, ghi vào i2cFound để hiển thị khi không thấy LCD
void scanI2CBus()
{
  i2cFound = "";
  for (uint8_t a = 0x08; a < 0x78; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0)
    {
      char b[8];
      sprintf(b, "0x%02X ", a);
      i2cFound += b;
    }
  }
  if (i2cFound.length() == 0) i2cFound = "khong co";
  Serial.println("    Thiet bi I2C: " + i2cFound);
}

// tryInitLCD(): thử các địa chỉ LCD hay gặp (0x27, 0x3F...), thấy thì khởi tạo
bool tryInitLCD()
{
  const uint8_t addrs[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                           0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
  uint8_t found = 0;
  for (uint8_t k = 0; k < sizeof(addrs); k++)
  {
    Wire.beginTransmission(addrs[k]);
    if (Wire.endTransmission() == 0) { found = addrs[k]; break; }
  }
  if (!found) { lcdOK = false; return false; }

  if (lcd && lcdAddr != found) { delete lcd; lcd = nullptr; }
  if (!lcd) lcd = new LiquidCrystal_I2C(found, 16, 2);

  lcdAddr = found;
  lcd->init();
  lcd->backlight();
  lcd->clear();
  lcdOK = true;
  return true;
}

// retryLCD(): chưa có LCD thì cứ 10 s thử lại (cắm LCD sau vẫn nhận)
void retryLCD()
{
  if (lcdOK || millis() - lastLcdRetry < 10000) return;
  lastLcdRetry = millis();
  const uint8_t quick[2] = {0x27, 0x3F};
  for (int k = 0; k < 2; k++)
  {
    Wire.beginTransmission(quick[k]);
    if (Wire.endTransmission() == 0)
    {
      if (tryInitLCD()) Serial.printf(">>> Da tim thay LCD 0x%02X\n", lcdAddr);
      return;
    }
  }
}

// printLCDLine(dòng, chữ): in đúng 16 ký tự (thiếu thì thêm khoảng trắng, thừa thì cắt)
void printLCDLine(byte row, String text)
{
  if (!lcdOK) return;
  while (text.length() < 16) text += " ";
  if (text.length() > 16) text = text.substring(0, 16);
  lcd->setCursor(0, row);
  lcd->print(text);
}

// gateChar(i): ký hiệu cổng trên LCD: '?' chưa học | '*' đang chạy | 'M' mở | 'D' đóng
char gateChar(int i)
{
  Gate& g = gates[i];
  if (!g.learned) return '?';
  if (g.motion) return '*';
  return gateIsOpen(i) ? 'M' : 'D';
}

// [GIAO DIỆN]
// ghepDongLCD(l1, l2): ghép nội dung 2 dòng LCD (dùng chung cho LCD thật và trang web)
//   Thường: dòng 1 số chỗ trống + trạng thái 2 cổng, dòng 2 trạng thái 4 ô.
//   Có sự kiện xe vào / ra ô: dòng 1 hiện sự kiện trong 3 s.
//   Có kết quả biển số (lệnh PLATE): cả 2 dòng hiện biển và chủ xe hoặc XE LA trong 5 s.
void ghepDongLCD(String& l1, String& l2)
{
  l1 = (freeSlots == 0) ? "BAI DAY " : ("Con:" + String(freeSlots) + "/4 ");
  l1 += "V:"; l1 += gateChar(0);
  l1 += " R:"; l1 += gateChar(1);
  if (lcdMsgUntil && millis() < lcdMsgUntil) l1 = lcdMsg;   // hien su kien 3 s

  l2 = "";
  for (int i = 0; i < 4; i++)
  {
    l2 += String(i + 1) + ":" + (parkingOccupied[i] ? "X" : "O");
    if (i < 3) l2 += " ";
  }
  if (plateUntil && millis() < plateUntil) { l1 = plateL1; l2 = plateL2; }   // hien bien so 5 s
}

// [GIAO DIỆN]
// updateLCD(): mỗi 500 ms vẽ lại 2 dòng LCD
void updateLCD()
{
  if (millis() - previousLCDMillis < 500) return;
  previousLCDMillis = millis();

  String l1, l2;
  ghepDongLCD(l1, l2);
  printLCDLine(0, l1);
  printLCDLine(1, l2);
}

// ================================================================================
// B9. API /api/status – trả JSON trạng thái toàn hệ thống cho trang web
// ================================================================================

// [GIAO DIỆN] web. handleModeAPI(): đổi chế độ FUZZY / MODEL từ nút trên trang web (?m=fuzzy hoặc ?m=model),
// lưu vào flash và trả lại chế độ hiện tại
void handleModeAPI()
{
  String m = server.arg("m"); m.toLowerCase();
  if (m == "fuzzy" || m == "model")
  {
    opMode = (m == "model") ? 1 : 0;
    prefs.begin("parking2", false); prefs.putInt("mode", opMode); prefs.end();
    Serial.printf(">>> CHE DO (web): %s\n", opMode ? "MODEL" : "FUZZY");
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/plain; charset=utf-8", opMode ? "Che do MODEL (ML, xanh/do)" : "Che do FUZZY (binh thuong)");
}

// [GIAO DIỆN] web. handleStatusAPI(): gom số chỗ trống, trạng thái 4 ô (khoảng cách, màu, lý do quyết định,
// độ tin cậy ML), 2 cổng, camera AI, chế độ và LCD thành JSON trả cho trang web
void handleStatusAPI()
{
  String j;
  j.reserve(3000);
  j = "{\"free\":" + String(freeSlots) + ",\"slots\":[";
  for (int i = 0; i < 4; i++)
  {
    j += "{\"distance\":" + String(parkingDistance[i], 1);
    j += ",\"state\":\"" + String(getParkingState(parkingDistance[i])) + "\"";
    j += ",\"fs\":\"" + String(finalState[i]) + "\"";
    j += ",\"hc\":" + String(hcOccupied[i] ? "true" : "false");
    j += ",\"ai\":" + String(aiOnline() ? (aiSlot[i] ? "1" : "0") : "-1");
    j += ",\"ml\":" + String(mlFresh() ? (mlSlot[i] ? "1" : "0") : "-1");
    j += ",\"mc\":" + String(mlConf[i], 2);
    j += ",\"why\":\"" + String(whySrc[i]) + "\"";
    j += ",\"mm\":" + String(mismatchFlag[i] ? "true" : "false");
    j += ",\"err\":" + String(hcErrCode(i));
    j += ",\"us\":" + String(sensorRawUs[i]);
    j += ",\"tp\":" + String(trigOf(i)) + ",\"ep\":" + String(echoOf(i));
    j += ",\"lk\":" + String(hcLocked[i] ? "true" : "false");
    j += ",\"occ\":" + String(parkingOccupied[i] ? "true" : "false") + "}";   // [3D] kết quả cuối (đã giữ 1 s)
    if (i < 3) j += ",";
  }
  j += "],\"aiOn\":" + String(aiOnline() ? "true" : "false");
  // [3D] 2 dòng LCD đúng như LCD thật đang hiện (cùng cách ghép với updateLCD)
  {
    String l1, l2;
    ghepDongLCD(l1, l2);
    j += ",\"lcd1\":\"" + l1 + "\",\"lcd2\":\"" + l2 + "\"";
  }
  j += ",\"aiCnt\":" + String(aiMsgCount);
  j += ",\"mlOn\":" + String(mlFresh() ? "true" : "false");
  j += ",\"mlCnt\":" + String(mlMsgCount);
  j += ",\"mlAge\":" + String(mlLastMs ? (long)(millis() - mlLastMs) : -1L);
  j += ",\"aiAge\":" + String(aiLastMs ? (long)(millis() - aiLastMs) : -1L);
  j += ",\"mlThr\":" + String(CONF_THRESHOLD, 2);
  j += ",\"mode\":" + String(opMode);
  j += ",\"src\":" + String(slotSource);
  j += ",\"ev\":[";
  for (int k = 0; k < eventCount; k++)
  {
    j += "{\"ago\":" + String((millis() - events[k].t) / 1000);
    j += ",\"s\":" + String(events[k].slot + 1);
    j += ",\"in\":" + String(events[k].in ? "true" : "false");
    j += ",\"by\":" + String(events[k].by) + "}";
    if (k < eventCount - 1) j += ",";
  }
  j += "],\"lcd\":\"";
  if (lcdOK) { char b[16]; sprintf(b, "OK 0x%02X", lcdAddr); j += b; }
  else j += "KHONG THAY (I2C: " + i2cFound + ")";
  j += "\",\"gates\":[";
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    j += "{\"det\":" + String(g.stable ? "true" : "false");
    j += ",\"mode\":" + String(g.mode);
    j += ",\"ang\":" + String((int)roundf(g.ang));
    j += ",\"target\":" + String((int)roundf(g.target));
    j += ",\"moving\":" + String(g.motion);
    j += ",\"isOpen\":" + String(gateIsOpen(i) ? "true" : "false");
    j += ",\"learned\":" + String(g.learned ? "true" : "false");
    j += ",\"cal\":" + String(isCalibrated(i) ? "true" : "false");
    j += ",\"zero\":" + String(g.zeroSet ? "true" : "false");
    j += ",\"calDir\":" + String(g.calDir);
    j += ",\"m90R\":" + String(g.ms90R);
    j += ",\"m90L\":" + String(g.ms90L);
    j += ",\"oa\":" + String((int)roundf(g.openAng));
    j += ",\"ca\":" + String((int)roundf(g.closeAng));
    j += ",\"os\":" + String(g.openSet ? "true" : "false");
    j += ",\"spd\":" + String(g.speedUs);
    j += ",\"manual\":" + String(g.manual ? "true" : "false");
    j += ",\"en\":" + String(g.sensorEn ? 1 : 0);
    j += ",\"inv\":" + String(g.sensorInv ? 1 : 0);
    j += ",\"cf\":" + String(g.confirmMs);
    j += ",\"cd\":" + String(g.closeDelay) + "}";
    if (i < 1) j += ",";
  }
  j += "]}";

  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", j);
}

// ================================================================================
// B10. API /api/gate – ĐIỀU KHIỂN / HỌC CỔNG từ trang web
//   /api/gate?g=0|1&cmd=stop|nudge|zero|calstart|calstop|goto|saveopen|saveclose|open|close|auto|forget&dir=1|-1
//   Chuỗi trả về bắt đầu bằng "!" = thông báo lỗi hiện cho người dùng
// ================================================================================

// [GIAO DIỆN] web. handleGateAPI(): nhận lệnh cổng từ trang web (dừng, nhích, đo tốc độ, quay tới góc,
// lưu góc mở hoặc đóng, mở, đóng, tự động, xoá cài đặt) và trả lại kết quả cho người dùng
void handleGateAPI()
{
  int i = server.arg("g").toInt();
  String cmd = server.arg("cmd");
  int dir = server.arg("dir").toInt() >= 0 ? 1 : -1;
  if (i < 0 || i > 1) { server.send(400, "text/plain", "!Sai cong"); return; }

  Gate& g = gates[i];
  String msg = "OK";
  unsigned long now = millis();
  const char* TXT = "text/plain; charset=utf-8";

  if (cmd == "stop")
  {
    gateStopAll(i);
    server.send(200, TXT, "OK");
    return;
  }
  else if (cmd == "nudge")
  {
    // Nhich: da do toc do -> theo DO; chua do -> theo thoi gian
    float step = constrain(server.arg("step").toFloat(), 1, 90);
    if (g.calDir) { server.send(200, TXT, "!Đang đo, bấm DỪNG trước"); return; }
    g.manual = true;
    if (g.mode == 180)
    {
      if (!g.outputOn) { g.target = g.ang; g.outputOn = true; }
      g.target = constrain(g.target + dir * step, 0.0f, 180.0f);
    }
    else if (isCalibrated(i))
    {
      g.target = g.target + dir * step;
      g.target = constrain(g.target, -360.0f, 360.0f);
    }
    else
    {
      g.motion = 0;
      g.sv.run(dir, g.speedUs);
      g.nudgeUntil = now + (unsigned long)(step * 8);   // 5 do ~ 40 ms
    }
    server.send(200, TXT, "OK");
    return;
  }
  else if ((cmd == "zero" || cmd == "calstart") && g.mode == 180)
  {
    server.send(200, TXT, "!Servo 180°: không cần bước này. Dùng thanh trượt rồi Lưu MỞ / Lưu ĐÓNG.");
    return;
  }
  else if (cmd == "zero")
  {
    gateStopAll(i);
    g.manual = true;
    g.ang = 0;
    g.target = 0;
    g.zeroSet = true;
    g.closeAng = 0;
    g.closeSet = true;
    updateLearned(i);
    saveGate(i);
    msg = "Da dat goc 0 = DONG";
  }
  else if (cmd == "calstart")
  {
    if (!g.zeroSet) { server.send(200, TXT, "!Hãy làm Bước 1: đưa thanh chắn nằm ngang rồi bấm \"Đặt = ĐÓNG (0°)\""); return; }
    gateStopAll(i);
    g.manual = true;
    g.calDir = dir;
    g.calStart = now;
    g.calStartAng = g.ang;
    g.sv.run(dir, g.speedUs);
    msg = String("Bat dau do 90 do, quay ") + (dir > 0 ? "PHAI" : "TRAI");
  }
  else if (cmd == "calstop")
  {
    if (!g.calDir) { server.send(200, TXT, "!Không có phép đo nào đang chạy"); return; }
    unsigned long el = now - g.calStart;
    int d = g.calDir;
    g.sv.stop();
    g.calDir = 0;
    g.motion = 0;
    if (el < 80) { server.send(200, TXT, "!Thời gian quá ngắn, hãy đo lại"); return; }

    if (d > 0) g.ms90R = el; else g.ms90L = el;
    g.ang = g.calStartAng + d * 90.0f;
    g.target = g.ang;

    // Lan do dau tien tu vi tri 0 -> day chinh la vi tri MO
    if (fabs(g.calStartAng) < 1.0f)
    {
      g.openAng = g.ang;
      g.openSet = true;
    }
    updateLearned(i);
    if (g.learned) { g.manual = false; saveGateOpenState(i, gateIsOpen(i)); }
    saveGate(i);
    msg = String("Do xong quay ") + (d > 0 ? "PHAI" : "TRAI") + " 90 do = " + String(el) + " ms";
  }
  else if (cmd == "goto")
  {
    if (!isCalibrated(i)) { server.send(200, TXT, "!Hãy đo tốc độ (Bước 2 và 3) trước"); return; }
    g.manual = true;
    if (g.mode == 180)
    {
      if (!g.outputOn) { g.ang = 90; g.outputOn = true; }
      g.target = constrain(server.arg("angle").toFloat(), 0.0f, 180.0f);
    }
    else g.target = constrain(server.arg("angle").toFloat(), -180.0f, 180.0f);
    server.send(200, TXT, "OK");
    return;
  }
  else if (cmd == "saveopen" || cmd == "saveclose")
  {
    if (!isCalibrated(i)) { server.send(200, TXT, "!Hãy đo tốc độ (Bước 2 và 3) trước"); return; }
    if (g.motion) { server.send(200, TXT, "!Chờ servo dừng hẳn rồi lưu"); return; }
    if (cmd == "saveopen") { g.openAng = g.ang;  g.openSet = true;  msg = "Đã lưu góc MỞ = " + String(g.ang, 0) + "°"; }
    else                   { g.closeAng = g.ang; g.closeSet = true; msg = "Đã lưu góc ĐÓNG = " + String(g.ang, 0) + "°"; }
    updateLearned(i);
    saveGate(i);
    if (!g.learned) msg += "\n(Góc MỞ và ĐÓNG phải cách nhau ít nhất 10°)";
    Serial.printf("[WEB] %s: %s\n", g.name, msg.c_str());
    server.send(200, TXT, msg);
    return;
  }
  else if (cmd == "open" || cmd == "close")
  {
    if (!g.learned) { server.send(200, TXT, "!Cổng chưa học xong – làm đủ Bước 1, 2, 3"); return; }
    if (cmd == "open" && entryBlocked(i)) { server.send(200, TXT, "!Bãi đã đầy, không mở cổng vào"); return; }
    g.manual = true;
    if (cmd == "open") gateOpen(i); else gateClose(i);
    msg = cmd == "open" ? "MO (thu cong)" : "DONG (thu cong)";
  }
  else if (cmd == "auto")
  {
    g.manual = false;
    g.detPrev = false;
    if (g.learned) gateClose(i);
    msg = "Ve TU DONG";
  }
  else if (cmd == "forget")
  {
    gateStopAll(i);
    g.ms90R = g.ms90L = 0;
    g.openSet = g.closeSet = g.zeroSet = false;
    g.ang = g.target = (g.mode == 180) ? 90 : 0;
    g.outputOn = false;
    g.sv.stop();
    updateLearned(i);
    saveGate(i);
    msg = "Da xoa du lieu hoc";
  }
  else
  {
    server.send(400, TXT, "!Sai lệnh");
    return;
  }

  Serial.printf("[WEB] %s: %s\n", g.name, msg.c_str());
  server.send(200, TXT, msg);
}

// ================================================================================
// B11. API /api/settings – lưu cài đặt từ trang web
// ================================================================================

// [GIAO DIỆN] web. handleSettingsAPI(): nhận cài đặt từ trang web rồi lưu vào flash: nguồn xác định chỗ đỗ (src),
// bật hoặc đảo cảm biến E18, thời gian xác nhận, thời gian chờ đóng cổng, loại servo 180 hoặc 360, tốc độ quay
void handleSettingsAPI()
{
  String note = "";
  if (server.hasArg("src"))
  {
    slotSource = constrain(server.arg("src").toInt(), 0, 2);
    prefs.begin("parking2", false);
    prefs.putInt("src", slotSource);
    prefs.end();
  }
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    String s = String(i);
    if (server.hasArg("en" + s))  g.sensorEn   = server.arg("en" + s).toInt() == 1;
    if (server.hasArg("inv" + s)) g.sensorInv  = server.arg("inv" + s).toInt() == 1;
    if (server.hasArg("cf" + s))  g.confirmMs  = constrain(server.arg("cf" + s).toInt(), 20, 5000);
    if (server.hasArg("cd" + s))  g.closeDelay = constrain(server.arg("cd" + s).toInt(), 200, 20000);
    if (server.hasArg("mode" + s))
    {
      int m = server.arg("mode" + s).toInt() == 360 ? 360 : 180;
      if (m != g.mode)
      {
        gateStopAll(i);
        g.mode = m;
        g.ms90R = g.ms90L = 0;
        g.openSet = g.closeSet = g.zeroSet = false;
        g.outputOn = false;
        g.ang = g.target = (m == 180) ? 90 : 0;
        g.sv.stop();
        updateLearned(i);
        note += String("\n• ") + (i == 0 ? "Cổng vào" : "Cổng ra") + ": đổi sang servo " + String(m) + "° → cần HỌC LẠI.";
      }
    }
    if (server.hasArg("spd" + s))
    {
      int spd = constrain(server.arg("spd" + s).toInt(), 100, 600);
      if (spd != g.speedUs)
      {
        g.speedUs = spd;
        if (g.ms90R || g.ms90L)
        {
          gateStopAll(i);
          g.ms90R = g.ms90L = 0;
          updateLearned(i);
          note += String("\n• ") + (i == 0 ? "Cổng vào" : "Cổng ra") + ": đã đổi tốc độ → cần ĐO LẠI 90° (Bước 2, 3).";
        }
      }
    }
    saveGate(i);
  }
  Serial.println("[WEB] Da luu cai dat");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/plain; charset=utf-8", "Đã lưu cài đặt vào ESP32 ✔" + note);
}

// ================================================================================
// B12. IN TRẠNG THÁI RA SERIAL MONITOR (mỗi 2 s)
// ================================================================================

// [GIAO DIỆN] Serial. printSerialStatus(): mỗi 2 s in tóm tắt ra Serial Monitor gồm số chỗ trống, chế độ,
// ML, camera AI, khoảng cách và trạng thái từng ô, trạng thái từng cổng
void printSerialStatus()
{
  if (millis() - previousSerialMillis < 2000) return;
  previousSerialMillis = millis();

  Serial.println("\n================ BAI XE ================");
  for (int i = 0; i < 4; i++)
  {
    Serial.printf("O %d (TRIG %2d ECHO %2d): ", i + 1, trigOf(i), echoOf(i));
    if      (hcErrCode(i) == 4) Serial.print("-- [LOI: MAT TIENG VANG QUA 3 s - kiem tra day]");
    else if (sensorErr[i] == 1) Serial.print("-- [khong co vat / khong co vang]");
    else if (sensorErr[i] == 2) Serial.print("-- [LOI: ECHO KET MUC CAO]");
    else if (sensorErr[i] == 3) Serial.print("-- [qua gan < 2 cm]");
    else Serial.printf("%.1f cm (%lu us) %s", parkingDistance[i], sensorRawUs[i], getParkingState(parkingDistance[i]));
    Serial.println();
  }
  Serial.printf("Cho trong: %d/4 | LCD: %s | Camera AI: %s (%s)\n", freeSlots, lcdOK ? "OK" : "KHONG THAY",
                aiOnline() ? "KET NOI" : "MAT KET NOI",
                slotSource == 0 ? "chi HC-SR04" : (slotSource == 1 ? "chi AI" : "ket hop"));
  if (aiOnline())
    Serial.printf("AI: o1=%d o2=%d o3=%d o4=%d\n", aiSlot[0], aiSlot[1], aiSlot[2], aiSlot[3]);
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    Serial.printf("%-8s: servo %d | cam bien %s (GPIO%d=%s) | %s | %s | goc %d (MO %d, DONG %d)\n",
                  g.name, g.mode,
                  !g.sensorEn ? "TAT" : (g.stable ? "CO XE" : "KHONG XE"),
                  g.irPin, digitalRead(g.irPin) ? "HIGH" : "LOW",
                  g.learned ? "DA HOC" : "CHUA HOC",
                  g.manual ? "THU CONG" : "TU DONG",
                  (int)g.ang, (int)g.openAng, (int)g.closeAng);
  }
  Serial.println("========================================");
}

// ================================================================================
// B13. KHỞI ĐỘNG WIFI + WEB SERVER
// ================================================================================

// startWiFiAP(): bật điểm phát WiFi BAI_XE_ESP32S3, IP cố định 192.168.4.1, DNS trỏ mọi tên miền về ESP32
void startWiFiAP()
{
  Serial.println("[1] Khoi dong WiFi AP...");
  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  { prefs.begin("parking2", true); String sv = prefs.getString("ssid", ""); prefs.end();
    wifiSsidNow = (sv.length() >= 1 && sv.length() <= 31) ? sv : String(WIFI_SSID); }
  bool ok = WiFi.softAP(wifiSsidNow.c_str(), WIFI_PASSWORD, WIFI_CHANNEL, 0, 4);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.setSleep(false);
  dnsServer.start(53, "*", WiFi.softAPIP());
  delay(200);
  Serial.printf("    %s | SSID %s | MK %s | http://%s\n", ok ? "WiFi OK" : "LOI WIFI",
                wifiSsidNow.c_str(), WIFI_PASSWORD, WiFi.softAPIP().toString().c_str());
}

// setupWebServer(): đăng ký các đường dẫn web: / (trang chính), /api/status, /api/gate, /api/settings
//   + các đường dẫn điện thoại / máy tính hay gọi để kiểm tra Internet
void setupWebServer()
{
  Serial.println("[2] Khoi dong Web server...");
  // Trang chính = GIAO DIỆN 3D (đã nén gzip trong web3d.h)
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html; charset=utf-8", (const char*)WEB3D_INDEX_GZ, WEB3D_INDEX_GZ_LEN);
  });
  server.on("/app.js", HTTP_GET, []() {   // Three.js + cảnh 3D (~160 KB nén) – trình duyệt giữ lại 7 ngày
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "max-age=604800");
    server.send_P(200, "application/javascript", (const char*)WEB3D_APP_GZ, WEB3D_APP_GZ_LEN);
  });
  // Trang CÀI ĐẶT & HỌC CỔNG (giao diện cũ, đủ chức năng học góc servo / cài đặt cảm biến)
  server.on("/caidat", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  server.on("/api/status",   HTTP_GET, handleStatusAPI);
  server.on("/api/mode",     HTTP_GET, handleModeAPI);
  server.on("/api/gate",     HTTP_GET, handleGateAPI);
  server.on("/api/settings", HTTP_GET, handleSettingsAPI);

  // Tra loi kiem tra Internet cua dien thoai / may tinh
  server.on("/generate_204", []() { server.send(204); });
  server.on("/gen_204",      []() { server.send(204); });
  server.on("/hotspot-detect.html", []() {
    server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/connecttest.txt", []() { server.send(200, "text/plain", "Microsoft Connect Test"); });
  server.on("/ncsi.txt",        []() { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// ================================================================================
// PHẦN C – CHƯƠNG TRÌNH CHÍNH
// C1. setup() – KHỞI ĐỘNG 7 BƯỚC (in [1]..[7] ra Serial)
// ================================================================================

void setup()
{
  Serial.begin(115200);
  Serial.setTimeout(20);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.begin(115200);   // cong UART (COM) - nhan du lieu camera AI tu PC
#endif
  delay(1500);
  Serial.println("\n========== KHOI DONG HE THONG ==========");

  // Cau hinh 2 cong
  gates[0].name = "Cong vao"; gates[0].servoPin = SERVO_IN_PIN;  gates[0].irPin = IR_IN_PIN;
  gates[1].name = "Cong ra";  gates[1].servoPin = SERVO_OUT_PIN; gates[1].irPin = IR_OUT_PIN;

  // Servo gan som va DUNG NGAY -> khong quay luc khoi dong
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    g.sv.attach(g.servoPin, 4 + i);
    g.sv.stop();
    g.motion = 0; g.calDir = 0; g.nudgeUntil = 0; g.manual = false; g.ang = 0; g.target = 0;
    g.outputOn = false; g.lastStepMs = 0; g.mode = 180;
    g.rawPrev = false; g.stable = false; g.detPrev = false; g.changeMs = 0; g.lastDetect = 0;
    g.lastMotionMs = millis();
  }

  // [1] WiFi + [2] web server – bật trước để dù phần cứng lỗi vẫn vào được web
  startWiFiAP();
  setupWebServer();

  // [3] Đọc cài đặt flash, đặt góc ban đầu cho 2 cổng
  Serial.println("[3] Doc cai dat da luu...");
  loadSettings();
  for (int i = 0; i < 2; i++)
  {
    Gate& g = gates[i];
    g.outputOn = false;
    if (g.mode == 180)
    {
      if (g.learned)
      {
        g.ang = g.savedOpen ? g.openAng : g.closeAng;
        g.target = g.closeAng;
        g.outputOn = true;
        g.sv.writeAngle(g.ang);
        Serial.printf("    %s: SERVO 180 - DA HOC (MO %.0f, DONG %.0f)\n", g.name, g.openAng, g.closeAng);
      }
      else
      {
        g.ang = g.target = 90;
        Serial.printf("    %s: SERVO 180 - CHUA HOC -> khong xuat xung. Vao web de hoc.\n", g.name);
      }
    }
    else if (g.learned)
    {
      // Lan truoc tat nguon khi dang MO -> tu dong DONG lai
      g.ang = g.savedOpen ? g.openAng : g.closeAng;
      g.target = g.closeAng;
      Serial.printf("    %s: DA HOC (MO %.0f do, DONG %.0f do, 90 do: phai %lums / trai %lums)%s\n",
                    g.name, g.openAng, g.closeAng, g.ms90R, g.ms90L,
                    g.savedOpen ? " -> dang mo, se DONG lai" : "");
    }
    else
    {
      g.ang = 0;
      g.target = 0;
      Serial.printf("    %s: CHUA HOC -> servo KHONG hoat dong. Vao web de hoc cong.\n", g.name);
    }
  }

  // [4] Chân HC-SR04 (đã tính tự đảo) + E18
  Serial.println("[4] Cau hinh chan cam bien...");
  loadHcSwap();
  for (int i = 0; i < 4; i++)
  {
    applyHcPins(i);
    Serial.printf("    O %d: TRIG=GPIO%d ECHO=GPIO%d%s\n", i + 1, trigOf(i), echoOf(i),
                  hcSwap[i] ? " (da tu dao)" : "");
  }
  pinMode(IR_IN_PIN, INPUT_PULLUP);
  pinMode(IR_OUT_PIN, INPUT_PULLUP);

  // [5] Đèn: khởi tạo + test lần lượt 4 đèn
  Serial.println("[5] Den WS2812B (GPIO11-14)...");
  for (int i = 0; i < 4; i++)
  {
    slotPixel[i].begin();
    slotPixel[i].setBrightness(80);
  }
  startupLEDTest();

  // [6] I2C + LCD
  Serial.println("[6] LCD...");
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  Wire.setTimeOut(20);
  scanI2CBus();
  if (tryInitLCD()) Serial.printf("    LCD OK 0x%02X\n", lcdAddr);
  else Serial.printf("    KHONG THAY LCD (SDA=GPIO%d SCL=GPIO%d) -> thu lai moi 10 s\n", I2C_SDA, I2C_SCL);
  printLCDLine(0, "WIFI:BAI_XE");
  printLCDLine(1, "192.168.4.1");

  // [7] Đo 4 ô lần đầu để có trạng thái ngay khi khởi động
  Serial.println("[7] Doc HC-SR04 lan dau...");
  for (int i = 0; i < 4; i++) { updateOneParkingSensor(); delay(60); }
  updateFusion();

  for (int i = 0; i < 2; i++) gates[i].lastMotionMs = millis();

  Serial.println("========== HE THONG SAN SANG ==========");
}

// ================================================================================
// C2. loop() – VÒNG LẶP CHÍNH (mỗi vòng ~2 ms, không có lệnh chờ lâu)
// ================================================================================

void loop()
{
  // (1) [GIAO DIỆN] Web: trả lời DNS + trình duyệt
  dnsServer.processNextRequest();
  server.handleClient();

  // (2) [CẢM BIẾN] → [PHÂN LOẠI]  Đo 1 HC-SR04 mỗi 70 ms (4 ô xoay vòng)
  if (millis() - previousSensorMillis >= SENSOR_INTERVAL)
  {
    previousSensorMillis = millis();
    updateOneParkingSensor();
  }

  // (3) [XỬ LÝ AI] → [KẾT HỢP]  Camera AI / PC: nhận lệnh → báo trạng thái → kết hợp AI + HC-SR04
  handlePCSerial();   // nhan ket qua camera AI + lenh tu PC
  sendStatusToPC();   // gui trang thai cho PC moi 1 s
  updateFusion();     // ket hop AI + HC-SR04

  // (4) [CẢM BIẾN] → [ĐIỀU KHIỂN LOGIC]  Cổng: đọc E18 → logic mở/đóng → quay servo
  updateIRSensors();
  for (int i = 0; i < 2; i++)
  {
    updateGateLogic(i);
    updateGateMotion(i);
  }

  // (5) [ĐIỀU KHIỂN LOGIC] đèn → [GIAO DIỆN] LCD → in Serial
  updateSlotLEDs();
  retryLCD();
  updateLCD();
  printSerialStatus();

  delay(2);
}
