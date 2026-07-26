# AegisDB v0.5.1 — reliability hardening 🛡️

A fixes-only release from a systematic C systems-programming audit of the
storage, network, and startup paths. No API, wire-protocol, or on-disk format
changes; upgrade in place. The theme is **failing loudly instead of silently** —
I/O errors that were previously swallowed are now detected, surfaced, and (where
it matters) refuse to report success.

## Highlights

- **No more silent write loss.** `fsync`/`close` failures on the write-ahead log
  were being dropped, so a disk error (e.g. a full volume) could let an
  *acknowledged* write never reach stable storage, and a clean shutdown could
  report success with the log tail lost. These are now checked end-to-end: a
  durability=`sync` write that can't be fsynced returns an error to the client
  instead of a false OK.
- **No more CPU meltdown under fd exhaustion.** When the process ran out of file
  descriptors, both accept loops spun at 100% CPU until one freed. They now back
  off and degrade gracefully.
- **Crash-consistent log swaps.** Compaction and legacy-log migration now fsync
  the directory after renaming the log in, so the swap survives a crash rather
  than potentially reverting.

## Fixes by area

**Durability**

- Write-ahead log `fsync`/`close` errors are now propagated, not ignored; a
  failed durability fsync fails the write rather than acknowledging it. (#206)
- Compaction and legacy migration fsync the log directory after `rename()`; a
  failed compaction reopen is now detected and reported as an unusable-database
  condition instead of running on silently. (#208)
- Snapshot creation now checks the directory fsync before reporting the snapshot
  as durable. (#211)
- Recovery aborts if trimming a torn log tail fails, so appends can't land past
  damaged data. (#211)

**Availability**

- `accept()` now backs off on `EMFILE`/`ENFILE` exhaustion instead of
  busy-spinning (main server + replication acceptor). (#209)

**Robustness & hygiene**

- All file descriptors and sockets are opened close-on-exec
  (`O_CLOEXEC`/`SOCK_CLOEXEC`). (#210)
- The CLI ignores `SIGPIPE`, so a mid-request server reset prints a clean error
  instead of killing the client by signal. (#207)
- The client retries `recv()` on `EINTR`. (#211)
- Signal-handler installation and a few other syscall return values are now
  checked. (#211)

**Input validation**

- Negative `--*-size` arguments (e.g. `--embedding-dim -1`) are rejected instead
  of wrapping to a huge value. (#207)
- `--tenant-rate-qps` rejects `nan`/`inf`, which previously would have denied
  every request for every tenant indefinitely. (#211)
- `--restore` claims the destination atomically (`O_EXCL`), closing a
  check-then-copy race that could clobber a live database. (#211)

## Operator notes

- **New failure surfaced:** with `--durability sync`/`batch`, a write whose fsync
  fails now returns `INTERNAL` rather than success. This is the intended fix, but
  if you have a failing/full disk you'll now see these errors (and `ERROR`-level
  log lines) where before they were silent.
- **Stricter arg parsing:** negative size flags and non-finite
  `--tenant-rate-qps` are now hard-rejected at startup. If a script was passing
  one of these (unintentionally), the server will refuse to start instead of
  misbehaving.

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*