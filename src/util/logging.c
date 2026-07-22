/* Diagnostic logging implementation (see <aegisdb/logging.h>). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

/* The threshold is read on every record and written once at startup. A plain
 * int is sufficient: a torn read can at worst mis-filter a single record during
 * the brief startup window, which is harmless. */
static AegisLogLevel g_level = AEGIS_LOG_INFO;

void aegis_log_set_level(AegisLogLevel level) { g_level = level; }

AegisLogLevel aegis_log_get_level(void) { return g_level; }

int aegis_log_level_from_string(const char *s, AegisLogLevel *out) {
    if (!s) return -1;
    if (strcasecmp(s, "error") == 0)
        *out = AEGIS_LOG_ERROR;
    else if (strcasecmp(s, "warn") == 0 || strcasecmp(s, "warning") == 0)
        *out = AEGIS_LOG_WARN;
    else if (strcasecmp(s, "info") == 0)
        *out = AEGIS_LOG_INFO;
    else if (strcasecmp(s, "debug") == 0)
        *out = AEGIS_LOG_DEBUG;
    else
        return -1;
    return 0;
}

const char *aegis_log_level_name(AegisLogLevel level) {
    switch (level) {
    case AEGIS_LOG_ERROR: return "error";
    case AEGIS_LOG_WARN: return "warn";
    case AEGIS_LOG_INFO: return "info";
    case AEGIS_LOG_DEBUG: return "debug";
    }
    return "?";
}

/* Fixed-width upper-case tag so columns line up across records. */
static const char *level_tag(AegisLogLevel level) {
    switch (level) {
    case AEGIS_LOG_ERROR: return "ERROR";
    case AEGIS_LOG_WARN: return "WARN ";
    case AEGIS_LOG_INFO: return "INFO ";
    case AEGIS_LOG_DEBUG: return "DEBUG";
    }
    return "?????";
}

void aegis_log_emit(AegisLogLevel level, const char *fmt, ...) {
    if (level > g_level) return;

    /* Wall-clock timestamp with millisecond precision. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    char ts[32];
    if (localtime_r(&tv.tv_sec, &tmv)) {
        size_t n = strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        snprintf(ts + n, sizeof(ts) - n, ".%03d", (int)(tv.tv_usec / 1000));
    } else {
        snprintf(ts, sizeof(ts), "-");
    }

    /* Format message body separately so the whole record is one fwrite. */
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[2176];
    int len = snprintf(line, sizeof(line), "%s %s [aegisdb] %s\n", ts,
                       level_tag(level), msg);
    if (len < 0) return;
    /* On truncation snprintf returns the would-be length; the buffer holds only
     * sizeof-1 chars + NUL. Clamp to the actual char count so we never write the
     * trailing NUL or read one byte past the buffer. */
    if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
    fwrite(line, 1, (size_t)len, stderr);
    fflush(stderr);
}