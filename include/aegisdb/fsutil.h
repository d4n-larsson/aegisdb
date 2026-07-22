/* Filesystem helpers shared across storage modules. */
#ifndef AEGISDB_FSUTIL_H
#define AEGISDB_FSUTIL_H

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

#endif /* AEGISDB_FSUTIL_H */