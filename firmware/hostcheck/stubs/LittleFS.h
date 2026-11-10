#pragma once
#include "Arduino.h"
struct File { bool ok = true; operator bool() const { return ok; } void close() {} };
struct LittleFSStub {
  bool begin(bool) { return true; }
  bool exists(const char*) { return true; }
  File open(const char*, const char*) { return File(); }
};
extern LittleFSStub LittleFS;
