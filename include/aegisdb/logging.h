/* Diagnostic logging with runtime-selectable severity levels.
 *
 * This is the server's stderr diagnostics channel; it is unrelated to the
 * append-only storage log in <aegisdb/log.h>. Output is line-oriented and
 * thread-safe (each record is formatted into a local buffer and written with a
 * single fwrite, so concurrent worker threads do not interleave). */
#ifndef AEGISDB_LOGGING_H
#define AEGISDB_LOGGING_H

typedef enum {
    AEGIS_LOG_ERROR = 0, /* unrecoverable or request-failing conditions */
    AEGIS_LOG_WARN = 1,  /* surprising but handled conditions */
    AEGIS_LOG_INFO = 2,  /* lifecycle milestones (default) */
    AEGIS_LOG_DEBUG = 3  /* per-request / per-connection detail */
} AegisLogLevel;

/* Set/get the global threshold: records at a level numerically greater than the
 * threshold are dropped. Default is AEGIS_LOG_INFO. Safe to call once at
 * startup before worker threads exist. */
void aegis_log_set_level(AegisLogLevel level);
AegisLogLevel aegis_log_get_level(void);

/* Parse "error"/"warn"/"info"/"debug" (case-insensitive) into *out. Returns 0
 * on success, -1 if the name is unknown. */
int aegis_log_level_from_string(const char *s, AegisLogLevel *out);

/* Lowercase name for a level ("info", ...); "?" for an out-of-range value. */
const char *aegis_log_level_name(AegisLogLevel level);

/* Emit one record if `level` passes the threshold. Prefer the macros below. */
void aegis_log_emit(AegisLogLevel level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOG_ERROR(...) aegis_log_emit(AEGIS_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...) aegis_log_emit(AEGIS_LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...) aegis_log_emit(AEGIS_LOG_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) aegis_log_emit(AEGIS_LOG_DEBUG, __VA_ARGS__)

#endif /* AEGISDB_LOGGING_H */