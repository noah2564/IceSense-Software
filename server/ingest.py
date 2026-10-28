"""Ingest pipeline: topic -> parse -> schema validation -> dedupe -> store -> derive.

Transport-independent on purpose: `Ingestor.handle(topic, payload)` is called by the MQTT
link in production and called directly by the tests, so none of this needs a broker.
"""
import json
import time
from typing import Callable

from jsonschema import Draft202012Validator

import evaluate
from db import Database

TOPIC_PREFIX = ["ice", "v1"]
DEVICE_MESSAGE_TYPES = {
    "telemetry", "event", "status", "heartbeat", "config",
    "command_ack", "calibration", "diagnostic", "log",
}


def load_validator(schema_path: str) -> Draft202012Validator:
    with open(schema_path, encoding="utf-8") as fh:
        schema = json.load(fh)
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def _is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def flatten_telemetry(telemetry: dict) -> list[tuple[str, str, float]]:
    """Turn a telemetry payload into (channel_id, metric, value) rows.

    Every numeric scalar inside a channel's kind-specific block (and its statistics) becomes a
    metric named "<block>.<field>". Adding fields to the schema therefore needs no server change.
    """
    rows: list[tuple[str, str, float]] = []
    for ch in telemetry.get("channels", []):
        cid = ch["channel_id"]
        for block in ("capacitive", "resistive", "impedance", "thermal", "generic"):
            for k, v in (ch.get(block) or {}).items():
                if _is_number(v):
                    rows.append((cid, f"{block}.{k}", float(v)))
        for k in ("n", "mean", "min", "max", "std"):
            v = (ch.get("statistics") or {}).get(k)
            if _is_number(v):
                rows.append((cid, f"stats.{k}", float(v)))
    for k, v in (telemetry.get("environment") or {}).items():
        if _is_number(v):
            rows.append(("environment", k, float(v)))
    return rows


class Ingestor:
    def __init__(self, db: Database, validator: Draft202012Validator,
                 now_ms: Callable[[], int] = lambda: int(time.time() * 1000)):
        self.db = db
        self.validator = validator
        self.now_ms = now_ms

    # ------------------------------------------------------------------ entry point
    def handle(self, topic: str, payload: bytes) -> str:
        """Returns a short result string: stored | duplicate | rejected | ignored | online."""
        parts = topic.split("/")
        if len(parts) != 4 or parts[:2] != TOPIC_PREFIX:
            return "ignored"
        device_from_topic, suffix = parts[2], parts[3]

        if suffix == "cmd":          # our own outgoing commands echoed back by the broker
            return "ignored"
        if suffix == "online":
            self._set_online(device_from_topic, payload.strip() == b"1")
            return "online"
        if suffix not in DEVICE_MESSAGE_TYPES:
            return self._reject(topic, payload, [f"unknown topic suffix '{suffix}'"])

        try:
            msg = json.loads(payload)
        except (ValueError, UnicodeDecodeError) as exc:
            return self._reject(topic, payload, [f"invalid JSON: {exc}"])

        errors = sorted(self.validator.iter_errors(msg), key=lambda e: list(e.absolute_path))
        if errors:
            return self._reject(topic, payload, [
                f"{'/'.join(str(p) for p in e.absolute_path) or '<root>'}: {e.message[:200]}"
                for e in errors[:10]
            ])
        # Guard against a device publishing as another device or on the wrong topic.
        if msg["device_id"] != device_from_topic:
            return self._reject(topic, payload, ["device_id does not match topic"])
        if msg["message_type"] != suffix:
            return self._reject(topic, payload, ["message_type does not match topic"])

        return self._store(msg, payload)

    # ------------------------------------------------------------------ internals
    def _reject(self, topic: str, payload: bytes, errors: list[str]) -> str:
        with self.db.tx() as c:
            c.execute(
                "INSERT INTO rejected(received_at, topic, payload, errors) VALUES(?,?,?,?)",
                (self.now_ms(), topic, payload[:4000].decode("utf-8", "replace"), json.dumps(errors)),
            )
        return "rejected"

    def _set_online(self, device_id: str, online: bool) -> None:
        now = self.now_ms()
        with self.db.tx() as c:
            c.execute(
                "INSERT INTO devices(device_id, first_seen, last_seen, online) VALUES(?,?,?,?) "
                "ON CONFLICT(device_id) DO UPDATE SET online=excluded.online",
                (device_id, now, now, int(online)),
            )

    def _store(self, msg: dict, raw: bytes) -> str:
        now = self.now_ms()
        dev = msg["device_id"]
        # A device without a synced clock sends uptime in ts; fall back to server receive time.
        eff_ts = now if msg.get("ts_source") == "uptime_only" else msg["ts"]

        with self.db.tx() as c:
            cur = c.execute(
                "INSERT OR IGNORE INTO messages(message_id, device_id, message_type, boot_count, seq, "
                "ts, ts_source, received_at, direction, payload) VALUES(?,?,?,?,?,?,?,?, 'in', ?)",
                (msg["message_id"], dev, msg["message_type"], msg.get("boot_count", 0), msg["seq"],
                 msg["ts"], msg.get("ts_source"), now, raw.decode("utf-8")),
            )
            if cur.rowcount == 0:
                return "duplicate"

            self._touch_device(c, msg, now)

            mtype = msg["message_type"]
            if mtype == "telemetry":
                telemetry = msg["telemetry"]
                c.executemany(
                    "INSERT INTO samples(device_id, ts, channel_id, metric, value) VALUES(?,?,?,?,?)",
                    [(dev, eff_ts, cid, metric, val) for cid, metric, val in flatten_telemetry(telemetry)],
                )
                result = evaluate.assess(telemetry)
                c.execute(
                    "INSERT INTO assessments(device_id, ts, state, details) VALUES(?,?,?,?)",
                    (dev, eff_ts, result["state"], json.dumps(result)),
                )
            elif mtype == "status":
                c.execute("UPDATE devices SET last_status=? WHERE device_id=?",
                          (json.dumps(msg["status"]), dev))
        return "stored"

    def _touch_device(self, c, msg: dict, now: int) -> None:
        dev = msg["device_id"]
        boot, seq = msg.get("boot_count", 0), msg["seq"]
        fw = (msg.get("firmware") or {}).get("version")
        row = c.execute("SELECT * FROM devices WHERE device_id=?", (dev,)).fetchone()
        if row is None:
            c.execute(
                "INSERT INTO devices(device_id, first_seen, last_seen, last_boot_count, last_seq, firmware) "
                "VALUES(?,?,?,?,?,?)", (dev, now, now, boot, seq, fw))
            return

        gaps = row["gaps"]
        last_boot, last_seq = row["last_boot_count"], row["last_seq"]
        # seq restarts at 0 on every boot, so gaps only make sense within one boot.
        if boot == last_boot and seq > last_seq + 1 and last_seq >= 0:
            gaps += seq - last_seq - 1
        newer = boot > last_boot or (boot == last_boot and seq > last_seq)
        c.execute(
            "UPDATE devices SET last_seen=?, gaps=?, last_boot_count=?, last_seq=?, "
            "firmware=COALESCE(?, firmware) WHERE device_id=?",
            (now, gaps, boot if newer else last_boot, seq if newer else last_seq, fw, dev),
        )

    # ------------------------------------------------------------------ outgoing
    def store_outgoing(self, msg: dict) -> None:
        """Keep a copy of every command we send so the UI can show history."""
        with self.db.tx() as c:
            c.execute(
                "INSERT INTO messages(message_id, device_id, message_type, boot_count, seq, ts, ts_source, "
                "received_at, direction, payload) VALUES(?,?,?,?,?,?,?,?, 'out', ?)",
                (msg["message_id"], msg["device_id"], msg["message_type"], 0, msg["seq"], msg["ts"],
                 "server", self.now_ms(), json.dumps(msg)),
            )
