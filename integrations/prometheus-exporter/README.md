# AegisDB Prometheus exporter

A small, dependency-free sidecar that scrapes AegisDB's `stats` operation and
exposes it as Prometheus metrics on `/metrics`. On each scrape it opens one TCP
connection, issues a single `stats` request, and renders the response — it holds
no state between scrapes.

## Run

```sh
# Directly (stdlib only, no install):
AEGIS_HOST=127.0.0.1 AEGIS_PORT=9470 python3 aegis_exporter.py
# -> serving /metrics on 0.0.0.0:9471

# Exporter only — bring your own Prometheus:
docker compose --profile metrics up

# Or the whole stack — exporter + a bundled Prometheus, wired together:
docker compose --profile monitoring up
```

## Bring your own Prometheus (`--profile metrics`)

Point your Prometheus at the exporter. The target depends on where Prometheus
runs relative to the exporter:

```yaml
scrape_configs:
  - job_name: aegisdb
    static_configs:
      # host Prometheus, exporter published on host loopback (the default):
      - targets: ['127.0.0.1:9471']
      # Prometheus in the same compose network: use the service name instead:
      # - targets: ['metrics:9471']
```

Prometheus in a container scraping a host-published exporter must use host
networking (or `host.docker.internal`), because `127.0.0.1` inside the container
is the container itself, not the host.

## Bundled monitoring stack (`--profile monitoring`)

Brings up the exporter **plus** a Prometheus and a Grafana, all pre-wired:

```sh
docker compose --profile monitoring up
```

- **Prometheus** scrapes the exporter over the compose network (`metrics:9471` —
  see `prometheus.yml`), so there's no host-networking or `127.0.0.1` juggling.
  UI on `127.0.0.1:9090` (`AEGIS_PROM_BIND` / `AEGIS_PROM_HOST_PORT`). Check
  **Status → Targets**: the `aegisdb` job should read `UP`.
- **Grafana** on `127.0.0.1:3000`, auto-provisioned with the Prometheus
  datasource and the **AegisDB dashboard** (`grafana/dashboards/aegisdb.json`) —
  it opens straight onto it. Anonymous viewing is on for zero-login demos; log in
  (`AEGIS_GRAFANA_USER` / `AEGIS_GRAFANA_PASSWORD`, default `admin`/`admin`) to
  edit.

The dashboard covers status/records/uptime, request + error rates, requests by
op, dispatch latency, index entries + resident bytes (with the `--max-index-bytes`
cap line), dataset growth, log size, and — when configured — per-tenant usage and
replication lag.

To customize: edit `grafana/dashboards/aegisdb.json` (provisioned from disk,
reloaded every 30s) or edit in the Grafana UI and export the JSON back to that
file.

## Configuration (environment)

| Variable | Default | Meaning |
|---|---|---|
| `AEGIS_HOST` | `127.0.0.1` | AegisDB host to scrape |
| `AEGIS_PORT` | `9470` | AegisDB client port |
| `AEGIS_AUTH_TOKEN` | *(none)* | **Admin** token — required when the server enforces auth (`stats` is admin-scoped) |
| `AEGIS_EXPORTER_BIND` | `0.0.0.0` | Address the exporter's HTTP server binds |
| `AEGIS_EXPORTER_PORT` | `9471` | Port `/metrics` is served on |
| `AEGIS_CONNECT_TIMEOUT_MS` | `500` | Connect timeout per scrape |
| `AEGIS_READ_TIMEOUT_MS` | `2000` | Read timeout per scrape |

### Auth note

The `stats` operation is **admin-scoped**. If the server runs with auth enabled,
give the exporter an admin (global) token via `AEGIS_AUTH_TOKEN`, or every scrape
comes back `UNAUTHORIZED` and the exporter reports `aegisdb_up 0`. A scrape never
hard-fails: unreachable / timeout / auth / malformed all surface as `aegisdb_up 0`
plus an `aegisdb_scrape_error{error="…"}` sample, so you alert on
`aegisdb_up == 0`.

## Metrics

| Metric | Type | Notes |
|---|---|---|
| `aegisdb_up` | gauge | 1 if the last scrape succeeded, else 0 |
| `aegisdb_scrape_duration_seconds` | gauge | exporter self-timing |
| `aegisdb_scrape_error{error}` | gauge | present only when down; label is the reason |
| `aegisdb_info{version,phase,durability}` | gauge | constant 1 |
| `aegisdb_uptime_seconds` | gauge | |
| `aegisdb_records`, `aegisdb_tombstones` | gauge | live vs deleted-not-compacted |
| `aegisdb_log_bytes`, `aegisdb_log_flush_pending`, `aegisdb_next_id` | gauge | |
| `aegisdb_index_entries{index}` | gauge | `index` ∈ time, tags, semantic, working |
| `aegisdb_index_bytes{index}` | gauge | `index` ∈ hash, time, tag, semantic |
| `aegisdb_index_bytes_total`, `aegisdb_index_bytes_limit` | gauge | limit is `--max-index-bytes` (0 = unlimited) |
| `aegisdb_requests_total`, `aegisdb_errors_total`, `aegisdb_unauthorized_total` | counter | use `rate()` for QPS / error-rate |
| `aegisdb_dispatch_seconds_total` | counter | cumulative in-dispatch time |
| `aegisdb_requests_by_op_total{op}` | counter | per operation |
| `aegisdb_memories_merged_total` | counter | records merged away by `consolidate` (dedup) |
| `aegisdb_memories_forgotten_total` | counter | records aged out by `forget` (decay) |
| `aegisdb_memories_purged_total` | counter | records erased by `purge` (right-to-be-forgotten) |
| `aegisdb_tenant_records{namespace}`, `aegisdb_tenant_bytes{namespace}` | gauge | only when tenant quotas are configured |
| `aegisdb_replication_*{role}` | gauge | only when the node replicates (lag_bytes, connected, offsets, replicas) |

Example alerts / queries:

```promql
aegisdb_up == 0                                            # server down or unauthorized
rate(aegisdb_requests_total[5m])                           # request rate
rate(aegisdb_errors_total[5m]) / rate(aegisdb_requests_total[5m])   # error ratio
aegisdb_index_bytes_total / aegisdb_index_bytes_limit > 0.9 and aegisdb_index_bytes_limit > 0   # near the memory cap
aegisdb_replication_lag_bytes > 1e7                        # replica falling behind
```

## Tests

Stdlib `unittest`, no pytest/install:

```sh
python3 -m unittest discover -s tests/unit -p 'test_*.py'          # pure render tests
python3 -m unittest discover -s tests/integration -p 'test_*.py'   # launches ../../build/aegisdb; skips if absent
```