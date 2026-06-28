# AegisDB

A standalone C database server optimized for **AI agent memory**: append-heavy
writes, fast ID lookup, temporal/tag search, semantic similarity, and volatile
working memory. AegisDB uses a log-structured storage engine with purpose-built
indexes (hash for ID lookup, a sorted time index, inverted tags, and exact-cosine
vector search) behind a JSON-over-TCP wire protocol.

## Features

- **Durable episodic memory** — append-only log with magic + CRC32 framing, corruption-resilient recovery, and legacy-log migration
- **Semantic facts** — updateable records (latest version wins)
- **Working memory** — volatile per-session ring buffer with TTL and promotion
- **Retrieval** — lookup by ID, time-range search, tag search (`all`/`any`),
  semantic (embedding) search ranked by cosine similarity weighted by
  importance × confidence
- **Relationships** — directed edges between records, graph traversal, and
  agent-namespace isolation
- **Multi-tenant auth** — optional bearer tokens (constant-time check; `ping` exempt), each bound to a namespace + scope (`ro`/`rw`/admin) so one server safely isolates many tenants
- **Concurrency** — worker thread pool; selectable `fsync` durability (`sync` / `batch` / `interval`)

## Requirements

- Linux (primary target) with GCC 11+ or Clang 14+
- One of: CMake 3.20+ **or** GNU Make
- Python 3.8+ (optional, for the example client below)

## Build

### With CMake (canonical)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # runs the unit test suite
```

### With Make (no CMake required)

```bash
make             # builds build/aegisdb
make test        # builds and runs the C unit tests
make integration # wire-protocol contract tests (launches the server)
make check       # unit + integration
make clean
```

The server binary is produced at `build/aegisdb`.

### With Docker

A multi-stage `Dockerfile` (Debian-slim) builds the server and ships a minimal
runtime image. Data persists in a named volume mounted at `/data`.

```bash
# Build and run with Docker Compose (recommended)
docker compose up --build        # serves on localhost:9470

# Or build and run the image directly
docker build -t aegisdb .
docker run -p 9470:9470 -v aegis-data:/data aegisdb
```

The image ships a `HEALTHCHECK` that uses the binary's built-in `--health-check`
probe (no extra tooling in the image), so `docker ps` and Compose
`depends_on: condition: service_healthy` reflect real server liveness.

The container runs as an unprivileged user and listens on `0.0.0.0:9470`.
Override the default flags by appending them to the run command, e.g.
`docker run aegisdb --embedding-dim 1024`, or by editing the `command:` block in
`docker-compose.yml`. To enable authentication, mount a token file into `/data`
and pass `--auth-token-file` (see [Authentication](#authentication)); the wire
protocol is plaintext, so keep the port on a trusted network.

## Run

```bash
./build/aegisdb --data-dir ./data --port 9470
```

Expected startup output:

```text
2026-06-28 12:00:00.000 INFO  [aegisdb] AegisDB 0.1.0 starting (log level: info)
2026-06-28 12:00:00.000 WARN  [aegisdb] no auth tokens configured; ...
2026-06-28 12:00:00.000 INFO  [aegisdb] recovery complete: N records loaded
2026-06-28 12:00:00.000 INFO  [aegisdb] listening on 0.0.0.0:9470
2026-06-28 12:00:00.000 INFO  [aegisdb] data directory: ./data
```

Logs go to stderr as `<timestamp> <LEVEL> [aegisdb] <message>`. Control the
verbosity with `--log-level error|warn|info|debug` (default `info`) or the
`AEGISDB_LOG_LEVEL` environment variable — the flag takes precedence. At
`debug`, the server logs every accepted connection and dispatched operation.

The `WARN` line appears only when the server is started without
`--auth-token`/`--auth-token-file` (see [Authentication](#authentication)).

### Configuration flags

| Flag | Default | Description |
|------|---------|-------------|
| `--data-dir <path>` | `./data` | Persistence directory |
| `--port <n>` | `9470` | TCP listen port |
| `--phase <1-4>` | `4` | Highest enabled feature phase (gates operations) |
| `--workers <n>` | `4` | Worker thread count |
| `--max-payload <bytes>` | `1048576` | Max `data` size (1 MiB) |
| `--embedding-dim <n>` | `384` | Expected embedding vector length |
| `--durability <mode>` | `interval` | `sync` (fsync per write), `batch` (per `--fsync-batch` records), or `interval` (per `--fsync-interval-ms`) |
| `--fsync-batch <n>` | `1000` | Records between `fsync` calls in `batch` mode |
| `--fsync-interval-ms <n>` | `1000` | Flush cadence in `interval` mode (floored at the ~1s maintenance tick) |
| `--checkpoint-sec <n>` | `60` | Index checkpoint cadence so recovery replays only the tail; `0` disables |
| `--working-capacity <n>` | `256` | Working-memory ring buffer size |
| `--log-level <level>` | `info` | `error`, `warn`, `info`, or `debug` (also `$AEGISDB_LOG_LEVEL`) |
| `--auth-token <token>` | — | Accept this global **admin** token (repeatable) |
| `--auth-token-file <path>` | — | Accept tokens, one per line: `<token> [namespace] [ro\|rw\|admin]`; a token may be `sha256$<hex>` (hashed at rest) |
| `--hash-token <token>` | | Print the token's `sha256$<hex>` form and exit (paste into the token file) |
| `--health-check` | | Probe a local server on `--port`, print nothing, exit 0 if healthy / 1 otherwise |
| `--help` | | Show usage |

### Authentication & multi-tenancy

With no `--auth-token`/`--auth-token-file`, the server runs **without
authentication** and every request is served with unrestricted access. When
tokens are configured, each request must carry a matching `"token"` field
(except `ping`, which is always exempt) or the server returns `UNAUTHORIZED`.
Tokens are compared in constant time.

Each token in the token file is bound to a **namespace** and a **scope**:

```
admin-key                 # global admin: any namespace, all operations
acme-key      acme   rw   # tenant "acme", read+write
acme-view     acme   ro   # tenant "acme", read-only
beta-key      beta   rw   # tenant "beta"
```

A namespaced token can only write into its own namespace (`agent_id` is pinned
automatically) and only read its own records — another tenant's records read
back as `NOT_FOUND`. Read-only tokens are refused writes with `FORBIDDEN`, and
`stats` is admin-only. This lets one server back many isolated tenants/agents.

Tokens can be **stored hashed** so a leaked token file reveals nothing usable.
Run `aegisdb --hash-token <tok>` to get its `sha256$<hex>` form and put that in
the token file; clients still send the plaintext token, which the server hashes
and compares in constant time. Use high-entropy tokens (`openssl rand -hex 32`).

Tokens are sent in plaintext, so run the server behind an encrypted channel — a
TLS-terminating reverse proxy (nginx/Caddy), `stunnel`, or a private network.
TLS is intentionally kept out of the binary to preserve the single,
dependency-free build.

## Wire Protocol

Newline-delimited JSON (NDJSON) over TCP — one JSON object per line per
request/response. See [`docs/wire-protocol.md`](docs/wire-protocol.md)
for the full contract.

```bash
# Health check
echo '{"operation":"ping"}' | nc -q1 localhost 9470

# Insert an episodic memory
echo '{"operation":"insert","type":"episodic","tags":["user","preference"],"data":"User likes coffee"}' | nc -q1 localhost 9470

# Retrieve by ID
echo '{"operation":"get","id":1}' | nc -q1 localhost 9470

# Time-range search
echo '{"operation":"search","start_time":0,"end_time":9999999999999,"top_k":10}' | nc -q1 localhost 9470

# Tag search
echo '{"operation":"search","tags":["user"],"match":"all","top_k":10}' | nc -q1 localhost 9470
```

Supported operations: `ping`, `insert` (episodic/semantic/working), `get`,
`update` (semantic), `delete`, `search` (time/tags/embedding), `promote`,
`relate`, `traverse`.

### Python client example

```python
import socket, json

def request(payload: dict) -> dict:
    with socket.create_connection(("localhost", 9470)) as s:
        s.sendall((json.dumps(payload) + "\n").encode())
        return json.loads(s.recv(65536).decode())

print(request({"operation": "ping"}))
print(request({"operation": "insert", "type": "episodic",
               "tags": ["demo"], "data": "Hello from quickstart"}))
```

## Project Layout

```text
src/
├── main.c              # Entry point, CLI args
├── server/             # TCP NDJSON server + worker thread pool
├── protocol/           # JSON request parsing / response building
├── query/              # Operation router / query engine
├── storage/            # Append-only log, hash/time/tag/semantic indexes,
│                       #   compaction, recovery
├── memory/             # MemoryRecord encode/decode, working buffer
└── util/               # CRC32, config
include/aegisdb/        # Public headers
tests/                  # unit/, integration/, contract/
third_party/            # Vendored cJSON and Unity
data/                   # Runtime data (gitignored)
```

## Crash Recovery

AegisDB persists `episodic` and `semantic` records to an append-only
`memory.log` with per-frame header and payload CRC32 checksums. On startup it
loads the index checkpoint (`memory.index`) and replays only the log tail written
since — falling back to a full scan if the checkpoint is missing or corrupt —
then rebuilds the in-memory indexes. A torn tail from a mid-write crash is
trimmed; interior corruption is skipped frame by frame so the surrounding records
still load. To verify: 

1. Insert several records.
2. `kill -9 <pid>` the server.
3. Restart it — startup logs `recovery complete: N records loaded`.
4. `get` each ID; all records return intact.

## Use as Claude Code memory

AegisDB can act as the persistent long-term memory of [Claude Code](https://claude.com/claude-code)
via the integration in [`integrations/claude-code/`](integrations/claude-code/):
an MCP server exposing memory tools plus hooks for automatic recall and capture.
See [`integrations/claude-code/README.md`](integrations/claude-code/README.md) for
the step-by-step setup (start AegisDB → register the MCP server → enable the hooks).

## Documentation

- Wire protocol: [`docs/wire-protocol.md`](docs/wire-protocol.md)
- Quickstart: [`docs/quickstart.md`](docs/quickstart.md)
- Architecture: [`docs/architecture.md`](docs/architecture.md)