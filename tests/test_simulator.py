import base64
import json
import threading
import urllib.error
import urllib.request

import pytest

from simulator.device import VirtualDevice, serve


def start(dev, password=None):
    srv = serve(dev, "127.0.0.1", 0, password)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}"


def call(url, body=None, auth=None):
    req = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(),
                                 method="GET" if body is None else "POST",
                                 headers={"Content-Type": "application/json"})
    if auth:
        req.add_header("Authorization", "Basic " + base64.b64encode(auth.encode()).decode())
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


@pytest.fixture
def dev():
    return VirtualDevice(cycle_s=600, window_s=2.0)


@pytest.fixture
def api(dev):
    srv, base = start(dev)
    yield base
    srv.shutdown()


def run_windows(dev, n):
    return [dev.tick() for _ in range(n)]


def test_full_cycle_reaches_ice_requests_defrost_and_recovers(dev):
    ws = run_windows(dev, 300)
    names = [w["state_name"] for w in ws]
    for s in ("clear", "condensation", "frost", "ice", "melting"):
        assert s in names
    assert any(w["defrost_request"] for w in ws)
    assert ws[-1]["state_name"] == "clear" and not ws[-1]["defrost_request"]
    assert max(w["thickness_mm"] for w in ws) > 3


def test_relay_is_disabled_by_default_and_follows_request_when_enabled(dev):
    ws = run_windows(dev, 300)
    assert not any(w["relay_on"] for w in ws)
    dev.set_relay(True)
    ws = run_windows(dev, 300)
    assert any(w["relay_on"] for w in ws)
    assert all(w["relay_on"] == w["defrost_request"] for w in ws)


def test_state_endpoint(dev, api):
    run_windows(dev, 200)
    code, body = call(api + "/api/state")
    s = json.loads(body)
    assert code == 200 and s["state_name"] == "ice" and s["thickness_calibrated"] is False
    assert s["baseline"]["captured"] and s["relay"] == {"enabled": False, "on": False}
    assert s["params"]["ice_pct"] == 20.0


def test_history_csv_columns_and_rows(dev, api):
    run_windows(dev, 10)
    code, body = call(api + "/api/history.csv")
    lines = body.decode().strip().split("\n")
    assert lines[0].startswith("uptime_s,state,state_name,thickness_mm,defrost_request")
    assert len(lines) == 11 and all(len(l.split(",")) == 9 for l in lines)


def test_page_is_served(api):
    code, body = call(api + "/")
    assert code == 200 and b"ICE-SENSE" in body


def test_params_validation_and_update(dev, api):
    assert call(api + "/api/params", {"ice_pct": 25})[0] == 200
    assert dev.params.ice_pct == 25
    for bad in ({"nope": 1}, {"ice_pct": "x"}, {"ice_pct": 1e9}, {"confirm_windows": 1.5},
                {"defrost_off_mm": 5, "defrost_on_mm": 2}):
        code, msg = call(api + "/api/params", bad)
        assert code == 400, bad
    assert dev.params.ice_pct == 25 and dev.params.confirm_windows == 2   # rejected = unchanged
    call(api + "/api/params/defaults", {})
    assert dev.params.ice_pct == 20.0


def test_relay_endpoint_rejects_non_boolean(api):
    assert call(api + "/api/relay", {"enabled": "yes"})[0] == 400
    assert call(api + "/api/relay", {"enabled": True})[0] == 200


def test_baseline_without_auto_baseline_starts_unknown_then_works():
    dev = VirtualDevice(auto_baseline=False)
    ws = run_windows(dev, 20)
    assert {w["state_name"] for w in ws} == {"unknown"}
    assert not any(w["defrost_request"] for w in ws)
    with pytest.raises(ValueError):
        VirtualDevice(auto_baseline=False).capture_baseline()   # nothing measured yet
    dev.capture_baseline()
    assert "clear" in [w["state_name"] for w in run_windows(dev, 20)]


def test_baseline_taken_on_a_dirty_sensor_hides_the_deposit():
    """A technician capturing the baseline while iced up gets wrong results: the UI warns about it."""
    dev = VirtualDevice(auto_baseline=False)
    run_windows(dev, 150)            # iced up
    dev.capture_baseline()
    ws = run_windows(dev, 5)
    assert ws[-1]["cap_delta_pct"] < 5


def test_basic_auth(dev):
    srv, base = start(dev, password="pw")
    try:
        assert call(base + "/api/state")[0] == 401
        assert call(base + "/api/state", auth="service:wrong")[0] == 401
        assert call(base + "/api/state", auth="service:pw")[0] == 200
        assert call(base + "/api/relay", {"enabled": True})[0] == 401
    finally:
        srv.shutdown()


def test_state_file_persists_settings(tmp_path):
    f = str(tmp_path / "nvs.json")
    d1 = VirtualDevice(state_file=f)
    d1.set_relay(True)
    d1.set_params({"ice_pct": 33})
    d2 = VirtualDevice(state_file=f)
    assert d2.relay_enabled and d2.params.ice_pct == 33 and d2.boot_count == d1.boot_count + 1
