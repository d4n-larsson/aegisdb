"""Shared test harness: launch a real aegisdb process and build wired tools.

Used by integration/contract tests. Unit tests do not need this. Tests skip
automatically (via should_skip) when the aegisdb binary is not built.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

# integrations/claude-code -> repo root is three levels up from this file.
_HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.dirname(_HERE)          # integrations/claude-code
REPO_ROOT = os.path.dirname(os.path.dirname(PKG_ROOT))
AEGIS_BIN = os.path.join(REPO_ROOT, "build", "aegisdb")

sys.path.insert(0, PKG_ROOT)

TEST_DIM = 16  # small embedding dimension for fast, deterministic tests


def binary_available() -> bool:
    return os.path.exists(AEGIS_BIN)


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class AegisServer:
    """Context manager that launches aegisdb on a free port with a temp datadir."""

    def __init__(self, dim: int = TEST_DIM, phase: int = 4):
        self.dim = dim
        self.phase = phase
        self.port = _free_port()
        self.datadir = tempfile.mkdtemp(prefix="aegis_it_")
        self.proc = None

    def __enter__(self):
        self.proc = subprocess.Popen(
            [AEGIS_BIN, "--data-dir", self.datadir, "--port", str(self.port),
             "--phase", str(self.phase), "--embedding-dim", str(self.dim)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.1)
        self.__exit__(None, None, None)
        raise RuntimeError("aegisdb did not start")

    def __exit__(self, *exc):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
        # best-effort temp cleanup
        try:
            for f in os.listdir(self.datadir):
                os.remove(os.path.join(self.datadir, f))
            os.rmdir(self.datadir)
        except OSError:
            pass


def make_config(server: AegisServer, namespace="test-ns", embedding_mode="fake",
                **overrides):
    """Build a Config pointed at a running server. embedding_mode='fake' is a
    test-only marker; tests pass a FakeProvider explicitly to MemoryTools."""
    from aegis_mcp.config import load_config
    env = {
        "AEGIS_HOST": "127.0.0.1",
        "AEGIS_PORT": str(server.port),
        "AEGIS_NAMESPACE": namespace,
        "AEGIS_EMBEDDING_DIMENSIONS": str(server.dim),
        "AEGIS_EMBEDDING_MODE": "none",  # real providers unavailable in tests
    }
    return load_config(env=env, overrides=overrides)