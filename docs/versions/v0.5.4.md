# AegisDB v0.5.4 — input hardening + code standards 🛡️

A fixes-and-tooling release. The runtime change is a round of input-validation
hardening from a security audit of the JSON request path; the rest is developer
tooling (enforced formatting + lint) that carries no runtime impact. No API,
wire-protocol, or on-disk format changes; upgrade in place.

The audit found **no memory-safety or cross-tenant issues** — client-controlled
sizes are clamped, the record decoder is bounds-checked, and reads are masked
per tenant. These close the medium/low gaps it did surface.

## Input hardening

- **`token_add` namespace validation.** A token's namespace is written into the
  space/newline-delimited token file, so it is now rejected if it contains
  whitespace or control characters. Previously a crafted namespace could inject
  an extra token-file line (e.g. a bare token = a hidden global-admin credential)
  that survived a reload. Admin-gated, so not a privilege escalation, but it
  corrupted persisted auth state.
- **Reject non-finite numbers.** NaN/Inf (reachable via an overflowing JSON
  literal like `1e400`) are now rejected in numeric fields and embeddings —
  they previously flowed into ranking/similarity thresholds and poisoned the
  math (every NaN comparison is false). A non-numeric embedding element is now
  rejected outright rather than silently dropped.
- **Sanitized pre-auth logging.** The client `operation` string is scrubbed to
  printable ASCII before it is logged, so an unauthenticated client can no
  longer inject newlines / terminal escapes into an operator's console.
- **Defensive decode.** `record_decode` clamps out-of-range/non-finite
  importance/confidence back to defaults (against a corrupt log or a malicious
  replication peer), and the serialization buffer guards against size overflow.

## Code standards (developer-facing, no runtime impact)

- **clang-format** enforces the canonical C style (`.clang-format`); CI fails an
  unformatted tree. Run `make format` / `make format-check`.
- **clang-tidy** enforces the mechanical style clang-format can't express —
  braces on every conditional/loop body, one declaration per line, clarifying
  parentheses, uppercase literal suffixes. Run `make tidy` / `make tidy-check`.
- Both are pinned in CI (clang output drifts between versions). See
  [conventions.md](conventions.md). Bug-pattern lint (`bugprone-*` etc.) is
  being enabled separately.

## Operator notes

- **`token_add`** now rejects a `namespace` containing whitespace or control
  characters with `INVALID_REQUEST`. Namespaces are otherwise unchanged.
- **Non-finite float fields** (e.g. `min_similarity`, `min_score`,
  `half_life_ms`, embedding values) are now rejected; a request that relied on
  passing `NaN`/`Inf` will be refused rather than silently mis-ranked.
- No migration required.

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*