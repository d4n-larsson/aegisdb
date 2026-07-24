/* Filesystem helpers shared across storage modules. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fs_mkdir_p(const char *path) {
    char tmp[AEGIS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int fs_fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc;
}

int fs_fsync_parent(const char *path) {
    char dir[AEGIS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == dir)
        dir[1] = '\0'; /* parent is the root "/" */
    else if (slash)
        *slash = '\0';
    else
        return fs_fsync_dir("."); /* bare filename -> current directory */
    return fs_fsync_dir(dir);
}

int fs_write_atomic(const char *path, const void *data, size_t len, mode_t mode) {
    char tmp[AEGIS_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1; /* path too long: refuse rather than write a truncated name */
    /* Create the temp file at `mode` from the start (not open-then-chmod, which
     * leaves a umask-dependent window). Clear any stale .tmp first so O_EXCL then
     * refuses to follow a pre-planted symlink. */
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return -1;
    const char *p = data;
    size_t off = 0;
    int ok = 1;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        off += (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    /* Order the rename: without a directory fsync the atomic rename can still be
     * lost on a crash even though the file data was fsync'd above. */
    return fs_fsync_parent(path);
}

int fs_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[AEGIS_IO_BUF_SIZE];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    if (ferror(in)) ok = 0;
    /* Make the copy durable: fsync the file, then its parent directory so the
     * new directory entry survives a crash too. */
    if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0)) ok = 0;
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    if (ok && fs_fsync_parent(dst) != 0) ok = 0;
    if (!ok) unlink(dst);
    return ok ? 0 : -1;
}