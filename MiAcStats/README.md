# MiAcStats

Lightweight, zero-CGO Go dashboard and electricity cost estimator for `MiAcLocal`.

## Architecture
- **Sleep-by-default**: This Go container only needs to run when you want to inspect runtime stats or calculate electricity costs (locally via Docker Compose or scaled-to-zero on Cloud Run / Fly.io).
- **Direct Turso Cloud DB**: Uses Turso's `/v2/pipeline` HTTPS JSON API so `ESP32` writes 1-minute summaries independently even while this container is stopped, and Turso never auto-pauses when idle.
- **Hourly Bucket Rollup**: High-resolution 1-minute telemetry is kept for the most recent $N$ days (`ROLLUP_KEEP_DAYS`, default 7). Older raw buckets are automatically aggregated by `(device_id, session_start_utc, hour)` into `ac_buckets_hourly`, reducing row count by ~60x while preserving exact session counts, daily summaries, 24-hour distribution charts, total runtime, kWh, and EVN costs.
- **Non-Inverter Focus**: Tracks `COOL`, `FAN` (Low/High), `OFF`, and `OFFLINE` (DRY mode intentionally excluded). Estimates kWh from fixed nameplate wattage and supports both flat rates and progressive 6-tier EVN tariffs (with base household kWh offset).

## Bucket Rollup Configuration & CLI
The rollup process runs automatically as a background task upon server startup (subject to `ROLLUP_MIN_INTERVAL_HOURS`), or can be run on-demand via CLI:

| Environment Variable | Default | Description |
| --- | --- | --- |
| `ROLLUP_ENABLED` | `true` | Enables or disables the rollup subsystem. |
| `ROLLUP_KEEP_DAYS` | `7` | Days to keep raw 1-minute buckets (minimum 3). |
| `ROLLUP_MIN_INTERVAL_HOURS` | `24` | Minimum interval between automatic background rollup runs. |
| `ROLLUP_DRY_RUN` | `false` | When true, runs aggregation and integrity verification without advancing watermark or deleting raw data. |

### Standalone Rollup CLI
Useful for running as a scheduled cron job (e.g. Dokploy / crontab):
```sh
# Run one-off rollup and exit
./miacstats rollup

# Force rollup even if minimum interval has not elapsed
./miacstats rollup --force

# Dry-run verification without mutating raw data
./miacstats rollup --dry-run
```

## Quick Start (Docker)
1. Copy `.env.example` to `.env` and set `TURSO_DATABASE_URL` + `TURSO_AUTH_TOKEN` (or `DEMO_MODE=true` for preview).
2. Run:
   ```sh
   docker compose up --build
   ```
3. Open `http://localhost:8080`.
4. Stop when done (`docker compose down`).

## Local Development / Tests
```sh
go test ./...
go run ./cmd/miacstats
```

