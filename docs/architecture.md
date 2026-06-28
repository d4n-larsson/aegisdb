# AegisDB Architecture

How AegisDB is built, as it actually exists today (version 0.1.0). This describes
the implementation — for the request/response contract see
[wire-protocol.md](wire-protocol.md), and to get running see
[quickstart.md](quickstart.md).

## Overview

AegisDB is a single-binary C server that gives AI agents durable memory. Clients
speak newline-delimited JSON over TCP (default port `9470`). The runtime pipeline
is a straight line:

```
TCP socket → NDJSON framing → request parse → query engine (router)
           → storage (append-only log + in-memory indexes)
```

The source tree mirrors that pipeline: `src/server/` (network), `src/protocol/`
(JSON parse/build), `src/query/` (router + query engine), `src/storage/` (log and
indexes), `src/memory/` (record codec + working buffer), `src/util/` (CRC32,
config). Public headers live in `include/aegisdb/`.

## Storage engine

### The log is the source of truth

All `episodic` and `semantic` records are appended to `memory.log` in the data
directory. Each record is **CRC32 + length framed**: a header carries the payload
length and a CRC32 over the payload, followed by the encoded record. Writes are
append-only — an `update` or `delete` appends a new frame rather than mutating
prior bytes.

`fsync` is batched: the log flushes every `--fsync-batch` records (default 1000),
trading a bounded window of durability for throughput.

### Crash recovery

On startup the server scans `memory.log` from the beginning, validating each
frame's CRC32 and rebuilding the in-memory indexes. If the process died
mid-append, the final frame may be torn; recovery detects this and truncates the
log back to the last valid offset (logged as `WARN … truncating torn tail: …`),
then reports `INFO … recovery complete: N records loaded`. Because every index is
derived from the log, the log alone is sufficient to reconstruct full state.

### Compaction

Updated and deleted records leave superseded frames behind. Compaction
(`src/storage/compaction.c`) rewrites the log to retain only live records,
reclaiming space from tombstones and stale versions.

### Data directory layout

```
<data-dir>/
├── memory.log     # append-only, CRC32+length framed record log (source of truth)
├── memory.index   # hash-index snapshot (id → log location)
└── metadata.db    # persisted counters (e.g. next id)
```

## Indexes

All indexes are in-memory, rebuilt from the log on startup, and guarded by a
single `pthread_rwlock_t` (see [Concurrency](#concurrency)).

| Index | Purpose | Structure |
|-------|---------|-----------|
| Hash | `get` by id | Open-addressing hash map, id → log location; snapshotted to `memory.index` |
| Time | `search` by time range | Sorted `(created, id)` array, range-scanned |
| Tag | `search` by tags | Inverted index: tag → sorted id list; `all` intersects, `any` unions |
| Semantic | `search` by embedding | Exact cosine top-K over stored float32 vectors (brute-force scan, no ANN) |

Semantic results are ranked by cosine similarity weighted by
`importance × confidence` (the weight falls back to `1.0` when both are unset).
Embedding length must equal the server's `--embedding-dim` (default 384) or the
request is rejected with `INVALID_REQUEST`.

## The record model

A `MemoryRecord` (`include/aegisdb/record.h`) carries:

- `id`, `type` (`episodic` | `semantic` | `working`)
- `created`, `updated` (epoch ms)
- `importance` (default 0.0), `confidence` (default 1.0)
- `tags` (array), `data` (opaque payload, returned as a UTF-8 string)
- `agent_id` — optional namespace
- `embedding` (+ `embedding_dim`) — optional vector
- `relationships` (+ `rel_count`) — directed edges `{ from_id, to_id, kind }`
- `expires_at` — working memory only (`0` = none)
- `deleted` — tombstone marker for compaction

### Memory types

- **Episodic** — immutable events. Once written they are never updated; `update`
  on an episodic id returns `IMMUTABLE`.
- **Semantic** — mutable facts. `update` appends a new version; the latest version
  wins.
- **Working** — volatile, RAM-held per-session entries in a ring buffer
  (`--working-capacity`, default 256) with an optional TTL. Created via `insert`
  with `type: "working"` and a `session_id`; `promote` copies one into the
  persistent log as an `episodic` or `semantic` record.

### Relationships

`relate` records a directed edge between two persisted records. Edges are stored
on the record and returned by `get`, `search`, and `traverse`. `traverse` does a
breadth-first walk of the graph from a starting record out to a given depth.

## Query engine

`src/query/query_engine.c` is the operation router. It parses the `operation`
field and dispatches to a handler: `ping`, `insert`, `get`, `update`, `delete`,
`search`, `promote`, `relate`, `traverse`. Handlers read from the in-memory
indexes (and the log for full payloads) and append to the log on writes.

### Phase gating

The server has a `--phase <1-4>` flag that caps the highest enabled feature tier;
operations above the cap return `NOT_READY`. **It defaults to 4 (everything
enabled)** and exists mainly for staged development and testing — most
deployments never set it. See the tier table in
[wire-protocol.md](wire-protocol.md#phase-gating-advanced).

## Concurrency

- A fixed **worker thread pool** (`--workers`, default 4) handles connections;
  each connection is submitted to the pool (`src/server/thread_pool.c`).
- A single **`pthread_rwlock_t`** (`index_lock`) guards the in-memory indexes and
  log reads: reads take the shared lock, writes take it exclusively. This is a
  simple, correct model rather than a lock-free or sharded design.
- `fsync` is batched as described above.

## Authentication

Authentication is **off by default** — with no `--auth-token`/`--auth-token-file`
the server serves every request and prints a startup warning. When tokens are
configured, each request must carry a matching `"token"` field; `ping` is always
exempt so health checks work unauthenticated. Tokens are compared in **constant
time**. They travel in plaintext, so run the server behind an encrypted channel
(VPN, SSH tunnel, or TLS-terminating proxy) when auth is enabled.

## Non-goals

- No SQL, no relational joins — retrieval is by id, time, tag, vector, and graph.
- No approximate nearest-neighbor index (HNSW/IVF/PQ); semantic search is exact.
- No built-in replication or sharding; AegisDB is a single-node server.
- No transport-level encryption; that is delegated to the surrounding channel.