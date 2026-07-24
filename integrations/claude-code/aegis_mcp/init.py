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
import shlex
import sys

RECALL_CMD = "uvx --from aegisdb-mcp aegisdb-recall-hook"
CAPTURE_CMD = "uvx --from aegisdb-mcp aegisdb-capture-hook"


def _capture_command(capture_env: dict | None) -> str:
    """The SessionEnd capture command, with any config env vars prefixed onto it.
    The hooks don't inherit the MCP server's env, and Claude Code hook entries
    have no separate env field, so config that must reach the capture hook (e.g.
    AEGIS_EXTRACT_MODE) is set inline on the command — which the shell applies."""
    if not capture_env:
        return CAPTURE_CMD
    prefix = " ".join(f"{k}={shlex.quote(str(v))}" for k, v in capture_env.items())
    return f"{prefix} {CAPTURE_CMD}"


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


def merge_hooks(settings: dict, capture_env: dict | None = None) -> tuple[dict, int]:
    """Merge the recall/capture hooks into a settings.json dict without touching
    unrelated hooks. Returns (new_dict, changed_count). The capture hook may carry
    inline config env (see `capture_env`); an existing AegisDB hook is matched by
    its base command and updated in place if the env changed — so re-running with,
    say, extraction toggled updates it rather than adding a duplicate."""
    out = json.loads(json.dumps(settings)) if settings else {}
    hooks = out.setdefault("hooks", {})
    changed = 0
    plan = [("UserPromptSubmit", RECALL_CMD, RECALL_CMD),
            ("SessionEnd", CAPTURE_CMD, _capture_command(capture_env))]
    for event, base, command in plan:
        groups = hooks.setdefault(event, [])
        target = None
        for g in groups:
            for h in g.get("hooks", []):
                if base in (h.get("command") or ""):
                    target = h
                    break
            if target:
                break
        if target is None:
            groups.append({"hooks": [{"type": "command", "command": command}]})
            changed += 1
        elif target.get("command") != command:
            target["command"] = command  # env prefix changed (e.g. extraction on/off)
            changed += 1
    return out, changed


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
    ap.add_argument("--extract-mode",
                    choices=["none", "claude-code", "anthropic", "openai"],
                    default=None,
                    help="capture quality: 'none' (default) keeps heuristic markers; "
                         "the others distil sessions into durable facts with an LLM "
                         "(dedup + contradiction supersession) via the capture hook")
    ap.add_argument("--extract-model", default=None,
                    help="optional model id override for the extraction backend")
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
    extract_mode = resolve(
        args.extract_mode,
        "Capture: heuristic markers (none) or LLM extraction "
        "(claude-code/anthropic/openai)", "none")
    extract_model = args.extract_model if args.extract_model is not None else ""

    # Config that must reach the SessionEnd capture hook (which doesn't inherit
    # the MCP server's env) is prefixed onto its command; API keys are NOT written
    # here — like voyage, they come from the environment.
    capture_env = {}
    if extract_mode and extract_mode != "none":
        capture_env["AEGIS_EXTRACT_MODE"] = extract_mode
        if extract_model:
            capture_env["AEGIS_EXTRACT_MODEL"] = extract_model

    proj = os.path.abspath(args.dir)
    mcp_path = os.path.join(proj, ".mcp.json")
    settings_path = os.path.join(proj, ".claude", "settings.json")

    entry = build_mcp_config(host=host, port=port, namespace=namespace,
                             auth_token=auth_token, embedding_mode=embedding_mode,
                             embedding_dim=embedding_dim)
    mcp_doc, mcp_status = merge_mcp(_load_json(mcp_path), entry, force=args.force)
    settings_doc, hooks_added = merge_hooks(_load_json(settings_path), capture_env)

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
    print(f"✓ .claude/settings.json — {hooks_added} hook(s) added/updated"
          f"{' (already present)' if hooks_added == 0 else ''}")
    if embedding_mode == "voyage":
        print("• voyage embeddings: ensure VOYAGE_API_KEY is set in your environment "
              "(not written to .mcp.json).")
    if capture_env:
        print(f"• capture extraction: {extract_mode} — sessions are distilled into "
              "durable facts (dedup + contradiction supersession).")
        if extract_mode in ("anthropic", "openai"):
            key = "ANTHROPIC_API_KEY" if extract_mode == "anthropic" else "OPENAI_API_KEY"
            print(f"  ensure {key} is set in the environment the hook runs in "
                  "(not written to settings.json).")
    if not args.no_verify:
        print(f"• server ping: {_verify(host, port)}")
    print("\nDone. Restart Claude Code in this project to pick up the memory "
          "server and hooks.")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
