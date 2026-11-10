# Architecture (edge-first hybrid)

```
 ESP32 (works alone)                               laptop, research phase only
 sensor task (core 1): touch / resistive / NTC     lab/logger.py   MQTT -> CSV + JSONL (schema-validated)
   -> window (2 s) -> queue                        lab/replay.py   re-run algorithm/icedetect.py on recorded data,
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

## HTTP API
`GET /` page, `GET /api/state`, `GET /api/history.csv`, `POST /api/baseline`, `POST /api/params` (partial JSON), `POST /api/params/defaults`,
`POST /api/relay {"enabled":bool}`. HTTP basic auth (`UI_USER`/`UI_PASS` in `secrets.h`). Plain HTTP: the access point is protected by WPA2, there is no TLS.

## MQTT commands (optional)
`set_config` (`config.algorithm.parameters`, `config.reporting.telemetry_interval_s` = window length 1-60 s), `capture_baseline`,
`set_output` (`params: {output:"relay", enabled:bool}`), `get_status`, `reboot`.

## Not done / open
Thresholds are placeholders. Thickness is uncalibrated. Resistive front end is the simple 1 Mohm divider (dry fins read "open"). Single sensor only.
Firmware was compiled and exercised on a PC against stubs, **not** built with PlatformIO or run on a board.
