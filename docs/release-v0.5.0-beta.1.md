# AegisDB v0.5.0-beta.1 — first public beta 🛡️

> **Self-hosted memory for your AI agents.** One small C binary — multi-tenant,
> encrypted, with backups, read replicas, and a one-command Prometheus + Grafana
> stack. Your agents' memory stays on your box; nothing ships to a SaaS.

AI agents forget everything between sessions. AegisDB gives them durable,
searchable long-term memory — episodic history, semantic facts with vector
search, and volatile working memory — behind a dead-simple JSON-over-TCP
protocol, with a first-class Claude Code integration. It's a single
dependency-free binary you run yourself.

**This is a beta.** The core is solid and well-tested, but it hasn't yet run in
anger across many real deployments — so we're shipping it to early adopters to
earn that mileage. See [What "beta" means here](#what-beta-means-here) before you
bet your production data on it.

## Try it in 30 seconds

```bash
docker run -d --name aegisdb -p 9470:9470 -v aegis-data:/data \
  ghcr.io/d4n-larsson/aegisdb:latest

docker exec aegisdb aegisdb client put --type semantic --tags user "prefers dark mode"
docker exec aegisdb aegisdb client search --tags user --top-k 5
```

Give **Claude Code** a persistent memory (needs a running server):

```bash
uvx --from aegisdb-mcp aegisdb-init --yes --host 127.0.0.1 --port 9470
```

## What's in the box

- **Three memory types** — episodic (immutable history), semantic (updatable
  facts with HNSW + exact vector search), and TTL'd working memory.
- **Multi-tenant** — namespaced, scoped, hashed auth tokens; per-tenant storage
  quotas and rate limits.
- **Private by default** — XChaCha20-Poly1305 encryption at rest; your data
  never leaves your box.
- **Durable & operable** — append-only log with CRC/AEAD framing, checkpoints +
  compaction, online snapshot/restore, health check.
- **Read replicas** — encrypted log shipping to read-only followers.
- **Observability** — Prometheus metrics + a pre-built Grafana dashboard in one
  command (`docker compose --profile monitoring up`).
- **Data rights** — per-subject export and purge (right-to-be-forgotten),
  temporal `as_of` queries, decay-based forgetting, and near-duplicate
  consolidation.
- **Claude-native** — MCP server + recall/capture hooks (`aegisdb-mcp` on PyPI),
  plus a browser-based memory inspector.

## What "beta" means here

We'd rather tell you the edges than have you find them:

- **Single-node.** You scale up, not out — no cross-machine sharding yet.
- **Replication is read-replica only.** Failover is manual; this is not (yet) an
  HA cluster.
- **Formats aren't frozen.** The wire protocol and on-disk format are versioned
  but may still change before 1.0. We'll document any migration; don't assume
  seamless upgrades yet.
- **Young in the wild.** Extensively tested (unit + wire-contract suites,
  ASan/UBSan, ThreadSanitizer, fuzzing in CI) but not yet battle-hardened by
  real-world load. The "agents remember better with it" claim is backed by an
  [A/B benchmark](https://github.com/d4n-larsson/aegisdb/blob/main/eval/README.md#recorded-result),
  not yet by production usage.

Good fit today: self-hosters, homelabs, and teams giving their agents memory who
can run a single node and tolerate a pre-1.0 contract. Not yet a drop-in for
mission-critical, multi-region, zero-downtime workloads.

## Help us get to 1.0

The fastest path to a stable 1.0 is **you running it and telling us what breaks**:

- ⭐ Star the repo if the idea resonates.
- 🐛 File issues — crashes, rough edges, docs gaps, missing ops knobs.
- 💬 Tell us your use case and scale so we can prioritize the road to GA
  (HA/failover, format-stability guarantees, load-tested limits).

## Links

- **Repo:** https://github.com/d4n-larsson/aegisdb
- **Quickstart & docs:** [README](https://github.com/d4n-larsson/aegisdb#readme) ·
  [wire protocol](https://github.com/d4n-larsson/aegisdb/blob/main/docs/wire-protocol.md) ·
  [team-server tutorial](https://github.com/d4n-larsson/aegisdb/blob/main/docs/tutorial-team-server.md)
- **Container:** `ghcr.io/d4n-larsson/aegisdb:latest`
- **Claude Code integration:** `aegisdb-mcp` on PyPI

*MIT licensed. Built in C17, no runtime dependencies.*