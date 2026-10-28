#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WIFI_STA 1
struct WiFiClient {};
struct WiFiStub {
  int status() { return WL_CONNECTED; }
  int RSSI() { return -61; }
  void mode(int) {}
  void setAutoReconnect(bool) {}
  void begin(const char*, const char*) {}
  void disconnect() {}
};
extern WiFiStub WiFi;
