"""Integration configuration (T006) and project-namespace resolution (T009).

Resolution precedence (lowest to highest): built-in defaults -> JSON config
file -> environment variables -> explicit overrides. All values are plain types
so the config is trivially testable.

The config file is **discovered**, not configured: ``AEGIS_CONFIG`` still names
one explicitly, and with it unset the project's ``.aegisdb/config.json`` is read
if present. That is what makes one setting reach both callers — the MCP server
gets its env from ``.mcp.json``, but a Claude Code hook entry has no env field
at all, so until now a hook fell back to the built-in defaults and silently
recalled without embeddings while the MCP server used them. A file on disk is
read by whoever runs next.
"""
from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, fields

# Everything the integration reads from or writes into a *consumer* project
# lives under one directory, so a project has one place to look. The split
# inside it is deliberate and is the whole reason `local/` exists: the files
# beside it are reviewed inputs that belong in git (the namespace this project
# writes under, its predicate registry, its fact corpora), and `local/` is
# machine state that never should be. One gitignore line, no judgement calls.
PROJECT_DIR = ".aegisdb"
CONFIG_BASENAME = "config.json"
LOCAL_SUBDIR = "local"


class ConfigError(RuntimeError):
    """A config file was named or found, and could not be used.

    Two cases: a file that exists but cannot be read, and a path named by
    ``AEGIS_CONFIG`` that is not there at all. A *discovered* file that is
    absent is neither — that is the ordinary "this project has no config" case.

    Raised rather than ignored: a discovered file that silently does nothing is
    the failure mode the predicate registry already refuses (a broken registry
    is an error, not silent permissiveness). Both hooks already catch every
    exception and degrade to a no-op with the message on stderr, so this
    surfaces as a legible complaint rather than a broken turn.
    """


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
    # Contradiction -> supersession (ROADMAP 2.1 follow-up): when an extracted
    # fact updates/contradicts an existing memory, replace it (tombstone + a
    # `supersedes` provenance link) instead of accumulating both. Needs
    # embeddings (to find similar candidates) + an extractor backend (to judge).
    extract_supersede: bool = True  # active only when extract_mode is on
    extract_supersede_top_k: int = 5  # similar candidates considered per new fact
    extract_supersede_min_score: float = 0.6  # cosine floor for a candidate
    # Typed triples (ROADMAP 5.4). Off by default, and inert without a registry:
    # the point of the vocabulary is that it is a contract, so proposing triples
    # with nothing to check them against is not a smaller version of the feature.
    # `extract_registry` is the *same file* the server was started with —
    # a second copy would drift, and extraction would then propose triples the
    # server refuses, which looks like a bad model rather than a misconfiguration.
    extract_triples: bool = False
    extract_registry: str = ""  # path to the server's --predicate-registry file
    # The read path (ROADMAP 5.4 §5). Both default off and both are strictly
    # additive: with them off, search behaves exactly as it does today.
    ask_pattern: bool = False  # let the model express a question as a pattern
    ask_verbalize: bool = False  # render a derivation as prose beside it
    extract_max_triples: int = 16  # cap candidates proposed per transcript
    # Grounding a mention to an entity record (ROADMAP 5.4 §4). The floor is
    # high on purpose: conflating two entities writes facts about the wrong
    # thing and inference then compounds them undetectably, while splitting one
    # entity in two only loses inferences and `consolidate` can merge them
    # later. A near-miss mints rather than guesses.
    #
    # Deliberately *not* shared with extract_supersede_min_score, though both
    # ask "are these the same?": consolidation's two errors are symmetric — a
    # missed merge and a wrong merge both cost a duplicate — so it can sit near
    # the middle, where this cannot.
    grounding_min_score: float = 0.85  # cosine floor for reusing an entity
    grounding_top_k: int = 5  # entity candidates considered per mention
    # Two mentions per triple at worst (subject and an id-valued object), so
    # this has to cover 2 * extract_max_triples or a first capture on a rich
    # transcript silently drops the overflow as ungrounded.
    grounding_max_mint: int = 32  # new entity records per extraction
    # Confidence for a fact a model proposed, deliberately below what a human
    # or a rule writes (ROADMAP 5.4 §7). It is not decoration: 5.3 propagates
    # confidence as a product along a derivation chain, so this number silently
    # sets how much weight every conclusion drawn from parsed facts carries
    # relative to one drawn from asserted facts.
    extract_triple_confidence: float = 0.6
    # Adjudication (ROADMAP 5.4 §6): hand a contradiction the rules flagged and
    # refused to settle to the model, and write the verdict as a supersession.
    #
    # Off by default and capped per run, because this is the one place in the
    # horizon where a model error becomes durable state. Everything else the
    # seam does is additive — a bad triple is one bad fact, a bad verbalization
    # is prose beside a payload that contradicts it — while a bad verdict
    # tombstones a record somebody wrote. The cap bounds a bad *run*, not just
    # a bad call: a model that has started answering badly does so for every
    # pair, and an uncapped loop would work through the whole backlog before
    # anyone saw it.
    adjudicate_conflicts: bool = False
    adjudicate_max_per_run: int = 8


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
    "extract_supersede": "AEGIS_EXTRACT_SUPERSEDE",
    "extract_supersede_top_k": "AEGIS_EXTRACT_SUPERSEDE_TOP_K",
    "extract_supersede_min_score": "AEGIS_EXTRACT_SUPERSEDE_MIN_SCORE",
    "extract_triples": "AEGIS_EXTRACT_TRIPLES",
    "ask_pattern": "AEGIS_ASK_PATTERN",
    "ask_verbalize": "AEGIS_ASK_VERBALIZE",
    "extract_registry": "AEGIS_EXTRACT_REGISTRY",
    "extract_max_triples": "AEGIS_EXTRACT_MAX_TRIPLES",
    "grounding_min_score": "AEGIS_GROUNDING_MIN_SCORE",
    "grounding_top_k": "AEGIS_GROUNDING_TOP_K",
    "grounding_max_mint": "AEGIS_GROUNDING_MAX_MINT",
    "extract_triple_confidence": "AEGIS_EXTRACT_TRIPLE_CONFIDENCE",
    "adjudicate_conflicts": "AEGIS_ADJUDICATE_CONFLICTS",
    "adjudicate_max_per_run": "AEGIS_ADJUDICATE_MAX_PER_RUN",
}

_BOOL = {"recall_enabled", "capture_enabled", "extract_supersede",
         "extract_triples", "ask_pattern", "ask_verbalize",
         "adjudicate_conflicts"}
_INT = {
    "aegis_port", "connect_timeout_ms", "read_timeout_ms",
    "embedding_dimensions", "recall_time_budget_ms", "recall_top_k",
    "recall_max_chars_per_memory", "recall_char_budget",
    "summary_min_age_ms", "summary_min_cluster", "summary_max_cluster",
    "summary_max_clusters_per_run", "summary_scan_top_k",
    "extract_max_facts", "extract_max_input_chars", "extract_supersede_top_k",
    "extract_max_triples", "grounding_top_k", "grounding_max_mint",
    "adjudicate_max_per_run",
}
_FLOAT = {"recall_min_score", "recall_dedup_threshold", "capture_min_salience",
          "grounding_min_score", "extract_triple_confidence",
          "summary_max_importance", "summary_min_confidence",
          "extract_supersede_min_score"}


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


def project_root(env=None, cwd: str | None = None) -> str:
    """The consumer project's root: CLAUDE_PROJECT_DIR, else cwd.

    The env var outranks `cwd` on purpose — a hook is invoked with whatever
    working directory the session happens to have, which may be a subdirectory,
    while `CLAUDE_PROJECT_DIR` always names the project. A **CLI that was given
    an explicit directory** must not go through this: see `env_for_explicit_root`.
    """
    env = os.environ if env is None else env
    return os.path.abspath(env.get("CLAUDE_PROJECT_DIR") or cwd or os.getcwd())


def env_for_explicit_root(env=None) -> dict:
    """`env` minus `CLAUDE_PROJECT_DIR`, for a CLI given an explicit directory.

    Naming a directory has to mean that directory. Without this, running
    `aegisdb-init --dir other/` from inside a Claude Code session resolves the
    project root to the *session's* project and writes its namespace into
    `other/.aegisdb/config.json` — after which `other` reads and writes another
    project's memories. Pinning is what makes that permanent rather than
    self-correcting, so the guard belongs wherever a pin is written.

    Everything else in the environment (the `AEGIS_*` settings) still applies:
    the point is which *directory* is meant, not which settings are in force.
    """
    env = os.environ if env is None else env
    return {k: v for k, v in env.items() if k != "CLAUDE_PROJECT_DIR"}


def project_dir(env=None, cwd: str | None = None) -> str:
    """The project's ``.aegisdb/`` directory. Not created by reading."""
    return os.path.join(project_root(env, cwd), PROJECT_DIR)


def local_dir(env=None, cwd: str | None = None) -> str:
    """``.aegisdb/local/`` — machine state, gitignored wholesale."""
    return os.path.join(project_dir(env, cwd), LOCAL_SUBDIR)


def config_path(env=None, cwd: str | None = None) -> str:
    """Where the JSON config is read from.

    ``AEGIS_CONFIG`` when set (an explicit path always wins, and may point
    anywhere), otherwise the project's ``.aegisdb/config.json``. The path is
    returned whether or not it exists; callers check.
    """
    env = os.environ if env is None else env
    return env.get("AEGIS_CONFIG") or os.path.join(
        project_dir(env, cwd), CONFIG_BASENAME)


def derive_namespace(env=None, cwd: str | None = None) -> str:
    """The fallback namespace: project basename plus a hash of its full path.

    The hash avoids collisions between two projects sharing a directory name —
    but it also means **moving or renaming the project changes the namespace**,
    which silently orphans everything written under the old one. That is why
    `aegisdb-init` now pins the derived value into `.aegisdb/config.json`
    instead of leaving it implied: the same string, but no longer a function of
    where the directory happens to sit.
    """
    root = project_root(env, cwd)
    base = os.path.basename(root.rstrip("/")) or "default"
    digest = hashlib.sha256(root.encode()).hexdigest()[:8]
    return f"{base}-{digest}"


def resolve_namespace(env=None, cwd: str | None = None, explicit: str | None = None) -> str:
    """Resolve the project isolation namespace (FR-008, R-008).

    Order: explicit override -> AEGIS_NAMESPACE -> the path-derived fallback.
    `load_config` passes the file's value as `explicit`, so a namespace pinned
    in `.aegisdb/config.json` beats the fallback and loses to the environment,
    which is the same precedence every other setting follows.
    """
    env = os.environ if env is None else env
    if explicit:
        return explicit
    if env.get("AEGIS_NAMESPACE"):
        return env["AEGIS_NAMESPACE"]
    return derive_namespace(env, cwd)


def load_config(env=None, cwd: str | None = None, overrides: dict | None = None) -> Config:
    env = os.environ if env is None else env
    cfg = Config()
    valid = {f.name for f in fields(Config)}

    # 1) JSON config file: AEGIS_CONFIG when set, else the project's
    #    .aegisdb/config.json. Absent is normal and means "defaults"; present
    #    but unreadable is an error, because a file someone wrote and the
    #    integration silently ignored is worse than a complaint.
    path = config_path(env, cwd)
    from_file: set[str] = set()
    if not os.path.isfile(path):
        # ...with one exception: a path named by AEGIS_CONFIG is a claim that
        # the file is there, so its absence is a misconfiguration and not the
        # ordinary "this project has no config" case. Silence here is the
        # nastiest version of the failure this module exists to prevent: the
        # settings revert to built-in defaults, embedding_mode goes back to
        # `none`, and recall keeps working while quietly retrieving worse.
        #
        # It bites for real when `.aegisdb/` lives on one branch and
        # AEGIS_CONFIG points into the working tree: every checkout of a branch
        # without the directory silently reconfigures the integration.
        if env.get("AEGIS_CONFIG"):
            what = "is not a file" if os.path.exists(path) else "does not exist"
            raise ConfigError(f"AEGIS_CONFIG names {path}, which {what}")
    else:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                doc = json.load(fh) or {}
        except (OSError, ValueError) as exc:
            raise ConfigError(f"cannot read {path}: {exc}") from exc
        if not isinstance(doc, dict):
            raise ConfigError(f"{path}: expected a JSON object")
        for k, v in doc.items():
            if k not in valid:
                continue
            # Coercion inside the guard, so a bad *value* names the file too.
            # `{"aegis_port": "abc"}` escaping as a bare ValueError leaves the
            # MCP server dying on a message that identifies nothing.
            try:
                _apply(cfg, k, v)
            except (TypeError, ValueError) as exc:
                raise ConfigError(f"{path}: bad value for {k!r}: {exc}") from exc
            from_file.add(k)

    # 2) environment variables
    for name, var in _ENV.items():
        if var in env and env[var] != "":
            _apply(cfg, name, env[var])

    # 3) explicit overrides (highest precedence)
    for k, v in (overrides or {}).items():
        if k in valid:
            _apply(cfg, k, v)

    # Default embedding mode: if unset *by any source* but a Voyage key is
    # present, prefer voyage. The file counts as a source — it is a chosen
    # setting, not a default, and a stray VOYAGE_API_KEY in the environment
    # must not silently switch a project configured for `local` onto a hosted
    # provider at a dimension that provider does not produce.
    if ("AEGIS_EMBEDDING_MODE" not in env
            and "embedding_mode" not in from_file
            and not (overrides or {}).get("embedding_mode")):
        if env.get("VOYAGE_API_KEY"):
            cfg.embedding_mode = "voyage"

    # Namespace always resolves to a non-blank value.
    cfg.namespace = resolve_namespace(env=env, cwd=cwd, explicit=cfg.namespace or None)
    return cfg