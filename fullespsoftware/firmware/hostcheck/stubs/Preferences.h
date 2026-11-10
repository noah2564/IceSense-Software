#pragma once
#include <map>
#include "Arduino.h"
class Preferences {
  std::map<std::string, double> m_;
 public:
  bool begin(const char*, bool) { return true; }
  uint32_t getUInt(const char* k, uint32_t d) { return m_.count(k) ? (uint32_t)m_[k] : d; }
  float getFloat(const char* k, float d) { return m_.count(k) ? (float)m_[k] : d; }
  uint64_t getULong64(const char* k, uint64_t d) { return m_.count(k) ? (uint64_t)m_[k] : d; }
  void putUInt(const char* k, uint32_t v) { m_[k] = v; }
  void putFloat(const char* k, float v) { m_[k] = v; }
  void putULong64(const char* k, uint64_t v) { m_[k] = (double)v; }
};
