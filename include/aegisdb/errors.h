/* AegisDB error codes and result types (T007).
 *
 * The public status codes mirror the wire-protocol error codes
 * (see contracts/wire-protocol.md). Internal-only failures collapse to
 * AEGIS_ERR_INTERNAL on the wire. */
#ifndef AEGISDB_ERRORS_H
#define AEGISDB_ERRORS_H

typedef enum {
    AEGIS_OK = 0,
    AEGIS_ERR_INVALID_REQUEST,   /* malformed / missing fields  -> INVALID_REQUEST */
    AEGIS_ERR_NOT_FOUND,         /* unknown id                  -> NOT_FOUND */
    AEGIS_ERR_PAYLOAD_TOO_LARGE, /* data exceeds limit          -> PAYLOAD_TOO_LARGE */
    AEGIS_ERR_IMMUTABLE,         /* update on episodic record   -> IMMUTABLE */
    AEGIS_ERR_NOT_READY,         /* phase-gated feature off     -> NOT_READY */
    AEGIS_ERR_UNAUTHORIZED,      /* missing/invalid auth token  -> UNAUTHORIZED */
    AEGIS_ERR_FORBIDDEN,         /* authenticated but not allowed -> FORBIDDEN */
    AEGIS_ERR_INTERNAL           /* unexpected failure          -> INTERNAL */
} aegis_status_t;

/* Wire-protocol code string for a status (e.g. "NOT_FOUND"). */
const char *aegis_status_code(aegis_status_t s);

/* Human-readable default message for a status. */
const char *aegis_status_message(aegis_status_t s);

#endif /* AEGISDB_ERRORS_H */