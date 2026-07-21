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
    char tmp[1024];
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