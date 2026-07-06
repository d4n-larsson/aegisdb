/* Filesystem helpers shared across storage modules. */
#ifndef AEGISDB_FSUTIL_H
#define AEGISDB_FSUTIL_H

/* Recursively create `path` and any missing parent directories (like
 * `mkdir -p`), mode 0755. An already-existing directory is not an error.
 * Returns 0 on success, -1 on failure (errno set). */
int fs_mkdir_p(const char *path);

#endif /* AEGISDB_FSUTIL_H */