// Minimal host-side stand-in for the Arduino-ESP32 core, only for compile/behaviour checks.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <algorithm>
typedef uint8_t byte;
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define ADC_11db 3
#define WL_CONNECTED 3
template <class T, class L, class H> T constrain(T x, L lo, H hi) { return x < lo ? (T)lo : (x > hi ? (T)hi : x); }

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
  bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
  bool concat(const char* p) { s += p; return true; }
  bool concat(const char* p, unsigned n) { s.append(p, n); return true; }
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
  String operator+(const char* o) const { return String(s + o); }
  String operator+(const String& o) const { return String(s + o.s); }
  String& operator+=(const char* c) { s += c; return *this; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  bool operator==(const char* o) const { return s == o; }
};

struct SerialStub {
  void begin(int) {}
  void println(const char* t) { fprintf(stderr, "[serial] %s\n", t); }
  void printf(const char* f, ...) { va_list a; va_start(a, f); fprintf(stderr, "[serial] "); vfprintf(stderr, f, a); va_end(a); }
};
extern SerialStub Serial;

// ---- fake clock & sensors, driven by the harness
extern uint32_t g_millis;
extern double g_ntc_mv, g_res_mv, g_touch;
inline uint32_t millis() { return g_millis; }
inline void delay(uint32_t ms) { g_millis += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void analogSetAttenuation(int) {}
inline uint32_t analogReadMilliVolts(int pin) { return (uint32_t)(pin == 7 || pin == 35 ? g_ntc_mv : g_res_mv); }
inline uint32_t touchRead(int) { return (uint32_t)g_touch; }

struct EspStub {
  String getChipModel() { return String("ESP32-S3"); }
  const char* getSdkVersion() { return "v5.1.4-host-stub"; }
  uint32_t getFreeHeap() { return 180000; }
  uint32_t getMinFreeHeap() { return 150000; }
  uint32_t getCpuFreqMHz() { return 240; }
  uint64_t getEfuseMac() { return 0xa4cf12345678ULL; }
  void restart() { fprintf(stderr, "[esp] restart requested\n"); }
};
extern EspStub ESP;
inline void configTime(long, int, const char*, const char* = nullptr) {}
