// Chọn mạng/broker theo bản build (xem platformio.ini). Không sửa code để đổi đường mạng.
#pragma once

#if defined(SIM_WOKWI)
  // Wokwi: AP ảo mở, kênh 6 (bỏ bước quét, nhanh ~4 s)
  #define WIFI_SSID "Wokwi-GUEST"
  #define WIFI_PASS ""
  #define WIFI_CHANNEL 6
  // Gateway mô phỏng chậm hơn mạng thật -> nới thời gian chờ
  #define TCP_CONNECT_TIMEOUT_S 5
  #define MQTT_SOCKET_TIMEOUT_S 5
#else
  #include "secrets.h"   // WIFI_SSID, WIFI_PASS, BROKER_IP (gitignored)
  #define WIFI_CHANNEL 0
  #define TCP_CONNECT_TIMEOUT_S 1
  #define MQTT_SOCKET_TIMEOUT_S 2
  #ifndef BROKER_HOST
    #define BROKER_HOST BROKER_IP
  #endif
#endif

#ifndef BROKER_PORT
  #define BROKER_PORT 1883
#endif
#ifndef TOPIC_PREFIX
  #define TOPIC_PREFIX "wp"
#endif
