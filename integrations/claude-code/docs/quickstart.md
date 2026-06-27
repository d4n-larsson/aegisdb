# Quickstart: AegisDB ↔ Claude Code Memory Integration


## Prerequisites

- A running **AegisDB** server (see the database quickstart). Launch it with the
  embedding dimension you intend to use, e.g. `--embedding-dim 1024`.
- **Python 3.10+** and Claude Code installed.
- (Optional, for semantic recall) a `VOYAGE_API_KEY`, **or** the optional local
  embedding model, **or** neither (semantic search disables; tag/time recall
  still works).

## Install

Only the MCP tools server needs installing (for the `mcp` SDK); the hooks run on
the standard library alone. Use a virtual environment — on Debian/Ubuntu a global
`pip install` is blocked by PEP 668 (`error: externally-managed-environment`):

```bash
python3 -m venv .venv
.venv/bin/pip install -e integrations/claude-code            # core + `mcp` SDK
.venv/bin/pip install -e "integrations/claude-code[voyage]"  # optional: Voyage SDK
.venv/bin/pip install -e "integrations/claude-code[local]"   # optional: local model
```

Record the venv Python path (`$(pwd)/.venv/bin/python`) for the MCP registration
below. (`pipx install -e integrations/claude-code` is an alternative.)

## Configure

Configuration resolves from defaults → config file → environment (highest wins).
Minimal env setup:

```bash
export AEGIS_HOST=127.0.0.1
export AEGIS_PORT=9470
export AEGIS_EMBEDDING_DIMENSIONS=1024      # MUST match the running server
export VOYAGE_API_KEY=...                   # omit to run in `none` mode
# export AEGIS_NAMESPACE=my-project         # defaults to the project directory
```

Start AegisDB at the matching dimension:

```bash
./build/aegisdb --data-dir ./data --port 9470 --embedding-dim 1024
```

## Register the MCP server

Project-scope (checked into the repo) `.mcp.json`, pointing at the venv's Python
via an absolute path:

```jsonc
{
  "mcpServers": {
    "memory": {
      "command": "/abs/path/to/repo/.venv/bin/python",
      "args": ["-m", "aegis_mcp.server"],
      "env": { "AEGIS_NAMESPACE": "my-project", "AEGIS_EMBEDDING_DIMENSIONS": "1024" }
    }
  }
}
```

Confirm Claude Code lists the tools (`mcp__memory__memory_save`, etc.).

## Enable automatic recall & capture

Add to `.claude/settings.json`:

```jsonc
{
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "python3 integrations/claude-code/hooks/recall_hook.py" } ] }
    ],
    "SessionEnd": [
      { "hooks": [ { "type": "command", "command": "python3 integrations/claude-code/hooks/capture_hook.py" } ] }
    ]
  }
}
```

## Verify

**1. Explicit save/recall (US1)** — in a Claude Code session:

> "Remember that this project deploys with `make ship`."

The agent calls `memory_save`. In a **new** session:

> "How do I deploy this project?"

The agent calls `memory_search` (or the recall hook injects it) and answers from
memory.

**2. Automatic recall (US2)** — with the recall hook enabled, simply ask a
question related to a stored memory; the relevant memory appears in context
without any explicit recall request.

**3. Isolation (US4)** — repeat in a different project directory; the first
project's memories are not returned.

**4. Degradation (FR-009/SC-003)** — stop AegisDB, then use Claude Code normally:
sessions remain fully usable; recall silently injects nothing; explicit tool calls
return `ok:false, error:"unavailable"`.

## Run the tests

```bash
cd integrations/claude-code
pytest                       # unit + contract (offline, fake embedding provider)
pytest -m integration        # end-to-end against a running aegisdb
```

## Troubleshooting

- **Semantic search returns nothing / errors about dimension** → the configured
  `AEGIS_EMBEDDING_DIMENSIONS` does not match the server's `--embedding-dim`. Make
  them equal and restart both.
- **Recall never appears** → check `recall_enabled`, that the hook is wired in
  `settings.json`, and that AegisDB is reachable (`ping`).
- **No semantic ranking, only tag/time hits** → embeddings are in `none` mode
  (no key / provider). Set `VOYAGE_API_KEY` or install the local extra.