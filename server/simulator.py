"""Device simulator: lets the server (and dashboard) be developed without any hardware.

Produces messages in exactly the shape the firmware produces. The frost cycle is an
ILLUSTRATIVE shape (cool down -> frost builds -> defrost -> drain), not a physical model.

  python simulator.py --host localhost --interval 5 --cycle 300
"""
import argparse
import json
import math
import random
import time

SCHEMA_VERSION = "1.0.0"
CAP_BASE = 50000.0        # "clean" touch counts
RES_BASE = 8.0e6          # "clean" resistance, ohm


class Scenario:
    """Maps time within a cycle to sensor values."""

    def __init__(self, cycle_s: float = 300.0, seed: int = 1):
        self.cycle_s = cycle_s
        self.rng = random.Random(seed)

    def at(self, t_s: float) -> dict:
        p = (t_s % self.cycle_s) / self.cycle_s
        if p < 0.20:                                   # cooling down, still clean
            k = p / 0.20
            temp, delta, res_drop = 6 - 12 * k, 0.5 * k, 0.0
        elif p < 0.75:                                 # frost builds up
            k = (p - 0.20) / 0.55
            temp, delta, res_drop = -6.0, 0.5 + 34 * k, 1.2 * k
        elif p < 0.85:                                 # defrost: warms up, meltwater on the electrode
            k = (p - 0.75) / 0.10
            temp, delta, res_drop = -6 + 10 * k, 34 + 25 * k, 1.2 + 1.5 * k
        else:                                          # drains and dries
            k = (p - 0.85) / 0.15
            temp, delta, res_drop = 4 + 2 * k, 59 * (1 - k), 2.7 * (1 - k)
        n = self.rng
        return {
            "temp_c": temp + n.gauss(0, 0.05),
            "cap_delta_pct": delta + n.gauss(0, 0.15),
            "res_ohm": RES_BASE * 10 ** (-res_drop) * (1 + n.gauss(0, 0.01)),
        }


class SimDevice:
    def __init__(self, device_id="esp32-sim000001", boot_count=1, cycle_s=300.0, window_s=10, rate_hz=10):
        self.device_id, self.boot_count = device_id, boot_count
        self.scenario = Scenario(cycle_s)
        self.window_s, self.rate_hz = window_s, rate_hz
        self.seq = 0
        self.t0 = time.time()

    def _envelope(self, mtype: str, now_ms: int) -> dict:
        s = self.seq
        self.seq += 1
        return {
            "schema_version": SCHEMA_VERSION, "message_type": mtype,
            "message_id": f"{self.device_id}-{self.boot_count}-{s}", "device_id": self.device_id,
            "seq": s, "boot_count": self.boot_count, "ts": now_ms, "ts_source": "ntp",
            "config_version": 0,
        }

    def telemetry(self, now_ms: int, sim_t_s: float | None = None) -> dict:
        v = self.scenario.at(time.time() - self.t0 if sim_t_s is None else sim_t_s)
        n = int(self.window_s * self.rate_hz)
        cap_mean = CAP_BASE * (1 + v["cap_delta_pct"] / 100)
        msg = self._envelope("telemetry", now_ms)
        msg["telemetry"] = {
            "window": {"start_ts": now_ms - self.window_s * 1000, "end_ts": now_ms,
                       "duration_ms": self.window_s * 1000, "sample_rate_hz": self.rate_hz,
                       "sample_count": n, "aggregation": "mean", "trigger": "periodic"},
            "channels": [
                {"channel_id": "cap-1", "kind": "capacitive",
                 "capacitive": {"raw_counts": round(cap_mean, 1), "delta_pct": round(v["cap_delta_pct"], 2)},
                 "statistics": {"n": n, "mean": round(cap_mean, 1), "min": round(cap_mean * 0.998, 1),
                                "max": round(cap_mean * 1.002, 1), "std": round(cap_mean * 0.0005, 2)},
                 "quality": {"status": "good"}},
                {"channel_id": "res-1", "kind": "resistive",
                 "resistive": {"voltage_v": 1.8, "resistance_ohm": round(v["res_ohm"]),
                               "log10_resistance": round(math.log10(v["res_ohm"]), 3),
                               "baseline_ohm": RES_BASE,
                               "delta_pct": round((v["res_ohm"] - RES_BASE) / RES_BASE * 100, 2)},
                 "statistics": {"n": n, "mean": round(v["res_ohm"]), "min": round(v["res_ohm"] * 0.99),
                                "max": round(v["res_ohm"] * 1.01), "std": round(v["res_ohm"] * 0.003)},
                 "quality": {"status": "good"}},
                {"channel_id": "temp-coil", "kind": "temperature",
                 "thermal": {"temperature_c": round(v["temp_c"], 2), "sensor_type": "ntc",
                             "location": "coil_surface", "ntc_beta": 3950,
                             "below_freezing": v["temp_c"] <= 0},
                 "statistics": {"n": n, "mean": round(v["temp_c"], 2)},
                 "quality": {"status": "good"}},
            ],
            "status_summary": {"rssi_dbm": -60, "free_heap_bytes": 180000, "queue_depth": 0},
        }
        return msg

    def status(self, now_ms: int) -> dict:
        msg = self._envelope("status", now_ms)
        msg["firmware"] = {"version": "sim-0.1", "chip": "ESP32-S3"}
        msg["status"] = {
            "health": "ok",
            "connectivity": {"link": "wifi", "connected": True, "rssi_dbm": -60, "transport": "mqtt"},
            "system": {"free_heap_bytes": 180000, "cpu_freq_mhz": 240, "reset_reason": "power_on"},
            "buffering": {"storage": "ram", "queued_messages": 0, "capacity_messages": 20, "dropped_messages": 0},
            "sensors": [{"channel_id": "cap-1", "state": "ok", "baseline_valid": True},
                        {"channel_id": "res-1", "state": "ok", "baseline_valid": True},
                        {"channel_id": "temp-coil", "state": "ok"}],
        }
        return msg

    def boot_event(self, now_ms: int) -> dict:
        msg = self._envelope("event", now_ms)
        msg["firmware"] = {"version": "sim-0.1", "chip": "ESP32-S3"}
        msg["event"] = {"event_type": "boot", "severity": "info", "state": "instant", "message": "simulated boot"}
        return msg

    def ack(self, now_ms: int, command_id: str, result: str = "completed") -> dict:
        msg = self._envelope("command_ack", now_ms)
        msg["in_reply_to"] = command_id
        msg["command_ack"] = {"command_id": command_id, "result": result}
        return msg


def main():
    import paho.mqtt.client as mqtt

    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--device", default="esp32-sim000001")
    ap.add_argument("--interval", type=float, default=5.0, help="seconds between telemetry messages")
    ap.add_argument("--cycle", type=float, default=300.0, help="seconds per frost/defrost cycle")
    ap.add_argument("--count", type=int, default=0, help="stop after N telemetry messages (0 = forever)")
    args = ap.parse_args()

    dev = SimDevice(args.device, boot_count=int(time.time()) % 100000, cycle_s=args.cycle)
    base = f"ice/v1/{args.device}"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=args.device)
    client.will_set(f"{base}/online", "0", qos=1, retain=True)

    def on_message(c, u, m):                      # answer every command with a completed ack
        try:
            cid = json.loads(m.payload)["command"]["command_id"]
        except (ValueError, KeyError):
            return
        c.publish(f"{base}/command_ack", json.dumps(dev.ack(int(time.time() * 1000), cid)), qos=1)
        print("acked command", cid)

    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=30)
    client.subscribe(f"{base}/cmd", qos=1)
    client.loop_start()
    client.publish(f"{base}/online", "1", qos=1, retain=True)
    client.publish(f"{base}/event", json.dumps(dev.boot_event(int(time.time() * 1000))), qos=1)

    sent = 0
    try:
        while True:
            now = int(time.time() * 1000)
            client.publish(f"{base}/telemetry", json.dumps(dev.telemetry(now)), qos=1)
            sent += 1
            if sent % 6 == 1:
                client.publish(f"{base}/status", json.dumps(dev.status(now)), qos=1)
            if args.count and sent >= args.count:
                break
            time.sleep(args.interval)
    finally:
        time.sleep(0.5)
        client.publish(f"{base}/online", "0", qos=1, retain=True).wait_for_publish(2)
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
