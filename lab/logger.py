"""Research-phase logger: MQTT -> schema-validated JSONL archive + CSV with the same columns as the
device's /api/history.csv.

    python lab/logger.py --host localhost --out data/        # needs: pip install paho-mqtt jsonschema
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
COLUMNS = ["uptime_s", "state", "state_name", "thickness_mm", "defrost_request",
           "cap_delta_pct", "res_log10", "temp_c", "cap_counts"]
STATE_NUM = {"unknown": 0, "clear": 1, "condensation": 2, "frost": 3, "ice": 4, "melting": 5}


def telemetry_to_row(msg: dict) -> dict | None:
    """One telemetry message -> one CSV row (None for other message types)."""
    if msg.get("message_type") != "telemetry":
        return None
    t = msg["telemetry"]
    ch = {c["channel_id"]: c for c in t["channels"]}
    ice = t.get("ice_assessment", {})
    cap = ch.get("cap-1", {}).get("capacitive", {})
    res = ch.get("res-1", {}).get("resistive", {})
    thermal = ch.get("temp-coil", {}).get("thermal", {})
    state = ice.get("state", "unknown")
    return {"uptime_s": round(msg["uptime_ms"] / 1000, 1), "state": STATE_NUM.get(state, 0), "state_name": state,
            "thickness_mm": ice.get("thickness_mm_est", 0), "defrost_request": int(ice.get("defrost_recommended", False)),
            "cap_delta_pct": cap.get("delta_pct", ""), "res_log10": res.get("log10_resistance", ""),
            "temp_c": thermal.get("temperature_c", ""), "cap_counts": cap.get("raw_counts", "")}


def main() -> None:
    import paho.mqtt.client as mqtt
    from jsonschema import Draft202012Validator

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--out", default="data")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    validator = Draft202012Validator(json.load(open(os.path.join(ROOT, "schema", "ice-sensing.schema.json"))))
    raw = open(os.path.join(a.out, "messages.jsonl"), "a", encoding="utf-8")
    new = not os.path.exists(os.path.join(a.out, "windows.csv"))
    fh = open(os.path.join(a.out, "windows.csv"), "a", newline="", encoding="utf-8")
    w = csv.DictWriter(fh, COLUMNS)
    if new:
        w.writeheader()

    def on_message(_c, _u, m):
        try:
            msg = json.loads(m.payload)
        except ValueError:
            return
        errs = list(validator.iter_errors(msg))
        if errs:
            print("rejected (schema):", errs[0].message[:120])
            return
        raw.write(json.dumps(msg) + "\n"); raw.flush()
        row = telemetry_to_row(msg)
        if row:
            w.writerow(row); fh.flush()

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    c.on_message = on_message
    c.on_connect = lambda cl, *_: cl.subscribe("ice/v1/+/#")
    c.connect(a.host, a.port)
    print(f"logging to {a.out}/ (Ctrl-C to stop)")
    c.loop_forever()


if __name__ == "__main__":
    main()
