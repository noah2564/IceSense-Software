"""Server-side ice evaluation: PLACEHOLDER.

This is a deliberately simple, transparent threshold rule so the pipeline has something to
show end to end. The thresholds below are NOT validated on real coils. Replace this module
once labelled data exists (e.g. measurements with camera ground truth); keep the function
signature so nothing else has to change.

Why the rule looks the way it does:
  * Capacitance changes whenever the dielectric around the electrode changes (air -> frost/ice/water),
    so the change versus the clean baseline is the primary indicator.
  * Capacitance alone cannot separate liquid water from ice, so the coil temperature is used as
    context: at or below ~0 C a deposit is frost/ice, above it, it is liquid.
  * The raw-count direction of the ESP32 touch peripheral depends on the chip, so the magnitude
    of the change is used. With a real capacitance-to-digital converter (pF) this goes away.
"""

ALGORITHM = {"name": "server-threshold", "version": "0.1", "type": "threshold"}

FROST_PCT = 8.0     # |capacitance change| above this -> something is on the electrode
ICE_PCT = 20.0      # above this at sub-zero temperature -> thick deposit
FREEZE_C = 0.5      # coil surface at or below this -> deposit is frozen


def extract_features(telemetry: dict) -> dict:
    f: dict = {}
    for ch in telemetry.get("channels", []):
        kind = ch.get("kind")
        if kind == "capacitive" and "cap_delta_pct" not in f:
            cap = ch.get("capacitive", {})
            if "delta_pct" in cap:
                f["cap_delta_pct"] = abs(cap["delta_pct"])
            elif "delta_pf" in cap and cap.get("baseline_pf"):
                f["cap_delta_pct"] = abs(cap["delta_pf"]) / cap["baseline_pf"] * 100.0
        elif kind == "resistive" and "res_log10_ohm" not in f:
            res = ch.get("resistive", {})
            if "log10_resistance" in res:
                f["res_log10_ohm"] = res["log10_resistance"]
            if "delta_pct" in res:
                f["res_delta_pct"] = res["delta_pct"]
        elif kind == "temperature" and "coil_temp_c" not in f:
            th = ch.get("thermal", {})
            if th.get("location") == "coil_surface" and "temperature_c" in th:
                f["coil_temp_c"] = th["temperature_c"]
    env = telemetry.get("environment", {})
    if "coil_temp_c" not in f and "coil_surface_temperature_c" in env:
        f["coil_temp_c"] = env["coil_surface_temperature_c"]
    return f


def assess(telemetry: dict) -> dict:
    f = extract_features(telemetry)
    cap = f.get("cap_delta_pct")
    temp = f.get("coil_temp_c")

    if cap is None:
        state, reason = "unknown", "no capacitive delta available (baseline missing?)"
    elif cap < FROST_PCT:
        state, reason = "clear", f"capacitance change {cap:.1f}% below {FROST_PCT}%"
    elif temp is None:
        state, reason = "unknown", "capacitance changed but no coil temperature to separate water from ice"
    elif temp <= FREEZE_C:
        if cap >= ICE_PCT:
            state, reason = "ice", f"change {cap:.1f}% >= {ICE_PCT}% at {temp:.1f} C"
        else:
            state, reason = "frost", f"change {cap:.1f}% >= {FROST_PCT}% at {temp:.1f} C"
    else:
        state, reason = "wet", f"change {cap:.1f}% at {temp:.1f} C (above freezing: liquid water)"

    return {
        "state": state,
        "reason": reason,
        "defrost_recommended": state == "ice",
        "features": f,
        "algorithm": ALGORITHM,
    }
