# MiAcStats

Lightweight, zero-CGO Go dashboard and electricity cost estimator for `MiAcLocal`.

## Architecture
- **Sleep-by-default**: This Go container only needs to run when you want to inspect runtime stats or calculate electricity costs (locally via Docker Compose or scaled-to-zero on Cloud Run / Fly.io).
- **Direct Turso Cloud DB**: Uses Turso's `/v2/pipeline` HTTPS JSON API so `ESP32` writes 1-minute summaries independently even while this container is stopped, and Turso never auto-pauses when idle.
- **Non-Inverter Focus**: Tracks `COOL`, `FAN` (Low/High), `OFF`, and `OFFLINE` (DRY mode intentionally excluded). Estimates kWh from fixed nameplate wattage and supports both flat rates and progressive 6-tier EVN tariffs (with base household kWh offset).

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

