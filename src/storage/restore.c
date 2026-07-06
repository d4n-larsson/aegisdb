/* Snapshot restore: install a db_snapshot() directory into an empty --data-dir. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/restore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aegisdb/fsutil.h"
#include "aegisdb/logging.h"
#include "cJSON.h"

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* -1 if the file is missing or cannot be stat'd. */
static long long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

/* Byte-for-byte copy; unlinks a partial destination on failure. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    if (fflush(out) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    if (!ok) unlink(dst);
    return ok ? 0 : -1;
}

/* Read an entire (small) file into a NUL-terminated heap buffer, or NULL. */
static char *read_text(const char *path) {
    long long sz = file_size(path);
    if (sz < 0 || sz > (1 << 20)) return NULL; /* manifests are tiny */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int restore_run(const Config *cfg) {
    const char *src = cfg->restore_from;
    char man_path[1300], src_log[1300], src_meta[1300];
    snprintf(man_path, sizeof(man_path), "%s/manifest.json", src);
    snprintf(src_log, sizeof(src_log), "%s/memory.log", src);
    snprintf(src_meta, sizeof(src_meta), "%s/metadata.db", src);

    if (!file_exists(src_log)) {
        LOG_ERROR("restore: %s has no memory.log — not a snapshot directory", src);
        return -1;
    }

    /* Validate the manifest: right format, and the embedding dimension the
     * snapshot was taken with must match this server's, or the semantic index
     * would be rebuilt against the wrong dimension. */
    char *text = read_text(man_path);
    if (!text) {
        LOG_ERROR("restore: cannot read manifest %s", man_path);
        return -1;
    }
    cJSON *man = cJSON_Parse(text);
    free(text);
    if (!man) {
        LOG_ERROR("restore: manifest %s is not valid JSON", man_path);
        return -1;
    }
    int rc = -1;
    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(man, "format");
    const cJSON *dim = cJSON_GetObjectItemCaseSensitive(man, "embedding_dim");
    const cJSON *nrec = cJSON_GetObjectItemCaseSensitive(man, "record_count");
    const cJSON *lsize = cJSON_GetObjectItemCaseSensitive(man, "log_size");
    if (!cJSON_IsNumber(fmt) || fmt->valueint != 1) {
        LOG_ERROR("restore: unsupported manifest format");
        goto done;
    }
    if (!cJSON_IsNumber(dim) ||
        (size_t)dim->valuedouble != cfg->embedding_dimensions) {
        LOG_ERROR("restore: snapshot embedding-dim %d does not match --embedding-dim "
                  "%zu; restore with a matching dimension",
                  cJSON_IsNumber(dim) ? dim->valueint : -1,
                  cfg->embedding_dimensions);
        goto done;
    }
    /* The copied log must be exactly the size the manifest claims. */
    if (cJSON_IsNumber(lsize) &&
        (long long)lsize->valuedouble != file_size(src_log)) {
        LOG_ERROR("restore: memory.log size %lld does not match manifest %lld — "
                  "snapshot is incomplete",
                  file_size(src_log), (long long)lsize->valuedouble);
        goto done;
    }

    /* Never clobber a live database. */
    char dst_log[1300], dst_meta[1300];
    snprintf(dst_log, sizeof(dst_log), "%s/memory.log", cfg->data_dir);
    snprintf(dst_meta, sizeof(dst_meta), "%s/metadata.db", cfg->data_dir);
    if (file_exists(dst_log)) {
        LOG_ERROR("restore: %s already contains a database; restore into an empty "
                  "--data-dir", cfg->data_dir);
        goto done;
    }
    if (fs_mkdir_p(cfg->data_dir) != 0) {
        LOG_ERROR("restore: cannot create data dir %s", cfg->data_dir);
        goto done;
    }
    if (copy_file(src_log, dst_log) != 0) {
        LOG_ERROR("restore: failed to copy log into %s", cfg->data_dir);
        goto done;
    }
    /* metadata.db carries the next_id floor; copy it when present. */
    if (file_exists(src_meta) && copy_file(src_meta, dst_meta) != 0) {
        LOG_ERROR("restore: failed to copy metadata into %s", cfg->data_dir);
        unlink(dst_log);
        goto done;
    }

    LOG_INFO("restore: installed snapshot into %s (%d records); start the server "
             "to recover",
             cfg->data_dir, cJSON_IsNumber(nrec) ? nrec->valueint : -1);
    rc = 0;
done:
    cJSON_Delete(man);
    return rc;
}