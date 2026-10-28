# Ice sensing: first draft (ESP32 node + server)

A deliberately small end-to-end slice that both engineers can extend. The contract between the
two halves is `schema/ice-sensing.schema.json` (generated, see "Schema" below); neither side may
send anything it does not allow.

```
 ESP32 node                          Mosquitto                    Server (FastAPI + SQLite)
 sample 3 channels  ──telemetry──►  ice/v1/<id>/telemetry  ──►   validate → dedupe → store → assess
 window statistics  ──status/event►  ice/v1/<id>/{status,…}  ──►   REST API + dashboard (port 8000)
 RAM buffer offline ◄──cmd────────   ice/v1/<id>/cmd         ◄──   POST /api/devices/<id>/commands
                    ──command_ack─►  ice/v1/<id>/command_ack
                    ──online (LWT, retained "1"/"0")──────────►   online/offline per device
```

## Run it

**Without hardware (simulator):**
```bash
docker compose up -d                       # Mosquitto on :1883 (dev config: no auth, no TLS)
cd server && pip install -r requirements.txt
uvicorn app:app --port 8000                # dashboard: http://localhost:8000
python simulator.py --interval 2 --cycle 120   # in a second terminal: one frost/defrost cycle per 2 min
```
(Without Docker: `apt install mosquitto` / `brew install mosquitto` works the same.)

**With an ESP32:**
```bash
cd firmware
cp include/secrets.h.example include/secrets.h   # WiFi + broker address
pio run -t upload && pio device monitor          # PlatformIO, env esp32-s3 (or esp32dev)
```
Wiring is described in `firmware/include/config.h` (touch pad to a fin electrode; resistive divider
with 1 MΩ and 1 nF; 10k NTC divider). Press **Capture baseline** in the dashboard *while the coil is
clean and dry*. Until then the capacitive and resistive deltas are absent and quality says `baseline_invalid`.

**Tests:** `cd server && pip install -r requirements-dev.txt && pytest`
**Firmware logic check without hardware:** `firmware/hostcheck/run.sh` compiles the real `main.cpp`
on your PC against fake hardware and validates every message it emits against the schema.

## What is in each half, and why

| | Firmware | Server |
|---|---|---|
| Does | sample, aggregate per window, publish, buffer, obey commands | validate, store, evaluate, serve |
| Does not | decide "ice or not", store history | talk to sensors, touch device state except via commands |
| Language | C++ (Arduino framework, PlatformIO) | Python (FastAPI, paho-mqtt, SQLite) |

Key decisions:
- **MQTT, device opens the connection.** Works behind customer firewalls, no inbound ports on the device.
- **Topic = message_type**, and the server rejects a message whose `device_id` or `message_type` disagrees with its topic.
- **Window aggregates, not raw samples.** 10 Hz sampling, one message per window (default 10 s). Raw
  waveforms stay available through the schema for later on-demand requests.
- **Evaluation lives on the server** (`server/evaluate.py`, placeholder thresholds) so it can change
  without touching deployed devices. The schema's `ice_assessment` block is there for later on-device use.
- **Server stores the original JSON *and* a flattened samples table.** Any numeric field added to the
  schema becomes chartable without a server change.
- **Invalid messages are kept in a `rejected` table** and shown in the dashboard, so contract
  violations are visible instead of silently lost.
- **Idempotent commands:** unique ids, expiry, and an ack for every command.

## Verified and not verified

Verified here: server unit/API tests (13 pass); a full round trip with a real Mosquitto broker
(simulator → server → API, a command sent via REST and acked, assessment going clear → frost → ice → wet → clear);
the firmware's message building, buffering and command handling compiled and run on a PC against fake
hardware, all 22 messages schema-valid.

**Not verified:** the firmware has never run on a real ESP32 and was not built with PlatformIO
(its toolchain download was blocked in the sandbox). Expect small first-build fixes. The sensor
drivers are untested against real electrodes, and the ice thresholds are not calibrated on real coils.

## Known limitations (the obvious next steps)

Firmware
1. Single thread: a blocking MQTT connect (up to 5 s) leaves a gap in sampling. Move networking to its own FreeRTOS task.
2. Plain DC excitation of the resistive electrode. Add polarity alternation to avoid electrode polarisation/corrosion.
3. Touch-pad counts are not capacitance. The `esp32_touch` front end gives relative change only, and the count direction depends on the chip. Move to an FDC1004/FDC2214 (pF) for real work.
4. Resistive range is about 10 kΩ to 10 MΩ. Dry fins read as `open_circuit`. A charge-time or high-impedance front end is needed for the dry/wet/frozen separation.
5. Buffer is RAM only (20 messages) and QoS 0. For longer outages use flash (LittleFS) and QoS 1 with server acks.
6. Missing: OTA, TLS enabled by default, per-device credentials, remote logging, impedance sweeps, on-device threshold events.
7. Baselines are only set manually; add automatic re-baselining after confirmed defrost.

Server
1. The evaluation is a placeholder rule (`evaluate.py`). Collect labelled data (camera ground truth) before tuning.
2. No authentication on the REST API or dashboard, and Mosquitto runs anonymous. Both are development settings only.
3. SQLite is fine for a pilot; move to TimescaleDB/InfluxDB for many devices and long retention.
4. Devices whose clock is unsynced (`ts_source: uptime_only`) are stamped with server receive time, which is wrong for buffered messages.
5. No alerting yet (e-mail/webhook on `ice`, device offline, gap count rising).

## Schema: only what is in use is switched on

The full schema (about 500 distinct properties) is kept, but only the ~130 property entries that the
firmware, simulator or server actually send or read are active. The rest is commented out line by line.

- **Edit** `schema/ice-sensing.schema.jsonc` (JSON with comments). A line starting with `//` is switched off.
- **Build** with `python3 schema/build_schema.py`. It strips the comments, prunes definitions nothing
  uses, checks that no `required` entry points at a switched-off property, and writes
  `ice-sensing.schema.json` (about 420 lines), which the server, tests and host check load.
- **Enable a property:** delete the leading `// ` on its line, rebuild. Trailing commas are allowed, so
  nothing else needs fixing. If its value is a `$ref`, enable the fields you need inside that definition too.
- `python3 schema/build_schema.py --check` fails if the generated file is stale (good for CI), and
  `--enable-all FILE` writes the complete schema for reference.
- Switched off means rejected: the server stores such a message in `rejected` rather than accepting it.
  So enable a field in the schema before the firmware starts sending it.
- Enum values (event types, quality flags, command types) are left as they were; only property names
  and message types are switched off.
- Candidates to enable first: `asset` (which HVAC unit and coil a device is on), `capacitive.capacitance_pf`,
  `delta_pf` and `baseline_pf` once an FDC1004/FDC2214 replaces the touch pad (`evaluate.py` already reads
  them), and `environment` (pressure drop across the coil, airflow).

## Contract change made while building this

`seq` in the schema was described as "reset only on factory reset", which conflicts with `boot_count`
making `(boot_count, seq)` unique. The firmware restarts `seq` at 0 on every boot (no flash wear) and
the description in `schema/ice-sensing.schema.json` now says so. Gap detection is per boot.
