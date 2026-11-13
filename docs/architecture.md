# Architecture (edge-first hybrid)

```
 ESP32-S3-DevKitC-1-N8R8 (works alone)             laptop, research phase only
 sensor task (core 1): Wire LM75B + MCP3426,       lab/logger.py   MQTT -> CSV + JSONL (schema-validated)
   Wire1 FDC2214 -> window (2 s) -> queue           lab/replay.py   re-run algorithm/icedetect.py on recorded data,
 main loop: icedetect -> state, thickness,                          fit mm_per_pct from reference measurements
   defrost request -> relay (default off)                  |
   web page + API (LittleFS gz, basic auth)       tune in Python, regenerate algorithm/testvectors.csv,
   optional MQTT (same schema)  ------------------>        port to firmware/lib/icedetect (C++)
```

* **One algorithm, two languages.** `algorithm/icedetect.py` is the reference. `firmware/lib/icedetect` is a line-by-line port.
  `algorithm/testvectors.csv` (generated from Python) is replayed through the C++ port by `tests/test_cpp_equivalence.py`.
* **One schema.** `schema/ice-sensing.schema.jsonc` (commented source) -> `.json` via `build_schema.py`. MQTT messages carry `ice_assessment`.
* **Same UI everywhere.** `firmware/web/index.html` is served by the ESP32 and by `python -m simulator.device`, same HTTP API.
* **Decision:** the page polls `/api/state` every second instead of WebSocket/SSE (simpler, robust on the ESP32 WebServer). Revisit if latency matters.

## Main board and sensors
The target is ESP32-S3-DevKitC-1-N8R8. Both 3.3 V I2C buses run initially at 400 kHz; startup scans log discovered
addresses. Scanning and all sensor transactions run in the sensor task. FDC2214 address `0x2A` and `0x2B` are both
probed. Its GPIO4 SD input is active HIGH shutdown: firmware holds it LOW and waits at least 2 ms before I2C. Firmware
polls STATUS and disables INTB because its polarity is not confirmed.

| Bus / signal | GPIO | Device or purpose |
|---|---:|---|
| Wire SDA / SCL | 2 / 1 | LM75B `0x48`; MCP3426 expected `0x68` (CH1 front, CH2 back differential voltage) |
| Wire1 SDA / SCL | 6 / 5 | FDC2214 `0x2A` or `0x2B` (LC-tank frequency code) |
| FDC2214 SD / INTB | 4 / 7 | Active-HIGH shutdown / unused input; STATUS polling instead |
| Relay request | 36 | Active-HIGH BS107 gate; relay output remains disabled by default |
| J3 ADC pins 2 / 4 | 19 / 20 | Defined but intentionally unused: USB D-/D+ and ADC2 |
| Spare J6 | 3 | Defined but unused |

GPIO36 is used as drawn, but may be allocated to octal PSRAM on some N8R8 modules. Verify the exact module pinout
before enabling the output. The thermistor and touch-pad acquisition have been removed.

The FDC2214 measures LC-tank frequency code. Because tank capacitance is proportional to $1/f^2$ and frequency is
proportional to code, firmware reports total-tank capacitance change as:

$$\Delta C_{pct}=\left[\left(\frac{code_{base}}{code_{window}}\right)^2-1\right]\,100$$

This estimate does not need L or reference frequency. `deposit_pct`, `ice_pct`, and `mm_per_pct` remain placeholders
and must be calibrated against real data. The optional absolute capacitance conversion remains disabled until tank
values are known.

The resistive front end is now modeled as 3.3 V through 1 MOhm to each sensor node, with the sensor on the low side
to ground and a shared 40 kOhm / 40 kOhm midpoint reference. MCP3426 CH1 (code channel 0) reads front node minus
reference; CH2 (code channel 1) reads back node minus reference. Both are sampled one-shot at 14-bit / 60 SPS and
reported as `res-1` and `res-2`. Firmware averages differential voltage per window and performs one log-domain
bisection inversion per side using the divider plus MCP3426 differential/common-mode input impedances. The detector
uses the lower side resistance as `res_log10`; this side-combination rule is explicitly provisional.

Above about 10 MOhm, inferred resistance is approximate because MCP3426 input impedance and tolerance matter; treat
that range as dry. The supply is nominally 3.3 V until measured. Bring-up should substitute known 100 kOhm, 1 MOhm,
and 10 MOhm resistors for both sensors and tune the input-impedance model only if those measurements disagree.

## Hardware TODOs
- Measure the board 3.3 V supply and replace the nominal `RES_VDD_V` value.
- Confirm the MCP3426 address (`0x68` is expected; firmware probes `0x68` through `0x6F`) and verify CH1/front and CH2/back wiring.
- Validate the front/back combination rule: firmware currently selects the lower resistance as the wetter side.
- Validate the MCP3426 input-impedance model with known resistors, particularly above 10 MOhm.
- Measure the FDC2214 tank inductor/capacitor, the electrode's actual channel, and the dependent RCOUNT, SETTLECOUNT, clock-divider and drive-current values. They remain unprogrammed placeholders.
- Verify whether the FDC board has its own 3.3 V regulator and pull-ups. Connector J1 supplies +5 V while the FDC2214 chip allows at most 3.6 V.
- Check for I2C pull-ups; none are visible on the main-board schematic.
- Verify the MCP3426 address (`0x68` is expected, not confirmed) and FDC2214 INTB polarity. Firmware scans addresses and polls STATUS instead of relying on INTB.
- Confirm GPIO36 availability on the exact ESP32-S3-N8R8 module before enabling the relay output.
- Calibrate `deposit_pct`, `ice_pct`, and `mm_per_pct` against real FDC2214 data; existing values are placeholders.

## HTTP API
`GET /` page, `GET /api/state`, `GET /api/history.csv`, `POST /api/baseline`, `POST /api/params` (partial JSON), `POST /api/params/defaults`,
`POST /api/relay {"enabled":bool}`. HTTP basic auth (`UI_USER`/`UI_PASS` in `secrets.h`). Plain HTTP: the access point is protected by WPA2, there is no TLS.

## MQTT commands (optional)
`set_config` (`config.algorithm.parameters`, `config.reporting.telemetry_interval_s` = window length 1-60 s), `capture_baseline`,
`set_output` (`params: {output:"relay", enabled:bool}`), `get_status`, `reboot`.

## Not done / open
Thresholds are placeholders and thickness is uncalibrated. Sensor acquisition has passed host simulation and the
ESP32-S3 PlatformIO build, but the analog chains have not yet been characterized on the assembled board.
