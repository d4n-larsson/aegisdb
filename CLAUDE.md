# AegisDB

A persistent memory database for AI agents: a C server speaking newline-delimited
JSON over TCP, plus a Claude Code memory integration (Python MCP server + hooks)
under `integrations/claude-code/`.

## Build & test

- Build: `make` (or CMake: `cmake -B build && cmake --build build`).
- C unit tests: `make test`. Wire-protocol contract tests: `make integration`.
  Both: `make check`.
- Integration (Python) tests: `cd integrations/claude-code && make test`.
- **Gotcha:** the `Makefile` does not track header dependencies — run
  `make clean && make` after editing any header in `include/`, or stale objects
  can produce silent struct/ABI mismatches.

## Run

```sh
./build/aegisdb --data-dir ./data --port 9470 --embedding-dim 1024
```

Add `--auth-token <tok>` (or `--auth-token-file <path>`) to require authentication.

## Layout

`src/` mirrors the runtime pipeline (network → protocol → query → storage);
`include/aegisdb/` holds public headers; `docs/` has the wire-protocol reference
and quickstart; `integrations/claude-code/` is the Claude Code memory integration.
