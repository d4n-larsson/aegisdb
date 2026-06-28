/* Unit tests for the one-shot health probe (src/util/health.c).
 *
 * Each test spins a minimal stub listener on an ephemeral loopback port in a
 * background thread, then calls health_check() against it — no full server. */
#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "aegisdb/health.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* What a stub listener should reply with after accepting one connection. */
typedef struct {
    int port;          /* out: the bound port */
    int listen_fd;     /* the listening socket */
    const char *reply; /* response to send, or NULL to accept-and-close */
} StubServer;

/* Bind a loopback socket on an ephemeral port and report it back. */
static int stub_listen(StubServer *s) {
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral */
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;

    socklen_t len = sizeof(addr);
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &len) != 0)
        return -1;
    s->port = ntohs(addr.sin_port);

    return listen(s->listen_fd, 1);
}

/* Accept one client, optionally send a canned reply, then close. */
static void *stub_serve(void *arg) {
    StubServer *s = arg;
    int c = accept(s->listen_fd, NULL, NULL);
    if (c >= 0) {
        char buf[64];
        (void)recv(c, buf, sizeof(buf), 0); /* drain the ping request */
        if (s->reply) (void)send(c, s->reply, strlen(s->reply), 0);
        close(c);
    }
    close(s->listen_fd);
    return NULL;
}

/* A server that answers with a valid ok response is healthy. */
void test_health_check_ok(void) {
    StubServer s = {.reply = "{\"ok\":true,\"version\":\"0.1.0\",\"phase\":4}\n"};
    TEST_ASSERT_EQUAL_INT(0, stub_listen(&s));
    pthread_t t;
    pthread_create(&t, NULL, stub_serve, &s);

    TEST_ASSERT_EQUAL_INT(0, health_check(s.port));

    pthread_join(t, NULL);
}

/* A server that replies without ok:true is unhealthy. */
void test_health_check_bad_reply(void) {
    StubServer s = {.reply = "{\"ok\":false,\"error\":\"BAD\"}\n"};
    TEST_ASSERT_EQUAL_INT(0, stub_listen(&s));
    pthread_t t;
    pthread_create(&t, NULL, stub_serve, &s);

    TEST_ASSERT_NOT_EQUAL(0, health_check(s.port));

    pthread_join(t, NULL);
}

/* Nothing listening on the port → connection refused → unhealthy. */
void test_health_check_refused(void) {
    /* Bind then immediately close to obtain a port that is almost certainly
     * free, so connect() is refused rather than hitting a live service. */
    StubServer s = {.reply = NULL};
    TEST_ASSERT_EQUAL_INT(0, stub_listen(&s));
    int port = s.port;
    close(s.listen_fd);

    TEST_ASSERT_NOT_EQUAL(0, health_check(port));
}

/* An out-of-range port is rejected without touching the network. */
void test_health_check_invalid_port(void) {
    TEST_ASSERT_NOT_EQUAL(0, health_check(0));
    TEST_ASSERT_NOT_EQUAL(0, health_check(70000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_health_check_ok);
    RUN_TEST(test_health_check_bad_reply);
    RUN_TEST(test_health_check_refused);
    RUN_TEST(test_health_check_invalid_port);
    return UNITY_END();
}