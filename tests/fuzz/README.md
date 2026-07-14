# Fuzzing

Coverage-guided fuzzing of the two attacker-reachable parse surfaces:

| Target                 | Entry point                              | Why |
|------------------------|------------------------------------------|-----|
| `fuzz_record_decode`   | `record_decode` (`src/memory/record.c`)  | Binary log-record codec — reached via a tampered log, a malicious `--restore` snapshot, or a replication frame (no CRC gate on the wire). Source of the one CRITICAL bug so far (embedding-size overflow, PR#115). |
| `fuzz_wire`            | `aegis_request_handle` (`src/protocol/json_request.c`) | Whole client request front end: cJSON parse, the `jr_*` extractors, dispatch, response encode. |

## Build flavors

Both targets share one source file, built two ways:

- **`make fuzz`** — libFuzzer + ASan/UBSan binaries under `build/fuzz/`. Needs
  clang (libFuzzer supplies `main`). Run one for a while:
  ```sh
  make fuzz FUZZ_CC=clang
  ./build/fuzz/fuzz_record_decode -max_total_time=300 tests/fuzz/corpus/record
  ./build/fuzz/fuzz_wire          -max_total_time=300 tests/fuzz/corpus/wire
  ```
- **`make fuzz-regress`** — replays the seed corpus + any checked-in crashers
  through `standalone_main.c` (no libFuzzer, so it builds under the default
  `$(CC)`/gcc). Deterministic and fast; this is the per-PR CI gate. Run it under
  sanitizers to match CI:
  ```sh
  CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
  LDFLAGS="-fsanitize=address,undefined" make fuzz-regress
  ```
- **`make fuzz-corpus`** — regenerate the seed corpus (`gen_seeds.c`).

## Corpus layout

```
corpus/
  record/            seed inputs for fuzz_record_decode (valid encoded records)
  wire/              seed inputs for fuzz_wire (valid NDJSON request lines)
  crashers/record/   minimized crash inputs → permanent regression guards
  crashers/wire/     (created when the first crash is filed)
```

## When libFuzzer finds a crash

Minimize it, drop the input into `corpus/crashers/<target>/`, and commit it.
`make fuzz-regress` (and the per-PR CI job) then replays it forever, so the bug
can't silently come back — the same pattern as
`tests/unit/test_record.c::test_decode_rejects_embedding_overflow`.

## CI

- **`fuzz-regress`** job — every PR; corpus + crasher replay under ASan/UBSan.
- **`fuzz`** job — nightly `schedule` + manual `workflow_dispatch` only; runs
  libFuzzer for `FUZZ_SECONDS` per target and uploads any `crash-*` artifacts.
  Kept off the PR path so a fresh find never blocks unrelated work.