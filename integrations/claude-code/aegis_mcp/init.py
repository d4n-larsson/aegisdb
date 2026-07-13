"""`aegisdb-init` — scaffold the Claude Code memory integration for a project.

Writes ``.mcp.json`` (the MCP server registration) and merges the recall +
capture hooks into ``.claude/settings.json``, so a user — or the ``/aegis-setup``
skill — can wire up AegisDB without hand-editing JSON.

Design goals:
- **Idempotent & non-destructive.** Re-running makes no duplicate hooks, and
  existing MCP servers / hooks / settings are preserved. The ``memory`` MCP entry
  is only overwritten with ``--force``.
- **Both driven and interactive.** The skill calls it with flags; a human running
  it in a terminal is prompted for anything missing (unless ``--yes``).
- **Testable core.** ``build_mcp_config`` and ``merge_hooks`` are pure functions
  over dicts; ``main`` is the only part that touches the filesystem / network.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

RECALL_CMD = "uvx --from aegisdb-mcp aegisdb-recall-hook"
CAPTURE_CMD = "uvx --from aegisdb-mcp aegisdb-capture-hook"
_HOOK_EVENTS = (("UserPromptSubmit", RECALL_CMD), ("SessionEnd", CAPTURE_CMD))


def build_mcp_config(*, host: str, port: int, namespace: str = "",
                     auth_token: str = "", embedding_mode: str = "none",
                     embedding_dim: int = 1024) -> dict:
    """Build the `memory` MCP server entry. Namespace is omitted when blank (an
    auth token's namespace is authoritative); the auth token / embedding env are
    only included when set, so nothing empty is written."""
    env = {"AEGIS_HOST": host, "AEGIS_PORT": str(port)}
    if namespace:
        env["AEGIS_NAMESPACE"] = namespace
    if auth_token:
        env["AEGIS_AUTH_TOKEN"] = auth_token
    if embedding_mode and embedding_mode != "none":
        env["AEGIS_EMBEDDING_MODE"] = embedding_mode
        env["AEGIS_EMBEDDING_DIMENSIONS"] = str(embedding_dim)
    return {"command": "uvx", "args": ["aegisdb-mcp"], "env": env}


def merge_mcp(existing: dict, entry: dict, *, force: bool) -> tuple[dict, str]:
    """Merge the `memory` server into an existing .mcp.json dict. Returns
    (new_dict, status): 'added' (new), 'updated' (replaced under --force),
    'unchanged' (already identical — a safe no-op), or 'conflict' (a different
    entry exists and --force was not given)."""
    out = json.loads(json.dumps(existing)) if existing else {}
    servers = out.setdefault("mcpServers", {})
    current = servers.get("memory")
    if current == entry:
        return out, "unchanged"
    if current is not None and not force:
        return out, "conflict"
    servers["memory"] = entry
    return out, "updated" if current is not None else "added"


def merge_hooks(settings: dict) -> tuple[dict, int]:
    """Merge the recall/capture hooks into a settings.json dict without touching
    unrelated hooks. Returns (new_dict, added_count); an already-present command
    is not duplicated."""
    out = json.loads(json.dumps(settings)) if settings else {}
    hooks = out.setdefault("hooks", {})
    added = 0
    for event, command in _HOOK_EVENTS:
        groups = hooks.setdefault(event, [])
        present = any(
            h.get("command") == command
            for g in groups for h in g.get("hooks", [])
        )
        if not present:
            groups.append({"hooks": [{"type": "command", "command": command}]})
            added += 1
    return out, added


def _load_json(path: str) -> dict:
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh) or {}
    except (json.JSONDecodeError, OSError) as exc:
        raise SystemExit(f"aegisdb-init: cannot parse {path}: {exc}")


def _write_json(path: str, obj: dict) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2)
        fh.write("\n")


def _prompt(label: str, default: str) -> str:
    try:
        got = input(f"{label} [{default}]: ").strip()
    except EOFError:
        return default
    return got or default


def _verify(host: str, port: int) -> str:
    """Best-effort connectivity check; never fatal."""
    try:
        from .client import AegisClient, AegisUnavailable
    except ImportError:  # pragma: no cover
        return "skipped (client unavailable)"
    try:
        resp = AegisClient(host, port).request({"operation": "ping"},
                                               read_timeout_ms=1500)
        return "ok" if resp.get("ok") else f"reachable but not ok: {resp}"
    except AegisUnavailable as exc:
        return f"unreachable ({exc}) — start the server, then re-run"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="aegisdb-init",
        description="Scaffold the AegisDB Claude Code memory integration: write "
                    ".mcp.json and merge the recall/capture hooks into "
                    ".claude/settings.json.")
    ap.add_argument("--dir", default=".", help="project directory (default: cwd)")
    ap.add_argument("--host", help="AegisDB host (default 127.0.0.1)")
    ap.add_argument("--port", type=int, help="AegisDB port (default 9470)")
    ap.add_argument("--namespace", default=None,
                    help="memory namespace; omit when using a namespaced auth token")
    ap.add_argument("--auth-token", default=None, help="bearer token if the server requires auth")
    ap.add_argument("--embedding-mode", choices=["none", "local", "voyage"],
                    default=None, help="embedding provider (default none)")
    ap.add_argument("--embedding-dim", type=int, default=None,
                    help="embedding dimension; must match the server's --embedding-dim")
    ap.add_argument("--yes", "-y", action="store_true",
                    help="non-interactive: take defaults for anything not given")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing `memory` MCP entry")
    ap.add_argument("--print", dest="dry", action="store_true",
                    help="print what would be written; change nothing")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the post-write connectivity check")
    args = ap.parse_args(argv)

    interactive = not args.yes and sys.stdin.isatty()

    def resolve(val, label, default):
        if val is not None:
            return val
        return _prompt(label, default) if interactive else default

    host = resolve(args.host, "AegisDB host", "127.0.0.1")
    port = int(resolve(None if args.port is None else str(args.port),
                       "AegisDB port", "9470"))
    embedding_mode = resolve(args.embedding_mode,
                             "Embedding mode (none/local/voyage)", "none")
    default_dim = "384" if embedding_mode == "local" else "1024"
    embedding_dim = int(resolve(None if args.embedding_dim is None else str(args.embedding_dim),
                                "Embedding dimension", default_dim))
    # Namespace/token: only prompt in interactive mode; blank is valid.
    namespace = args.namespace if args.namespace is not None else (
        _prompt("Namespace (blank = derive from a namespaced token)", "") if interactive else "")
    auth_token = args.auth_token if args.auth_token is not None else (
        _prompt("Auth token (blank = server has no auth)", "") if interactive else "")

    proj = os.path.abspath(args.dir)
    mcp_path = os.path.join(proj, ".mcp.json")
    settings_path = os.path.join(proj, ".claude", "settings.json")

    entry = build_mcp_config(host=host, port=port, namespace=namespace,
                             auth_token=auth_token, embedding_mode=embedding_mode,
                             embedding_dim=embedding_dim)
    mcp_doc, mcp_status = merge_mcp(_load_json(mcp_path), entry, force=args.force)
    settings_doc, hooks_added = merge_hooks(_load_json(settings_path))

    if args.dry:
        print("# .mcp.json\n" + json.dumps(mcp_doc, indent=2))
        print("\n# .claude/settings.json\n" + json.dumps(settings_doc, indent=2))
        return 0

    if mcp_status == "conflict":
        print(f"aegisdb-init: {mcp_path} already has a different `memory` MCP "
              f"server; re-run with --force to overwrite it. Nothing was changed.",
              file=sys.stderr)
        return 1

    _write_json(mcp_path, mcp_doc)
    _write_json(settings_path, settings_doc)

    print(f"✓ .mcp.json         ({mcp_status}) — memory server → {host}:{port}")
    print(f"✓ .claude/settings.json — {hooks_added} hook(s) added"
          f"{' (already present)' if hooks_added == 0 else ''}")
    if embedding_mode == "voyage":
        print("• voyage embeddings: ensure VOYAGE_API_KEY is set in your environment "
              "(not written to .mcp.json).")
    if not args.no_verify:
        print(f"• server ping: {_verify(host, port)}")
    print("\nDone. Restart Claude Code in this project to pick up the memory "
          "server and hooks.")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
