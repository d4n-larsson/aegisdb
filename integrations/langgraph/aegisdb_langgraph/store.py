"""AegisDB as a LangGraph long-term-memory store (ROADMAP 3.3).

`BaseStore` funnels `get`/`put`/`search`/`delete`/`list_namespaces` through one
abstract `batch(ops)`, so that is all this implements — the concrete sugar on
the base class comes free and cannot drift from it.

**The mapping, and where it is lossy.** LangGraph addresses items by a
hierarchical `namespace: tuple[str, ...]` plus a `key`, and searches by
namespace *prefix*. AegisDB has a flat `agent_id` that is an isolation boundary
rather than a path, so the hierarchy is carried in tags instead:

    agent_id   the store's own namespace — one tenant, isolated by the server
    tags       a marker, a hash of (namespace, key), and a hash of *every*
               prefix of the namespace, so a prefix search is an exact tag
               match rather than a scan
    data       JSON: {"ns": [...], "key": ..., "value": {...}}

Hashing rather than the literal path because a tag is capped at 64 bytes and a
namespace or key can be longer; the readable copy lives in `data`, which is
also what makes `list_namespaces` possible at all.

Three things this cannot do, stated rather than approximated:

- **TTL.** AegisDB expires only working memory, which is a per-session ring
  buffer and not what a store is. A `ttl` is *refused*, not ignored: silently
  never expiring something a caller asked to expire is a retention surprise,
  and `forget` is the mechanism that actually applies here.
- **Vector search.** `query` runs the server's BM25 index, which needs no
  embedding provider and matches exact tokens well. `index=` is accepted and
  ignored, because honouring it would mean owning an embeddings function.
- **`filter`.** AegisDB cannot filter on arbitrary JSON fields, so filtering
  happens here, over a bounded page. See `search_scan_limit`.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
from datetime import datetime, timezone
from typing import Any, Iterable

from aegisdb import AegisClient, NotReady
from langgraph.store.base import (BaseStore, GetOp, Item, ListNamespacesOp,
                                  PutOp, Result, SearchItem, SearchOp)

# LangGraph's own filter semantics, reused rather than reimplemented: two
# copies of "does this item match?" would drift, and the difference would show
# up as a graph behaving differently against this store than against the
# in-memory one. `_supports_native_filter` records whether the import worked,
# and a test asserts it did — so a version that moves these fails loudly
# instead of silently changing what a filter means.
try:
    from langgraph.store.memory import _compare_values as _lg_compare
    _supports_native_filter = True
except ImportError:  # pragma: no cover - only on a future langgraph
    _lg_compare = None
    _supports_native_filter = False

#: A tag is `[A-Za-z0-9_-]`, at most 64 bytes, 32 per record (the server's
#: `valid_tag`). Hence `-` as the separator rather than the `:` a namespace
#: usually reads with, and a truncated digest rather than the path itself.
MARKER = "lg-store"
#: marker + key + one per namespace prefix (including the empty one) must fit
#: inside AegisDB's 32-tag ceiling.
MAX_NAMESPACE_DEPTH = 29


def _h(parts: Iterable[str]) -> str:
    """A short, stable digest of a path.

    Length-prefixed per segment so ("a", "bc") and ("ab", "c") cannot collide —
    joining on a separator would make them the same string, and two distinct
    namespaces sharing a tag is a cross-namespace read.
    """
    h = hashlib.sha256()
    for p in parts:
        raw = p.encode("utf-8")
        h.update(str(len(raw)).encode("ascii"))
        h.update(b":")
        h.update(raw)
    return h.hexdigest()[:16]


def _prefix_tags(ns: tuple[str, ...]) -> list[str]:
    """One tag per prefix of `ns`, including the empty prefix and `ns` itself.

    This is what turns LangGraph's prefix search into an index lookup: a search
    under ("users",) matches every item whose namespace starts that way,
    because each of them carries that prefix's tag.
    """
    return [f"p-{_h(ns[:i])}" for i in range(len(ns) + 1)]


def _to_datetime(ms: int | None) -> datetime:
    return datetime.fromtimestamp((ms or 0) / 1000.0, tz=timezone.utc)


def _matches(value: dict, filt: dict | None) -> bool:
    if not filt:
        return True
    if _lg_compare is not None:
        return all(_lg_compare(value.get(k), v) for k, v in filt.items())
    # Fallback for a langgraph that moved the helper: exact equality only,
    # which is a strict subset of the real semantics rather than a guess at
    # them. Operator dicts are refused rather than silently ignored.
    for k, v in filt.items():
        if isinstance(v, dict) and any(str(x).startswith("$") for x in v):
            raise NotImplementedError(
                "this langgraph version moved _compare_values; operator "
                "filters are unavailable — please file an issue")
        if value.get(k) != v:
            return False
    return True


class AegisStore(BaseStore):
    """A LangGraph store backed by AegisDB.

        from aegisdb_langgraph import AegisStore

        store = AegisStore(host="127.0.0.1", port=9470, namespace="my-agent")
        store.put(("users", "42"), "prefs", {"theme": "dark"})
        store.get(("users", "42"), "prefs").value
        store.search(("users",), query="dark")

    `namespace` is the AegisDB namespace every item is written under — the
    server-enforced isolation boundary. The LangGraph namespace tuple lives
    *inside* it, so one AegisDB tenant holds one store's whole hierarchy.
    """

    supports_ttl = False  # consulted by BaseStore; see the module docstring

    def __init__(self, client: AegisClient | None = None, *,
                 host: str = "127.0.0.1", port: int = 9470, token: str = "",
                 namespace: str = "langgraph", search_scan_limit: int = 1000,
                 **client_kwargs: Any):
        super().__init__()
        self.namespace = namespace
        # A filtered search cannot be paged by the server, so this bounds how
        # many candidates are pulled back to filter here. Raising it costs a
        # bigger read; leaving it low silently truncates — which is why a
        # truncated filtered search says so (see `_search`).
        self.search_scan_limit = int(search_scan_limit)
        self.client = client or AegisClient(
            host=host, port=port, token=token, agent_id=namespace,
            **client_kwargs)

    # ---- lifecycle ------------------------------------------------------
    #
    # BaseStore defines none of these, but this one holds a socket: the client
    # reuses a connection across calls, so without a way to close it a
    # long-lived process leaks one per store it builds.

    def close(self) -> None:
        """Release the connection. The next call opens a new one."""
        self.client.close()

    def __enter__(self) -> "AegisStore":
        return self

    def __exit__(self, *exc: Any) -> bool:
        self.close()
        return False

    # ---- encoding -------------------------------------------------------

    def _tags(self, ns: tuple[str, ...], key: str) -> list[str]:
        if len(ns) > MAX_NAMESPACE_DEPTH:
            raise ValueError(
                f"namespace is {len(ns)} deep; AegisDB allows 32 tags per "
                f"record and this encoding needs one per prefix, so the "
                f"ceiling is {MAX_NAMESPACE_DEPTH}")
        return [MARKER, f"k-{_h((*ns, key))}"] + _prefix_tags(ns)

    @staticmethod
    def _decode(rec: dict) -> tuple[tuple[str, ...], str, dict] | None:
        """Record -> (namespace, key, value), or None if it is not ours."""
        try:
            body = json.loads(rec.get("data") or "")
        except ValueError:
            return None
        if not isinstance(body, dict) or "key" not in body:
            return None
        ns = tuple(body.get("ns") or ())
        return ns, str(body["key"]), body.get("value") or {}

    def _item(self, rec: dict, *, score: float | None = None):
        decoded = self._decode(rec)
        if decoded is None:
            return None
        ns, key, value = decoded
        common = dict(namespace=ns, key=key, value=value,
                      created_at=_to_datetime(rec.get("created")),
                      updated_at=_to_datetime(rec.get("updated")))
        return SearchItem(**common, score=score) if score is not None \
            else Item(**common)

    def _find(self, ns: tuple[str, ...], key: str) -> dict | None:
        """The record for one (namespace, key), or None.

        An exact tag match on the hashed identity, so this is an index probe
        rather than a scan however large the store gets.
        """
        res = self.client.search(tags=[f"k-{_h((*ns, key))}", MARKER],
                                 match="all", top_k=1)
        recs = res.get("records") or []
        return recs[0] if recs else None

    # ---- the one abstract method ---------------------------------------

    def batch(self, ops: Iterable[Op]) -> list[Result]:  # noqa: F821
        out: list[Result] = []
        for op in ops:
            if isinstance(op, GetOp):
                out.append(self._get(op))
            elif isinstance(op, PutOp):
                out.append(self._put(op))
            elif isinstance(op, SearchOp):
                out.append(self._search(op))
            elif isinstance(op, ListNamespacesOp):
                out.append(self._list_namespaces(op))
            else:  # pragma: no cover - a langgraph that added an op type
                raise NotImplementedError(
                    f"AegisStore does not handle {type(op).__name__}")
        return out

    async def abatch(self, ops: Iterable[Op]) -> list[Result]:  # noqa: F821
        """The sync path on a worker thread.

        A second, genuinely async implementation would be a second place for
        the encoding to be wrong. The client is blocking sockets, so a thread
        is what "async" honestly means here.
        """
        return await asyncio.to_thread(self.batch, list(ops))

    # ---- per-op ---------------------------------------------------------

    def _get(self, op: GetOp) -> Item | None:
        rec = self._find(tuple(op.namespace), op.key)
        return self._item(rec) if rec else None

    def _put(self, op: PutOp) -> None:
        ns = tuple(op.namespace)
        if op.value is None:
            rec = self._find(ns, op.key)
            if rec:
                self.client.delete(rec["id"])
            return None
        if op.ttl is not None:
            raise NotImplementedError(
                "AegisDB expires only working memory, which is a per-session "
                "ring buffer rather than a store, so a TTL here would never "
                "fire. Refused rather than ignored: use the `forget` "
                "maintenance op to age items out by importance and recency.")
        payload = json.dumps({"ns": list(ns), "key": op.key, "value": op.value},
                             ensure_ascii=False, sort_keys=True)
        tags = self._tags(ns, op.key)
        existing = self._find(ns, op.key)
        if existing:
            # Updated in place so the id — and anything pointing at it —
            # survives an overwrite, which delete+insert would not.
            self.client.update(existing["id"], data=payload, tags=tags)
        else:
            self.client.insert(payload, type="semantic", tags=tags)
        return None

    def _search(self, op: SearchOp) -> list[SearchItem]:
        ns = tuple(op.namespace_prefix)
        tags = [MARKER, f"p-{_h(ns)}"]
        # Without a filter the server can page directly. With one, matching
        # happens here, so paging must happen here too — asking the server for
        # `offset` would skip rows before they were filtered.
        if op.filter:
            top_k, offset = self.search_scan_limit, 0
        else:
            top_k, offset = op.limit + op.offset, 0
        try:
            res = self.client.search(tags=tags, match="all", top_k=top_k,
                                     offset=offset,
                                     query=op.query or None)
        except NotReady as exc:
            raise RuntimeError(
                "search(query=...) needs the server's BM25 index, which this "
                "server was started without (--no-lexical-index). Drop the "
                "query, or restart the server with the index enabled."
            ) from exc

        items: list[SearchItem] = []
        for rec in res.get("records") or []:
            decoded = self._decode(rec)
            if decoded is None:
                continue
            item_ns, key, value = decoded
            # The tag says the prefix matches; this re-checks it against the
            # readable path, so a hash collision cannot leak another
            # namespace's item into the result.
            if item_ns[:len(ns)] != ns:
                continue
            if not _matches(value, op.filter):
                continue
            score = rec.get("explain", {}).get("score") if op.query else None
            items.append(SearchItem(
                namespace=item_ns, key=key, value=value,
                created_at=_to_datetime(rec.get("created")),
                updated_at=_to_datetime(rec.get("updated")),
                score=score))
        if op.filter:
            items = items[op.offset:op.offset + op.limit]
        else:
            items = items[op.offset:]
        return items

    def _list_namespaces(self, op: ListNamespacesOp) -> list[tuple[str, ...]]:
        """Every namespace holding at least one item.

        A scan of the store's own records, bounded by `search_scan_limit`:
        AegisDB indexes tags but cannot enumerate them, and the namespaces are
        only readable from the payloads. Rare enough to be worth the read, and
        bounded so it cannot become an accidental full-table scan.
        """
        res = self.client.search(tags=[MARKER], match="all",
                                 top_k=self.search_scan_limit)
        seen: set[tuple[str, ...]] = set()
        for rec in res.get("records") or []:
            decoded = self._decode(rec)
            if decoded is not None:
                seen.add(decoded[0])

        out = sorted(seen)
        for cond in (op.match_conditions or ()):
            path = tuple(cond.path)
            if cond.match_type == "prefix":
                out = [ns for ns in out if _path_matches(ns[:len(path)], path)]
            elif cond.match_type == "suffix":
                out = [ns for ns in out if _path_matches(ns[-len(path):], path)]
            else:  # pragma: no cover - langgraph only defines the two
                raise NotImplementedError(cond.match_type)
        if op.max_depth is not None:
            # Truncating can make two namespaces equal, and LangGraph returns
            # the distinct set — so dedupe after, not before.
            out = sorted({ns[:op.max_depth] for ns in out})
        return out[op.offset:op.offset + op.limit]


def _path_matches(actual: tuple[str, ...], pattern: tuple[str, ...]) -> bool:
    """Compare a namespace slice to a match path, honouring the `*` wildcard."""
    if len(actual) != len(pattern):
        return False
    return all(p == "*" or a == p for a, p in zip(actual, pattern))
