"""`aegisdb-doctor` — check every link between this project and its memory.

    aegisdb-doctor            # human-readable report; exit 1 if anything failed
    aegisdb-doctor --json     # the same findings as JSON, for CI
    aegisdb-doctor --no-write # skip the round trip (read-only checks only)

Why this exists, and why it is not a connectivity check with extra steps: the
ways this integration breaks are almost all *silent*. A dimension that disagrees
with the server refuses every embedded write. A hook that is not in
`settings.json` recalls nothing and says nothing. `ask_pattern` with no
extraction backend answers every question by ordinary search and reports
`"symbolic": false`, which reads exactly like a corpus with no answer. None of
those raise, none of them appear in a transcript, and each one leaves the setup
looking finished.

So every check here has to end in a sentence a person can act on: what is wrong,
and the flag or file that fixes it. A check that can only say "something is off"
is not worth the line it prints.

Ordered from the outside in — config, server, embeddings, wiring, then an actual
write — because a failure early makes the later ones meaningless, and saying so
is more useful than a screen of red.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

from . import config as config_mod

OK, WARN, FAIL, SKIP = "ok", "warn", "fail", "skip"

_MARK = {OK: "✓", WARN: "!", FAIL: "✗", SKIP: "-"}


class Report:
    """The findings, in the order they were made."""

    def __init__(self):
        self.checks: list[dict] = []

    def add(self, name: str, status: str, detail: str, fix: str = "") -> str:
        self.checks.append({"check": name, "status": status, "detail": detail,
                            **({"fix": fix} if fix else {})})
        return status

    @property
    def failed(self) -> bool:
        return any(c["status"] == FAIL for c in self.checks)

    @property
    def warned(self) -> bool:
        return any(c["status"] == WARN for c in self.checks)

    def render(self) -> str:
        width = max((len(c["check"]) for c in self.checks), default=0)
        lines = []
        for c in self.checks:
            lines.append(f"{_MARK[c['status']]} {c['check']:<{width}}  "
                         f"{c['detail']}")
            if c.get("fix"):
                lines.append(f"  {'':<{width}}  → {c['fix']}")
        return "\n".join(lines)


def _load_json(path: str):
    """The file as a dict, `None` when it is absent, `False` when unparseable.

    Three outcomes rather than two: "you have no `.mcp.json`" and "your
    `.mcp.json` is broken JSON" need different fixes, and collapsing them would
    send someone to write a file they already have.
    """
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh) or {}
    except (json.JSONDecodeError, OSError):
        return False


def check_config(rep: Report, env, cwd):
    """Which settings are in force, and where they came from."""
    path = config_mod.config_path(env, cwd)
    named = bool(env.get("AEGIS_CONFIG"))
    try:
        cfg = config_mod.load_config(env=env, cwd=cwd)
    except config_mod.ConfigError as exc:
        rep.add("config", FAIL, str(exc),
                "fix the path or the file; every setting is at its built-in "
                "default until this reads")
        return None
    if os.path.isfile(path):
        rep.add("config", OK, f"{path}"
                + (" (named by AEGIS_CONFIG)" if named else " (discovered)"))
    else:
        rep.add("config", WARN,
                f"no {config_mod.PROJECT_DIR}/{config_mod.CONFIG_BASENAME} — "
                f"built-in defaults, and the hooks cannot read your settings",
                "run `aegisdb-init` from the project root")
    rep.add("namespace", OK, cfg.namespace)
    return cfg


def check_server(rep: Report, cfg):
    """Reachability, and what the server says about itself."""
    from .client import AegisClient, AegisUnavailable
    client = AegisClient.from_config(cfg)
    try:
        resp = client.ping()
    except AegisUnavailable as exc:
        rep.add("server", FAIL, f"unreachable at {cfg.aegis_host}:"
                f"{cfg.aegis_port} ({exc})",
                "start it, or correct aegis_host/aegis_port")
        return client, None
    if not resp.get("ok"):
        rep.add("server", FAIL, f"answered, but not ok: {resp}")
        return client, None
    rep.add("server", OK, f"{cfg.aegis_host}:{cfg.aegis_port} "
            f"v{resp.get('version')} phase {resp.get('phase')}")
    return client, resp


def check_dimension(rep: Report, cfg, ping):
    """The number both sides must agree on, from the side that decides it."""
    if cfg.embedding_mode == "none":
        rep.add("dimension", SKIP, "embeddings are off")
        return
    if ping is None:
        # Distinct from "this server does not report one": blaming the server's
        # age for an outage sends someone to upgrade a server that is simply
        # not running, and the line above already said it is down.
        rep.add("dimension", SKIP, "the server is unreachable")
        return
    server_dim = ping.get("embedding_dimensions")
    if not isinstance(server_dim, int) or server_dim <= 0:
        rep.add("dimension", WARN,
                f"client is {cfg.embedding_dimensions}; this server is too "
                f"old to report its own, so the agreement cannot be checked",
                "upgrade the server, or match its --embedding-dim by hand")
        return
    if server_dim != cfg.embedding_dimensions:
        rep.add("dimension", FAIL,
                f"client {cfg.embedding_dimensions} ≠ server {server_dim} "
                f"— every embedded write is refused",
                f"set embedding_dimensions to {server_dim}, or restart the "
                f"server with --embedding-dim {cfg.embedding_dimensions}")
        return
    rep.add("dimension", OK, f"{server_dim}, agreed with the server")


def check_embeddings(rep: Report, cfg):
    """The provider itself: configured is not the same as usable."""
    if cfg.embedding_mode == "none":
        rep.add("embeddings", WARN,
                "mode is `none` — recall is tags and time only, no meaning",
                "set embedding_mode to `voyage` or `local` for semantic recall")
        return
    from .embeddings import make_provider
    provider = make_provider(cfg)
    if not provider.available():
        # Two independent reasons per provider — a missing package and a
        # missing key — and naming only one sends people to check what is
        # already fine.
        hint = {"voyage": "pip install 'aegisdb-mcp[voyage]', and set "
                          "VOYAGE_API_KEY in the environment the MCP server "
                          "and the hooks run in",
                "local": "pip install 'aegisdb-mcp[local]'"}.get(
                    cfg.embedding_mode, "")
        rep.add("embeddings", FAIL,
                f"mode `{cfg.embedding_mode}` is configured but the provider "
                f"is unavailable — recall silently falls back to no vectors",
                hint)
        return
    rep.add("embeddings", OK, f"{cfg.embedding_mode} provider available")


def check_registration(rep: Report, root):
    """The MCP entry Claude Code launches the tools from."""
    path = os.path.join(root, ".mcp.json")
    doc = _load_json(path)
    if doc is False:
        rep.add("mcp entry", FAIL, f"{path} is not valid JSON")
        return
    if doc is None:
        rep.add("mcp entry", FAIL, f"no {path} — the memory tools are not "
                f"registered", "run `aegisdb-init`")
        return
    servers = doc.get("mcpServers") or {}
    if "memory" not in servers:
        rep.add("mcp entry", FAIL,
                f"{path} has no `memory` server (has: "
                f"{', '.join(sorted(servers)) or 'nothing'})",
                "run `aegisdb-init`")
        return
    rep.add("mcp entry", OK, "`memory` registered in .mcp.json")


def check_hooks(rep: Report, root):
    """Recall and capture. A missing hook is the quietest failure here: nothing
    is injected, nothing is stored, and nothing anywhere says so."""
    path = os.path.join(root, ".claude", "settings.json")
    doc = _load_json(path)
    if doc is False:
        rep.add("hooks", FAIL, f"{path} is not valid JSON")
        return
    hooks = (doc or {}).get("hooks") or {}
    missing = []
    for event, needle, label in (("UserPromptSubmit", "aegisdb-recall-hook",
                                  "recall"),
                                 ("SessionEnd", "aegisdb-capture-hook",
                                  "capture")):
        found = any(needle in (h.get("command") or "")
                    for group in hooks.get(event, [])
                    for h in group.get("hooks", []))
        if not found:
            missing.append(f"{label} ({event})")
    if missing:
        rep.add("hooks", FAIL, "not wired: " + ", ".join(missing),
                "run `aegisdb-init`, then restart Claude Code")
        return
    rep.add("hooks", OK, "recall + capture wired in .claude/settings.json")


def check_read_path(rep: Report, cfg, tools, online: bool):
    """The prerequisite that is not implied by the setting that turns it on.

    Both halves are resolved exactly as the MCP server resolves them — the same
    vocabulary lookup and the same note — so this cannot pass while the server
    it is reporting on sits inert.
    """
    if not (cfg.ask_pattern or cfg.ask_verbalize):
        rep.add("read path", SKIP, "ask_pattern / ask_verbalize are off")
        return
    from .extract import (VocabularyError, make_extraction_provider,
                          resolve_vocabulary)
    from .server import read_path_note
    provider = make_extraction_provider(cfg)
    extractor = provider if provider.available() else None
    if extractor is None:
        # The backend half needs no server, and it is the half that is usually
        # wrong. `read_path_note` reports it before it looks at any vocabulary.
        rep.add("read path", FAIL, read_path_note(cfg, None, None).split(" — ", 1)[0],
                "every question falls back to ordinary search until this is "
                "fixed, answering \"symbolic\": false")
        return
    if not online:
        # With no registry configured the vocabulary comes from the server, so
        # there is nothing to say about it while the server is down — and
        # "no vocabulary" would read as a second, separate fault.
        rep.add("read path", WARN,
                f"backend `{cfg.extract_mode}` is usable; the vocabulary is "
                f"unchecked while the server is unreachable")
        return
    try:
        vocab = resolve_vocabulary(cfg, tools)
    except VocabularyError as exc:
        rep.add("read path", FAIL, str(exc),
                "fix extract_registry, or unset it to read the vocabulary from "
                "the server")
        return
    note = read_path_note(cfg, vocab, extractor)
    if note:
        rep.add("read path", FAIL, note.split(" — ", 1)[0],
                "every question falls back to ordinary search until this is "
                "fixed, answering \"symbolic\": false")
        return
    rep.add("read path", OK,
            f"on via {cfg.extract_mode}, {len(vocab or [])} predicate(s)")


def check_round_trip(rep: Report, tools, enabled: bool):
    """Save, find it, remove it. The only check that proves the whole chain.

    Everything above can pass on a server this project cannot actually write to
    — a namespaced token pointing elsewhere, a read-only token, a full quota.
    """
    if not enabled:
        rep.add("round trip", SKIP, "--no-write")
        return
    marker = "aegisdb-doctor round trip; safe to delete"
    saved = tools.save(marker, tags=["aegisdb-doctor"], importance=0.0)
    if not saved.get("ok"):
        rep.add("round trip", FAIL, f"save refused: {saved.get('message') or saved}",
                "check the auth token and the server's quotas")
        return
    mem_id = saved["id"]
    found = tools.search(tags=["aegisdb-doctor"], top_k=5)
    hit = any(m.get("id") == mem_id for m in found.get("memories", []))
    removed = tools.delete(mem_id).get("ok")
    if not hit:
        rep.add("round trip", FAIL,
                f"saved record {mem_id} but a tag search did not find it",
                "the write and the read are not seeing the same namespace")
        return
    if not removed:
        rep.add("round trip", WARN,
                f"round trip worked, but test record {mem_id} could not be "
                f"deleted", f"remove it by hand: it is tagged `aegisdb-doctor`")
        return
    rep.add("round trip", OK, f"saved, found and removed record {mem_id}")


def run(env=None, cwd=None, write=True) -> Report:
    """Every check, outside in. Returns the report; never raises."""
    env = os.environ if env is None else env
    root = config_mod.project_root(env, cwd)
    rep = Report()
    cfg = check_config(rep, env, cwd)
    if cfg is None:
        return rep  # every later check would be about the wrong settings
    client, ping = check_server(rep, cfg)
    from .embeddings import make_provider
    from .tools import MemoryTools
    tools = MemoryTools(cfg, client, make_provider(cfg))
    check_dimension(rep, cfg, ping)
    check_embeddings(rep, cfg)
    check_registration(rep, root)
    check_hooks(rep, root)
    check_read_path(rep, cfg, tools, ping is not None)
    if ping is None:
        # Not "--no-write": the server is down, which the report already says
        # once. Repeating it as a second failure would double-count one fault.
        rep.add("round trip", SKIP, "the server is unreachable")
    else:
        check_round_trip(rep, tools, write)
    return rep


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="aegisdb-doctor",
        description="Check every link between this project and its AegisDB "
                    "memory, and say what to fix. Exits non-zero if a check "
                    "failed.")
    ap.add_argument("--json", action="store_true",
                    help="emit the findings as JSON instead of a report")
    ap.add_argument("--no-write", action="store_true",
                    help="skip the save/search/delete round trip")
    ap.add_argument("--strict", action="store_true",
                    help="treat warnings as failures")
    ap.add_argument("--dir", default=None,
                    help="project directory (default: this one)")
    args = ap.parse_args(argv)

    if args.dir is None:
        env, cwd = os.environ, None
    else:
        # An explicit directory has to mean that directory, or running this
        # inside a Claude Code session reports on the *session's* project.
        env, cwd = config_mod.env_for_explicit_root(), os.path.abspath(args.dir)

    rep = run(env=env, cwd=cwd, write=not args.no_write)
    if args.json:
        json.dump({"ok": not rep.failed, "checks": rep.checks}, sys.stdout,
                  indent=2)
        sys.stdout.write("\n")
    else:
        print(rep.render())
        if rep.failed:
            print("\nSomething above is broken. Memory is best-effort, so "
                  "Claude Code still works — it just will not remember.")
        elif rep.warned:
            print("\nWorking, with the caveats above.")
        else:
            print("\nAll good.")
    return 1 if (rep.failed or (args.strict and rep.warned)) else 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
