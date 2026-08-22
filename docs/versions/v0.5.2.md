# AegisDB v0.5.2 — concurrency hardening 🛡️

A fixes-only release from a systematic concurrency audit of the threaded
server — the sharded poll() loops, the maintenance/compaction thread, and the
replication threads. No API, wire-protocol, or on-disk format changes; upgrade
in place. The headline fixes close two **data-loss** races that could only
surface under concurrent load (which is why the test suite and CI's
ThreadSanitizer run stayed green until the audit went looking for them).

## Highlights

- **No data loss when compaction races a write.** The post-write durability
  fsync touched the log file's descriptor and mutex without the lock that
  compaction's log swap holds — so a compaction running concurrently with a
  write could corrupt the log handle, turn a *committed* write into a spurious
  error (client retries → **duplicate record**), or worse. Fixed.
- **No corruption when two compactions overlap.** Compaction could run on the
  maintenance thread and, at the same time, inline on a request thread (a
  `purge` with `compact`). The two shared one scratch file and could double-swap
  the log, leaving index offsets pointing at freed data → **lost live records**.
  Compactions are now serialized.
- **Bounded, clean shutdown for replicas.** A read-replica following an
  unreachable primary could wedge shutdown for ~2 minutes (kernel connect
  default) → the orchestrator `SIGKILL`s it → no final checkpoint. Connects are
  now bounded to 5s.

## Fixes by area

**Data-loss races (HIGH)**

- Write-path durability fsync (and snapshot pre-flush, and the interval flush)
  now hold `log_lock` around the log fsync, so a concurrent compaction swap
  cannot close/reopen the log underneath them.
- Compaction is serialized by a per-database lock; a second concurrent caller
  skips rather than clobbering the shared scratch log.

**Availability & latency (MED)**

- `net_dial` now uses a 5s non-blocking connect, so an unreachable replication
  peer can no longer hang shutdown (also bounds the client CLI and health probe).
- Snapshots no longer hold the index write-lock across the whole log copy — the
  copy now runs under only the log lock, so writes proceed during a snapshot of
  a large database.
- Per-tenant accounting reads the auth-token count under the auth lock instead
  of racing runtime token add/revoke.

**Robustness (LOW)**

- A partial I/O-thread spawn now runs **degraded** on the threads that started
  instead of taking the whole server down (and no longer reports a clean start
  when it had aborted).
- Compaction now aborts (keeping the original log) if a live record can't be
  read back, instead of silently dropping it from the compacted log.
- Removed two benign-but-real unsynchronized reads (the replication streamer's
  frame-size read; the `history`/`as_of` audit scan's log-size read).

## Operator notes

- **Connect timeout:** replication follows, the client CLI, and the health
  probe now give up on a TCP connect after 5 seconds. If you relied on a very
  slow connect succeeding, it will now fail (and, for a replica, retry).
- **Degraded start:** if some (but not all) I/O threads fail to spawn, the
  server now logs a warning and runs on the rest rather than exiting. A start
  with **zero** threads still fails.
- No migration required.

## Validation

Unit + wire-contract suites pass; the compaction suite gained a
concurrent-compaction regression test and the whole suite is
ThreadSanitizer-clean. The audit's one remaining item — adding a stop-flag
check *inside* a long-running compaction / index build to trim shutdown latency
— is tracked for a future release; it affects only how quickly a shutdown
completes mid-operation, not correctness.

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*