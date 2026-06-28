"""Integration configuration (T006) and project-namespace resolution (T009).

Resolution precedence (lowest to highest): built-in defaults -> optional JSON
config file (``AEGIS_CONFIG`` path) -> environment variables -> explicit
overrides. All values are plain types so the config is trivially testable.
"""
from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, fields


@dataclass
class Config:
    aegis_host: str = "127.0.0.1"
    aegis_port: int = 9470
    connect_timeout_ms: int = 500
    read_timeout_ms: int = 1000

    auth_token: str = ""  # bearer token sent with every request when set

    namespace: str = ""  # resolved separately; never blank after load_config()

    embedding_mode: str = "none"  # "voyage" | "local" | "none"
    embedding_model: str = "voyage-3-large"
    embedding_dimensions: int = 1024

    recall_enabled: bool = True
    recall_time_budget_ms: int = 800
    recall_top_k: int = 5
    recall_min_score: float = 0.2

    capture_enabled: bool = True
    capture_scope: str = "session"  # "session" | "turn"
    capture_min_salience: float = 0.5


# Map each config field to its environment variable name.
_ENV = {
    "aegis_host": "AEGIS_HOST",
    "aegis_port": "AEGIS_PORT",
    "connect_timeout_ms": "AEGIS_CONNECT_TIMEOUT_MS",
    "read_timeout_ms": "AEGIS_READ_TIMEOUT_MS",
    "auth_token": "AEGIS_AUTH_TOKEN",
    "namespace": "AEGIS_NAMESPACE",
    "embedding_mode": "AEGIS_EMBEDDING_MODE",
    "embedding_model": "AEGIS_EMBEDDING_MODEL",
    "embedding_dimensions": "AEGIS_EMBEDDING_DIMENSIONS",
    "recall_enabled": "AEGIS_RECALL_ENABLED",
    "recall_time_budget_ms": "AEGIS_RECALL_TIME_BUDGET_MS",
    "recall_top_k": "AEGIS_RECALL_TOP_K",
    "recall_min_score": "AEGIS_RECALL_MIN_SCORE",
    "capture_enabled": "AEGIS_CAPTURE_ENABLED",
    "capture_scope": "AEGIS_CAPTURE_SCOPE",
    "capture_min_salience": "AEGIS_CAPTURE_MIN_SALIENCE",
}

_BOOL = {"recall_enabled", "capture_enabled"}
_INT = {
    "aegis_port", "connect_timeout_ms", "read_timeout_ms",
    "embedding_dimensions", "recall_time_budget_ms", "recall_top_k",
}
_FLOAT = {"recall_min_score", "capture_min_salience"}


def _coerce(name: str, value):
    if name in _BOOL:
        return str(value).strip().lower() in ("1", "true", "yes", "on")
    if name in _INT:
        return int(value)
    if name in _FLOAT:
        return float(value)
    return str(value)


def _apply(cfg: Config, name: str, value) -> None:
    setattr(cfg, name, _coerce(name, value))


def resolve_namespace(env=None, cwd: str | None = None, explicit: str | None = None) -> str:
    """Resolve the project isolation namespace (FR-008, R-008).

    Order: explicit override -> AEGIS_NAMESPACE -> CLAUDE_PROJECT_DIR/cwd basename
    plus a short stable hash of the full path (avoids collisions between two
    projects sharing a directory name).
    """
    env = os.environ if env is None else env
    if explicit:
        return explicit
    if env.get("AEGIS_NAMESPACE"):
        return env["AEGIS_NAMESPACE"]
    root = env.get("CLAUDE_PROJECT_DIR") or cwd or os.getcwd()
    root = os.path.abspath(root)
    base = os.path.basename(root.rstrip("/")) or "default"
    digest = hashlib.sha256(root.encode()).hexdigest()[:8]
    return f"{base}-{digest}"


def load_config(env=None, cwd: str | None = None, overrides: dict | None = None) -> Config:
    env = os.environ if env is None else env
    cfg = Config()
    valid = {f.name for f in fields(Config)}

    # 1) optional JSON config file
    path = env.get("AEGIS_CONFIG")
    if path and os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as fh:
            for k, v in (json.load(fh) or {}).items():
                if k in valid:
                    _apply(cfg, k, v)

    # 2) environment variables
    for name, var in _ENV.items():
        if var in env and env[var] != "":
            _apply(cfg, name, env[var])

    # 3) explicit overrides (highest precedence)
    for k, v in (overrides or {}).items():
        if k in valid:
            _apply(cfg, k, v)

    # Default embedding mode: if unset but a Voyage key is present, prefer voyage.
    if "AEGIS_EMBEDDING_MODE" not in env and not (overrides or {}).get("embedding_mode"):
        if env.get("VOYAGE_API_KEY"):
            cfg.embedding_mode = "voyage"

    # Namespace always resolves to a non-blank value.
    cfg.namespace = resolve_namespace(env=env, cwd=cwd, explicit=cfg.namespace or None)
    return cfg