/* Generates the seed corpus for the fuzz targets. Coverage-guided fuzzing finds
 * bugs far faster from valid seeds than from an empty corpus — especially for
 * the binary record codec, whose framing a fuzzer would take a long time to
 * discover from scratch. Regenerate with `make fuzz-corpus`.
 *
 * Writes valid encoded records to <record_dir>/ and valid NDJSON request lines
 * to <wire_dir>/. Run as: gen_seeds <record_dir> <wire_dir>.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/record.h"

static void write_file(const char *dir, const char *name, const void *buf,
                       size_t len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "gen_seeds: cannot write %s\n", path);
        exit(1);
    }
    fwrite(buf, 1, len, f);
    fclose(f);
}

/* Encode `r` and drop it as <dir>/<name>. */
static void emit_record(const char *dir, const char *name, MemoryRecord *r) {
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (record_encode(r, &enc, &enc_len) != 0) {
        fprintf(stderr, "gen_seeds: encode failed for %s\n", name);
        exit(1);
    }
    write_file(dir, name, enc, enc_len);
    free(enc);
}

static void gen_records(const char *dir) {
    /* Minimal: just an opaque payload. */
    {
        MemoryRecord r;
        record_init(&r);
        r.id = 1;
        r.type = MEM_EPISODIC;
        r.created = r.updated = 1700000000000ULL;
        const char *d = "hello world";
        r.data = strdup(d);
        r.data_len = strlen(d);
        emit_record(dir, "minimal.bin", &r);
        record_free(&r);
    }
    /* With tags + agent_id. */
    {
        MemoryRecord r;
        record_init(&r);
        r.id = 2;
        r.type = MEM_SEMANTIC;
        r.created = r.updated = 1700000000001ULL;
        r.importance = 0.5f;
        r.agent_id = strdup("agent-1");
        const char *tags[] = {"user", "preference"};
        record_set_tags(&r, tags, 2);
        const char *d = "{\"k\":\"v\"}";
        r.data = strdup(d);
        r.data_len = strlen(d);
        emit_record(dir, "tags.bin", &r);
        record_free(&r);
    }
    /* With an embedding (single vector). */
    {
        MemoryRecord r;
        record_init(&r);
        r.id = 3;
        r.type = MEM_SEMANTIC;
        r.created = r.updated = 1700000000002ULL;
        size_t dim = 8;
        r.embedding = malloc(dim * sizeof(float));
        for (size_t i = 0; i < dim; i++)
            r.embedding[i] = (float)i / (float)dim;
        r.embedding_dim = dim;
        r.vec_count = 1;
        const char *d = "vectorized";
        r.data = strdup(d);
        r.data_len = strlen(d);
        emit_record(dir, "embedding.bin", &r);
        record_free(&r);
    }
    /* With relationships. */
    {
        MemoryRecord r;
        record_init(&r);
        r.id = 4;
        r.type = MEM_SEMANTIC;
        r.created = r.updated = 1700000000003ULL;
        record_add_relationship(&r, 4, 1, "derived_from");
        record_add_relationship(&r, 4, 2, "relates_to");
        const char *d = "linked";
        r.data = strdup(d);
        r.data_len = strlen(d);
        emit_record(dir, "relationships.bin", &r);
        record_free(&r);
    }
}

static void gen_wire(const char *dir) {
    static const struct {
        const char *name;
        const char *line;
    } lines[] = {
        {"ping.json", "{\"operation\":\"ping\"}"},
        {"insert.json",
         "{\"operation\":\"insert\",\"type\":\"episodic\","
         "\"tags\":[\"user\",\"preference\"],\"data\":\"hello\"}"},
        {"insert_embedding.json",
         "{\"operation\":\"insert\",\"type\":\"semantic\",\"data\":\"v\","
         "\"embedding\":[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8]}"},
        {"get.json", "{\"operation\":\"get\",\"id\":1}"},
        {"search.json",
         "{\"operation\":\"search\",\"tags\":[\"user\"],\"top_k\":5}"},
        {"count.json", "{\"operation\":\"count\"}"},
        {"stats.json", "{\"operation\":\"stats\"}"},
        {"relate.json",
         "{\"operation\":\"relate\",\"from_id\":1,\"to_id\":2,"
         "\"kind\":\"relates_to\"}"},
        {"traverse.json", "{\"operation\":\"traverse\",\"id\":1,\"depth\":2}"},
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        write_file(dir, lines[i].name, lines[i].line, strlen(lines[i].line));
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <record_dir> <wire_dir>\n", argv[0]);
        return 2;
    }
    gen_records(argv[1]);
    gen_wire(argv[2]);
    return 0;
}