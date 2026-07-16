# Launch checklist

A runbook for taking AegisDB public. Audience: **self-hosters / teams**; goal:
**awareness (GitHub stars, first users)**. Work top to bottom — don't post
before the pre-launch gates are green.

## Positioning (keep this consistent everywhere)

> **Self-hosted memory for your AI agents.** One small C binary — multi-tenant,
> encrypted, with backups, replicas, and a one-command Prometheus + Grafana
> stack. Your agents' memory stays on your box; nothing ships to a SaaS.

The wedge is **self-hosted + private + single binary + Claude-native**, vs. the
Python/SaaS memory services. Lead with "own your data," not a feature list.

## Pre-launch gates (all must be green before posting)

- [ ] Grafana dashboard shipped (`docker compose --profile monitoring up` works on a fresh clone).
- [ ] README landing merged (hero + quickstart + "why self-host").
- [ ] **Dashboard screenshot** captured and committed to `docs/img/dashboard.png`, and the `<img>` slot in `README.md` uncommented.
- [ ] `docker run … ghcr.io/d4n-larsson/aegisdb` verified from a clean machine — the quickstart must work first try.
- [ ] Repo **About** description + **Website** (Pages URL) + **Topics** set (see below).
- [ ] Repo **Social preview** image set to the dashboard screenshot (Settings → Social preview).
- [ ] Latest CI green on `main`.

## Assets

- [ ] Grafana dashboard screenshot (dark theme, wide viewport, panels populated with some traffic).
- [ ] Optional: a ~60s screen recording / GIF of Claude Code recalling something across sessions.
- [ ] Post drafts ready (Show HN, r/selfhosted, X thread) — see notes below.

## GitHub repo metadata

- **About:** `Self-hosted memory for AI agents — a single C binary. Episodic + semantic (vector search) + working memory, multi-tenant, encrypted. MCP-native for Claude Code.`
- **Website:** the docs site (Pages URL).
- **Topics:** `ai-agents` `agent-memory` `mcp` `model-context-protocol` `claude`
  `claude-code` `llm` `vector-database` `vector-search` `hnsw` `semantic-search`
  `embeddings` `self-hosted` `memory` `database` `retrieval` `c`

## Launch sequence (staggered — do NOT fire all at once)

1. [ ] **r/selfhosted** — friendliest audience; the dashboard image lands here.
       Post as "I built this," lead with the screenshot, include a self-host
       checklist (ports, volume, auth, backups). Fix whatever the comments surface.
2. [ ] **Show HN** — a few days later, after the quickstart/README are polished
       from round 1. You get one shot. Post ~7–10am ET on a weekday; be in the
       thread for the first 2–3 hours, concede real limitations, don't astroturf.
3. [ ] **X/Twitter thread** — same day as Show HN; link the HN post in a reply.
4. [ ] **r/LocalLLaMA** (optional) — reframe around local/privacy, any-agent;
       lead with the memory model rather than Claude.

## MCP directory listings (long-tail discovery; low effort)

List the **MCP server** (`aegisdb-mcp`) with AegisDB as the self-hosted backend —
make the backend dependency explicit (it's not a hosted endpoint).

Reusable one-liner: *Persistent long-term memory for Claude Code and any MCP
client, backed by a self-hosted AegisDB server — your data stays on your box.*
Tools: `memory_save` / `memory_search` / `memory_get` / `memory_update` /
`memory_relate`.

- [ ] `punkpeye/awesome-mcp-servers` (PR; tag 🐍 🏠, under Knowledge & Memory)
- [ ] `modelcontextprotocol/servers` (community-list PR)
- [ ] Smithery (smithery.ai) — list as local/self-hosted
- [ ] Glama (glama.ai/mcp/servers) — ensure `mcp` topics so it's auto-classified; claim the listing
- [ ] PulseMCP (pulsemcp.com/submit)
- [ ] mcp.so

## FAQ to have ready for comments

- **vs. mem0 / Zep / Letta?** Self-hosted single binary, no Python/DB stack;
  multi-tenant + encryption + ops built in; MCP-native. Infra, not a framework.
- **vs. pgvector / SQLite + a vector ext?** Those are storage; this is a memory
  *service* with the agent model (episodic/semantic/working), namespace
  isolation, recall hooks, and MCP wiring already done.
- **Production-ready?** Be honest: young, single-writer, main user is the author;
  but CI runs ASan/UBSan/TSan + fuzzing and there's a security-review pass.

## Post-launch

- [ ] Respond to every early comment/issue within the first hours.
- [ ] Triage feedback into issues; ship an obvious quick win the same week.
- [ ] Add a `good first issue` label + a couple of starter issues for drive-by contributors.
- [ ] Note what channel drove traffic (repo insights) to inform the next round.