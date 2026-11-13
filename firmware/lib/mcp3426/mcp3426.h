#pragma once

#include <Wire.h>
#include <cstdint>

class MCP3426 {
 public:
  enum class Error : uint8_t { None, I2c, Timeout, InvalidConfig, Saturated };

  MCP3426(TwoWire& wire, uint8_t address) : wire_(wire), address_(address) {}
  void setAddress(uint8_t address) { address_ = address; }
  uint8_t address() const { return address_; }
  bool begin(uint16_t timeoutMs, Error& error);
  bool reset(uint16_t timeoutMs, Error& error);
  bool readVoltage(uint8_t channel, uint8_t resolutionBits, uint8_t gain, bool continuous,
                   uint16_t timeoutMs, float referenceV, int32_t& code, float& volts,
                   bool& saturated, Error& error);
  static bool decodeCode(uint16_t raw, uint8_t resolutionBits, int32_t& code, Error& error);
  static bool codeToVolts(int32_t code, uint8_t resolutionBits, uint8_t gain, float referenceV,
                          float& volts, Error& error);

 private:
  TwoWire& wire_;
  uint8_t address_;
};