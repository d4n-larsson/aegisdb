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
| `embedding` | number[] | No | Length must equal the server's `--embedding-dim` (default 384) or the request is rejected with `INVALID_REQUEST` |
| `agent_id` | string | No | Namespace the record to an agent; scopes `get`/`search`/`traverse` |
| `session_id` | string | Working only | Required to create working memory |
| `ttl_ms` | integer | Working only | Expiry for working memory |

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
`agent_id`, same semantics as `search`), without returning the records. A
namespaced token counts only its own.

```json
{ "operation": "count", "tags": ["user"], "match": "all" }
→ { "ok": true, "count": 42 }
```

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

`stats` is admin-only (a namespaced token gets `FORBIDDEN`), so metrics are
server-wide. AegisDB has no HTTP endpoint by design — a sidecar can poll `stats`
and translate to Prometheus, the same way TLS is terminated by a proxy.

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
