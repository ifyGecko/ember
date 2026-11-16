/*
 * frontend_net.c - ember's end of the fuzz protocol (docs/fuzz-formats.md "Fuzz
 * protocol").
 *
 * Dials a fuzz brain (fuzz/tools/fuzz_brain.py, or any program that speaks the protocol)
 * over TCP and serves as a uam_fuzz_frontend: each INPUT frame is one harness input, and
 * each gets exactly one reply - COVERAGE (kind ITER) for an iteration that ended
 * normally, CRASH otherwise:
 *
 *   UAM_FUZZ_OK            COVERAGE, kind ITER
 *   UAM_FUZZ_CRASH_FAULT   CRASH FAULT,    status = faulting pc (low 32 bits)
 *   UAM_FUZZ_CRASH_GUARD   CRASH GUARD,    status = pc of the guarded access (low 32 bits)
 *   UAM_FUZZ_CRASH_ORACLE  CRASH ORACLE,   status = detail (low 32 bits): the crash pc, a
 *                                          halt status or interrupt lines
 *   UAM_FUZZ_HANG          CRASH HANG,     status = the uam_stop_reason
 *   UAM_FUZZ_TIMEOUT       CRASH TIMEOUT,  status 0
 *   UAM_FUZZ_REJECTED      CRASH REJECTED, status 0: the route refused the input
 *
 * A brain that sends OPTIONS with UAM_NET_OPT_CMP_LOG gets compare logging (HELLO says
 * UAM_NET_HELLO_CMP): each CMP_INPUT runs like an INPUT with its compares logged, and its
 * reply is preceded by a CMPS frame listing them.
 *
 * A brain that sends OPTIONS with UAM_NET_OPT_SPARSE gets SPARSE replies in place of
 * COVERAGE from then on: the map's set buckets as (index, count) pairs, plus the
 * iteration's instruction count; a map too dense for that to be smaller still goes as
 * COVERAGE. HELLO's flags carry UAM_NET_HELLO_BIG_ENDIAN for a big-endian target.
 *
 * The boot map goes out once, unsolicited, as COVERAGE id 0 kind INIT. A malformed INPUT
 * is answered with CRASH MALFORMED and nothing runs. A BYE from the brain ends the run;
 * ember sends BYE when it stops first (an error, an interrupt). EOF before a BYE is an
 * error. Unknown frame types are ignored.
 *
 * A brain that sends MUTATOR, CORPUS and HAVOC (HELLO says UAM_NET_HELLO_HAVOC) has ember
 * mutate: a HAVOC batch hands the harness n mutations of a corpus entry, one after
 * another with no frame in between, through this frontend's next_input, so that the
 * observers and traces see each one as they see an INPUT. Each is judged here: a
 * bucket/count class ember has not seen (the virgin map, which every INPUT that ends
 * normally also updates), a crash with a new (reason, status), or a hang up to MUTATOR's
 * cap is sent as a FIND; HAVOC_DONE closes the batch with its totals.
 *
 * A dead peer is noticed by TCP keepalive (a brain host that vanished without closing the
 * connection would otherwise leave ember waiting forever); a live brain that stops
 * answering, by the optional per-frame timeout (--fuzz-net-timeout).
 */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "formats.h"
#include "cov_formats.h"
#include "uam/fuzz_mutate.h"

_Static_assert(UAM_NET_CMP_EQ == UAM_CMP_EQ && UAM_NET_CMP_ORDER == UAM_CMP_ORDER
               && UAM_NET_CMP_SIGNED == UAM_CMP_SIGNED && UAM_NET_CMP_IMM == UAM_CMP_IMM,
               "CMPS kinds are the IR's compare kinds");

#define CONNECT_TRIES     50            /* the brain may still be starting: ~5 s */
#define CONNECT_DELAY_NS  100000000L
#define KEEPALIVE_IDLE_S  60            /* probe after a minute of silence ... */
#define KEEPALIVE_INTVL_S 10            /* ... every 10 s ... */
#define KEEPALIVE_CNT     6             /* ... and give up after 6 unanswered: ~2 min */

/* An open-addressing table of 64-bit keys and counts (paths, buckets), emptied per batch. */
typedef struct counts {
    uint64_t *key;
    uint32_t *n;           /* 0: the slot is free */
    size_t    cap, used;
} counts;

/* The HAVOC batch in progress. */
typedef struct havoc_batch {
    bool           active;
    uint64_t       first_id, next_id;
    uint32_t       left, flags;
    const uint8_t *parent;
    size_t         parent_len;
    const uint8_t *data;   /* the mutation being run (the mutator's buffer) */
    size_t         len;
    uint64_t       ops;
    uint32_t       execs, finds;
    uint32_t       by_reason[UAM_NET_HAVOC_BY_REASON];
    uint32_t       used[UAM_MUT_OP_LIMIT], found[UAM_MUT_OP_LIMIT];
    counts         paths, edges;
} havoc_batch;

static void counts_free(counts *c);

struct cf_net {
    int       fd;
    bool      broken;      /* a frame was cut short: the stream is out of sync */
    bool      peer_bye;    /* the brain said BYE */
    uint16_t  flags;       /* HELLO scheme flags */
    bool      big_endian;  /* the target's data byte order (HELLO) */
    bool      sparse;      /* OPTIONS asked for SPARSE replies */
    bool      cmp_pending; /* the input being run is a CMP_INPUT: CMPS before its reply */
    uam_fuzz *fuzz;        /* cf_net_attach: the harness (results, hit lists) */
    unsigned  timeout_s;   /* per-frame wait for the brain; 0: none */
    uint8_t  *in;          /* the last frame's payload */
    size_t    in_cap;
    uint8_t  *out;         /* one outgoing frame */
    size_t    out_cap;
    char      peer[300];

    /* mutation in ember (MUTATOR, CORPUS, HAVOC) */
    uam_mutator *mut;
    bool      mut_ok;      /* the last MUTATOR was accepted */
    uint32_t  max_hang_finds, hang_finds;
    uint8_t  *virgin;      /* map_size: the count classes seen per bucket */
    size_t    map_size;
    uint64_t *crash_sigs;  /* (reason << 32 | status) of every crash seen */
    size_t    n_sigs, cap_sigs;
    havoc_batch h;
};

/* ---- big-endian fields ---------------------------------------------------- */

static uint8_t *put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { p = put16(p, (uint16_t)(v >> 16)); return put16(p, (uint16_t)v); }
static uint8_t *put64(uint8_t *p, uint64_t v) { p = put32(p, (uint32_t)(v >> 32)); return put32(p, (uint32_t)v); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---- I/O ---------------------------------------------------------------- */

/* All of buf, retrying short writes and interrupted ones (a signal only sets the abort
 * flag; finishing the frame keeps the stream in sync for a final BYE). */
static bool write_all(cf_net *z, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = send(z->fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: the brain took nothing for %u s "
                        "(--fuzz-net-timeout)", z->peer, z->timeout_s);
            else
                uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: send: %s", z->peer, strerror(errno));
            z->broken = true;
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

enum { RD_OK, RD_EOF, RD_ERR, RD_INTR };

/* All of buf. EOF counts only before the first byte; an interrupt returns RD_INTR
 * (the runner's signal handler has asked the harness to stop). */
static int read_all(cf_net *z, uint8_t *buf, size_t len, bool frame_start)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(z->fd, buf + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            if (got || !frame_start)
                z->broken = true;
            return RD_INTR;
        }
        z->broken = true;
        if (n == 0 && !got && frame_start)
            return RD_EOF;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: nothing from the brain for %u s (--fuzz-net-timeout)",
                    z->peer, z->timeout_s);
        else if (n == 0)
            uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: connection closed in the middle of a frame", z->peer);
        else
            uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: recv: %s", z->peer, strerror(errno));
        return RD_ERR;
    }
    return RD_OK;
}

static bool reserve(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return true;
    uint8_t *p = realloc(*buf, need);
    if (!p)
        return false;
    *buf = p;
    *cap = need;
    return true;
}

/* One frame: payload_len bytes already in z->out after the 5-byte header. */
static bool send_frame(cf_net *z, uint8_t type, size_t payload_len)
{
    if (z->broken)
        return false;
    put32(z->out, (uint32_t)(payload_len + 1));
    z->out[4] = type;
    return write_all(z, z->out, 5 + payload_len);
}

static bool send_crash(cf_net *z, uint64_t id, uint8_t reason, uint32_t status)
{
    uint8_t *p = put64(z->out + 5, id);
    *p++ = reason;
    put32(p, status);
    return send_frame(z, UAM_NET_CRASH, UAM_NET_CRASH_LEN);
}

/* ---- connection ------------------------------------------------------- */

static int dial(const char *host, const char *port, char *err, size_t errlen)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) {
        snprintf(err, errlen, "%s:%s: %s", host, port, gai_strerror(rc));
        return -1;
    }
    int fd = -1, last = 0;
    for (int attempt = 0; fd < 0 && attempt < CONNECT_TRIES; attempt++) {
        if (attempt) {
            struct timespec ts = { 0, CONNECT_DELAY_NS };
            nanosleep(&ts, NULL);
        }
        for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) {
                last = errno;
                continue;
            }
            if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
                last = errno;
                close(fd);
                fd = -1;
            }
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(err, errlen, "cannot connect to %s:%s: %s (is the brain listening?)", host, port,
                 strerror(last));
        return -1;
    }
    int one = 1, idle = KEEPALIVE_IDLE_S, intvl = KEEPALIVE_INTVL_S, cnt = KEEPALIVE_CNT;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    return fd;
}

cf_net *cf_net_connect(const char *hostport, uint16_t flags, unsigned timeout_s, char *err,
                       size_t errlen)
{
    /* HOST:PORT, or [V6ADDR]:PORT */
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport || !colon[1]) {
        snprintf(err, errlen, "expected HOST:PORT, got '%s'", hostport);
        return NULL;
    }
    size_t hlen = (size_t)(colon - hostport);
    const char *h = hostport;
    if (h[0] == '[' && hlen >= 2 && h[hlen - 1] == ']') {
        h++;
        hlen -= 2;
    }
    char *host = strndup(h, hlen);
    cf_net *z = calloc(1, sizeof(*z));
    if (!host || !z) {
        free(host);
        free(z);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    z->fd = dial(host, colon + 1, err, errlen);
    free(host);
    if (z->fd < 0) {
        free(z);
        return NULL;
    }
    z->flags = flags;
    z->timeout_s = timeout_s;
    if (timeout_s) {
        struct timeval tv = { .tv_sec = (time_t)timeout_s };
        setsockopt(z->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(z->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    snprintf(z->peer, sizeof(z->peer), "%s", hostport);
    uam_log(UAM_LOG_INFO, "fuzz-net", "connected to the brain at %s", hostport);
    return z;
}

void cf_net_close(cf_net *z)
{
    if (!z)
        return;
    if (z->fd >= 0)
        close(z->fd);
    free(z->in);
    free(z->out);
    uam_mutator_destroy(z->mut);
    free(z->virgin);
    free(z->crash_sigs);
    counts_free(&z->h.paths);
    counts_free(&z->h.edges);
    free(z);
}

/* ---- mutation in ember ------------------------------------------------- */

/* A hit count's class (AFL's buckets): what makes a count new. */
static uint8_t count_class(uint8_t c)
{
    if (c <= 2)   return c;
    if (c == 3)   return 4;
    if (c <= 7)   return 8;
    if (c <= 15)  return 16;
    if (c <= 31)  return 32;
    if (c <= 127) return 64;
    return 128;
}

static uint64_t mix64(uint64_t v)
{
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return v;
}

#define FNV_BASIS 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t fnv_byte(uint64_t h, uint8_t b) { return (h ^ b) * FNV_PRIME; }

/* The path's name: FNV-1a 64 over each (index as u32 LE, class as u8), then mix64. */
static uint64_t path_hash(const uint32_t *hits, size_t n, const uint8_t *map)
{
    uint64_t h = FNV_BASIS;
    for (size_t i = 0; i < n; i++) {
        uint32_t ix = hits[i];
        for (int k = 0; k < 4; k++)
            h = fnv_byte(h, (uint8_t)(ix >> (8 * k)));
        h = fnv_byte(h, count_class(map[ix]));
    }
    return mix64(h);
}

static void counts_free(counts *c)
{
    free(c->key);
    free(c->n);
    *c = (counts){ 0 };
}

static void counts_clear(counts *c)
{
    if (c->used)
        memset(c->n, 0, c->cap * sizeof(*c->n));
    c->used = 0;
}

static bool counts_grow(counts *c)
{
    size_t cap = c->cap ? 2 * c->cap : 1024;
    uint64_t *k = calloc(cap, sizeof(*k));
    uint32_t *n = calloc(cap, sizeof(*n));
    if (!k || !n) {
        free(k);
        free(n);
        return false;
    }
    for (size_t i = 0; i < c->cap; i++) {
        if (!c->n[i])
            continue;
        size_t j = (size_t)mix64(c->key[i]) & (cap - 1);
        while (n[j])
            j = (j + 1) & (cap - 1);
        k[j] = c->key[i];
        n[j] = c->n[i];
    }
    free(c->key);
    free(c->n);
    c->key = k;
    c->n = n;
    c->cap = cap;
    return true;
}

static bool counts_add(counts *c, uint64_t key)
{
    if (2 * (c->used + 1) > c->cap && !counts_grow(c))
        return false;
    size_t j = (size_t)mix64(key) & (c->cap - 1);
    while (c->n[j] && c->key[j] != key)
        j = (j + 1) & (c->cap - 1);
    if (!c->n[j]) {
        c->key[j] = key;
        c->used++;
    }
    if (c->n[j] < UINT32_MAX)
        c->n[j]++;
    return true;
}

/* Fold an iteration that ended normally into the virgin map: true if it set a bucket/count
 * class not seen before. */
static bool virgin_fold(cf_net *z, const uint32_t *hits, size_t n, const uint8_t *map)
{
    bool fresh = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t cl = count_class(map[hits[i]]);
        if (!(z->virgin[hits[i]] & cl)) {
            z->virgin[hits[i]] |= cl;
            fresh = true;
        }
    }
    return fresh;
}

/* Note a crash's (reason, status): true the first time. */
static bool crash_sig_new(cf_net *z, uint8_t reason, uint32_t status)
{
    uint64_t sig = (uint64_t)reason << 32 | status;
    for (size_t i = 0; i < z->n_sigs; i++)
        if (z->crash_sigs[i] == sig)
            return false;
    if (z->n_sigs == z->cap_sigs) {
        size_t cap = z->cap_sigs ? 2 * z->cap_sigs : 64;
        uint64_t *p = realloc(z->crash_sigs, cap * sizeof(*p));
        if (!p)
            return true;          /* report it; the set stays as it is */
        z->crash_sigs = p;
        z->cap_sigs = cap;
    }
    z->crash_sigs[z->n_sigs++] = sig;
    return true;
}

/* The protocol's crash reason (0: ended normally) and status of a verdict. */
static uint8_t verdict_reason(uam_fuzz_verdict v, uint64_t detail, uint32_t *status)
{
    *status = (uint32_t)detail;
    switch (v) {
    case UAM_FUZZ_OK:           *status = 0; return 0;
    case UAM_FUZZ_CRASH_FAULT:  return UAM_NET_FAULT;
    case UAM_FUZZ_CRASH_GUARD:  return UAM_NET_GUARD;
    case UAM_FUZZ_CRASH_ORACLE: return UAM_NET_ORACLE;
    case UAM_FUZZ_HANG:         return UAM_NET_HANG;
    case UAM_FUZZ_REJECTED:     *status = 0; return UAM_NET_REJECTED;
    case UAM_FUZZ_TIMEOUT:
    default:                    *status = 0; return UAM_NET_TIMEOUT;
    }
}

/* The map's set buckets, ascending (the harness's list, which also makes the next clear
 * cheap). NULL only when out of memory. */
static const uint32_t *all_hits(cf_net *z, size_t *n)
{
    return uam_fuzz_map_hits(z->fuzz, z->map_size, n);
}

static bool need_virgin(cf_net *z)
{
    if (z->virgin)
        return true;
    size_t size;
    uam_fuzz_map(z->fuzz, &size);
    z->map_size = size;
    z->virgin = calloc(size ? size : 1, 1);
    return z->virgin != NULL;
}

static bool send_mutator_info(cf_net *z, bool ok, const char *msg)
{
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > 1024)
        mlen = 1024;
    size_t need = 5 + 1 + 2 + mlen + 2 + UAM_MUT_OP_LIMIT * (1 + 2 + 2 + 1 + 64);
    if (!reserve(&z->out, &z->out_cap, need)) {
        z->broken = true;
        return false;
    }
    uint8_t *p = z->out + 5;
    *p++ = ok ? 0 : 1;
    p = put16(p, (uint16_t)mlen);
    memcpy(p, msg ? msg : "", mlen);
    p += mlen;
    uint8_t *count = p;
    p += 2;
    uint16_t n = 0;
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++) {
        if (!uam_mutator_is_op(op))
            continue;
        const char *name = uam_mutator_op_name(op);
        size_t nl = strlen(name);
        *p++ = (uint8_t)op;
        p = put16(p, (uint16_t)uam_mutator_default_weight(op));
        p = put16(p, (uint16_t)(z->mut ? uam_mutator_weight(z->mut, op)
                                       : uam_mutator_default_weight(op)));
        *p++ = (uint8_t)nl;
        memcpy(p, name, nl);
        p += nl;
        n++;
    }
    put16(count, n);
    return send_frame(z, UAM_NET_MUTATOR_INFO, (size_t)(p - (z->out + 5)));
}

/* MUTATOR: replace the mutator's settings and dictionary. NULL when accepted, else why. */
static const char *apply_mutator(cf_net *z, const uint8_t *b, size_t len)
{
    if (len < UAM_NET_MUTATOR_MIN_LEN)
        return "MUTATOR is shorter than its fixed fields";
    if (!z->mut && !(z->mut = uam_mutator_create()))
        return "out of memory";
    uam_mutator *m = z->mut;
    const uint8_t *p = b, *end = b + len;
    uint32_t max_size = get32(p);
    unsigned lo = get16(p + 4), hi = get16(p + 6), splice = get16(p + 8);
    uint32_t max_hangs = get32(p + 10);
    unsigned nw = get16(p + 14);
    p += 16;
    if (!max_size || max_size > UAM_FUZZ_NET_FRAME_MAX)
        return "max_size must be 1 .. 64 MiB";
    if (uam_mutator_set_max_size(m, max_size) != UAM_OK)
        return "cannot allocate max_size bytes";
    if (uam_mutator_set_stack(m, lo, hi) != UAM_OK)
        return "the stack depth range must be 1 <= lo <= hi <= 1024";
    if (splice > 1000)
        return "splice is per mille (0 .. 1000)";
    uam_mutator_set_splice(m, splice);
    uam_mutator_reset_weights(m);
    if ((size_t)(end - p) < (size_t)nw * 3 + 4)
        return "MUTATOR's weight list is cut short";
    for (unsigned i = 0; i < nw; i++, p += 3)
        if (uam_mutator_set_weight(m, p[0], get16(p + 1)) != UAM_OK)
            return "a weight names no havoc operator";
    uint32_t nt = get32(p);
    p += 4;
    uam_mutator_clear_tokens(m);
    for (uint32_t i = 0; i < nt; i++) {
        if (p >= end)
            return "MUTATOR's token list is cut short";
        uint8_t kind = *p++;
        if (kind == 0) {
            if (end - p < 2 || (size_t)(end - p - 2) < get16(p))
                return "MUTATOR's token list is cut short";
            size_t tl = get16(p);
            if (uam_mutator_add_token(m, p + 2, tl) != UAM_OK)
                return "a byte token must be 1 .. 256 bytes";
            p += 2 + tl;
        } else if (kind == 1) {
            if (end - p < 10)
                return "MUTATOR's token list is cut short";
            if (uam_mutator_add_int_token(m, get64(p + 2), p[0], p[1] != 0) != UAM_OK)
                return "an integer token's width must be 1, 2, 4 or 8";
            p += 10;
        } else {
            return "unknown token kind";
        }
    }
    if (p != end)
        return "MUTATOR has bytes past its token list";
    /* A probe: no operator enabled is the brain's mistake, said now rather than per batch. */
    size_t ol;
    static const uint8_t probe_in[1] = { 0 };
    if (!uam_mutator_mutate(m, probe_in, 1, &ol, NULL))
        return "no operator is enabled (every weight 0, or only dictionary ones and no tokens)";
    z->max_hang_finds = max_hangs;
    return NULL;
}

static bool send_havoc_done(cf_net *z, uint8_t status)
{
    havoc_batch *h = &z->h;
    size_t n_ops = 0;
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++)
        n_ops += h->used[op] != 0;
    size_t need = 5 + 8 + 1 + 4 + 4 * UAM_NET_HAVOC_BY_REASON + 4 + 2 + n_ops * 9
                + 4 + h->paths.used * 12 + 4 + h->edges.used * 8;
    if (!reserve(&z->out, &z->out_cap, need)) {
        z->broken = true;
        return false;
    }
    uint8_t *p = put64(z->out + 5, h->first_id);
    *p++ = status;
    p = put32(p, h->execs);
    for (unsigned i = 0; i < UAM_NET_HAVOC_BY_REASON; i++)
        p = put32(p, h->by_reason[i]);
    p = put32(p, h->finds);
    p = put16(p, (uint16_t)n_ops);
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++) {
        if (!h->used[op])
            continue;
        *p++ = (uint8_t)op;
        p = put32(p, h->used[op]);
        p = put32(p, h->found[op]);
    }
    p = put32(p, (uint32_t)h->paths.used);
    for (size_t i = 0; i < h->paths.cap; i++) {
        if (h->paths.n[i]) {
            p = put64(p, h->paths.key[i]);
            p = put32(p, h->paths.n[i]);
        }
    }
    p = put32(p, (uint32_t)h->edges.used);
    for (size_t i = 0; i < h->edges.cap; i++) {
        if (h->edges.n[i]) {
            p = put32(p, (uint32_t)h->edges.key[i]);
            p = put32(p, h->edges.n[i]);
        }
    }
    h->active = false;
    return send_frame(z, UAM_NET_HAVOC_DONE, (size_t)(p - (z->out + 5)));
}

/* HAVOC: start a batch. false: answered at once (nothing to run) or the stream broke. */
static bool start_havoc(cf_net *z, const uint8_t *b)
{
    havoc_batch *h = &z->h;
    uint64_t first = get64(b), seed = get64(b + 16);
    uint32_t entry = get32(b + 8), n = get32(b + 12), flags = get32(b + 24);
    counts paths = h->paths, edges = h->edges;
    counts_clear(&paths);
    counts_clear(&edges);
    *h = (havoc_batch){ .first_id = first, .next_id = first, .left = n, .flags = flags,
                        .paths = paths, .edges = edges };
    uint8_t status = UAM_NET_HAVOC_OK;
    if (!z->mut || !z->mut_ok)
        status = UAM_NET_HAVOC_NO_MUTATOR;
    else if (!(h->parent = uam_mutator_corpus_get(z->mut, entry, &h->parent_len)))
        status = UAM_NET_HAVOC_NO_ENTRY;
    else if (!n || n > UAM_NET_HAVOC_MAX || !first)
        status = UAM_NET_HAVOC_BAD;
    else if (!need_virgin(z)) {
        z->broken = true;
        return false;
    }
    if (status != UAM_NET_HAVOC_OK) {
        uam_log(UAM_LOG_WARN, "fuzz-net", "HAVOC for entry %u refused (status %u)", entry, status);
        send_havoc_done(z, status);
        return false;
    }
    uam_mutator_seed(z->mut, seed);
    h->active = true;
    return true;
}

/* The next mutation of the batch. */
static void next_mutation(cf_net *z, uint8_t **data, size_t *len, uint64_t *id)
{
    havoc_batch *h = &z->h;
    h->data = uam_mutator_mutate(z->mut, h->parent, h->parent_len, &h->len, &h->ops);
    *data = (uint8_t *)h->data;   /* the harness only reads it */
    *len = h->len;
    *id = h->next_id++;
    h->left--;
}

static bool send_find(cf_net *z, uint64_t id, uint8_t reason, uint32_t status, uint64_t insns,
                      const uint32_t *hits, size_t n, const uint8_t *map)
{
    havoc_batch *h = &z->h;
    if (!reserve(&z->out, &z->out_cap, 5 + UAM_NET_FIND_HDR_LEN + h->len + 4 + n * 5)) {
        z->broken = true;
        return false;
    }
    uint8_t *p = put64(z->out + 5, id);
    *p++ = reason;
    p = put32(p, status);
    p = put64(p, insns);
    p = put64(p, h->ops);
    p = put32(p, (uint32_t)h->len);
    memcpy(p, h->data, h->len);
    p += h->len;
    p = put32(p, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        p = put32(p, hits[i]);
        *p++ = map[hits[i]];
    }
    h->finds++;
    return send_frame(z, UAM_NET_FIND, (size_t)(p - (z->out + 5)));
}

/* One mutation of the batch ran: judge it, and close the batch after the last. */
static bool havoc_report(cf_net *z, uint64_t id, uam_fuzz_verdict v, uint64_t detail)
{
    havoc_batch *h = &z->h;
    uint32_t status;
    uint8_t reason = verdict_reason(v, detail, &status);
    h->execs++;
    h->by_reason[reason]++;
    bool fresh = false, ok = true;
    if (reason == 0) {
        size_t n;
        const uint32_t *hits = all_hits(z, &n);
        if (!hits) {
            z->broken = true;
            return false;
        }
        const uint8_t *map = uam_fuzz_map(z->fuzz, NULL);
        fresh = virgin_fold(z, hits, n, map);
        counts_add(&h->paths, path_hash(hits, n, map));
        if (h->flags & UAM_NET_HAVOC_EDGES)
            for (size_t i = 0; i < n; i++)
                counts_add(&h->edges, hits[i]);
        if (fresh) {
            const uam_fuzz_result *r = uam_fuzz_last_result(z->fuzz);
            ok = send_find(z, id, 0, 0, r ? r->insns : 0, hits, n, map);
        }
    } else if (reason == UAM_NET_HANG || reason == UAM_NET_TIMEOUT) {
        if (z->hang_finds < z->max_hang_finds) {
            z->hang_finds++;
            ok = send_find(z, id, reason, status, 0, NULL, 0, NULL);
        }
    } else if (reason != UAM_NET_REJECTED && crash_sig_new(z, reason, status)) {
        ok = send_find(z, id, reason, status, 0, NULL, 0, NULL);
    }
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++) {
        if (h->ops >> op & 1) {
            h->used[op]++;
            h->found[op] += fresh;
        }
    }
    if (ok && !h->left)
        ok = send_havoc_done(z, UAM_NET_HAVOC_OK);
    return ok;
}

/* ---- the frontend --------------------------------------------------- */

static uam_status net_hello(void *ctx, const uam_fuzz_info *info)
{
    cf_net *z = ctx;
    size_t alen = strlen(info->arch);
    if (alen > UAM_NET_ARCH_MAX)
        alen = UAM_NET_ARCH_MAX;
    size_t cov = (size_t)5 + UAM_NET_COVERAGE_HDR_LEN + info->map_size;
    if (cov - 4 > UAM_FUZZ_NET_FRAME_MAX) {
        uam_log(UAM_LOG_ERROR, "fuzz-net", "a %u-byte coverage map does not fit a frame",
                info->map_size);
        return UAM_ERR_INVALID;
    }
    const size_t hello = (size_t)5 + UAM_NET_HELLO_FIXED_LEN + UAM_NET_ARCH_MAX;
    const size_t sparse = (size_t)5 + UAM_NET_SPARSE_HDR_LEN + info->map_size;  /* the list's bound */
    size_t need = cov > hello ? cov : hello;
    if (sparse > need)
        need = sparse;
    if (!reserve(&z->out, &z->out_cap, need))
        return UAM_ERR_NOMEM;
    if (!write_all(z, (const uint8_t *)UAM_FUZZ_NET_MAGIC, 4))
        return UAM_ERR_INVALID;
    uint8_t *p = put16(z->out + 5, UAM_FUZZ_NET_VERSION);
    p = put16(p, (uint16_t)(z->flags | (z->big_endian ? UAM_NET_HELLO_BIG_ENDIAN : 0)
                            | (z->fuzz ? UAM_NET_HELLO_CMP | UAM_NET_HELLO_HAVOC : 0)));
    p = put32(p, info->map_size);
    p = put16(p, (uint16_t)alen);
    memcpy(p, info->arch, alen);
    return send_frame(z, UAM_NET_HELLO, UAM_NET_HELLO_FIXED_LEN + alen) ? UAM_OK : UAM_ERR_INVALID;
}

static int net_next_input(void *ctx, uint8_t **data, size_t *len, uint64_t *id)
{
    cf_net *z = ctx;
    for (;;) {
        if (z->broken)
            return -1;
        if (z->h.active && z->h.left) {   /* a HAVOC batch runs on, with no frame between */
            next_mutation(z, data, len, id);
            return 1;
        }
        uint8_t hdr[5];
        int rc = read_all(z, hdr, sizeof(hdr), true);
        if (rc == RD_EOF) {
            uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: the brain closed the connection without BYE", z->peer);
            return -1;
        }
        if (rc != RD_OK)
            return -1;
        uint32_t flen = get32(hdr);
        if (flen < 1 || flen > UAM_FUZZ_NET_FRAME_MAX) {
            uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: bad frame length %u", z->peer, flen);
            z->broken = true;
            return -1;
        }
        size_t plen = flen - 1;
        if (!reserve(&z->in, &z->in_cap, plen ? plen : 1)) {
            z->broken = true;
            return -1;
        }
        if (plen && read_all(z, z->in, plen, false) != RD_OK)
            return -1;
        switch (hdr[4]) {
        case UAM_NET_BYE:
            z->peer_bye = true;
            return 0;
        case UAM_NET_OPTIONS:
            if (plen >= UAM_NET_OPTIONS_MIN_LEN && z->fuzz) {
                const uint32_t o = get32(z->in);
                z->sparse = o & UAM_NET_OPT_SPARSE;
                unsigned modes = uam_fuzz_cmp_modes(z->fuzz) & ~UAM_FUZZ_CMP_LOG;
                if (o & UAM_NET_OPT_CMP_LOG)
                    modes |= UAM_FUZZ_CMP_LOG;
                if (uam_fuzz_set_cmp(z->fuzz, modes) != UAM_OK)
                    uam_log(UAM_LOG_WARN, "fuzz-net", "cannot turn compare logging %s",
                            modes & UAM_FUZZ_CMP_LOG ? "on" : "off");
                else if (modes & UAM_FUZZ_CMP_LOG)
                    uam_log(UAM_LOG_INFO, "fuzz-net", "compare logging on (the brain asked)");
            }
            continue;
        case UAM_NET_INPUT:
        case UAM_NET_CMP_INPUT: {
            uint32_t dlen = plen >= UAM_NET_INPUT_HDR_LEN ? get32(z->in + 8) : 0;
            if (plen < UAM_NET_INPUT_HDR_LEN || dlen > plen - UAM_NET_INPUT_HDR_LEN) {
                uint64_t bad = plen >= 8 ? get64(z->in) : 0;
                uam_log(UAM_LOG_WARN, "fuzz-net", "malformed INPUT (id %llu, %zu payload bytes)",
                        (unsigned long long)bad, plen);
                if (!send_crash(z, bad, UAM_NET_MALFORMED, 0))
                    return -1;
                continue;
            }
            *id = get64(z->in);
            *len = dlen;
            *data = z->in + UAM_NET_INPUT_HDR_LEN;
            z->cmp_pending = hdr[4] == UAM_NET_CMP_INPUT;
            if (z->cmp_pending && z->fuzz)
                uam_fuzz_cmp_arm(z->fuzz);   /* false with logging off: an empty CMPS */
            return 1;
        }
        case UAM_NET_MUTATOR: {
            const char *why = z->fuzz ? apply_mutator(z, z->in, plen)
                                      : "this frontend runs without a harness";
            z->mut_ok = !why;
            if (why)
                uam_log(UAM_LOG_ERROR, "fuzz-net", "MUTATOR refused: %s", why);
            if (!send_mutator_info(z, !why, why))
                return -1;
            continue;
        }
        case UAM_NET_CORPUS: {
            uint32_t clen = plen >= UAM_NET_CORPUS_HDR_LEN ? get32(z->in + 4) : 0;
            if (plen < UAM_NET_CORPUS_HDR_LEN || clen != plen - UAM_NET_CORPUS_HDR_LEN) {
                uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: malformed CORPUS (%zu payload bytes)",
                        z->peer, plen);
                z->broken = true;
                return -1;
            }
            if ((!z->mut && !(z->mut = uam_mutator_create()))
                || uam_mutator_corpus_put(z->mut, get32(z->in), z->in + UAM_NET_CORPUS_HDR_LEN,
                                          clen) != UAM_OK) {
                uam_log(UAM_LOG_ERROR, "fuzz-net", "out of memory for the corpus");
                z->broken = true;
                return -1;
            }
            continue;
        }
        case UAM_NET_HAVOC:
            if (plen < UAM_NET_HAVOC_LEN) {
                uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: malformed HAVOC (%zu payload bytes)",
                        z->peer, plen);
                z->broken = true;
                return -1;
            }
            if (start_havoc(z, z->in)) {
                next_mutation(z, data, len, id);
                return 1;
            }
            continue;   /* answered at once (or broken: the loop's top says so) */
        default:
            continue;   /* any other frame type is ignored */
        }
    }
}

/* One map as SPARSE: false (nothing sent) when the list would not be smaller than the map,
 * so that the caller sends COVERAGE. *ok: the frame went out. */
static bool send_sparse(cf_net *z, uint64_t id, int kind, size_t map_size, bool *ok)
{
    const size_t max = map_size / UAM_NET_SPARSE_ENTRY_LEN;   /* past this, COVERAGE is smaller */
    size_t n;
    const uint32_t *hits = uam_fuzz_map_hits(z->fuzz, max, &n);
    if (!hits)
        return false;
    const uint8_t *map = uam_fuzz_map(z->fuzz, NULL);
    const uam_fuzz_result *r = uam_fuzz_last_result(z->fuzz);
    uint8_t *p = put64(z->out + 5, id);
    *p++ = kind == UAM_FUZZ_KIND_INIT ? UAM_COV_KIND_INIT : UAM_COV_KIND_ITER;
    p = put64(p, kind == UAM_FUZZ_KIND_INIT || !r ? 0 : r->insns);
    p = put32(p, (uint32_t)map_size);
    p = put32(p, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        p = put32(p, hits[i]);
        *p++ = map[hits[i]];
    }
    *ok = send_frame(z, UAM_NET_SPARSE, UAM_NET_SPARSE_HDR_LEN + n * UAM_NET_SPARSE_ENTRY_LEN);
    return true;
}

/* The compares the last iteration logged (none unless it was armed). */
static bool send_cmps(cf_net *z, uint64_t id)
{
    size_t n = 0;
    uint64_t dropped = 0;
    const uam_cmp_entry *e = z->fuzz ? uam_fuzz_cmp_log(z->fuzz, &n, &dropped) : NULL;
    if (!e)
        n = 0;
    const size_t len = UAM_NET_CMPS_HDR_LEN + n * UAM_NET_CMPS_ENTRY_LEN;
    if (!reserve(&z->out, &z->out_cap, 5 + len)) {
        z->broken = true;
        return false;
    }
    uint8_t *p = put64(z->out + 5, id);
    p = put32(p, (uint32_t)n);
    p = put32(p, dropped > UINT32_MAX ? UINT32_MAX : (uint32_t)dropped);
    for (size_t i = 0; i < n; i++) {
        p = put32(p, e[i].site);
        *p++ = e[i].width;
        *p++ = e[i].kind;
        p = put64(p, e[i].a);
        p = put64(p, e[i].b);
    }
    return send_frame(z, UAM_NET_CMPS, len);
}

static uam_status net_report(void *ctx, uint64_t id, uam_fuzz_verdict v, uint64_t detail,
                             const uint8_t *map, size_t map_size, int kind)
{
    cf_net *z = ctx;
    bool ok;
    if (kind == UAM_FUZZ_KIND_ITER && z->h.active)
        return havoc_report(z, id, v, detail) ? UAM_OK : UAM_ERR_INVALID;
    /* What an INPUT showed counts for the batches too: its classes are not new to them,
     * and a crash it found is not reported again. */
    if (kind == UAM_FUZZ_KIND_ITER && z->fuzz && v == UAM_FUZZ_OK) {
        size_t n;
        const uint32_t *hits = need_virgin(z) ? all_hits(z, &n) : NULL;
        if (!hits) {
            uam_log(UAM_LOG_ERROR, "fuzz-net", "out of memory for the virgin map");
            return UAM_ERR_NOMEM;
        }
        virgin_fold(z, hits, n, map);
    } else if (kind == UAM_FUZZ_KIND_ITER && v != UAM_FUZZ_HANG && v != UAM_FUZZ_TIMEOUT
               && v != UAM_FUZZ_REJECTED) {
        uint32_t st;
        uint8_t reason = verdict_reason(v, detail, &st);
        crash_sig_new(z, reason, st);
    }
    if (kind == UAM_FUZZ_KIND_ITER && z->cmp_pending) {
        z->cmp_pending = false;
        if (!send_cmps(z, id))
            return UAM_ERR_INVALID;
    }
    if ((kind == UAM_FUZZ_KIND_INIT || v == UAM_FUZZ_OK) && z->sparse
        && send_sparse(z, kind == UAM_FUZZ_KIND_INIT ? 0 : id, kind, map_size, &ok)) {
        /* sent (or failed) as SPARSE */
    } else if (kind == UAM_FUZZ_KIND_INIT || v == UAM_FUZZ_OK) {
        uint8_t *p = put64(z->out + 5, kind == UAM_FUZZ_KIND_INIT ? 0 : id);
        *p++ = kind == UAM_FUZZ_KIND_INIT ? UAM_COV_KIND_INIT : UAM_COV_KIND_ITER;
        p = put32(p, (uint32_t)map_size);
        memcpy(p, map, map_size);
        ok = send_frame(z, UAM_NET_COVERAGE, UAM_NET_COVERAGE_HDR_LEN + map_size);
    } else {
        uint8_t reason;
        uint32_t status = (uint32_t)detail;
        switch (v) {
        case UAM_FUZZ_CRASH_FAULT:  reason = UAM_NET_FAULT; break;
        case UAM_FUZZ_CRASH_GUARD:  reason = UAM_NET_GUARD; break;
        case UAM_FUZZ_CRASH_ORACLE: reason = UAM_NET_ORACLE; break;
        case UAM_FUZZ_HANG:         reason = UAM_NET_HANG; break;
        case UAM_FUZZ_REJECTED:     reason = UAM_NET_REJECTED; status = 0; break;
        default:                    reason = UAM_NET_TIMEOUT; status = 0; break;
        }
        ok = send_crash(z, id, reason, status);
    }
    return ok ? UAM_OK : UAM_ERR_INVALID;
}

static void net_bye(void *ctx)
{
    cf_net *z = ctx;
    if (!z->peer_bye && !z->broken && z->out)
        send_frame(z, UAM_NET_BYE, 0);
    shutdown(z->fd, SHUT_WR);
}

void cf_net_attach(cf_net *z, uam_fuzz *f, bool big_endian)
{
    z->fuzz = f;
    z->big_endian = big_endian;
}

const uam_fuzz_frontend cf_net_frontend = {
    .hello = net_hello,
    .next_input = net_next_input,
    .report = net_report,
    .bye = net_bye,
};
