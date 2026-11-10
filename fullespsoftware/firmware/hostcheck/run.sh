#!/usr/bin/env bash
# Compiles the REAL firmware/src/main.cpp on your PC against fake hardware (no ESP32, no PlatformIO),
# replays one synthetic frost/defrost cycle, issues HTTP requests and MQTT commands, then checks the
# outcome and validates every MQTT message against the JSON schema.
# Needs: g++ (C++17), git, python3 with `pip install jsonschema`.
set -euo pipefail
cd "$(dirname "$0")"
[ -d .deps/ArduinoJson ] || git clone -q --depth 1 --branch v7.2.0 https://github.com/bblanchon/ArduinoJson.git .deps/ArduinoJson
python3 trace.py
for MQ in 1 0; do
  g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -DARDUINOJSON_USE_LONG_LONG=1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DCONFIG_IDF_TARGET_ESP32S3=1 -DMQTT_ENABLED=$MQ \
    -Istubs -I.deps/ArduinoJson/src -I../src -I../include -I../lib/icedetect harness.cpp ../lib/icedetect/icedetect.cpp -o .harness$MQ
done
./.harness1 > out.tsv 2> err.log
./.harness0 > /dev/null 2> err0.log      # same firmware with MQTT compiled out must also build and run
python3 check_out.py
