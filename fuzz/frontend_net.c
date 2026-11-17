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
 * another with no frame from the brain in between, through this frontend's next_input, so
 * that the observers and traces see each one as they see an INPUT. The campaign engine
 * (uam/fuzz_campaign.h) owns the mutator and the record of what runs have shown, and
 * judges every run, INPUTs included; this file only turns its judgements into frames:
 * FIND for a batch's run worth keeping, PROGRESS when a batch has sent nothing for a
 * second, HAVOC_DONE with the totals, and, when OPTIONS asks (UAM_NET_OPT_JUDGE), a JUDGE
 * frame before each INPUT's reply, so that the brain keeps no record of its own.
 *
 * STAGE (HELLO says UAM_NET_HELLO_STAGES) runs one of the campaign's stages on a corpus
 * entry the same way: input-to-state or trimming, with a STAGE_RESULT before the batch's
 * HAVOC_DONE.
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
#include "uam/fuzz_campaign.h"

_Static_assert(UAM_NET_STAGE_I2S_MAX == UAM_FUZZ_I2S_MAX && UAM_NET_STAGE_TRIM_MAX == UAM_FUZZ_TRIM_MAX,
               "STAGE's limits are the campaign's");
_Static_assert(UAM_NET_CMP_EQ == UAM_CMP_EQ && UAM_NET_CMP_ORDER == UAM_CMP_ORDER
               && UAM_NET_CMP_SIGNED == UAM_CMP_SIGNED && UAM_NET_CMP_IMM == UAM_CMP_IMM,
               "CMPS kinds are the IR's compare kinds");

#define CONNECT_TRIES     50            /* the brain may still be starting: ~5 s */
#define CONNECT_DELAY_NS  100000000L
#define KEEPALIVE_IDLE_S  60            /* probe after a minute of silence ... */
#define KEEPALIVE_INTVL_S 10            /* ... every 10 s ... */
#define KEEPALIVE_CNT     6             /* ... and give up after 6 unanswered: ~2 min */

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

    /* the campaign (fuzz_campaign.h): the record of what runs showed, the mutator and
     * its batches. This frontend only puts its judgements into frames. */
    uam_fuzz_campaign *camp;
    bool      mut_ok;      /* the last MUTATOR was accepted */
    bool      judge;       /* OPTIONS asked for a JUDGE frame before each INPUT's reply */
    bool      batch;       /* a HAVOC batch or a stage is running */
    uint8_t   stage;       /* the stage's kind (UAM_NET_STAGE_*); 0: a HAVOC batch */
    uint64_t  first_id, next_id;
    uint64_t  last_ns;     /* when the last frame of the batch went out (PROGRESS) */
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
    uam_fuzz_campaign_destroy(z->camp);
    free(z);
}

/* ---- mutation in ember ------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
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

static bool send_mutator_info(cf_net *z, bool ok, const char *msg)
{
    uam_mutator *mut = z->camp ? uam_fuzz_campaign_mutator(z->camp) : NULL;
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
        p = put16(p, (uint16_t)(mut ? uam_mutator_weight(mut, op) : uam_mutator_default_weight(op)));
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
    if (!z->camp)
        return "this frontend runs without a campaign";
    if (len < UAM_NET_MUTATOR_MIN_LEN)
        return "MUTATOR is shorter than its fixed fields";
    uam_mutator *m = uam_fuzz_campaign_mutator(z->camp);
    const uint8_t *p = b, *end = b + len;
    uint32_t max_size = get32(p);
    unsigned lo = get16(p + 4), hi = get16(p + 6), splice = get16(p + 8);
    uint32_t max_hangs = get32(p + 10);
    unsigned nw = get16(p + 14);
    p += 16;
    if (!max_size || max_size > UAM_FUZZ_NET_FRAME_MAX / 2)
        return "max_size must be 1 .. 32 MiB";
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
    uam_fuzz_campaign_set_hang_cap(z->camp, max_hangs);
    return NULL;
}

/* by_reason of HAVOC_DONE from the batch's count per verdict. */
static uint32_t runs_of_reason(const uam_fuzz_batch *b, uint8_t reason)
{
    static const uam_fuzz_verdict of[UAM_NET_HAVOC_BY_REASON] = {
        [0] = UAM_FUZZ_OK, [UAM_NET_FAULT] = UAM_FUZZ_CRASH_FAULT, [UAM_NET_GUARD] = UAM_FUZZ_CRASH_GUARD,
        [UAM_NET_ORACLE] = UAM_FUZZ_CRASH_ORACLE, [UAM_NET_HANG] = UAM_FUZZ_HANG,
        [UAM_NET_TIMEOUT] = UAM_FUZZ_TIMEOUT, [UAM_NET_REJECTED] = UAM_FUZZ_REJECTED,
    };
    return b->by_verdict[of[reason]];
}

/* HAVOC_DONE; with a status other than OK nothing ran and the totals are zero. */
static bool send_havoc_done(cf_net *z, uint64_t first_id, uint8_t status)
{
    static const uam_fuzz_batch none;
    const uam_fuzz_batch *b = status == UAM_NET_HAVOC_OK ? uam_fuzz_campaign_batch(z->camp) : &none;
    size_t n_paths = 0, n_edges = 0, n_ops = 0;
    const uam_fuzz_count *paths = NULL, *edges = NULL;
    if (status == UAM_NET_HAVOC_OK) {
        paths = uam_fuzz_campaign_batch_paths(z->camp, &n_paths);
        edges = uam_fuzz_campaign_batch_edges(z->camp, &n_edges);
    }
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++)
        n_ops += b->op_used[op] != 0;
    size_t need = 5 + 8 + 1 + 4 + 4 * UAM_NET_HAVOC_BY_REASON + 4 + 2 + n_ops * 9
                + 4 + n_paths * 12 + 4 + n_edges * 8;
    z->batch = false;
    if (!reserve(&z->out, &z->out_cap, need)) {
        z->broken = true;
        return false;
    }
    uint8_t *p = put64(z->out + 5, first_id);
    *p++ = status;
    p = put32(p, b->execs);
    for (uint8_t r = 0; r < UAM_NET_HAVOC_BY_REASON; r++)
        p = put32(p, runs_of_reason(b, r));
    p = put32(p, b->reports);
    p = put16(p, (uint16_t)n_ops);
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++) {
        if (!b->op_used[op])
            continue;
        *p++ = (uint8_t)op;
        p = put32(p, b->op_used[op]);
        p = put32(p, b->op_found[op]);
    }
    p = put32(p, (uint32_t)n_paths);
    for (size_t i = 0; i < n_paths; i++) {
        p = put64(p, paths[i].key);
        p = put32(p, paths[i].n);
    }
    p = put32(p, (uint32_t)n_edges);
    for (size_t i = 0; i < n_edges; i++) {
        p = put32(p, (uint32_t)edges[i].key);
        p = put32(p, edges[i].n);
    }
    return send_frame(z, UAM_NET_HAVOC_DONE, (size_t)(p - (z->out + 5)));
}

/* HAVOC: start a batch. false: answered at once (nothing to run) or the stream broke. */
static bool start_havoc(cf_net *z, const uint8_t *b)
{
    const uint64_t first = get64(b), seed = get64(b + 16);
    const uint32_t entry = get32(b + 8), n = get32(b + 12), flags = get32(b + 24);
    uint8_t status = UAM_NET_HAVOC_OK;
    size_t elen;
    if (!z->camp || !z->mut_ok)
        status = UAM_NET_HAVOC_NO_MUTATOR;
    else if (!uam_mutator_corpus_get(uam_fuzz_campaign_mutator(z->camp), entry, &elen))
        status = UAM_NET_HAVOC_NO_ENTRY;
    else if (!n || n > UAM_NET_HAVOC_MAX || !first
             || uam_fuzz_campaign_batch_begin(z->camp, entry, n, seed,
                                              flags & UAM_NET_HAVOC_EDGES ? UAM_FUZZ_BATCH_EDGES : 0)
                != UAM_OK)
        status = UAM_NET_HAVOC_BAD;
    if (status != UAM_NET_HAVOC_OK) {
        uam_log(UAM_LOG_WARN, "fuzz-net", "HAVOC for entry %u refused (status %u)", entry, status);
        send_havoc_done(z, first, status);
        return false;
    }
    z->batch = true;
    z->stage = 0;
    z->first_id = z->next_id = first;
    z->last_ns = now_ns();
    return true;
}

/* STAGE: start a stage. false: answered at once (nothing to run) or the stream broke. */
static bool start_stage(cf_net *z, const uint8_t *b)
{
    const uint64_t first = get64(b);
    const uint32_t entry = get32(b + 8), limit = get32(b + 13), flags = get32(b + 17);
    const uint8_t kind = b[12];
    const unsigned bf = flags & UAM_NET_HAVOC_EDGES ? UAM_FUZZ_BATCH_EDGES : 0;
    uint8_t status = UAM_NET_HAVOC_OK;
    size_t elen;
    if (!z->camp || !z->mut_ok)
        status = UAM_NET_HAVOC_NO_MUTATOR;
    else if (!uam_mutator_corpus_get(uam_fuzz_campaign_mutator(z->camp), entry, &elen))
        status = UAM_NET_HAVOC_NO_ENTRY;
    else if (kind == UAM_NET_STAGE_I2S && !(uam_fuzz_cmp_modes(z->fuzz) & UAM_FUZZ_CMP_LOG))
        status = UAM_NET_HAVOC_NO_CMP;
    else if (!first || (kind != UAM_NET_STAGE_I2S && kind != UAM_NET_STAGE_TRIM)
             || (kind == UAM_NET_STAGE_I2S ? uam_fuzz_campaign_i2s_begin(z->camp, entry, limit, bf)
                                           : uam_fuzz_campaign_trim_begin(z->camp, entry, limit, bf))
                != UAM_OK)
        status = UAM_NET_HAVOC_BAD;
    if (status != UAM_NET_HAVOC_OK) {
        uam_log(UAM_LOG_WARN, "fuzz-net", "STAGE %u for entry %u refused (status %u)", kind, entry,
                status);
        send_havoc_done(z, first, status);
        return false;
    }
    z->batch = true;
    z->stage = kind;
    z->first_id = z->next_id = first;
    z->last_ns = now_ns();
    return true;
}

/* STAGE_RESULT: what the stage that just ended found. */
static bool send_stage_result(cf_net *z)
{
    uint8_t *p;
    if (z->stage == UAM_NET_STAGE_I2S) {
        const uam_fuzz_i2s *r = uam_fuzz_campaign_i2s(z->camp);
        if (!reserve(&z->out, &z->out_cap, 5 + UAM_NET_STAGE_RESULT_HDR_LEN + UAM_NET_STAGE_I2S_FIXED_LEN
                                           + r->n_tokens * UAM_NET_STAGE_TOKEN_LEN)) {
            z->broken = true;
            return false;
        }
        p = put64(z->out + 5, z->first_id);
        *p++ = z->stage;
        p = put32(p, r->compares);
        p = put32(p, r->dropped > UINT32_MAX ? UINT32_MAX : (uint32_t)r->dropped);
        p = put32(p, r->candidates);
        p = put16(p, (uint16_t)r->n_tokens);
        for (size_t i = 0; i < r->n_tokens; i++) {
            *p++ = r->tokens[i].width;
            p = put64(p, r->tokens[i].value);
        }
    } else {
        const uam_fuzz_trim *r = uam_fuzz_campaign_trim(z->camp);
        if (!reserve(&z->out, &z->out_cap, 5 + UAM_NET_STAGE_RESULT_HDR_LEN + UAM_NET_STAGE_TRIM_FIXED_LEN
                                           + r->len)) {
            z->broken = true;
            return false;
        }
        p = put64(z->out + 5, z->first_id);
        *p++ = z->stage;
        *p++ = r->ran;
        p = put64(p, r->insns);
        p = put64(p, r->path);
        p = put32(p, (uint32_t)r->len);
        memcpy(p, r->data, r->len);
        p += r->len;
    }
    return send_frame(z, UAM_NET_STAGE_RESULT, (size_t)(p - (z->out + 5)));
}

/* A batch's input worth keeping, with the campaign's judgement of it. */
static bool send_find(cf_net *z, uint64_t id, const uam_fuzz_judgement *j)
{
    const uam_fuzz_batch *b = uam_fuzz_campaign_batch(z->camp);
    uint32_t status;
    const uint8_t reason = verdict_reason(j->verdict, j->detail, &status);
    size_t n = 0, map_size;
    const uint32_t *hits = NULL;
    const uint8_t *map = uam_fuzz_map(z->fuzz, &map_size);
    if (!reason && !(hits = uam_fuzz_map_hits(z->fuzz, map_size, &n))) {   /* ascending */
        z->broken = true;
        return false;
    }
    if (!reserve(&z->out, &z->out_cap, 5 + UAM_NET_FIND_HDR_LEN + b->len + 4 + n * 5)) {
        z->broken = true;
        return false;
    }
    const uam_fuzz_result *r = uam_fuzz_last_result(z->fuzz);
    uint8_t *p = put64(z->out + 5, id);
    *p++ = reason;
    p = put32(p, status);
    p = put64(p, !reason && r ? r->insns : 0);
    p = put64(p, b->ops);
    p = put32(p, j->new_buckets);
    p = put32(p, j->new_classes);
    p = put64(p, j->path);
    p = put32(p, (uint32_t)b->len);
    memcpy(p, b->data, b->len);
    p += b->len;
    p = put32(p, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        p = put32(p, hits[i]);
        *p++ = map[hits[i]];
    }
    return send_frame(z, UAM_NET_FIND, (size_t)(p - (z->out + 5)));
}

/* One mutation of the batch ran: judge it, report it if it is worth it, say that the
 * batch is alive when nothing went out for a second, and close the batch after the last. */
static bool havoc_report(cf_net *z, uint64_t id, uam_fuzz_verdict v, uint64_t detail)
{
    uam_fuzz_judgement j;
    if (uam_fuzz_campaign_batch_judge(z->camp, v, detail, &j) != UAM_OK) {
        z->broken = true;
        return false;
    }
    const uam_fuzz_batch *b = uam_fuzz_campaign_batch(z->camp);
    bool ok = true;
    if (j.report) {
        ok = send_find(z, id, &j);
        z->last_ns = now_ns();
    } else if (b->left) {
        const uint64_t now = now_ns();
        if (now - z->last_ns >= UAM_NET_PROGRESS_NS) {
            uint8_t *p = put64(z->out + 5, z->first_id);
            put32(p, b->execs);
            ok = send_frame(z, UAM_NET_PROGRESS, UAM_NET_PROGRESS_LEN);
            z->last_ns = now;
        }
    }
    if (ok && !b->left) {
        if (z->stage)
            ok = send_stage_result(z);
        if (ok)
            ok = send_havoc_done(z, z->first_id, UAM_NET_HAVOC_OK);
    }
    return ok;
}

/* JUDGE: the campaign's judgement of an INPUT's run, right before its reply. */
static bool send_judge(cf_net *z, uint64_t id, const uam_fuzz_judgement *j)
{
    uint8_t *p = put64(z->out + 5, id);
    p = put32(p, j->new_buckets);
    p = put32(p, j->new_classes);
    p = put64(p, j->path);
    *p = j->novel_crash;
    return send_frame(z, UAM_NET_JUDGE, UAM_NET_JUDGE_LEN);
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
                            | (z->fuzz ? UAM_NET_HELLO_CMP : 0)
                            | (z->camp ? UAM_NET_HELLO_HAVOC | UAM_NET_HELLO_STAGES : 0)));
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
        if (z->batch) {   /* a HAVOC batch runs on, with no frame from the brain between */
            const uint8_t *d;
            if (uam_fuzz_campaign_batch_next(z->camp, &d, len)) {
                *data = (uint8_t *)d;   /* the harness only reads it */
                *id = z->next_id++;
                return 1;
            }
            z->batch = false;           /* cannot happen: the last report closed it */
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
                z->judge = (o & UAM_NET_OPT_JUDGE) && z->camp;
                if (plen >= UAM_NET_OPTIONS_MIN_LEN + 8 && get64(z->in + 4))
                    uam_fuzz_set_iter_insns(z->fuzz, get64(z->in + 4));
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
            const char *why = apply_mutator(z, z->in, plen);
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
            if (!z->camp
                || uam_mutator_corpus_put(uam_fuzz_campaign_mutator(z->camp), get32(z->in),
                                          z->in + UAM_NET_CORPUS_HDR_LEN, clen) != UAM_OK) {
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
            start_havoc(z, z->in);
            continue;   /* the loop's top hands out the batch's first mutation (or the
                         * batch was answered at once, or the stream broke) */
        case UAM_NET_STAGE:
            if (plen < UAM_NET_STAGE_LEN) {
                uam_log(UAM_LOG_ERROR, "fuzz-net", "%s: malformed STAGE (%zu payload bytes)",
                        z->peer, plen);
                z->broken = true;
                return -1;
            }
            start_stage(z, z->in);
            continue;
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
    if (kind == UAM_FUZZ_KIND_ITER && z->batch)
        return havoc_report(z, id, v, detail) ? UAM_OK : UAM_ERR_INVALID;
    /* Every run is judged, so that the campaign's record is whole: what an INPUT showed
     * is not new to the batches either. */
    uam_fuzz_judgement j = { 0 };
    if (kind == UAM_FUZZ_KIND_ITER && z->camp
        && uam_fuzz_campaign_judge(z->camp, v, detail, &j) != UAM_OK) {
        uam_log(UAM_LOG_ERROR, "fuzz-net", "out of memory judging a run");
        return UAM_ERR_NOMEM;
    }
    if (kind == UAM_FUZZ_KIND_ITER && z->cmp_pending) {
        z->cmp_pending = false;
        if (!send_cmps(z, id))
            return UAM_ERR_INVALID;
    }
    if (kind == UAM_FUZZ_KIND_ITER && z->judge && !send_judge(z, id, &j))
        return UAM_ERR_INVALID;
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
    z->camp = uam_fuzz_campaign_create(f);   /* NULL: HELLO does not offer HAVOC */
}

const uam_fuzz_frontend cf_net_frontend = {
    .hello = net_hello,
    .next_input = net_next_input,
    .report = net_report,
    .bye = net_bye,
};
