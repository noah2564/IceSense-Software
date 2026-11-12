#include "fdc2214.h"

#include <Arduino.h>
#include <math.h>

namespace {
constexpr uint8_t REG_DATA_CH0_MSB = 0x00;
constexpr uint8_t REG_DATA_CH0_LSB = 0x01;
constexpr uint8_t REG_RCOUNT_CH0 = 0x08;
constexpr uint8_t REG_SETTLECOUNT_CH0 = 0x10;
constexpr uint8_t REG_CLOCK_DIVIDERS_CH0 = 0x14;
constexpr uint8_t REG_STATUS = 0x18;
constexpr uint8_t REG_ERROR_CONFIG = 0x19;
constexpr uint8_t REG_CONFIG = 0x1A;
constexpr uint8_t REG_MUX_CONFIG = 0x1B;
constexpr uint8_t REG_RESET_DEV = 0x1C;
constexpr uint8_t REG_DRIVE_CURRENT_CH0 = 0x1E;
constexpr uint8_t REG_MANUFACTURER_ID = 0x7E;
constexpr uint8_t REG_DEVICE_ID = 0x7F;
constexpr uint16_t STATUS_ERR_WD = 1u << 12;
constexpr uint16_t STATUS_CH0_UNREAD_CONVERSION = 1u << 3;
constexpr uint16_t DATA_MSB_MASK = 0x0FFF;
constexpr uint32_t CODE_MASK = 0x0FFFFFFF;
constexpr double PI_VALUE = 3.14159265358979323846;
}

bool FDC2214::begin(const Config& config, uint16_t i2cTimeoutMs, uint16_t powerupDelayMs,
                    uint16_t resetDelayMs, Error& error) {
  wire_.setTimeOut(i2cTimeoutMs);
  pinMode(shutdownPin_, OUTPUT);
  digitalWrite(shutdownPin_, HIGH);
  delay(1);
  digitalWrite(shutdownPin_, LOW);
  delay(powerupDelayMs < 2 ? 2 : (powerupDelayMs > 5 ? 5 : powerupDelayMs));

  if (!writeRegister(REG_RESET_DEV, 0x8000, error)) return false;
  delay(resetDelayMs < 2 ? 2 : (resetDelayMs > 5 ? 5 : resetDelayMs));

  uint16_t id = 0;
  if (!readRegister(REG_MANUFACTURER_ID, id, error)) return false;
  if (id != 0x5449) {
    error = Error::ManufacturerMismatch;
    return false;
  }
  if (!readRegister(REG_DEVICE_ID, id, error)) return false;
  if (id != 0x3055) {
    error = Error::DeviceMismatch;
    return false;
  }

  if (!writeRegister(REG_ERROR_CONFIG, config.errorConfig, error) ||
      !writeRegister(REG_MUX_CONFIG, config.muxConfig, error) ||
      !writeRegister(REG_CONFIG, config.config, error)) return false;

  if (config.tankSettingsValid) {
    if (config.rcount == 0 || config.settleCount == 0 || config.clockDividers == 0) {
      error = Error::InvalidConfig;
      return false;
    }
    if (!writeRegister(REG_RCOUNT_CH0, config.rcount, error) ||
        !writeRegister(REG_SETTLECOUNT_CH0, config.settleCount, error) ||
        !writeRegister(REG_CLOCK_DIVIDERS_CH0, config.clockDividers, error) ||
        !writeRegister(REG_DRIVE_CURRENT_CH0, config.driveCurrent, error)) return false;
  }
  error = Error::None;
  return true;
}

bool FDC2214::readCode(uint32_t timeoutMs, uint32_t& code, Error& error) {
  const uint32_t started = millis();
  uint16_t status = 0;
  for (;;) {
    if (!readRegister(REG_STATUS, status, error)) return false;
    if (status & STATUS_ERR_WD) {
      error = Error::DeviceStatus;
      return false;
    }
    if (status & STATUS_CH0_UNREAD_CONVERSION) break;
    if ((uint32_t)(millis() - started) >= timeoutMs) {
      error = Error::Timeout;
      return false;
    }
    delay(1);
  }

  uint16_t msb = 0, lsb = 0;
  if (!readRegister(REG_DATA_CH0_MSB, msb, error) || !readRegister(REG_DATA_CH0_LSB, lsb, error)) return false;
  if (msb & 0x3000) {
    error = Error::DeviceStatus;
    return false;
  }
  code = ((((uint32_t)msb & DATA_MSB_MASK) << 16) | lsb) & CODE_MASK;
  error = Error::None;
  return true;
}

bool FDC2214::codeToFrequency(uint32_t code, float referenceClockHz, uint16_t referenceDivider,
                              uint8_t inputDivider, float& frequencyHz, Error& error) {
  if (code > CODE_MASK || !(referenceClockHz > 0.0f) || referenceDivider == 0 || inputDivider == 0) {
    error = Error::InvalidConfig;
    return false;
  }
  frequencyHz = (float)(((double)code * referenceClockHz * inputDivider) /
                        (268435456.0 * referenceDivider));
  error = Error::None;
  return true;
}

bool FDC2214::codeToCapacitancePf(uint32_t code, float referenceClockHz, uint16_t referenceDivider,
                                  uint8_t inputDivider, float inductanceUh, float tankCapacitancePf,
                                  float& capacitancePf, Error& error) {
  if (!(inductanceUh > 0.0f) || tankCapacitancePf < 0.0f) {
    error = Error::InvalidConfig;
    return false;
  }
  float frequencyHz;
  if (!codeToFrequency(code, referenceClockHz, referenceDivider, inputDivider, frequencyHz, error)) return false;
  if (!(frequencyHz > 0.0f)) { error = Error::InvalidConfig; return false; }
  const double inductanceH = inductanceUh * 1.0e-6;
  capacitancePf = (float)(1.0e12 / (4.0 * PI_VALUE * PI_VALUE * inductanceH * frequencyHz * frequencyHz) - tankCapacitancePf);
  if (!isfinite(capacitancePf) || capacitancePf < 0.0f) {
    error = Error::InvalidConfig;
    return false;
  }
  error = Error::None;
  return true;
}

bool FDC2214::writeRegister(uint8_t reg, uint16_t value, Error& error) {
  wire_.beginTransmission(address_);
  wire_.write(reg);
  wire_.write((uint8_t)(value >> 8));
  wire_.write((uint8_t)value);
  if (wire_.endTransmission() != 0) {
    error = Error::I2c;
    return false;
  }
  error = Error::None;
  return true;
}

bool FDC2214::readRegister(uint8_t reg, uint16_t& value, Error& error) {
  wire_.beginTransmission(address_);
  wire_.write(reg);
  if (wire_.endTransmission(false) != 0 || wire_.requestFrom(address_, (uint8_t)2) != 2) {
    error = Error::I2c;
    return false;
  }
  value = ((uint16_t)wire_.read() << 8) | (uint8_t)wire_.read();
  error = Error::None;
  return true;
}