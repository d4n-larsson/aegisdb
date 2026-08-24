/* Predicate registry: parse and validate the declared fact vocabulary. */
#include "aegisdb/predicate_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/fact_index.h"
#include "aegisdb/fsutil.h"
#include "cJSON.h"

/* A registry file is authored by hand and read once; this bound exists so a
 * malformed or hostile path cannot be slurped without limit. */
#define REGISTRY_MAX_BYTES (1u << 20)

/* The registry stores PredicateSpec directly and owns the strings it points at,
 * freeing them through a const cast. The alternative — a private Entry plus a
 * spec built on demand — needs somewhere to put the returned view, and the
 * obvious somewhere (a static) silently aliases when a caller holds two specs
 * at once. Storing the public shape means `get` returns a stable pointer into
 * an immutable table, which is what the header promises. */
struct PredicateRegistry {
    PredicateSpec *specs; /* sorted by name, so lookup is a binary search */
    size_t n;
};

static void spec_free(PredicateSpec *s) {
    free((char *)s->name);
    free((char *)s->inverse_of);
    if (s->mutex_with) {
        for (size_t i = 0; i < s->mutex_count; i++) {
            free((char *)s->mutex_with[i]);
        }
        free((char **)s->mutex_with);
    }
}

void predicate_registry_free(PredicateRegistry *r) {
    if (!r) {
        return;
    }
    for (size_t i = 0; i < r->n; i++) {
        spec_free(&r->specs[i]);
    }
    free(r->specs);
    free(r);
}

size_t predicate_registry_count(const PredicateRegistry *r) {
    return r ? r->n : 0;
}

static int spec_cmp(const void *a, const void *b) {
    return strcmp(((const PredicateSpec *)a)->name,
                  ((const PredicateSpec *)b)->name);
}

const PredicateSpec *predicate_registry_at(const PredicateRegistry *r,
                                           size_t i) {
    if (!r || i >= r->n) {
        return NULL;
    }
    return &r->specs[i];
}

const PredicateSpec *predicate_registry_get(const PredicateRegistry *r,
                                            const char *predicate) {
    if (!r || !predicate) {
        return NULL;
    }
    size_t lo = 0;
    size_t hi = r->n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        int c = strcmp(r->specs[mid].name, predicate);
        if (c < 0) {
            lo = mid + 1;
        } else if (c > 0) {
            hi = mid;
        } else {
            return &r->specs[mid];
        }
    }
    return NULL;
}

int predicate_registry_check(const PredicateRegistry *r, const char *predicate,
                             FactKind okind, char *err, size_t errlen) {
    if (!r) {
        return 0; /* no vocabulary configured: anything goes */
    }
    const PredicateSpec *spec = predicate_registry_get(r, predicate);
    if (!spec) {
        snprintf(err, errlen, "predicate '%s' is not in the registry",
                 predicate ? predicate : "");
        return -1;
    }
    PredObjKind want = (okind == FACT_OBJ_ID) ? PRED_OBJ_ID : PRED_OBJ_STRING;
    if (spec->object != want) {
        snprintf(err, errlen, "predicate '%s' declares an object of type %s",
                 predicate, spec->object == PRED_OBJ_ID ? "id" : "string");
        return -1;
    }
    return 0;
}

/* ----- parsing ----------------------------------------------------------- */

/* Only these keys may appear in a predicate's declaration. An unknown key is a
 * typo, and a typo in a vocabulary file would otherwise be a property that
 * silently does not apply. */
static int known_key(const char *k) {
    static const char *const KEYS[] = {"object",     "cardinality",
                                       "symmetric",  "transitive",
                                       "inverse_of", "mutex_with"};
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (strcmp(k, KEYS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_bool(const cJSON *v, const char *pred, const char *key,
                      int *out, char *err, size_t errlen) {
    if (!v) {
        *out = 0;
        return 0;
    }
    if (!cJSON_IsBool(v)) {
        snprintf(err, errlen, "predicate '%s': %s must be true or false", pred,
                 key);
        return -1;
    }
    *out = cJSON_IsTrue(v);
    return 0;
}

static int parse_spec(const cJSON *item, PredicateSpec *e, char *err,
                      size_t errlen) {
    const char *pred = item->string;
    if (!pred || !*pred) {
        snprintf(err, errlen, "a predicate name must be a non-empty string");
        return -1;
    }
    if (strlen(pred) > FACT_MAX_PREDICATE_LEN) {
        /* It could never be indexed, so every fact using it would be refused at
         * write time. Failing startup names the culprit instead. */
        snprintf(err, errlen, "predicate '%s' is longer than the %d-byte limit",
                 pred, FACT_MAX_PREDICATE_LEN);
        return -1;
    }
    if (!cJSON_IsObject(item)) {
        snprintf(err, errlen, "predicate '%s': declaration must be an object",
                 pred);
        return -1;
    }
    for (const cJSON *k = item->child; k; k = k->next) {
        if (!k->string || !known_key(k->string)) {
            snprintf(err, errlen, "predicate '%s': unknown key '%s'", pred,
                     k->string ? k->string : "");
            return -1;
        }
    }

    const cJSON *jobj = cJSON_GetObjectItemCaseSensitive(item, "object");
    if (!cJSON_IsString(jobj) || !jobj->valuestring) {
        snprintf(err, errlen,
                 "predicate '%s': \"object\" is required and must be "
                 "\"id\" or \"string\"",
                 pred);
        return -1;
    }
    if (strcmp(jobj->valuestring, "id") == 0) {
        e->object = PRED_OBJ_ID;
    } else if (strcmp(jobj->valuestring, "string") == 0) {
        e->object = PRED_OBJ_STRING;
    } else {
        snprintf(err, errlen,
                 "predicate '%s': \"object\" must be \"id\" or \"string\", "
                 "not \"%s\"",
                 pred, jobj->valuestring);
        return -1;
    }

    const cJSON *jcard = cJSON_GetObjectItemCaseSensitive(item, "cardinality");
    if (jcard) {
        if (!cJSON_IsString(jcard) || !jcard->valuestring) {
            snprintf(err, errlen,
                     "predicate '%s': cardinality must be a string", pred);
            return -1;
        }
        if (strcmp(jcard->valuestring, "one") == 0) {
            e->single_valued = 1;
        } else if (strcmp(jcard->valuestring, "many") != 0) {
            snprintf(err, errlen,
                     "predicate '%s': cardinality must be \"one\" or \"many\"",
                     pred);
            return -1;
        }
    }

    if (parse_bool(cJSON_GetObjectItemCaseSensitive(item, "symmetric"), pred,
                   "symmetric", &e->symmetric, err, errlen) != 0) {
        return -1;
    }
    if (parse_bool(cJSON_GetObjectItemCaseSensitive(item, "transitive"), pred,
                   "transitive", &e->transitive, err, errlen) != 0) {
        return -1;
    }
    /* Symmetry and transitivity relate a subject to a subject, and subjects are
     * records — so a literal-valued predicate cannot have either. Catching this
     * here turns a rule that could never fire into a startup error. */
    if ((e->symmetric || e->transitive) && e->object != PRED_OBJ_ID) {
        snprintf(err, errlen,
                 "predicate '%s': symmetric/transitive require an \"id\" "
                 "object, since both relate one record to another",
                 pred);
        return -1;
    }

    const cJSON *jinv = cJSON_GetObjectItemCaseSensitive(item, "inverse_of");
    if (jinv) {
        if (!cJSON_IsString(jinv) || !jinv->valuestring ||
            !*jinv->valuestring) {
            snprintf(err, errlen,
                     "predicate '%s': inverse_of must be a predicate name",
                     pred);
            return -1;
        }
        if (strcmp(jinv->valuestring, pred) == 0) {
            snprintf(err, errlen, "predicate '%s': inverse_of itself", pred);
            return -1;
        }
        e->inverse_of = strdup(jinv->valuestring);
        if (!e->inverse_of) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
    }

    const cJSON *jmx = cJSON_GetObjectItemCaseSensitive(item, "mutex_with");
    if (jmx) {
        if (!cJSON_IsArray(jmx)) {
            snprintf(err, errlen, "predicate '%s': mutex_with must be an array",
                     pred);
            return -1;
        }
        int n = cJSON_GetArraySize(jmx);
        if (n > 0) {
            e->mutex_with = calloc((size_t)n, sizeof(char *));
            if (!e->mutex_with) {
                snprintf(err, errlen, "out of memory");
                return -1;
            }
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, jmx) {
                if (!cJSON_IsString(it) || !it->valuestring) {
                    snprintf(err, errlen,
                             "predicate '%s': mutex_with entries must be "
                             "predicate names",
                             pred);
                    return -1;
                }
                ((char **)e->mutex_with)[e->mutex_count] =
                    strdup(it->valuestring);
                if (!e->mutex_with[e->mutex_count]) {
                    snprintf(err, errlen, "out of memory");
                    return -1;
                }
                e->mutex_count++;
            }
        }
    }

    e->name = strdup(pred);
    if (!e->name) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    return 0;
}

/* Cross-entry checks, once every name is known. */
static int validate_refs(const PredicateRegistry *r, char *err, size_t errlen) {
    for (size_t i = 0; i < r->n; i++) {
        const PredicateSpec *e = &r->specs[i];
        if (e->inverse_of) {
            const PredicateSpec *other =
                predicate_registry_get(r, e->inverse_of);
            if (!other) {
                snprintf(err, errlen,
                         "predicate '%s': inverse_of names '%s', which is not "
                         "declared",
                         e->name, e->inverse_of);
                return -1;
            }
            /* One-sided inverses are the failure this catches: `part_of`
             * declaring `contains` while `contains` says nothing back would let
             * 5.3 derive in one direction only, which reads as a bug in the
             * reasoner rather than a typo in the file. */
            if (!other->inverse_of || strcmp(other->inverse_of, e->name) != 0) {
                snprintf(err, errlen,
                         "predicate '%s': inverse_of '%s' is not mutual ('%s' "
                         "must declare inverse_of '%s')",
                         e->name, e->inverse_of, e->inverse_of, e->name);
                return -1;
            }
            if (other->object != PRED_OBJ_ID || e->object != PRED_OBJ_ID) {
                snprintf(err, errlen,
                         "predicate '%s': an inverse pair must both take an "
                         "\"id\" object",
                         e->name);
                return -1;
            }
        }
        for (size_t m = 0; m < e->mutex_count; m++) {
            if (!predicate_registry_get(r, e->mutex_with[m])) {
                snprintf(err, errlen,
                         "predicate '%s': mutex_with names '%s', which is not "
                         "declared",
                         e->name, e->mutex_with[m]);
                return -1;
            }
        }
    }
    return 0;
}

PredicateRegistry *predicate_registry_load(const char *path, char *err,
                                           size_t errlen) {
    if (errlen) {
        err[0] = '\0';
    }
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        snprintf(err, errlen, "cannot open '%s'", path);
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        snprintf(err, errlen, "cannot size '%s'", path);
        return NULL;
    }
    long sz = ftell(fh);
    if (sz < 0 || (unsigned long)sz > REGISTRY_MAX_BYTES) {
        fclose(fh);
        snprintf(err, errlen, "'%s' is empty or larger than %u bytes", path,
                 REGISTRY_MAX_BYTES);
        return NULL;
    }
    rewind(fh);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fh);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, fh);
    fclose(fh);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(err, errlen, "'%s' is not valid JSON", path);
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, errlen,
                 "'%s' must be a JSON object mapping predicate names to "
                 "declarations",
                 path);
        return NULL;
    }
    int n = 0;
    for (const cJSON *it = root->child; it; it = it->next) {
        n++;
    }
    if (n == 0) {
        cJSON_Delete(root);
        snprintf(err, errlen, "'%s' declares no predicates", path);
        return NULL;
    }
    if ((size_t)n > FACT_MAX_PREDICATES) {
        cJSON_Delete(root);
        snprintf(err, errlen, "'%s' declares %d predicates; the limit is %d",
                 path, n, FACT_MAX_PREDICATES);
        return NULL;
    }

    PredicateRegistry *r = calloc(1, sizeof(*r));
    if (!r) {
        cJSON_Delete(root);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    r->specs = calloc((size_t)n, sizeof(PredicateSpec));
    if (!r->specs) {
        cJSON_Delete(root);
        free(r);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    for (const cJSON *it = root->child; it; it = it->next) {
        if (parse_spec(it, &r->specs[r->n], err, errlen) != 0) {
            spec_free(&r->specs[r->n]); /* partials from a failed parse */
            cJSON_Delete(root);
            predicate_registry_free(r);
            return NULL;
        }
        r->n++;
    }
    cJSON_Delete(root);

    qsort(r->specs, r->n, sizeof(PredicateSpec), spec_cmp);
    /* cJSON keeps duplicate keys, so a file naming the same predicate twice
     * would otherwise load with one of them silently winning. */
    for (size_t i = 1; i < r->n; i++) {
        if (strcmp(r->specs[i - 1].name, r->specs[i].name) == 0) {
            snprintf(err, errlen, "predicate '%s' is declared twice",
                     r->specs[i].name);
            predicate_registry_free(r);
            return NULL;
        }
    }
    if (validate_refs(r, err, errlen) != 0) {
        predicate_registry_free(r);
        return NULL;
    }
    return r;
}
