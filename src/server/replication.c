/* Phase 1 read-replica log shipping. See replication.h + docs/read-replica-design.md.
 *
 * Runs on a dedicated TCP port, independent of the main poll() loops: the
 * primary's source has an acceptor thread + one streamer thread per replica; a
 * replica's follower is a single thread. Blocking sockets throughout — replica
 * connections are long-lived and few, so a thread each is simpler than folding
 * a long-lived subscription into the non-blocking request/response server. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* accept4 + SOCK_CLOEXEC (the Makefile also defines it) */
#endif
#include "aegisdb/replication.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aegisdb/endian.h"
#include "aegisdb/log.h"
#include "aegisdb/logging.h"
#include "aegisdb/netio.h"
#include "aegisdb/sha256.h"
#include "cJSON.h"

#define REPL_MAGIC 0xA6E515EDu
#define MSG_FRAME 0u
#define MSG_HEARTBEAT 1u
#define MSG_RESET 2u
#define MSG_HDR 17 /* magic(4) + type(1) + offset(8) + len(4) */
#define POLL_MS 50
#define MAX_PAYLOAD (64u * 1024 * 1024) /* sanity bound on a streamed frame */

/* ----------------------------------------------------------------- util ---- */

/* Constant-time fixed-length byte compare (no early exit) — for secrets. */
static int ct_eq_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* Socket I/O (net_dial/net_write_all/net_read_full/net_read_line/…) is shared
 * with the client and health probe — see aegisdb/netio.h. */

/* ============================================================ SOURCE ======= */

#define MAX_REPLICAS 128
/* Concurrent un-authenticated handshakes. A small cap kept separate from the
 * replica slots so a flood of connections that never (or slowly) authenticate
 * can neither starve real replicas of their 128 slots nor spawn unbounded
 * threads before proving the token. */
#define MAX_PENDING 16
#define HANDSHAKE_TIMEOUT_SEC 5 /* hard bound on the pre-auth handshake read */
#define STREAM_TIMEOUT_SEC 30 /* post-auth recv/send timeout while streaming */
/* The registry holds every live connection fd (pending + established). */
#define REPL_CONN_SLOTS (MAX_REPLICAS + MAX_PENDING)

struct ReplicationSource {
    AegisDB *db;
    int listen_fd;
    uint8_t token_hash[SHA256_DIGEST_LEN]; /* sha256 of the subscribe token */
    pthread_t acceptor;
    atomic_int stop;
    atomic_int
        replicas; /* authenticated, streaming (what replica_count reports) */
    atomic_int pending; /* accepted, not yet authenticated */
    /* Live connection sockets (pending + established), so stop() can wake them
     * (shutdown) and wait for them to exit before the source — and then the log
     * — is freed. */
    pthread_mutex_t lock;
    int conn_fds[REPL_CONN_SLOTS];
    int conn_n;
};

/* Track/untrack a streamer's socket in the source's registry. */
static int src_track(ReplicationSource *s, int fd) {
    int ok = 0;
    pthread_mutex_lock(&s->lock);
    if (s->conn_n < REPL_CONN_SLOTS) {
        s->conn_fds[s->conn_n++] = fd;
        ok = 1;
    }
    pthread_mutex_unlock(&s->lock);
    return ok;
}
static void src_untrack(ReplicationSource *s, int fd) {
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->conn_n; i++)
        if (s->conn_fds[i] == fd) {
            s->conn_fds[i] = s->conn_fds[--s->conn_n];
            break;
        }
    pthread_mutex_unlock(&s->lock);
}

/* Read the log's current end offset race-free. Appends update db->log.size
 * while holding index_lock (write), so read it under index_lock (read) — the
 * log's own mutex does not serialize with index-lock readers. */
static uint64_t log_size_locked(AegisDB *db) {
    pthread_rwlock_rdlock(&db->index_lock);
    uint64_t sz = (uint64_t)db->log.size;
    pthread_rwlock_unlock(&db->index_lock);
    return sz;
}

/* One binary message on the stream. */
static int send_msg(int fd, uint8_t type, uint64_t offset, const uint8_t *pay,
                    uint32_t len) {
    uint8_t h[MSG_HDR];
    aegis_put_u32le(h, REPL_MAGIC);
    h[4] = type;
    aegis_put_u64le(h + 5, offset);
    aegis_put_u32le(h + 13, len);
    if (net_write_all(fd, h, MSG_HDR) != 0)
        return -1;
    if (len && net_write_all(fd, pay, len) != 0)
        return -1;
    return 0;
}

/* Per-connection streamer: handshake, then tail the log frame by frame. */
struct stream_ctx {
    ReplicationSource *s;
    int fd;
};

static void *streamer(void *arg) {
    struct stream_ctx *c = arg;
    ReplicationSource *s = c->s;
    AegisDB *db = s->db;
    int fd = c->fd;
    free(c);
    /* The acceptor tracked this fd and incremented s->pending. The thread stays
     * counted in exactly one of pending/replicas until `done`, and every exit
     * path goes through `done` to untrack + close + decrement, so stop() can
     * reliably wait for all connections before the source is freed. `authed`
     * selects which counter `done` releases. */
    int authed = 0;
    /* Bound the handshake hard: a short per-recv timeout AND an absolute deadline
     * so an un-authenticated peer cannot hold a slot open. */
    net_set_timeouts(fd, HANDSHAKE_TIMEOUT_SEC);

    char line[512];
    if (net_read_line(fd, line, sizeof line,
                      net_mono_ms() + HANDSHAKE_TIMEOUT_SEC * 1000ULL) < 0)
        goto done;
    cJSON *req = cJSON_Parse(line);
    if (!req)
        goto done;
    const cJSON *jtok = cJSON_GetObjectItemCaseSensitive(req, "token");
    const cJSON *joff = cJSON_GetObjectItemCaseSensitive(req, "from_offset");
    const cJSON *jgen = cJSON_GetObjectItemCaseSensitive(req, "generation");
    const cJSON *jfp = cJSON_GetObjectItemCaseSensitive(req, "key_fingerprint");
    const char *tok = cJSON_IsString(jtok) ? jtok->valuestring : "";
    const char *req_fp = cJSON_IsString(jfp) ? jfp->valuestring : "";
    uint64_t from_off = cJSON_IsNumber(joff) ? (uint64_t)joff->valuedouble : 0;
    uint64_t req_gen = cJSON_IsNumber(jgen) ? (uint64_t)jgen->valuedouble : 0;

    /* Constant-time compare of the token's SHA-256 (not the raw bytes): avoids
     * both the byte-by-byte short-circuit of strcmp and leaking the token's
     * length via iteration count. A timing oracle here would otherwise let a
     * network peer recover the token and stream the entire multi-tenant log. */
    uint8_t th[SHA256_DIGEST_LEN];
    sha256(tok, strlen(tok), th);
    if (!ct_eq_bytes(th, s->token_hash, SHA256_DIGEST_LEN)) {
        (void)net_write_str(fd, "{\"ok\":false}\n");
        cJSON_Delete(req);
        LOG_WARN("replication: subscriber rejected (bad token)");
        goto done;
    }
    /* Encryption at rest re-frames the log per mode, and the follower re-appends
     * shipped payloads into its own log at byte-identical offsets — which only
     * stays in lockstep if both sides use the same framing (same key/mode). The
     * replica must therefore be configured with the SAME encryption key; reject a
     * mismatch (which includes a plaintext/encrypted mode difference) up front,
     * comparing non-secret fingerprints ("" when unencrypted). */
    char own_fp[13] = "";
    if (db->config.encryption_enabled)
        config_key_fingerprint(db->config.encryption_key, own_fp);
    if (strcmp(req_fp, own_fp) != 0) {
        (void)net_write_str(
            fd, "{\"ok\":false,\"error\":\"encryption key mismatch\"}\n");
        cJSON_Delete(req);
        LOG_WARN(
            "replication: subscriber rejected (encryption key/mode mismatch)");
        goto done;
    }
    cJSON_Delete(req);

    /* Token verified: only now claim a replica slot (bounded by MAX_REPLICAS).
     * Increment replicas BEFORE dropping pending so the thread is never
     * momentarily uncounted (keeps stop() correct). */
    if (atomic_fetch_add(&s->replicas, 1) >= MAX_REPLICAS) {
        atomic_fetch_sub(&s->replicas, 1);
        (void)net_write_str(fd, "{\"ok\":false,\"error\":\"at capacity\"}\n");
        LOG_WARN("replication: replica rejected (at capacity)");
        goto done; /* still pending-counted; `done` releases it */
    }
    authed = 1;
    atomic_fetch_sub(&s->pending, 1);
    /* Streaming can legitimately block on a slow replica's backpressure, so use
     * the longer post-auth timeout rather than the tight handshake one. */
    net_set_timeouts(fd, STREAM_TIMEOUT_SEC);

    uint64_t cur_gen =
        atomic_load_explicit(&db->log_generation, memory_order_relaxed);
    uint64_t logsz = log_size_locked(db);
    int reset = (req_gen != cur_gen) || (from_off > logsz);
    uint64_t cursor = reset ? 0 : from_off;

    char resp[128];
    int rl = snprintf(resp, sizeof resp,
                      "{\"ok\":true,\"generation\":%llu,\"reset\":%s}\n",
                      (unsigned long long)cur_gen, reset ? "true" : "false");
    if (net_write_all(fd, resp, (size_t)rl) != 0)
        goto done;

    LOG_INFO("replication: replica subscribed from offset %llu (reset=%d)",
             (unsigned long long)cursor, reset);

    /* Frame overhead depends only on the log's (immutable) encryption mode. A
     * compaction swap that could change db->log ends this session via the
     * generation check below, so cache it once under log_lock rather than read
     * db->log.encrypted unlocked while the swap may be memset-ing the struct. */
    pthread_rwlock_rdlock(&db->log_lock);
    uint64_t frame_overhead = (uint64_t)log_frame_overhead(&db->log);
    pthread_rwlock_unlock(&db->log_lock);

    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        /* Early out if the primary compacted (offsets changed); the authoritative
         * re-check happens under log_lock before each read below. */
        if (atomic_load_explicit(&db->log_generation, memory_order_relaxed) !=
            cur_gen) {
            (void)send_msg(fd, MSG_RESET, 0, NULL, 0);
            goto done;
        }
        uint64_t size = log_size_locked(db);
        int sent_any = 0;
        while (cursor < size &&
               !atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            uint8_t *payload = NULL;
            size_t plen = 0;
            /* log_lock excludes compaction's log swap; re-check the generation
             * under it so we never read a stale offset from a swapped log. */
            pthread_rwlock_rdlock(&db->log_lock);
            if (atomic_load_explicit(&db->log_generation,
                                     memory_order_relaxed) != cur_gen) {
                pthread_rwlock_unlock(&db->log_lock);
                (void)send_msg(fd, MSG_RESET, 0, NULL, 0);
                goto done;
            }
            int rc = log_read(&db->log, cursor, &payload, &plen);
            pthread_rwlock_unlock(&db->log_lock);
            if (rc != 0)
                break; /* transient — retry next tick */
            if (send_msg(fd, MSG_FRAME, cursor, payload, (uint32_t)plen) != 0) {
                free(payload);
                goto done;
            }
            free(payload);
            /* Step by the actual on-disk frame size (v2 vs encrypted v3 differ),
             * so tailing stays aligned on an encrypted primary log. */
            cursor += frame_overhead + (uint64_t)plen;
            sent_any = 1;
        }
        /* Heartbeat carries the current log size so an idle replica can measure
         * lag and notice a dead primary. */
        if (send_msg(fd, MSG_HEARTBEAT, size, NULL, 0) != 0)
            goto done;
        if (!sent_any)
            usleep(POLL_MS * 1000);
    }
done:
    src_untrack(s, fd);
    close(fd);
    /* Release whichever counter still holds this thread; stop() waits on both
     * hitting 0. */
    if (authed)
        atomic_fetch_sub(&s->replicas, 1);
    else
        atomic_fetch_sub(&s->pending, 1);
    LOG_INFO("replication: replica stream ended");
    return NULL;
}

/* accept() hit a resource-exhaustion error: the pending connection is not
 * drained, so an immediate retry would spin at 100% CPU until an fd frees. Nap
 * briefly to cap the retry rate and warn at most once a second. */
static void accept_backoff(int err) {
    static _Atomic uint64_t last_warn_ms;
    uint64_t now = net_mono_ms();
    uint64_t prev = atomic_load_explicit(&last_warn_ms, memory_order_relaxed);
    if (now - prev >= 1000 && atomic_compare_exchange_strong_explicit(
                                  &last_warn_ms, &prev, now,
                                  memory_order_relaxed, memory_order_relaxed))
        LOG_WARN("replication: cannot accept replica connections (%s); "
                 "throttling until a file descriptor frees",
                 strerror(err));
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000L},
              NULL);
}

static void *acceptor(void *arg) {
    ReplicationSource *s = arg;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        int fd = accept4(s->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue; /* SO_RCVTIMEO wakeup to re-check stop */
            if (atomic_load_explicit(&s->stop, memory_order_relaxed))
                break;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM)
                accept_backoff(errno); /* fd exhaustion: nap so we don't spin */
            continue;
        }
        if (atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            close(fd);
            break;
        }
        /* Throttle un-authenticated connections: refuse a new one once
         * MAX_PENDING handshakes are already in flight, so a flood can't consume
         * replica slots or threads before proving the token. */
        if (atomic_load_explicit(&s->pending, memory_order_relaxed) >=
            MAX_PENDING) {
            close(fd);
            continue;
        }
        struct stream_ctx *c = malloc(sizeof *c);
        /* Count (as pending) + track before creating the thread so stop() (which
         * joins the acceptor first, then waits for the counts) never races a
         * starting streamer. The streamer flips pending->replicas on auth. */
        if (!c || !src_track(s, fd)) {
            free(c);
            close(fd);
            continue;
        }
        atomic_fetch_add(&s->pending, 1);
        c->s = s;
        c->fd = fd;
        pthread_t t;
        if (pthread_create(&t, NULL, streamer, c) != 0) {
            src_untrack(s, fd);
            atomic_fetch_sub(&s->pending, 1);
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

ReplicationSource *replication_source_start(AegisDB *db, int port,
                                            const char *token) {
    ReplicationSource *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->db = db;
    sha256(token ? token : "", token ? strlen(token) : 0, s->token_hash);
    pthread_mutex_init(&s->lock, NULL);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 16) != 0) {
        LOG_ERROR("replication: cannot listen on port %d: %s", port,
                  strerror(errno));
        close(fd);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    /* Timeout so accept() wakes periodically to observe the stop flag. */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    s->listen_fd = fd;
    if (pthread_create(&s->acceptor, NULL, acceptor, s) != 0) {
        close(fd);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    LOG_INFO("replication: serving read-replica stream on port %d", port);
    return s;
}

void replication_source_stop(ReplicationSource *s) {
    if (!s)
        return;
    atomic_store(&s->stop, 1);
    /* Join the acceptor first: after this no new streamer is created, so the
     * replica count + fd set are stable. */
    pthread_join(s->acceptor, NULL);
    close(s->listen_fd);
    /* Wake every live streamer so its blocking send/recv returns promptly. */
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->conn_n; i++)
        shutdown(s->conn_fds[i], SHUT_RDWR);
    pthread_mutex_unlock(&s->lock);
    /* Wait for all connections — established replicas AND in-flight handshakes —
     * to finish before freeing the source (and, in the caller, before db_close
     * closes the log they read via pread). The shutdown() above wakes any thread
     * blocked in recv/send so this drains promptly. */
    while (atomic_load(&s->replicas) + atomic_load(&s->pending) > 0)
        usleep(1000);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

int replication_source_replica_count(ReplicationSource *s) {
    return s ? atomic_load(&s->replicas) : 0;
}

/* ============================================================ FOLLOWER ===== */

struct ReplicationFollower {
    AegisDB *db;
    char host[256];
    int port;
    char token[128];
    pthread_t thread;
    atomic_int stop;
    atomic_int connected;
    atomic_uint_fast64_t primary_size;
    atomic_uint_fast64_t
        applied; /* local applied offset (published for stats) */
    uint64_t
        gen; /* last generation synced from the primary (follower-thread only) */
};

/* Wipe the replica back to empty (primary compacted / desync). */
static void follower_reset(ReplicationFollower *f) {
    AegisDB *db = f->db;
    pthread_rwlock_wrlock(&db->index_lock);
    pthread_rwlock_wrlock(&db->log_lock);
    if (db_reset_replica(db) != 0)
        LOG_ERROR("replication: replica reset failed");
    pthread_rwlock_unlock(&db->log_lock);
    pthread_rwlock_unlock(&db->index_lock);
    atomic_store(&f->applied, 0);
}

/* One connected session: handshake, then apply the stream until it drops. */
static void follow_session(ReplicationFollower *f) {
    AegisDB *db = f->db;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", f->port);
    int fd = net_dial(f->host, portstr);
    if (fd < 0)
        return;
    /* Short read timeout so a dead primary doesn't stall shutdown for long (the
     * follower thread is joined on stop). Healthy streams send heartbeats every
     * poll cycle, so reads return well within this. */
    net_set_timeouts(fd, 5);

    uint64_t from_off = log_size_locked(db);
    /* Advertise our encryption key fingerprint ("" when unencrypted) so the
     * primary can reject a key/mode mismatch before streaming (see the source
     * side). Both sides must share the key for offsets to stay in lockstep. */
    char own_fp[13] = "";
    if (db->config.encryption_enabled)
        config_key_fingerprint(db->config.encryption_key, own_fp);
    char hs[320];
    int hl =
        snprintf(hs, sizeof hs,
                 "{\"from_offset\":%llu,\"generation\":%llu,\"token\":\"%s\","
                 "\"key_fingerprint\":\"%s\"}\n",
                 (unsigned long long)from_off, (unsigned long long)f->gen,
                 f->token, own_fp);
    if (net_write_all(fd, hs, (size_t)hl) != 0) {
        close(fd);
        return;
    }

    char resp[256];
    if (net_read_line(fd, resp, sizeof resp, 0) < 0) {
        close(fd);
        return;
    }
    cJSON *r = cJSON_Parse(resp);
    if (!r) {
        close(fd);
        return;
    }
    int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"));
    int reset = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "reset"));
    const cJSON *jg = cJSON_GetObjectItemCaseSensitive(r, "generation");
    const cJSON *jerr = cJSON_GetObjectItemCaseSensitive(r, "error");
    char err[64] = "check token";
    if (cJSON_IsString(jerr))
        snprintf(err, sizeof err, "%s", jerr->valuestring);
    if (cJSON_IsNumber(jg))
        f->gen = (uint64_t)jg->valuedouble;
    int rejected = !ok;
    cJSON_Delete(r);
    if (rejected) {
        LOG_ERROR("replication: primary rejected subscription (%s)", err);
        close(fd);
        return;
    }
    if (reset) {
        LOG_INFO("replication: primary requested reset; re-bootstrapping");
        follower_reset(f);
    }
    atomic_store(&f->applied, log_size_locked(db)); /* 0 after a reset */
    atomic_store(&f->connected, 1);
    LOG_INFO("replication: following primary %s:%d from offset %llu", f->host,
             f->port, (unsigned long long)atomic_load(&f->applied));

    uint8_t hdr[MSG_HDR];
    while (!atomic_load_explicit(&f->stop, memory_order_relaxed)) {
        if (net_read_full(fd, hdr, MSG_HDR) != 0)
            break; /* timeout / drop */
        if (aegis_get_u32le(hdr) != REPL_MAGIC) {
            LOG_ERROR("replication: bad stream magic; dropping connection");
            break;
        }
        uint8_t type = hdr[4];
        uint64_t offset = aegis_get_u64le(hdr + 5);
        uint32_t len = aegis_get_u32le(hdr + 13);

        if (type == MSG_HEARTBEAT) {
            atomic_store(&f->primary_size, offset);
            continue;
        }
        if (type == MSG_RESET) {
            LOG_INFO("replication: reset from primary (compaction); "
                     "re-bootstrapping");
            follower_reset(f);
            break; /* reconnect and re-subscribe from 0 */
        }
        if (type != MSG_FRAME || len == 0 || len > MAX_PAYLOAD) {
            LOG_ERROR("replication: malformed stream message; dropping");
            break;
        }
        uint8_t *payload = malloc(len);
        if (!payload)
            break;
        if (net_read_full(fd, payload, len) != 0) {
            free(payload);
            break;
        }

        pthread_rwlock_wrlock(&db->index_lock);
        uint64_t expected = (uint64_t)db->log.size;
        if (offset != expected) {
            pthread_rwlock_unlock(&db->index_lock);
            free(payload);
            LOG_ERROR(
                "replication: offset desync (got %llu, at %llu); reconnecting",
                (unsigned long long)offset, (unsigned long long)expected);
            break;
        }
        uint64_t off = 0;
        int arc = log_append(&db->log, payload, len, &off);
        if (arc == 0)
            arc = db_replica_apply(db, off, payload, len);
        uint64_t new_applied = (uint64_t)db->log.size;
        pthread_rwlock_unlock(&db->index_lock);
        free(payload);
        if (arc != 0) {
            LOG_ERROR("replication: apply failed; reconnecting");
            break;
        }
        atomic_store(&f->applied, new_applied);
        if (new_applied > atomic_load(&f->primary_size))
            atomic_store(&f->primary_size, new_applied);
    }
    atomic_store(&f->connected, 0);
    close(fd);
}

static void *follower_loop(void *arg) {
    ReplicationFollower *f = arg;
    while (!atomic_load_explicit(&f->stop, memory_order_relaxed)) {
        follow_session(f);
        if (atomic_load_explicit(&f->stop, memory_order_relaxed))
            break;
        sleep(1); /* backoff before reconnecting */
    }
    return NULL;
}

ReplicationFollower *replication_follower_start(AegisDB *db, const char *host,
                                                int port, const char *token) {
    ReplicationFollower *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->db = db;
    f->port = port;
    snprintf(f->host, sizeof f->host, "%s", host);
    snprintf(f->token, sizeof f->token, "%s", token ? token : "");
    if (pthread_create(&f->thread, NULL, follower_loop, f) != 0) {
        free(f);
        return NULL;
    }
    LOG_INFO("replication: replica following %s:%d", host, port);
    return f;
}

void replication_follower_stop(ReplicationFollower *f) {
    if (!f)
        return;
    atomic_store(&f->stop, 1);
    pthread_join(f->thread, NULL);
    free(f);
}

void replication_follower_status(ReplicationFollower *f, uint64_t *applied,
                                 uint64_t *primary_size, int *connected) {
    if (!f)
        return;
    if (applied)
        *applied = atomic_load(&f->applied);
    if (primary_size)
        *primary_size = atomic_load(&f->primary_size);
    if (connected)
        *connected = atomic_load(&f->connected);
}