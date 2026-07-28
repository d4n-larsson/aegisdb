#!/usr/bin/env python3
"""First-contact check: everything a brand-new user does, in order.

The quickstart is the one code path a launch visitor is guaranteed to run, and
nothing else in CI covers it — the C suites never touch the container image, and
the integration's contract tests call `MemoryTools` in-process, so they exercise
neither the installed console scripts nor the MCP stdio protocol. This script
walks the documented path end to end:

    1. get the image (build it locally, or pull the published one)
    2. `docker run` it exactly as the README says
    3. the README's `docker exec … aegisdb client` ping/put/search commands
    4. reach the published port from the *host* over raw TCP (where a real
       Claude Code sits — a container-internal ping does not prove this)
    5. install the package into a clean venv (or resolve it with `uvx`)
    6. `aegisdb-init` scaffolds .mcp.json + the hooks
    7. drive the MCP server over stdio JSON-RPC: initialize, tools/list,
       memory_save, memory_search — the real protocol, not the Python API
    8. run the recall hook with a UserPromptSubmit event and assert the memory
       saved in step 7 comes back as injected context

Step 8 is the point of the whole thing: it proves a memory written in one
session is recalled in the next, through the installed entry points, against a
server nobody built by hand.

Two modes, same steps:

    --build --package local     what the current commit would ship (PR gate)
    --package pypi              what users actually get today (ghcr + PyPI)

Stdlib only, like the other drivers in this tree (eval/, tools/inspector/,
tests/contract/). Needs docker; `--package pypi` also needs `uv`.

The commands in steps 2-3 mirror the README's Quickstart rather than parsing it,
so change them together — this script can prove the documented commands work, not
that they are still the documented ones.

Usage:
    python3 scripts/first_contact.py --build --package local
    python3 scripts/first_contact.py --package pypi --port 19470
    python3 scripts/first_contact.py --build --mcp-spec 'mcp<2'   # older SDK major
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_IMAGE = "ghcr.io/d4n-larsson/aegisdb:latest"
DEFAULT_PORT = 9470
LOCAL_TAG = "aegisdb:first-contact"

# The README says `--name aegisdb -v aegis-data:/data`, but this check must be
# safe to run on a machine that is already running AegisDB (the author's is), so
# it never reuses those names — teardown would delete a live container and its
# data. Unique per run so concurrent runs don't collide either.
SUFFIX = f"fc-{os.getpid()}"
CONTAINER = f"aegisdb-{SUFFIX}"
VOLUME = f"aegis-data-{SUFFIX}"

# Unique per run so the assertions can't pass on a memory left by an earlier run.
MARKER = f"first-contact canary {SUFFIX}: the deploy runbook lives in LAUNCH.md"


class CheckFailed(Exception):
    """A step failed; the message is the user-facing reason."""


# ---- small helpers -------------------------------------------------------

def step(msg: str) -> None:
    print(f"\n== {msg}", flush=True)


def info(msg: str) -> None:
    print(f"   {msg}", flush=True)


def run(cmd: list[str], *, timeout: int = 300, cwd: str | None = None,
        env: dict | None = None, stdin: str | None = None,
        check: bool = True) -> subprocess.CompletedProcess:
    """Run a command, capturing both streams. Raises CheckFailed on nonzero."""
    info("$ " + " ".join(cmd))
    try:
        proc = subprocess.run(cmd, cwd=cwd, env=env, input=stdin, timeout=timeout,
                              capture_output=True, text=True)
    except subprocess.TimeoutExpired as exc:
        raise CheckFailed(f"timed out after {timeout}s: {' '.join(cmd)}") from exc
    except FileNotFoundError as exc:
        raise CheckFailed(f"command not found: {cmd[0]}") from exc
    if check and proc.returncode != 0:
        raise CheckFailed(f"exit {proc.returncode}: {' '.join(cmd)}\n"
                          f"--- stdout ---\n{proc.stdout}\n"
                          f"--- stderr ---\n{proc.stderr}")
    return proc


def contains(haystack: str, needle: str, what: str) -> None:
    if needle not in haystack:
        raise CheckFailed(f"{what}: expected to find {needle!r} in:\n{haystack}")


def hook_env(port: int, extra: dict | None = None) -> dict:
    """Environment for an entry point, with the ambient AegisDB config stripped.

    A developer box has AEGIS_* exported (and CLAUDE_PROJECT_DIR set inside
    Claude Code); either would silently point the check at the real server or
    the real namespace. Start from a scrubbed copy so a run here looks like a
    fresh machine.
    """
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("AEGIS_") and k != "CLAUDE_PROJECT_DIR"}
    # The default host/port is what a zero-config user gets; only tell the
    # entry point otherwise when this run is on a non-default port, so CI (on
    # the documented port) still exercises the zero-config path.
    if port != DEFAULT_PORT:
        env["AEGIS_HOST"] = "127.0.0.1"
        env["AEGIS_PORT"] = str(port)
    env.update(extra or {})
    return env


# ---- steps 1-2: the image and the container ------------------------------

def build_or_pull(image: str, build: bool) -> str:
    if build:
        step(f"Build the image from this checkout ({LOCAL_TAG})")
        run(["docker", "build", "-t", LOCAL_TAG, REPO], timeout=1800)
        return LOCAL_TAG
    step(f"Pull the published image ({image})")
    run(["docker", "pull", image], timeout=600)
    return image


def start_container(image: str, port: int) -> None:
    step(f"docker run the image (README quickstart) on port {port}")
    # The README's command, with the safe names from the header comment.
    run(["docker", "run", "-d", "--name", CONTAINER, "-p", f"{port}:9470",
         "-v", f"{VOLUME}:/data", image], timeout=120)

    deadline = time.monotonic() + 60
    last = ""
    while time.monotonic() < deadline:
        proc = run(["docker", "exec", CONTAINER, "aegisdb", "client", "ping"],
                   timeout=30, check=False)
        if proc.returncode == 0 and '"ok"' in proc.stdout:
            info(f"server answered ping: {proc.stdout.strip()}")
            return
        last = (proc.stdout + proc.stderr).strip()
        time.sleep(0.5)
    raise CheckFailed(f"server never became ready within 60s; last reply: {last}\n"
                      f"{container_logs()}")


def container_logs() -> str:
    proc = subprocess.run(["docker", "logs", "--tail", "50", CONTAINER],
                          capture_output=True, text=True)
    return f"--- container logs ---\n{proc.stdout}{proc.stderr}"


# ---- step 3: the README's client commands --------------------------------

def readme_client_commands() -> None:
    step("Run the README's `docker exec … aegisdb client` commands")
    text = f"prefers dark mode ({SUFFIX})"
    put = run(["docker", "exec", CONTAINER, "aegisdb", "client", "put",
               "--type", "semantic", "--tags", "user", text], timeout=60)
    contains(put.stdout, '"ok"', "client put")
    search = run(["docker", "exec", CONTAINER, "aegisdb", "client", "search",
                  "--tags", "user", "--top-k", "5"], timeout=60)
    contains(search.stdout, text, "client search should return what put wrote")
    info("put/search round trip through the CLI: ok")


# ---- step 4: the published port, from the host ---------------------------

def host_tcp_ping(port: int) -> None:
    step(f"Speak the wire protocol from the host on 127.0.0.1:{port}")
    # A container-internal ping proves the server runs; only this proves the
    # `-p` publish works, which is how Claude Code (on the host) connects.
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(b'{"operation":"ping"}\n')
        sock.settimeout(10)
        chunks = []
        while b"\n" not in b"".join(chunks):
            got = sock.recv(4096)
            if not got:
                break
            chunks.append(got)
    reply = b"".join(chunks).decode("utf-8", "replace").strip()
    contains(reply, '"ok"', "host-side ping")
    info(f"host reply: {reply}")


# ---- step 5: install the package ----------------------------------------

class Package:
    """How to invoke the integration's console scripts."""

    def __init__(self, source: str, venv: str | None):
        self.source = source
        self.venv = venv

    def cmd(self, script: str) -> list[str]:
        if self.source == "pypi":
            return ["uvx", "--from", "aegisdb-mcp", script]
        return [os.path.join(self.venv, "bin", script)]


def install_package(source: str, workdir: str, mcp_spec: str = "") -> Package:
    if source == "pypi":
        step("Resolve the published package with uvx (as .mcp.json does)")
        if shutil.which("uvx") is None:
            raise CheckFailed("uvx not found; --package pypi needs uv "
                              "(https://docs.astral.sh/uv/), or use --package local")
        proc = run(["uvx", "--from", "aegisdb-mcp", "aegisdb-init", "--help"],
                   timeout=600)
        contains(proc.stdout, "--dir", "uvx aegisdb-init --help")
        return Package(source, None)

    step("Install the integration into a clean venv (pip install)")
    venv = os.path.join(workdir, "venv")
    run([sys.executable, "-m", "venv", venv], timeout=300)
    pip = os.path.join(venv, "bin", "pip")
    env = dict(os.environ)
    # setuptools-scm derives the version from git; pin it like the PyPI workflow
    # does so the build doesn't depend on clone depth or .git being present.
    env["SETUPTOOLS_SCM_PRETEND_VERSION"] = "0.0.0+first-contact"
    run([pip, "install", "--quiet", os.path.join(REPO, "integrations", "claude-code")],
        timeout=900, env=env)
    if mcp_spec:
        # server.py supports both SDK majors, so CI runs this check once per
        # major; a fresh resolve only ever exercises the newest one.
        info(f"pinning the MCP SDK to {mcp_spec!r} for this run")
        run([pip, "install", "--quiet", mcp_spec], timeout=900, env=env)
        shown = run([pip, "show", "mcp"], timeout=120).stdout.splitlines()
        info(next((ln for ln in shown if ln.startswith("Version:")), "mcp: ?"))
    return Package(source, venv)


# ---- step 6: aegisdb-init ------------------------------------------------

def scaffold_project(pkg: Package, project: str, port: int) -> dict:
    step("Scaffold a project with aegisdb-init")
    run(pkg.cmd("aegisdb-init") + ["--dir", project, "--yes",
                                   "--host", "127.0.0.1", "--port", str(port)],
        timeout=600, env=hook_env(port))

    mcp_path = os.path.join(project, ".mcp.json")
    settings_path = os.path.join(project, ".claude", "settings.json")
    for path in (mcp_path, settings_path):
        if not os.path.isfile(path):
            raise CheckFailed(f"aegisdb-init did not write {path}")

    with open(mcp_path, encoding="utf-8") as fh:
        mcp = json.load(fh)
    entry = (mcp.get("mcpServers") or {}).get("memory")
    if not entry:
        raise CheckFailed(f".mcp.json has no `memory` server:\n{json.dumps(mcp, indent=2)}")
    env = entry.get("env") or {}
    if env.get("AEGIS_PORT") != str(port):
        raise CheckFailed(f".mcp.json points at port {env.get('AEGIS_PORT')!r}, "
                          f"expected {port}")

    hooks = json.dumps(json.load(open(settings_path, encoding="utf-8")))
    contains(hooks, "aegisdb-recall-hook", "settings.json recall hook")
    contains(hooks, "aegisdb-capture-hook", "settings.json capture hook")
    info(f"registered: {json.dumps(entry)}")
    return env


# ---- step 7: the MCP server over stdio JSON-RPC --------------------------

class McpStdio:
    """A minimal MCP stdio client: newline-delimited JSON-RPC on the pipes.

    Deliberately hand-rolled rather than using the `mcp` SDK's client — the
    point is to check the server the way an independent client would, so an SDK
    bug on both ends can't cancel out.
    """

    def __init__(self, cmd: list[str], cwd: str, env: dict):
        info("$ " + " ".join(cmd))
        self.proc = subprocess.Popen(cmd, cwd=cwd, env=env, text=True, bufsize=1,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.lines: queue.Queue = queue.Queue()
        self.stderr: list[str] = []
        self._pump(self.proc.stdout, self.lines.put)
        self._pump(self.proc.stderr, self.stderr.append)
        self._id = 0

    def _pump(self, stream, sink) -> None:
        thread = threading.Thread(target=lambda: [sink(ln) for ln in stream],
                                  daemon=True)
        thread.start()

    def _send(self, msg: dict) -> None:
        if self.proc.poll() is not None:
            raise CheckFailed(f"MCP server exited with {self.proc.returncode}\n"
                              f"{self.diagnostics()}")
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def call(self, method: str, params: dict | None = None, *,
             timeout: int = 60) -> dict:
        self._id += 1
        want = self._id
        self._send({"jsonrpc": "2.0", "id": want, "method": method,
                    "params": params or {}})
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.1, deadline - time.monotonic()))
            except queue.Empty:
                raise CheckFailed(f"no response to {method} within {timeout}s\n"
                                  f"{self.diagnostics()}") from None
            try:
                msg = json.loads(line)
            except ValueError:
                continue  # not JSON-RPC (a stray print); ignore
            if msg.get("id") != want:
                continue  # a notification or an out-of-band message
            if "error" in msg:
                raise CheckFailed(f"{method} returned an error: {msg['error']}\n"
                                  f"{self.diagnostics()}")
            return msg.get("result") or {}

    def notify(self, method: str, params: dict | None = None) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params or {}})

    def diagnostics(self) -> str:
        return "--- MCP server stderr ---\n" + "".join(self.stderr[-40:])

    def close(self) -> None:
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                stream.close()
            except OSError:
                pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def tool_payload(result: dict, what: str) -> dict:
    """Unwrap a tools/call result into the dict the tool returned.

    The SDK reports a dict return as JSON inside a text content block (and, on
    newer versions, also as `structuredContent`), so assert on the decoded
    payload rather than on the wire shape — the wrapper has changed between SDK
    releases and the tool contract has not.
    """
    if result.get("isError"):
        raise CheckFailed(f"{what} returned isError:\n{json.dumps(result, indent=2)}")
    if isinstance(result.get("structuredContent"), dict):
        return result["structuredContent"]
    for block in result.get("content") or []:
        if block.get("type") == "text":
            try:
                decoded = json.loads(block.get("text") or "")
            except ValueError:
                continue
            if isinstance(decoded, dict):
                return decoded
    raise CheckFailed(f"{what}: no JSON payload in the result:\n"
                      f"{json.dumps(result, indent=2)}")


def mcp_round_trip(pkg: Package, project: str, port: int, mcp_env: dict) -> None:
    step("Drive the MCP server over stdio (initialize → save → search)")
    # Claude Code launches the server with the project as cwd and the env from
    # .mcp.json; the namespace is derived from that cwd, so both must match for
    # the recall hook in the next step to see this memory.
    client = McpStdio(pkg.cmd("aegisdb-mcp"), project, hook_env(port, mcp_env))
    try:
        init = client.call("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "aegisdb-first-contact", "version": "0"},
        }, timeout=120)  # first call pays for the SDK import
        info(f"initialized: {json.dumps(init.get('serverInfo', {}))}")
        client.notify("notifications/initialized")

        names = {t.get("name") for t in client.call("tools/list").get("tools", [])}
        missing = {"memory_save", "memory_search", "memory_get", "memory_update",
                   "memory_relate"} - names
        if missing:
            raise CheckFailed(f"MCP server is missing tools: {sorted(missing)}; "
                              f"advertised: {sorted(names)}")
        info(f"tools: {sorted(names)}")

        saved = tool_payload(client.call("tools/call", {
            "name": "memory_save",
            "arguments": {"text": MARKER, "tags": ["first-contact"],
                          "semantic": True, "importance": 0.9},
        }), "memory_save")
        if not saved.get("ok") or not isinstance(saved.get("id"), int):
            raise CheckFailed(f"memory_save did not persist: {json.dumps(saved)}\n"
                              f"{client.diagnostics()}")
        info(f"memory_save → id={saved['id']} kind={saved.get('kind')}")

        found = tool_payload(client.call("tools/call", {
            "name": "memory_search",
            "arguments": {"tags": ["first-contact"], "top_k": 5},
        }), "memory_search")
        texts = [m.get("text", "") for m in found.get("memories") or []]
        if not any(MARKER in t for t in texts):
            raise CheckFailed("memory_search did not return what memory_save "
                              f"wrote:\n{json.dumps(found, indent=2)}")
        info("save → search round trip through MCP: ok")
    finally:
        client.close()


# ---- step 8: the recall hook --------------------------------------------

def recall_hook_injects(pkg: Package, project: str, port: int) -> None:
    step("Run the recall hook and assert the memory comes back as context")
    event = json.dumps({
        "hook_event_name": "UserPromptSubmit",
        "prompt": "Where does the deploy runbook live?",
        "cwd": project,
        "session_id": SUFFIX,
    })
    proc = run(pkg.cmd("aegisdb-recall-hook"), timeout=300, cwd=project,
               env=hook_env(port), stdin=event)
    # The hook is best-effort by contract: it always exits 0 and prints nothing
    # when it has nothing to inject, so an empty stdout is the failure mode to
    # catch here, not a nonzero exit.
    if not proc.stdout.strip():
        raise CheckFailed("recall hook injected nothing (memory saved in the "
                          "previous step was not recalled)\n"
                          f"--- hook stderr ---\n{proc.stderr}")
    payload = json.loads(proc.stdout)
    context = (payload.get("hookSpecificOutput") or {}).get("additionalContext", "")
    contains(context, MARKER, "recall hook additionalContext")
    info(f"injected context:\n{context}")


# ---- teardown -----------------------------------------------------------

def teardown(keep: bool) -> None:
    if keep:
        print(f"\n-- kept container {CONTAINER} and volume {VOLUME} (--keep)")
        return
    step("Tear down")
    subprocess.run(["docker", "rm", "-f", CONTAINER], capture_output=True, text=True)
    subprocess.run(["docker", "volume", "rm", "-f", VOLUME],
                   capture_output=True, text=True)
    info(f"removed container {CONTAINER} and volume {VOLUME}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", default=DEFAULT_IMAGE,
                    help=f"image to run (default: {DEFAULT_IMAGE})")
    ap.add_argument("--build", action="store_true",
                    help="build the image from this checkout instead of pulling")
    ap.add_argument("--package", choices=["local", "pypi"], default="local",
                    help="install the integration from this checkout (local) or "
                         "resolve the published one with uvx (pypi)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"host port to publish (default: {DEFAULT_PORT}); a "
                         f"non-default port makes the entry points read "
                         f"AEGIS_HOST/AEGIS_PORT instead of their defaults")
    ap.add_argument("--mcp-spec", default="",
                    help="pip requirement pinning the MCP SDK under test (e.g. "
                         "'mcp<2'); --package local only, since uvx resolves the "
                         "published package's own dependencies")
    ap.add_argument("--keep", action="store_true",
                    help="leave the container and volume behind for debugging")
    args = ap.parse_args(argv)
    if args.mcp_spec and args.package != "local":
        ap.error("--mcp-spec only applies to --package local")

    print(f"first-contact: image={'(local build)' if args.build else args.image} "
          f"package={args.package} port={args.port}"
          + (f" mcp-spec={args.mcp_spec}" if args.mcp_spec else ""))
    if args.port == DEFAULT_PORT:
        # Better a clear message than a confusing pass against the wrong server.
        with socket.socket() as probe:
            if probe.connect_ex(("127.0.0.1", DEFAULT_PORT)) == 0:
                print(f"\nfirst-contact FAILED: something is already listening on "
                      f"127.0.0.1:{DEFAULT_PORT} — stop it, or pass "
                      f"--port <other> to test alongside it.", file=sys.stderr)
                return 1

    workdir = tempfile.mkdtemp(prefix="aegis-first-contact-")
    project = os.path.join(workdir, "project")
    os.makedirs(project)
    started = time.monotonic()
    try:
        image = build_or_pull(args.image, args.build)
        start_container(image, args.port)
        readme_client_commands()
        host_tcp_ping(args.port)
        pkg = install_package(args.package, workdir, args.mcp_spec)
        mcp_env = scaffold_project(pkg, project, args.port)
        mcp_round_trip(pkg, project, args.port, mcp_env)
        recall_hook_injects(pkg, project, args.port)
    except CheckFailed as exc:
        print(f"\nfirst-contact FAILED: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nfirst-contact interrupted", file=sys.stderr)
        return 130
    finally:
        teardown(args.keep)
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)

    print(f"\nfirst-contact PASSED in {int(time.monotonic() - started)}s — "
          f"the documented quickstart works from scratch.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())