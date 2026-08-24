"""Exceptions, one per wire error code (ROADMAP 3.3).

The server answers a refusal as `{"ok": false, "error": {"code", "message"}}`
rather than by closing the connection, so every failure mode here is a *value*
until this module turns it into an exception. That translation is the whole
point of the module: a caller should be able to write `except NotFound` instead
of comparing a string it copied out of the protocol doc, and a new code the
client has never heard of must still arrive as something catchable.

Two families, and the distinction matters for retries:

- `AegisUnavailable` — the request never got an answer. Nothing is known about
  whether the server acted on it.
- `AegisRequestError` — the server answered, and said no. It definitely did not
  act, so a retry without changing the request is pointless.
"""
from __future__ import annotations


class AegisError(Exception):
    """Base for everything this package raises."""


class AegisUnavailable(AegisError):
    """The server could not be reached, or did not answer in time.

    Deliberately *not* a subclass of AegisRequestError: an unanswered request
    may or may not have been applied, which is a different thing to reason
    about than a refusal.
    """


class AegisRequestError(AegisError):
    """The server answered `ok: false`.

    Carries the wire `code` and `message` verbatim, so a caller can handle a
    code this client predates without waiting for a release that names it.
    """

    def __init__(self, code: str, message: str, payload: dict | None = None):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        # The whole response, for the handful of errors that carry more than a
        # message (a quota refusal naming the limit it hit, say).
        self.payload = payload or {}


class InvalidRequest(AegisRequestError):
    """`INVALID_REQUEST` — malformed, or a field the operation will not accept."""


class NotFound(AegisRequestError):
    """`NOT_FOUND` — no such record, or none visible in this namespace."""


class PayloadTooLarge(AegisRequestError):
    """`PAYLOAD_TOO_LARGE` — `data` exceeds the server's `--max-payload`."""


class Immutable(AegisRequestError):
    """`IMMUTABLE` — an episodic record cannot be updated. Insert a new one."""


class NotReady(AegisRequestError):
    """`NOT_READY` — the operation needs something this server has turned off.

    Raised by a `query` under `--no-lexical-index`, a `pattern` under
    `--no-fact-index`, a backward `traverse` under `--no-edge-index`, and by
    `--phase` gating. It says "not on this server", never "not yet" — retrying
    will not help, but the same call against a differently configured server
    will work.
    """


class Unauthorized(AegisRequestError):
    """`UNAUTHORIZED` — missing or wrong token while auth is enabled."""


class Forbidden(AegisRequestError):
    """`FORBIDDEN` — authenticated, but this token's scope or namespace says no."""


class QuotaExceeded(AegisRequestError):
    """`QUOTA_EXCEEDED` — the write would breach the tenant's record/byte cap."""


class RateLimited(AegisRequestError):
    """`RATE_LIMITED` — the tenant is over `--tenant-rate-qps`.

    The one refusal worth retrying, after a wait: the request was rejected for
    its timing rather than its content.
    """


class ReadOnly(AegisRequestError):
    """`READ_ONLY` — a write reached a replica. Send it to the primary."""


class MemoryLimit(AegisRequestError):
    """`MEMORY_LIMIT` — in-RAM index size hit `--max-index-bytes`.

    Inserts are refused; reads, deletes, updates and working-memory inserts are
    not. Free memory or raise the cap.
    """


class InternalError(AegisRequestError):
    """`INTERNAL` — the server hit something it did not expect."""


# Wire code -> exception. A code absent from this table still raises
# AegisRequestError, so a server newer than this client degrades to a less
# specific exception rather than to an unhandled KeyError.
_BY_CODE = {
    "INVALID_REQUEST": InvalidRequest,
    "NOT_FOUND": NotFound,
    "PAYLOAD_TOO_LARGE": PayloadTooLarge,
    "IMMUTABLE": Immutable,
    "NOT_READY": NotReady,
    "UNAUTHORIZED": Unauthorized,
    "FORBIDDEN": Forbidden,
    "QUOTA_EXCEEDED": QuotaExceeded,
    "RATE_LIMITED": RateLimited,
    "READ_ONLY": ReadOnly,
    "MEMORY_LIMIT": MemoryLimit,
    "INTERNAL": InternalError,
}


def from_response(resp: dict) -> AegisRequestError:
    """Build the exception for an `ok: false` response."""
    err = resp.get("error")
    if not isinstance(err, dict):
        # `ok: false` with no error object at all. Reported rather than
        # smoothed over: it means the server and this client disagree about the
        # response envelope, which is worth seeing.
        return AegisRequestError("INTERNAL", "refused with no error object",
                                 resp)
    code = str(err.get("code") or "INTERNAL")
    message = str(err.get("message") or "")
    return _BY_CODE.get(code, AegisRequestError)(code, message, resp)
