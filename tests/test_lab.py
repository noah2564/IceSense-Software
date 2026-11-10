import csv
import io
import os
import subprocess
import sys

from lab.logger import COLUMNS, telemetry_to_row
from simulator.device import VirtualDevice

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_logger_row_from_firmware_style_message():
    msg = {"message_type": "telemetry", "uptime_ms": 12345, "telemetry": {"channels": [
        {"channel_id": "cap-1", "capacitive": {"raw_counts": 61000.5, "delta_pct": 1.7}},
        {"channel_id": "res-1", "resistive": {"log10_resistance": 6.4}},
        {"channel_id": "temp-coil", "thermal": {"temperature_c": -5.1}}],
        "ice_assessment": {"state": "frost", "thickness_mm_est": 0.8, "defrost_recommended": False}}}
    r = telemetry_to_row(msg)
    assert set(r) == set(COLUMNS) and r["state"] == 3 and r["cap_delta_pct"] == 1.7 and r["uptime_s"] == 12.3
    assert telemetry_to_row({"message_type": "status"}) is None


def test_replay_reproduces_device_and_fits_thickness(tmp_path):
    dev = VirtualDevice()
    for _ in range(300):
        dev.tick()
    path = tmp_path / "w.csv"
    path.write_text(dev.history_csv())
    run = lambda *a: subprocess.run([sys.executable, os.path.join(ROOT, "lab", "replay.py"), str(path), *a],
                                    capture_output=True, text=True, check=True).stdout
    assert "ice@" in run() and "melting@" in run()
    rows = list(csv.DictReader(io.StringIO(dev.history_csv())))
    ref = tmp_path / "ref.csv"
    with open(ref, "w", newline="") as fh:
        w = csv.DictWriter(fh, COLUMNS + ["ruler_mm"]); w.writeheader()
        for r in rows[150:160]:
            w.writerow({**r, "ruler_mm": abs(float(r["cap_delta_pct"])) * 0.2})   # true factor 0.2
    out = subprocess.run([sys.executable, os.path.join(ROOT, "lab", "replay.py"), str(ref), "--reference-col", "ruler_mm"],
                         capture_output=True, text=True, check=True).stdout
    assert "mm_per_pct = 0.2000" in out
