#pragma once

#include <Wire.h>
#include <cstdint>

class FDC2214 {
 public:
  enum class Error : uint8_t { None, I2c, Timeout, ManufacturerMismatch, DeviceMismatch, DeviceStatus, InvalidConfig };
  struct Config {
    uint16_t rcount;
    uint16_t settleCount;
    uint16_t clockDividers;
    uint16_t driveCurrent;
    uint16_t config;
    uint16_t muxConfig;
    uint16_t errorConfig;
    bool tankSettingsValid;
  };

  FDC2214(TwoWire& wire, uint8_t address, uint8_t shutdownPin)
      : wire_(wire), address_(address), shutdownPin_(shutdownPin) {}
  void setAddress(uint8_t address) { address_ = address; }
  bool begin(const Config& config, uint16_t i2cTimeoutMs, uint16_t powerupDelayMs,
             uint16_t resetDelayMs, Error& error);
  bool readCode(uint32_t timeoutMs, uint32_t& code, Error& error);
  static bool codeToFrequency(uint32_t code, float referenceClockHz, uint16_t referenceDivider,
                              uint8_t inputDivider, float& frequencyHz, Error& error);
  static bool codeToCapacitancePf(uint32_t code, float referenceClockHz, uint16_t referenceDivider,
                                  uint8_t inputDivider, float inductanceUh, float tankCapacitancePf,
                                  float& capacitancePf, Error& error);

 private:
  bool writeRegister(uint8_t reg, uint16_t value, Error& error);
  bool readRegister(uint8_t reg, uint16_t& value, Error& error);

  TwoWire& wire_;
  uint8_t address_;
  uint8_t shutdownPin_;
};