"""Ice detection, reference implementation.

This is the single source of truth for the detection logic. Develop and tune it here, on recorded
data (see lab/replay.py). The firmware has a line-by-line C++ port in firmware/lib/icedetect/;
algorithm/testvectors.csv is generated from THIS file, and the C++ port must reproduce it exactly
(tests/test_cpp_equivalence.py). Change the logic here, regenerate the vectors, then port.

Idea, taken from the project description: the capacitive channel tells HOW MUCH is on the
electrode, the resistive channel tells WHICH PHASE it is in (a liquid film conducts, ice does not),
and the coil temperature gives context. All thresholds below are PLACEHOLDERS: they have not been
calibrated on real evaporators and must be tuned from experiment data.

Inputs per measurement window (all optional; None/NaN means "not available"):
    cap_delta_pct  change of capacitance versus the clean baseline, in percent (sign is ignored:
                   the direction of raw touch counts depends on the chip)
    res_log10      log10 of the window-mean resistance in ohm
    temp_c         coil surface temperature in degrees C

Outputs: state, thickness estimate (mm, UNCALIBRATED), defrost request (latched, with hysteresis).
"""
from __future__ import annotations

import math
from dataclasses import asdict, dataclass
from enum import IntEnum

ALGORITHM_NAME = "icedetect"
ALGORITHM_VERSION = "0.1"


class State(IntEnum):
    """Numeric values are part of the output concept (docs/outputs.md): do not renumber."""
    UNKNOWN = 0
    CLEAR = 1
    CONDENSATION = 2   # liquid water on the electrode (above freezing, or not yet frozen)
    FROST = 3          # thin solid deposit
    ICE = 4            # thick solid deposit
    MELTING = 5        # a deposit that was solid now behaves like liquid (defrost running or ended)

    @property
    def label(self) -> str:
        return self.name.lower()


SOLID_STATES = (State.FROST, State.ICE, State.MELTING)


@dataclass
class Params:
    freeze_c: float = 0.5          # coil at or below this: deposits are frozen
    deposit_pct: float = 5.0       # |capacitance change| at or above this: something is on the electrode
    ice_pct: float = 20.0          # at or above this while frozen: thick deposit (ICE instead of FROST)
    liquid_log10: float = 5.5      # log10(ohm) below this: a liquid film is present
    mm_per_pct: float = 0.10       # thickness per percent capacitance change (UNCALIBRATED)
    max_mm: float = 15.0           # clamp for the thickness estimate
    confirm_windows: int = 2       # a new state must be seen this many windows in a row
    defrost_on_mm: float = 2.0     # request defrost at or above this thickness ...
    defrost_on_windows: int = 3    # ... for this many windows in a row
    defrost_off_mm: float = 0.5    # release the request at or below this thickness ...
    defrost_off_windows: int = 2   # ... for this many windows in a row

    def as_dict(self) -> dict:
        return asdict(self)


# Allowed ranges (min, max, integer?) for parameters set from the web page, over MQTT or by lab tools.
# The firmware has the same table (firmware/src/main.cpp); keep them in sync.
PARAM_LIMITS = {
    "freeze_c": (-30.0, 10.0, False),
    "deposit_pct": (0.5, 100.0, False),
    "ice_pct": (1.0, 500.0, False),
    "liquid_log10": (2.0, 8.0, False),
    "mm_per_pct": (0.001, 10.0, False),
    "max_mm": (1.0, 100.0, False),
    "confirm_windows": (1, 20, True),
    "defrost_on_mm": (0.1, 100.0, False),
    "defrost_on_windows": (1, 100, True),
    "defrost_off_mm": (0.0, 100.0, False),
    "defrost_off_windows": (1, 100, True),
}


def validate_params(d: dict) -> str | None:
    """Return an error message, or None if every key is known and within its range."""
    for k, v in d.items():
        if k not in PARAM_LIMITS:
            return f"unknown parameter '{k}'"
        lo, hi, is_int = PARAM_LIMITS[k]
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
            return f"'{k}' must be a number"
        if is_int and float(v) != int(v):
            return f"'{k}' must be an integer"
        if not lo <= v <= hi:
            return f"'{k}' must be between {lo:g} and {hi:g}"
    return None


@dataclass
class Output:
    state: State
    raw_state: State          # classification of this window before confirmation
    thickness_mm: float
    defrost_request: bool


def _finite(x) -> bool:
    return x is not None and math.isfinite(x)


class Detector:
    def __init__(self, params: Params | None = None):
        self.p = params or Params()
        self.reset()

    def reset(self) -> None:
        self.state = State.UNKNOWN
        self._cand = State.UNKNOWN
        self._cand_n = 0
        self._on_n = 0
        self._off_n = 0
        self.defrost = False

    def _classify(self, cap, t, r) -> State:
        p = self.p
        if cap is None or t is None:
            return State.UNKNOWN                 # no baseline or no temperature: cannot judge
        deposit = cap >= p.deposit_pct
        liquid = r is not None and r < p.liquid_log10
        frozen_regime = t <= p.freeze_c
        prev_solid = self.state in SOLID_STATES

        if prev_solid and liquid and deposit:
            return State.MELTING                 # a deposit that was solid now conducts like liquid
        if not deposit and not liquid:
            return State.CLEAR
        if frozen_regime:
            if liquid:
                return State.CONDENSATION        # liquid at/below freezing: about to freeze
            return State.ICE if cap >= p.ice_pct else State.FROST
        if deposit and prev_solid:
            return State.MELTING                 # still thick, now above freezing
        return State.CONDENSATION

    def step(self, cap_delta_pct=None, res_log10=None, temp_c=None) -> Output:
        p = self.p
        cap = abs(cap_delta_pct) if _finite(cap_delta_pct) else None
        t = temp_c if _finite(temp_c) else None
        r = res_log10 if _finite(res_log10) else None

        raw = self._classify(cap, t, r)

        # Confirmation: the reported state changes only after the same new state was seen
        # confirm_windows times in a row. Suppresses single-window glitches.
        if raw == self.state:
            self._cand_n = 0
        else:
            if raw == self._cand:
                self._cand_n += 1
            else:
                self._cand = raw
                self._cand_n = 1
            if self._cand_n >= p.confirm_windows:
                self.state = raw
                self._cand_n = 0

        thickness = 0.0
        if self.state in SOLID_STATES and cap is not None:
            thickness = min(cap * p.mm_per_pct, p.max_mm)

        # Defrost request: latched, with hysteresis on the thickness estimate. No request while the
        # state is unknown (fail-safe: without a judgement we do not ask for anything).
        if self.state == State.UNKNOWN:
            self.defrost = False
            self._on_n = 0
            self._off_n = 0
        elif not self.defrost:
            self._on_n = self._on_n + 1 if thickness >= p.defrost_on_mm else 0
            if self._on_n >= p.defrost_on_windows:
                self.defrost = True
                self._on_n = 0
                self._off_n = 0
        else:
            self._off_n = self._off_n + 1 if thickness <= p.defrost_off_mm else 0
            if self._off_n >= p.defrost_off_windows:
                self.defrost = False
                self._off_n = 0
                self._on_n = 0

        return Output(self.state, raw, thickness, self.defrost)
