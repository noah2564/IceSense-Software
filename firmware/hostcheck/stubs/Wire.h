#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

extern double g_temperature_c;
extern double g_cap_delta_pct;
extern double g_trace_log10r;
extern bool g_fdc_high_address;

class TwoWire {
 public:
  explicit TwoWire(bool secondBus = false) : secondBus_(secondBus) {}
  void setTimeOut(uint16_t) {}
  void begin(int = -1, int = -1, uint32_t = 0) {}
  void beginTransmission(uint8_t address) { address_ = address; txCount_ = 0; }
  size_t write(uint8_t value) { if (txCount_ < sizeof(tx_)) tx_[txCount_++] = value; return 1; }
  uint8_t endTransmission(bool = true) {
    if (address_ == 0x00 && txCount_ == 1 && tx_[0] == 0x06) return 0;
    if (!present(address_)) return 2;
    if (txCount_ == 0) return 0;
    if (secondBus_) {
      if (txCount_ == 1) register_ = tx_[0];
      else if (txCount_ >= 3) {
        register_ = tx_[0];
        registers_[register_] = (uint16_t)((uint16_t)tx_[1] << 8) | tx_[2];
      }
    } else if (address_ == 0x68 && txCount_ == 1) {
      mcpConfig_ = tx_[0];
    } else {
      register_ = tx_[0];
    }
    return 0;
  }
  uint8_t requestFrom(uint8_t address, uint8_t count) {
    rxPos_ = rxCount_ = 0;
    if (!present(address) || address != address_) return 0;
    if (secondBus_) fillFdc(count);
    else if (address == 0x48) fillLm75(count);
    else if (address == 0x68) fillMcp(count);
    return rxCount_;
  }
  int available() { return rxCount_ - rxPos_; }
  int read() { return rxPos_ < rxCount_ ? rx_[rxPos_++] : -1; }

 private:
  bool present(uint8_t address) const {
    if (secondBus_) return address == 0x2A || (address == 0x2B && g_fdc_high_address);
    return address == 0x48 || address == 0x68;
  }
  void push(uint8_t value) { if (rxCount_ < sizeof(rx_)) rx_[rxCount_++] = value; }
  void pushWord(uint16_t value) { push((uint8_t)(value >> 8)); push((uint8_t)value); }
  void fillLm75(uint8_t count) {
    if (register_ != 0 || count != 2) return;
    int16_t code = (int16_t)std::lround(g_temperature_c * 8.0);
    pushWord((uint16_t)code << 5);
  }
  void fillMcp(uint8_t count) {
    if (count != 3) return;
    const double volts = 0.5 + (g_trace_log10r - 5.0) * 0.25;
    const int32_t code = (int32_t)std::lround(volts * 8192.0 / 2.048);
    const uint16_t raw = (uint16_t)(((uint16_t)code & 0x3FFF) << 2);
    pushWord(raw);
    push((uint8_t)(mcpConfig_ & 0x7F));
  }
  void fillFdc(uint8_t count) {
    if (count != 2) return;
    uint16_t value = registers_[register_];
    if (register_ == 0x7E) value = 0x5449;
    else if (register_ == 0x7F) value = 0x3055;
    else if (register_ == 0x18) value = 0x0008;
    else if (register_ == 0x00) {
      const uint32_t code = (uint32_t)(20000000.0 * (1.0 - g_cap_delta_pct / 100.0));
      value = (uint16_t)((code >> 16) & 0x0FFF);
    } else if (register_ == 0x01) {
      const uint32_t code = (uint32_t)(20000000.0 * (1.0 - g_cap_delta_pct / 100.0));
      value = (uint16_t)code;
    }
    pushWord(value);
  }

  bool secondBus_;
  uint8_t address_ = 0, register_ = 0, tx_[4] = {}, txCount_ = 0;
  uint8_t rx_[3] = {}, rxCount_ = 0, rxPos_ = 0, mcpConfig_ = 0;
  uint16_t registers_[128] = {};
};

extern TwoWire Wire;
extern TwoWire Wire1;