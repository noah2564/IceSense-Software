"""SQLite storage. One small class, no ORM, so the schema is easy to read and change."""
import os
import sqlite3
import threading
from contextlib import contextmanager

SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS messages (
    id          INTEGER PRIMARY KEY,
    message_id  TEXT UNIQUE NOT NULL,   -- de-duplication key
    device_id   TEXT NOT NULL,
    message_type TEXT NOT NULL,
    boot_count  INTEGER,
    seq         INTEGER,
    ts          INTEGER,                -- as sent by the device
    ts_source   TEXT,
    received_at INTEGER NOT NULL,       -- server clock, epoch ms
    direction   TEXT NOT NULL DEFAULT 'in',
    payload     TEXT NOT NULL           -- the full original JSON
);
CREATE INDEX IF NOT EXISTS idx_messages_dev_type ON messages(device_id, message_type, received_at);

-- Narrow time-series table: one row per numeric value. New schema fields show up
-- here automatically (see ingest.flatten_telemetry), no migration needed.
CREATE TABLE IF NOT EXISTS samples (
    device_id  TEXT NOT NULL,
    ts         INTEGER NOT NULL,
    channel_id TEXT NOT NULL,
    metric     TEXT NOT NULL,
    value      REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_samples ON samples(device_id, channel_id, metric, ts);

CREATE TABLE IF NOT EXISTS devices (
    device_id       TEXT PRIMARY KEY,
    first_seen      INTEGER NOT NULL,
    last_seen       INTEGER NOT NULL,
    last_boot_count INTEGER NOT NULL DEFAULT 0,
    last_seq        INTEGER NOT NULL DEFAULT -1,
    gaps            INTEGER NOT NULL DEFAULT 0,   -- messages missing within one boot
    online          INTEGER,                      -- from the retained MQTT 'online' topic
    firmware        TEXT,
    last_status     TEXT
);

CREATE TABLE IF NOT EXISTS assessments (
    device_id TEXT NOT NULL,
    ts        INTEGER NOT NULL,
    state     TEXT NOT NULL,
    details   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_assess ON assessments(device_id, ts);

-- Everything that failed validation lands here so schema mismatches are visible, not silent.
CREATE TABLE IF NOT EXISTS rejected (
    id          INTEGER PRIMARY KEY,
    received_at INTEGER NOT NULL,
    topic       TEXT,
    payload     TEXT,
    errors      TEXT
);

CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
"""


class Database:
    def __init__(self, path: str):
        if path != ":memory:":
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        with self.lock:
            if path != ":memory:":
                self.conn.execute("PRAGMA journal_mode=WAL")
            self.conn.executescript(SCHEMA_SQL)
            self.conn.commit()

    @contextmanager
    def tx(self):
        """Serialised transaction shared between the MQTT thread and API threads."""
        with self.lock:
            try:
                yield self.conn
                self.conn.commit()
            except Exception:
                self.conn.rollback()
                raise

    def query(self, sql: str, params=()) -> list[dict]:
        with self.lock:
            return [dict(r) for r in self.conn.execute(sql, params).fetchall()]

    def one(self, sql: str, params=()):
        rows = self.query(sql, params)
        return rows[0] if rows else None

    def next_server_seq(self) -> int:
        with self.tx() as c:
            row = c.execute("SELECT value FROM meta WHERE key='server_seq'").fetchone()
            n = (row["value"] + 1) if row else 0
            c.execute("INSERT OR REPLACE INTO meta(key, value) VALUES('server_seq', ?)", (n,))
            return n
