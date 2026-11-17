// Giai đoạn 1: ESP32 DevKit V1 nhận lệnh MQTT từ server, điều khiển 2 cổng, nút chọn, màn OLED trạng thái.
// Lệnh (topic <prefix>/esp/cmd): OPEN_GATE, OPEN_WASH, ASK_CHOICE, END_CHOICE, DROP_WIFI,<giây>
// Status (retained): ONLINE,GATE=OPEN|CLOSED,WASH=OPEN|CLOSED,GATE_CYCLES=<n>; Last Will = OFFLINE
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ESP32Servo.h>
#include "net_config.h"

// ---- chân (ESP32 thường: tránh flash 6-11, chỉ-vào 34-39, strapping 0/2/5/12/15) ----
#define GATE_SERVO_PIN 18
#define PUMP_RELAY_PIN 23  // relay bơm nước (bơm nối COM/NO: mất điện = bơm tắt)
#define OLED_SDA_PIN 21     // OLED I2C 128x64
#define OLED_SCL_PIN 22     // chân "SCK" trên module
#define BUTTON_PIN 27      // nút -> GND

// ---- góc servo 180° (mặc định theo lab4 MO_HINH 2; hiệu chỉnh ở CP3 trên cổng thật) ----
#define GATE_OPEN_DEG 15
#define GATE_CLOSED_DEG 90
#define PUMP_RUN_MS 10000      // OPEN_WASH -> bơm chạy 10 s rồi ESP tự tắt, không chờ server
// ---- Khác biệt mô phỏng / mạch thật: mỗi mục 1 cờ, mặc định = mạch thật (xem stage1/README.md) ----
#ifndef SERVO_SAFE_90
  #define SERVO_SAFE_90 0        // 1 = chỉ gửi 1500 us, không quay (env esp32dev bật khi chưa biết loại servo)
#endif
#ifndef SIM_HEARTBEAT_MS
  #define SIM_HEARTBEAT_MS 0     // >0: gửi <prefix>/esp/hb mỗi N ms (mô phỏng chạy chậm, broker cắt keepalive)
#endif
#ifndef RELAY_ACTIVE_LOW
  #define RELAY_ACTIVE_LOW 1     // 1 = relay đóng khi IN mức THẤP (đa số module relay rẻ; chưa kiểm module thật)
#endif
#ifndef OLED_SH1106
  #define OLED_SH1106 0          // 1 = module dùng chip SH1106 (ảnh lệch/rác ở mép), 0 = SSD1306 (Wokwi)
#endif
#ifndef SIM_SERIAL_BUTTON
  #define SIM_SERIAL_BUTTON 0    // 1: gõ 'b' qua serial = nhấn nút 200 ms
#endif

#define GATE_OPEN_MS 5000      // ESP tự đóng cổng, không chờ server
#ifndef CHOICE_MAX_MS
  #define CHOICE_MAX_MS 20000  // tự tắt chế độ chọn nếu không nhận END_CHOICE (> 15 s của server)
#endif
#define DEBOUNCE_MS 30
#define KEEPALIVE_S 5          // broker đăng OFFLINE sau ~1.5 x 5 = 7.5 s
#define RETRY_MS 2000
#define WIFI_REBEGIN_MS 15000

const String T_CMD = String(TOPIC_PREFIX) + "/esp/cmd";
const String T_STATUS = String(TOPIC_PREFIX) + "/esp/status";
const String T_EVENT = String(TOPIC_PREFIX) + "/esp/event";
const String CLIENT_ID = String(TOPIC_PREFIX) + "-esp32";

WiFiClient net;
PubSubClient mqtt(net);
#if OLED_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif
bool oledOk = false;          // false = không thấy màn: bỏ qua vẽ, phần còn lại chạy bình thường
Servo gateServo;

bool gateOpen = false, washOpen = false;  // washOpen = bơm đang chạy (status WASH=OPEN)
uint32_t gateOpenedAt = 0, washOpenedAt = 0;
uint32_t gateCycles = 0;      // số lần cổng vào mở -> đóng xong, tính từ lúc bật nguồn
bool statusDirty = false;     // cần gửi status khi online

bool choiceArmed = false;
uint32_t choiceSince = 0;

uint32_t dropUntil = 0;       // >0: đang cố ý ngắt WiFi (Test D)
uint32_t lastMqttTry = 0;
uint32_t wifiLostAt = 0;

void drawStatus(bool online, bool blinkOn) {
  if (!oledOk) return;
  uint32_t t0 = micros();
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, online ? "MANG: ONLINE" : "MANG: OFFLINE");
  oled.drawStr(0, 22, gateOpen ? "CONG VAO: MO" : "CONG VAO: DONG");
  oled.drawStr(0, 34, washOpen ? "BOM NUOC: CHAY" : "BOM NUOC: TAT");
  if (blinkOn) {  // chế độ chọn: ô nhắc đảo màu, nhấp nháy 1 Hz
    oled.drawBox(0, 50, 128, 14);
    oled.setDrawColor(0);
    oled.drawStr(13, 61, "BAM NUT DE RUA XE");
    oled.setDrawColor(1);
  }
  oled.sendBuffer();
  static bool timed = false;
  if (!timed) { timed = true; Serial.printf("[OLED] ve 1 lan mat %lu us\n", (unsigned long)(micros() - t0)); }
}

void servoTo(Servo &s, const char *name, int deg) {
#if SERVO_SAFE_90
  Serial.printf("[SERVO] %s -> %d do (SAFE_90: khong quay)\n", name, deg);
#else
  s.write(deg);
  Serial.printf("[SERVO] %s -> %d do\n", name, deg);
#endif
}

void setPump(bool on) {
  digitalWrite(PUMP_RELAY_PIN, on == !RELAY_ACTIVE_LOW ? HIGH : LOW);
  Serial.printf("[BOM] %s\n", on ? "CHAY" : "TAT");
}

void publishStatus() {
  char buf[96];
  snprintf(buf, sizeof buf, "ONLINE,GATE=%s,WASH=%s,GATE_CYCLES=%lu",
           gateOpen ? "OPEN" : "CLOSED", washOpen ? "OPEN" : "CLOSED", (unsigned long)gateCycles);
  if (mqtt.publish(T_STATUS.c_str(), buf, true)) {  // retained
    statusDirty = false;
    Serial.printf("[MQTT] status %s\n", buf);
  }
}

void openGate() {
  if (gateOpen) return;  // đang mở: bỏ qua, không tính 2 chu kỳ
  gateOpen = true;
  gateOpenedAt = millis();
  servoTo(gateServo, "cong vao", GATE_OPEN_DEG);
  statusDirty = true;
}

void openWash() {
  if (washOpen) return;
  washOpen = true;
  washOpenedAt = millis();
  setPump(true);
  statusDirty = true;
}

void setChoice(bool on) {
  choiceArmed = on;
  choiceSince = millis();
}

void startDrop(int sec) {
  Serial.printf("[TEST] DROP_WIFI %d s\n", sec);
  dropUntil = millis() + sec * 1000UL;
  if (dropUntil == 0) dropUntil = 1;
  // KHÔNG gọi mqtt.disconnect(): ngắt sạch thì broker không đăng Last Will
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  net.stop();
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.printf("[MQTT] %s %s\n", topic, cmd.c_str());
  if (cmd == "OPEN_GATE") openGate();
  else if (cmd == "OPEN_WASH") openWash();
  else if (cmd == "ASK_CHOICE") setChoice(true);
  else if (cmd == "END_CHOICE") setChoice(false);
  else if (cmd.startsWith("DROP_WIFI,")) {
    int sec = cmd.substring(10).toInt();
    if (sec >= 1 && sec <= 60) startDrop(sec);
  }
}

void mqttTryConnect() {
  Serial.printf("[MQTT] noi %s:%d (prefix %s) ... ", BROKER_HOST, BROKER_PORT, TOPIC_PREFIX);
  if (mqtt.connect(CLIENT_ID.c_str(), T_STATUS.c_str(), 1, true, "OFFLINE")) {
    Serial.println("OK");
    mqtt.subscribe(T_CMD.c_str(), 0);
    publishStatus();
  } else {
    Serial.printf("loi rc=%d\n", mqtt.state());
  }
}

void wifiBegin() {
  if (WIFI_CHANNEL) WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  else WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\n[BOOT] prefix=%s broker=%s:%d SAFE_90=%d\n", TOPIC_PREFIX, BROKER_HOST, BROKER_PORT, SERVO_SAFE_90);

  // Servo về ĐÓNG ngay khi khởi động: reset không thể để cổng mở
  gateServo.setPeriodHertz(50);
  gateServo.attach(GATE_SERVO_PIN, 500, 2500);
#if SERVO_SAFE_90
  gateServo.writeMicroseconds(1500);  // test an toàn: đúng 1500 us (servo 360 thì đứng yên)
  Serial.println("[SERVO] SAFE_90: servo cong vao nhan 1500 us, khong dieu khien them");
#else
  servoTo(gateServo, "cong vao", GATE_CLOSED_DEG);
#endif
  // Bơm TẮT ngay khi khởi động: ghi mức TẮT trước khi bật OUTPUT để relay không nháy
  digitalWrite(PUMP_RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  setPump(false);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  uint8_t oledAddr = 0;
  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { oledAddr = a; break; }
  }
  if (oledAddr) {
    oled.setI2CAddress(oledAddr * 2);  // U8g2 dùng địa chỉ 8 bit
    oled.setBusClock(400000);
    oledOk = oled.begin();
  }
  Serial.printf("[OLED] %s 0x%02X (chip %s)\n", oledOk ? "thay" : "KHONG thay", oledAddr, OLED_SH1106 ? "SH1106" : "SSD1306");
  drawStatus(false, false);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiBegin();
  Serial.printf("[WIFI] vao %s\n", WIFI_SSID);

  net.setTimeout(TCP_CONNECT_TIMEOUT_S);
  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(KEEPALIVE_S);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
}

void loop() {
  uint32_t now = millis();

  // 1) Tự đóng cổng chạy ĐẦU TIÊN, không phụ thuộc mạng
  if (gateOpen && now - gateOpenedAt >= GATE_OPEN_MS) {
    gateOpen = false;
    gateCycles++;
    servoTo(gateServo, "cong vao", GATE_CLOSED_DEG);
    statusDirty = true;
  }
  if (washOpen && now - washOpenedAt >= PUMP_RUN_MS) {
    washOpen = false;
    setPump(false);
    statusDirty = true;
  }

  // 2) Chế độ chọn: OLED nhắc (mục 5), nút gửi BUTTON (chống dội)
  if (choiceArmed && now - choiceSince >= CHOICE_MAX_MS) setChoice(false);
  static bool btnStable = HIGH, btnLast = HIGH;
  static uint32_t btnChangedAt = 0;
  bool raw = digitalRead(BUTTON_PIN);
#if SIM_SERIAL_BUTTON
  // Chỉ bản mô phỏng: gõ 'b' qua serial = giữ nút 200 ms (đi qua cùng đường chống dội + gửi BUTTON)
  static uint32_t simPressUntil = 0;
  while (Serial.available()) {
    if (Serial.read() == 'b') { simPressUntil = now + 200; Serial.println("[SIM] nut ao qua serial"); }
  }
  if ((int32_t)(simPressUntil - now) > 0) raw = LOW;
#endif
  if (raw != btnLast) { btnLast = raw; btnChangedAt = now; }
  if (now - btnChangedAt >= DEBOUNCE_MS && raw != btnStable) {
    btnStable = raw;
    if (btnStable == LOW && !choiceArmed) Serial.println("[BTN] nhan (khong o che do chon, bo qua)");
    if (btnStable == LOW && choiceArmed) {
      bool sent = mqtt.connected() && mqtt.publish(T_EVENT.c_str(), "BUTTON");
      Serial.printf("[BTN] nhan -> BUTTON %s\n", sent ? "da gui" : "KHONG gui duoc (offline)");
      if (sent) setChoice(false);
    }
  }

  // 3) Test D: hết thời gian ngắt thì vào lại WiFi
  if (dropUntil && (int32_t)(now - dropUntil) >= 0) {
    dropUntil = 0;
    Serial.println("[TEST] het DROP_WIFI, vao lai WiFi");
    WiFi.setAutoReconnect(true);
    wifiBegin();
    wifiLostAt = now;
  }

  // 4) WiFi + MQTT (bỏ qua khi đang cố ý ngắt)
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  if (!dropUntil) {
    if (!wifiOk) {
      if (wifiLostAt == 0) {
        wifiLostAt = now;
        Serial.println("[WIFI] mat ket noi");
      } else if (now - wifiLostAt > WIFI_REBEGIN_MS) {
        Serial.println("[WIFI] begin lai");
        WiFi.disconnect();
        wifiBegin();
        wifiLostAt = now;
      }
    } else if (wifiLostAt != 0) {
      wifiLostAt = 0;
      Serial.printf("[WIFI] OK, IP %s\n", WiFi.localIP().toString().c_str());
    }
    if (wifiOk && !mqtt.connected() && now - lastMqttTry >= RETRY_MS) {
      lastMqttTry = now;
      mqttTryConnect();
    }
  }
  if (mqtt.connected()) {
    mqtt.loop();
    if (statusDirty) publishStatus();
#if SIM_HEARTBEAT_MS > 0
    static uint32_t lastHb = 0;
    if (now - lastHb >= SIM_HEARTBEAT_MS) {
      lastHb = now;
      mqtt.publish((String(TOPIC_PREFIX) + "/esp/hb").c_str(), "1");
    }
#endif
  }

  // 5) OLED: chỉ vẽ lại khi trạng thái đổi (gửi cả màn qua I2C mất vài chục ms)
  bool online = mqtt.connected();
  bool blinkOn = choiceArmed && (now / 500) % 2;
  int16_t shown = online | gateOpen << 1 | washOpen << 2 | blinkOn << 3;
  static int16_t lastShown = -1;
  if (shown != lastShown) {
    lastShown = shown;
    drawStatus(online, blinkOn);
  }
}
