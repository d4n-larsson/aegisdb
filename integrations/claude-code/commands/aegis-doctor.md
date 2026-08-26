---
description: Check this project's AegisDB memory wiring and explain anything that is broken.
argument-hint: "[--no-write]"
allowed-tools: Bash(uvx --from aegisdb-mcp aegisdb-doctor:*), Bash(aegisdb-doctor:*), Read
disable-model-invocation: true
---

# Check the memory wiring

Run the doctor and report what it found:

```bash
uvx --from aegisdb-mcp aegisdb-doctor $ARGUMENTS
```

It checks every link between this project and its memory — which config file is
in force and the namespace it resolves to, whether the server answers, whether
the embedding dimension agrees with the server's, whether the embedding provider
is usable rather than merely configured, whether `.mcp.json` and both hooks are
wired, whether the read path can run — and finishes with a real
`save → search → delete` round trip. It exits non-zero if anything failed.

Then:

- **Everything passed** — say so in one line. Do not restate the report.
- **Something failed** — each failure already names its fix (the `→` line).
  Relay the failures and the fixes plainly, then offer to apply the ones you can:
  most are a matter of running `uvx --from aegisdb-mcp aegisdb-init` (which is
  idempotent), or editing `.aegisdb/config.json`. Ask before writing anything.
- **`uvx` is not installed** — the doctor ships with the `aegisdb-mcp` package;
  point them at [uv](https://docs.astral.sh/uv/) or an existing checkout's venv.

Two things worth knowing when you interpret the output:

- **A failure is not an emergency.** Memory is best-effort by design: with the
  server down or the hooks missing, Claude Code still works — it just will not
  remember. Say that rather than alarming anyone.
- **The wiring is only half of it.** A green report means the plumbing is
  connected, not that anything useful is stored. If they are asking because
  recall feels empty, check the corpus too: `memory_search` for something they
  know they saved, and remember that `embedding_mode: none` means recall matches
  tags and time but not meaning.

If they have not set memory up in this project at all, the doctor will say so —
run `/aegis-setup` instead of trying to fix the report line by line.
