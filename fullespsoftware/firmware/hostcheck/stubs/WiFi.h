#pragma once
#include "Arduino.h"
#define WIFI_STA 1
#define WIFI_AP_STA 3
struct WiFiClient {};
struct WiFiStub {
  int status() { return WL_CONNECTED; }
  int RSSI() { return -61; }
  void mode(int) {}
  void setAutoReconnect(bool) {}
  void begin(const char*, const char*) {}
  void softAP(const char*, const char*) {}
  void disconnect() {}
};
extern WiFiStub WiFi;
