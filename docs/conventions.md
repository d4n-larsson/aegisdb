# C conventions

The server is C17, built `-Wall -Wextra` and warning-clean (CI builds with
`-Werror`; run `make check WERROR=1` locally). The codebase is deliberately
consistent — this document records the conventions a code-quality review found in
force, so they can be followed rather than rediscovered.

## Types

- **Structs and enums are PascalCase**: `AegisDB`, `MemoryRecord`, `Config`,
  `HashIndex`, `TimeIndex`, `HnswParams`, `Compactor`. Enum constants are
  `SCREAMING_SNAKE` (`MEM_SEMANTIC`, `AEGIS_ERR_NOT_FOUND`, `MOP_INSERT`).
- **One deliberate exception**: `aegis_status_t` (errors.h) uses the
  `snake_case_t` spelling. It is the result type of the public C result-code API
  (`aegis_status_code` / `aegis_status_message`) and is styled after the C
  standard library's own `*_t` types. New *internal* types should be PascalCase.

## Functions

- **`<module>_<verb>`** is the dominant scheme for a module's API:
  `hash_index_get`, `semantic_index_search`, `working_store_create`,
  `tenant_rate_allow`, `replication_source_start`, `db_open`.
- **Short module prefixes** are used where they are pervasive and readable, and
  are equally valid: `qe_` (query engine, `query_engine.h`), `jr_` (JSON request
  helpers, `json_request.h`), `fs_` (filesystem, `fsutil.h`), `net_`
  (`netio.h`), `aegis_` (crypto/util primitives, e.g. `aegis_put_u32le`).
- **CLI entry points** keep their historical names: `client_main`,
  `gen_token_main`, `gen_key_main` (client.h) are the `argv`-style subcommand
  entry points dispatched from `main.c`, not module APIs.
- File-local helpers are `static` and may use terse names scoped to their file.

## Error handling

Two idioms, split by layer — this is intentional, not drift:

- **Low-level modules** (storage, log, index, crypto, fsutil, netio) return
  `int`: `0` on success, `-1` on failure. Booleans are `int`.
- **The query layer** returns `aegis_status_t` (see errors.h), which maps to the
  wire-protocol error codes. Internal-only failures collapse to
  `AEGIS_ERR_INTERNAL` on the wire.

## Headers & sizing

- Include guards are `AEGISDB_<NAME>_H` across every header.
- Include order within a `.c`: own header, blank line, system headers
  (alphabetized), blank line, project headers (alphabetized).
- Path buffers use `AEGIS_PATH_MAX`; the configured data dir is capped at
  `AEGIS_DATA_DIR_MAX`; stream copy/scan buffers use `AEGIS_IO_BUF_SIZE`
  (types.h). All on-disk/wire integers go through the little-endian codec in
  endian.h (`aegis_{put,get}_u{16,32,64}le`) — never a raw `memcpy` of a scalar.

## Tooling

Formatting is enforced with **clang-format** (`.clang-format`: LLVM base,
4-space indent, no tabs, 80-column, comment reflow off). CI fails a build whose
tree is not formatted. Run it before committing:

```sh
make format         # rewrite in place
make format-check   # verify only (what CI runs)
```

clang-format output drifts between major versions, so pin the tool to the CI
version to avoid spurious diffs:

```sh
python3 -m venv .venv && .venv/bin/pip install clang-format==22.1.8
make format CLANG_FORMAT=.venv/bin/clang-format
```

The naming/idiom conventions above are beyond what clang-format checks; a
clang-tidy configuration to enforce them is being added separately.