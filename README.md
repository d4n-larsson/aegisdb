# AegisDB

A standalone C database server optimized for **AI agent memory**: append-heavy
writes, fast ID lookup, temporal/tag search, semantic similarity, and volatile
working memory. AegisDB uses a log-structured storage engine with phased indexes
(hash → B+ tree + inverted tags → HNSW-style vector search) behind a JSON-over-TCP
wire protocol.

## Features

- **Durable episodic memory** — append-only log with CRC32 framing and crash recovery
- **Semantic facts** — updateable records (latest version wins)
- **Working memory** — volatile per-session ring buffer with TTL and promotion
- **Retrieval** — lookup by ID, time-range search, tag search (`all`/`any`),
  semantic (embedding) search with importance × confidence × similarity ranking
- **Relationships** — directed edges between records and agent-namespace isolation
- **Concurrency** — worker thread pool, batched `fsync`

## Requirements

- Linux (primary target) with GCC 11+ or Clang 14+
- One of: CMake 3.20+ **or** GNU Make
- Python 3.8+ (optional, for the example client below)

## Build

### With CMake (canonical)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # runs any unit tests present
```

### With Make (no CMake required)

```bash
make            # builds build/aegisdb
make test       # builds and runs any tests/unit/*.c
make clean
```

The server binary is produced at `build/aegisdb`.

## Run

```bash
./build/aegisdb --data-dir ./data --port 9470
```

Expected startup output:

```text
[aegisdb] recovery complete: N records loaded
[aegisdb] listening on 0.0.0.0:9470
[aegisdb] data directory: ./data
```

### Configuration flags

| Flag | Default | Description |
|------|---------|-------------|
| `--data-dir <path>` | `./data` | Persistence directory |
| `--port <n>` | `9470` | TCP listen port |
| `--phase <1-4>` | `4` | Highest enabled feature phase (gates operations) |
| `--workers <n>` | `4` | Worker thread count |
| `--max-payload <bytes>` | `1048576` | Max `data` size (1 MiB) |
| `--embedding-dim <n>` | `384` | Expected embedding vector length |
| `--fsync-batch <n>` | `1000` | Records between `fsync` calls |
| `--working-capacity <n>` | `256` | Working-memory ring buffer size |
| `--help` | | Show usage |

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
`memory.log` with per-record CRC32 checksums. On startup it scans the log,
validates checksums, and rebuilds the in-memory indexes. To verify:

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