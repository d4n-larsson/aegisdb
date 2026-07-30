# Security Policy

## Supported versions

AegisDB is pre-1.0. Security fixes land on `main` and ship in the next release;
only the latest published version is supported.

| Version | Supported |
| ------- | --------- |
| 0.5.x (latest release) | ✅ |
| Older 0.x releases | ❌ — upgrade to the latest |

## Reporting a vulnerability

Report privately through GitHub:
**[Security → Report a vulnerability](https://github.com/d4n-larsson/aegisdb/security/advisories/new)**

Please do **not** open a public issue, pull request, or discussion for a
suspected vulnerability.

Helpful things to include:

- The version or commit, and the flags the server was started with.
- What an attacker gains (crash, data disclosure, cross-tenant read, auth
  bypass, …) and what access they need to get it.
- A reproducer — a request sequence, a corrupt log/checkpoint file, or a fuzz
  input. `tests/fuzz/` has the harnesses if you found it that way.
- Whether authentication was enabled, and the token's namespace and scope.

This is a small, single-maintainer project. Expect an acknowledgement within a
few days and an honest estimate rather than a fixed SLA. If a report is valid
you will be credited in the advisory and release notes unless you ask not to be.

## Security model

Read this before reporting — several properties below are deliberate design
decisions, documented in the README and
[`docs/wire-protocol.md`](docs/wire-protocol.md), not bugs.

**The wire protocol has no transport encryption.** TLS is intentionally kept out
of the binary to preserve the single, dependency-free build. Tokens are
therefore sent in plaintext and are only as private as the channel: terminate
TLS at a reverse proxy (nginx/Caddy), use `stunnel`, an SSH tunnel, or a private
network. "Credentials are transmitted in cleartext" is a known design decision.

**Authentication is off by default.** With no `--auth-token` /
`--auth-token-file`, every operation is unauthenticated and unrestricted. An
AegisDB port reachable by an untrusted network without auth is a
misconfiguration, not a vulnerability.

**The server listens on all interfaces.** There is no `--bind` flag; the
listener binds `0.0.0.0` and exposure is restricted at the layer above. The
provided `docker-compose.yml` publishes the port on host loopback
(`127.0.0.1`) only — running the bare binary does not, so restrict it with your
firewall, network namespace, or container port mapping.

**Encryption at rest is opt-in.** Without `--encryption-key-file`, the log and
checkpoints are plaintext on disk, so anyone who can read the data directory can
read every memory. Keys are supplied by file; protect that file's permissions.

**Trust boundaries.** A global admin token is fully trusted: it can read and
write every namespace, administer tokens at runtime, and take snapshots. A
namespaced token is confined to its own tenant, and a read-only token is refused
writes. Escaping *those* confinements is a vulnerability — see below.

## In scope

- Memory-safety faults (out-of-bounds access, use-after-free, double-free,
  integer overflow leading to either) reachable from the wire protocol or from a
  malformed log, checkpoint, or snapshot file.
- Authentication bypass, or any way to authenticate without a valid token.
- Cross-tenant access: a namespaced token reading, writing, or inferring the
  existence of another namespace's records; a read-only token completing a write;
  a non-admin token reaching an admin-only operation.
- Token handling flaws — timing side channels in comparison, tokens written
  unhashed or leaked into logs or error responses, a revoked token still
  authenticating.
- Cryptographic flaws in the vendored XChaCha20-Poly1305 usage, the frame
  format's authentication, nonce handling, or key derivation.
- Path traversal or arbitrary file write via a request-supplied name (snapshot
  names, token file rewrites).
- Remote denial of service that defeats the documented guards — that is, a way
  to exhaust memory, file descriptors, or CPU on a server that has
  `--max-payload`, `--max-connections`, `--idle-timeout-sec`, `--query-scan-cap`,
  `--max-index-bytes`, and the tenant limits configured.
- Replication flaws: a replica accepting a stream without the
  `--replication-token`, or a stream that corrupts the follower's state.
- Silent data loss or corruption — a write acknowledged but not recoverable, or
  recovery that discards intact records.

## Out of scope

- The design decisions in [Security model](#security-model): absent TLS,
  plaintext tokens on the wire, auth disabled by default, the listener binding
  all interfaces, plaintext-at-rest without a key.
- Anything an authenticated **global admin** token can do — it is fully trusted
  by design.
- Attacks requiring read or write access to the data directory or key file when
  encryption at rest is not enabled.
- Resource exhaustion on a server run with the DoS guards disabled or unset
  (`0` = unlimited for most), or with unbounded tenant quotas.
- Reports produced only by running with auth disabled on an exposed port.
- Weak or guessable operator-chosen tokens. Use
  `openssl rand -hex 32`, as the docs instruct.
- Vulnerabilities in vendored third-party code (`third_party/cjson`,
  `third_party/unity`) that do not affect AegisDB's use of it — please still
  report these, but they are usually upstream's to fix.

## Hardening checklist

For an AegisDB instance reachable by anything other than localhost:

- [ ] Enable authentication (`--auth-token-file`) with high-entropy tokens.
- [ ] Store tokens hashed — `aegisdb --hash-token <tok>` — so a leaked token
      file reveals nothing usable.
- [ ] Give each agent or tenant its own namespaced token, `ro` where a write is
      not needed. Reserve admin tokens for administration.
- [ ] Terminate TLS at a proxy, or keep the port on a private network.
- [ ] Restrict the listener at the firewall or container layer; do not publish
      the port to `0.0.0.0` unless you mean to.
- [ ] Enable encryption at rest (`--encryption-key-file`) and protect the key
      file's permissions. Back it up somewhere the data directory is not.
- [ ] Set the DoS guards: `--max-payload`, `--max-connections`,
      `--idle-timeout-sec`, `--query-scan-cap`, `--max-index-bytes`.
- [ ] Set per-tenant limits on a shared server: `--tenant-max-records`,
      `--tenant-max-bytes`, `--tenant-rate-qps`.
- [ ] Protect the replication port with `--replication-token`.

## What the project does proactively

- **Fuzzing** of the two attacker-reachable parse surfaces — the binary log
  record codec and the wire request path — with libFuzzer + ASan/UBSan
  (`make fuzz`), soaked nightly in CI. Findings are minimized into
  `tests/fuzz/corpus/crashers/` and replayed under ASan/UBSan as a permanent
  per-PR regression gate (`make fuzz-regress`).
- **Sanitizers in CI**, on every pull request: the unit tests and the live
  server the wire-protocol contract suite drives both run under
  `-fsanitize=address,undefined` with leak detection fatal, and the contract
  suite runs again against a `-fsanitize=thread` server so the event loop and
  locking are checked under real concurrent load.
- **Static analysis and warning hygiene**: `clang-tidy` with
  `WarningsAsErrors`, and `-Werror` builds in CI.
- **Corruption resilience** by design: the append-only log checksums frame
  headers independently of payloads, so a damaged frame is skipped and the
  intact frames around it are still recovered rather than the whole tail being
  discarded. Recovery paths are unit-tested against deliberately corrupted logs,
  checkpoints, and snapshots.
- **Constant-time token comparison**, tokens hashable at rest, and revocation
  that takes effect immediately.