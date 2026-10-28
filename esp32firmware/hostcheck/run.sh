#!/usr/bin/env bash
# Compiles the REAL firmware/src/main.cpp on your PC against fake hardware, runs 70 simulated
# seconds (broker down at first, commands injected, a sensor failure at the end) and validates
# every message it would have published against the JSON schema. No ESP32, no PlatformIO needed.
# Needs: g++ (C++17), git, python3 with `pip install jsonschema`.
set -euo pipefail
cd "$(dirname "$0")"
[ -d .deps/ArduinoJson ] || git clone -q --depth 1 --branch v7.2.0 https://github.com/bblanchon/ArduinoJson.git .deps/ArduinoJson
g++ -std=c++17 -O0 -Wall -Wextra -Wno-unused-parameter \
  -DARDUINOJSON_USE_LONG_LONG=1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DCONFIG_IDF_TARGET_ESP32S3=1 \
  -Istubs -I.deps/ArduinoJson/src -I../src -I../include harness.cpp -o .harness
./.harness > out.tsv 2> err.log
python3 check_out.py
