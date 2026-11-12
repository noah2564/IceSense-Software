#pragma once

#include <Wire.h>
#include <cstdint>

class LM75B {
 public:
  enum class Error : uint8_t { None, I2c, Timeout, InvalidData };

  LM75B(TwoWire& wire, uint8_t address) : wire_(wire), address_(address) {}
  bool begin(uint16_t timeoutMs, Error& error);
  bool readTemperatureC(float& temperatureC, Error& error);
  static bool codeToC(uint16_t rawRegister, float& temperatureC, Error& error);

 private:
  TwoWire& wire_;
  uint8_t address_;
};