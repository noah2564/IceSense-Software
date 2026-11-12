# ICE-SENSE, edge-first iteration

ESP32 detects ice on an evaporator by itself (capacitive + resistive + coil temperature), shows it on its own web page for the
service technician, and can drive a defrost-request relay. A laptop is only needed for research (log, replay, tune).
Branch `edge-first`; `main` holds the first ESP32 + server iteration.

## Try it without hardware
```bash
pip install -r requirements-dev.txt
python -m simulator.device --speed 20            # open http://localhost:8080
python -m pytest tests                           # algorithm, C++ equivalence, simulator API, lab tools, firmware sync
firmware/hostcheck/run.sh                        # compiles the REAL firmware on the PC against stubs, checks behaviour + schema
```
## On the board
```bash
cd firmware && cp include/secrets.h.example include/secrets.h   # set UI password!
pio run -t upload && pio run -t uploadfs                         # firmware, then web page
```
Join WiFi `icesense-xxxx` (password in `secrets.h`), open http://192.168.4.1/ (user `service`). Clean and dry sensor -> "Capture baseline".

## Main board
Firmware targets the ESP32-S3-DevKitC-1-N8R8. Bus A (`Wire`, 400 kHz) connects the LM75B coil-temperature sensor
at `0x48` and MCP3426 resistive-channel ADC (expected `0x68`, scanned at boot). Bus B (`Wire1`, 400 kHz) connects
the FDC2214 at `0x2A` or `0x2B`; its address is probed at boot. FDC2214 SD is active HIGH, so GPIO4 is driven LOW
and allowed to settle for at least 2 ms before access. STATUS is polled and INTB is disabled.

| Signal | GPIO | Use |
|---|---:|---|
| Wire SDA / SCL | 2 / 1 | LM75B and MCP3426, 3.3 V logic |
| Wire1 SDA / SCL | 6 / 5 | FDC2214 |
| FDC2214 SD / INTB | 4 / 7 | Shutdown output / unused status input |
| Relay request | 36 | Active-HIGH BS107 gate; disabled by default |
| J3 ADC pins 2 / 4 | 19 / 20 | Defined but unused (USB D-/D+, ADC2) |
| Spare J6 | 3 | Defined but unused |

GPIO36 may be consumed by octal PSRAM on N8R8 modules. Confirm availability on the exact module before enabling the
relay output. See [docs/architecture.md](docs/architecture.md) for the full pin map and open hardware items.

The FDC2214 returns LC-tank frequency code, not capacitance. The existing code-delta feature is retained unchanged;
`deposit_pct`, `ice_pct`, and `mm_per_pct` must be recalibrated using real sensor data. The MCP3426 voltage is reported,
but resistance and `res_log10` remain unavailable on-device until the resistive front-end circuit is identified.

Open hardware items: identify the resistive excitation/reference/input channel; measure the FDC tank L/C, electrode
channel and channel timing/drive values; verify whether the FDC board powers itself and has pull-ups (its connector J1
provides +5 V while the chip limit is 3.6 V); verify main-board I2C pull-ups; confirm the MCP3426 address and FDC2214
INTB polarity; and confirm GPIO36 is free on the N8R8 module. Do not infer these from the current placeholders.

## Research phase
`docker compose -f lab/docker-compose.yml up -d`, set `MQTT_ENABLED 1`, then `python lab/logger.py`, later `python lab/replay.py data/windows.csv`.

Docs: `docs/architecture.md`, `docs/outputs.md`. Status: thresholds are placeholders, thickness uncalibrated, not yet run on hardware.
