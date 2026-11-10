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

## Research phase
`docker compose -f lab/docker-compose.yml up -d`, set `MQTT_ENABLED 1`, then `python lab/logger.py`, later `python lab/replay.py data/windows.csv`.

Docs: `docs/architecture.md`, `docs/outputs.md`. Status: thresholds are placeholders, thickness uncalibrated, not yet run on hardware.
