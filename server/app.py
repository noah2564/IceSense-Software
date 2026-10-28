"""FastAPI application: MQTT ingest in the background, REST API + dashboard in the foreground.

Run:  uvicorn app:app --port 8000
Env:  MQTT_HOST (localhost) MQTT_PORT (1883) MQTT_USER MQTT_PASS MQTT_TLS=1
      DB_PATH (data/ice.db) SCHEMA_PATH (../schema/ice-sensing.schema.json) MQTT_ENABLED=0 to disable
"""
import json
import logging
import os
import time
import uuid
from contextlib import asynccontextmanager
from typing import Callable, Optional

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from db import Database
from ingest import Ingestor, load_validator

HERE = os.path.dirname(os.path.abspath(__file__))
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")


class CommandRequest(BaseModel):
    command_type: str
    params: Optional[dict] = None
    config: Optional[dict] = None
    ttl_s: int = Field(300, ge=10, le=86400, description="Device ignores the command after this time")


def create_app(db_path: Optional[str] = None, schema_path: Optional[str] = None,
               mqtt_enabled: Optional[bool] = None,
               publisher: Optional[Callable[[str, str], bool]] = None) -> FastAPI:
    db_path = db_path or os.environ.get("DB_PATH", os.path.join(HERE, "data", "ice.db"))
    schema_path = schema_path or os.environ.get(
        "SCHEMA_PATH", os.path.join(HERE, "..", "schema", "ice-sensing.schema.json"))
    if mqtt_enabled is None:
        mqtt_enabled = os.environ.get("MQTT_ENABLED", "1") != "0"

    db = Database(db_path)
    validator = load_validator(schema_path)
    ingestor = Ingestor(db, validator)
    state = {"publish": publisher, "link": None}

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        if mqtt_enabled:
            from mqtt_link import MqttLink
            link = MqttLink(
                os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883")),
                ingestor.handle, os.environ.get("MQTT_USER", ""), os.environ.get("MQTT_PASS", ""),
                tls=os.environ.get("MQTT_TLS", "0") == "1")
            link.start()
            state["link"] = link
            state["publish"] = lambda topic, payload: link.publish(topic, payload)
        yield
        if state["link"]:
            state["link"].stop()

    app = FastAPI(title="Ice sensing server", lifespan=lifespan)
    app.state.db, app.state.ingestor = db, ingestor

    def require_device(device_id: str) -> dict:
        row = db.one("SELECT * FROM devices WHERE device_id=?", (device_id,))
        if not row:
            raise HTTPException(404, "unknown device")
        return row

    # ------------------------------------------------------------------ read API
    @app.get("/api/devices")
    def devices():
        out = []
        for d in db.query("SELECT * FROM devices ORDER BY device_id"):
            a = db.one("SELECT ts, state FROM assessments WHERE device_id=? ORDER BY ts DESC LIMIT 1",
                       (d["device_id"],))
            out.append({
                "device_id": d["device_id"], "online": None if d["online"] is None else bool(d["online"]),
                "last_seen": d["last_seen"], "gaps": d["gaps"], "firmware": d["firmware"],
                "state": a["state"] if a else None, "state_ts": a["ts"] if a else None,
            })
        return out

    @app.get("/api/devices/{device_id}/channels")
    def channels(device_id: str):
        require_device(device_id)
        return db.query("SELECT DISTINCT channel_id, metric FROM samples WHERE device_id=? "
                        "ORDER BY channel_id, metric", (device_id,))

    @app.get("/api/devices/{device_id}/series")
    def series(device_id: str, channel_id: str, metric: str, since_ms: int = 0,
               limit: int = Query(2000, le=20000)):
        require_device(device_id)
        rows = db.query(
            "SELECT ts, value FROM (SELECT ts, value FROM samples WHERE device_id=? AND channel_id=? "
            "AND metric=? AND ts>=? ORDER BY ts DESC LIMIT ?) ORDER BY ts",
            (device_id, channel_id, metric, since_ms, limit))
        return [[r["ts"], r["value"]] for r in rows]

    @app.get("/api/devices/{device_id}/assessments")
    def assessments(device_id: str, limit: int = Query(20, le=500)):
        require_device(device_id)
        rows = db.query("SELECT ts, state, details FROM assessments WHERE device_id=? "
                        "ORDER BY ts DESC LIMIT ?", (device_id, limit))
        return [{"ts": r["ts"], "state": r["state"], **json.loads(r["details"])} for r in rows]

    @app.get("/api/devices/{device_id}/status")
    def device_status(device_id: str):
        d = require_device(device_id)
        return {"last_seen": d["last_seen"], "status": json.loads(d["last_status"]) if d["last_status"] else None}

    @app.get("/api/devices/{device_id}/messages")
    def messages(device_id: str, message_type: str = "", limit: int = Query(20, le=500)):
        require_device(device_id)
        types = [t for t in message_type.split(",") if t]
        sql = "SELECT received_at, direction, message_type, payload FROM messages WHERE device_id=?"
        params: list = [device_id]
        if types:
            sql += f" AND message_type IN ({','.join('?' * len(types))})"
            params += types
        rows = db.query(sql + " ORDER BY id DESC LIMIT ?", (*params, limit))
        return [{"received_at": r["received_at"], "direction": r["direction"],
                 "message_type": r["message_type"], "message": json.loads(r["payload"])} for r in rows]

    @app.get("/api/rejected")
    def rejected(limit: int = Query(20, le=200)):
        rows = db.query("SELECT * FROM rejected ORDER BY id DESC LIMIT ?", (limit,))
        return [{**r, "errors": json.loads(r["errors"])} for r in rows]

    # ------------------------------------------------------------------ commands (server -> device)
    @app.post("/api/devices/{device_id}/commands", status_code=202)
    def send_command(device_id: str, req: CommandRequest):
        require_device(device_id)
        now = int(time.time() * 1000)
        cmd = {
            "command_id": "cmd-" + uuid.uuid4().hex[:12], "command_type": req.command_type,
            "issued_ts": now, "expires_ts": now + req.ttl_s * 1000, "priority": "normal", "issued_by": "server",
        }
        if req.params:
            cmd["params"] = req.params
        if req.config:
            cmd["config"] = req.config
        msg = {
            "schema_version": "1.0.0", "message_type": "command",
            "message_id": "srv-" + uuid.uuid4().hex[:12], "device_id": device_id,
            "seq": db.next_server_seq(), "ts": now, "ts_source": "server_sync", "command": cmd,
        }
        errors = list(validator.iter_errors(msg))
        if errors:      # never publish something the device would have to reject
            raise HTTPException(422, [f"{'/'.join(map(str, e.absolute_path))}: {e.message[:200]}"
                                      for e in errors[:5]])
        publish = state["publish"]
        if publish is None:
            raise HTTPException(503, "MQTT not connected")
        if not publish(f"ice/v1/{device_id}/cmd", json.dumps(msg)):
            raise HTTPException(502, "publish failed")
        ingestor.store_outgoing(msg)
        return {"command_id": cmd["command_id"], "expires_ts": cmd["expires_ts"]}

    # ------------------------------------------------------------------ dashboard
    app.mount("/static", StaticFiles(directory=os.path.join(HERE, "static")), name="static")

    @app.get("/", include_in_schema=False)
    def index():
        return FileResponse(os.path.join(HERE, "static", "index.html"))

    return app


app = create_app() if os.environ.get("ICE_NO_AUTOCREATE") != "1" else None
