# aegisdb-langgraph — AegisDB as a LangGraph store

Use [AegisDB](https://github.com/d4n-larsson/aegisdb) as the long-term memory
behind a LangGraph agent.

```bash
pip install aegisdb-langgraph
```

```python
from aegisdb_langgraph import AegisStore

store = AegisStore(host="127.0.0.1", port=9470, namespace="my-agent")

store.put(("users", "42"), "prefs", {"theme": "dark"})
store.get(("users", "42"), "prefs").value        # {"theme": "dark"}
store.search(("users",), query="dark")           # BM25 over stored values
store.list_namespaces()                          # [("users", "42")]
```

…and inside a graph, the way LangGraph injects it:

```python
graph = builder.compile(store=AegisStore(namespace="my-agent"))

def remember(state, *, store):
    store.put(("users", state["user"]), "last", {"seen": "hello"})
```

`BaseStore` funnels `get` / `put` / `search` / `delete` / `list_namespaces`
through one abstract `batch(ops)`, so that is all this implements — the
concrete methods come from the base class and cannot drift from it. `aput`,
`aget` and friends work too, on a worker thread.

## The mapping

LangGraph addresses items by a hierarchical `namespace: tuple[str, ...]` plus a
`key`, and searches by namespace *prefix*. AegisDB has a flat `agent_id` that is
an isolation boundary rather than a path, so the hierarchy is carried in tags:

| | |
|---|---|
| `agent_id` | the store's own `namespace=` — one tenant, isolated by the server |
| tags | a marker, a hash of `(namespace, key)`, and a hash of **every prefix** of the namespace |
| `data` | JSON: `{"ns": [...], "key": ..., "value": {...}}` |

A tag per prefix is what makes a prefix search an index lookup rather than a
scan. Tags are hashed because AegisDB caps one at 64 bytes (and 32 per record)
while a namespace or key can be longer; the readable copy lives in `data`,
which is also what makes `list_namespaces` possible.

The digest is length-prefixed per segment, so `("a", "bc")` and `("ab", "c")`
cannot collide — joining on a separator would make them the same string, and
two namespaces sharing a tag is a cross-namespace read.

**Namespace depth is capped at 29.** Marker + key + one tag per prefix has to
fit in AegisDB's 32-tag ceiling. Deeper raises `ValueError` naming the limit,
rather than an `INVALID_REQUEST` that says nothing about namespaces.

## Three things it does not do

**TTL is refused, not ignored.** AegisDB expires only working memory, which is
a per-session ring buffer rather than a store, so a TTL here would never fire.
Silently never expiring something a caller asked to expire is a retention
surprise. Use the server's `forget` op, which ages records out by importance
and recency — and which the LangGraph API has no way to express.

**No vector indexing.** `query=` runs the server's BM25 index: no embedding
provider needed, and exact tokens (an error string, a flag, an identifier)
match well, with the server's relevance score carried through to
`SearchItem.score`. `index=` is accepted and ignored, because honouring it
would mean this package owning an embeddings function. A server started with
`--no-lexical-index` raises a `RuntimeError` naming that flag when a query was
actually passed — a queryless search keeps working, and any other `NOT_READY`
is re-raised untouched rather than blamed on an index it never used.

**`filter=` is applied client-side.** AegisDB cannot filter on arbitrary JSON
fields, so candidates are pulled back and matched here — using *LangGraph's
own* `_compare_values`, so the semantics are identical to `InMemoryStore`
rather than a second implementation that drifts. It inherits that function's
limits too: `$eq`, `$ne`, `$gt`, `$gte`, `$lt`, `$lte` are supported and `$in`
is not, in both stores alike.

Because filtering happens here, paging does too — asking the server for an
`offset` would skip rows before they were filtered, so page 2 would silently
omit matches. An **unfiltered** search does page on the server, so a deep page
stays a small read.

`search_scan_limit` (default 1000) bounds how many candidates a filtered search
pulls back; `list_namespaces` is bounded the same way, since AegisDB indexes
tags but cannot enumerate them. It is **clamped to 1000**, because the server
clamps `top_k` there and says nothing — a larger value would read as "scans
more" while changing nothing.

**Past that bound the answer is short and cannot say so.** `search` and
`list_namespaces` return plain lists, with nowhere to put a flag. A store
holding more than `search_scan_limit` items can therefore be missing matches
from a filtered search, or namespaces from a listing. Stated here because it
cannot be signalled there.

## Isolation

`namespace=` is the AegisDB namespace, enforced by the server. Two stores with
different ones cannot see each other's items even though they share a server —
so one AegisDB instance can back many agents, and the LangGraph hierarchy lives
inside each.

Passing your own `client=` makes *its* `agent_id` authoritative, since that is
what the server enforces; giving both and disagreeing raises, rather than
reporting one namespace while writing into another.

## Concurrency

**One store is one connection, and `batch` holds a lock.** The client owns a
socket and is not thread-safe, while `abatch` runs on a worker thread and
LangGraph's sync runner executes a superstep's tasks on a pool — so without
serialising, two nodes touching the store interleave on the same socket and one
reads the other's response. Concurrent nodes therefore queue; for real
parallelism, build a store per worker.

Across *processes* nothing here can help: `put` is a read-modify-write and
AegisDB has no upsert, so two racing writers can leave two records for one key.
Reads pick deterministically (lowest id) so the store keeps answering
consistently if that happens.

## Tests

```bash
make langgraph-test        # from the repo root
```

Every behaviour meant to match the reference implementation is asserted by
running the same case against `InMemoryStore` and comparing — an assertion
written from a reading of the contract would only encode that reading. The
suite also compiles a real graph with `store=` and lets LangGraph inject it,
which is the part that would break if the class satisfied the ABC but not the
runtime's expectations of it.
