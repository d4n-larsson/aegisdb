/* Filesystem helpers shared across storage modules. */
#ifndef AEGISDB_FSUTIL_H
#define AEGISDB_FSUTIL_H

#include <stddef.h>
#include <sys/types.h> /* mode_t */

#include "aegisdb/types.h" /* AEGIS_PATH_MAX for callers' path buffers */

/* Recursively create `path` and any missing parent directories (like
 * `mkdir -p`), mode 0755. An already-existing directory is not an error.
 * Returns 0 on success, -1 on failure (errno set). */
int fs_mkdir_p(const char *path);

/* fsync the directory `dir` so a preceding rename()/create of an entry within
 * it is made durable (the rename is atomic but its durability is not ordered
 * without this). Returns 0 on success, -1 on failure (errno set). */
int fs_fsync_dir(const char *dir);

/* fsync the directory containing `path` (the parent of a file just renamed or
 * created there). Returns 0 on success, -1 on failure (errno set). */
int fs_fsync_parent(const char *path);

/* Durably write `len` bytes of `data` to `path`, atomically: writes a sibling
 * "<path>.tmp" (created with `mode`, no umask/symlink window), fsyncs it,
 * renames it over `path`, then fsyncs the parent directory so the rename itself
 * survives a crash. A partial temp is unlinked on any failure. Returns 0 on
 * success, -1 on failure. */
int fs_write_atomic(const char *path, const void *data, size_t len, mode_t mode);

/* Copy the file at `src` to `dst`, byte for byte, then fsync `dst` and its
 * parent directory so the copy is durable. A partial `dst` is unlinked on
 * failure. Returns 0 on success, -1 on failure. */
int fs_copy_file(const char *src, const char *dst);

#endif /* AEGISDB_FSUTIL_H */