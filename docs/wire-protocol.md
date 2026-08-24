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

Records returned by `get` and `search` additionally carry **usage feedback** when
the server tracks it (it does unless started with `--no-usage-feedback`):

| Field | Type | Notes |
|-------|------|-------|
| `recall_count` | integer | Times this record has been returned by a `search` that did not opt out. `0` means tracked but never recalled; the field is **absent** when the server keeps no counters at all, so the two cases are distinguishable |
| `last_recalled` | integer | Unix ms of the most recent recall; absent until the first one |

`get` reports these but does not increment them — fetching a known id is not
retrieval, and counting it would let a tool that walks ids inflate every record.
| `tags` | string[] | No | |
| `importance` | number | No | 0.0–1.0 |
| `confidence` | number | No | 0.0–1.0 |
| `embedding` | number[] | No | A single vector; length must equal the server's `--embedding-dim` (default 384) or the request is rejected with `INVALID_REQUEST` |
| `embeddings` | number[][] | No | Multiple vectors for one record (each `--embedding-dim` long, up to 64), stored and returned together. Use instead of `embedding`. Semantic search scores the record by its best-matching vector (best-of-N) and returns it once. |
| `agent_id` | string | No | Namespace the record to an agent; scopes `get`/`search`/`traverse` |
| `session_id` | string | Working only | Required to create working memory |
| `fact` | object | No | A machine-readable assertion alongside the prose in `data`: `{"s": <record id>, "p": "<predicate>", "o": "<literal>" \| {"id": <record id>}}`. See below |
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

**Why a record is believed (`explain.derivation`)**: with `"explain": true`, a
record the inference job derived carries a `derivation` alongside the ranking
breakdown — the shallowest `depth`, and each `route` with the `rule` that fired
and its `premises`, every premise marked `live` or not. One level, not a tree: a
premise that is itself derived carries its own `derivation` when fetched, so a
client walks the chain by following ids. An asserted record has no `derivation`
field at all.

**Typed facts (`fact`)**: a record's `data` is prose, which can be searched by
its words but not *asked* a question. An optional `fact` adds the same claim in a
form a machine can match on — subject, predicate, object — without replacing the
prose, which stays what a human or a model reads. Neither is derived from the
other; a writer supplies both.

```json
{ "operation": "insert", "type": "semantic",
  "data": "The recall hook defaults to embedding_mode=none.",
  "fact": { "s": 42, "p": "defaults_to", "o": "none" } }
```

| Position | Notes |
|----------|-------|
| `s` | The **record id** the fact is about. Entities are records: to say something about "the recall hook", insert a record for it first (conventionally a `semantic` record tagged `entity`) and use its id |
| `p` | The predicate, at most 64 bytes. A longer one is `INVALID_REQUEST` — it is the limit the fact indexes can intern, and an un-internable predicate would make the fact unreachable by any `pattern` naming it. When the server runs with `--predicate-registry`, it must also be **declared** there, with the matching object kind |
| `o` | Either a **literal string**, or `{"id": N}` to reference another record. A bare number is rejected: it would be ambiguous between an id and a literal, and numeric literals do not exist |

The fact is echoed back by `insert`, `get`, `search`, and `traverse` exactly as
written, and **absent entirely** on a record that carries none.

Three deliberate properties:

- **A fact is immutable.** `update` changes tags and the payload and leaves the
  fact untouched. Changing what a record asserts is a supersession, not an edit,
  so it leaves an auditable chain rather than rewriting what the record used to
  claim.
- **The referenced ids need not exist.** A fact may name a subject or object
  written later — or never. This keeps a batch that inserts an entity *and* a
  fact about it in one request from being refused, and costs nothing in
  isolation terms: the fact lives on the asserting record, so a `pattern` search
  still returns only records the caller owns.
- **The fact is stored even under `--no-fact-index`.** The record keeps what it
  asserts; only the lookups go away.

**Materializing the closures (`--inference`)**: off by default. With it set,
a background pass draws what the registry declares — transitive joins,
symmetric reversals, `inverse_of` pairs — and writes each conclusion as an
ordinary `semantic` record carrying the concluded `fact`, a synthesized
payload, and `derived_from` edges to every premise. Conclusions are namespaced
to their premises, so a rule never joins one tenant's fact to another's, and
a replica never derives: it receives its primary's conclusions through the
replication stream like any other record.

The same pass also reports **contradictions** the registry makes
deterministic: two live values for a `cardinality: one` predicate, or two
predicates declared `mutex_with` each other both holding of one subject. Both
records gain a `conflicts_with` edge to the other and the pair is counted in
`stats.conflicts`. Nothing is tombstoned and nothing is reranked — choosing
which of two conflicting facts survives needs to know which is newer, which
source is better, or what the world is actually like, none of which the server
knows. What it guarantees is that the contradiction is *found*, with no model
call.

Closure takes several passes rather than one — a conclusion becomes the next
pass's premise — and each pass is bounded by `--inference-max-derived`
(records written), `--inference-max-candidates` (conclusions considered, which
is what actually bounds a tick) and `--inference-max-depth` (chain length).
See `docs/inference-design.md`.

**The predicate vocabulary (`--predicate-registry <file>`)**: optional, and off
by default — with no registry, any predicate is accepted. With one, a `fact` is
refused (`INVALID_REQUEST`) unless its predicate is declared *and* its object
matches the declared kind. The point is that a write path which mints predicates
freely produces a vocabulary nothing can reason over; the registry is the
contract that prevents it.

```json
{
  "defaults_to":    { "object": "string", "cardinality": "one" },
  "part_of":        { "object": "id", "transitive": true, "inverse_of": "contains" },
  "contains":       { "object": "id", "inverse_of": "part_of" },
  "conflicts_with": { "object": "id", "symmetric": true }
}
```

`object` (`"id"` or `"string"`) is **required** — a predicate that accepted
either would make a `{p, o}` lookup mean two different things. `cardinality`,
`symmetric`, `transitive`, `inverse_of` and `mutex_with` are declared here and
validated for coherence, but nothing acts on them yet.

The file is validated at **startup**, and a problem is a startup *failure* with
the offending predicate named — not a fallback to accepting everything, which
would be the opposite of what configuring a registry asks for. An unknown key, a
duplicate declaration, a one-sided `inverse_of`, or `symmetric`/`transitive` on a
literal-valued predicate are all refused: each is a typo that would otherwise
become a property that silently does not apply. `stats` reports
`indexes.registered_predicates` so a loaded vocabulary is confirmable from
outside (`0` means none configured).

**`derivation` is not a client field**: a record derived by the inference job
(ROADMAP 5.3) carries one, and `insert` and `update` both refuse a request that
supplies one, with `INVALID_REQUEST`. Provenance a client could author would be
provenance nobody could trust. An explicit `null` is treated as absent, as
elsewhere.

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

**Point-in-time (`as_of`)**: pass `as_of` (epoch ms) to get the record **as it was
at that time** — the version with the greatest `updated` ≤ `as_of`, reconstructed
from the log. Returns `NOT_FOUND` if the record did not yet exist, or had been
deleted, by then. Absent `as_of` = the current version.

```json
{ "operation": "get", "id": 42, "as_of": 1719400000000 }
```

---

### `history`

The full version trail of a record (ROADMAP 3.1) — the audit answer to "what did
the agent know, and when?". Returns every version still in the log, in causal
(append) order, each annotated with its validity interval and `deleted` flag.
Namespace-scoped like `get` (a cross-tenant id reads as `NOT_FOUND`).

```json
{ "operation": "history", "id": 42 }
→ { "ok": true, "id": 42, "count": 3, "versions": [
     { "...": "MemoryRecord", "valid_from": 1719400000000,
       "valid_to": 1719400005000, "deleted": false },
     { "...": "MemoryRecord", "valid_from": 1719400005000,
       "valid_to": 0, "deleted": false }
   ] }
```

Each version's `valid_from` is its `updated`; `valid_to` is the next version's
`updated` (`0` = still current). The final entry is a tombstone (`deleted:true`)
if the record was deleted.

> **History depth is bounded by compaction.** These reads reconstruct from the
> on-disk log, and compaction reclaims superseded versions and tombstones — so
> after a compaction pass, history reflects only what the log still holds
> (typically the current version). For a durable archival trail, snapshot before
> compacting, or defer compaction. This is point-in-time reconstruction over the
> live log, not an immutable audit ledger.

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
retention = importance × 0.5^(age / half_life_ms) × use_boost

age        measured from the last recall, or `updated` if never recalled
use_boost  1 + usage_weight × (1 − 1 / (1 + recall_count))     (saturating)
```

and it is forgotten when `retention < min_retention`. High-importance and
recently-touched records survive; old, low-importance ones age out.

**Usage feedback** is what the last two terms add: `importance` is a number the
writer guessed once, while a record's recall history is evidence of what is
actually being used. So recency is measured from the last *recall* rather than
the last write — a fact written a year ago and recalled yesterday is live
knowledge — and a frequently-recalled record earns a bounded retention boost, at
most `1 + usage_weight`. The boost saturates on purpose: recall count is evidence
of use, not proof of value, and an unbounded multiplier would pin whatever
happens to be hot. Set `usage_weight: 0` to ignore usage entirely and score
exactly as this op did before the feature; a server started with
`--no-usage-feedback` keeps no counters, so it always behaves that way. Requires `rw`
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
| `usage_weight` | number | How much recall history protects a record; default `1.0` (a well-used record can be worth up to twice an equivalent unused one). `0` disables usage weighting. Negative is rejected |
| `max_forget` | integer | Safety cap on deletions this pass (`0`/absent = unbounded) |

`scanned` is how many records of the target type were examined; `forgotten` is
how many crossed the threshold (tombstoned, or counted under `dry_run`). This is
the mechanical importance×recency policy; model-driven "is this still relevant?"
judgment belongs in a client/maintenance job on top.

---

### `export`

Export a **subject's** records — the "export what you store about me" side of data
compliance. Returns the live records owned by a namespace, in id order, paginated.
The subject is the token's namespace (namespaced token) or an admin-specified
`agent_id`; a subjectless export is refused (no "dump the whole DB"). Read-only;
`ro` tokens may call it.

```json
{ "operation": "export", "agent_id": "acme", "limit": 100, "after_id": 0 }
→ { "ok": true, "namespace": "acme", "records": [ ... ], "count": 100,
    "cursor": 342, "has_more": true }
```

| Field | Type | Notes |
|-------|------|-------|
| `agent_id` | string | Subject to export; ignored for a namespaced token (pinned to its own namespace). Required for an admin/no-auth caller |
| `limit` | integer | Max records this page; default 100, capped at 1000 |
| `after_id` | integer | Return records with id greater than this — pass the previous page's `cursor` to page through everything |
| `include_embeddings` | boolean | As elsewhere; set `false` to omit vectors |

Page until `has_more` is `false`, passing `cursor` as the next `after_id`.

---

### `purge`

**Right to be forgotten**: hard-delete every record owned by a namespace and make
the payloads actually leave disk. The records are tombstoned and then compaction
rewrites the log without them, so a purged subject's plaintext is gone from
`memory.log` (not merely hidden). Requires `rw` scope; a namespaced token purges
only its own namespace, an admin targets one via `agent_id`. A subjectless purge
is refused. Requires at least phase 1.

```json
{ "operation": "purge", "agent_id": "acme", "dry_run": false, "compact": true }
→ { "ok": true, "namespace": "acme", "purged": 42, "dry_run": false, "compacted": true }
```

| Field | Type | Notes |
|-------|------|-------|
| `agent_id` | string | Namespace to erase; ignored for a namespaced token. Required for admin/no-auth |
| `dry_run` | boolean | Default `false`. When `true`, counts what *would* be purged and deletes nothing (preview) |
| `compact` | boolean | Default `true`. Run compaction after the purge so payloads leave the on-disk log. Set `false` to defer to a scheduled/batched compaction |

`purged` is the number of records tombstoned; `compacted` is whether the reclaim
pass ran. Deferring compaction leaves the plaintext recoverable from the log
until the next compaction, so keep it on for a true erase.

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
| `pattern` | object | Filter by the typed fact a record asserts (ROADMAP 5.2); see below. Returns `NOT_READY` if the server runs with `--no-fact-index` |
| `subsume` | boolean | With a bound `pattern.s`, also match facts about anything that reaches it through `is_a` (ROADMAP 5.3). Off by default; see below |
| `query` | string | Lexical query text; ranked by Okapi BM25 over record payloads. Combine with `embedding` for hybrid retrieval (see below). Returns `NOT_READY` if the server runs with `--no-lexical-index` |
| `type` | string | Filter by memory type |
| `agent_id` | string | Namespace filter |
| `top_k` | integer | Max results; default 10 |
| `offset` | integer | Skip this many top-ranked results (pagination); default 0 |
| `min_score` | number | Semantic only: drop matches below this cosine similarity ([-1, 1]) |
| `half_life_ms` | integer | Semantic only: recency half-life — multiply each score by `0.5^(age/half_life)`, age measured from `updated`; 0/absent = no decay |
| `max_importance` | number | Keep only records with `importance` ≤ this value (candidate selection) |
| `order` | string | `oldest` \| `recent` (default). Non-semantic only: when a bounded time scan truncates, `oldest` keeps the aging tail instead of the most-recent records — so a large namespace still surfaces its oldest candidates |
| `track_usage` | boolean | Defaults to `true`. Counts the returned records as recalled, feeding the usage counters `forget` weighs. Set `false` when the caller is inspecting rather than recalling — the bundled inspector does, so browsing memories does not protect them from `forget` |
| `explain` | boolean | Defaults to `false`. When `true`, each returned record gains an `explain` object with the per-hit ranking breakdown (see below) so a client/inspection UI can show *why* a memory surfaced |

`max_importance` combined with `type` + a time range and `order: "oldest"` is how
a summarization/maintenance job selects the oldest, lowest-value records to
distill (server-side, so the client doesn't over-fetch and filter). `order` has
no effect on ranked (embedding or `query`) searches, which rank by relevance.

**Lexical search (`query`)**: matches the words a record's payload actually
contains, ranked by BM25. Identifier shape is preserved by the tokenizer, so
`--tenant-max-records`, `hnsw.c:214` and `AEGIS_RECALL_TOP_K` are each findable
by their exact spelling — the rare tokens a dense embedding averages away. A
compound term is additionally indexed by its parts (`tenant`, `max`, `records`),
so one word finds the flag. Matching is case-insensitive.

```json
{ "operation": "search", "query": "--tenant-max-records", "top_k": 5 }
```

**Hybrid search (`query` + `embedding`)**: the two result lists are fused by
reciprocal rank — each record scores `Σ 1/(60 + rank)` over the lists it appears
in. Ranks are fused rather than scores because a cosine in [-1,1] and an
unbounded BM25 score share no scale, and normalising them per query would make
one record's score depend on the rest of the batch.

```json
{ "operation": "search", "query": "CRC framing", "embedding": [ 0.01, "..." ],
  "top_k": 10, "explain": true }
```

Two behaviours differ in hybrid mode, both deliberate:

- `min_score` gates the **semantic** side only (it is a cosine floor), applied
  before fusion — so it means the same thing it does in a semantic-only query.
- `importance × confidence` weighting and `half_life_ms` recency decay are **not
  applied**. Reciprocal-rank scores are near-uniform by construction (adjacent
  ranks differ by under 2%), so any multiplier with a wider spread stops being a
  modifier and becomes the primary sort key — measurably costing recall@1. A
  fused query therefore ranks on the fusion alone and reports `weight` and
  `recency_factor` as `1.0`. Use a single-source query when that shaping is what
  you want.

**Pattern filter (`pattern`)**: matches on the machine-readable `fact` a record
carries (see `insert`), rather than on its words. Bind any non-empty subset of
the three positions; omit a position, or set it to `"*"`, to leave it free.

```json
{ "operation": "search", "pattern": { "s": 42, "p": "defaults_to" }, "top_k": 20 }
```

| Pattern | Question it answers |
|---------|--------------------|
| `{"s": 42}` | everything asserted about record 42 |
| `{"s": 42, "p": "defaults_to"}` | what 42 defaults to |
| `{"p": "defaults_to"}` | every record using that predicate |
| `{"o": "none"}` | every record asserting the literal `none` |
| `{"p": "part_of", "o": {"id": 99}}` | everything declared part of record 99 |

An **all-wildcard pattern is refused** with `INVALID_REQUEST` — it is a scan of
every fact wearing a filter's clothes. A record that asserts no fact never
matches any pattern. A literal object and an id reference are never confused:
`{"o": "99"}` and `{"o": {"id": 99}}` are different queries.

`pattern` **intersects** with the ordinary filters (`type`, `tags`,
`agent_id`, the time range) rather than replacing them, and namespace scoping
applies as everywhere else — the fact indexes are server-wide, but a pattern
search returns only records the caller owns.

It is a **filter, not a ranking**: on its own it orders by time and reports
`explain.semantic`/`lexical` as `false`. Combined with `embedding` or `query`
the ranked index still chooses the candidates and the pattern removes those that
do not assert the right thing.

**Subsumption (`subsume`)**: a fact about a member also answers a question
about the category it belongs to. With `subsume: true` and a bound
`pattern.s`, the subject matches that record *or* anything that reaches it
through the `is_a` taxonomy:

```json
{ "operation": "search", "pattern": {"s": 7, "p": "defaults_to"},
  "subsume": true }
```

Off by default, because it changes what a pattern *means* — a caller asking
what record 7 defaults to should not silently receive an answer about a
different record.

It requires `--inference` and reports `NOT_READY` without it: the expansion
reads the `is_a` closure the job materializes, and without that closure it would
reach direct members only — a partial answer indistinguishable from a narrow
one. That same closure is why it reaches a descendant any number of levels down
for the cost of one index lookup rather than a graph walk.

The expansion is scoped to the caller's namespace, and deduplicated per entity,
so a membership asserted twice does not return anything twice. It is bounded by
`--inference-max-subsume` (default 256); past the cap the response carries
`"subsume_truncated": true`, and `count` folds the same signal into its existing
`capped` flag.

`count` accepts `pattern` and `subsume` too. Bulk `delete` deliberately **rejects** it rather
than ignoring it: deleting by pattern would be a new destructive capability, so
it is refused until it is asked for explicitly.

**Ranking explanation (`explain: true`)**: each record carries an `explain`
object so retrieval is inspectable rather than a black box:

```json
{
  "explain": {
    "semantic": true,
    "lexical": false,
    "score": 0.8945,
    "similarity": 0.9939,
    "importance": 0.9,
    "confidence": 1.0,
    "weight": 0.9,
    "recency_factor": 1.0
  }
}
```

In every mode `score == weight × relevance × recency_factor`, where
`weight = importance × confidence` (or `1.0` if that product is ≤ 0),
`recency_factor = 0.5^(age/half_life)` (`1.0` when no `half_life_ms` is set), and
`relevance` is whichever signal ranked the hit:

| Query | `relevance` | Fields present |
|-------|-------------|----------------|
| `embedding` | `similarity` — raw cosine ([-1, 1]) | `similarity` |
| `query` | `bm25` — raw BM25 score (≥ 0, unbounded) | `bm25` |
| both (hybrid) | `rrf` — fused reciprocal rank | `similarity`/`bm25` as applicable, plus `semantic_rank`, `lexical_rank`, `rrf` |
| filters only | — | `semantic`/`lexical` both `false`, `similarity`/`score` `0`, ordered by time |

`semantic` and `lexical` are **per hit, not per query**: in a hybrid search one
record can be found only lexically and the next only semantically. The rank pair
is what says which, and both are always reported including the zero —
`"lexical_rank": 1, "semantic_rank": 0` means the exact term found this record and
the vector search missed it entirely, which is the case hybrid retrieval exists
to cover.

```json
{
  "explain": {
    "semantic": false, "lexical": true, "score": 0.01639,
    "bm25": 4.0581, "semantic_rank": 0, "lexical_rank": 1, "rrf": 0.01639,
    "importance": 0.9, "confidence": 1.0, "weight": 1.0, "recency_factor": 1.0
  }
}
```

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

`kind` is optional and at most **64 bytes**; a longer one is rejected with
`INVALID_REQUEST`. The limit is what the reverse edge index can intern, and an
un-internable kind would quietly demote a filtered backward `traverse` from an
answer to a candidate list.

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
| `kinds` | string[] | No | Follow only edges of these kinds (union); up to 16. Omit to follow every kind, as before. Must be an array of strings — a malformed one is `INVALID_REQUEST` rather than a silent unfiltered walk |
| `direction` | string | No | `out` (default) \| `in` \| `both`. `in`/`both` need the reverse edge index and return `NOT_READY` under `--no-edge-index`; any other value is `INVALID_REQUEST` |

**Response**: Same shape as `search` — `{ "ok": true, "records": [ … ], "total": N }`
— plus `"capped": true` when the walk hit its ceiling (see below).

**Bounded work.** A traversal visits at most **8192** records; past that it
returns what it reached and adds `"capped": true`, exactly as a truncated `count`
does. The result is then a prefix of the reachable set, not all of it. The
ceiling matters most in reverse: a record's *outdegree* is capped at 4096
relationships, but its *indegree* is not capped at all, so a backward walk from
a heavily-referenced record is the case this bounds. It is set clear of the
outdegree limit, so no single forward hop can be truncated.

**Edge-kind filter (`kinds`)**: a relationship's `kind` is set by `relate`, and
until now a walk followed every edge regardless of it — so retrieving one
relation type (a `supersedes` chain, a `derived_from` lineage) meant walking the
whole neighbourhood and filtering client-side. `kinds` pushes that filter into
the walk, which also bounds it: an excluded edge is never followed, so its
subtree is never read.

```json
{ "operation": "traverse", "id": 42, "depth": 3, "kinds": ["supersedes"] }
```

An edge carrying **no** kind is followed only by an unfiltered walk. Once a
caller names the kinds it wants, an unkinded edge is not one of them.

Filtering costs nothing extra: a walk already reads each record it returns, and
the kinds are stored in the record. Only the *reverse* direction needs an index
(`--no-edge-index` disables it), because a record lists the edges it points
along, not the ones pointing at it.

**Direction.** A relationship is directed, so half the questions about it can
only be asked backwards. `relate` records "v2 supersedes v1" as an edge from v2
to v1, which makes *"what did v1 turn into?"* a `direction: "in"` walk:

```json
{ "operation": "traverse", "id": 42, "depth": 3,
  "direction": "in", "kinds": ["supersedes"] }
```

`both` follows either orientation from each hop; `traversal.via_direction` then
says which one reached a given record, since an id can be reachable both ways
and `via_id` alone would not distinguish them. Namespace scoping applies
identically in reverse: a record you do not own never appears, so a backward
walk cannot be used to discover that someone else's memory links to yours.

**Caveat — consolidation lineage is not reachable this way.** `consolidate`
records a `supersedes` edge from the survivor to each record it absorbs, and then
tombstones those records. Deleting a record drops every edge pointing at it, so
the lineage edge leaves the reverse index immediately, and a tombstoned record
cannot start or appear in a walk in any case. Consolidation lineage is therefore
visible only in the survivor's `relationships` array (via `get`/`search`), not
through `traverse` in either direction. A backward `supersedes` walk works for
links you wrote yourself with `relate` between records that both stay live.

**Per-hop attribution (`traversal`)**: every returned record carries a
`traversal` object saying how the walk reached it, so a result reads as a path
rather than a set whose shape has to be inferred:

```json
{
  "id": 99,
  "data": "…",
  "traversal": { "depth": 2, "via_id": 42, "via_kind": "derived_from" }
}
```

| Field | Notes |
|-------|-------|
| `depth` | Hops from the starting record; `0` is the starting record itself |
| `via_id` | The record at the other end of the reaching edge; absent at depth `0` |
| `via_kind` | That edge's `kind`; absent at depth `0`, and absent for an unkinded edge |
| `via_direction` | `out` if the edge was followed forwards (`via_id` points at this record), `in` if backwards (this record points at `via_id`); absent at depth `0` |
| `via_kind_unknown` | Present and `true` only on a reverse hop whose edge kind the index could not determine, so `via_kind` is *unknown* rather than absent — and, under a `kinds` filter, this hop is a candidate rather than a confirmed match. Requires a corpus with more than 4096 distinct edge kinds; normally absent |

The walk is breadth-first and records **first** discovery, so `via_*` describes
the shortest path found; a record reachable by several edges reports whichever
reached it first. This is unconditional — there is no flag — because it is a
handful of bytes next to a record that already carries its payload.

---

### `conflicts`

The contradictions the inference job flagged and refused to settle
(ROADMAP 5.4). `stats.metrics.conflicts` says *how many*; this says **which**,
which is what anything meaning to act on one needs.

Before this there was no way to reach the pairs. A `conflicts_with` edge is only
walkable from an id you already hold, and the reverse edge index is keyed
target → sources with no enumeration by kind — so finding them meant reading
every live record. The list is not a new scan: the pass that counts the gauge
fills it in the same loop, which is also why the two cannot disagree.

**Request**:

```json
{ "operation": "conflicts", "limit": 50 }
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `limit` | integer | No | Page size; default `100`, capped at `1024`. An explicit `0` lists nothing and still reports `total`, which makes it a "how many are there?" probe. `0` is *not* read as unlimited |

`agent_id` is **ignored**, exactly as `export` and `purge` ignore it: the
token's namespace is the authority, so a namespaced caller sees its own
tenant's contradictions and nothing else. An admin (or unauthenticated) caller
sees every namespace, which is why each pair names its own.

**Response**:

```json
{
  "ok": true,
  "conflicts": [
    {
      "a": 42,
      "b": 57,
      "namespace": "acme",
      "predicate_a": "defaults_to",
      "predicate_b": "defaults_to",
      "reason": "cardinality"
    }
  ],
  "total": 1
}
```

`a` and `b` are the two records that contradict each other, ordered by id so a
pair has one identity regardless of which the scan reached first. `reason` is
`cardinality` (two live values for a single-valued predicate) or `mutex_with`
(two predicates the registry declares mutually exclusive).

**Two different shortfalls, reported apart.** `"capped": true` means this
response carries fewer than `total` — ask again with a bigger `limit`.
`"truncated": true` means the *pass* found more contradictions than the list
retains (1024), so a bigger limit cannot help; the gauge in `stats` is the
number to trust there.

**A gauge, not a log.** The list is replaced whole on every pass, so a
contradiction resolved since the last one stops being reported. That is what
makes it safe to work through: an accumulating list would hand back pairs whose
records are already tombstoned. It follows that a server started **without**
`--inference` answers `{"conflicts": [], "total": 0}` — nothing has scanned. It
answers rather than erroring, so a client's adjudication loop does not need to
know whether the feature is on.

**Read-only, deliberately.** The inference job detects contradictions and
refuses to resolve them, because choosing between two conflicting facts needs
to know which is newer, which source is better, or what the world is like. This
op does not resolve them either — it hands the pair to whoever can. A verdict
comes back as an ordinary `relate` (`supersedes`) plus `delete`, on the record
and in the log like every other write, which is what keeps an adjudicated
contradiction auditable rather than a silent deletion.

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
| `indexes.usage_tracked` / `memory.usage_bytes` | Records with usage counters, and their resident bytes. Both `0` under `--no-usage-feedback` |
| `metrics.recall_latency` | Latency distribution of the `search` (recall) operation — absent until the first search, so a fresh server reports no distribution rather than an all-zero one. `count`, `micros_total`, `mean_micros`, interpolated `p50_micros`/`p95_micros`/`p99_micros`, and `buckets` keyed by upper bound in microseconds. Bucket counts are **cumulative** (Prometheus `le` semantics — each includes every faster bucket, and `+Inf` equals `count`), so a scraper can pass them straight through. Recall sits in an agent's inner loop, so this is the tail to alert on; `dispatch_micros` is a cumulative mean over *all* operations and hides it. |
| `records` | Live (non-deleted) persisted records |
| `tombstones` | Deleted-but-not-yet-compacted records still in the log |
| `log_bytes` | Current size of `memory.log` on disk |
| `log_flush_pending` | `true` if writes have not yet been `fsync`'d — the current durability lag |
| `indexes` | Per-index entry counts (`semantic` is the brute-force vector count; watch it for scale). `lexical_terms`/`lexical_docs` are the distinct terms and indexed payloads in the BM25 index, both `0` under `--no-lexical-index`. `edges`/`edge_kinds` are the indexed incoming edges and the distinct kinds they carry, both `0` under `--no-edge-index`. `facts`/`fact_predicates` are the indexed typed facts and the distinct predicates in use, both `0` under `--no-fact-index`. `derived` counts conclusions the inference job has written **since this process started** (not live derived records), `inference_last_ms` is how long the last pass took, and `inference_deferred` is `1` when a cap stopped that pass short — a value that stays `1` means the caps are too small for the corpus and the job never reaches fixpoint, which is survivable but worth alerting on. `retracted` counts conclusions withdrawn because every one of their justifications lost a premise. `conflicts` is a **gauge**, not a total: how many contradicting *pairs* the corpus holds right now, recomputed each pass, so it falls when one is resolved. Pairs, not groups — three different values for one `cardinality: one` predicate report 2, since each is linked to the first — so treat it as "is anything contradictory, and roughly how much", not as a count of distinct disagreements. A pair whose `conflicts_with` edges could not be written (the record is at `MAX_RELATIONSHIPS`) is still counted, so the gauge never goes quiet about a contradiction it could not record. All five are `0` without `--inference` |
| `memory` | Approximate resident bytes per in-RAM index — `hash_bytes`, `time_bytes`, `tag_bytes`, `lexical_bytes`, `edge_bytes`, `fact_bytes`, `usage_bytes`, `semantic_bytes`, `index_bytes_total`, and `index_bytes_limit` (the configured `--max-index-bytes` cap; 0 = unlimited). Indexes are held in memory and grow with the dataset (the semantic vectors usually dominate), so this is the figure to monitor/alert on; past the limit inserts return `MEMORY_LIMIT`. Excludes allocator overhead. |
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
