// Recording stand-in for PubSubClient: prints every publish as "TOPIC<TAB>PAYLOAD" to stdout.
#pragma once
#include "Arduino.h"
#include "WiFi.h"
extern bool g_broker_up;
class PubSubClient {
  bool connected_ = false;
  void (*cb_)(char*, byte*, unsigned int) = nullptr;
 public:
  PubSubClient(WiFiClient&) {}
  void setServer(const char*, int) {}
  void setCallback(void (*cb)(char*, byte*, unsigned int)) { cb_ = cb; }
  bool setBufferSize(unsigned) { return true; }
  void setKeepAlive(int) {}
  void setSocketTimeout(int) {}
  bool connect(const char*, const char*, const char*, const char*, int, bool, const char*) {
    connected_ = g_broker_up; return connected_; }
  bool connected() { if (!g_broker_up) connected_ = false; return connected_; }
  int state() { return -2; }
  bool publish(const char* t, const char* p, bool retained = false) {
    if (!connected()) return false;
    if (!retained) printf("%s\t%s\n", t, p); else fprintf(stderr, "[retained] %s=%s\n", t, p);
    return true; }
  bool subscribe(const char*, int = 0) { return true; }
  bool loop() { return connected(); }
  void disconnect() { connected_ = false; }
  void inject(const char* payload) { if (cb_) cb_((char*)"cmd", (byte*)payload, strlen(payload)); }
};
