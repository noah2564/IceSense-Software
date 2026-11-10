"""Virtual ICE-SENSE device.

Runs the real reference detector on a synthetic frost/defrost cycle and serves the SAME HTTP API
and the SAME web page as the ESP32 firmware (firmware/web/index.html), so the technician UI and the
lab tools can be developed without hardware.

    python -m simulator.device                 # http://localhost:8080, one cycle per 10 min
    python -m simulator.device --speed 20      # 20x faster
    python -m simulator.device --password pw   # HTTP basic auth, user "service"

HTTP API (identical on the firmware):
    GET  /api/state             current values, state, parameters
    GET  /api/history.csv       ring buffer of recent windows
    POST /api/baseline          capture the clean-sensor baseline
    POST /api/params            set (partial) detection parameters
    POST /api/params/defaults   restore defaults
    POST /api/relay {"enabled"} enable / disable the defrost-request relay output
"""
from __future__ import annotations

import argparse
import base64
import collections
import hmac
import json
import math
import os
import random
import threading
import time
from dataclasses import asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from algorithm.icedetect import ALGORITHM_NAME, ALGORITHM_VERSION, Detector, Params, validate_params
from simulator import scenario

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_INDEX = os.path.join(ROOT, "firmware", "web", "index.html")
CLEAN_COUNTS = 60000.0          # raw touch counts of the clean sensor (illustrative)
HISTORY_LEN = 900
CSV_COLUMNS = ["uptime_s", "state", "state_name", "thickness_mm", "defrost_request",
               "cap_delta_pct", "res_log10", "temp_c", "cap_counts"]


class VirtualDevice:
    def __init__(self, device_id="sim-001", cycle_s=600.0, window_s=2.0, speed=1.0, seed=1,
                 auto_baseline=True, state_file=None):
        self.device_id = device_id
        self.cycle_s = cycle_s
        self.window_s = window_s
        self.speed = speed
        self.rng = random.Random(seed)
        self.lock = threading.RLock()
        self.state_file = state_file
        self.params = Params()
        self.det = Detector(self.params)
        self.t = 0.0                           # simulated uptime in seconds
        self.seq = 0
        self.boot_count = 1
        self.baseline_counts = None
        self.relay_enabled = False
        self.history = collections.deque(maxlen=HISTORY_LEN)
        self.last = None
        self.listeners = []                    # called with every new window dict (MQTT bridge, tests)
        self._load_state()
        if auto_baseline and self.baseline_counts is None:
            self.baseline_counts = CLEAN_COUNTS

    # ---- persistence (the firmware keeps the same items in NVS) ----
    def _load_state(self):
        if not self.state_file or not os.path.exists(self.state_file):
            return
        with open(self.state_file, encoding="utf-8") as fh:
            d = json.load(fh)
        self.baseline_counts = d.get("baseline_counts")
        self.relay_enabled = bool(d.get("relay_enabled", False))
        self.params = Params(**{k: v for k, v in d.get("params", {}).items() if k in asdict(Params())})
        self.det.p = self.params
        self.boot_count = int(d.get("boot_count", 0)) + 1

    def _save_state(self):
        if not self.state_file:
            return
        with open(self.state_file, "w", encoding="utf-8") as fh:
            json.dump({"baseline_counts": self.baseline_counts, "relay_enabled": self.relay_enabled,
                       "params": asdict(self.params), "boot_count": self.boot_count}, fh)

    # ---- one measurement window ----
    def tick(self) -> dict:
        with self.lock:
            v = scenario.at(self.t, self.cycle_s, self.rng)
            counts = CLEAN_COUNTS * (1.0 + v["cap_delta_pct"] / 100.0)
            if self.baseline_counts:
                cap_pct = (counts - self.baseline_counts) / self.baseline_counts * 100.0
            else:
                cap_pct = float("nan")
            out = self.det.step(cap_pct, v["log10_ohm"], v["temp_c"])
            relay_on = self.relay_enabled and out.defrost_request
            w = {"uptime_s": round(self.t, 3), "seq": self.seq, "state": int(out.state),
                 "state_name": out.state.label, "thickness_mm": round(out.thickness_mm, 3),
                 "defrost_request": out.defrost_request, "relay_on": relay_on,
                 "cap_delta_pct": None if math.isnan(cap_pct) else round(cap_pct, 3),
                 "res_log10": round(v["log10_ohm"], 4), "temp_c": round(v["temp_c"], 3),
                 "cap_counts": round(counts, 1)}
            self.history.append(w)
            self.last = w
            self.t += self.window_s
            self.seq += 1
        for fn in list(self.listeners):
            fn(w)
        return w

    # ---- API operations ----
    def state_json(self) -> dict:
        with self.lock:
            w = self.last or {"state": 0, "state_name": "unknown", "thickness_mm": 0.0,
                              "defrost_request": False, "relay_on": False, "cap_delta_pct": None,
                              "res_log10": None, "temp_c": None, "cap_counts": None}
            return {
                "device_id": self.device_id, "simulated": True, "fw": "sim",
                "algorithm": {"name": ALGORITHM_NAME, "version": ALGORITHM_VERSION},
                "uptime_s": int(self.t), "boot_count": self.boot_count, "seq": self.seq,
                "state": w["state"], "state_name": w["state_name"],
                "thickness_mm": w["thickness_mm"], "thickness_calibrated": False,
                "defrost_request": w["defrost_request"],
                "raw": {"cap_delta_pct": w["cap_delta_pct"], "cap_counts": w["cap_counts"],
                        "res_log10": w["res_log10"], "temp_c": w["temp_c"]},
                "baseline": {"captured": self.baseline_counts is not None,
                             "cap_counts": self.baseline_counts or 0},
                "relay": {"enabled": self.relay_enabled, "on": bool(w["relay_on"])},
                "params": asdict(self.params),
                "net": {"sta_connected": False, "mqtt_connected": False},
            }

    def history_csv(self) -> str:
        with self.lock:
            rows = list(self.history)
        lines = [",".join(CSV_COLUMNS)]
        for w in rows:
            lines.append(",".join(
                "" if w[c] is None else ("1" if w[c] is True else "0" if w[c] is False else str(w[c]))
                for c in CSV_COLUMNS))
        return "\n".join(lines) + "\n"

    def capture_baseline(self) -> dict:
        with self.lock:
            if self.last is None:
                raise ValueError("no measurement yet")
            recent = [w["cap_counts"] for w in list(self.history)[-5:]]
            self.baseline_counts = sum(recent) / len(recent)
            self.det.reset()
            self._save_state()
        return self.state_json()

    def set_params(self, d: dict) -> dict:
        err = validate_params(d)
        if err:
            raise ValueError(err)
        with self.lock:
            merged = {**asdict(self.params), **d}
            for k in ("confirm_windows", "defrost_on_windows", "defrost_off_windows"):
                merged[k] = int(merged[k])
            if merged["defrost_off_mm"] >= merged["defrost_on_mm"]:
                raise ValueError("defrost_off_mm must be below defrost_on_mm (hysteresis)")
            self.params = Params(**merged)
            self.det.p = self.params
            self._save_state()
        return self.state_json()

    def set_relay(self, enabled: bool) -> dict:
        with self.lock:
            self.relay_enabled = bool(enabled)
            self._save_state()
        return self.state_json()

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            self.tick()
            stop.wait(self.window_s / self.speed)


def make_handler(dev: VirtualDevice, password: str | None, user="service"):
    with open(WEB_INDEX, "rb") as fh:
        page = fh.read()

    class Handler(BaseHTTPRequestHandler):
        server_version = "ice-sense-sim"

        def log_message(self, *a):  # quiet
            pass

        def _send(self, code, body: bytes, ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, code=200):
            self._send(code, json.dumps(obj).encode())

        def _authed(self) -> bool:
            if not password:
                return True
            h = self.headers.get("Authorization", "")
            if h.startswith("Basic "):
                try:
                    got = base64.b64decode(h[6:]).decode()
                except Exception:
                    got = ""
                if hmac.compare_digest(got, f"{user}:{password}"):
                    return True
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="ice-sense"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return False

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n > 2048:
                raise ValueError("body too large")
            raw = self.rfile.read(n) if n else b"{}"
            obj = json.loads(raw or b"{}")
            if not isinstance(obj, dict):
                raise ValueError("JSON object expected")
            return obj

        def do_GET(self):
            if not self._authed():
                return
            if self.path in ("/", "/index.html"):
                self._send(200, page, "text/html; charset=utf-8")
            elif self.path == "/api/state":
                self._json(dev.state_json())
            elif self.path == "/api/history.csv":
                self._send(200, dev.history_csv().encode(), "text/csv")
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            if not self._authed():
                return
            try:
                body = self._body()
                if self.path == "/api/baseline":
                    self._json(dev.capture_baseline())
                elif self.path == "/api/params":
                    self._json(dev.set_params(body))
                elif self.path == "/api/params/defaults":
                    self._json(dev.set_params(asdict(Params())))
                elif self.path == "/api/relay":
                    if not isinstance(body.get("enabled"), bool):
                        raise ValueError("'enabled' must be true or false")
                    self._json(dev.set_relay(body["enabled"]))
                else:
                    self._send(404, b"not found", "text/plain")
            except (ValueError, json.JSONDecodeError) as e:
                self._send(400, str(e).encode(), "text/plain")

    return Handler


def serve(dev: VirtualDevice, host="127.0.0.1", port=8080, password=None):
    srv = ThreadingHTTPServer((host, port), make_handler(dev, password))
    srv.daemon_threads = True
    return srv


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--device-id", default="sim-001")
    ap.add_argument("--speed", type=float, default=1.0, help="simulated seconds per real second")
    ap.add_argument("--cycle", type=float, default=600.0, help="length of one frost/defrost cycle, simulated s")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--password", default=None, help="enable HTTP basic auth (user: service)")
    ap.add_argument("--no-auto-baseline", action="store_true",
                    help="start without baseline, like a fresh device: capture it on the web page")
    ap.add_argument("--state-file", default=None, help="persist baseline/params/relay like NVS does")
    args = ap.parse_args()
    dev = VirtualDevice(args.device_id, args.cycle, 2.0, args.speed, args.seed,
                        not args.no_auto_baseline, args.state_file)
    stop = threading.Event()
    threading.Thread(target=dev.run, args=(stop,), daemon=True).start()
    srv = serve(dev, args.host, args.port, args.password)
    print(f"virtual device on http://{args.host}:{args.port}  (speed x{args.speed:g}, cycle {args.cycle:g} s)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()


if __name__ == "__main__":
    main()
