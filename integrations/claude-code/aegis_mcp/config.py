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
    # Diversity gate: drop a recalled memory whose embedding is >= this cosine to
    # an already-kept, higher-ranked one, so recall doesn't spend tokens
    # re-injecting the same fact several ways. Semantic recall only. 0 or >= 1
    # disables. (Complements the server's `consolidate`, which merges duplicates
    # destructively; this is non-destructive and applies even before it runs.)
    recall_dedup_threshold: float = 0.95
    # Token-cost guard rails for the injected context block. A few long memories
    # can otherwise dominate a turn; cap each memory's rendered text and the
    # block's total size (both measured in characters; 0 = unlimited).
    recall_max_chars_per_memory: int = 500
    recall_char_budget: int = 2000

    capture_enabled: bool = True
    capture_scope: str = "session"  # "session" | "turn"
    capture_min_salience: float = 0.5

    # Background summarization (opt-in; see docs/summarization-design.md). Run by
    # `aegisdb-summarize` on an operator schedule — NOT a per-turn hook. Default
    # `none` = off, no dependency. `claude-code` distills via the `claude` CLI.
    summary_mode: str = "none"  # "none"|"fake"|"claude-code"|"anthropic"|"openai"
    summary_model: str = ""  # optional model override for the selected backend
    summary_api_base: str = ""  # openai backend: base URL for openai-compatible APIs
    summary_min_age_ms: int = 604800000  # only distill memories older than 7 days
    summary_max_importance: float = 0.6  # leave high-importance memories alone
    summary_min_cluster: int = 3  # min related memories to bother summarizing
    summary_max_cluster: int = 20  # max memories folded into one summary
    summary_max_clusters_per_run: int = 20  # bound work/cost per run
    summary_min_confidence: float = 0.0  # skip a summary below this confidence
    summary_scan_top_k: int = 1000  # candidate records pulled per run

    # LLM fact extraction for capture (opt-in; see ROADMAP 2.1). Default `none`
    # keeps the heuristic capture path. When enabled, a session transcript is
    # distilled into durable facts stored as semantic memories instead of raw
    # marker-matched sentences.
    extract_mode: str = "none"  # "none"|"fake"|"claude-code"|"anthropic"|"openai"
    extract_model: str = ""  # optional model override for the selected backend
    extract_api_base: str = ""  # openai backend: base URL for openai-compatible APIs
    extract_max_facts: int = 12  # cap facts stored per session
    extract_max_input_chars: int = 24000  # cap transcript chars sent to the model


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
    "recall_dedup_threshold": "AEGIS_RECALL_DEDUP_THRESHOLD",
    "recall_max_chars_per_memory": "AEGIS_RECALL_MAX_CHARS_PER_MEMORY",
    "recall_char_budget": "AEGIS_RECALL_CHAR_BUDGET",
    "capture_enabled": "AEGIS_CAPTURE_ENABLED",
    "capture_scope": "AEGIS_CAPTURE_SCOPE",
    "capture_min_salience": "AEGIS_CAPTURE_MIN_SALIENCE",
    "summary_mode": "AEGIS_SUMMARY_MODE",
    "summary_model": "AEGIS_SUMMARY_MODEL",
    "summary_api_base": "AEGIS_SUMMARY_API_BASE",
    "summary_min_age_ms": "AEGIS_SUMMARY_MIN_AGE_MS",
    "summary_max_importance": "AEGIS_SUMMARY_MAX_IMPORTANCE",
    "summary_min_cluster": "AEGIS_SUMMARY_MIN_CLUSTER",
    "summary_max_cluster": "AEGIS_SUMMARY_MAX_CLUSTER",
    "summary_max_clusters_per_run": "AEGIS_SUMMARY_MAX_CLUSTERS_PER_RUN",
    "summary_min_confidence": "AEGIS_SUMMARY_MIN_CONFIDENCE",
    "summary_scan_top_k": "AEGIS_SUMMARY_SCAN_TOP_K",
    "extract_mode": "AEGIS_EXTRACT_MODE",
    "extract_model": "AEGIS_EXTRACT_MODEL",
    "extract_api_base": "AEGIS_EXTRACT_API_BASE",
    "extract_max_facts": "AEGIS_EXTRACT_MAX_FACTS",
    "extract_max_input_chars": "AEGIS_EXTRACT_MAX_INPUT_CHARS",
}

_BOOL = {"recall_enabled", "capture_enabled"}
_INT = {
    "aegis_port", "connect_timeout_ms", "read_timeout_ms",
    "embedding_dimensions", "recall_time_budget_ms", "recall_top_k",
    "recall_max_chars_per_memory", "recall_char_budget",
    "summary_min_age_ms", "summary_min_cluster", "summary_max_cluster",
    "summary_max_clusters_per_run", "summary_scan_top_k",
    "extract_max_facts", "extract_max_input_chars",
}
_FLOAT = {"recall_min_score", "recall_dedup_threshold", "capture_min_salience",
          "summary_max_importance", "summary_min_confidence"}


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