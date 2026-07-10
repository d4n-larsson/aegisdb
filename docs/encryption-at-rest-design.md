# Design: Encryption at Rest

**Status:** Implemented (issue #94, sub-issue of the operational-hardening epic
#68). Shipped across PRs #123 (vendored cipher), #124 (log), #125 (checkpoints),
#126 (offline `--encrypt-migrate`), #127 (backup/restore), and the replication
integration. Enabled with `--encryption-key-file`; `aegisdb gen-key` mints a key.

Two deliberate differences from the original design below: replication ships the
decrypted payload and each node re-encrypts into its own log with its own copy of
the (shared) key, rather than shipping ciphertext byte-for-byte — so a primary
and its replicas must be configured with the **same** key (the handshake rejects
a mismatch), and the replication link itself is plaintext (tunnel it, as with the
replication token). Nonce management uses XChaCha20's random 192-bit nonce as
planned; the persisted-counter fallback was not needed.
**Scope:** Confidentiality of AegisDB's on-disk state — the append-only log and
the derived checkpoints — under an operator-supplied key, without taking on an
external crypto dependency.

The append-only `memory.log` is the source of truth; `memory.index` (hash) and
`memory.sem` (HNSW) are derived checkpoints; `metadata.db` holds the `next_id`
floor. Today all four are plaintext on disk. This design encrypts the log's
frame payloads and the checkpoints so that a stolen disk, volume snapshot, or
backup tarball does not leak stored memories.

## 1. Goals & non-goals

**Goals**

- **At-rest confidentiality.** An attacker who reads the data directory, a
  backup tarball, or a detached volume learns nothing about record contents,
  tags, embeddings, or relationships without the key.
- **Tamper-evidence.** Silent modification of on-disk frames or checkpoints is
  detected (authenticated encryption), not just accidental corruption (today's
  CRCs).
- **Stay single-binary / no-deps.** Vendor a small, audited cipher the way
  `src/util/sha256.c` is already vendored — no OpenSSL/libsodium link.
- **Stay in AegisDB's grain.** Reuse the frame format, the recovery/scan path,
  compaction, backup, and replication with the smallest surface change that is
  still correct. No new storage engine.
- **Opt-in and backward-compatible.** A data directory created without a key
  stays plaintext and keeps working untouched. Encryption is enabled per
  data directory by supplying a key.

**Non-goals** (explicitly out of scope)

- **Not in-memory confidentiality.** Indexes and records are plaintext in RAM
  while the server runs; this protects data *at rest*, not a live process's
  address space or a core dump.
- **Not a transport control.** The wire protocol stays plaintext; TLS remains a
  proxy concern (see the replication design). Frame payloads happening to travel
  encrypted over replication is a side benefit, not a security guarantee for the
  channel (the handshake token is still plaintext on the wire).
- **Not key management.** AegisDB loads a key from a file the operator secures;
  it does not do KMS integration, envelope keys, or automatic rotation (see
  §9 for a rotation path enabled by this design).
- **Not a replacement for auth.** Encryption gates *disk* access, not *client*
  access; auth tokens still govern the wire protocol.

## 2. Threat model

Protects against: theft of the data directory, a filesystem/volume snapshot, or
a backup artifact; an attacker with read (or read/write) access to the files
while the server is **not** the one mediating access.

Does **not** protect against: a compromised running server (RAM holds
plaintext), an attacker who also has the key file, traffic interception (use a
proxy), or metadata inference from file sizes / frame counts / timing.

## 3. Cipher choice

**XChaCha20-Poly1305** (an AEAD: XChaCha20 stream cipher + Poly1305 MAC), vendored
as `src/util/chacha20poly1305.c` / `include/aegisdb/aead.h`.

Rationale:

- **Software-friendly & constant-time.** ChaCha20/Poly1305 are add-rotate-xor
  and field arithmetic — fast and side-channel-resistant in portable C with no
  hardware-accel dependency (unlike AES-GCM, which is either slow or needs
  AES-NI to be safe and fast). Matches the no-deps, portable-C posture.
- **Extended nonce removes the hardest correctness hazard.** The 192-bit
  (24-byte) nonce of XChaCha20 makes a **randomly chosen** per-frame nonce safe:
  collision probability is negligible (~2⁻⁹⁶ at any realistic frame count), so we
  need no persistent nonce counter, no per-frame sequencing, and no special
  handling across compaction or replica promotion (all of which reuse or reset
  offsets). This is the decisive simplification versus plain ChaCha20-Poly1305.
- **Small, well-specified, testable.** ChaCha20 and Poly1305 have RFC 8439 test
  vectors; XChaCha20's HChaCha20 subkey step has published vectors too. We commit
  the standard vectors as unit tests.

Key: 32 bytes (256-bit). One key per data directory.

> **Alternative considered — ChaCha20-Poly1305 (96-bit nonce, RFC 8439).** ~20
> fewer lines (no HChaCha20 subkey step) but a 96-bit nonce is unsafe to choose
> randomly at scale, forcing a persisted monotonic nonce counter in
> `metadata.db` plus careful handling so compaction (rewrites frames) and replica
> promotion (a different node resumes writing) never reuse a (key, nonce) pair.
> The extended-nonce variant buys us out of all of that for a few extra lines;
> recommended.

## 4. Key material & configuration

New flag, mirroring `--auth-token-file` (never pass the key in `argv` — it would
leak via `/proc/<pid>/cmdline` and shell history):

```
--encryption-key-file <path>
```

The file holds a 32-byte key encoded as 64 hex chars (or base64), on one line,
`0600`. On startup:

1. Read + decode + length-check the key. A malformed key is a hard startup
   error (fail closed — never silently run unencrypted when a key was intended).
2. Derive a **key fingerprint** = first 12 hex of `SHA-256(key)` (never the key
   itself), for logs and the backup manifest, so an operator can tell *which*
   key a directory/backup needs without exposing it. This reuses the
   fingerprint idea already used for auth tokens.
3. Reconcile with the data directory's existing state (§5.3).

A helper (`aegisdb gen-key`) prints a fresh random 32-byte key in the accepted
encoding, analogous to `gen-token`.

## 5. Log encryption

### 5.1 Frame format (v3, encrypted)

Today's v2 frame (`include/aegisdb/log.h`) is:

```
[MAGIC u32][LEN u32][PAYLOAD_CRC u32][HEADER_CRC u32][PAYLOAD: LEN bytes]   (16-byte header)
```

The encrypted frame is a new **v3** variant, self-describing via a distinct
magic so recovery can tell them apart the same way v1→v2 is distinguished today:

```
[MAGIC_ENC u32][LEN u32][NONCE 24B][HEADER_CRC u32][CIPHERTEXT: LEN bytes][TAG 16B]
 └───────────────── 32-byte header ─────────────────┘
```

- `LEN` is the plaintext length. XChaCha20 is a stream cipher, so
  `len(ciphertext) == len(plaintext) == LEN` and **all frame-offset arithmetic
  keeps working** by simply accounting for the larger header (32 vs 16) and the
  trailing 16-byte tag.
- `HEADER_CRC` covers `MAGIC_ENC|LEN|NONCE` (the 32-byte-minus-crc prefix). Its
  job is unchanged: let the recovery scanner **resynchronize and skip a damaged
  frame without the key**, using `LEN` to find the next frame boundary. It is a
  keyless integrity aid, not a confidentiality control.
- The **AEAD** authenticates the ciphertext with the plaintext header prefix
  (`MAGIC_ENC|LEN|NONCE`) as **associated data (AAD)**, so tampering with the
  length or nonce fails the tag. `TAG` is the Poly1305 tag. The tag is a strictly
  stronger integrity check than the old `PAYLOAD_CRC`, which is therefore dropped
  for v3 frames.
- `NONCE` is 24 random bytes drawn per frame (see §3).

Per-frame overhead vs. v2: +16 header + 16 tag = **+32 bytes/frame**.

### 5.2 Read / write / recovery path

Touch points, all in `src/storage/`:

- `log_append` — build the v3 header, draw a nonce, AEAD-seal the payload, write
  `header || ciphertext || tag`. `lf->size` accounting adds the tag length.
- `log_read` — read the header, validate `HEADER_CRC`, read `ciphertext || tag`,
  AEAD-open. A failed tag is returned as a read error (distinct log message:
  authentication failure vs. plain corruption).
- `log_scan` (`recovery.c`) — unchanged control flow; per-frame it now decrypts.
  A frame that fails to decrypt is treated like a corrupt frame (skip + resync),
  so a partially-damaged encrypted log still recovers the good frames. Recovery
  of a large log now pays one AEAD-open per frame (see §8).
- `compaction.c` — reads live frames (decrypt) and writes them into the new-
  generation log (**fresh random nonces**; never reuse the old ones). The
  generation bump already forces replica re-bootstrap.
- The `LogFile` gains a borrowed `const Aead *` (NULL = plaintext mode).

The v1→v2 in-place migration precedent (`log.c`) shows the shape of a format
transition; see §5.3 for the plaintext→encrypted case.

### 5.3 Mode reconciliation & migration

A log must be uniformly plaintext or uniformly encrypted; the mode is discovered
at `log_open` from the first frame's magic (empty log ⇒ mode follows config):

| Existing log | Key given? | Behavior |
|---|---|---|
| empty / none | no  | plaintext (today's behavior) |
| empty / none | yes | encrypted from the first append |
| plaintext    | no  | plaintext |
| plaintext    | yes | **refuse to start**; direct the operator to the offline migrator |
| encrypted    | yes (fp matches) | run |
| encrypted    | yes (fp mismatch) | **refuse** (wrong key) |
| encrypted    | no  | **refuse** (key required) |

Converting an existing plaintext directory is an explicit, offline one-shot
(never an implicit rewrite of the source of truth):

```
aegisdb --encrypt-migrate --data-dir <dir> --encryption-key-file <key>
```

It rewrites `memory.log` into a v3 log and re-writes the checkpoints, atomically
(write new files, fsync, rename), leaving the originals until success — the same
discipline as compaction and restore. The key fingerprint is recorded (§7).

## 6. Checkpoint encryption

`memory.index` (hash, header `"AIDX" + u32 version + …`) and `memory.sem` (HNSW)
are written wholesale via tmp-file + rename. Encrypt them as **whole-file AEAD**:

```
[CKPT_MAGIC u32][u32 version][NONCE 24B][CIPHERTEXT: whole checkpoint body][TAG 16B]
```

- Written atomically as today; on load, decrypt then parse the existing body
  format unchanged.
- A wrong key or a failed tag is treated exactly like a **missing/corrupt
  checkpoint**: recovery already falls back to a full log scan, so a key mismatch
  degrades to "rebuild from the (encrypted) log," not a crash.
- The HNSW checkpoint can be large; the first cut encrypts it as one buffer
  (it is already materialized to write). Chunked/streaming AEAD is a follow-up if
  memory pressure during checkpointing shows up.
- `metadata.db` holds only the `next_id` floor — a non-secret counter — so it is
  left plaintext (documented). Revisit if it ever carries sensitive fields.

## 7. Backup / restore

Snapshots (`snapshot` op, `--restore`) copy `memory.log` + checkpoints
byte-for-byte, so an encrypted directory's snapshot **stays encrypted** with no
extra work. Changes:

- `manifest.json` gains `"encrypted": true` and `"key_fingerprint": "<12hex>"`.
- `--restore` into a directory validates the configured key's fingerprint
  against the manifest **before** copying, and the recovery-time embedding-dim
  probe (which reads log frames) decrypts with the key. Wrong/absent key ⇒ a
  clear, early error (`restore.c` already validates the manifest and log size;
  this adds a fingerprint check in the same place).

## 8. Replication interaction

The streamer ships **raw log frames byte-identically** and the replica writes
them to its own log verbatim (byte-identical offsets). Encrypted frames ship as
ciphertext and the replica stores them as-is, so:

- An encrypted primary requires each replica to be configured with the **same
  `--encryption-key-file`** (the replica must decrypt frames it reads to serve
  `get`/`search`). Add the primary's **key fingerprint to the subscribe
  handshake** so a mis-keyed replica is rejected fast with a clear reason rather
  than failing later on every read.
- Random per-frame nonces make **promotion** safe with no bookkeeping: a promoted
  replica appends new frames with its own fresh random nonces — no risk of
  reusing a (key, nonce) pair the old primary used.
- The replication *token* and *key* are distinct secrets with distinct jobs
  (subscribe authorization vs. frame confidentiality); both are required for an
  encrypted replica.

## 9. Rotation (future)

Not in this design, but enabled by it: because compaction already rewrites the
whole live log under a new generation, a "rotate to a new key" operation is a
compaction that reads with the old key and writes with the new one, then updates
the fingerprint — a natural extension once the above lands. Noted so the format
choices here don't foreclose it.

## 10. Testing

- **Cipher:** RFC 8439 ChaCha20 + Poly1305 vectors; HChaCha20 subkey vectors;
  XChaCha20-Poly1305 seal/open round-trip; a flipped ciphertext/tag/AAD byte
  fails `open` (tamper detection).
- **Log:** append→read round-trip encrypted; recovery of an encrypted log;
  recovery skips a tampered frame and recovers the good tail; wrong key ⇒ frames
  fail to open (degrade, not crash).
- **Checkpoint:** encrypt/decrypt round-trip; wrong key ⇒ falls back to log scan.
- **Migration:** plaintext→encrypted one-shot yields a byte-recoverable database
  with identical query results; refuses on fingerprint mismatch.
- **Backup/restore:** snapshot of an encrypted dir restores with the matching
  key and is rejected (early, clear error) with a mismatched/absent key.
- **Replication:** shared-key primary/replica converges; mis-keyed replica is
  rejected at handshake.
- All new C runs under the existing ASan+UBSan and TSan CI.

## 11. Rollout (PR sequence)

1. **Vendored AEAD + vectors** — `chacha20poly1305.c`, `aead.h`, RFC 8439 /
   XChaCha20 test vectors. Self-contained, no wiring; lowest risk. Add to the
   Makefile and CMake source lists (remember the Makefile header-dep gotcha:
   `make clean` after touching headers).
2. **Key config + log encryption** — `--encryption-key-file`, `gen-key`, mode
   reconciliation, v3 frame in `log.c`/recovery/compaction, and the offline
   `--encrypt-migrate`. Largest, highest risk (format + source-of-truth).
3. **Checkpoint encryption** — hash + HNSW checkpoints.
4. **Backup/restore + replication integration + docs** — manifest fingerprint,
   restore key check, handshake fingerprint; update `README`, `.env.example`
   (`AEGIS_ENCRYPTION_KEY_FILE`), and this doc's status to "implemented."

## 12. Open questions

- **Salt vs. extended nonce.** This doc commits to XChaCha20 random nonces. If
  vendoring HChaCha20 is undesirable, fall back to ChaCha20-Poly1305 with a
  persisted nonce counter (§3 alternative) — larger blast radius, documented
  there.
- **Checkpoint streaming.** Whole-file AEAD first; revisit if HNSW checkpoint
  size makes the one-buffer encrypt a memory problem.
- **Key file reload.** Rotation (§9) aside, do we ever reload the key without a
  restart? Assumed no for the first cut.