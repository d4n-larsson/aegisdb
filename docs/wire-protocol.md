# AegisDB Wire Protocol Contract

**Version**: 1.0.0 (draft)  
**Transport**: TCP  
**Framing**: Newline-delimited JSON (NDJSON) — one JSON object per line per request/response  
**Default port**: `9470`  
**Encoding**: UTF-8

## Connection Lifecycle

1. Client opens TCP connection to server.
2. Client sends one JSON request line terminated by `\n`.
3. Server responds with one JSON response line terminated by `\n`.
4. Connection may remain open for multiple request/response cycles (pipelining allowed).

## Common Request Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `operation` | string | Yes | Operation name (see below) |
| `request_id` | string | No | Client correlation ID echoed in response |
| `token` | string | When auth enabled | Bearer token. Required on every operation except `ping` when the server is started with `--auth-token`/`--auth-token-file`; ignored when authentication is disabled |
| `include_embeddings` | boolean | No | Defaults to `true`. Set `false` to omit the (large) `embedding`/`embeddings` arrays from every record in the response — all other fields are unchanged. Embeddings dominate response size (a 384-dim vector is ~8 KB of JSON per record), so recall/read clients that only need the payload and metadata can cut latency and bandwidth. Honored by `get`, `search`, `traverse`, `insert`, `update`, and `promote`. |

### Authentication & multi-tenancy

Authentication is **disabled by default**: with no `--auth-token`/`--auth-token-file`
configured, every request is served with unrestricted access and `token` is
ignored. When one or more tokens are configured, each request (except `ping`)
must carry a `token` matching one of them or the server returns `UNAUTHORIZED`.
Tokens are compared in constant time.

Each token is bound to a **namespace** (tenant) and a **scope**, configured in
the token file (`--auth-token-file`), one token per line:

```
# <token>            -> global admin (any namespace, all operations)
# <token> <ns>       -> bound to namespace <ns>, read+write
# <token> <ns> ro    -> bound to namespace <ns>, read-only
# <token> <ns> rw    -> bound to namespace <ns>, read+write (explicit)
# <token> admin      -> global admin
s3cr3t-admin
acme-key   acme   rw
acme-view  acme   ro
beta-key   beta   rw
```

`--auth-token <tok>` on the command line registers a **global admin** token.

**Tokens hashed at rest.** A token field may be stored as a SHA-256 digest
instead of plaintext, so a leaked token file does not reveal usable secrets.
Generate the hashed form with `--hash-token` and paste it in place of the
plaintext token:

```
$ aegisdb --hash-token s3cr3t-acme-key
sha256$2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae
```
```
sha256$2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae acme rw
```

Clients still send the **plaintext** token on the wire; the server hashes it and
compares (in constant time) against the stored digest. Because bearer tokens are
high-entropy secrets, an unsalted hash is sufficient — generate them randomly
(e.g. `openssl rand -hex 32`).

For a **namespaced** token, the server enforces tenant isolation:

- **`insert`** / **`promote`** store the record with `agent_id` set to the
  token's namespace; any client-supplied `agent_id` is ignored.
- **`get`** / **`search`** / **`traverse`** are restricted to the namespace; a
  record in another namespace reads back as `NOT_FOUND` (existence does not leak).
- **`update`** / **`delete`** / **`relate`** act only on records in the
  namespace; otherwise `NOT_FOUND`. `relate` requires *both* endpoints in it.
- A **read-only** (`ro`) token is rejected with `FORBIDDEN` on any write
  (`insert`, `update`, `delete`, `promote`, `relate`).
- **`stats`** is admin-only; a namespaced token receives `FORBIDDEN`.

A **global admin** token (or auth-disabled mode) keeps the original behavior:
unrestricted access, with `agent_id` chosen freely by the client.

Tokens travel in plaintext, so run the server behind an encrypted channel — a
TLS-terminating reverse proxy (nginx/Caddy), `stunnel`, or a VPN/private network.
TLS is intentionally not built into the binary (it keeps AegisDB a single,
dependency-free C binary); terminate it at the edge.

## Common Response Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ok` | boolean | Yes | `true` on success, `false` on error |
| `request_id` | string | No | Echo of client `request_id` |
| `error` | object | On failure | `{ "code": string, "message": string }` |

### Error Codes

| Code | HTTP Analog | Description |
|------|-------------|-------------|
| `INVALID_REQUEST` | 400 | Malformed JSON or missing required fields |
| `NOT_FOUND` | 404 | Memory ID does not exist |
| `PAYLOAD_TOO_LARGE` | 413 | `data` exceeds limit |
| `IMMUTABLE` | 409 | Update attempted on episodic record |
| `NOT_READY` | 503 | Operation disabled by `--phase` gating (advanced; see below) |
| `UNAUTHORIZED` | 401 | Missing or invalid `token` when authentication is enabled |
| `FORBIDDEN` | 403 | Authenticated, but the token's scope/namespace disallows the operation |
| `QUOTA_EXCEEDED` | 507 | Write would push the tenant over its `--tenant-max-records`/`--tenant-max-bytes` cap |
| `RATE_LIMITED` | 429 | Tenant exceeded its `--tenant-rate-qps` request rate |
| `READ_ONLY` | 405 | Write attempted on a read-only replica (`--replicate-from`/`--read-only`); write to the primary |
| `MEMORY_LIMIT` | 507 | Insert refused: in-RAM index size reached `--max-index-bytes`. Free memory (delete/compact) or raise the cap. Reads, deletes, updates, and working-memory inserts are unaffected. |
| `INTERNAL` | 500 | Unexpected server error |

---

## Operations

### `insert`

Store a new memory record. To create **working** memory, set `type` to `working`
and include a `session_id` (and optionally `ttl_ms`); the returned record's `id`
is what you later pass to `promote` as `working_id`.

**Request**:

```json
{
  "operation": "insert",
  "type": "episodic",
  "tags": ["user", "preference"],
  "importance": 0.7,
  "confidence": 1.0,
  "data": "User likes coffee",
  "agent_id": "agent-001"
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `type` | string | Yes | `episodic` \| `semantic` \| `working` |
| `data` | string | Yes | Payload |
| `tags` | string[] | No | |
| `importance` | number | No | 0.0–1.0 |
| `confidence` | number | No | 0.0–1.0 |
| `embedding` | number[] | No | A single vector; length must equal the server's `--embedding-dim` (default 384) or the request is rejected with `INVALID_REQUEST` |
| `embeddings` | number[][] | No | Multiple vectors for one record (each `--embedding-dim` long, up to 64), stored and returned together. Use instead of `embedding`. Semantic search scores the record by its best-matching vector (best-of-N) and returns it once. |
| `agent_id` | string | No | Namespace the record to an agent; scopes `get`/`search`/`traverse` |
| `session_id` | string | Working only | Required to create working memory |
| `ttl_ms` | integer | No | Time-to-live in ms. For episodic/semantic, an opt-in TTL: the record is archived (hidden from recall, then reclaimed) once `created + ttl_ms` passes; omit or `0` = never expires. For working memory, its buffer expiry. |

**Response (success)**:

```json
{
  "ok": true,
  "record": {
    "id": 42,
    "type": "episodic",
    "created": 1719400000123,
    "updated": 1719400000123,
    "importance": 0.7,
    "confidence": 1.0,
    "tags": ["user", "preference"],
    "data": "User likes coffee"
  }
}
```

A `MemoryRecord` also carries `agent_id`, `embedding`, and `relationships` (an
array of `{ "from_id", "to_id", "kind" }`) when those are set. `relationships`
is populated by `relate` and returned by `get`, `search`, and `traverse`.

**Batch insert**: supply a `records` array (each element a record body as above,
up to 1000) instead of a single record. Every element is validated first, so a
malformed element rejects the whole batch before anything is written; the
response returns the inserted records and a count:

```json
{ "operation": "insert", "records": [ {…}, {…} ] }
→ { "ok": true, "count": 2, "records": [ {…}, {…} ] }
```

---

### `get`

Retrieve a memory by ID.

**Request**:

```json
{
  "operation": "get",
  "id": 42
}
```

**Response (success)**:

```json
{
  "ok": true,
  "record": { "...": "full MemoryRecord" }
}
```

**Response (not found)**:

```json
{
  "ok": false,
  "error": { "code": "NOT_FOUND", "message": "Memory 42 not found" }
}
```

---

### `update` (semantic only)

Update a semantic memory record.

**Request**:

```json
{
  "operation": "update",
  "id": 42,
  "data": "User prefers espresso",
  "confidence": 0.9,
  "tags": ["user", "preference"]
}
```

**Response**: Same shape as `insert` with updated `record`.

**Errors**: `IMMUTABLE` if `id` refers to episodic record.

---

### `delete`

Soft-delete a memory record by id. The record is tombstoned in the log and
dropped from the secondary indexes, so it no longer appears in `get`, `search`,
or `traverse`. Works for both episodic and semantic records.

**Request**:

```json
{
  "operation": "delete",
  "id": 42
}
```

**Response**:

```json
{
  "ok": true,
  "id": 42,
  "deleted": true
}
```

**Errors**: `NOT_FOUND` if `id` is unknown or already deleted (delete is
idempotent in effect — a second call reports `NOT_FOUND`).

**Delete by query**: omit `id` and supply filters (`tags`/`type`/`start_time`
+`end_time`) to delete every matching record; at least one filter is required
(an unfiltered delete is refused with `INVALID_REQUEST`). A namespaced token
only deletes its own records. The response reports the count:

```json
{ "operation": "delete", "tags": ["scratch"], "match": "any" }
→ { "ok": true, "deleted": 7 }
```

---

### `count`

Count live records matching the filters (`tags`/`type`/`start_time`+`end_time`/
`agent_id`/`max_importance`, same semantics as `search`), without returning the
records. A namespaced token counts only its own.

```json
{ "operation": "count", "tags": ["user"], "match": "all" }
→ { "ok": true, "count": 42 }
```

A broad, filterless count (no selective filter, or a wide-open time range) is
bounded to the most-recent `--query-scan-cap` records (default 100000) so it
can't load the whole dataset into memory. When that cap truncates the scan the
count is a floor and the response adds `"capped": true`; add a selective filter
(e.g. tags) for an exact count.

---

### `consolidate`

Merge near-duplicate **semantic** memories. Records whose embeddings are within
`min_similarity` cosine of each other (default `0.95`) collapse into a single
survivor — the most-recently-updated member — which absorbs the others' tags
and relationships and the max importance/confidence; the rest are tombstoned.
Requires `rw` scope; a namespaced token consolidates only its own tenant.
Episodic memory (the immutable log) is never touched. Idempotent.

```json
{ "operation": "consolidate", "min_similarity": 0.95 }
→ { "ok": true, "clusters": 12, "merged": 34 }
```

`clusters` is the number of duplicate groups collapsed; `merged` is how many
records were tombstoned. There is no LLM summarization — this is mechanical
dedup only. Use a conservative threshold: too low merges genuinely distinct
memories.

**Provenance:** before a loser is tombstoned, the survivor records a
`supersedes` relationship pointing at it, so a merge is auditable lineage rather
than silent data loss — the memory inspector (and `get`/`search`, which return
`relationships`) can show exactly what a surviving memory absorbed.

---

### `forget`

Decay-based forgetting: a maintenance pass that tombstones **aging, low-value**
records so a long-running corpus (and its in-RAM indexes) plateaus instead of
growing without bound. A record's **retention** is

```
retention = importance × 0.5^(age / half_life_ms)      age measured from `updated`
```

and it is forgotten when `retention < min_retention`. High-importance and
recently-touched records survive; old, low-importance ones age out. Requires `rw`
scope; a namespaced token forgets only its own tenant. Idempotent. Forgotten
records reclaim disk on the next compaction, exactly like TTL expiry.

```json
{ "operation": "forget", "half_life_ms": 604800000, "min_retention": 0.05 }
→ { "ok": true, "scanned": 1840, "forgotten": 1712, "dry_run": false }
```

| Field | Type | Notes |
|-------|------|-------|
| `half_life_ms` | integer | Recency half-life; default 7 days. Floored at 1s. |
| `min_retention` | number | Forget when `retention` falls below this; default `0.05` |
| `type` | string | Which type to sweep; **default `episodic`** — the high-volume, low-individual-value events. Curated `semantic` facts are protected unless you name the type explicitly |
| `dry_run` | boolean | Default `false`. When `true`, counts what *would* be forgotten and tombstones nothing — preview a policy before applying it |
| `max_forget` | integer | Safety cap on deletions this pass (`0`/absent = unbounded) |

`scanned` is how many records of the target type were examined; `forgotten` is
how many crossed the threshold (tombstoned, or counted under `dry_run`). This is
the mechanical importance×recency policy; model-driven "is this still relevant?"
judgment belongs in a client/maintenance job on top.

---

### `search`

Unified search with mutually combinable filters. The time filter activates only
when **both** `start_time` and `end_time` are present.

**Request (time range)**:

```json
{
  "operation": "search",
  "start_time": 1719400000000,
  "end_time": 1719500000000,
  "type": "episodic",
  "top_k": 100
}
```

**Request (tags)**:

```json
{
  "operation": "search",
  "tags": ["user", "preference"],
  "match": "all",
  "top_k": 50
}
```

| Field | Type | Notes |
|-------|------|-------|
| `start_time` | integer | Unix ms inclusive; pair with `end_time` |
| `end_time` | integer | Unix ms inclusive; pair with `start_time` |
| `tags` | string[] | Tag filter |
| `match` | string | `all` (intersection) \| `any` (union); default `all` |
| `embedding` | number[] | Semantic query vector; ranked by cosine similarity weighted by importance × confidence |
| `type` | string | Filter by memory type |
| `agent_id` | string | Namespace filter |
| `top_k` | integer | Max results; default 10 |
| `offset` | integer | Skip this many top-ranked results (pagination); default 0 |
| `min_score` | number | Semantic only: drop matches below this cosine similarity ([-1, 1]) |
| `half_life_ms` | integer | Semantic only: recency half-life — multiply each score by `0.5^(age/half_life)`, age measured from `updated`; 0/absent = no decay |
| `max_importance` | number | Keep only records with `importance` ≤ this value (candidate selection) |
| `order` | string | `oldest` \| `recent` (default). Non-semantic only: when a bounded time scan truncates, `oldest` keeps the aging tail instead of the most-recent records — so a large namespace still surfaces its oldest candidates |
| `explain` | boolean | Defaults to `false`. When `true`, each returned record gains an `explain` object with the per-hit ranking breakdown (see below) so a client/inspection UI can show *why* a memory surfaced |

`max_importance` combined with `type` + a time range and `order: "oldest"` is how
a summarization/maintenance job selects the oldest, lowest-value records to
distill (server-side, so the client doesn't over-fetch and filter). `order` has
no effect on semantic (embedding) queries, which rank by similarity.

**Ranking explanation (`explain: true`)**: each record carries an `explain`
object so retrieval is inspectable rather than a black box:

```json
{
  "explain": {
    "semantic": true,
    "score": 0.8945,
    "similarity": 0.9939,
    "importance": 0.9,
    "confidence": 1.0,
    "weight": 0.9,
    "recency_factor": 1.0
  }
}
```

For semantic queries `score == weight × similarity × recency_factor`, where
`weight = importance × confidence` (or `1.0` if that product is ≤ 0) and
`recency_factor = 0.5^(age/half_life)` (`1.0` when no `half_life_ms` is set).
`similarity` is the raw cosine ([-1, 1]). For non-semantic queries `semantic` is
`false`, `similarity`/`score` are `0`, and results are ordered by time, not score.

**Response (success)**:

```json
{
  "ok": true,
  "records": [ { "...": "MemoryRecord" } ],
  "total": 2
}
```

Empty result:

```json
{
  "ok": true,
  "records": [],
  "total": 0
}
```

---

### `promote`

Promote a working-memory entry to persistent storage.

**Request**:

```json
{
  "operation": "promote",
  "session_id": "sess-abc",
  "working_id": 7,
  "to_type": "episodic"
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `working_id` | integer | Yes | `id` of the working record to promote |
| `session_id` | string | No | Session the working record belongs to |
| `to_type` | string | No | `episodic` (default) or `semantic` |

**Response**: Same as `insert` (the new persisted `record`).

---

### `relate`

Add a relationship between two persisted records.

**Request**:

```json
{
  "operation": "relate",
  "from_id": 42,
  "to_id": 99,
  "kind": "derived_from"
}
```

**Response**:

```json
{
  "ok": true,
  "relationship": {
    "from_id": 42,
    "to_id": 99,
    "kind": "derived_from"
  }
}
```

---

### `traverse`

Breadth-first walk of the relationship graph from a starting record, returning
the records reached within `depth` hops.

**Request**:

```json
{
  "operation": "traverse",
  "id": 42,
  "depth": 2,
  "agent_id": "agent-001"
}
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `id` | integer | Yes | Starting record |
| `depth` | integer | No | Max hops to follow; default `1` |
| `agent_id` | string | No | Restrict the walk to one namespace |

**Response**: Same shape as `search` — `{ "ok": true, "records": [ … ], "total": N }`.

---

### `ping`

Health check. Always exempt from authentication.

**Request**:

```json
{ "operation": "ping" }
```

**Response**:

```json
{
  "ok": true,
  "version": "0.1.0",
  "phase": 4
}
```

`version` is the server version; `phase` echoes the server's `--phase` setting
(default `4`).

---

### `stats`

Operational snapshot for monitoring and capacity planning. Requires
authentication when enabled (unlike `ping`). Available at every phase.

**Request**:

```json
{ "operation": "stats" }
```

**Response**:

```json
{
  "ok": true,
  "version": "0.1.0",
  "phase": 4,
  "uptime_ms": 38214,
  "durability": "interval",
  "fsync_interval_ms": 1000,
  "records": 1042,
  "tombstones": 17,
  "log_bytes": 2310544,
  "log_flush_pending": false,
  "indexes": { "time": 1042, "tags": 88, "semantic": 1042, "working": 3 },
  "next_id": 1060
}
```

| Field | Meaning |
|-------|---------|
| `uptime_ms` | Milliseconds since the database finished recovery at startup |
| `durability` | Active durability mode (`sync`, `batch`, or `interval`) |
| `fsync_batch` / `fsync_interval_ms` | The tuning value for the active mode (only the relevant one is present; `sync` has neither) |
| `records` | Live (non-deleted) persisted records |
| `tombstones` | Deleted-but-not-yet-compacted records still in the log |
| `log_bytes` | Current size of `memory.log` on disk |
| `log_flush_pending` | `true` if writes have not yet been `fsync`'d — the current durability lag |
| `indexes` | Per-index entry counts (`semantic` is the brute-force vector count; watch it for scale) |
| `memory` | Approximate resident bytes per in-RAM index — `hash_bytes`, `time_bytes`, `tag_bytes`, `semantic_bytes`, `index_bytes_total`, and `index_bytes_limit` (the configured `--max-index-bytes` cap; 0 = unlimited). Indexes are held in memory and grow with the dataset (the semantic vectors usually dominate), so this is the figure to monitor/alert on; past the limit inserts return `MEMORY_LIMIT`. Excludes allocator overhead. |
| `next_id` | The id the next persisted insert will receive |
| `metrics` | Monotonic operational counters since startup (below) |

The `metrics` object holds cumulative counters — scrapers compute rates from
successive differences:

| Field | Meaning |
|-------|---------|
| `requests` | Total requests dispatched |
| `errors` | Responses with `ok: false` |
| `unauthorized` | Auth rejections (a subset of `errors`) |
| `dispatch_micros` | Cumulative in-dispatch time in µs (avg latency = `dispatch_micros / requests`) |
| `by_op` | Per-operation request counts (`insert`, `search`, …, `other`) |

When any per-tenant limit is configured (`--tenant-max-records`,
`--tenant-max-bytes`, or `--tenant-rate-qps`), the response also carries a
`tenant_limits` object (the configured caps) and a `tenants` array of
`{ "namespace", "records", "bytes" }` — each namespace's current live usage
against the caps, for capacity planning:

```json
"tenant_limits": { "max_records": 100000, "max_bytes": 0, "rate_qps": 50 },
"tenants": [ { "namespace": "acme", "records": 1042, "bytes": 2310544 } ]
```

When the node participates in replication, `stats` also carries a `replication`
object. On a **primary**: `{ "role":"primary", "replicas":N, "log_generation":G }`.
On a **replica**: `{ "role":"replica", "connected":bool, "applied_offset":N,
"primary_offset":N, "lag_bytes":N }` — the byte lag behind the primary. A replica
answers reads (`get`/`search`/`traverse`/`count`) but returns `READ_ONLY` to
writes; see [read-replica-design.md](read-replica-design.md).

`stats` is admin-only (a namespaced token gets `FORBIDDEN`), so metrics are
server-wide. AegisDB has no HTTP endpoint by design — a sidecar can poll `stats`
and translate to Prometheus, the same way TLS is terminated by a proxy.

---

### `snapshot`

Take a consistent **online backup** without stopping the server. Admin-only (a
namespaced or read-only token gets `FORBIDDEN`). Because the log is append-only,
a snapshot is just its durable prefix plus a fresh `metadata.db` (the `next_id`
floor) and a `manifest.json`; the derived index checkpoints are omitted and
rebuilt on restore. An in-flight compaction cannot interfere, and concurrent
writes simply land past the captured offset.

**Request**:

```json
{ "operation": "snapshot", "name": "nightly-2026-07-05" }
```

`name` is optional (defaults to `snap-<epoch_ms>`) and must be a single path
component — a value containing `/` or `..` returns `INVALID_REQUEST`.

**Response**:

```json
{
  "ok": true,
  "snapshot": "./data/snapshots/nightly-2026-07-05",
  "log_size": 2310544,
  "record_count": 1042,
  "next_id": 1060,
  "created_ms": 1783236416709
}
```

The snapshot directory (`<data-dir>/snapshots/<name>/`) is a self-contained,
restorable data set: `memory.log`, `metadata.db`, and `manifest.json`.

**Restore** with the one-shot `--restore` mode — it validates the manifest
(format and `embedding-dim` must match), refuses to overwrite an existing
database, and installs the log + metadata into an empty data dir:

```sh
# server must be stopped (or restore into a different, empty --data-dir)
aegisdb --restore /backups/nightly-2026-07-05 --data-dir ./data --embedding-dim 1024
aegisdb --data-dir ./data --embedding-dim 1024   # start; recovery rebuilds indexes
```

`--embedding-dim` must match the value the snapshot was taken with (recorded in
the manifest); a mismatch is rejected. Since a snapshot is a log prefix,
truncating its `memory.log` to an earlier frame boundary before restoring yields
a point-in-time restore.

---

### `token_list` / `token_add` / `token_revoke`

Manage accepted tokens at runtime, without restarting — for onboarding/
offboarding tenants on a shared server. **Admin-only** (a global/unrestricted
token; a namespaced token gets `FORBIDDEN`). Changes are persisted back to
`--auth-token-file` (all entries rewritten hashed); if no token file was
configured they apply in-memory only (`"persisted": false`) and are lost on
restart. Tokens are referenced by a **fingerprint** `id` (first 12 hex of the
token's SHA-256) so they can be listed and revoked without exposing the secret.

```json
{ "operation": "token_list", "token": "<admin>" }
→ { "ok": true, "tokens": [ { "id": "caa0cd7de01a", "namespace": "acme", "scope": "rw" } ] }
```

`token_add` binds a `namespace` + `scope` (`ro`|`rw`, or `admin` for a global
token — which ignores `namespace`). Supply the secret as `new_token`, or omit it
to have the server mint one (returned **once** as `token`):

```json
{ "operation": "token_add", "namespace": "acme", "scope": "rw", "token": "<admin>" }
→ { "ok": true, "id": "3f9c…", "token": "9f3c… (minted, shown once)", "persisted": true }
```

`token_revoke` removes the token with the given `id`; it stops authenticating
immediately. Returns `NOT_FOUND` if no token has that id.

```json
{ "operation": "token_revoke", "id": "3f9c…", "token": "<admin>" }
→ { "ok": true, "revoked": true, "persisted": true }
```

---

## Phase gating (advanced)

By default the server enables every operation (`--phase 4`). The `--phase <1-4>`
flag exists mainly for staged development and testing: it caps the highest
enabled feature tier, and any operation above that tier returns `NOT_READY`.
Most deployments never set it and never see `NOT_READY`.

| Tier | Adds |
|------|------|
| 1 | `ping`, `get`, `delete`, episodic `insert` |
| 2 | semantic `insert`, `update`, `search` by time/tags |
| 3 | `search` by embedding |
| 4 | working memory + `insert`, `promote`, `relate`, `traverse`, `agent_id` namespaces |
