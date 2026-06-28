/* Built-in client + token tooling (see aegisdb/client.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aegisdb/client.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aegisdb/sha256.h"
#include "cJSON.h"

/* ----- small helpers ---------------------------------------------------- */

static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xf];
    }
    out[n * 2] = '\0';
}

/* Parse an unsigned 64-bit value; returns 0 on success. */
static int parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!s[0] || (end && *end)) return -1;
    *out = (uint64_t)v;
    return 0;
}

/* Split a comma-separated list into a cJSON string array (NULL if empty). */
static cJSON *csv_to_array(const char *csv) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    const char *p = csv;
    while (*p) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        char *tok = strndup(p, len);
        if (tok && *tok) cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
        free(tok);
        if (!c) break;
        p = c + 1;
    }
    return arr;
}

/* ----- networking ------------------------------------------------------- */

/* Connect to host:port (TCP). Returns a socket fd or -1. */
static int dial(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t put = 0;
    while (put < len) {
        ssize_t w = send(fd, buf + put, len - put, 0);
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

/* Read one newline-terminated response line. Caller frees. */
static char *recv_line(int fd) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *g = realloc(buf, cap);
            if (!g) {
                free(buf);
                return NULL;
            }
            buf = g;
        }
        ssize_t r = recv(fd, buf + len, cap - len - 1, 0);
        if (r < 0) {
            free(buf);
            return NULL;
        }
        if (r == 0) break; /* server closed */
        len += (size_t)r;
        buf[len] = '\0';
        char *nl = memchr(buf, '\n', len);
        if (nl) {
            *nl = '\0';
            break;
        }
    }
    return buf;
}

/* ----- request building ------------------------------------------------- */

static void client_usage(void) {
    fprintf(stderr,
        "Usage: aegisdb client [--host H] [--port P] [--token T] <op> [args]\n"
        "  ping                              liveness check\n"
        "  stats                             server stats (admin/no-auth only)\n"
        "  get <id>                          fetch a record by id\n"
        "  delete <id>                       delete a record by id\n"
        "  put [opts] <data>                 insert a record\n"
        "      --type episodic|semantic|working   (default episodic)\n"
        "      --tags a,b,c   --importance F   --confidence F\n"
        "      --session S    --ttl-ms N          (working memory)\n"
        "  search [opts]                     search records\n"
        "      --type T  --tags a,b,c  --match all|any  --top-k N\n"
        "      --start MS  --end MS\n"
        "\n"
        "Host/port/token default to $AEGIS_HOST / $AEGIS_PORT / $AEGIS_TOKEN\n"
        "(127.0.0.1 / 9470 / none). Exit code is 0 on an ok response.\n");
}

/* Build the request object for `cmd` with its `argc`/`argv` (after the op
 * name). Returns NULL on a usage error. */
static cJSON *build_request(const char *cmd, int argc, char **argv) {
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;

    if (strcmp(cmd, "ping") == 0 || strcmp(cmd, "stats") == 0) {
        cJSON_AddStringToObject(r, "operation", cmd);
        return r;
    }
    if (strcmp(cmd, "get") == 0 || strcmp(cmd, "delete") == 0) {
        uint64_t id;
        if (argc != 1 || parse_u64(argv[0], &id) != 0) goto usage;
        cJSON_AddStringToObject(r, "operation", cmd);
        cJSON_AddNumberToObject(r, "id", (double)id);
        return r;
    }
    if (strcmp(cmd, "put") == 0) {
        const char *type = "episodic", *tags = NULL, *session = NULL;
        const char *data = NULL;
        double imp = 0, conf = 0;
        int has_imp = 0, has_conf = 0;
        uint64_t ttl = 0;
        int has_ttl = 0;
        for (int i = 0; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "--type") && i + 1 < argc) type = argv[++i];
            else if (!strcmp(a, "--tags") && i + 1 < argc) tags = argv[++i];
            else if (!strcmp(a, "--session") && i + 1 < argc) session = argv[++i];
            else if (!strcmp(a, "--importance") && i + 1 < argc) { imp = atof(argv[++i]); has_imp = 1; }
            else if (!strcmp(a, "--confidence") && i + 1 < argc) { conf = atof(argv[++i]); has_conf = 1; }
            else if (!strcmp(a, "--ttl-ms") && i + 1 < argc) { if (parse_u64(argv[++i], &ttl)) goto usage; has_ttl = 1; }
            else if (a[0] != '-' && !data) data = a;
            else goto usage;
        }
        if (!data) goto usage;
        cJSON_AddStringToObject(r, "operation", "insert");
        cJSON_AddStringToObject(r, "type", type);
        cJSON_AddStringToObject(r, "data", data);
        if (tags) cJSON_AddItemToObject(r, "tags", csv_to_array(tags));
        if (session) cJSON_AddStringToObject(r, "session_id", session);
        if (has_imp) cJSON_AddNumberToObject(r, "importance", imp);
        if (has_conf) cJSON_AddNumberToObject(r, "confidence", conf);
        if (has_ttl) cJSON_AddNumberToObject(r, "ttl_ms", (double)ttl);
        return r;
    }
    if (strcmp(cmd, "search") == 0) {
        const char *type = NULL, *tags = NULL, *match = NULL;
        uint64_t top_k = 0, start = 0, end = 0;
        int has_top = 0, has_start = 0, has_end = 0;
        for (int i = 0; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "--type") && i + 1 < argc) type = argv[++i];
            else if (!strcmp(a, "--tags") && i + 1 < argc) tags = argv[++i];
            else if (!strcmp(a, "--match") && i + 1 < argc) match = argv[++i];
            else if (!strcmp(a, "--top-k") && i + 1 < argc) { if (parse_u64(argv[++i], &top_k)) goto usage; has_top = 1; }
            else if (!strcmp(a, "--start") && i + 1 < argc) { if (parse_u64(argv[++i], &start)) goto usage; has_start = 1; }
            else if (!strcmp(a, "--end") && i + 1 < argc) { if (parse_u64(argv[++i], &end)) goto usage; has_end = 1; }
            else goto usage;
        }
        cJSON_AddStringToObject(r, "operation", "search");
        if (type) cJSON_AddStringToObject(r, "type", type);
        if (tags) cJSON_AddItemToObject(r, "tags", csv_to_array(tags));
        if (match) cJSON_AddStringToObject(r, "match", match);
        cJSON_AddNumberToObject(r, "top_k", (double)(has_top ? top_k : 10));
        if (has_start) cJSON_AddNumberToObject(r, "start_time", (double)start);
        if (has_end) cJSON_AddNumberToObject(r, "end_time", (double)end);
        return r;
    }

usage:
    cJSON_Delete(r);
    return NULL;
}

int client_main(int argc, char **argv) {
    const char *host = getenv("AEGIS_HOST");
    const char *port = getenv("AEGIS_PORT");
    const char *token = getenv("AEGIS_TOKEN");
    if (!host) host = "127.0.0.1";
    if (!port) port = "9470";

    /* leading global flags, then the subcommand and its args */
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) token = argv[++i];
        else break;
    }
    if (i >= argc) {
        client_usage();
        return 2;
    }
    const char *cmd = argv[i++];
    cJSON *req = build_request(cmd, argc - i, argv + i);
    if (!req) {
        client_usage();
        return 2;
    }
    if (token) cJSON_AddStringToObject(req, "token", token);

    char *line = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!line) return 1;
    size_t n = strlen(line);
    char *framed = malloc(n + 2);
    if (!framed) { free(line); return 1; }
    memcpy(framed, line, n);
    framed[n] = '\n';
    framed[n + 1] = '\0';
    free(line);

    int fd = dial(host, port);
    if (fd < 0) {
        fprintf(stderr, "client: cannot connect to %s:%s\n", host, port);
        free(framed);
        return 1;
    }
    int rc = 1;
    if (send_all(fd, framed, n + 1) == 0) {
        char *resp = recv_line(fd);
        if (resp) {
            cJSON *o = cJSON_Parse(resp);
            char *pretty = o ? cJSON_Print(o) : NULL;
            printf("%s\n", pretty ? pretty : resp);
            cJSON *ok = o ? cJSON_GetObjectItemCaseSensitive(o, "ok") : NULL;
            rc = cJSON_IsTrue(ok) ? 0 : 1;
            free(pretty);
            cJSON_Delete(o);
            free(resp);
        } else {
            fprintf(stderr, "client: no response\n");
        }
    } else {
        fprintf(stderr, "client: send failed\n");
    }
    close(fd);
    free(framed);
    return rc;
}

/* ----- gen-token -------------------------------------------------------- */

static void gen_token_usage(void) {
    fprintf(stderr,
        "Usage: aegisdb gen-token [--namespace NS] [--scope ro|rw|admin] "
        "[--token SECRET]\n"
        "  Prints a token-file line and the plaintext token. With no\n"
        "  --namespace the token is a global admin token; ro/rw require one.\n"
        "  A random token is generated unless --token is given.\n");
}

/* Fill `out` (>= 2n+1 bytes) with a random hex token of n bytes. Returns 0. */
static int random_hex(char *out, size_t nbytes) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    uint8_t buf[64];
    if (nbytes > sizeof(buf)) nbytes = sizeof(buf);
    int ok = fread(buf, 1, nbytes, f) == nbytes;
    fclose(f);
    if (!ok) return -1;
    hex_encode(buf, nbytes, out);
    return 0;
}

int gen_token_main(int argc, char **argv) {
    const char *ns = NULL, *scope = NULL, *tok = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--namespace") && i + 1 < argc) ns = argv[++i];
        else if (!strcmp(a, "--scope") && i + 1 < argc) scope = argv[++i];
        else if (!strcmp(a, "--token") && i + 1 < argc) tok = argv[++i];
        else {
            gen_token_usage();
            return 2;
        }
    }

    /* resolve scope: ro/rw require a namespace; default rw with one, admin
     * without one. */
    if (scope && strcmp(scope, "ro") && strcmp(scope, "rw") &&
        strcmp(scope, "admin")) {
        gen_token_usage();
        return 2;
    }
    int admin = (!ns && (!scope || !strcmp(scope, "admin"))) ||
                (scope && !strcmp(scope, "admin"));
    if (!admin && !ns) {
        fprintf(stderr, "gen-token: --scope %s requires --namespace\n", scope);
        return 2;
    }
    const char *eff_scope = admin ? NULL : (scope ? scope : "rw");

    char generated[2 * 32 + 1];
    if (!tok) {
        if (random_hex(generated, 32) != 0) {
            fprintf(stderr, "gen-token: cannot read /dev/urandom\n");
            return 1;
        }
        tok = generated;
    }

    uint8_t d[SHA256_DIGEST_LEN];
    sha256(tok, strlen(tok), d);
    char hex[2 * SHA256_DIGEST_LEN + 1];
    hex_encode(d, SHA256_DIGEST_LEN, hex);

    fprintf(stderr,
            "Add the first line to your --auth-token-file. The token is shown "
            "only once;\nthe server keeps only its hash.\n\n");
    if (admin)
        printf("sha256$%s\n", hex);
    else
        printf("sha256$%s %s %s\n", hex, ns, eff_scope);
    printf("token: %s\n", tok);
    return 0;
}