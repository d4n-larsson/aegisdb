# AegisDB ↔ Claude Code Memory Integration

Make [AegisDB](https://github.com/d4n-larsson/aegisdb) the **persistent long-term memory** of Claude
Code. The agent gets memory **tools** (save/search/get/update/relate) via an MCP
server, plus automatic **recall** and **capture** via hooks — so knowledge learned
in one session (decisions, conventions, fixes, preferences) is available in later
ones without the user re-explaining it. Each project keeps its own isolated memory.

## How it works

```
Claude Code ──(MCP stdio)──▶ aegis_mcp.server ──┐
Claude Code ──(hooks)──────▶ recall/capture ────┼──▶ AegisDB (NDJSON/TCP)
                             embeddings ─────────┘
```

- **MCP tools** (`mcp__memory__memory_save`, `_search`, `_get`, `_update`, `_relate`) —
  explicit, model-driven memory.
- **`UserPromptSubmit` hook** — automatic recall: injects relevant memories into
  context before each turn, best-effort under a time budget.
- **`SessionEnd` hook** — automatic capture: persists salient session outcomes.
- **Embeddings** — pluggable provider (Voyage / local / none) turns text into
  vectors for semantic recall; the integration never asks the agent for vectors.

All logic lives in dependency-free modules under `aegis_mcp/`; only the MCP server
entry point needs the `mcp` SDK. Memory is always best-effort: if AegisDB is down,
the agent stays fully usable.

## Why it saves tokens

Long context is the real cost driver, and this integration keeps durable
knowledge **out** of the window — feeding back only what's relevant per prompt —
so you spend tokens on the work, not on re-establishing context.

- **Recall instead of re-explaining.** Stack, conventions, decisions, and gotchas
  learned earlier are injected automatically, so you stop re-pasting them every
  session and the model stops re-deriving them.
- **A relevant slice, not a dump.** Recall ranks by similarity × importance ×
  confidence and injects only the top matches — capped by `AEGIS_RECALL_TOP_K`
  and filtered by `AEGIS_RECALL_MIN_SCORE`, within `AEGIS_RECALL_TIME_BUDGET_MS`.
  The *selection* happens in AegisDB, so the model never sees (or pays to sift)
  the rest.
- **Short sessions, full knowledge.** Because memory is external, you can start
  fresh sessions instead of dragging one giant transcript whose every turn
  re-bills the whole context.
- **Distilled, then reused.** Capture stores salient outcomes (filtered by
  `AEGIS_CAPTURE_MIN_SALIENCE`), not raw logs — and a [shared team
  server](#shared-team-server) lets everyone reuse context established once.

Recall does add a small, bounded amount per turn (the injected memories, plus a
query embedding if enabled) — far less than re-pasting context blocks or carrying
a long transcript, and tunable via the knobs above.

## Integrate with Claude Code (step by step)

From a zero state to working memory in six steps. Run these from your project root.

### 1. Start AegisDB

Pick an embedding dimension and use it everywhere (see the dimension note below).

```bash
./build/aegisdb --data-dir ./data --port 9470 --embedding-dim 1024
```

### 2. Make the integration available

Only the **MCP tools server** needs the package (it requires the `mcp` SDK). The
**hooks need no install** — they run on the standard library — so if you only want
automatic recall/capture, skip to step 3.

The package is published on PyPI as **`aegisdb-mcp`**, so the zero-clone path is to
let [`uv`](https://docs.astral.sh/uv/) fetch and run it on demand — nothing to
install or keep updated. Just have `uv` available and register `uvx aegisdb-mcp`
(step 4); `uvx` resolves the package the first time Claude Code launches it.

For local development from a checkout, install it editable into a venv instead
(on Debian/Ubuntu a plain `pip install` fails with PEP 668's
`externally-managed-environment`, so a venv is the clean fix):

```bash
python3 -m venv .venv
.venv/bin/pip install -e integrations/claude-code              # MCP server + `mcp` SDK
.venv/bin/pip install -e "integrations/claude-code[voyage]"    # optional: semantic embeddings
```

### 3. Choose an embedding mode

- **Semantic recall (recommended)**: `export VOYAGE_API_KEY=...` (Voyage is auto-selected).
- **Offline**: install the `[local]` extra and set `AEGIS_EMBEDDING_MODE=local`.
  The default local model (`all-MiniLM-L6-v2`) produces **384-dim** vectors, so set
  `AEGIS_EMBEDDING_DIMENSIONS=384` and start the server with `--embedding-dim 384`.
- **None**: skip this — semantic search disables and recall falls back to tags/time.

### 4. Register the MCP server

The simplest registration runs the published package with `uvx` — no clone, no
venv, no absolute paths. Either use the CLI:

```bash
claude mcp add memory --scope project \
  -e AEGIS_NAMESPACE=my-project \
  -e AEGIS_EMBEDDING_DIMENSIONS=1024 \
  -- uvx aegisdb-mcp
```

…or commit a project-scope `.mcp.json` (see [`examples/mcp.json`](https://github.com/d4n-larsson/aegisdb/blob/main/integrations/claude-code/examples/mcp.json)):

```jsonc
{
  "mcpServers": {
    "memory": {
      "command": "uvx",
      "args": ["aegisdb-mcp"],
      "env": { "AEGIS_NAMESPACE": "my-project", "AEGIS_EMBEDDING_DIMENSIONS": "1024" }
    }
  }
}
```

Pin a version with `uvx aegisdb-mcp@0.1.0`. If you installed editable into a venv
instead (step 2), point `command` at that venv's `aegisdb-mcp` console script (or
`.venv/bin/python` with `"args": ["-m", "aegis_mcp.server"]`) using an
**absolute path**, since Claude Code controls the launch directory.

### 5. Enable automatic recall & capture

Add the hooks to `.claude/settings.json` (see [`examples/settings.json`](https://github.com/d4n-larsson/aegisdb/blob/main/integrations/claude-code/examples/settings.json)).
The published package exposes them as console scripts, so `uvx` runs them with no
clone — the same zero-install path as the MCP server:

```jsonc
{
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "uvx --from aegisdb-mcp aegisdb-recall-hook" } ] }
    ],
    "SessionEnd": [
      { "hooks": [ { "type": "command", "command": "uvx --from aegisdb-mcp aegisdb-capture-hook" } ] }
    ]
  }
}
```

From a checkout, run the scripts by path instead:
`python3 integrations/claude-code/hooks/recall_hook.py` (and `capture_hook.py`).

### 6. Confirm it works

Start Claude Code in the project, then:

1. Run `/mcp` — the `memory` server should be listed `connected`, exposing
   `memory_save`, `memory_search`, `memory_get`, `memory_update`, `memory_relate`.
2. Ask: *"Remember that this project deploys with `make ship`."* → the agent calls
   `memory_save`.
3. Start a **new** session and ask: *"How do I deploy this project?"* → the recall
   hook injects the memory (or the agent calls `memory_search`) and answers from it.

If `/mcp` shows the server but tools error, AegisDB is unreachable — check it is
running on the configured host/port; the agent stays usable either way.

> Tools surface to the model as `mcp__memory__memory_save`, etc. The reference
> sections below cover every configuration option and the exact tool/hook contracts.

## Requirements

- A running **AegisDB** server, started with the embedding dimension you intend to
  use, e.g. `./build/aegisdb --embedding-dim 1024`.
- **Python 3.10+**.
- For semantic recall: a `VOYAGE_API_KEY` (Voyage), the optional local model, or
  neither (semantic search disables; tag/time recall still works).

## Install

Only the MCP server needs the package (for the `mcp` SDK). The simplest option
is **not to install it at all**: it is published on PyPI as `aegisdb-mcp`, so
registering `uvx aegisdb-mcp` lets [`uv`](https://docs.astral.sh/uv/) fetch and run
it on demand (see "Register the MCP server"). `pipx run aegisdb-mcp` works the same
way.

For development from a checkout, install it editable into a virtual environment —
on Debian/Ubuntu a global `pip install` is blocked by PEP 668
(`error: externally-managed-environment`):

```bash
python3 -m venv .venv
.venv/bin/pip install -e integrations/claude-code     # MCP server (needs the `mcp` SDK)
.venv/bin/pip install -e "integrations/claude-code[voyage]"   # optional: Voyage embeddings
.venv/bin/pip install -e "integrations/claude-code[local]"    # optional: local embeddings
```

The hooks and all core logic run on the standard library alone — **no install is
required** to use the hooks or to run the tests.

## Configure

Resolution precedence: defaults → JSON file (`AEGIS_CONFIG`) → environment →
explicit overrides.

| Env var | Default | Description |
|---------|---------|-------------|
| `AEGIS_HOST` | `127.0.0.1` | AegisDB host |
| `AEGIS_PORT` | `9470` | AegisDB TCP port |
| `AEGIS_CONNECT_TIMEOUT_MS` | `500` | connect timeout (degradation guard) |
| `AEGIS_READ_TIMEOUT_MS` | `1000` | per-request read timeout |
| `AEGIS_NAMESPACE` | derived from project dir | isolation boundary (AegisDB `agent_id`); **ignored when the token is namespaced** — the token's namespace then governs |
| `AEGIS_AUTH_TOKEN` | _(none)_ | bearer token sent with every request; required when the server enforces auth. A namespaced token also defines the tenant |
| `AEGIS_EMBEDDING_MODE` | `voyage` if key present, else `none` | `voyage` \| `local` \| `none` \| `fake` |
| `AEGIS_EMBEDDING_MODEL` | `voyage-3-large` | provider model id (Voyage mode) |
| `AEGIS_EMBEDDING_DIMENSIONS` | `1024` | **must match the server's `--embedding-dim`** |
| `AEGIS_RECALL_ENABLED` | `true` | toggle automatic recall |
| `AEGIS_RECALL_TIME_BUDGET_MS` | `800` | hard ceiling for recall |
| `AEGIS_RECALL_TOP_K` | `5` | max memories injected per turn |
| `AEGIS_RECALL_MIN_SCORE` | `0.2` | drop weak semantic matches |
| `AEGIS_CAPTURE_ENABLED` | `true` | toggle automatic capture |
| `AEGIS_CAPTURE_SCOPE` | `session` | `session` (SessionEnd) \| `turn` (Stop) |
| `AEGIS_CAPTURE_MIN_SALIENCE` | `0.5` | below this, nothing is captured |

> **Embedding dimension must match.** AegisDB validates that a stored vector's
> length equals its configured `embedding_dimensions`. Keep
> `AEGIS_EMBEDDING_DIMENSIONS` equal to the server's `--embedding-dim`
> (Voyage models emit the requested size via `output_dimension`). A mismatch
> surfaces as a clear error on the first embedded operation.
>
> `fake` mode is a deterministic, dependency-free provider for local development
> and tests — not for production recall quality.

## Register the MCP server

Project-scope `.mcp.json` (see [`examples/mcp.json`](https://github.com/d4n-larsson/aegisdb/blob/main/integrations/claude-code/examples/mcp.json)):

```jsonc
{
  "mcpServers": {
    "memory": {
      "command": "uvx",
      "args": ["aegisdb-mcp"],
      "env": { "AEGIS_NAMESPACE": "my-project", "AEGIS_EMBEDDING_DIMENSIONS": "1024" }
    }
  }
}
```

## Enable automatic recall & capture

Add to `.claude/settings.json` (see [`examples/settings.json`](https://github.com/d4n-larsson/aegisdb/blob/main/integrations/claude-code/examples/settings.json)).
`uvx` runs the packaged hooks with no clone (use the `python3 …/hooks/*.py` paths
from a checkout):

```jsonc
{
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "uvx --from aegisdb-mcp aegisdb-recall-hook" } ] }
    ],
    "SessionEnd": [
      { "hooks": [ { "type": "command", "command": "uvx --from aegisdb-mcp aegisdb-capture-hook" } ] }
    ]
  }
}
```

## Shared team server

Run **one** AegisDB for the whole team and point everyone's Claude Code at it.
Two arrangements, depending on whether projects should be isolated or share a
pool.

**Steps common to both** — run one server and keep it private:

```sh
# Prebuilt image (no toolchain needed); persists to a named volume.
docker run -d -p 9470:9470 -v aegis-data:/data \
    ghcr.io/d4n-larsson/aegisdb:latest \
    --data-dir /data --embedding-dim 1024 --auth-token-file /data/tokens.txt
```

Tokens travel in plaintext, so expose the port only over a VPN/WireGuard, an SSH
tunnel, or a TLS-terminating reverse proxy — AegisDB does not terminate TLS
itself. Every client must set `AEGIS_EMBEDDING_DIMENSIONS` to the server's
`--embedding-dim`.

### Isolated tenants (recommended)

Give each project (or person) a **namespaced token** so the server *enforces*
isolation — one tenant can never read another's memories, even by asking. Mint a
token per tenant (its plaintext is shown once; the file keeps only a hash):

```sh
aegisdb gen-token --namespace acme-api --scope rw   # paste the line into tokens.txt
```

Each project's `.mcp.json` carries its token. The token's namespace is
authoritative, so you do **not** need `AEGIS_NAMESPACE` — the server pins every
write and filters every read to the token's tenant:

```jsonc
{
  "mcpServers": {
    "memory": {
      "command": "uvx",
      "args": ["aegisdb-mcp"],
      "env": {
        "AEGIS_HOST": "memory.internal",
        "AEGIS_PORT": "9470",
        "AEGIS_AUTH_TOKEN": "<the gen-token plaintext>",
        "AEGIS_EMBEDDING_DIMENSIONS": "1024"
      }
    }
  }
}
```

Use `--scope ro` for a read-only token (writes are refused with `forbidden`).

### Shared pool (collaborate)

To have several people share **one common memory pool**, give them tokens in the
**same namespace** (or global `admin` tokens) and set the **same**
`AEGIS_NAMESPACE` on every client — that shared namespace is what joins the pool.
Note that `admin` tokens are not isolated: they can read and write any namespace,
so only hand them to trusted operators.

## Verify

See [quickstart](https://github.com/d4n-larsson/aegisdb/blob/main/integrations/claude-code/docs/quickstart.md) for the
full walkthrough (explicit save/recall, automatic recall, isolation, degradation).

## Test

```bash
cd integrations/claude-code
make test            # unit + contract + integration (stdlib unittest)
make unit            # offline, no backend needed
make integration     # launches ../../build/aegisdb automatically
```

Integration/contract tests launch the `aegisdb` binary from `../../build` and
skip automatically if it is not built. They use a deterministic `fake` embedding
provider, so no API key or network is needed.

## Layout

```text
aegis_mcp/
  client.py       # AegisDB NDJSON/TCP client (stdlib)
  config.py       # config + namespace resolution
  embeddings.py   # provider abstraction: voyage | local | none | fake
  results.py      # structured results + AegisDB error translation
  tools.py        # core save/search/get/update/relate logic
  recall.py       # automatic-recall query/format + time budget
  capture.py      # session salience heuristic + persistence
  server.py       # FastMCP binding (lazy-imports `mcp`)
hooks/
  recall_hook.py  # UserPromptSubmit
  capture_hook.py # SessionEnd / Stop
tests/            # unit, contract, integration (stdlib unittest)
examples/         # mcp.json, settings.json
```