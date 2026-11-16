// Giai đoạn 1: ESP32-S3 nhận lệnh MQTT từ server, điều khiển 2 cổng, nút chọn, LED trạng thái.
// Lệnh (topic <prefix>/esp/cmd): OPEN_GATE, OPEN_WASH, ASK_CHOICE, END_CHOICE, DROP_WIFI,<giây>
// Status (retained): ONLINE,GATE=OPEN|CLOSED,WASH=OPEN|CLOSED,GATE_CYCLES=<n>; Last Will = OFFLINE
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include "net_config.h"

// ---- chân (dùng lại phần cứng bo lab4) ----
#define GATE_SERVO_PIN 18
#define WASH_SERVO_PIN 39
#define LED_ONLINE_PIN 11  // WS2812 ô 1
#define LED_CHOICE_PIN 12  // WS2812 ô 2
#define BUTTON_PIN 15      // nút -> GND

// ---- góc servo 180° (mặc định theo lab4 MO_HINH 2; hiệu chỉnh ở CP3 trên cổng thật) ----
#define GATE_OPEN_DEG 15
#define GATE_CLOSED_DEG 90
#define WASH_OPEN_DEG 29
#define WASH_CLOSED_DEG 90
// ---- Khác biệt mô phỏng / mạch thật: mỗi mục 1 cờ, mặc định = mạch thật (xem stage1/README.md) ----
#ifndef SERVO_SAFE_90
  #define SERVO_SAFE_90 0        // 1 = chỉ gửi 1500 us, không quay (env esp32s3 bật khi chưa biết loại servo)
#endif
#ifndef SIM_HEARTBEAT_MS
  #define SIM_HEARTBEAT_MS 0     // >0: gửi <prefix>/esp/hb mỗi N ms (mô phỏng chạy chậm, broker cắt keepalive)
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
Adafruit_NeoPixel ledOnline(1, LED_ONLINE_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledChoice(1, LED_CHOICE_PIN, NEO_GRB + NEO_KHZ800);
Servo gateServo, washServo;

bool gateOpen = false, washOpen = false;
uint32_t gateOpenedAt = 0, washOpenedAt = 0;
uint32_t gateCycles = 0;      // số lần cổng vào mở -> đóng xong, tính từ lúc bật nguồn
bool statusDirty = false;     // cần gửi status khi online

bool choiceArmed = false;
uint32_t choiceSince = 0;

uint32_t dropUntil = 0;       // >0: đang cố ý ngắt WiFi (Test D)
uint32_t lastMqttTry = 0;
uint32_t wifiLostAt = 0;

void setLed(Adafruit_NeoPixel &p, uint8_t r, uint8_t g, uint8_t b) {
  p.setPixelColor(0, p.Color(r, g, b));
  p.show();
}

void servoTo(Servo &s, const char *name, int deg) {
#if SERVO_SAFE_90
  Serial.printf("[SERVO] %s -> %d do (SAFE_90: khong quay)\n", name, deg);
#else
  s.write(deg);
  Serial.printf("[SERVO] %s -> %d do\n", name, deg);
#endif
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
  servoTo(washServo, "cong rua", WASH_OPEN_DEG);
  statusDirty = true;
}

void setChoice(bool on) {
  choiceArmed = on;
  choiceSince = millis();
  if (!on) setLed(ledChoice, 0, 0, 0);
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
  washServo.setPeriodHertz(50);
  gateServo.attach(GATE_SERVO_PIN, 500, 2500);
  washServo.attach(WASH_SERVO_PIN, 500, 2500);
#if SERVO_SAFE_90
  gateServo.writeMicroseconds(1500);  // test an toàn: đúng 1500 us (servo 360 thì đứng yên)
  washServo.writeMicroseconds(1500);
  Serial.println("[SERVO] SAFE_90: ca 2 servo nhan 1500 us, khong dieu khien them");
#else
  servoTo(gateServo, "cong vao", GATE_CLOSED_DEG);
  servoTo(washServo, "cong rua", WASH_CLOSED_DEG);
#endif

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ledOnline.begin();
  ledChoice.begin();
  setLed(ledOnline, 40, 0, 0);  // đỏ = chưa online
  setLed(ledChoice, 0, 0, 0);

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
  if (washOpen && now - washOpenedAt >= GATE_OPEN_MS) {
    washOpen = false;
    servoTo(washServo, "cong rua", WASH_CLOSED_DEG);
    statusDirty = true;
  }

  // 2) Chế độ chọn: LED 2 nhấp nháy vàng, nút gửi BUTTON (chống dội)
  if (choiceArmed) {
    if (now - choiceSince >= CHOICE_MAX_MS) setChoice(false);
    else {
      bool on = (now / 500) % 2;
      static bool lastOn = false;
      if (on != lastOn) { lastOn = on; on ? setLed(ledChoice, 40, 30, 0) : setLed(ledChoice, 0, 0, 0); }
    }
  }
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

  static bool shownOnline = false;
  bool online = mqtt.connected();
  if (online != shownOnline) {
    shownOnline = online;
    online ? setLed(ledOnline, 0, 40, 0) : setLed(ledOnline, 40, 0, 0);
  }
}
