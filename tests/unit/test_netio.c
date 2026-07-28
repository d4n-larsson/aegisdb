/* netio I/O primitives over a socketpair. The load-bearing property is that
 * net_read_line reads one byte at a time and does NOT consume bytes past the
 * newline — the replication handshake relies on a binary stream following the
 * line on the same socket. net_dial's connect paths are covered by the health
 * and replication tests. */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aegisdb/netio.h"
#include "unity.h"

static int sv[2];

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
}
void tearDown(void) {
    close(sv[0]);
    close(sv[1]);
}

static void test_write_all_read_full_roundtrip(void) {
    uint8_t out[300], in[300];
    for (int i = 0; i < 300; i++)
        out[i] = (uint8_t)(i * 7 + 1);
    TEST_ASSERT_EQUAL_INT(0, net_write_all(sv[0], out, sizeof out));
    TEST_ASSERT_EQUAL_INT(0, net_read_full(sv[1], in, sizeof in));
    TEST_ASSERT_EQUAL_MEMORY(out, in, sizeof out);
}

static void test_write_str_writes_strlen(void) {
    const char *s = "hello world";
    TEST_ASSERT_EQUAL_INT(0, net_write_str(sv[0], s));
    char in[16] = {0};
    TEST_ASSERT_EQUAL_INT(0, net_read_full(sv[1], in, strlen(s)));
    TEST_ASSERT_EQUAL_STRING(s, in);
}

/* The critical property: reading a line leaves the bytes after '\n' unread. */
static void test_read_line_stops_at_newline(void) {
    const char *msg = "abc\ndef";
    TEST_ASSERT_EQUAL_INT(0, net_write_all(sv[0], msg, strlen(msg)));

    char line[64];
    TEST_ASSERT_EQUAL_INT(3, net_read_line(sv[1], line, sizeof line, 0));
    TEST_ASSERT_EQUAL_STRING("abc", line); /* newline stripped */

    /* The remaining "def" must still be on the socket, untouched. */
    char rest[3];
    TEST_ASSERT_EQUAL_INT(0, net_read_full(sv[1], rest, 3));
    TEST_ASSERT_EQUAL_MEMORY("def", rest, 3);
}

static void test_read_line_truncates_to_cap(void) {
    const char *msg = "abcdef\n";
    TEST_ASSERT_EQUAL_INT(0, net_write_all(sv[0], msg, strlen(msg)));
    char line[4]; /* room for 3 chars + NUL */
    TEST_ASSERT_EQUAL_INT(3, net_read_line(sv[1], line, sizeof line, 0));
    TEST_ASSERT_EQUAL_STRING("abc", line);
}

static void test_read_line_past_deadline_returns_error(void) {
    /* Deadline already elapsed, no data written: returns -1 without blocking. */
    char line[16];
    TEST_ASSERT_EQUAL_INT(
        -1, net_read_line(sv[1], line, sizeof line, net_mono_ms()));
}

static void test_read_full_eof_is_error(void) {
    close(sv[0]); /* peer closed: no bytes will ever arrive */
    uint8_t in[4];
    TEST_ASSERT_EQUAL_INT(-1, net_read_full(sv[1], in, sizeof in));
}

static void test_mono_ms_monotonic(void) {
    uint64_t a = net_mono_ms();
    uint64_t b = net_mono_ms();
    TEST_ASSERT_TRUE(b >= a);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_write_all_read_full_roundtrip);
    RUN_TEST(test_write_str_writes_strlen);
    RUN_TEST(test_read_line_stops_at_newline);
    RUN_TEST(test_read_line_truncates_to_cap);
    RUN_TEST(test_read_line_past_deadline_returns_error);
    RUN_TEST(test_read_full_eof_is_error);
    RUN_TEST(test_mono_ms_monotonic);
    return UNITY_END();
}