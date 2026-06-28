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
directory. Each record is framed with a 16-byte header — a **magic sync marker**,
the payload length, a **CRC32 over the payload**, and a **CRC32 over the header
itself** — followed by the encoded record. Checksumming the header separately
means a corrupt length is detected as a header fault distinct from a payload
fault, and the magic lets recovery resynchronize to the next frame after damage.
The magic also versions the format: a legacy log written by an earlier build
(8-byte `[crc][len]` header, no magic) is detected on open and migrated in place
to the current format (logged as `INFO … migrated N legacy frame(s)`). Writes are
append-only — an `update` or `delete` appends a new frame rather than mutating
prior bytes.

Durability is governed by `--durability`:

- **`sync`** — `fsync` before acknowledging every write. No acknowledged write
  is ever lost, at the cost of one `fsync` per write.
- **`batch`** — `fsync` once per `--fsync-batch` records (default 1000). Bounds
  loss by *count*, but an idle server may leave the last partial batch unflushed
  indefinitely.
- **`interval`** (default) — the maintenance thread `fsync`s at most every
  `--fsync-interval-ms` (default 1000) whenever writes are pending. Bounds loss
  by *time* regardless of write rate; resolution is floored at the ~1s
  maintenance tick.

Note that `fsync` protects against power loss and OS crashes, not a plain
process kill — the OS page cache preserves un-`fsync`'d writes across a process
restart.

### Crash recovery

On startup the server first tries to load the **index checkpoint**
(`memory.index`): a periodically-written map of `id → log location` that records
the log offset it reflects (`covered`) and the id allocator. When the checkpoint
is present and consistent (`covered ≤` the current log size), recovery trusts the
covered prefix and scans only the log **tail** written since — so a crash replays
only recent writes instead of the whole log. A missing, stale, or corrupt
checkpoint (CRC-checked) falls back to a full scan from offset 0.

The checkpoint is refreshed by the maintenance thread every `--checkpoint-sec`
(default 60; `0` disables) and on clean shutdown, and is invalidated by
compaction (which rewrites every log offset). It only carries the hash index;
the time/tag/embedding indexes are still rebuilt by reading the live records
(their payloads and vectors are needed anyway), so recovery is `O(live records +
tail)` rather than `O(entire log)`.

Either way, recovery validates each scanned frame's header and payload CRCs and
rebuilds the in-memory indexes. Two failure modes are distinguished:

- **Torn tail** — the process died mid-append, so the final frame is incomplete.
  Recovery truncates the log back to the last valid offset (logged as
  `WARN … truncating torn tail: …`). This is benign and expected.
- **Mid-stream corruption** — a frame in the interior is damaged (e.g. silent
  disk bit-rot). Rather than discarding the entire tail, recovery resynchronizes
  to the next intact frame via the magic marker and recovers the records that
  follow, logging `ERROR … skipped N corrupt frame(s)`. The damaged region is
  left in place (it is skipped again on every boot until the next compaction
  rewrites the log without it).

Recovery then reports `INFO … recovery complete: N records loaded`. Because every
index is derived from the log, the log alone is sufficient to reconstruct full
state.

### Compaction

Updated and deleted records leave superseded frames behind. Compaction
(`src/storage/compaction.c`) rewrites the log to retain only live records,
reclaiming space from tombstones and stale versions.

Compaction is **mostly concurrent**: it snapshots the live set under a brief read
lock, then copies those frames into a fresh log *without holding any lock* (the
copied region is immutable because the log is append-only, so reads and writes
proceed during the copy). Only the final step — draining frames written while the
copy ran, swapping the log in, and rebuilding the hash index — holds the write
lock, and that critical section is bounded by the writes that raced compaction
rather than by the total size of the database.

### Data directory layout

```
<data-dir>/
├── memory.log     # append-only, magic + CRC framed record log (source of truth)
├── memory.index   # index checkpoint: id → log location + covered offset + next_id
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
field and dispatches to a handler: `ping`, `stats`, `insert`, `get`, `update`,
`delete`, `search`, `promote`, `relate`, `traverse`. Handlers read from the
in-memory indexes (and the log for full payloads) and append to the log on
writes. Before dispatch it resolves the request's token to a namespace + scope
and enforces them (see [Authentication & multi-tenancy](#authentication--multi-tenancy)).

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
- `fsync` follows the configured `--durability` mode as described above; in
  `interval` mode the maintenance thread drives the periodic flush.

## Authentication & multi-tenancy

Authentication is **off by default** — with no `--auth-token`/`--auth-token-file`
the server serves every request (unrestricted) and prints a startup warning.
When tokens are configured, each request must carry a matching `"token"` field;
`ping` is always exempt so health checks work unauthenticated. Tokens are
compared in **constant time**.

Each token is bound to a **namespace** (tenant) and **scope** (`ro`/`rw`/admin)
via the token file. The query engine resolves a request's token to its identity
and then enforces it:

- writes (`insert`/`promote`) are pinned to the token's namespace (`agent_id`);
- reads (`get`/`search`/`traverse`) are filtered to it — other namespaces read
  back as `NOT_FOUND`, so existence does not leak across tenants;
- `update`/`delete`/`relate` act only within the namespace (`agent_id` is
  immutable per id, so a single ownership pre-check is sufficient and stays valid
  through the write);
- read-only tokens are refused writes with `FORBIDDEN`; `stats` is admin-only.

A global **admin** token (or auth-disabled mode) keeps the original unrestricted
behavior. This makes one server safely shareable across tenants/agents.

Tokens travel in plaintext, so run the server behind an encrypted channel (a
TLS-terminating reverse proxy, `stunnel`, or a private network) when auth is
enabled. TLS is deliberately kept out of the binary — see Non-goals.

## Non-goals

- No SQL, no relational joins — retrieval is by id, time, tag, vector, and graph.
- No approximate nearest-neighbor index (HNSW/IVF/PQ); semantic search is exact.
- No built-in replication or sharding; AegisDB is a single-node server.
- No transport-level encryption; that is delegated to the surrounding channel.