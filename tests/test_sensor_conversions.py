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
#include "lm75b.h"
#include "mcp3426.h"
#include "fdc2214.h"

uint32_t g_millis = 0;
double g_temperature_c = 0, g_cap_delta_pct = 0, g_trace_log10r = 0;
bool g_fdc_high_address = false;

int main() {
  float value = 0;
  LM75B::Error te;
  if (!LM75B::codeToC(0x1900, value, te) || value != 25.0f) return 1;
  if (!LM75B::codeToC(0xF600, value, te) || value != -10.0f) return 2;

  int32_t code = 0;
  MCP3426::Error me;
  if (!MCP3426::decodeCode(0x7FFC, 14, code, me) || code != 8191) return 3;
  if (!MCP3426::decodeCode(0x8000, 14, code, me) || code != -8192) return 4;
  if (!MCP3426::codeToVolts(-8192, 14, 1, 2.048f, value, me) || value != -2.048f) return 5;

  FDC2214::Error fe;
  if (!FDC2214::codeToFrequency(0x08000000, 40000000.0f, 1, 1, value, fe) || value != 20000000.0f) return 6;
  if (FDC2214::codeToFrequency(0x10000000, 40000000.0f, 1, 1, value, fe)) return 7;
    if (!FDC2214::codeToCapacitancePf(0x08000000, 40000000.0f, 1, 1, 100.0f, 0.1f, value, fe) ||
            value < 0.53f || value > 0.54f) return 8;
    if (FDC2214::codeToCapacitancePf(0x08000000, 40000000.0f, 1, 1, 0.0f, 0.0f, value, fe)) return 9;
  return 0;
}
''', encoding="utf-8")
    exe = str(tmp_path / "sensor_conversions")
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra",
        "-I" + os.path.join(FIRMWARE, "hostcheck", "stubs"),
        "-I" + os.path.join(FIRMWARE, "lib", "lm75b"),
        "-I" + os.path.join(FIRMWARE, "lib", "mcp3426"),
        "-I" + os.path.join(FIRMWARE, "lib", "fdc2214"),
        str(source),
        os.path.join(FIRMWARE, "lib", "lm75b", "lm75b.cpp"),
        os.path.join(FIRMWARE, "lib", "mcp3426", "mcp3426.cpp"),
        os.path.join(FIRMWARE, "lib", "fdc2214", "fdc2214.cpp"),
        "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True)