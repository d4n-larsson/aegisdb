"""`aegisdb-init` — scaffold the Claude Code memory integration for a project.

Writes ``.mcp.json`` (the MCP server registration) and ``.aegisdb/config.json``
(the settings both the server and the hooks read), and merges the recall +
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

from . import config as config_mod
from . import seed as seed_mod
from .config import derive_namespace, env_for_explicit_root, project_root

#: Words people type when they mean "use the default", each of which is also a
#: perfectly valid namespace. Warned about, never rewritten.
_PLACEHOLDER_NAMESPACES = frozenset(
    {"default", "derived", "derive", "auto", "none", "blank"})

#: How a scaffolded project reaches this package. Two forms, because one of
#: them can fail on a machine where the other works: `script` runs the console
#: shims `uvx`/`pip` installed, `module` runs the same code through `python -m`.
#: A launcher that cannot exec a shim takes the whole integration down —
#: silently, since a hook that never starts prints nothing and Claude Code says
#: nothing — while the module form on the same package keeps working. `auto`
#: proves one before writing it.
LAUNCHERS = {
    "script": {
        "recall": "uvx --from aegisdb-mcp aegisdb-recall-hook",
        "capture": "uvx --from aegisdb-mcp aegisdb-capture-hook",
        "mcp": ("uvx", ["aegisdb-mcp"]),
    },
    "module": {
        "recall": "uvx --from aegisdb-mcp python -m aegis_mcp.hooks recall",
        "capture": "uvx --from aegisdb-mcp python -m aegis_mcp.hooks capture",
        "mcp": ("uvx", ["--from", "aegisdb-mcp", "python", "-m",
                        "aegis_mcp.server"]),
    },
}

RECALL_CMD = LAUNCHERS["script"]["recall"]
CAPTURE_CMD = LAUNCHERS["script"]["capture"]


def _capture_command(capture_env: dict | None,
                     launcher: str = "script") -> str:
    """The SessionEnd capture command, with any config env vars prefixed onto it.
    The hooks don't inherit the MCP server's env, and Claude Code hook entries
    have no separate env field, so config that must reach the capture hook (e.g.
    AEGIS_EXTRACT_MODE) is set inline on the command — which the shell applies."""
    base = LAUNCHERS[launcher]["capture"]
    if not capture_env:
        return base
    prefix = " ".join(f"{k}={shlex.quote(str(v))}" for k, v in capture_env.items())
    return f"{prefix} {base}"


def build_mcp_config(*, host: str, port: int, namespace: str = "",
                     auth_token: str = "", embedding_mode: str = "none",
                     embedding_dim: int = 1024,
                     launcher: str = "script") -> dict:
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
    command, args = LAUNCHERS[launcher]["mcp"]
    return {"command": command, "args": list(args), "env": env}


def build_project_config(*, namespace: str, host: str, port: int,
                         embedding_mode: str = "none",
                         embedding_dim: int = 1024) -> dict:
    """The `.aegisdb/config.json` document.

    Deliberately a *subset* of `.mcp.json`'s env, not a copy of it. What goes
    here is what both callers need — and the hooks are the ones that could not
    get it before, since a Claude Code hook entry carries no env. The auth
    token is the one thing left out on purpose: this file is meant to be
    committed, and a bearer token in git is a different kind of mistake than a
    wrong port.
    """
    doc = {"namespace": namespace, "aegis_host": host, "aegis_port": port,
           "embedding_mode": embedding_mode or "none"}
    # Dimensions only mean something with a provider; omitting them when there
    # is none keeps a stale width from outliving the mode that chose it.
    if doc["embedding_mode"] != "none":
        doc["embedding_dimensions"] = embedding_dim
    return doc


# The keys init manages. Anything else in the file is the user's and is never
# touched — but a key we own and no longer emit has to be *removed*, not left
# behind: `--embedding-mode none` drops AEGIS_EMBEDDING_MODE from .mcp.json, and
# a stale `embedding_mode: local` here would then be the value the server reads,
# reviving the divergence this file exists to prevent, inverted.
OWNED_CONFIG_KEYS = ("namespace", "aegis_host", "aegis_port",
                     "embedding_mode", "embedding_dimensions")


def merge_project_config(existing: dict, entry: dict) -> tuple[dict, int]:
    """Merge our keys into an existing `.aegisdb/config.json`, preserving the
    rest. Returns (doc, changed-key-count) so an unchanged file is a no-op the
    caller can report as such — re-running init is expected, not exceptional."""
    out = dict(existing or {})
    changed = 0
    for k in OWNED_CONFIG_KEYS:
        if k in entry:
            if out.get(k) != entry[k]:
                changed += 1
            out[k] = entry[k]
        elif k in out:
            del out[k]
            changed += 1
    return out, changed


def ensure_predicates(proj: str) -> str:
    """Put the starter vocabulary in `.aegisdb/predicates.json` if there is none.

    A project should not have to invent a vocabulary before it can see typed
    facts work — that was the gap `predicates.example.json` was written to close,
    and copying it in is what makes it reachable from a `uvx` run with no clone.

    **Never overwritten.** Once the file exists it is the project's contract:
    predicates already asserted against it are in records that cannot be
    rewritten, so replacing it could invalidate facts already in the store.
    """
    path = os.path.join(proj, config_mod.PROJECT_DIR, "predicates.json")
    if os.path.isfile(path):
        return "kept (yours)"
    with open(seed_mod.DEFAULT_PREDICATES, encoding="utf-8") as fh:
        doc = json.load(fh)
    _write_json(path, doc)
    return f"written ({len(doc)} starter predicates)"


GITIGNORE_LINE = f"{config_mod.PROJECT_DIR}/{config_mod.LOCAL_SUBDIR}/"
GITIGNORE_NOTE = "# aegisdb: machine state (local server data, overrides)"


def _git_checkout_root(start: str) -> str | None:
    """The nearest ancestor containing `.git`, or None.

    `os.path.exists` rather than `isdir`: in a worktree or a submodule `.git` is
    a *file*, and walking up covers a project that is a subdirectory of its
    checkout — between them, exactly the setups where an un-ignored data
    directory would otherwise get committed.
    """
    cur = os.path.abspath(start)
    while True:
        if os.path.exists(os.path.join(cur, ".git")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def ensure_gitignore(proj: str) -> str:
    """Ignore `.aegisdb/local/`, and only that.

    The directory holds both reviewed inputs and machine state, so the boundary
    has to be a path rather than a convention people remember — everything
    beside `local/` is meant to be committed. Touches nothing outside a git
    checkout, and never rewrites an existing line.
    """
    if _git_checkout_root(proj) is None:
        return "skipped (not a git checkout)"
    path = os.path.join(proj, ".gitignore")
    body = ""
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as fh:
            body = fh.read()
        if any(ln.strip() == GITIGNORE_LINE for ln in body.splitlines()):
            return "already ignored"
    prefix = "" if (not body or body.endswith("\n")) else "\n"
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(f"{prefix}\n{GITIGNORE_NOTE}\n{GITIGNORE_LINE}\n")
    return "added"


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


#: Every wiring of a hook this project recognises. The first two are the forms
#: `aegisdb-init` writes (console script and `python -m`); the third is the
#: path-run script a checkout uses. Matching all three is what keeps a second
#: hook from being added beside a working one — two would recall twice into a
#: single turn and capture the same session twice.
#:
#: Public because `aegisdb-doctor` matches against the same set. Two lists
#: drift, and the drift is invisible in the worst way: the doctor reports a
#: correctly wired project as unwired, and its advice adds the duplicate.
HOOK_FORMS = {
    "recall": ("aegisdb-recall-hook", "aegis_mcp.hooks recall", "recall_hook.py"),
    "capture": ("aegisdb-capture-hook", "aegis_mcp.hooks capture",
                "capture_hook.py"),
}
#: The subset above that this scaffolder generates, and may therefore rewrite.
_OURS = {"recall": HOOK_FORMS["recall"][:2],
         "capture": HOOK_FORMS["capture"][:2]}


def _find_hook(groups, needles):
    for g in groups:
        for h in g.get("hooks", []):
            cmd = h.get("command") or ""
            if any(n in cmd for n in needles):
                return h
    return None


def merge_hooks(settings: dict, capture_env: dict | None = None,
                launcher: str = "script") -> tuple[dict, int]:
    """Merge the recall/capture hooks into a settings.json dict without touching
    unrelated hooks. Returns (new_dict, changed_count).

    Three outcomes per hook, and the third is the one that matters:

    - nothing there yet: add it;
    - one of *our* forms is there: update it in place, so re-running with the
      env or the launcher changed rewrites rather than duplicates;
    - a hook we recognise but did not write — the path-run script a checkout
      uses — is **left exactly as it is**. It works, it was chosen, and adding
      ours beside it would recall twice into every turn.
    """
    out = json.loads(json.dumps(settings)) if settings else {}
    hooks = out.setdefault("hooks", {})
    changed = 0
    plan = [("UserPromptSubmit", "recall", LAUNCHERS[launcher]["recall"]),
            ("SessionEnd", "capture", _capture_command(capture_env, launcher))]
    for event, kind, command in plan:
        groups = hooks.setdefault(event, [])
        target = _find_hook(groups, _OURS[kind])
        if target is None:
            if _find_hook(groups, HOOK_FORMS[kind]) is not None:
                continue  # somebody else's working wiring: leave it alone
            groups.append({"hooks": [{"type": "command", "command": command}]})
            changed += 1
        elif target.get("command") != command:
            target["command"] = command  # env prefix or launcher changed
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


#: What `--start-local` runs. A named container and a named volume, so a second
#: run adopts the first rather than racing it for the port, and so the data
#: survives `docker rm`. Kept beside the command the README and the setup skill
#: print, because three copies of a docker line drift into three behaviours.
LOCAL_IMAGE = "ghcr.io/d4n-larsson/aegisdb:latest"
LOCAL_CONTAINER = "aegisdb"
LOCAL_VOLUME = "aegis-data"


def local_server_command(port: int, dim: int) -> list[str]:
    """The `docker run` for a local server, as an argv (never a shell string)."""
    return ["docker", "run", "-d", "--name", LOCAL_CONTAINER,
            "-p", f"{port}:9470", "-v", f"{LOCAL_VOLUME}:/data",
            "--restart", "unless-stopped", LOCAL_IMAGE,
            "--data-dir", "/data", "--embedding-dim", str(dim)]


def _docker(*args, timeout=60):
    """Run docker, returning (rc, output). rc 127 = no docker on this machine."""
    import subprocess
    try:
        r = subprocess.run(["docker", *args], capture_output=True, text=True,
                           timeout=timeout)
    except FileNotFoundError:
        return 127, ""
    except (subprocess.TimeoutExpired, OSError) as exc:
        return 1, str(exc)
    return r.returncode, (r.stdout or r.stderr or "").strip()


def start_local_server(port: int, dim: int, wait_s: float = 30.0):
    """Bring up a local AegisDB in Docker. Returns (started, message).

    Adopts rather than duplicates: a container already named `aegisdb` is
    started if stopped and left alone if running. Creating a second one would
    fail on the port anyway, and that failure reads as "docker is broken"
    rather than "you already have one".

    Never raises, and never leaves the caller guessing: every path returns a
    sentence, because the fallback for all of them is the same — print the
    command and let a person run it.
    """
    adopted = False
    rc, out = _docker("inspect", "-f", "{{.State.Running}}", LOCAL_CONTAINER)
    if rc == 127:
        return False, ("docker is not on PATH — start a server yourself:\n  "
                       + " ".join(local_server_command(port, dim)))
    if rc == 0 and out == "true":
        message = f"a container named {LOCAL_CONTAINER} is already running"
        adopted = True
    elif rc == 0:
        rc2, out2 = _docker("start", LOCAL_CONTAINER)
        if rc2 != 0:
            return False, f"`docker start {LOCAL_CONTAINER}` failed: {out2}"
        message = f"started the existing {LOCAL_CONTAINER} container"
    else:
        rc2, out2 = _docker(*local_server_command(port, dim)[1:], timeout=300)
        if rc2 != 0:
            return False, f"`docker run` failed: {out2}"
        message = f"started {LOCAL_CONTAINER} on port {port} (dim {dim})"

    # Answering a ping is the only definition of "up" worth acting on: the
    # container can be running while the server is still opening its data dir.
    import time
    from .client import AegisClient, AegisUnavailable
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        try:
            if AegisClient("127.0.0.1", port).request(
                    {"operation": "ping"}, read_timeout_ms=1000).get("ok"):
                return True, message
        except AegisUnavailable:
            pass
        time.sleep(0.5)
    if adopted:
        # The likeliest reason by far, and a timeout alone points at the wrong
        # thing: the container we adopted is somebody's existing AegisDB, mapped
        # to the port *they* chose rather than the one asked for here.
        return False, (f"{message}, but nothing answers on port {port} — it is "
                       f"probably mapped to another one. Check `docker port "
                       f"{LOCAL_CONTAINER}` and re-run with that --port, or "
                       f"give the new server a different container name.")
    return False, (f"{message}, but it did not answer a ping within "
                   f"{wait_s:.0f}s — check `docker logs {LOCAL_CONTAINER}`")


def _local(host: str) -> bool:
    """Is this a server we could start? Only ever loopback: `--start-local`
    against a remote host would bind a container here and configure the project
    to talk to a machine that still has nothing running on it."""
    return host in ("127.0.0.1", "localhost", "::1", "0.0.0.0")


def _answers(host: str, port: int) -> bool:
    return _server_dimension(host, port) is not None or _pings(host, port)


def _pings(host: str, port: int) -> bool:
    """Reachability alone, for a server too old to report a dimension."""
    try:
        from .client import AegisClient, AegisUnavailable
    except ImportError:  # pragma: no cover
        return False
    try:
        return bool(AegisClient(host, port).request(
            {"operation": "ping"}, read_timeout_ms=1500).get("ok"))
    except AegisUnavailable:
        return False


def launcher_works(launcher: str, timeout_s: float = 45.0) -> bool:
    """Does a scaffolded project reach this package through `launcher`?

    Asks the MCP server command to complete one `initialize` handshake, because
    that is the strictest thing available without a server or a config: it
    proves the launcher execs, the package resolves, the SDK imports, and the
    process speaks the protocol Claude Code will speak to it.

    Worth doing at all because the failure it guards against is invisible. A
    console-script shim that a launcher cannot exec produces no output and no
    log; the hooks then do nothing in every session, the MCP tools never
    appear, and every file on disk looks right. Better to find that here, where
    a fallback is one line away, than to leave someone with a wired project and
    no memory.
    """
    import subprocess
    command, args = LAUNCHERS[launcher]["mcp"]
    hello = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "aegisdb-init", "version": "0"}}}) + "\n"
    try:
        r = subprocess.run([command, *args], input=hello, text=True,
                           capture_output=True, timeout=timeout_s)
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return False
    # A handshake, not just an exit code: the server stays up for stdio, so it
    # is killed by the pipe closing and its status says little either way.
    return '"result"' in (r.stdout or "") and '"serverInfo"' in (r.stdout or "")


def resolve_launcher(choice: str) -> tuple[str, str]:
    """(launcher, note). `auto` prefers the console scripts and proves them."""
    if choice in LAUNCHERS:
        return choice, ""
    if launcher_works("script"):
        return "script", ""
    if launcher_works("module"):
        return "module", (
            "the `uvx aegisdb-mcp` console script did not complete an MCP "
            "handshake here, so this project is wired through `python -m` "
            "instead — same package, same behaviour, one less shim. Force "
            "either form with --launcher script|module.")
    return "script", (
        "neither `uvx aegisdb-mcp` nor `uvx --from aegisdb-mcp python -m "
        "aegis_mcp.server` completed a handshake — writing the standard "
        "wiring anyway, but expect the memory tools and both hooks to do "
        "nothing until `uvx` works. Check with: uvx aegisdb-mcp < /dev/null")


def _server_dimension(host: str, port: int):
    """The server's `--embedding-dim`, or None if it cannot say.

    None covers three cases that need no distinguishing here — no server yet, an
    older build whose `ping` omits the field, or an unparseable answer — because
    all three mean the same thing to the caller: fall back to asking.
    """
    try:
        from .client import AegisClient, AegisUnavailable
    except ImportError:  # pragma: no cover
        return None
    try:
        resp = AegisClient(host, port).request({"operation": "ping"},
                                               read_timeout_ms=1500)
    except AegisUnavailable:
        return None
    dim = resp.get("embedding_dimensions")
    return dim if isinstance(dim, int) and dim > 0 else None


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
    ap.add_argument("--dir", default=None,
                    help="project directory. Default: CLAUDE_PROJECT_DIR if set "
                         "(so running this from a subdirectory still scaffolds "
                         "the project root), else the cwd. Naming one here "
                         "overrides the session's project entirely.")
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
    ap.add_argument("--launcher", choices=["auto", "script", "module"],
                    default="auto",
                    help="how the scaffolded project runs this package: the "
                         "console scripts (`script`), `python -m` (`module`), "
                         "or `auto` — try the first and fall back if it cannot "
                         "complete an MCP handshake")
    ap.add_argument("--start-local", action="store_true",
                    help="if nothing answers on the local port, start AegisDB "
                         "in Docker (named container `aegisdb`, volume "
                         "`aegis-data`) and wait for it")
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
    # Getting a server is the step this scaffolder used to leave on the floor:
    # it wrote a perfect client config, pinged, and said "unreachable — start
    # the server, then re-run". Offered here rather than after the write,
    # because a server that exists by the next line is one the dimension can be
    # read from.
    #
    # Only ever on an explicit flag or an explicit yes: starting a container is
    # a side effect on the machine, and `--yes` means "don't ask me about the
    # config", not "do things to my Docker daemon".
    if args.start_local and not _local(host):
        # Refused rather than obeyed: it would bind a container on *this*
        # machine and then configure the project to talk to another one, which
        # is two wrong things that look like one working setup.
        print(f"! --start-local ignored: {host} is not this machine",
              file=sys.stderr)
    elif _local(host) and not args.dry and not _answers(host, port):
        start = args.start_local or (
            interactive and _prompt(
                f"Nothing is listening on {host}:{port}. Start AegisDB in "
                f"Docker? (y/N)", "n").strip().lower().startswith("y"))
        if start:
            dim_for_start = args.embedding_dim or (
                384 if embedding_mode == "local" else 1024)
            ok, message = start_local_server(port, dim_for_start)
            print(("✓ " if ok else "! ") + message)

    # The dimension is the server's to state, not the user's to remember: every
    # vector has to match `--embedding-dim`, and a number typed here from memory
    # is a number that can be wrong for days — a mismatch does not surface until
    # the first embedded write is refused. Ask the server; fall back to the
    # per-mode guess only when it cannot answer (older build, or not up yet).
    server_dim = _server_dimension(host, port)
    default_dim = str(server_dim or (384 if embedding_mode == "local" else 1024))
    embedding_dim = int(resolve(None if args.embedding_dim is None else str(args.embedding_dim),
                                "Embedding dimension"
                                + (" (from the server)" if server_dim else ""),
                                default_dim))
    if server_dim and embedding_dim != server_dim:
        print(f"! embedding dimension {embedding_dim} does not match the "
              f"server's {server_dim} — every embedded write will be refused. "
              f"Writing it anyway; `aegisdb-doctor` will keep saying so.",
              file=sys.stderr)
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

    # An explicit --dir must beat CLAUDE_PROJECT_DIR, or init writes the
    # *session's* namespace into another project's config and pins it there.
    if args.dir is None:
        proj, env = project_root(), os.environ
    else:
        proj, env = os.path.abspath(args.dir), env_for_explicit_root()
    mcp_path = os.path.join(proj, ".mcp.json")
    settings_path = os.path.join(proj, ".claude", "settings.json")
    cfg_path = os.path.join(proj, config_mod.PROJECT_DIR, config_mod.CONFIG_BASENAME)

    # Pin the namespace rather than leaving it implied. Blank previously meant
    # "derive it from the path at read time", which is the same string until
    # someone renames or moves the directory and every memory written under the
    # old name becomes unreachable. Writing the value it *already* resolves to
    # is therefore a no-op for an existing project and a fix for a future one.
    #
    # "default" is a namespace, not a request for one. Someone answering a
    # prompt with the word almost always means "give me the default" — and gets
    # a real, shared namespace instead, which every other project answering the
    # same way also lands in. That is the isolation the derived name exists to
    # provide, lost silently. Said rather than corrected: a namespace is a
    # deliberate choice and quietly rewriting one would be its own surprise.
    if namespace and namespace.strip().lower() in _PLACEHOLDER_NAMESPACES:
        print(f"! namespace {namespace!r} is a literal name, not a request for "
              f"the default — every project that answers this way shares one "
              f"memory store. For this project's own, re-run without "
              f"--namespace (it derives {derive_namespace(env=env, cwd=proj)}).",
              file=sys.stderr)
    # A namespaced auth token is the exception: the server pins agent_id from
    # the token and ignores what the client asks for, so a namespace written
    # beside one would be a value nothing reads.
    pinned = namespace or ("" if auth_token else derive_namespace(env=env, cwd=proj))

    # Which launcher before what to write with it: `auto` proves the console
    # scripts can actually complete a handshake, and silently writing a wiring
    # that cannot run is the failure this whole step exists to prevent. Skipped
    # for --print, which must not spawn anything.
    launcher, launcher_note = (
        (args.launcher if args.launcher in LAUNCHERS else "script", "")
        if args.dry else resolve_launcher(args.launcher))

    entry = build_mcp_config(host=host, port=port, namespace=pinned,
                             auth_token=auth_token, embedding_mode=embedding_mode,
                             embedding_dim=embedding_dim, launcher=launcher)
    mcp_doc, mcp_status = merge_mcp(_load_json(mcp_path), entry, force=args.force)
    settings_doc, hooks_added = merge_hooks(_load_json(settings_path), capture_env,
                                            launcher)
    cfg_doc, cfg_changed = merge_project_config(
        _load_json(cfg_path),
        build_project_config(namespace=pinned, host=host, port=port,
                             embedding_mode=embedding_mode,
                             embedding_dim=embedding_dim))

    if args.dry:
        print("# .mcp.json\n" + json.dumps(mcp_doc, indent=2))
        print(f"\n# {config_mod.PROJECT_DIR}/{config_mod.CONFIG_BASENAME}\n"
              + json.dumps(cfg_doc, indent=2))
        print("\n# .claude/settings.json\n" + json.dumps(settings_doc, indent=2))
        print(f"\n# .gitignore\n{GITIGNORE_NOTE}\n{GITIGNORE_LINE}")
        return 0

    if mcp_status == "conflict":
        print(f"aegisdb-init: {mcp_path} already has a different `memory` MCP "
              f"server; re-run with --force to overwrite it. Nothing was changed.",
              file=sys.stderr)
        return 1

    _write_json(mcp_path, mcp_doc)
    _write_json(cfg_path, cfg_doc)
    _write_json(settings_path, settings_doc)
    pred_status = ensure_predicates(proj)
    os.makedirs(os.path.join(proj, config_mod.PROJECT_DIR, "facts"), exist_ok=True)
    ignore_status = ensure_gitignore(proj)

    print(f"✓ .mcp.json         ({mcp_status}) — memory server → {host}:{port}")
    print(f"✓ {config_mod.PROJECT_DIR}/{config_mod.CONFIG_BASENAME}  "
          f"({'unchanged' if cfg_changed == 0 else 'written'})"
          + (f" — namespace `{pinned}`" if pinned else
             " — namespace comes from the auth token"))
    print(f"✓ {config_mod.PROJECT_DIR}/predicates.json  ({pred_status})")
    print(f"✓ .claude/settings.json — {hooks_added} hook(s) added/updated"
          f"{' (already present)' if hooks_added == 0 else ''}")
    print(f"• .gitignore: {config_mod.PROJECT_DIR}/{config_mod.LOCAL_SUBDIR}/ "
          f"({ignore_status}) — commit the rest of {config_mod.PROJECT_DIR}/")
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
    print(f"• typed facts: start the server with --predicate-registry "
          f"<path>/{config_mod.PROJECT_DIR}/predicates.json (it reads the file, "
          f"not this config), then drop corpora in "
          f"{config_mod.PROJECT_DIR}/facts/ and run `aegisdb-seed`.")
    if launcher_note:
        print(f"! {launcher_note}", file=sys.stderr)
    elif launcher != "script":
        print(f"• launcher: {launcher} (forced with --launcher)")
    if not args.no_verify:
        print(f"• server ping: {_verify(host, port)}")
    print("\nDone. Restart Claude Code in this project to pick up the memory "
          "server and hooks.")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
