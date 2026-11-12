"""Checks what the host run of the real firmware did. Exit code 1 on any failure."""
import json, sys, collections
from jsonschema import Draft202012Validator

fails = []
def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok: fails.append(what)

# ---- MQTT messages: schema-valid, ordered
schema = json.load(open("../../schema/ice-sensing.schema.json"))
v = Draft202012Validator(schema)
msgs = []
for line in open("out.tsv"):
    topic, payload = line.rstrip("\n").split("\t", 1)
    m = json.loads(payload)
    errs = list(v.iter_errors(m))
    if errs: print("  invalid:", topic, errs[0].message[:150])
    msgs.append((topic, m, errs))
check(msgs and not any(e for _, _, e in msgs), f"all {len(msgs)} MQTT messages valid against the schema")
check(all(t.rsplit("/", 1)[1] == m["message_type"] and t.split("/")[2] == m["device_id"] for t, m, _ in msgs), "topic matches message_type and device_id")
check([m["seq"] for _, m, _ in msgs] == list(range(len(msgs))), "seq strictly increasing from 0")
tel = [m for _, m, _ in msgs if m["message_type"] == "telemetry"]
check(290 <= len(tel) <= 330, f"about one telemetry message per 2 s window ({len(tel)})")
status = next((m["status"] for _, m, _ in msgs if m["message_type"] == "status"), None)
cap_status = next((s for s in status["sensors"] if s["channel_id"] == "cap-1"), {}) if status else {}
check(cap_status.get("baseline_valid") is False, "persisted zero baseline is reported invalid after reboot")

# ---- on-device detection produced the expected cycle
seq = []
for m in tel:
    s = m["telemetry"]["ice_assessment"]["state"]
    if not seq or seq[-1] != s: seq.append(s)
print("  state sequence:", " > ".join(seq))
for want in ("clear", "condensation", "frost", "ice", "melting"):
    check(want in seq, f"state '{want}' reached")
check(any(m["telemetry"]["ice_assessment"]["defrost_recommended"] for m in tel), "defrost recommended during ice")
check(max(m["telemetry"]["ice_assessment"]["thickness_mm_est"] for m in tel) > 3, "thickness estimate rises above 3 mm")
events = [m["event"] for _, m, _ in msgs if m["message_type"] == "event"]
types = collections.Counter(e["event_type"] for e in events)
check(types["ice_state_changed"] >= 5 and types["defrost_recommended"] >= 2 and types["baseline_updated"] == 2, f"events: {dict(types)}")

# ---- commands over MQTT
acks = {m["command_ack"]["command_id"]: m["command_ack"] for _, m, _ in msgs if m["message_type"] == "command_ack"}
check(acks.get("c1", {}).get("result") == "completed", "set_config with algorithm.parameters acknowledged")
check(acks.get("c2", {}).get("result") == "completed", "set_output relay acknowledged")

# ---- web API
rows = [l.rstrip("\n").split("\t", 5) for l in open("http.tsv")]
def find(t, method, path, auth="auth"):
    return next(r for r in rows if int(r[0]) == t and r[1] == method and r[2] == path and r[3] == auth)
def state_at(t): return json.loads(find(t, "GET", "/api/state")[5])
check(find(6, "GET", "/api/state", "noauth")[4] == "401", "request without credentials is refused (401)")
for path in ("/generate_204", "/hotspot-detect.html", "/connecttest.txt"):
    row = find(0, "GET", path, "noauth")
    check(row[4] == "302" and "Location: /" in row[5], f"{path} redirects unauthenticated clients to /")
redirect = find(0, "GET", "/redirect", "noauth")
check(redirect[4] == "302" and "Location: http://www.msftconnecttest.com/" in redirect[5],
      "/redirect sends unauthenticated clients to the Microsoft connectivity test URL")
check(find(7, "GET", "/")[4] == "200" and "gz" in find(7, "GET", "/")[5], "page served")
check(find(8, "POST", "/api/params")[4] == "200", "valid parameter change accepted")
bad = [r for r in rows if r[2] == "/api/params" and r[4] == "400"]
check(len(bad) == 3, "invalid parameter changes rejected (wrong type, unknown name, hysteresis violated)")
check(find(11, "POST", "/api/relay")[4] == "400", "relay request with non-boolean rejected")
check(state_at(12)["relay"] == {"enabled": False, "on": False}, "relay disabled by default")
check(state_at(60)["defrost_request"] is False, "no defrost request while clear")
s450 = state_at(450)
check(s450["defrost_request"] and s450["relay"] == {"enabled": True, "on": True}, "relay closes on defrost request once enabled")
check(s450["state_name"] == "ice" and s450["thickness_calibrated"] is False, "state 'ice', thickness flagged uncalibrated")
s630 = state_at(630)
check(s630["state_name"] == "clear" and not s630["relay"]["on"], "cycle ends clear, relay open")
csv = find(630, "GET", "/api/history.csv")[5]
check(csv.startswith("uptime_s,state,state_name,thickness_mm,defrost_request,cap_delta_pct,res_log10,temp_c,cap_counts"), "history CSV has the agreed columns")
print("biggest MQTT message bytes:", max(len(json.dumps(m, separators=(',', ':'))) for _, m, _ in msgs))
sys.exit(1 if fails else 0)
