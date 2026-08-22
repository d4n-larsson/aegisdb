# AegisDB v0.5.3 — test stability 🧪

A test-only release. **No runtime, API, wire-protocol, or on-disk format
changes** — the server binary is functionally identical to v0.5.2. This release
exists only to carry a CI-stability fix so the tree tags cleanly.

## What changed

- **Fixed a timing-flaky contract test.** The `recency_factor < 1 under an
  aggressive half_life_ms` wire-protocol check searched immediately after
  inserting a record. Recency decay is `0.5^(age_ms / half_life_ms)`, which is
  exactly `1.0` when `age` is `0` — so when the insert→search round-trips
  happened to complete within the same millisecond (more likely on a faster
  build), the factor was `1.0` and the assertion failed. It surfaced as a
  clang-only failure purely because that build occasionally finished the
  round-trips in under a millisecond. The test now sleeps briefly so the record
  is measurably older than "now", making the assertion deterministic on any
  build speed.

  This was a pre-existing flake in the test itself; the server's recency math
  (`qe_search.c`) is unchanged.

## Upgrading

Nothing to do — no behavior changed. If you are already on v0.5.2 there is no
functional reason to upgrade; v0.5.3 simply matches the green build.

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*