import json
import os

import pytest
from fastapi.testclient import TestClient

import evaluate
from app import create_app
from db import Database
from ingest import Ingestor, flatten_telemetry, load_validator
from simulator import SimDevice

SCHEMA = os.path.join(os.path.dirname(__file__), "..", "..", "schema", "ice-sensing.schema.json")
NOW = 1_800_000_000_000


@pytest.fixture
def validator():
    return load_validator(SCHEMA)


@pytest.fixture
def ing(validator):
    return Ingestor(Database(":memory:"), validator, now_ms=lambda: NOW)


def topic(msg):
    return f"ice/v1/{msg['device_id']}/{msg['message_type']}"


def send(ing, msg):
    return ing.handle(topic(msg), json.dumps(msg).encode())


def test_simulated_messages_conform_to_schema(validator):
    dev = SimDevice()
    for msg in (dev.telemetry(NOW), dev.status(NOW), dev.boot_event(NOW), dev.ack(NOW, "cmd-1")):
        assert not list(validator.iter_errors(msg)), msg["message_type"]


def test_telemetry_is_stored_flattened_and_assessed(ing):
    dev = SimDevice(cycle_s=100)
    assert send(ing, dev.telemetry(NOW, sim_t_s=50)) == "stored"
    metrics = {(r["channel_id"], r["metric"]) for r in ing.db.query("SELECT * FROM samples")}
    assert ("cap-1", "capacitive.delta_pct") in metrics
    assert ("temp-coil", "thermal.temperature_c") in metrics
    assert ing.db.one("SELECT state FROM assessments")["state"] in ("frost", "ice")


def test_duplicates_are_ignored(ing):
    msg = SimDevice().telemetry(NOW)
    assert send(ing, msg) == "stored"
    assert send(ing, msg) == "duplicate"
    assert ing.db.one("SELECT COUNT(*) AS n FROM messages")["n"] == 1
    assert ing.db.one("SELECT COUNT(*) AS n FROM assessments")["n"] == 1


def test_gap_detection_within_one_boot_only(ing):
    dev = SimDevice(boot_count=1)
    first = dev.telemetry(NOW)           # seq 0
    dev.seq = 4
    later = dev.telemetry(NOW + 1)       # seq 4 -> seq 1..3 missing
    send(ing, first)
    send(ing, later)
    assert ing.db.one("SELECT gaps FROM devices")["gaps"] == 3
    rebooted = SimDevice(boot_count=2)   # seq restarts at 0: not a gap
    send(ing, rebooted.telemetry(NOW + 2))
    assert ing.db.one("SELECT gaps FROM devices")["gaps"] == 3


def test_invalid_messages_are_rejected_and_recorded(ing):
    msg = SimDevice().telemetry(NOW)
    msg["telemetry"]["channels"][0]["capacitive"]["bogus_field"] = 1     # additionalProperties: false
    assert send(ing, msg) == "rejected"
    assert ing.handle("ice/v1/x/telemetry", b"not json") == "rejected"
    assert ing.db.one("SELECT COUNT(*) AS n FROM rejected")["n"] == 2
    assert ing.db.one("SELECT COUNT(*) AS n FROM messages")["n"] == 0


def test_device_cannot_publish_as_another_device(ing):
    msg = SimDevice(device_id="esp32-aaaa").telemetry(NOW)
    assert ing.handle("ice/v1/esp32-bbbb/telemetry", json.dumps(msg).encode()) == "rejected"


def test_online_topic_and_foreign_topics(ing):
    assert ing.handle("ice/v1/esp32-aaaa/online", b"1") == "online"
    assert ing.db.one("SELECT online FROM devices")["online"] == 1
    assert ing.handle("ice/v1/esp32-aaaa/cmd", b"{}") == "ignored"
    assert ing.handle("other/topic", b"{}") == "ignored"


def test_status_message_updates_device(ing):
    send(ing, SimDevice().status(NOW))
    d = ing.db.one("SELECT * FROM devices")
    assert d["firmware"] == "sim-0.1" and json.loads(d["last_status"])["health"] == "ok"


def test_frost_cycle_goes_clear_to_ice_to_wet():
    dev = SimDevice(cycle_s=100)
    states = [evaluate.assess(dev.telemetry(NOW, sim_t_s=t)["telemetry"])["state"] for t in range(100)]
    assert states[5] == "clear"
    assert "ice" in states and "frost" in states and "wet" in states
    assert states.index("frost") < states.index("ice") < states.index("wet")


def test_assessment_without_baseline_is_unknown():
    result = evaluate.assess({"channels": [{"channel_id": "c", "kind": "capacitive", "capacitive": {"raw_counts": 1}}]})
    assert result["state"] == "unknown"


def test_flatten_ignores_booleans_and_nested():
    rows = flatten_telemetry({"channels": [{"channel_id": "t", "kind": "temperature",
                                            "thermal": {"temperature_c": -3.0, "below_freezing": True}}]})
    assert rows == [("t", "thermal.temperature_c", -3.0)]


# ---------------------------------------------------------------------- API
@pytest.fixture
def client(tmp_path):
    published = []
    app = create_app(db_path=str(tmp_path / "t.db"), schema_path=SCHEMA, mqtt_enabled=False,
                     publisher=lambda t, p: published.append((t, p)) or True)
    c = TestClient(app)
    c.published = published
    c.ingestor = app.state.ingestor
    return c


def test_api_end_to_end(client):
    dev = SimDevice(cycle_s=100)
    for i, t in enumerate((5, 40, 60)):
        send(client.ingestor, dev.telemetry(NOW + i * 1000, sim_t_s=t))
    devices = client.get("/api/devices").json()
    assert devices[0]["device_id"] == dev.device_id and devices[0]["state"] is not None
    series = client.get(f"/api/devices/{dev.device_id}/series",
                        params={"channel_id": "cap-1", "metric": "capacitive.delta_pct"}).json()
    assert len(series) == 3 and series[0][0] < series[-1][0]
    assert client.get(f"/api/devices/{dev.device_id}/assessments").json()[0]["state"]
    assert client.get("/api/devices/nope/series", params={"channel_id": "a", "metric": "b"}).status_code == 404
    assert client.get("/").status_code == 200


def test_command_is_validated_and_published(client):
    dev = SimDevice()
    send(client.ingestor, dev.status(NOW))
    r = client.post(f"/api/devices/{dev.device_id}/commands", json={"command_type": "capture_baseline"})
    assert r.status_code == 202
    topic_, payload = client.published[-1]
    assert topic_ == f"ice/v1/{dev.device_id}/cmd"
    sent = json.loads(payload)
    assert sent["command"]["command_type"] == "capture_baseline" and sent["device_id"] == dev.device_id
    bad = client.post(f"/api/devices/{dev.device_id}/commands", json={"command_type": "format_hard_drive"})
    assert bad.status_code == 422
    assert len(client.published) == 1
    # the ack from the device is stored and visible
    send(client.ingestor, dev.ack(NOW + 5, sent["command"]["command_id"]))
    msgs = client.get(f"/api/devices/{dev.device_id}/messages", params={"message_type": "command_ack"}).json()
    assert msgs[0]["message"]["command_ack"]["result"] == "completed"
