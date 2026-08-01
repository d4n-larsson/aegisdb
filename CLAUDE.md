# AegisDB

A persistent memory database for AI agents: a C server speaking newline-delimited
JSON over TCP, plus a Claude Code memory integration (Python MCP server + hooks)
under `integrations/claude-code/`.

## Build & test

- Build: `make` (or CMake: `cmake -B build && cmake --build build`).
- C unit tests: `make test`. Wire-protocol contract tests: `make integration`.
  Both: `make check`.
- Integration (Python) tests: `cd integrations/claude-code && make test`.
- Header dependencies **are** tracked (the `Makefile` compiles with `-MMD -MP`;
  CMake does it natively), so editing a header rebuilds every object that
  included it. `make clean` is not needed for that.
- **Gotcha:** objects are *not* keyed by build flags, so switching `CFLAGS`
  between runs (e.g. into or out of a `-fsanitize=...` build) reuses the objects
  from the previous flags and fails at link with missing `__asan_*`/`__ubsan_*`
  symbols. Run `make clean` when changing `CFLAGS`/`LDFLAGS`.

## Run

```sh
./build/aegisdb --data-dir ./data --port 9470 --embedding-dim 1024
```

Add `--auth-token <tok>` (or `--auth-token-file <path>`) to require authentication.

## Layout

`src/` mirrors the runtime pipeline (network → protocol → query → storage);
`include/aegisdb/` holds public headers; `docs/` has the wire-protocol reference
and quickstart; `integrations/claude-code/` is the Claude Code memory integration.
