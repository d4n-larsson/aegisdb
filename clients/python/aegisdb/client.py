"""A client for AegisDB's newline-delimited JSON protocol (ROADMAP 3.3).

Standard library only, on purpose. AegisDB is a single dependency-free binary
and its wire protocol is one JSON object per line over TCP; a client that
dragged in a dependency tree would be the heaviest thing in the deployment.

**One request at a time.** The server supports pipelining, and this client
deliberately does not use it: it writes one line and reads one line, so a
response is never mistaken for the tail of another. That keeps a reused
connection safe to share across calls without tracking outstanding requests.

**Not thread-safe.** A client owns one socket. Use one per thread, or set
`reuse=False` to get a fresh connection per request.
"""
from __future__ import annotations

import json
import socket

from .errors import AegisUnavailable, from_response

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9470


class _Stale(Exception):
    """A reused connection failed before any of the response arrived.

    Internal. The server reaps connections idle past `--idle-timeout-sec`, so
    this is the ordinary outcome of a client that pauses between calls — not an
    error worth surfacing until a fresh connection has also failed.
    """


class AegisClient:
    """A connection to one AegisDB server.

    ``agent_id`` is the namespace applied to every request that does not name
    its own, mirroring how the server scopes reads and writes.

    **It does not scope everything.** `consolidate`, `forget`, `update`,
    `delete`, `relate` and `promote` are scoped by the *token's* namespace and
    ignore `agent_id` entirely, so with authentication off they act across the
    whole server whatever this is set to. That asymmetry is the server's, not
    this client's; the affected methods say so individually.
    """

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT, *,
                 token: str = "", agent_id: str | None = None,
                 connect_timeout: float = 2.0, read_timeout: float = 10.0,
                 reuse: bool = True, retry_stale: bool = True):
        self.host = host
        self.port = int(port)
        self.token = token or ""
        self.agent_id = agent_id
        self.connect_timeout = float(connect_timeout)
        self.read_timeout = float(read_timeout)
        self.reuse = bool(reuse)
        # Retry once on a fresh connection when a *reused* one failed with no
        # response received. See `request` for why that is safe and where it
        # stops being so.
        self.retry_stale = bool(retry_stale)
        self._sock: socket.socket | None = None

    # ---- connection lifecycle ------------------------------------------

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self) -> None:
        """Drop the connection. The next request opens a new one."""
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _connect(self) -> socket.socket:
        if self._sock is not None:
            return self._sock
        try:
            sock = socket.create_connection((self.host, self.port),
                                            timeout=self.connect_timeout)
        except OSError as exc:
            raise AegisUnavailable(
                f"cannot reach aegisdb at {self.host}:{self.port}: {exc}"
            ) from exc
        if self.reuse:
            self._sock = sock
        return sock

    # ---- the protocol --------------------------------------------------

    def _roundtrip(self, line: bytes, read_timeout: float) -> dict:
        reused = self._sock is not None
        sock = self._connect()
        try:
            sock.settimeout(read_timeout)
            sock.sendall(line)
            buf = bytearray()
            # Read to the first newline and no further. A JSON string cannot
            # contain a raw newline (it is escaped), so the first one is the
            # end of exactly one response — which is what makes a reused
            # connection safe without tracking read offsets.
            while not buf.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        except OSError as exc:
            if reused:
                raise _Stale(str(exc)) from exc
            raise AegisUnavailable(str(exc)) from exc
        finally:
            if not self.reuse:
                try:
                    sock.close()
                except OSError:
                    pass
        if not buf.endswith(b"\n"):
            # Closed mid-response, or with nothing at all.
            if reused and not buf:
                raise _Stale("connection closed before any response")
            raise AegisUnavailable("truncated response")
        try:
            return json.loads(buf.decode("utf-8"))
        except ValueError as exc:
            raise AegisUnavailable(f"malformed response: {exc}") from exc

    def request(self, payload: dict, *, read_timeout: float | None = None,
                raise_on_error: bool = True) -> dict:
        """Send one operation and return the parsed response.

        Raises `AegisUnavailable` if the request went unanswered, and — unless
        `raise_on_error=False` — the specific `AegisRequestError` subclass for
        an `ok: false` answer.

        A *reused* connection that fails with no response received is retried
        once on a fresh one, because the server reaps idle connections and that
        is what a pause between calls looks like. The retry is safe for the
        case it exists for: a reaped connection never delivered the request. It
        is not safe in general — if the server received the request and the
        answer was lost, a retried `insert` writes a second record. Pass
        `retry_stale=False` to the constructor where that matters more than the
        convenience.
        """
        payload = dict(payload)
        if self.token:
            payload.setdefault("token", self.token)
        if self.agent_id is not None:
            payload.setdefault("agent_id", self.agent_id)
        line = (json.dumps(payload) + "\n").encode("utf-8")
        timeout = self.read_timeout if read_timeout is None else read_timeout

        try:
            resp = self._roundtrip(line, timeout)
        except _Stale as stale:
            self.close()
            if not self.retry_stale:
                raise AegisUnavailable(str(stale)) from stale
            try:
                resp = self._roundtrip(line, timeout)
            except _Stale as again:
                self.close()
                raise AegisUnavailable(str(again)) from again

        if raise_on_error and not resp.get("ok"):
            raise from_response(resp)
        return resp

    # ---- operations ----------------------------------------------------
    #
    # One method per wire operation, and each one only accepts fields the
    # server actually reads — the list was taken from the dispatcher rather
    # than from prose, so a typo here is a test failure and not a field the
    # server silently ignores. `**extra` is the escape hatch for a field a
    # newer server understands and this client does not.

    def ping(self) -> dict:
        """Liveness. Exempt from authentication, so it works before a token
        does — which is what makes it usable as a health probe."""
        return self.request({"operation": "ping"})

    def available(self) -> bool:
        """True if the server answered a ping. Never raises."""
        try:
            return bool(self.ping().get("ok"))
        except Exception:
            return False

    def predicates(self) -> dict:
        """The typed-fact vocabulary this server declares (ROADMAP 5.2).

        What a `fact` may be written in, and therefore what `insert` will
        accept. Readable with an ordinary token, because a vocabulary is server
        *configuration* rather than any tenant's data.

        `enforced` distinguishes two states an empty list cannot: with no
        `--predicate-registry` the server accepts **any** predicate, which is
        the opposite of a vocabulary of none. Check it before deciding that
        proposing a fact is pointless.

        Each entry carries only the properties actually declared, plus a
        per-predicate `facts` count for an unrestricted caller — absent for a
        namespaced one, since the fact indexes are server-wide and that count
        would span every tenant.
        """
        resp = self.request({"operation": "predicates"})
        return {"ok": True, "predicates": resp.get("predicates") or [],
                "total": resp.get("total", 0),
                "enforced": bool(resp.get("enforced"))}

    def stats(self) -> dict:
        """Operational snapshot: counts, index bytes, metrics. Admin-only when
        auth is enabled."""
        return self.request({"operation": "stats"})

    def insert(self, data: str, *, type: str = "episodic", tags=None,
               importance: float | None = None, confidence: float | None = None,
               embedding=None, embeddings=None, fact: dict | None = None,
               agent_id: str | None = None, session_id: str | None = None,
               ttl_ms: int | None = None, include_embeddings: bool | None = None,
               **extra) -> dict:
        """Store one record; returns it, including the `id` the server assigned.

        `fact` attaches a typed `{s, p, o}` assertion beside the prose, which a
        `pattern` search can then match. It is validated against the server's
        `--predicate-registry`: an undeclared predicate is refused, not stored.

        The server *requires* `type` and has no default for it; `episodic` is
        this client's, named here so it is not mistaken for the protocol's.
        """
        p = {"operation": "insert", "type": type, "data": data}
        _put(p, tags=tags, importance=importance, confidence=confidence,
             embedding=embedding, embeddings=embeddings, fact=fact,
             agent_id=agent_id, session_id=session_id, ttl_ms=ttl_ms,
             include_embeddings=include_embeddings, **extra)
        return self.request(p)

    def insert_many(self, records: list, *, session_id: str | None = None,
                    ttl_ms: int | None = None, **extra) -> dict:
        """Batch insert. `records` is a list of the same shape `insert` builds.

        One round trip and one response for the whole batch — worth it over a
        loop of `insert` for anything bulk, since each of those is its own
        request/response pair.
        """
        p = {"operation": "insert", "records": list(records)}
        _put(p, session_id=session_id, ttl_ms=ttl_ms, **extra)
        return self.request(p)

    def get(self, id: int, *, as_of: int | None = None,
            agent_id: str | None = None, track_usage: bool | None = None,
            **extra) -> dict:
        """One record by id.

        `as_of` reconstructs the version live at a past epoch-ms instant.
        `track_usage=False` reads without bumping the recall counters `forget`
        scores on — what a browsing UI wants, so that looking at a memory does
        not protect it from decay.
        """
        p = {"operation": "get", "id": int(id)}
        _put(p, as_of=as_of, agent_id=agent_id, track_usage=track_usage, **extra)
        return self.request(p)

    def history(self, id: int, *, agent_id: str | None = None, **extra) -> dict:
        """Every version of a record in causal order, each with its
        `[valid_from, valid_to)` interval and a `deleted` flag.

        Depth is bounded by compaction: this is reconstructed from the live log,
        so a full archival trail needs a snapshot or deferred compaction.
        """
        p = {"operation": "history", "id": int(id)}
        _put(p, agent_id=agent_id, **extra)
        return self.request(p)

    def update(self, id: int, *, data: str | None = None, tags=None,
               importance: float | None = None, confidence: float | None = None,
               fact: dict | None = None, **extra) -> dict:
        """Amend a semantic record. Episodic records are immutable (`Immutable`).

        Scoped by the token's namespace; `agent_id` does not apply.
        """
        p = {"operation": "update", "id": int(id)}
        _put(p, data=data, tags=tags, importance=importance,
             confidence=confidence, fact=fact, **extra)
        return self.request(p)

    def delete(self, id: int | None = None, **extra) -> dict:
        """Tombstone by id, or in bulk by filter (`tags`, `type`, time range).

        A bulk delete deliberately refuses a `pattern`: deleting by the fact a
        record asserts would be a new destructive capability rather than a
        filter. Scoped by the token's namespace; `agent_id` does not apply.
        """
        p = {"operation": "delete"}
        if id is not None:
            p["id"] = int(id)
        _put(p, **extra)
        return self.request(p)

    def search(self, *, query: str | None = None, embedding=None, tags=None,
               match: str | None = None, type: str | None = None,
               pattern: dict | None = None, subsume: bool | None = None,
               start_time: int | None = None, end_time: int | None = None,
               top_k: int | None = None, offset: int | None = None,
               order: str | None = None, min_score: float | None = None,
               max_importance: float | None = None, explain: bool | None = None,
               half_life_ms: int | None = None, agent_id: str | None = None,
               track_usage: bool | None = None,
               include_embeddings: bool | None = None, **extra) -> dict:
        """Recall, by any combination of the retrieval paths.

        `query` is BM25 over the payload and keeps identifiers whole, so
        `--tenant-max-records` is findable by its exact spelling. Pass it with
        `embedding` and the two ranked lists fuse by reciprocal rank. `pattern`
        matches the typed fact a record asserts; `subsume=True` broadens its
        subject through `is_a`. `explain=True` returns the per-hit ranking
        breakdown, and the `derivation` of any record the inference job wrote.
        """
        p = {"operation": "search"}
        _put(p, query=query, embedding=embedding, tags=tags, match=match,
             type=type, pattern=pattern, subsume=subsume,
             start_time=start_time, end_time=end_time, top_k=top_k,
             offset=offset, order=order, min_score=min_score,
             max_importance=max_importance, explain=explain,
             half_life_ms=half_life_ms, agent_id=agent_id,
             track_usage=track_usage, include_embeddings=include_embeddings,
             **extra)
        return self.request(p)

    def count(self, *, tags=None, match: str | None = None,
              type: str | None = None, pattern: dict | None = None,
              subsume: bool | None = None, start_time: int | None = None,
              end_time: int | None = None, max_importance: float | None = None,
              agent_id: str | None = None, **extra) -> dict:
        """How many records match, without returning them. Reports `capped`
        when the answer is over a bounded view rather than exact."""
        p = {"operation": "count"}
        _put(p, tags=tags, match=match, type=type, pattern=pattern,
             subsume=subsume, start_time=start_time, end_time=end_time,
             max_importance=max_importance, agent_id=agent_id, **extra)
        return self.request(p)

    def consolidate(self, *, min_similarity: float | None = None,
                    **extra) -> dict:
        """Merge near-duplicate semantic facts, recording a `supersedes` link
        from the survivor to each record it absorbs — an auditable merge rather
        than silent loss.

        Scoped by the token's namespace; `agent_id` does not apply, so with
        auth off this acts across every namespace.
        """
        p = {"operation": "consolidate"}
        _put(p, min_similarity=min_similarity, **extra)
        return self.request(p)

    def forget(self, *, half_life_ms: int | None = None,
               min_retention: float | None = None, type: str | None = None,
               usage_weight: float | None = None, max_forget: int | None = None,
               dry_run: bool | None = None, **extra) -> dict:
        """Age out low-value records by `importance x recency x use`.

        Defaults to episodic only, so curated semantic facts are protected
        unless you name the type. `dry_run=True` reports what would go without
        touching anything — worth doing first, every time.

        Scoped by the token's namespace; `agent_id` does not apply.
        """
        p = {"operation": "forget"}
        _put(p, half_life_ms=half_life_ms, min_retention=min_retention,
             type=type, usage_weight=usage_weight, max_forget=max_forget,
             dry_run=dry_run, **extra)
        return self.request(p)

    def export(self, *, agent_id: str | None = None, after_id: int | None = None,
               limit: int | None = None, **extra) -> dict:
        """Everything stored about one subject, id-paginated via `after_id`.

        The subject is the token's namespace, or this `agent_id` when the token
        has none. A subjectless export is refused rather than dumping the
        database.
        """
        p = {"operation": "export"}
        _put(p, agent_id=agent_id, after_id=after_id, limit=limit, **extra)
        return self.request(p)

    def purge(self, *, agent_id: str | None = None, compact: bool | None = None,
              dry_run: bool | None = None, **extra) -> dict:
        """Hard-delete a namespace and compact, so the payloads leave the log.

        The compliance answer to "erase everything about X", and irreversible
        once compaction runs. `dry_run=True` previews; `rw` scope required.
        """
        p = {"operation": "purge"}
        _put(p, agent_id=agent_id, compact=compact, dry_run=dry_run, **extra)
        return self.request(p)

    def promote(self, working_id: int, *, to_type: str | None = None,
                session_id: str | None = None, **extra) -> dict:
        """Persist a working-memory entry before its TTL takes it.

        `to_type` is left to the server's default (`episodic`) when omitted —
        this client does not carry a second copy of it, which is the rule
        `_put` explains.
        """
        p = {"operation": "promote", "working_id": int(working_id)}
        _put(p, to_type=to_type)
        _put(p, session_id=session_id, **extra)
        return self.request(p)

    def relate(self, from_id: int, to_id: int, kind: str | None = None,
               **extra) -> dict:
        """Add a directed edge. `kind` is at most 64 bytes — the limit the
        reverse index can intern, past which a filtered backward `traverse`
        would quietly degrade to a candidate list.

        Scoped by the token's namespace; `agent_id` does not apply.
        """
        p = {"operation": "relate", "from_id": int(from_id), "to_id": int(to_id)}
        _put(p, kind=kind, **extra)
        return self.request(p)

    def traverse(self, id: int, *, depth: int | None = None, kinds=None,
                 direction: str | None = None, agent_id: str | None = None,
                 **extra) -> dict:
        """Walk the graph from one record, breadth-first.

        `kinds` follows only those edge kinds; `direction` is `out` (default),
        `in`, or `both` — and the backward directions need the reverse edge
        index, so they answer `NotReady` under `--no-edge-index`. Each hit
        carries a `traversal` object naming the hop that reached it, so a walk
        reads as a path rather than a set.
        """
        p = {"operation": "traverse", "id": int(id)}
        _put(p, depth=depth, kinds=kinds, direction=direction,
             agent_id=agent_id, **extra)
        return self.request(p)

    def conflicts(self, *, limit: int | None = None,
                  agent_id: str | None = None, **extra) -> dict:
        """Contradictions the inference job flagged and refused to settle.

        Empty on a server without `--inference` — nothing has scanned, which is
        an answer rather than an error. `limit=0` counts without listing.
        """
        p = {"operation": "conflicts"}
        _put(p, limit=limit, agent_id=agent_id, **extra)
        return self.request(p)

    def snapshot(self, name: str | None = None, **extra) -> dict:
        """Take an online backup. Admin token with `rw` scope."""
        p = {"operation": "snapshot"}
        _put(p, name=name, **extra)
        return self.request(p)

    def token_list(self, **extra) -> dict:
        """List configured tokens (never the secrets). Admin only."""
        return self.request({"operation": "token_list", **extra})

    def token_add(self, new_token: str, *, namespace: str | None = None,
                  scope: str | None = None, **extra) -> dict:
        """Add a token bound to a namespace and scope (`ro`/`rw`/`admin`)."""
        p = {"operation": "token_add", "new_token": new_token}
        _put(p, namespace=namespace, scope=scope, **extra)
        return self.request(p)

    def token_revoke(self, id: str, **extra) -> dict:
        """Revoke a token by the id `token_list` reports. Admin only.

        The id is a **string** fingerprint, not a number — the server reads it
        with `jr_str`, and coercing it to an int here raised ValueError on
        every real id.
        """
        p = {"operation": "token_revoke", "id": str(id)}
        _put(p, **extra)
        return self.request(p)


def _put(payload: dict, **fields) -> None:
    """Add the fields that were actually given.

    `None` means "not specified" and is dropped, so the *server's* default
    applies rather than one this client invented — the two would drift, and a
    client-side default silently overriding a server-side one is the harder bug
    to see. A caller who genuinely means JSON null can pass it through
    `request` directly.
    """
    for key, value in fields.items():
        if value is not None:
            payload[key] = value
