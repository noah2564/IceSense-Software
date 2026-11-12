#include "lm75b.h"

#include <math.h>

bool LM75B::begin(uint16_t timeoutMs, Error& error) {
  wire_.setTimeOut(timeoutMs);
  float temperatureC;
  return readTemperatureC(temperatureC, error);
}

bool LM75B::readTemperatureC(float& temperatureC, Error& error) {
  wire_.beginTransmission(address_);
  wire_.write((uint8_t)0x00);
  if (wire_.endTransmission(false) != 0) {
    error = Error::I2c;
    return false;
  }
  if (wire_.requestFrom(address_, (uint8_t)2) != 2) {
    error = Error::I2c;
    return false;
  }
  const uint16_t raw = ((uint16_t)wire_.read() << 8) | (uint8_t)wire_.read();
  return codeToC(raw, temperatureC, error);
}

bool LM75B::codeToC(uint16_t rawRegister, float& temperatureC, Error& error) {
  int16_t code = (int16_t)(rawRegister >> 5);
  if (code & 0x0400) code |= (int16_t)0xF800;
  temperatureC = code * 0.125f;
  if (!isfinite(temperatureC)) {
    error = Error::InvalidData;
    return false;
  }
  error = Error::None;
  return true;
}