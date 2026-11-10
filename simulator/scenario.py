"""Synthetic frost/defrost cycle for the virtual device.

ILLUSTRATIVE ONLY. The shape follows the principle in the project description (capacitance grows with
the amount of deposit; resistance separates liquid from frozen) but it is not a physical model and
the numbers are not measurements. Use it to exercise software, not to tune thresholds.
"""
from __future__ import annotations

import random

# (cycle fraction, coil temp C, |capacitance change| %, log10 resistance in ohm)
KEYFRAMES = [
    (0.00, 6.0, 0.2, 6.9),     # clean, dry, coil cooling down
    (0.10, 1.5, 0.3, 6.9),
    (0.12, 1.2, 6.0, 5.0),     # condensation: liquid film, capacitance up, resistance down
    (0.22, 0.6, 6.5, 5.0),
    (0.30, -4.0, 8.0, 5.2),    # below freezing, film still liquid
    (0.36, -6.0, 9.0, 6.4),    # freezing: resistance jumps back up (phase change)
    (0.80, -6.0, 42.0, 6.4),   # frost/ice builds up: capacitance keeps growing
    (0.90, 4.0, 22.0, 4.6),    # defrost: warms up, deposit melts, liquid again
    (0.97, 5.5, 0.5, 5.6),     # drains
    (1.00, 6.0, 0.2, 6.9),     # dry again
]

DRY_LOG10 = 6.9
NOISE_TEMP = 0.05
NOISE_CAP = 0.15
NOISE_LOG10 = 0.02


def clean_values() -> dict:
    return {"temp_c": KEYFRAMES[0][1], "cap_delta_pct": 0.0, "log10_ohm": DRY_LOG10}


def at(t_s: float, cycle_s: float = 600.0, rng: random.Random | None = None) -> dict:
    """Signal values at simulated time t_s (seconds). Pass rng=None for the noise-free curve."""
    p = (t_s % cycle_s) / cycle_s
    for (p0, t0, c0, r0), (p1, t1, c1, r1) in zip(KEYFRAMES, KEYFRAMES[1:]):
        if p0 <= p <= p1:
            k = 0.0 if p1 == p0 else (p - p0) / (p1 - p0)
            vals = {"temp_c": t0 + (t1 - t0) * k, "cap_delta_pct": c0 + (c1 - c0) * k,
                    "log10_ohm": r0 + (r1 - r0) * k}
            break
    else:  # pragma: no cover - p is always within [0, 1)
        vals = clean_values()
    if rng is not None:
        vals["temp_c"] += rng.gauss(0, NOISE_TEMP)
        vals["cap_delta_pct"] = max(0.0, vals["cap_delta_pct"] + rng.gauss(0, NOISE_CAP))
        vals["log10_ohm"] += rng.gauss(0, NOISE_LOG10)
    return vals
