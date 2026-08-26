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
  confidence and injects only the top matches — capped by `AEGIS_RECALL_TOP_K`,
  filtered by `AEGIS_RECALL_MIN_SCORE`, and de-duplicated by
  `AEGIS_RECALL_DEDUP_THRESHOLD` (so the same fact phrased several ways isn't
  injected repeatedly), within `AEGIS_RECALL_TIME_BUDGET_MS`. The *selection*
  happens client-side after AegisDB ranks, so the model never sees (or pays to
  sift) the rest.
- **A bounded block, not a runaway one.** The injected context is size-capped:
  each memory is truncated at `AEGIS_RECALL_MAX_CHARS_PER_MEMORY` (on a word
  boundary, marked `[…]`) and the whole block at `AEGIS_RECALL_CHAR_BUDGET` (a
  hard ceiling — even the top memory is bounded by it), so a few long memories
  can't quietly dominate a turn's tokens. Dropped memories are flagged with an
  explicit "N more omitted" trailer, so the model knows the list is partial (and
  can `memory_search` for the rest) rather than mistaking it for complete.
- **Short sessions, full knowledge.** Because memory is external, you can start
  fresh sessions instead of dragging one giant transcript whose every turn
  re-bills the whole context.
- **Distilled, then reused.** Capture stores salient outcomes (filtered by
  `AEGIS_CAPTURE_MIN_SALIENCE`), not raw logs — and a [shared team
  server](#shared-team-server) lets everyone reuse context established once.

Recall does add a small, bounded amount per turn (the injected memories, plus a
query embedding if enabled) — far less than re-pasting context blocks or carrying
a long transcript, and tunable via the knobs above.

## Fast path: one command

If you already have (or can start) a server, scaffold the whole client side —
`.mcp.json`, the recall/capture hooks, and [`.aegisdb/`](#aegisdb--the-projects-own-directory)
with a starter predicate vocabulary — with one command from your project root,
instead of the manual steps below:

```bash
# preview what it writes (changes nothing)
uvx --from aegisdb-mcp aegisdb-init --print

# do it (interactive; prompts for host/port/embeddings/auth)
uvx --from aegisdb-mcp aegisdb-init
```

It's idempotent and non-destructive (it won't clobber other MCP servers or hooks,
and only replaces an existing `memory` entry with `--force`). Flags let you drive
it non-interactively: `--host --port --namespace --auth-token --embedding-mode
--embedding-dim --yes`. Restart Claude Code afterward.

**It proves the wiring before writing it.** Everything the scaffolder writes —
the memory server and both hooks — runs through the same launcher, and a
launcher that cannot exec a console-script shim fails *silently*: no output, no
log, tools that never appear and hooks that never fire. So `aegisdb-init` asks
the memory server command to complete one MCP handshake first, and if the
console scripts cannot, it wires the project through `python -m` instead — the
same package, one less shim — and says so. Force either form with
`--launcher script|module`.

**No server yet?** Add `--start-local` and it brings one up in Docker before
writing anything, then reads the embedding dimension back from it:

```bash
uvx --from aegisdb-mcp aegisdb-init --start-local
```

That runs a named container (`aegisdb`) on a named volume (`aegis-data`), waits
until it actually answers a ping — a container can be running while the server is
still opening its data directory — and **adopts** an existing `aegisdb` container
rather than racing it for the port. Interactively you're offered this whenever
nothing is listening, so you don't have to know the flag. It never happens on
`--yes` alone or under `--print`: `--yes` means "don't ask me about the config",
not "do things to my Docker daemon". A non-local `--host` is refused, since
starting a container here and pointing the project at another machine is two
wrong things that look like one working setup.

**Even easier — the plugin.** Installing it needs no clone and no install step,
and it brings the guided setup with it:

```
/plugin marketplace add d4n-larsson/aegisdb
/plugin install aegisdb-memory@aegisdb
```

Then, in any project you want to give a memory:

```
/aegis-setup     # a few short questions, then it scaffolds this project
/aegis-doctor    # check the wiring whenever recall looks wrong
```

The plugin ships **only** the skill and the check — deliberately. The memory
server registration and the recall/capture hooks stay per-project, written by
`aegisdb-init`, because they are a per-project decision (which server, which
namespace, which embeddings) and because a plugin that registered them globally
would collide with the ones `aegisdb-init` writes: two `memory` servers, and
hooks that fire twice — recalling twice into one turn and capturing the same
session twice. One place owns that wiring.

The manual, step-by-step path follows for anyone who wants to see exactly what
those write.

## Integrate with Claude Code (step by step)

From a zero state to working memory in six steps. Run these from your project root.

### 1. Start AegisDB

Pick an embedding dimension **here** and let the client read it from the server
rather than repeating it (see the dimension note below).

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

**What's an embedding?** A model that turns a piece of text into a vector (a list
of numbers) encoding its *meaning*, so texts about similar things sit close
together. AegisDB uses this for **semantic recall**: it embeds your prompt and
finds stored memories whose vectors are nearest — so "how do I ship a release?"
can surface "deploys go through `make ship`" even with no shared keywords. The
vector's length is its **dimension**, and it must be identical on the server
(`--embedding-dim`) and every client (`AEGIS_EMBEDDING_DIMENSIONS`) — a mismatch
disables embeddings rather than storing unusable vectors.

Without embeddings, recall still works but falls back to **tags and time** only
(no meaning-based matching). Pick a provider:

| Mode | Model | Dim | Recall quality | Privacy | Cost | Offline | Per-client weight |
|------|-------|-----|----------------|---------|------|---------|-------------------|
| `none` (default) | — | — | tags/time only | 100% local | free | ✅ | none |
| `local` | `all-MiniLM-L6-v2` | 384 | good | 100% local | free | ✅ | `sentence-transformers` + ~80 MB model |
| `voyage` | `voyage-3-large` | 1024 | best | text sent to Voyage API | $ per call | ❌ | just an API key |

- **`voyage` (best recall)** — [Voyage AI](https://www.voyageai.com/) is a hosted
  embeddings service (the provider Anthropic recommends). `export VOYAGE_API_KEY=...`
  and it's auto-selected; clients stay lightweight, but memory text is sent to
  Voyage's API and billed per use. Use `--embedding-dim 1024`.
- **`local` (offline, free)** — install the `[local]` extra and set
  `AEGIS_EMBEDDING_MODE=local`. The default model `all-MiniLM-L6-v2` is a small
  sentence-transformer; on first use `sentence-transformers` downloads it (~80 MB)
  from the Hugging Face Hub and caches it under `~/.cache/`, then runs entirely on
  your CPU — nothing leaves the machine. It produces **384-dim** vectors, so set
  `AEGIS_EMBEDDING_DIMENSIONS=384` and start the server with `--embedding-dim 384`.
- **`none`** — skip embeddings entirely. Recall then uses the server's **keyword
  (BM25)** index, which still matches on content — including the exact
  identifiers embeddings are worst at (`--tenant-max-records`, `hnsw.c:214`) —
  plus tags and time. You lose *paraphrase* matching, not recall itself. Zero
  setup, nothing sent anywhere.

(A fourth mode, `fake`, is a deterministic hash used only by the test suite — not
for real use.)

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

**Check the wiring first, in one command:**

```bash
uvx --from aegisdb-mcp aegisdb-doctor
```

It walks every link between the project and its memory — which config file is in
force and the namespace it resolves to, whether the server answers, whether your
embedding dimension agrees with the server's, whether the provider is actually
usable, whether `.mcp.json` and both hooks are wired, whether the registered
memory server **completes an MCP handshake**, whether the recall hook **starts**,
whether the read path can run — and then proves the whole chain: it writes a
memory carrying a unique probe token and requires your *actual recall hook* to
hand that token back, before deleting it.

That last step is the one that matters. Wired, started and working are three
different things: a hook is contracted to exit 0 whatever happens, so a launcher
that starts but resolves nothing leaves every file on disk correct, every check
green, and no memory ever injected. The probe token is what tells them apart. Each failure
names the fix; it exits non-zero, so a pre-commit hook or CI step can run it, and
`--json` emits the same findings for a script. `--no-write` skips the round trip.

This exists because almost every way this integration breaks is **silent**. A
dimension that disagrees with the server refuses every embedded write. A hook
missing from `settings.json` recalls nothing and says nothing. `AEGIS_ASK_PATTERN`
without an extraction backend answers every question by ordinary search. None of
those raise, and each leaves the setup looking finished.

```text
✓ config      /repo/.aegisdb/config.json (discovered)
✓ namespace   my-project-6a9dbcb9
✓ server      127.0.0.1:9470 v0.8.4 phase 4
✗ dimension   client 384 ≠ server 1024 — every embedded write is refused
              → set embedding_dimensions to 1024, or restart the server with --embedding-dim 384
✓ mcp entry   `memory` registered in .mcp.json
✓ hooks       recall + capture wired in .claude/settings.json
```

**Then try it for real.** Start Claude Code in the project, then:

1. Run `/mcp` — the `memory` server should be listed `connected`, exposing
   `memory_save`, `memory_search`, `memory_get`, `memory_update`, `memory_relate`.
2. Ask: *"Remember that this project deploys with `make ship`."* → the agent calls
   `memory_save`.
3. Start a **new** session and ask: *"How do I deploy this project?"* → the recall
   hook injects the memory (or the agent calls `memory_search`) and answers from it.

If `/mcp` shows the server but tools error, AegisDB is unreachable — check it is
running on the configured host/port (`aegisdb-doctor` says which, and whether it
answers); the agent stays usable either way.

> Tools surface to the model as `mcp__memory__memory_save`, etc. The reference
> sections below cover every configuration option and the exact tool/hook contracts.

### 7. (Optional) Background summarization

Over months a namespace accumulates thousands of low-value episodic events. The
`aegisdb-summarize` job distils clusters of related, aging memories into a single
semantic fact, links it to its sources (`summarizes` edges), and archives the
sources — so recall stays small and cheap. It is **off by default** and runs on a
schedule you control (cron / systemd timer / the compose sidecar), **never** on
the per-turn hot path.

```sh
# preview what it would do — writes nothing
AEGIS_SUMMARY_MODE=claude-code uvx --from aegisdb-mcp aegisdb-summarize --dry-run

# run it (e.g. from a nightly cron)
AEGIS_SUMMARY_MODE=claude-code uvx --from aegisdb-mcp aegisdb-summarize
```

Pick a backend with `AEGIS_SUMMARY_MODE`:

- **`claude-code`** — distils via the `claude` CLI in headless mode, reusing your
  existing Claude Code auth. **No API key**, no extra install.
- **`anthropic`** — direct Anthropic API. `pip install "aegisdb-mcp[anthropic]"`,
  set `ANTHROPIC_API_KEY`.
- **`openai`** — any OpenAI-compatible chat API. `pip install "aegisdb-mcp[openai]"`,
  set `OPENAI_API_KEY` (and `AEGIS_SUMMARY_API_BASE` to point at a compatible
  endpoint). Use for environments without the Claude Code CLI.

A misconfigured backend (missing SDK or key) degrades to off rather than erroring.
Summaries are conservative and reversible: sources are tombstoned (recoverable
from the log until compaction), provenance is a graph edge, and `--dry-run` shows
the plan first.

Set `AEGIS_NAMESPACE` to the namespace your agents write under — the pass runs
against exactly one namespace, and the default (cwd-derived) name won't match
your clients'.

#### Scheduling it

The job is a one-shot: it runs a single pass and exits, so any scheduler works.
Three ready-made options:

- **Compose sidecar** — opt-in profile that loops the job on an interval:

  ```sh
  # in .env: AEGIS_SUMMARY_MODE=anthropic, ANTHROPIC_API_KEY=…, AEGIS_SUMMARY_NAMESPACE=…
  docker compose --profile summarize up -d --build
  # one-shot preview (writes nothing):
  docker compose run --rm --entrypoint aegisdb-summarize summarize --dry-run
  ```

  Backend keys and `AEGIS_SUMMARY_*` knobs are set in `.env`; the interval is
  `AEGIS_SUMMARY_INTERVAL` (default daily). The `claude-code` backend won't work
  here (no `claude` CLI in the image) — use `anthropic`/`openai`.

- **systemd timer** — [`examples/aegisdb-summarize.service`](examples/aegisdb-summarize.service)
  + [`examples/aegisdb-summarize.timer`](examples/aegisdb-summarize.timer). Install
  both, drop the config in an `EnvironmentFile`, then
  `systemctl enable --now aegisdb-summarize.timer`.

- **cron** — [`examples/summarize.crontab`](examples/summarize.crontab): a daily
  `/etc/cron.d` line that sources an env file and runs the job via `uvx`.

See [`docs/summarization-design.md`](https://github.com/d4n-larsson/aegisdb/blob/main/docs/summarization-design.md)
for the full design.

## Requirements

- A running **AegisDB** server, started with the embedding dimension you intend to
  use, e.g. `./build/aegisdb --embedding-dim 1024`.
- **Python 3.10+**.
- For *semantic* (paraphrase) recall: a `VOYAGE_API_KEY` (Voyage), the optional
  local model, or neither — with neither, recall falls back to the server's
  keyword index plus tags/time, which still matches on content.

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

Resolution precedence: defaults → JSON file → environment → explicit overrides.

### `.aegisdb/` — the project's own directory

Everything the integration reads from or writes into *your* project lives in
one place, created by `aegisdb-init`:

```text
.aegisdb/
  config.json      # settings both the MCP server and the hooks read — commit this
  predicates.json  # your predicate vocabulary — starter copy written for you
  facts/           # typed-fact corpora, loaded by `aegisdb-seed` — commit these
  local/           # machine state: gitignored wholesale, never committed
```

The split is the point. Everything beside `local/` is a reviewed input that
belongs in git — the namespace this project writes under, the vocabulary it may
assert in — and `local/` is machine state that does not. One gitignore line,
which `aegisdb-init` adds, and no per-file judgement calls.

**Why a file and not more env vars.** `config.json` is *discovered*, not
configured: `AEGIS_CONFIG` still names a file explicitly and always wins, but
with it unset the project's `.aegisdb/config.json` is read if present. That is
what makes one setting reach both callers. The MCP server gets its env from
`.mcp.json`; a Claude Code hook entry has **no env field at all**, which is why
`aegisdb-init` has to inline extraction settings onto the hook command — and why
a project with `embedding_mode: local` on the server would still recall
*without* embeddings in the hook, silently, because the hook fell back to the
built-in default. A file on disk is read by whoever runs next.

A path named by `AEGIS_CONFIG` and not found is an **error**, not a quiet fall back to the defaults: naming it is a claim that it is there, and the failure it would otherwise cause is the exact one this file exists to prevent — settings reverting to the built-ins with nothing said. Worth knowing while `.aegisdb/` is on one branch and `AEGIS_CONFIG` points into the working tree, since every other checkout then has no file at that path. A *discovered* `.aegisdb/config.json` that is simply absent stays normal and means "defaults".

Two things deliberately not in it: the **auth token**, because this file is
meant to be committed and a bearer token in git is a different class of mistake
than a wrong port; and the **server's data directory**, which stays wherever you
point `--data-dir`. `memory.log` is the plaintext of everything the agent was
ever told, and a project-root default invites exactly one `git add -f` too many.

**The namespace is now pinned.** With no namespace configured the fallback is
`basename(project) + hash(absolute path)` — stable until someone renames or
moves the directory, at which point every memory written under the old name is
still there and no longer reachable. `aegisdb-init` writes the value the path
*already* derives into `config.json`, so re-running it on an existing project
changes nothing and future you can move the directory freely. A namespaced auth
token is the exception: the server pins `agent_id` from the token, so the file
leaves the namespace blank rather than writing a value nothing reads.

### Typed facts: a vocabulary you start with, not one you invent

`aegisdb-init` writes `.aegisdb/predicates.json` containing the ten starter
predicates — `is_a`, `part_of`/`contains`, `depends_on` for structure;
`defaults_to`, `guarded_by`, `measured_by`, `owned_by` for the properties a
developer keeps re-asking about; `deprecated_by`/`recommended_by` declared
`mutex_with` each other. You are meant to edit it: it is your project's
contract, and ten is a deliberate ceiling, not a starting bid — a registry that
grows to fit every sentence has stopped being a contract.

It is **written once and never overwritten**. Re-running init keeps yours,
because predicates already asserted live in records that cannot be rewritten.

Two things this does *not* do, both because the server is a separate process:

- **Point the server at it.** The registry is read by the server, from disk, at
  startup: `--predicate-registry /path/to/.aegisdb/predicates.json`. If the
  server was started with a *different* file, that one decides what `insert`
  accepts, and this copy is decoration. Same file, both places.
- **Turn on inference.** `--inference` is separate, and without it the
  `transitive` / `inverse_of` / `mutex_with` declarations are validated and
  inert, and `subsume` reports `NOT_READY`.

Then drop corpora in `.aegisdb/facts/` and load them:

```bash
uvx --from aegisdb-mcp aegisdb-seed --dry-run   # report; write nothing
uvx --from aegisdb-mcp aegisdb-seed
```

It discovers everything: the registry from `.aegisdb/predicates.json`, every
`*.json` under `.aegisdb/facts/` in name order, and host/port/namespace from
`config.json` — so a corpus lands in the namespace the agent recalls from rather
than one somebody typed twice. Re-running is safe: an entity whose exact prose
already exists is reused and a fact already asserted is skipped.

A corpus is one JSON object. Entities are keyed by a label local to the file;
the label becomes a record id at load time, and the **prose is the identity** —
which is how two corpora written months apart land on one record instead of
minting a second:

```json
{
  "name": "my-service",
  "entities": { "api": "the public API", "db": "the datastore" },
  "facts": [
    ["api", "depends_on", "db", "The API reads and writes the datastore."],
    ["db",  "defaults_to", "postgres 15", "We run Postgres 15 in every environment."]
  ]
}
```

Each fact row is `[subject, predicate, object, prose]`. The object is another
**label** when the registry declares that predicate `object: "id"`, and a
**literal string** when it declares `object: "string"` — so a corpus only means
something against a registry, and `aegisdb-seed` refuses (naming the predicate)
rather than guessing. The prose becomes the record's `data`; the triple becomes
its `fact`.

| Env var | Default | Description |
|---------|---------|-------------|
| `AEGIS_HOST` | `127.0.0.1` | AegisDB host |
| `AEGIS_PORT` | `9470` | AegisDB TCP port |
| `AEGIS_CONNECT_TIMEOUT_MS` | `500` | connect timeout (degradation guard) |
| `AEGIS_READ_TIMEOUT_MS` | `1000` | per-request read timeout |
| `AEGIS_CONFIG` | `.aegisdb/config.json` in the project | explicit path to the JSON config file; overrides the discovered one. Naming a path is a claim that it is there, so a path that **does not exist is an error**, not a fall back to the defaults — worth knowing if you point it into the working tree while `.aegisdb/` is still on one branch, since every other checkout would otherwise reconfigure the integration silently. A *discovered* file that is simply absent stays normal |
| `AEGIS_NAMESPACE` | `.aegisdb/config.json`, else derived from project dir | isolation boundary (AegisDB `agent_id`); **ignored when the token is namespaced** — the token's namespace then governs |
| `AEGIS_AUTH_TOKEN` | _(none)_ | bearer token sent with every request; required when the server enforces auth. A namespaced token also defines the tenant |
| `AEGIS_EMBEDDING_MODE` | `voyage` if key present, else `none` | `voyage` \| `local` \| `none` \| `fake`. With `none`, recall is the **keyword** index alone: a question reaches a memory when they share a word, not when they mean the same thing. The client softens that where it can — the query it sends drops function words and carries light morphological variants, so *"how do I deploy this project?"* reaches *"widgetco deploys with make ship"* — but *"where does the deployment happen?"* still will not. Set a real provider if recall matters |
| `AEGIS_EMBEDDING_MODEL` | `voyage-3-large` | provider model id (Voyage mode) |
| `AEGIS_EMBEDDING_DIMENSIONS` | *(the server's)* | **must match the server's `--embedding-dim`** — which is why you should not normally set it by hand: `ping` reports the server's own, so `aegisdb-init` reads it from there and `aegisdb-doctor` checks the two still agree. A number typed from memory is a number that can be wrong for days, since a mismatch first surfaces as a refused write. Falls back to `1024` (`384` for `local`) only when the server cannot be reached or is too old to say |
| `AEGIS_RECALL_ENABLED` | `true` | toggle automatic recall |
| `AEGIS_RECALL_TIME_BUDGET_MS` | `800` | hard ceiling for recall |
| `AEGIS_RECALL_TOP_K` | `5` | max memories injected per turn |
| `AEGIS_RECALL_MIN_SCORE` | `0.2` | drop weak semantic matches |
| `AEGIS_RECALL_DEDUP_THRESHOLD` | `0.95` | drop a memory ≥ this cosine to a higher-ranked one, so near-duplicates don't waste tokens (semantic only; 0 or ≥1 disables) |
| `AEGIS_RECALL_MAX_CHARS_PER_MEMORY` | `500` | truncate each injected memory's text (0 = unlimited) |
| `AEGIS_RECALL_CHAR_BUDGET` | `2000` | total chars of injected memory text per turn; keeps the top-ranked slice, drops the rest (0 = unlimited) |
| `AEGIS_CAPTURE_ENABLED` | `true` | toggle automatic capture |
| `AEGIS_CAPTURE_SCOPE` | `session` | `session` (SessionEnd) \| `turn` (Stop) |
| `AEGIS_CAPTURE_MIN_SALIENCE` | `0.5` | below this, nothing is captured (heuristic path) |
| `AEGIS_EXTRACT_MODE` | `none` | LLM fact extraction for capture: `none` (off → heuristic markers) \| `fake` (tests) \| `claude-code` \| `anthropic` \| `openai`. When on, a session is distilled into durable facts stored as **semantic** memories (so they dedup/supersede and resist decay) instead of raw marker-matched sentences |
| `AEGIS_EXTRACT_MODEL` | — | optional model override for the extraction backend |
| `AEGIS_EXTRACT_API_BASE` | — | `openai` backend: base URL for an OpenAI-compatible endpoint |
| `AEGIS_EXTRACT_MAX_FACTS` | `12` | cap facts stored per session |
| `AEGIS_EXTRACT_MAX_INPUT_CHARS` | `24000` | cap transcript chars sent to the model (keeps the most recent) |
| `AEGIS_EXTRACT_TRIPLES` | `false` | propose typed `{s, p, o}` triples alongside prose facts (ROADMAP 5.4). Works with every extraction backend: `fake` reads explicit `SUBJECT : predicate : OBJECT` lines, while `claude-code`/`anthropic`/`openai` are prompted with the registry as a closed list and asked for JSON. Needs a vocabulary — the registry file, or the server's own, see the row below: the vocabulary is a contract, so proposing triples with nothing to check them against is not a smaller version of the feature. A predicate the registry does not declare is **dropped and counted, never coerced** onto the nearest one — the prose is still captured, so a rejection degrades to today's behaviour rather than losing anything |
| `AEGIS_EXTRACT_REGISTRY` | *(ask the server)* | **Optional.** Leave it unset and the vocabulary is read from the server over the wire (the `predicates` op) — the only option when the client is not on the same machine, and it removes the second copy of the registry file this used to need. A copy drifts, and the drift surfaces as the server refusing triples, which looks like a bad model rather than a misconfiguration. Set it to pin a specific file: a configured path **wins** over the server, because an operator who set it is relying on it. A path that is set but unreadable, or a malformed entry, is an **error** — not a fallback to the server, nor to accepting everything: the server refuses to *start* on a bad registry for the same reason, and silently degrading would be the opposite of what configuring a vocabulary asks for |
| `AEGIS_EXTRACT_MAX_TRIPLES` | `16` | cap candidates proposed per transcript |
| `AEGIS_GROUNDING_MIN_SCORE` | `0.85` | cosine floor for reusing an existing entity record rather than minting a new one (ROADMAP 5.4). **High on purpose.** Conflating two entities writes facts about the wrong thing and inference then compounds them undetectably; splitting one entity in two only loses inferences, and `consolidate` can merge them later. One error is recoverable and the other is not, so a near-miss mints. Deliberately *not* shared with `AEGIS_EXTRACT_SUPERSEDE_MIN_SCORE`: consolidation's errors are symmetric, so it can sit near the middle where this cannot |
| `AEGIS_GROUNDING_TOP_K` | `5` | entity candidates considered per mention |
| `AEGIS_EXTRACT_TRIPLE_CONFIDENCE` | `0.6` | confidence for a fact a model proposed, deliberately below what a human or a rule writes. Not decoration: ROADMAP 5.3 propagates confidence as a **product** along a derivation chain, so this number silently sets how much weight every conclusion drawn from parsed facts carries relative to one drawn from asserted facts |
| `AEGIS_GROUNDING_MAX_MINT` | `32` | new entity records per extraction. Covers `2 × AEGIS_EXTRACT_MAX_TRIPLES`, since a triple needs a subject and possibly an id-valued object — a smaller cap silently drops the overflow. Past the cap a mention is reported unresolved and its triple dropped — one lost fact, rather than a wrong resolution that would cost every conclusion drawn from it |
| `AEGIS_ASK_PATTERN` | `false` | let the model express a question as a `pattern` over typed facts (ROADMAP 5.4 §5), so *"what does the storage layer cap at?"* reaches a fact about a component of that layer. **Strictly an addition**: every way it can decline — no registry, the model can't express the question, the predicate isn't declared, the subject doesn't resolve, the lookup finds nothing — falls back to the search that runs today, so no question that is answerable now stops being. Grounding here **resolves but never mints**: a question about an unknown thing has no answer, and minting one would let reading the store write to it. **Needs `AEGIS_EXTRACT_MODE` set to a working backend** — expressing a question as a pattern is a model call, so with the default `none` this is on in the config and inert at runtime, and every answer comes back `"symbolic": false` as though the corpus had nothing. The server says which on startup: `read path: on via <mode>`, or a line naming what is missing. A vocabulary is also needed, but `AEGIS_EXTRACT_REGISTRY` is *not*: unset, it is read from the server |
| `AEGIS_ASK_VERBALIZE` | `false` | render a derived record's proof as one plain sentence, attached as `because` **beside** its `derivation`, never instead of it. The model reads the proof; it never produces it — the rendering can be checked against the payload, and if the two disagree the payload is right. Independent of `AEGIS_ASK_PATTERN`, since a derived record can surface through ordinary retrieval too — but it needs `AEGIS_EXTRACT_MODE` for the same reason: the sentence is rendered by a model |
| `AEGIS_ADJUDICATE_CONFLICTS` | `false` | hand a contradiction the server flagged and refused to settle to the model, and write the verdict as a **supersession** — never an edit (ROADMAP 5.4 §6). The inverse of `AEGIS_EXTRACT_SUPERSEDE`: the rules find the conflict deterministically at no cost, and the model sees only the one pair they could not decide, rather than every candidate fact. **"Neither" is first-class and is the default** — an unreachable backend, an unparseable reply and an unsure model all abstain, and an unresolved contradiction stays reported, which is the state the corpus was already in. Runs at the end of a capture — on **both** capture paths, and over whatever the corpus holds rather than what this session wrote. Needs the server started with `--inference` (nothing flags a contradiction otherwise) **and** a model backend: `AEGIS_EXTRACT_MODE` must not be `none`, since with no backend the provider abstains and the setting is legitimately inert |
| `AEGIS_ADJUDICATE_MAX_PER_RUN` | `8` | contradictions put to the model per capture. This is the one place in 5.4 where a model error becomes durable state, so the cap bounds a bad *run* and not just a bad call: a model answering badly does so for every pair, and an uncapped loop would work through the whole backlog before anyone saw it. `0` disables adjudication as surely as the flag above |
| `AEGIS_EXTRACT_SUPERSEDE` | `true` | when an extracted fact updates/contradicts an existing memory, replace it (tombstone + a `supersedes` provenance link) instead of accumulating both. Needs embeddings + an extractor backend; active only when `AEGIS_EXTRACT_MODE` is on |
| `AEGIS_EXTRACT_SUPERSEDE_TOP_K` | `5` | similar existing memories considered per new fact |
| `AEGIS_EXTRACT_SUPERSEDE_MIN_SCORE` | `0.6` | cosine floor for a supersession candidate |
| `AEGIS_SUMMARY_MODE` | `none` | `aegisdb-summarize` backend: `none` (off) \| `fake` (tests) \| `claude-code` \| `anthropic` \| `openai` |
| `AEGIS_SUMMARY_MODEL` | — | optional model override for the selected backend |
| `AEGIS_SUMMARY_API_BASE` | — | `openai` backend: base URL for an OpenAI-compatible endpoint |
| `AEGIS_SUMMARY_MIN_AGE_MS` | `604800000` | only distil memories older than this (7 days) |
| `AEGIS_SUMMARY_MAX_IMPORTANCE` | `0.6` | leave higher-importance memories alone |
| `AEGIS_SUMMARY_MIN_CLUSTER` | `3` | min related memories before a cluster is summarized |
| `AEGIS_SUMMARY_MAX_CLUSTER` | `20` | max memories folded into one summary |
| `AEGIS_SUMMARY_MAX_CLUSTERS_PER_RUN` | `20` | bound work/cost per run |
| `AEGIS_SUMMARY_MIN_CONFIDENCE` | `0.0` | skip a summary below this confidence |
| `AEGIS_SUMMARY_SCAN_TOP_K` | `1000` | candidate records pulled per run |

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

To keep the shared server stable, cap what any one tenant can consume:
`--tenant-max-records` / `--tenant-max-bytes` bound per-namespace storage and
`--tenant-rate-qps` bounds a namespace's request rate, so one member's runaway
agent can't fill the disk or monopolize the server (over-limit writes get
`QUOTA_EXCEEDED`, over-rate requests `RATE_LIMITED`). Admin `stats` reports each
tenant's live usage.

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


## What Claude sees about typed facts

When the server declares a predicate vocabulary — its own
`--predicate-registry`, or `AEGIS_EXTRACT_REGISTRY` here — the `memory_search`
tool description **names the predicates**:

> This store keeps typed facts, and a question that maps onto its vocabulary is
> answered from the fact graph rather than by text similarity. The declared
> predicates are: defaults_to, part_of. Phrasing a question in those terms —
> "what does X default to?", "what is part of Y?" — is what lets it be answered
> structurally; anything else still works and falls back to ordinary search.

The point is not that the model calls a different tool. It is that a question
can be answered from the fact graph only when it maps onto a declared
predicate, and the model is what chooses the phrasing — so without this it is
guessing at a contract the server enforces. The same gap the `predicates` op
closed for programs, closed for the model.

Three things worth knowing:

- **A server without a vocabulary pays nothing.** The description is then
  byte-for-byte what it has always been. This is not a feature to opt out of;
  it appears only when there is something to say.
- **It is fixed at startup.** MCP clients read the tool list once, when they
  connect, so changing the registry needs this server restarted before the
  model sees the change.
- **Long registries are summarised, not dumped.** At most 24 predicates are
  named and the rest counted. A tool description is prompt text on *every*
  request, and a registry of hundreds would quietly become the largest thing in
  the context.

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
  server.py       # MCP binding (lazy-imports `mcp`; supports SDK 1.x and 2.x)
  hooks.py        # console-script entry points (aegisdb-recall-hook / -capture-hook)
  seed.py         # aegisdb-seed: discover + load .aegisdb/facts/ corpora
  default_predicates.json  # starter vocabulary aegisdb-init copies into a project
hooks/
  recall_hook.py  # UserPromptSubmit (checkout path: python3 …/hooks/recall_hook.py)
  capture_hook.py # SessionEnd / Stop
tests/            # unit, contract, integration (stdlib unittest)
examples/         # mcp.json, settings.json
```

In a project that *uses* the integration, `aegisdb-init` writes `.mcp.json`,
`.claude/settings.json` (merged), and `.aegisdb/` — see
[`.aegisdb/`](#aegisdb--the-projects-own-directory). The first two are Claude
Code's paths and stay where it expects them; everything of ours is in the third.