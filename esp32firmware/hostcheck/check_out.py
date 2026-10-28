import json, sys, collections
from jsonschema import Draft202012Validator
schema = json.load(open("../../schema/ice-sensing.schema.json"))
v = Draft202012Validator(schema)
bad = 0; types = collections.Counter(); msgs = []
for line in open("out.tsv"):
    topic, payload = line.rstrip("\n").split("\t", 1)
    m = json.loads(payload)
    errs = list(v.iter_errors(m))
    suffix = topic.rsplit("/", 1)[1]
    if errs or suffix != m["message_type"] or topic.split("/")[2] != m["device_id"]:
        bad += 1; print("INVALID", topic, [e.message[:120] for e in errs[:3]])
    types[m["message_type"]] += 1; msgs.append(m)
print("messages:", dict(types), "invalid:", bad)
seqs = [m["seq"] for m in msgs]
print("seq strictly increasing from 0:", seqs == list(range(len(seqs))))
print("buffered flags:", sum(1 for m in msgs if m.get("buffered")), "of", len(msgs))
print("ts_source:", {m["ts_source"] for m in msgs})
tel = [m for m in msgs if m["message_type"] == "telemetry"]
def cap(m): return next(c for c in m["telemetry"]["channels"] if c["channel_id"] == "cap-1")
print("telemetry with delta_pct:", sum("delta_pct" in cap(m)["capacitive"] for m in tel), "of", len(tel))
print("last cap delta_pct:", cap(tel[-1])["capacitive"].get("delta_pct"))
print("sample_rate after set_config:", tel[-1]["telemetry"]["window"]["sample_rate_hz"], "config_version:", tel[-1]["config_version"])
t = next(c for c in tel[-1]["telemetry"]["channels"] if c["channel_id"] == "temp-coil")
print("last temp channel quality:", t["quality"])
for m in msgs:
    if m["message_type"] == "command_ack":
        a = m["command_ack"]; print("ack", a["command_id"], a["result"], a.get("error_code", ""), a.get("message", ""))
    if m["message_type"] == "calibration":
        c = m["calibration"]; print("calibration", c["channel_id"], c["result"]["baseline_value"], c["result"]["baseline_unit"])
print("biggest message bytes:", max(len(json.dumps(m, separators=(',', ':'))) for m in msgs))
