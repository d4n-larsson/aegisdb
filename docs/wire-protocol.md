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

### Authentication

Authentication is **disabled by default**: with no `--auth-token`/`--auth-token-file`
configured, every request is served and `token` is ignored. When one or more
tokens are configured, each request must carry a `token` matching one of them or
the server returns `UNAUTHORIZED`. `ping` is always exempt so liveness and
startup probes work unauthenticated. Tokens are compared in constant time and
sent in plaintext — run the server behind an encrypted channel (VPN, SSH tunnel,
or TLS-terminating proxy).

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
| `NOT_READY` | 503 | Index/feature not yet enabled (phase gating) |
| `UNAUTHORIZED` | 401 | Missing or invalid `token` when authentication is enabled |
| `INTERNAL` | 500 | Unexpected server error |

---

## Operations

### `insert` (Phase 1+)

Store a new memory record.

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
| `embedding` | number[] | No | Phase 3+ |
| `agent_id` | string | No | Phase 4+ |
| `ttl_ms` | integer | No | Working memory only |

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

---

### `get` (Phase 1+)

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

### `update` (Phase 2+ semantic only)

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

### `delete` (Phase 1+)

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

---

### `search` (Phase 2+)

Unified search with mutually combinable filters.

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
| `start_time` | integer | Unix ms inclusive |
| `end_time` | integer | Unix ms inclusive |
| `tags` | string[] | Tag filter |
| `match` | string | `all` (intersection) \| `any` (union); default `all` |
| `embedding` | number[] | Phase 3: semantic query vector |
| `type` | string | Filter by memory type |
| `agent_id` | string | Phase 4: namespace filter |
| `top_k` | integer | Max results; default 10 |

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

### `promote` (Phase 4+)

Promote working memory entry to persistent storage.

**Request**:

```json
{
  "operation": "promote",
  "session_id": "sess-abc",
  "working_id": 7,
  "to_type": "episodic"
}
```

**Response**: Same as `insert`.

---

### `relate` (Phase 4+)

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

### `ping` (Phase 1+)

Health check.

**Request**:

```json
{ "operation": "ping" }
```

**Response**:

```json
{
  "ok": true,
  "version": "0.1.0",
  "phase": 1
}
```

---

## Phase Availability Matrix

| Operation | Phase 1 | Phase 2 | Phase 3 | Phase 4 |
|-----------|---------|---------|---------|---------|
| `ping` | ✓ | ✓ | ✓ | ✓ |
| `insert` (episodic) | ✓ | ✓ | ✓ | ✓ |
| `insert` (semantic) | | ✓ | ✓ | ✓ |
| `insert` (working) | | | | ✓ |
| `get` | ✓ | ✓ | ✓ | ✓ |
| `delete` | ✓ | ✓ | ✓ | ✓ |
| `update` | | ✓ | ✓ | ✓ |
| `search` (time/tags) | | ✓ | ✓ | ✓ |
| `search` (embedding) | | | ✓ | ✓ |
| `promote` | | | | ✓ |
| `relate` | | | | ✓ |

Requests for unavailable operations return `NOT_READY`.

## Contract Tests

Contract tests in `tests/contract/` MUST validate:

1. Every operation's required fields reject missing values with `INVALID_REQUEST`.
2. Response schemas match examples above for success and error paths.
3. `NOT_FOUND` returned for unknown IDs without side effects.
4. Phase-gated operations return `NOT_READY` when disabled.
