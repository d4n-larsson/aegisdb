# Design: Log-Shipping Read Replicas

**Status:** Proposed (design only — not implemented)
**Scope:** Asynchronous, read-only replicas that follow a primary by streaming
its append-only log, for read availability, read scaling, and a warm standby to
promote by hand after a primary failure.

This is a proposal, not current behavior. AegisDB is single-node today (see
[architecture.md](architecture.md) → Non-goals); this document describes how we
would add replication *without* abandoning that simplicity, and is deliberately
explicit about what it does and does not guarantee.

## 1. Goals & non-goals

**Goals**

- **Read availability.** If the primary is down, replicas keep answering
  `get`/`search`/`traverse`/`count` from their copy (stale, but serving).
- **Read scaling.** Fan recall-heavy read traffic out across replicas.
- **A warm standby.** A replica is a continuously-updated copy on another host,
  promotable to primary by hand after a failure — faster and lower-loss than
  restoring last night's backup.
- **Stay in AegisDB's grain.** Reuse the append-only log, the existing frame
  format, and the recovery apply path. No new storage engine, no consensus.

**Non-goals** (explicitly out of scope for this design)

- **Not synchronous / not strongly consistent.** Replication is asynchronous;
  reads on a replica are *eventually* consistent, bounded by replication lag.
- **Not automatic failover.** Promotion is a manual (or externally-orchestrated)
  operation. There is no leader election in-binary.
- **Not multi-writer.** Exactly one primary accepts writes at a time. Correctness
  under promotion relies on **fencing** the old primary (below).
- **Not HA for writes.** A primary outage pauses writes until promotion.

These non-goals are deliberate: agent memory tolerates brief unavailability (the
Claude Code integration degrades gracefully when the store is unreachable), so
the value/complexity trade-off favors async read replicas over a consensus
system.

## 2. Why the log makes this natural

AegisDB is **single-writer**: every `episodic`/`semantic` mutation (insert,
update, delete-tombstone, promote, relate) is a frame appended to `memory.log`,
which is the sole source of truth; all in-memory indexes are derived from it and
rebuilt on startup ([architecture.md](architecture.md) → Storage engine). Each
frame is self-describing and self-checksummed
([log.h](../include/aegisdb/log.h)):

```
[MAGIC u32][LEN u32][PAYLOAD_CRC u32][HEADER_CRC u32][PAYLOAD LEN bytes]
```

So "replicate" reduces to "**ship the log frames in order and replay them**" —
exactly what crash recovery already does from a local file (`log_scan` →
per-frame index apply). A replica is just a node that runs that apply loop
against a remote, growing log instead of a local one.

Because a replica appends the **same frames in the same order**, its log is
**byte-identical** to the primary's, so **frame offsets match on both sides**.
That is the linchpin: the replication cursor is a single byte offset, and it is
directly comparable to the primary's log size (→ a trivial, exact lag metric)
and interoperable with checkpoints and `--restore`.

## 3. Architecture

```
                 writes                     reads
 clients ────────────────▶  PRIMARY  ◀───────────────── (also serves reads)
                             │  memory.log (source of truth)
                             │
              replication    │  frames [offset, bytes]  (async stream)
              stream ────────┼───────────────┬───────────────┐
                             ▼               ▼               ▼
                          REPLICA A       REPLICA B        REPLICA C
                          (read-only)     (read-only)      (read-only)
                          local memory.log = verbatim copy; own indexes
                             ▲               ▲               ▲
 read clients ───────────────┴───────────────┴───────────────┘
```

- The **primary** is an ordinary AegisDB server plus a replication *source*: it
  streams appended frames to subscribed replicas.
- A **replica** runs in read-only mode: it connects to the primary, streams
  frames from its cursor, appends them verbatim to its own `memory.log`, and
  applies each to its in-memory indexes (the recovery apply path, run
  continuously). It serves reads and refuses writes.
- Replicas are independent: each maintains its own indexes, its own HNSW graph
  (built off-lock, [architecture.md](architecture.md) → Indexes), its own
  checkpoints. No replica talks to another.

## 4. Replication protocol

### 4.1 Handshake & streaming

A replica opens a dedicated connection to the primary and subscribes from its
cursor:

```
replica → primary:  {"operation":"replicate","from_offset":<N>,"token":"<repl-token>"}
```

On success the connection **switches from request/response NDJSON to a
one-directional binary frame stream** (the request/response model doesn't fit a
long-lived subscription). Each stream message is length-prefixed:

```
[STREAM_MAGIC u32][offset u64][frame_len u32][frame bytes …]
```

where `frame bytes` is the **raw on-disk frame** (header + payload) — shipped
verbatim so its existing CRCs validate end-to-end and the replica can `append`
it byte-for-byte. Raw framing (not base64-over-NDJSON) keeps backfill of a large
log efficient.

The primary first **backfills** history (streams frames from `from_offset` to
the current log end), then **tails** live: each subsequent `log_append` also
enqueues the frame to every subscribed replica. A periodic **heartbeat**
(`offset` = current log size, `frame_len` = 0) lets an idle replica measure lag
and detect a dead primary.

### 4.2 Apply on the replica

For each received frame the replica:

1. Verifies `offset` == its own current log size (the streams must stay in
   lockstep; a mismatch means desync → re-bootstrap, §4.4).
2. `log_append`s the raw frame to its local log (identical bytes → identical
   offset).
3. Decodes the record and updates its in-memory indexes — the **same per-frame
   apply** recovery uses (hash, time, tag, semantic; tombstones drop from the
   secondary indexes). `next_id` tracks `max(id)+1` exactly as in recovery.

The replica periodically checkpoints (`memory.index`, `memory.sem`) like any
node, so a replica restart replays only its own tail, then resumes streaming
from its cursor.

### 4.3 Cursor & resume

The cursor is the replica's **applied byte offset** (= its local log size). On
(re)connect it sends `from_offset = <applied offset>`; the primary resumes the
stream there. Disconnects are therefore cheap and self-healing: reconnect, resume
from the cursor, catch up. No frame is applied twice (offset check) and none is
skipped (contiguous byte stream).

### 4.4 Compaction: the one wrinkle

Compaction rewrites `memory.log` to drop tombstones/superseded versions, which
**changes every offset** ([architecture.md](architecture.md) → Compaction). That
breaks the byte-identical-offsets invariant, so a naive tailing replica would
desync the moment the primary compacts.

**Resolution:** compaction emits a **rebase** control message on the stream
(`STREAM_MAGIC` with a distinguished type carrying the new base identity, e.g. a
log epoch/generation id + the new log size). On receiving it, the replica
discards its log and **re-bootstraps**: it fetches the compacted log as a base
(reuse the `snapshot` primitive — the primary's compacted log *is* a consistent
prefix) and resumes streaming from the new log's end. Re-bootstrap is O(live
data) but rare (compaction cadence is `--compact-sec`, default 300s, and only
when enough of the log is dead). A log **generation id** in the handshake lets
primary and replica detect a stale cursor across a compaction they missed and
force the re-bootstrap deterministically.

### 4.5 Bootstrap of a fresh replica

A brand-new replica starts empty and would backfill from offset 0 — fine for
small datasets. For large ones it instead pulls a **base snapshot** first (the
existing `snapshot`/`--restore` path), then streams from the snapshot's covered
offset. Bootstrap is thus "restore a backup, then follow the tail" — reusing
machinery that already exists and is tested.

## 5. Read serving & consistency

- A replica serves the read ops over persistent data: `get`, `search`
  (time/tags/embedding), `traverse`, `count`. Auth/namespace scoping is enforced
  identically (replicas load the same token file).
- A replica **refuses writes** (`insert`/`update`/`delete`/`promote`/`relate`/
  `consolidate`/`snapshot`) with a dedicated error, e.g. `READ_ONLY`, ideally
  carrying a hint to the primary's address. `ping` and `stats` still work.
- **Working memory is not replicated.** It is volatile, per-session, RAM-only
  ([architecture.md](architecture.md) → Memory types) and node-local; it never
  enters the log. Working-memory ops and `promote` are writes, so they are
  primary-only anyway.

**Consistency model:** asynchronous, **eventually consistent**. A read on a
replica reflects all writes up to its applied offset; writes newer than that
(bounded by replication lag) are not yet visible. Guarantees:

- **Monotonic reads per replica** (the log only moves forward on a replica).
- **No read-your-writes across nodes** — a client that writes to the primary and
  immediately reads from a replica may not see its write. Clients needing
  read-your-writes should read from the primary.
- **Bounded staleness** is observable (lag metric, §7); an optional advanced read
  flag `max_staleness_ms` could let a replica refuse/redirect a read that is too
  stale, but that is a later refinement, not v1.

## 6. Promotion & failure handling

| Event | Behavior |
|---|---|
| Replica lags / disconnects | Reconnect from cursor, catch up. Reads keep serving (staler) throughout. |
| Replica crashes | Restart → local recovery → resume streaming from cursor. |
| **Primary crashes** | Writes pause. Replicas keep serving stale reads. Operator promotes a replica (below). |
| Network partition | Replica serves stale reads; catches up when the link returns. |

**Manual promotion runbook:**

1. **Fence the old primary** — ensure it is truly stopped (or cannot accept
   writes). This is the critical, operator-owned step: two writable nodes = split
   brain = divergent logs. AegisDB does not fence automatically; an external
   orchestrator (or a human) must guarantee single-writer.
2. Pick the **most-caught-up** replica (highest applied offset — visible in
   `stats`).
3. Flip it to read-write (`SIGHUP` to reload role, or restart without
   `--replicate-from`). Its replayed log already yields the correct `next_id`
   (`max(id)+1`, further floored by `metadata.db` — no id reuse), so writes
   resume safely **provided the old primary is fenced**.
4. Repoint clients (and other replicas' `--replicate-from`) at the new primary.
   Other replicas re-bootstrap if their cursor predates divergence.

Automatic failover (health-checked election + fencing via an external
coordinator/VIP) is a possible **Phase 2**, deliberately excluded here to avoid
in-binary consensus and split-brain hazards.

## 7. Configuration & observability

**Config (proposed flags):**

| Flag | Role | Meaning |
|---|---|---|
| `--replication-token <tok>` | primary | Require this token on `replicate` subscriptions (admin/replication scope). Enables the replication source. |
| `--replicate-from <host:port>` | replica | Make this node a read-only replica following that primary. |
| `--replication-token <tok>` | replica | Token sent to subscribe. |
| `--read-only` | either | Refuse writes (implied by `--replicate-from`; also usable to quiesce a node). |

**Observability (extend `stats`):**

- Primary: `replicas: [{ addr, applied_offset, lag_bytes, connected_ms }]`, log
  generation id.
- Replica: `role: "replica"`, `primary`, `applied_offset`,
  `primary_offset`, `lag_bytes`, `lag_ms`, `connected`, `bootstrapping`.

Lag is exact and cheap: `primary_offset − applied_offset` in bytes (offsets are
comparable by the §2 invariant); `lag_ms` derives from heartbeat timestamps.

## 8. Security

The replication stream ships the full log, so it must be authenticated
(`--replication-token`, constant-time compared like other tokens) and, like all
AegisDB traffic, carries no transport encryption — run it over a trusted
network / VPN / TLS-terminating proxy, the same stance as the client protocol
and backups.

## 9. Deployment sketch (compose)

```yaml
services:
  primary:
    image: aegisdb:latest
    command: >
      --data-dir /data --port 9470 --embedding-dim 384
      --replication-token ${AEGIS_REPLICATION_TOKEN}
    volumes: [ primary-data:/data ]

  replica:
    image: aegisdb:latest
    command: >
      --data-dir /data --port 9470 --embedding-dim 384
      --replicate-from primary:9470
      --replication-token ${AEGIS_REPLICATION_TOKEN}
    volumes: [ replica-data:/data ]
    depends_on: { primary: { condition: service_healthy } }
```

Clients send writes to `primary:9470` and can spread reads across
`primary`/`replica`. `--embedding-dim` must match across all nodes.

## 10. Testing strategy

- **Unit:** per-frame apply is deterministic and offset-exact; cursor resume
  applies each frame exactly once; rebase resets cleanly.
- **Integration:** primary + replica in-process; write on primary → assert the
  record appears on the replica after catch-up; kill/restart the replica →
  resumes from cursor; compact the primary → replica re-bootstraps and reconverges;
  promote a replica (with the primary fenced) → writes resume with no id reuse;
  a write sent to a replica → `READ_ONLY`.
- **Soak:** sustained writes with a lagging replica; verify convergence and
  bounded lag; verify byte-identical logs after quiescence.

## 11. Alternatives considered

- **Snapshot polling** (replica periodically `--restore`s a fresh snapshot):
  simplest, but high lag and wasteful re-transfer of the whole log. Log shipping
  is strictly better for freshness at similar complexity. (Snapshot *is* reused
  for bootstrap/rebase.)
- **Synchronous replication** (ack writes only after a replica applies): adds
  write latency and couples primary availability to replicas — contradicts the
  best-effort, low-latency memory workload. Rejected.
- **Consensus (Raft/multi-writer):** strong consistency + auto-failover, but a
  different class of system (leader election, quorum, fencing, log compaction
  under consensus). Disproportionate for agent memory and against the single-
  binary ethos. Rejected.

## 12. Phasing

1. **Phase 1 (this design):** async read replicas, manual promotion, `stats`
   lag, compaction rebase. Delivers read availability/scaling + a warm standby.
2. **Phase 2 (optional, later):** automatic failover via an external coordinator
   (health checks + fencing + client redirection). Kept out-of-binary.

## 13. Open questions

- **Rebase transport:** reuse `snapshot` files over a side channel, or add a
  bulk-transfer mode to the replication stream itself? (Leaning: reuse snapshot.)
- **Backpressure:** if a replica can't keep up, does the primary buffer, drop the
  slow subscriber, or apply flow control? (Leaning: bounded buffer, then drop +
  force re-bootstrap on reconnect.)
- **Read routing:** client-side (the integration learns replica addresses) vs. a
  proxy. Likely out of scope — leave to the deployment/proxy layer.
- **Partial/namespace replication:** replicate only some tenants to some
  replicas? Deferred; whole-log shipping first.