# Outputs towards a controller

Nothing is required by Danfoss yet, so this is an **open concept**. What exists today, and what is only a proposal:

| Output | Status | Notes |
|---|---|---|
| Relay contact "defrost requested" | **implemented**, disabled by default | Closes while the detector's defrost request is latched (hysteresis on the thickness estimate). The technician enables it on the web page (or MQTT `set_output`). Open contact = no request (also when the device is unpowered or the state is unknown). |
| Numeric values (state, thickness, raw values) | **implemented** | Web page, `GET /api/state`, `GET /api/history.csv`, optional MQTT. |
| Modbus RTU | proposal only | Needs an RS-485 transceiver. Suggested input registers below. |
| 4-20 mA / 0-10 V | proposal only | Needs a DAC/PWM + driver stage. Suggest thickness 0-15 mm -> 4-20 mA. |

State codes (stable, do not renumber): 0 unknown, 1 clear, 2 condensation, 3 frost, 4 ice, 5 melting.

Proposed Modbus input registers: 0 state, 1 thickness x100 (mm), 2 defrost request, 3 capacitance change x100 (%),
4 log10 resistance x100, 5 coil temperature x10 (degC, signed), 6 health flags, 7 window counter.

Thickness is **uncalibrated** until `mm_per_pct` has been fitted against reference measurements (`lab/replay.py --reference-col`).
Do not wire the relay into a real defrost control before the thresholds have been verified on the real evaporator.
