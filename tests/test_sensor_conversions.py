import os
import shutil
import subprocess

import pytest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, "firmware")


@pytest.mark.skipif(shutil.which("g++") is None, reason="g++ not installed")
def test_sensor_conversion_helpers(tmp_path):
    source = tmp_path / "sensor_conversions.cpp"
    source.write_text(r'''
#include <cmath>
#include <Arduino.h>
#include "config.h"
#include "lm75b.h"
#include "mcp3426.h"
#include "fdc2214.h"
#include "resistive_model.h"

uint32_t g_millis = 0;
double g_temperature_c = 0, g_cap_delta_pct = 0, g_trace_log10r = 0;
bool g_fdc_high_address = false;

#if FDC2214_USE_EXTERNAL_CLKIN
static_assert(FDC2214_CONFIG_CH0 == 0x1681u, "external-clock FDC2214 CONFIG mismatch");
#else
static_assert(FDC2214_CONFIG_CH0 == 0x1481u, "internal-clock FDC2214 CONFIG mismatch");
#endif

int main() {
  float value = 0;
  LM75B::Error te;
  if (!LM75B::codeToC(0x1900, value, te) || value != 25.0f) return 1;
  if (!LM75B::codeToC(0xF600, value, te) || value != -10.0f) return 2;

  int32_t code = 0;
  MCP3426::Error me;
  if (!MCP3426::decodeCode(0x1FFF, 14, code, me) || code != 8191) return 3;
  if (!MCP3426::decodeCode(0x2000, 14, code, me) || code != -8192) return 4;
  if (!MCP3426::decodeCode(0xFFFB, 14, code, me) || code != -5) return 5;
  if (!MCP3426::decodeCode(0x0001, 14, code, me) || code != 1) return 6;
  if (!MCP3426::decodeCode(0x07FF, 12, code, me) || code != 2047) return 7;
  if (!MCP3426::decodeCode(0x0800, 12, code, me) || code != -2048) return 8;
  if (!MCP3426::decodeCode(0x0FFB, 12, code, me) || code != -5) return 9;
  if (!MCP3426::decodeCode(0x0001, 12, code, me) || code != 1) return 10;
  if (!MCP3426::decodeCode(0x7FFF, 16, code, me) || code != 32767) return 11;
  if (!MCP3426::decodeCode(0x8000, 16, code, me) || code != -32768) return 12;
  if (!MCP3426::decodeCode(0xFFFB, 16, code, me) || code != -5) return 13;
  if (!MCP3426::decodeCode(0x0001, 16, code, me) || code != 1) return 14;
  if (!MCP3426::codeToVolts(-8192, 14, 1, 2.048f, value, me) || value != -2.048f) return 15;

    resistive_model::Model model{RES_VDD_V, RES_R_FIXED_OHM, RES_R_REF_TOP_OHM,
                   RES_R_REF_BOTTOM_OHM, MCP3426_ZIN_DIFF_OHM_X1,
                   MCP3426_ZIN_CM_OHM, RES_VDIFF_OFFSET_V,
                   RES_MIN_OHM, RES_MAX_OHM};
    const double knownResistances[] = {1e3, 1e4, 1e5, 3e5, 1e6, 3e6, 1e7, 1e8, 1e9};
    for (size_t index = 0; index < sizeof(knownResistances) / sizeof(knownResistances[0]); index++) {
    double measuredVoltage = 0;
    resistive_model::Estimate estimate;
    if (!resistive_model::voltageDiffForResistance(knownResistances[index], 1, model, measuredVoltage) ||
      !resistive_model::resistanceFromVoltageDiff(measuredVoltage, 1, model, estimate) ||
      std::fabs(std::log10(estimate.resistanceOhm / knownResistances[index])) > 1e-9 ||
      estimate.openCircuit != (knownResistances[index] == RES_MAX_OHM) ||
      estimate.shortCircuit != (knownResistances[index] == RES_MIN_OHM)) return 16;
    }
    resistive_model::Model idealModel = model;
    idealModel.differentialInputOhmAtGain1 = 1e30;
    idealModel.commonModeInputOhm = 1e30;
    double idealVoltage = 0;
    if (!resistive_model::voltageDiffForResistance(1e6, 1, idealModel, idealVoltage) ||
      std::fabs(idealVoltage) > 1e-12) return 17;
    if (!resistive_model::voltageDiffForResistance(1e5, 1, idealModel, idealVoltage) ||
      std::fabs(idealVoltage - (RES_VDD_V * 1e5 / (1e5 + RES_R_FIXED_OHM) - RES_VDD_V / 2.0)) > 1e-12) return 18;
    double limitVoltage = 0;
    resistive_model::Estimate limitEstimate;
    if (!resistive_model::voltageDiffForResistance(RES_MAX_OHM, 1, model, limitVoltage) ||
      !resistive_model::resistanceFromVoltageDiff(limitVoltage, 1, model, limitEstimate) ||
      !limitEstimate.openCircuit || limitEstimate.resistanceOhm != RES_MAX_OHM) return 19;
    if (!resistive_model::voltageDiffForResistance(RES_MIN_OHM, 1, model, limitVoltage) ||
      !resistive_model::resistanceFromVoltageDiff(limitVoltage, 1, model, limitEstimate) ||
      !limitEstimate.shortCircuit || limitEstimate.resistanceOhm != RES_MIN_OHM) return 20;

  FDC2214::Error fe;
    if (!FDC2214::codeToFrequency(0x08000000, 40000000.0f, 1, 1, value, fe) || value != 20000000.0f) return 21;
    if (FDC2214::codeToFrequency(0x10000000, 40000000.0f, 1, 1, value, fe)) return 22;
    if (!FDC2214::codeToCapacitancePf(0x08000000, 40000000.0f, 1, 1, 100.0f, 0.1f, value, fe) ||
        value < 0.53f || value > 0.54f) return 23;
    if (FDC2214::codeToCapacitancePf(0x08000000, 40000000.0f, 1, 1, 0.0f, 0.0f, value, fe)) return 24;

  TwoWire unconfiguredBus(true);
  FDC2214 unconfiguredFdc(unconfiguredBus, FDC2214_I2C_ADDRESS_LOW, PIN_FDC2214_SD);
  FDC2214::Config unconfigured{0, 0, 0, 0, FDC2214_CONFIG_CH0, FDC2214_MUX_CONFIG_CH0,
                               FDC2214_ERROR_CONFIG, false};
  if (unconfiguredFdc.begin(unconfigured, 20, 2, 2, fe) || fe != FDC2214::Error::NotConfigured ||
      unconfiguredBus.registerWriteCount() != 1 || unconfiguredBus.registerWriteAt(0) != 0x1C) return 25;

  TwoWire configuredBus(true);
  FDC2214 configuredFdc(configuredBus, FDC2214_I2C_ADDRESS_LOW, PIN_FDC2214_SD);
  FDC2214::Config configured{0x0100, 0x000A, 0x1001, 0, FDC2214_CONFIG_CH0,
                             FDC2214_MUX_CONFIG_CH0, FDC2214_ERROR_CONFIG, true};
  if (!configuredFdc.begin(configured, 20, 2, 2, fe) || fe != FDC2214::Error::None) return 26;
  const uint8_t expectedWrites[] = {0x1C, 0x19, 0x1B, 0x08, 0x10, 0x14, 0x1E, 0x1A};
  if (configuredBus.registerWriteCount() != sizeof(expectedWrites)) return 27;
  for (size_t index = 0; index < sizeof(expectedWrites); index++)
    if (configuredBus.registerWriteAt(index) != expectedWrites[index]) return 28;

  configured.clockDividers = 0x2001;
  TwoWire finSelectTenBus(true);
  FDC2214 finSelectTenFdc(finSelectTenBus, FDC2214_I2C_ADDRESS_LOW, PIN_FDC2214_SD);
  if (!finSelectTenFdc.begin(configured, 20, 2, 2, fe)) return 29;

  configured.rcount = 0x00FF;
  TwoWire lowRcountBus(true);
  FDC2214 lowRcountFdc(lowRcountBus, FDC2214_I2C_ADDRESS_LOW, PIN_FDC2214_SD);
  if (lowRcountFdc.begin(configured, 20, 2, 2, fe) || fe != FDC2214::Error::InvalidConfig) return 30;

  configured.rcount = 0x0100;
  const uint16_t invalidDividers[] = {0x0001, 0x3001, 0x1000};
  for (size_t index = 0; index < sizeof(invalidDividers) / sizeof(invalidDividers[0]); index++) {
    configured.clockDividers = invalidDividers[index];
    TwoWire invalidDividerBus(true);
    FDC2214 invalidDividerFdc(invalidDividerBus, FDC2214_I2C_ADDRESS_LOW, PIN_FDC2214_SD);
    if (invalidDividerFdc.begin(configured, 20, 2, 2, fe) || fe != FDC2214::Error::InvalidConfig) return 31;
  }
  return 0;
}
''', encoding="utf-8")
    exe = str(tmp_path / "sensor_conversions")
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra",
        "-I" + os.path.join(FIRMWARE, "hostcheck", "stubs"),
        "-I" + os.path.join(FIRMWARE, "include"),
        "-I" + os.path.join(FIRMWARE, "lib", "lm75b"),
        "-I" + os.path.join(FIRMWARE, "lib", "mcp3426"),
        "-I" + os.path.join(FIRMWARE, "lib", "fdc2214"),
        str(source),
        os.path.join(FIRMWARE, "lib", "lm75b", "lm75b.cpp"),
        os.path.join(FIRMWARE, "lib", "mcp3426", "mcp3426.cpp"),
        os.path.join(FIRMWARE, "lib", "fdc2214", "fdc2214.cpp"),
        "-DCONFIG_IDF_TARGET_ESP32S3=1", "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True)
    external_exe = str(tmp_path / "sensor_conversions_external_clock")
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra",
        "-I" + os.path.join(FIRMWARE, "hostcheck", "stubs"),
        "-I" + os.path.join(FIRMWARE, "include"),
        "-I" + os.path.join(FIRMWARE, "lib", "lm75b"),
        "-I" + os.path.join(FIRMWARE, "lib", "mcp3426"),
        "-I" + os.path.join(FIRMWARE, "lib", "fdc2214"),
        str(source),
        os.path.join(FIRMWARE, "lib", "lm75b", "lm75b.cpp"),
        os.path.join(FIRMWARE, "lib", "mcp3426", "mcp3426.cpp"),
        os.path.join(FIRMWARE, "lib", "fdc2214", "fdc2214.cpp"),
        "-DCONFIG_IDF_TARGET_ESP32S3=1", "-DFDC2214_USE_EXTERNAL_CLKIN=1", "-o", external_exe,
    ], check=True)
    subprocess.run([external_exe], check=True)