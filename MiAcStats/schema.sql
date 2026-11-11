-- Turso / SQLite schema for MiAcStats telemetry and billing configuration.
-- DRY mode is intentionally counted as COOL.
-- When the AC is powered off, no rows are recorded.

CREATE TABLE IF NOT EXISTS ac_buckets (
    device_id          TEXT    NOT NULL,
    ts_utc             INTEGER NOT NULL,
    session_start_utc  INTEGER NOT NULL,
    duration_sec       INTEGER NOT NULL,
    cool_sec           INTEGER NOT NULL,
    fan_low_sec        INTEGER NOT NULL,
    fan_high_sec       INTEGER NOT NULL,
    offline_sec        INTEGER NOT NULL DEFAULT 0,
    room_temp_avg      REAL,
    room_temp_min      REAL,
    room_temp_max      REAL,
    ac_temp_avg        REAL,
    target_temp        INTEGER,
    auto_sec           INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, ts_utc)
);

CREATE INDEX IF NOT EXISTS idx_ac_buckets_ts ON ac_buckets (ts_utc);
CREATE INDEX IF NOT EXISTS idx_ac_buckets_session ON ac_buckets (device_id, session_start_utc);

CREATE TABLE IF NOT EXISTS app_config (
    key        TEXT PRIMARY KEY,
    value      TEXT    NOT NULL,
    updated_at INTEGER NOT NULL
);
