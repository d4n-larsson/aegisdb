#include "aegisdb/randutil.h"

#include <errno.h>
#include <sys/random.h>

int aegis_fill_random(uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}