/* JSON request parsing and the per-connection request entry point (T015). */
#include "aegisdb/json_request.h"

#include <stdlib.h>
#include <string.h>

#include "aegisdb/json_response.h"
#include "aegisdb/query_engine.h"

const char *jr_str(const cJSON *o, const char *key, const char *dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    return dflt;
}

int jr_u64(const cJSON *o, const char *key, uint64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) return -1;
    if (v->valuedouble < 0) return -1;
    /* Casting a double >= 2^64 to uint64_t is undefined behaviour and would
     * yield a garbage huge integer; reject it instead. */
    if (v->valuedouble >= 18446744073709551616.0) return -1;
    *out = (uint64_t)v->valuedouble;
    return 0;
}

int jr_f64(const cJSON *o, const char *key, double *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) return -1;
    *out = v->valuedouble;
    return 0;
}

int jr_bool(const cJSON *o, const char *key, int dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    return dflt;
}

int jr_str_array(const cJSON *o, const char *key, const char ***out,
                 size_t *out_n, size_t max) {
    *out = NULL;
    *out_n = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(arr)) return 0;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return 0;
    if (max && (size_t)n > max) return -1; /* reject oversized before allocating */
    const char **vals = malloc((size_t)n * sizeof(char *));
    if (!vals) return -1;
    size_t cnt = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it) && it->valuestring) vals[cnt++] = it->valuestring;
    }
    *out = vals;
    *out_n = cnt;
    return 0;
}

int jr_float_array(const cJSON *o, const char *key, float **out, size_t *out_n,
                   size_t max) {
    *out = NULL;
    *out_n = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(arr)) return 0;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return 0;
    if (max && (size_t)n > max) return -1; /* reject oversized before allocating */
    float *vals = malloc((size_t)n * sizeof(float));
    if (!vals) return -1;
    size_t cnt = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsNumber(it)) vals[cnt++] = (float)it->valuedouble;
    }
    *out = vals;
    *out_n = cnt;
    return 0;
}

char *aegis_request_handle(AegisDB *db, const char *line, size_t len,
                           size_t *out_len) {
    cJSON *req = cJSON_ParseWithLength(line, len);
    if (!req || !cJSON_IsObject(req)) {
        if (req) cJSON_Delete(req);
        return json_finish_line(json_error_status(AEGIS_ERR_INVALID_REQUEST),
                                NULL, out_len);
    }
    const char *request_id = jr_str(req, "request_id", NULL);
    cJSON *resp = query_engine_dispatch(db, req);
    char *out = json_finish_line(resp, request_id, out_len);
    cJSON_Delete(req);
    return out;
}