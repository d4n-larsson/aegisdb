/* JSON response builders (T014). */
#include "aegisdb/json_response.h"

#include <stdlib.h>
#include <string.h>

#include "aegisdb/types.h"

cJSON *json_record(const MemoryRecord *r) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "id", (double)r->id);
    cJSON_AddStringToObject(o, "type", memory_type_to_string(r->type));
    cJSON_AddNumberToObject(o, "created", (double)r->created);
    cJSON_AddNumberToObject(o, "updated", (double)r->updated);
    cJSON_AddNumberToObject(o, "importance", r->importance);
    cJSON_AddNumberToObject(o, "confidence", r->confidence);
    if (r->agent_id) cJSON_AddStringToObject(o, "agent_id", r->agent_id);

    cJSON *tags = cJSON_AddArrayToObject(o, "tags");
    for (size_t i = 0; i < r->tag_count; i++)
        cJSON_AddItemToArray(tags, cJSON_CreateString(r->tags[i]));

    if (r->embedding_dim && r->vec_count == 1) {
        cJSON *emb = cJSON_AddArrayToObject(o, "embedding");
        for (size_t i = 0; i < r->embedding_dim; i++)
            cJSON_AddItemToArray(emb, cJSON_CreateNumber(r->embedding[i]));
    } else if (r->embedding_dim && r->vec_count > 1) {
        /* multi-vector: echo as an array of arrays (#85) */
        cJSON *embs = cJSON_AddArrayToObject(o, "embeddings");
        for (size_t v = 0; v < r->vec_count; v++) {
            cJSON *vec = cJSON_CreateArray();
            for (size_t i = 0; i < r->embedding_dim; i++)
                cJSON_AddItemToArray(
                    vec, cJSON_CreateNumber(r->embedding[v * r->embedding_dim + i]));
            cJSON_AddItemToArray(embs, vec);
        }
    }

    if (r->rel_count) {
        cJSON *rels = cJSON_AddArrayToObject(o, "relationships");
        for (size_t i = 0; i < r->rel_count; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "from_id",
                                    (double)r->relationships[i].from_id);
            cJSON_AddNumberToObject(e, "to_id",
                                    (double)r->relationships[i].to_id);
            if (r->relationships[i].kind)
                cJSON_AddStringToObject(e, "kind", r->relationships[i].kind);
            cJSON_AddItemToArray(rels, e);
        }
    }

    /* data is sent as a UTF-8 string (wire protocol is JSON/text). */
    char *s = malloc(r->data_len + 1);
    if (s) {
        if (r->data_len) memcpy(s, r->data, r->data_len);
        s[r->data_len] = '\0';
        cJSON_AddStringToObject(o, "data", s);
        free(s);
    }
    return o;
}

cJSON *json_ok(void) {
    cJSON *o = cJSON_CreateObject();
    if (o) cJSON_AddBoolToObject(o, "ok", 1);
    return o;
}

cJSON *json_error(const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON *err = cJSON_AddObjectToObject(o, "error");
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return o;
}

cJSON *json_error_status(aegis_status_t status) {
    return json_error(aegis_status_code(status), aegis_status_message(status));
}

char *json_finish_line(cJSON *resp, const char *request_id, size_t *out_len) {
    if (!resp) return NULL;
    if (request_id) cJSON_AddStringToObject(resp, "request_id", request_id);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) return NULL;
    size_t n = strlen(json);
    char *line = malloc(n + 2);
    if (!line) {
        free(json);
        return NULL;
    }
    memcpy(line, json, n);
    line[n] = '\n';
    line[n + 1] = '\0';
    free(json);
    if (out_len) *out_len = n + 1;
    return line;
}