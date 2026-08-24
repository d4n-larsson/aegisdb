"""Core memory operations (US1, US4, US5 logic).

These pure methods implement the behaviour behind the MCP tools; ``server.py``
binds them to FastMCP. Keeping the logic here (not in the MCP binding) means it
is fully testable without the ``mcp`` SDK installed. Every method maps to one
AegisDB operation (contracts/aegisdb-mapping.md), always scopes to the
configured namespace (FR-008), and never raises — backend failures become
``{"ok": false, "error": "unavailable"}`` (FR-009).
"""
from __future__ import annotations

import sys

from .client import AegisClient, AegisUnavailable
from .embeddings import EmbeddingProvider, cosine
from . import results


def record_to_memory(rec: dict, score: float | None = None) -> dict:
    """Project an AegisDB record onto the integration's Memory shape."""
    mem = {
        "id": rec.get("id"),
        "text": rec.get("data"),
        "kind": rec.get("type"),
        "tags": rec.get("tags", []),
        "importance": rec.get("importance"),
        "confidence": rec.get("confidence"),
        "created": rec.get("created"),
        "updated": rec.get("updated"),
    }
    if score is not None:
        mem["score"] = round(score, 4)
    # The typed claim (5.2) and the proof of it (5.3), when the record carries
    # them. Both were being dropped on the floor here: `data` is prose, which a
    # reader can search but cannot check, and a caller that asked `pattern` a
    # question got back only the sentence — no way to see which fact matched,
    # and no way to see why the record is believed at all.
    if isinstance(rec.get("fact"), dict):
        mem["fact"] = rec["fact"]
    exp = rec.get("explain")
    if isinstance(exp, dict) and isinstance(exp.get("derivation"), dict):
        mem["derivation"] = exp["derivation"]
    return mem


def score_record(rec: dict, query_embedding, *, semantic: bool) -> float | None:
    """Relevance score for a record (US5 / T041).

    Semantic: importance * confidence * cosine(similarity). Non-semantic: None
    (results keep AegisDB's native ordering).
    """
    if not semantic or query_embedding is None:
        return None
    sim = cosine(query_embedding, rec.get("embedding") or [])
    importance = rec.get("importance")
    importance = 0.5 if importance is None else float(importance)
    confidence = rec.get("confidence")
    confidence = 1.0 if confidence is None else float(confidence)
    # Blend: similarity dominates, importance/confidence modulate.
    return sim * (0.5 + 0.5 * importance) * confidence


def _explain_score(rec: dict) -> float | None:
    """The server's own rank score, from the `explain` block it was asked for.

    Used when the server did the ranking (lexical or fused), where a client-side
    cosine is either unavailable or the wrong signal. Note the scale differs from
    score_record's: a fused score is a reciprocal-rank sum (~0.01-0.03), a lexical
    one is an unbounded BM25 value. Compare scores only within one result set."""
    exp = rec.get("explain")
    if not isinstance(exp, dict):
        return None
    score = exp.get("score")
    return float(score) if isinstance(score, (int, float)) else None


def _suppress_near_duplicates(scored, threshold):
    """Drop a memory whose embedding is >= `threshold` cosine to an already-kept,
    higher-ranked one, so recall doesn't spend tokens re-injecting the same fact
    phrased several ways. `scored` is a list of (score, record) sorted best-first;
    the highest-scored member of each near-duplicate cluster is the one kept.
    A threshold outside (0, 1) disables the filter (returns the input unchanged).
    Records without an embedding are always kept (nothing to compare)."""
    if not 0.0 < threshold < 1.0:
        return scored
    kept, kept_vecs = [], []
    for s, r in scored:
        vec = r.get("embedding") or []
        if vec and any(cosine(vec, kv) >= threshold for kv in kept_vecs):
            continue
        kept.append((s, r))
        if vec:
            kept_vecs.append(vec)
    return kept


class MemoryTools:
    def __init__(self, config, client: AegisClient, provider: EmbeddingProvider):
        self.config = config
        self.client = client
        self.provider = provider

    # ---- helpers ---------------------------------------------------------

    def _request(self, payload: dict, read_timeout_ms: int | None = None) -> dict:
        payload.setdefault("agent_id", self.config.namespace)
        return self.client.request(payload, read_timeout_ms=read_timeout_ms)

    def _embeddings_usable(self) -> bool:
        """Whether the provider can supply correctly-sized embeddings.

        Validated once, lazily. The first call loads the model (for a local
        provider) and checks its output dimension against the configured size.
        This is done here rather than at server startup on purpose: loading a
        local model to read its dimension can stall (e.g. a Hugging Face Hub
        check), and blocking the MCP ``initialize`` handshake on it trips the
        client's startup timeout. On a mismatch, embeddings are disabled (so
        semantic search degrades) and a warning is logged, instead of silently
        sending wrong-sized vectors the backend would reject.
        """
        cached = getattr(self, "_emb_usable", None)
        if cached is not None:
            return cached
        usable = self.provider.available()
        if usable and self.provider.dimension() != self.config.embedding_dimensions:
            print(f"[aegis-mcp] embedding dimension mismatch: "
                  f"provider={self.provider.dimension()} "
                  f"config={self.config.embedding_dimensions}; disabling embeddings",
                  file=sys.stderr)
            usable = False
        self._emb_usable = usable
        return usable

    def _send(self, payload: dict, read_timeout_ms=None):
        """Send a request and translate transport/backend failures.

        Returns ``(resp, None)`` on success, or ``(None, error_result)`` if the
        backend was unreachable or returned ``ok=false`` — so callers do
        ``resp, err = self._send(...); if err: return err``.
        """
        try:
            resp = self._request(payload, read_timeout_ms=read_timeout_ms)
        except AegisUnavailable as exc:
            return None, results.unavailable(str(exc))
        if not resp.get("ok"):
            return None, results.from_aegis_error(resp)
        return resp, None

    # ---- operations ------------------------------------------------------

    def save(self, text: str, tags=None, importance: float = 0.5,
             semantic: bool = False, confidence: float = 1.0,
             fact: dict | None = None) -> dict:
        """`fact` attaches a typed {s, p, o} assertion (ROADMAP 5.2) alongside
        the prose. The server validates it against its predicate registry and
        refuses the whole insert if it does not match — which is the intended
        arrangement: a client-side check is an optimization, and the server's
        answer is the one that counts."""
        if not text or not text.strip():
            return results.err("invalid", "text must be non-empty")
        payload = {
            "operation": "insert",
            "type": "semantic" if semantic else "episodic",
            "data": text,
            "tags": list(tags or []),
            "importance": importance,
        }
        if semantic:
            payload["confidence"] = confidence
        if fact is not None:
            payload["fact"] = fact
        if self._embeddings_usable():
            payload["embedding"] = self.provider.embed_document(text)
        resp, err = self._send(payload)
        if err:
            return err
        rec = resp.get("record", {})
        return results.ok(id=rec.get("id"), kind=rec.get("type"))

    def get(self, id: int) -> dict:
        resp, err = self._send({"operation": "get", "id": id})
        if err:
            return err
        return results.ok(memory=record_to_memory(resp.get("record", {})))

    def search(self, query: str | None = None, tags=None, match: str = "any",
               start_time: int | None = None, end_time: int | None = None,
               top_k: int | None = None, kind: str | None = None,
               max_importance: float | None = None,
               order: str | None = None, lexical: bool = False,
               pattern: dict | None = None, subsume: bool = False,
               derivations: bool = False) -> dict:
        """Recall memories. `lexical` opts into the server's BM25 keyword index
        (fused with the embedding when one is available), which is what makes an
        exact identifier findable and is the *only* content-based path when
        embeddings are off — the recall hook's default.

        It is opt-in per call site rather than always-on because server-ranked
        results carry a score on a different scale (see _explain_score): a caller
        that filters on a cosine floor — capture's supersede detection does —
        would silently discard everything if handed fused scores."""
        top_k = top_k or self.config.recall_top_k
        tags = list(tags or [])
        if (not query and not tags and start_time is None
                and end_time is None and pattern is None):
            return results.err("invalid",
                               "search requires query, tags, a pattern, or a "
                               "time range")

        payload = {"operation": "search", "top_k": top_k}
        if pattern is not None:
            # Filter on the typed fact a record asserts (ROADMAP 5.2). With all
            # three positions bound this is an index probe, which is what makes
            # "does the corpus already say this?" cheap enough to ask per write.
            payload["pattern"] = pattern
            if subsume:
                # Broaden the subject through `is_a` at query time (5.3). A
                # question about a layer has to reach a fact about one of its
                # components, which is the whole multi-hop result — and it is
                # an expansion of the *query*, not a materialized closure, so
                # asking for it costs nothing when nothing subsumes.
                payload["subsume"] = True
        if tags:
            payload["tags"] = tags
            payload["match"] = match
        if start_time is not None:
            payload["start_time"] = start_time
        if end_time is not None:
            payload["end_time"] = end_time
        # Server-side candidate-selection filters (ignored by older servers, which
        # is safe: callers that rely on them also filter client-side).
        if kind is not None:
            payload["type"] = kind
        if max_importance is not None:
            payload["max_importance"] = max_importance
        if order is not None:
            payload["order"] = order

        query_embedding = None
        usable = self._embeddings_usable()
        semantic = bool(query) and usable
        want_lexical = bool(lexical and query)
        if want_lexical:
            payload["query"] = query
        if semantic:
            query_embedding = self.provider.embed_query(query)
            payload["embedding"] = query_embedding
            if want_lexical and self.config.recall_min_score > 0:
                # The cosine floor still gates the semantic side, but the server
                # must apply it *before* fusing: applied client-side afterwards it
                # would also discard lexical-only hits, which have no cosine.
                payload["min_score"] = self.config.recall_min_score
        # When the server ranks (fused or lexical-only) its order is
        # authoritative — re-sorting by client-side cosine would throw the fusion
        # away — so ask it to explain and carry its score through.
        server_ranked = want_lexical
        if server_ranked or derivations:
            # `explain` carries two unrelated things: the ranking breakdown the
            # fused path needs, and the derivation the read path renders. A
            # pattern search is not server-ranked, so the flag has to be
            # reachable on its own.
            payload["explain"] = True

        try:
            resp = self._request(payload)
        except AegisUnavailable as exc:
            return {**results.unavailable(str(exc)), "memories": [], "total": 0,
                    "degraded": True}
        # A server built with --no-lexical-index rejects `query` with NOT_READY.
        # Retry once without it rather than failing recall outright.
        if (not resp.get("ok") and want_lexical
                and str(resp.get("error", {}).get("code", "")) == "NOT_READY"):
            for k in ("query", "explain", "min_score"):
                payload.pop(k, None)
            if derivations:
                payload["explain"] = True
            want_lexical = False
            server_ranked = False
            try:
                resp = self._request(payload)
            except AegisUnavailable as exc:
                return {**results.unavailable(str(exc)), "memories": [],
                        "total": 0, "degraded": True}
        # Degraded now means no content-based retrieval happened at all: no usable
        # embeddings *and* no lexical fallback.
        degraded = bool(query) and not semantic and not want_lexical
        if not resp.get("ok"):
            return {**results.from_aegis_error(resp), "memories": [], "total": 0,
                    "degraded": degraded}

        records = resp.get("records", [])
        if server_ranked:
            scored = [(_explain_score(r), r) for r in records]
            # min_score was applied server-side; dedup still pays for itself here
            # (it keeps recall from re-injecting one fact phrased several ways).
            scored = _suppress_near_duplicates(
                scored, self.config.recall_dedup_threshold)
        else:
            scored = [(score_record(r, query_embedding, semantic=semantic), r)
                      for r in records]
            if semantic:
                scored.sort(key=lambda s: (s[0] if s[0] is not None else 0.0),
                            reverse=True)
                scored = [(s, r) for (s, r) in scored
                          if (s or 0.0) >= self.config.recall_min_score]
                scored = _suppress_near_duplicates(
                    scored, self.config.recall_dedup_threshold)
        scored = scored[:top_k]
        memories = [record_to_memory(r, score=s) for (s, r) in scored]
        return results.ok(total=len(memories), memories=memories, degraded=degraded)

    def update(self, id: int, text: str | None = None,
               confidence: float | None = None, tags=None) -> dict:
        payload = {"operation": "update", "id": id}
        if text is not None:
            payload["data"] = text
        if confidence is not None:
            payload["confidence"] = confidence
        if tags is not None:
            payload["tags"] = list(tags)
        if text is not None and self._embeddings_usable():
            payload["embedding"] = self.provider.embed_document(text)
        resp, err = self._send(payload)
        if err:
            return err
        return results.ok(memory=record_to_memory(resp.get("record", {})))

    def predicates(self) -> dict:
        """The typed-fact vocabulary the server declares (ROADMAP 5.2).

        What makes `AEGIS_EXTRACT_REGISTRY` optional: the server is the thing
        that enforces the vocabulary, so asking it removes the second copy of
        the registry file this integration used to need — and the drift that
        copy invited, which surfaced as the server refusing triples and looked
        like a bad model rather than a misconfiguration.

        `enforced` is not the same as a non-empty list: a server with no
        registry accepts *any* predicate, which is the opposite of declaring
        none. An older server has no such operation and answers
        INVALID_REQUEST, which arrives as `{"ok": false}` — read as "no
        vocabulary", the same degradation as every other capability check here.
        """
        resp, err = self._send({"operation": "predicates"})
        if err:
            return err
        return results.ok(predicates=resp.get("predicates") or [],
                          total=resp.get("total", 0),
                          enforced=bool(resp.get("enforced")))

    def conflicts(self, limit: int | None = None) -> dict:
        """Contradictions the inference job flagged and refused to settle.

        `stats` says how many; this says which, which is what anything meaning
        to act on one needs. Scoped by the server to the caller's own namespace
        — nothing here names a tenant, and a request that did would be ignored.

        An older server has no such operation and answers INVALID_REQUEST,
        which arrives as `{"ok": false}` like any other refusal. Callers treat
        that as "nothing to adjudicate", because it is: a server that cannot
        list contradictions is not one that has any to hand over.
        """
        payload = {"operation": "conflicts"}
        if limit is not None:
            payload["limit"] = limit
        resp, err = self._send(payload)
        if err:
            return err
        return results.ok(conflicts=resp.get("conflicts") or [],
                          total=resp.get("total", 0),
                          capped=bool(resp.get("capped")),
                          truncated=bool(resp.get("truncated")))

    def relate(self, from_id: int, to_id: int, kind: str | None = None) -> dict:
        payload = {"operation": "relate", "from_id": from_id, "to_id": to_id}
        if kind:
            payload["kind"] = kind
        resp, err = self._send(payload)
        if err:
            return err
        return results.ok(relationship=resp.get("relationship"))

    def delete(self, id: int) -> dict:
        """Tombstone a record: dropped from recall, reclaimed by compaction, but
        recoverable from the log until then. Used to archive summarized sources."""
        resp, err = self._send({"operation": "delete", "id": id})
        if err:
            return err
        return results.ok(id=id, deleted=resp.get("deleted", True))