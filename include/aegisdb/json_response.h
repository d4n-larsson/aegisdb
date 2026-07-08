/* JSON response builders (T014). Returns owned cJSON nodes / strings. */
#ifndef AEGISDB_JSON_RESPONSE_H
#define AEGISDB_JSON_RESPONSE_H

#include "aegisdb/errors.h"
#include "aegisdb/record.h"
#include "cJSON.h"

/* Build a JSON object representing a MemoryRecord per the wire protocol. When
 * `include_embeddings` is 0 the (large) embedding/embeddings arrays are omitted;
 * all other fields are unchanged. */
cJSON *json_record(const MemoryRecord *r, int include_embeddings);

/* {"ok":true} base object the caller can extend. */
cJSON *json_ok(void);

/* {"ok":false,"error":{"code":...,"message":...}} */
cJSON *json_error(const char *code, const char *message);
cJSON *json_error_status(aegis_status_t status);

/* Serialize `resp` to a newline-terminated line (malloc'd; caller frees).
 * If request_id is non-NULL it is added as "request_id". Frees `resp`. */
char *json_finish_line(cJSON *resp, const char *request_id, size_t *out_len);

#endif /* AEGISDB_JSON_RESPONSE_H */