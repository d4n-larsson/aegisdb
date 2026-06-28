/* Status-code and memory-type string helpers (supports T007, T006). */
#include <string.h>

#include "aegisdb/errors.h"
#include "aegisdb/types.h"

const char *aegis_status_code(aegis_status_t s) {
    switch (s) {
        case AEGIS_OK: return "OK";
        case AEGIS_ERR_INVALID_REQUEST: return "INVALID_REQUEST";
        case AEGIS_ERR_NOT_FOUND: return "NOT_FOUND";
        case AEGIS_ERR_PAYLOAD_TOO_LARGE: return "PAYLOAD_TOO_LARGE";
        case AEGIS_ERR_IMMUTABLE: return "IMMUTABLE";
        case AEGIS_ERR_NOT_READY: return "NOT_READY";
        case AEGIS_ERR_UNAUTHORIZED: return "UNAUTHORIZED";
        case AEGIS_ERR_FORBIDDEN: return "FORBIDDEN";
        case AEGIS_ERR_INTERNAL:
        default: return "INTERNAL";
    }
}

const char *aegis_status_message(aegis_status_t s) {
    switch (s) {
        case AEGIS_OK: return "ok";
        case AEGIS_ERR_INVALID_REQUEST: return "malformed or incomplete request";
        case AEGIS_ERR_NOT_FOUND: return "memory not found";
        case AEGIS_ERR_PAYLOAD_TOO_LARGE: return "payload exceeds configured limit";
        case AEGIS_ERR_IMMUTABLE: return "record is immutable";
        case AEGIS_ERR_NOT_READY: return "feature not enabled for current phase";
        case AEGIS_ERR_UNAUTHORIZED: return "authentication required or invalid token";
        case AEGIS_ERR_FORBIDDEN: return "not permitted for this token";
        case AEGIS_ERR_INTERNAL:
        default: return "internal server error";
    }
}

int memory_type_from_string(const char *s, MemoryType *out) {
    if (!s) return -1;
    if (strcmp(s, "working") == 0) { *out = MEM_WORKING; return 0; }
    if (strcmp(s, "episodic") == 0) { *out = MEM_EPISODIC; return 0; }
    if (strcmp(s, "semantic") == 0) { *out = MEM_SEMANTIC; return 0; }
    return -1;
}

const char *memory_type_to_string(MemoryType t) {
    switch (t) {
        case MEM_WORKING: return "working";
        case MEM_EPISODIC: return "episodic";
        case MEM_SEMANTIC: return "semantic";
        default: return "unknown";
    }
}