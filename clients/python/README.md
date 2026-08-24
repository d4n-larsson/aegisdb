# aegisdb — Python client

The client for [AegisDB](https://github.com/d4n-larsson/aegisdb)'s
newline-delimited JSON protocol. **No dependencies** — AegisDB is a single
dependency-free binary and its wire protocol is one JSON object per line over
TCP, so the client that talks to it has no business dragging in a tree.

```bash
pip install aegisdb
```

```python
from aegisdb import AegisClient, NotFound

with AegisClient(host="127.0.0.1", port=9470, token="…") as db:
    rec = db.insert("prefers dark mode", type="semantic", tags=["user"])
    print(db.search(query="dark mode", top_k=5)["records"])
    try:
        db.get(999)
    except NotFound as exc:
        print(exc.code, exc.message)
```

Every wire operation has a method — `insert` / `insert_many`, `get`, `history`,
`update`, `delete`, `search`, `count`, `consolidate`, `forget`, `export`,
`purge`, `promote`, `relate`, `traverse`, `conflicts`, `ping`, `stats`,
`snapshot`, and the token admin trio. Each accepts only the fields the server
actually reads (the list was taken from the dispatcher, not from prose), plus
`**extra` as the escape hatch for a field a newer server understands.

## What it does that a bare socket doesn't

**Errors are exceptions, one class per wire code.** `NotFound`, `Forbidden`,
`NotReady`, `RateLimited`, `MemoryLimit`, and the rest — all under
`AegisRequestError`, which carries `.code` and `.message` verbatim so a code
this client predates still arrives catchable rather than as a string you compare
by hand. `AegisUnavailable` is deliberately *not* one of them: a refusal means
the server did not act, while an unanswered request says nothing either way, and
that difference is what you reason about when deciding whether to retry.

**One connection, reused.** The server supports pipelining and this client
deliberately does not use it: one line out, one line back, so a response is
never mistaken for the tail of another. A reused connection that fails with no
response received is retried once on a fresh one, because the server reaps
connections idle past `--idle-timeout-sec` and that is exactly what a pause
between calls looks like.

That retry is safe for the case it exists for — a reaped connection never
delivered the request. It is not safe in general: if the server received the
request and the answer was lost, a retried `insert` writes a second record.
Pass `retry_stale=False` where that matters more than the convenience, or
`reuse=False` for a fresh connection per request.

**Not thread-safe.** A client owns one socket. Use one per thread, or
`reuse=False`.

**An unspecified argument is omitted, not defaulted.** `None` means "not
specified", so the *server's* default applies rather than a copy of it kept
here — two copies drift, and the client's would silently win. Falsy values are
not treated as absent: `limit=0` is the `conflicts` count-without-listing probe,
and `subsume=False` means something.

## `agent_id` does not scope everything

`AegisClient(agent_id="…")` is applied to every request that does not name its
own, mirroring how the server scopes reads and writes. But `consolidate`,
`forget`, `update`, `delete`, `relate` and `promote` are scoped by the
**token's** namespace and ignore `agent_id` entirely — so with authentication
off they act across the whole server whatever you set it to. That asymmetry is
the server's, not this client's; the affected methods say so in their
docstrings.

## Version

Published from the same `git tag` as the server and the Claude Code
integration, so `aegisdb`, `aegisdb-mcp` and the server binary all carry the
same version.

## Tests

```bash
python3 -m unittest discover -s tests            # from clients/python/
make sdk-test                                    # from the repo root
```

`test_protocol.py` runs against a fake server and needs nothing. `test_live.py`
exercises **every** method against a real `build/aegisdb` and skips when it is
not built — which is the point: the server ignores request fields it does not
recognise, so a misspelled field name here would otherwise succeed and quietly
do the wrong thing. It has already earned its keep, catching `token_revoke`
coercing a string fingerprint to an `int`.
