/* One-shot health probe (see aegisdb/health.h). Sends `{"operation":"ping"}`
 * to a locally running server and checks for an ok response. `ping` is exempt
 * from authentication, so this works regardless of token configuration. */
#include "aegisdb/health.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HEALTH_TIMEOUT_SEC 2

static const char PING[] = "{\"operation\":\"ping\"}\n";

int health_check(int port) {
    if (port <= 0 || port > 65535) return 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 1;

    /* Bound every blocking call so a hung server can't outlast the container's
     * HEALTHCHECK timeout. */
    struct timeval tv = {.tv_sec = HEALTH_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rv = 1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        ssize_t n = send(fd, PING, sizeof(PING) - 1, 0);
        if (n == (ssize_t)(sizeof(PING) - 1)) {
            char buf[256];
            ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
            if (r > 0) {
                buf[r] = '\0';
                if (strstr(buf, "\"ok\":true")) rv = 0;
            }
        }
    }

    close(fd);
    return rv;
}