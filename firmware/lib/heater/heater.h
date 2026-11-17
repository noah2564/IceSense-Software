#pragma once
// Board-heater thermostat: hysteresis + min on/off times + hard max + on-time limit with cooldown.
// Fail-safe: missing or stale reading -> heater OFF. Header-only, host-testable.
#include <stdint.h>
#include <math.h>

struct HeaterConfig {
  bool enabled;
  float onBelowC, offAboveC, hardMaxC;
  uint32_t minOnMs, minOffMs, maxOnMs, cooldownMs;
};

class HeaterThermostat {
 public:
  explicit HeaterThermostat(const HeaterConfig& c) : cfg_(c) {}
  bool update(uint32_t nowMs, bool tempValid, float tempC) {
    if (!cfg_.enabled || !tempValid || isnan(tempC) || tempC >= cfg_.hardMaxC ||
        !(cfg_.offAboveC > cfg_.onBelowC)) {
      setOn(false, nowMs);
      return on_;
    }
    if (on_) {
      const uint32_t onFor = nowMs - changedMs_;
      if (onFor >= cfg_.maxOnMs) {
        setOn(false, nowMs);
        cooldownUntilMs_ = nowMs + cfg_.cooldownMs;
        inCooldown_ = true;
      } else if (tempC >= cfg_.offAboveC && onFor >= cfg_.minOnMs) {
        setOn(false, nowMs);
      }
    } else {
      if (inCooldown_ && (int32_t)(nowMs - cooldownUntilMs_) >= 0) inCooldown_ = false;
      const bool offLongEnough = !everSwitched_ || (nowMs - changedMs_) >= cfg_.minOffMs;
      if (!inCooldown_ && tempC <= cfg_.onBelowC && offLongEnough) setOn(true, nowMs);
    }
    return on_;
  }
  bool isOn() const { return on_; }
 private:
  void setOn(bool on, uint32_t nowMs) {
    if (on != on_) { on_ = on; changedMs_ = nowMs; everSwitched_ = true; }
  }
  HeaterConfig cfg_;
  bool on_ = false, everSwitched_ = false, inCooldown_ = false;
  uint32_t changedMs_ = 0, cooldownUntilMs_ = 0;
};