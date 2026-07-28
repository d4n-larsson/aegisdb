/* One-shot health probe (see aegisdb/health.h). Sends `{"operation":"ping"}`
 * to a locally running server and checks for an ok response. `ping` is exempt
 * from authentication, so this works regardless of token configuration. */
#include "aegisdb/health.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aegisdb/netio.h"

#define HEALTH_TIMEOUT_SEC 2

static const char PING[] = "{\"operation\":\"ping\"}\n";

int health_check(int port) {
    if (port <= 0 || port > 65535)
        return 1;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    int fd = net_dial("127.0.0.1", portstr); /* loopback, numeric — no DNS */
    if (fd < 0)
        return 1;

    /* Bound every blocking call so a hung server can't outlast the container's
     * HEALTHCHECK timeout. */
    net_set_timeouts(fd, HEALTH_TIMEOUT_SEC);

    int rv = 1;
    if (net_write_all(fd, PING, sizeof(PING) - 1) == 0) {
        char buf[256];
        ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
        if (r > 0) {
            buf[r] = '\0';
            if (strstr(buf, "\"ok\":true"))
                rv = 0;
        }
    }

    close(fd);
    return rv;
}