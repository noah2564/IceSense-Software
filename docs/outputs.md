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

## Sensor values currently reported
Telemetry has `cap-1`, `res-1` (front), `res-2` (back), and `temp-coil`. `cap-1.delta_pct` is the percent change
of total LC tank capacitance inferred without absolute L or reference-clock values:

$$\Delta C_{pct}=\left[\left(\frac{code_{base}}{code_{window}}\right)^2-1\right]100$$

Both resistive channels report MCP3426 `voltage_v` (sensor node minus the shared midpoint reference), inferred
`resistance_ohm`, and `log10_resistance`. The resistance is calculated once from each channel's window-mean voltage,
using the 1 MOhm high-side resistor, 40 kOhm / 40 kOhm reference divider, and MCP3426 input-impedance model. The
detector's existing `res_log10` is the lower of front/back log resistances (the presumed wetter side); this is a
pending design decision. An `open_circuit` flag at the upper model limit is normal and means dry, not a health fault.
A `short_circuit` or failed MCP3426 read degrades health.

The ADC uses a nominal 3.3 V supply that must be measured and substituted for `RES_VDD_V`. Its input-impedance
model is approximate above roughly 10 MOhm. Bring-up should check known 100 kOhm, 1 MOhm, and 10 MOhm resistors
in place of each sensor. The MCP3426 address is expected to be `0x68`; firmware probes `0x68` through `0x6F` and
logs the selected address. Confirm the front/back channel wiring and whether minimum-side resistance is the correct
combination rule before relying on `res_log10`.

Thickness is **uncalibrated** until `mm_per_pct` has been fitted against reference measurements (`lab/replay.py --reference-col`).
Do not wire the relay into a real defrost control before the thresholds have been verified on the real evaporator.
