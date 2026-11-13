#include "mcp3426.h"

#include <Arduino.h>
#include <math.h>

bool MCP3426::begin(uint16_t timeoutMs, Error& error) {
  return reset(timeoutMs, error);
}

bool MCP3426::reset(uint16_t timeoutMs, Error& error) {
  wire_.setTimeOut(timeoutMs);
  wire_.beginTransmission(0x00);
  wire_.write((uint8_t)0x06);
  if (wire_.endTransmission() != 0) {
    error = Error::I2c;
    return false;
  }
  error = Error::None;
  return true;
}

bool MCP3426::readVoltage(uint8_t channel, uint8_t resolutionBits, uint8_t gain, bool continuous,
                          uint16_t timeoutMs, float referenceV, int32_t& code, float& volts,
                          bool& saturated, Error& error) {
  if (channel > 1 || (resolutionBits != 12 && resolutionBits != 14 && resolutionBits != 16) ||
      (gain != 1 && gain != 2 && gain != 4 && gain != 8)) {
    error = Error::InvalidConfig;
    return false;
  }

  const uint8_t rate = resolutionBits == 12 ? 0 : (resolutionBits == 14 ? 1 : 2);
  const uint8_t config = (uint8_t)(0x80 | (channel << 5) | (continuous ? 0x10 : 0) | (rate << 2) |
                                   (gain == 1 ? 0 : (gain == 2 ? 1 : (gain == 4 ? 2 : 3))));
  wire_.beginTransmission(address_);
  wire_.write(config);
  if (wire_.endTransmission() != 0) {
    error = Error::I2c;
    return false;
  }

  const uint32_t started = millis();
  uint8_t high = 0, low = 0, status = 0;
  for (;;) {
    if (wire_.requestFrom(address_, (uint8_t)3) == 3) {
      high = (uint8_t)wire_.read();
      low = (uint8_t)wire_.read();
      status = (uint8_t)wire_.read();
      if ((status & 0x80) == 0) break;
    } else {
      error = Error::I2c;
      return false;
    }
    if ((uint32_t)(millis() - started) >= timeoutMs) {
      error = Error::Timeout;
      return false;
    }
    delay(1);
  }

  if (!decodeCode(((uint16_t)high << 8) | low, resolutionBits, code, error)) return false;
  const int32_t positiveLimit = (1L << (resolutionBits - 1)) - 1;
  const int32_t negativeLimit = -(1L << (resolutionBits - 1));
  saturated = code == positiveLimit || code == negativeLimit;
  if (!codeToVolts(code, resolutionBits, gain, referenceV, volts, error)) return false;
  error = saturated ? Error::Saturated : Error::None;
  return true;
}

bool MCP3426::decodeCode(uint16_t raw, uint8_t resolutionBits, int32_t& code, Error& error) {
  if (resolutionBits != 12 && resolutionBits != 14 && resolutionBits != 16) {
    error = Error::InvalidConfig;
    return false;
  }
  const uint32_t mask = (1UL << resolutionBits) - 1;
  uint32_t value = raw & mask;
  if (value & (1UL << (resolutionBits - 1))) value |= ~mask;
  code = (int32_t)value;
  error = Error::None;
  return true;
}

bool MCP3426::codeToVolts(int32_t code, uint8_t resolutionBits, uint8_t gain, float referenceV,
                          float& volts, Error& error) {
  if ((resolutionBits != 12 && resolutionBits != 14 && resolutionBits != 16) ||
      (gain != 1 && gain != 2 && gain != 4 && gain != 8) || !(referenceV > 0.0f)) {
    error = Error::InvalidConfig;
    return false;
  }
  const float fullScaleCode = (float)(1UL << (resolutionBits - 1));
  volts = ((float)code * referenceV) / (fullScaleCode * gain);
  error = Error::None;
  return true;
}