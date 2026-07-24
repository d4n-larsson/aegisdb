---
name: aegis-setup
description: Set up AegisDB as persistent memory for this Claude Code project. Asks a few short questions (local vs shared server, embeddings, capture quality, auth), then scaffolds .mcp.json and the recall/capture hooks via `aegisdb-init`.
disable-model-invocation: true
argument-hint: "[host] [port]"
---

# Set up AegisDB memory for this project

Your job is to wire AegisDB in as this project's persistent memory by gathering a
few settings and then running the `aegisdb-init` scaffolder. Do **not** hand-edit
`.mcp.json` or `.claude/settings.json` yourself — `aegisdb-init` writes them
correctly and idempotently. Keep the conversation short.

## 1. Gather the settings

Ask the user these questions (offer the defaults; accept "just use defaults").
If they passed arguments, treat `$0` as the host and `$1` as the port.

1. **Where is the server?**
   - *Local, and I need one* → you'll start one in step 2.
   - *Local, already running* → host `127.0.0.1`, port `9470`.
   - *Shared / team server* → ask for the host and port, and the **auth token**
     they were given. (With a namespaced token they do **not** need a namespace.)
2. **Embeddings** (controls semantic recall; the dimension MUST match the
   server's `--embedding-dim`):
   - `none` (default) — tag/time recall only, no setup.
   - `voyage` — best recall; dimension `1024`; needs `VOYAGE_API_KEY` in the
     environment.
   - `local` — offline; dimension `384`; downloads a model on first use.
3. **Capture quality** (how finished sessions become memories):
   - `none` (default) — heuristic: keep salient sentences from the transcript.
   - `claude-code` — distil the session into durable facts with an LLM (dedup +
     supersede contradictions on write), reusing the local `claude` CLI auth —
     **no API key needed**. Best quality; recommended if they have the CLI.
   - `anthropic` / `openai` — same, via API; needs `ANTHROPIC_API_KEY` /
     `OPENAI_API_KEY` in the environment the hook runs in (never written to the
     project). Only offer these if they can't use the `claude` CLI.
4. **Namespace** — only if they are *not* using a namespaced auth token. Default:
   derive from the project directory.

## 2. (Only if they need a local server) offer to start one

If they chose "local, and I need one", offer to run:

```bash
docker run -d --name aegisdb -p 9470:9470 -v aegis-data:/data \
  ghcr.io/d4n-larsson/aegisdb:latest --data-dir /data --embedding-dim 1024
```

Match `--embedding-dim` to their embedding choice (1024 for voyage/none, 384 for
local). If they don't have Docker or want auth/encryption/quotas, point them to
the [team server tutorial](https://github.com/d4n-larsson/aegisdb/blob/main/docs/tutorial-team-server.md)
and continue — `aegisdb-init` can still write the config now.

## 3. Preview, then scaffold

First show what will be written (this changes nothing):

```bash
uvx --from aegisdb-mcp aegisdb-init --print \
  --host <HOST> --port <PORT> --embedding-mode <MODE> --embedding-dim <DIM> \
  [--extract-mode <EXTRACT>] [--namespace <NS>] [--auth-token <TOKEN>]
```

If it looks right, run it for real (drop `--print`, add `--yes`; add `--force`
only if it reports an existing, different `memory` server you want to replace):

```bash
uvx --from aegisdb-mcp aegisdb-init --yes \
  --host <HOST> --port <PORT> --embedding-mode <MODE> --embedding-dim <DIM> \
  [--extract-mode <EXTRACT>] [--namespace <NS>] [--auth-token <TOKEN>]
```

Omit `--extract-mode`/`--namespace`/`--auth-token` when they're blank/`none`. The
command prints a connectivity check at the end.

## 4. Wrap up

Report what was written and tell the user to **restart Claude Code in this
project** so it picks up the `memory` MCP server and the recall/capture hooks. If
the connectivity check failed, help them get the server running, then re-run.

Optionally point them at the **memory inspector** — a local browser UI to
browse/search what's been captured, see why each hit ranked, and edit or delete
memories: `docker compose --profile inspector up` (then <http://127.0.0.1:8600/>),
or `make inspector` from a clone.
