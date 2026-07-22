/* Tests for the volatile working-memory ring buffer.
 *
 * The store is reached concurrently by every io-thread (MEM_WORKING inserts and
 * promotes run off the index_lock) and by the maintenance thread's sweep, so it
 * carries its own mutex. The stress test (#3 regression) hammers add/get/take/
 * sweep from many threads against a few shared sessions with a tight capacity so
 * evictions and TTL expiry constantly free slots — the window where the
 * unsynchronised version double-freed / corrupted the heap. It is a crash/ASan/
 * TSan test: it must complete without aborting and leave a consistent count. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aegisdb/working_buffer.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- single-threaded functional smoke ---------------------------------- */

static MemoryRecord mk(const char *payload, const char *ns) {
    MemoryRecord r;
    record_init(&r);
    r.type = MEM_WORKING;
    r.data = (void *)payload;
    r.data_len = strlen(payload);
    r.agent_id = (char *)ns; /* borrowed; record_clone deep-copies it */
    return r;
}

static void test_working_add_get_take(void) {
    WorkingStore *ws = working_store_create(4, 1000);
    TEST_ASSERT_NOT_NULL(ws);

    MemoryRecord in = mk("hello", NULL);
    uint64_t id = 0;
    TEST_ASSERT_EQUAL_INT(0, working_store_add(ws, "sess", &in, 100, 0, &id));
    TEST_ASSERT_TRUE(id != 0);

    MemoryRecord *got = working_store_get(ws, "sess", id, 200);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_size_t(5, got->data_len);
    TEST_ASSERT_EQUAL_MEMORY("hello", got->data, 5);
    record_free(got);
    free(got);

    /* expired entries are not returned */
    TEST_ASSERT_NULL(working_store_get(ws, "sess", id, 2000));

    working_store_free(ws);
}

static void test_working_ring_evicts_oldest(void) {
    WorkingStore *ws = working_store_create(2, 100000);
    uint64_t a = 0, b = 0, c = 0;
    MemoryRecord r = mk("x", NULL);
    working_store_add(ws, "s", &r, 1, 0, &a);
    working_store_add(ws, "s", &r, 1, 0, &b);
    working_store_add(ws, "s", &r, 1, 0, &c); /* evicts a */
    TEST_ASSERT_NULL(working_store_get(ws, "s", a, 2));
    MemoryRecord *gb = working_store_get(ws, "s", b, 2);
    MemoryRecord *gc = working_store_get(ws, "s", c, 2);
    TEST_ASSERT_NOT_NULL(gb);
    TEST_ASSERT_NOT_NULL(gc);
    record_free(gb); free(gb);
    record_free(gc); free(gc);
    TEST_ASSERT_EQUAL_size_t(2, working_store_count(ws));
    working_store_free(ws);
}

/* A huge ttl must saturate to "far future", not wrap now+ttl past the epoch and
 * make the just-added entry look already-expired. */
static void test_working_ttl_saturates(void) {
    WorkingStore *ws = working_store_create(4, 1000);
    MemoryRecord in = mk("v", NULL);
    uint64_t id = 0;
    uint64_t now = 1000;
    TEST_ASSERT_EQUAL_INT(0,
        working_store_add(ws, "s", &in, now, UINT64_MAX, &id));
    /* Far in the future relative to `now` — a wrapped expiry would drop it. */
    MemoryRecord *g = working_store_get(ws, "s", id, now + 1000000000ULL);
    TEST_ASSERT_NOT_NULL(g);
    record_free(g);
    free(g);
    working_store_free(ws);
}

/* ---- concurrent stress (#3 regression) --------------------------------- */

#define NTHREADS 8
#define NITERS 20000
#define NSESSIONS 4

static WorkingStore *g_ws;
static atomic_uint_least64_t g_clock; /* logical ms clock, always advancing */

static const char *sess(int i) {
    static const char *s[NSESSIONS] = {"alpha", "beta", "gamma", "delta"};
    return s[i % NSESSIONS];
}

static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    for (int i = 0; i < NITERS; i++) {
        uint64_t now = atomic_fetch_add(&g_clock, 1) + 1;
        const char *s = sess(tid + i);
        MemoryRecord in = mk("payload", (i & 1) ? "ns" : NULL);
        uint64_t id = 0;
        /* short ttl so entries expire and both find_slot and sweep free them */
        if (working_store_add(g_ws, s, &in, now, 8, &id) != 0) continue;

        MemoryRecord *g = working_store_get(g_ws, s, id, now);
        if (g) { record_free(g); free(g); }

        if ((i & 3) == 0) {
            MemoryRecord out;
            if (working_store_take(g_ws, s, id, now, NULL, &out) == 0)
                record_free(&out);
        }
        if ((i & 7) == 0)
            (void)working_store_sweep(g_ws, now); /* races the frees above */
        if ((i & 15) == 0)
            (void)working_store_count(g_ws);
    }
    return NULL;
}

static void test_working_concurrent_stress(void) {
    g_ws = working_store_create(8, 100000);
    TEST_ASSERT_NOT_NULL(g_ws);
    atomic_store(&g_clock, 0);

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        TEST_ASSERT_EQUAL_INT(0,
            pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i));
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

    /* Sweep far in the future: every remaining entry has expired, so the store
     * must drain to zero — a consistent count proves no slots were lost/leaked
     * through the concurrent frees. */
    uint64_t future = atomic_load(&g_clock) + 1000000;
    (void)working_store_sweep(g_ws, future);
    TEST_ASSERT_EQUAL_size_t(0, working_store_count(g_ws));
    working_store_free(g_ws);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_working_add_get_take);
    RUN_TEST(test_working_ring_evicts_oldest);
    RUN_TEST(test_working_ttl_saturates);
    RUN_TEST(test_working_concurrent_stress);
    return UNITY_END();
}