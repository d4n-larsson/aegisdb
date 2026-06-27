/* JSON request parsing + connection entry point (T015). */
#ifndef AEGISDB_JSON_REQUEST_H
#define AEGISDB_JSON_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#include "aegisdb/db.h"
#include "cJSON.h"

/* Handle one NDJSON request line and produce one NDJSON response line
 * (malloc'd, newline-terminated; caller frees). Never returns NULL except on
 * allocation failure. *out_len receives the response length. */
char *aegis_request_handle(AegisDB *db, const char *line, size_t len,
                           size_t *out_len);

/* ----- generic cJSON field accessors used by the dispatch handlers ------- */

/* Returns the string value of `key`, or `dflt` if absent/not a string. */
const char *jr_str(const cJSON *o, const char *key, const char *dflt);

/* Reads an unsigned 64-bit number from `key`. Returns 0 found / -1 absent. */
int jr_u64(const cJSON *o, const char *key, uint64_t *out);

/* Reads a double from `key`. Returns 0 found / -1 absent. */
int jr_f64(const cJSON *o, const char *key, double *out);

/* Extract a string array (e.g. tags). Allocates *out (array of char*, each
 * borrowed from the cJSON tree — do not free the strings, only the array).
 * Returns 0/-1; *out_n set. */
int jr_str_array(const cJSON *o, const char *key, const char ***out,
                 size_t *out_n);

/* Extract a float array (e.g. embedding). Allocates *out (free with free()). */
int jr_float_array(const cJSON *o, const char *key, float **out, size_t *out_n);

#endif /* AEGISDB_JSON_REQUEST_H */