/*
 * frontend_szr1.c - ember as an SZR1 executor (suzzer's docs/FORMATS.md section 4).
 *
 * Dials a suzzer brain over TCP and serves as a uam_fuzz_frontend: each INPUT frame
 * is one harness input, and each gets exactly one reply - COVERAGE (kind ITER) for an
 * iteration that ended normally, CRASH otherwise:
 *
 *   UAM_FUZZ_OK            COVERAGE, kind ITER
 *   UAM_FUZZ_CRASH_FAULT   CRASH GUEST_FAULT, status = faulting pc (low 32 bits)
 *   UAM_FUZZ_CRASH_ORACLE  CRASH ORACLE,      status = detail (low 32 bits): the crash
 *                                             pc, a halt status or interrupt lines
 *   UAM_FUZZ_HANG          CRASH HANG,        status = the uam_stop_reason
 *   UAM_FUZZ_TIMEOUT       CRASH TIMEOUT,     status 0
 *   UAM_FUZZ_REJECTED      CRASH DELIVERY,    status 0: the route refused the input
 *
 * The boot map goes out once, unsolicited, as COVERAGE id 0 kind INIT. A malformed
 * INPUT is answered with CRASH MALFORMED and nothing runs. A BYE from the brain ends
 * the run; ember sends BYE when it stops first (an error, an interrupt). EOF before a
 * BYE is an error. Unknown frame types are ignored.
 *
 * A dead peer is noticed by TCP keepalive (a brain host that vanished without closing the
 * connection would otherwise leave ember waiting forever); a live brain that stops
 * answering, by the optional per-frame timeout (--fuzz-szr-timeout).
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

#include "lcv_formats.h"
#include "suzzer.h"

#define SZR_FRAME_MAX     (64u << 20)   /* suzzer-relay's limit: stay below it */
#define CONNECT_TRIES     50            /* the brain may still be starting: ~5 s */
#define CONNECT_DELAY_NS  100000000L
#define KEEPALIVE_IDLE_S  60            /* probe after a minute of silence ... */
#define KEEPALIVE_INTVL_S 10            /* ... every 10 s ... */
#define KEEPALIVE_CNT     6             /* ... and give up after 6 unanswered: ~2 min */

struct sz_szr {
    int       fd;
    bool      broken;      /* a frame was cut short: the stream is out of sync */
    bool      peer_bye;    /* the brain said BYE */
    uint16_t  flags;       /* HELLO scheme flags */
    unsigned  timeout_s;   /* per-frame wait for the brain; 0: none */
    uint64_t  gen;         /* COVERAGE generation counter */
    uint8_t  *in;          /* the last frame's payload */
    size_t    in_cap;
    uint8_t  *out;         /* one outgoing frame */
    size_t    out_cap;
    char      peer[300];
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

/* ---- I/O ---------------------------------------------------------------- */

/* All of buf, retrying short writes and interrupted ones (a signal only sets the abort
 * flag; finishing the frame keeps the stream in sync for a final BYE). */
static bool write_all(sz_szr *z, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = send(z->fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                uam_log(UAM_LOG_ERROR, "szr", "%s: the brain took nothing for %u s "
                        "(--fuzz-szr-timeout)", z->peer, z->timeout_s);
            else
                uam_log(UAM_LOG_ERROR, "szr", "%s: send: %s", z->peer, strerror(errno));
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
static int read_all(sz_szr *z, uint8_t *buf, size_t len, bool frame_start)
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
            uam_log(UAM_LOG_ERROR, "szr", "%s: nothing from the brain for %u s (--fuzz-szr-timeout)",
                    z->peer, z->timeout_s);
        else if (n == 0)
            uam_log(UAM_LOG_ERROR, "szr", "%s: connection closed in the middle of a frame", z->peer);
        else
            uam_log(UAM_LOG_ERROR, "szr", "%s: recv: %s", z->peer, strerror(errno));
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
static bool send_frame(sz_szr *z, uint8_t type, size_t payload_len)
{
    if (z->broken)
        return false;
    put32(z->out, (uint32_t)(payload_len + 1));
    z->out[4] = type;
    return write_all(z, z->out, 5 + payload_len);
}

static bool send_crash(sz_szr *z, uint64_t id, uint8_t reason, uint32_t status)
{
    uint8_t *p = put64(z->out + 5, id);
    *p++ = reason;
    put32(p, status);
    return send_frame(z, SZR_CRASH, SZR_CRASH_LEN);
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

sz_szr *sz_szr_connect(const char *hostport, uint16_t flags, unsigned timeout_s, char *err,
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
    sz_szr *z = calloc(1, sizeof(*z));
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
    uam_log(UAM_LOG_INFO, "szr", "connected to the brain at %s", hostport);
    return z;
}

void sz_szr_close(sz_szr *z)
{
    if (!z)
        return;
    if (z->fd >= 0)
        close(z->fd);
    free(z->in);
    free(z->out);
    free(z);
}

/* ---- the frontend --------------------------------------------------- */

static uam_status szr_hello(void *ctx, const uam_fuzz_info *info)
{
    sz_szr *z = ctx;
    const char *arch = !strcmp(info->arch, "rv32imac") ? "rv32" : info->arch;
    size_t alen = strlen(arch);
    if (alen > SZR_HELLO_ARCH_MAX)
        alen = SZR_HELLO_ARCH_MAX;
    size_t cov = (size_t)5 + SZR_COVERAGE_HDR_LEN + info->map_size;
    if (cov - 4 > SZR_FRAME_MAX) {
        uam_log(UAM_LOG_ERROR, "szr", "a %u-byte coverage map does not fit an SZR1 frame",
                info->map_size);
        return UAM_ERR_INVALID;
    }
    const size_t hello = (size_t)5 + SZR_HELLO_FIXED_LEN + SZR_HELLO_ARCH_MAX;
    if (!reserve(&z->out, &z->out_cap, cov > hello ? cov : hello))
        return UAM_ERR_NOMEM;
    if (!write_all(z, (const uint8_t *)SZR_MAGIC, 4))
        return UAM_ERR_INVALID;
    uint8_t *p = put16(z->out + 5, SZR_PROTO_VERSION);
    p = put16(p, z->flags);          /* never MT or LIVE_EMPTY: those describe libcov */
    p = put32(p, info->map_size);
    p = put16(p, 1);                 /* ring_size: one snapshot per input */
    p = put16(p, (uint16_t)alen);
    memcpy(p, arch, alen);
    return send_frame(z, SZR_HELLO, SZR_HELLO_FIXED_LEN + alen) ? UAM_OK : UAM_ERR_INVALID;
}

static int szr_next_input(void *ctx, uint8_t **data, size_t *len, uint64_t *id)
{
    sz_szr *z = ctx;
    for (;;) {
        if (z->broken)
            return -1;
        uint8_t hdr[5];
        int rc = read_all(z, hdr, sizeof(hdr), true);
        if (rc == RD_EOF) {
            uam_log(UAM_LOG_ERROR, "szr", "%s: the brain closed the connection without BYE", z->peer);
            return -1;
        }
        if (rc != RD_OK)
            return -1;
        uint32_t flen = get32(hdr);
        if (flen < 1 || flen > SZR_FRAME_MAX) {
            uam_log(UAM_LOG_ERROR, "szr", "%s: bad frame length %u", z->peer, flen);
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
        case SZR_BYE:
            z->peer_bye = true;
            return 0;
        case SZR_INPUT: {
            uint32_t dlen = plen >= SZR_INPUT_HDR_LEN ? get32(z->in + 8) : 0;
            if (plen < SZR_INPUT_HDR_LEN || dlen > plen - SZR_INPUT_HDR_LEN) {
                uint64_t bad = plen >= 8 ? get64(z->in) : 0;
                uam_log(UAM_LOG_WARN, "szr", "malformed INPUT (id %llu, %zu payload bytes)",
                        (unsigned long long)bad, plen);
                if (!send_crash(z, bad, SZR_CRASH_MALFORMED, 0))
                    return -1;
                continue;
            }
            *id = get64(z->in);
            *len = dlen;
            *data = z->in + SZR_INPUT_HDR_LEN;
            return 1;
        }
        default:
            continue;   /* unknown (or executor-bound) frame types are ignored */
        }
    }
}

static uam_status szr_report(void *ctx, uint64_t id, uam_fuzz_verdict v, uint64_t detail,
                             const uint8_t *map, size_t map_size, int kind)
{
    sz_szr *z = ctx;
    bool ok;
    if (kind == UAM_FUZZ_KIND_INIT || v == UAM_FUZZ_OK) {
        uint8_t *p = put64(z->out + 5, kind == UAM_FUZZ_KIND_INIT ? 0 : id);
        p = put64(p, ++z->gen);
        *p++ = kind == UAM_FUZZ_KIND_INIT ? LIBCOV_SLOT_FLAG_INIT : LIBCOV_SLOT_FLAG_ITER;
        p = put32(p, (uint32_t)map_size);
        memcpy(p, map, map_size);
        ok = send_frame(z, SZR_COVERAGE, SZR_COVERAGE_HDR_LEN + map_size);
    } else {
        uint8_t reason;
        uint32_t status = (uint32_t)detail;
        switch (v) {
        case UAM_FUZZ_CRASH_FAULT:  reason = SZR_CRASH_GUEST_FAULT; break;
        case UAM_FUZZ_CRASH_ORACLE: reason = SZR_CRASH_ORACLE; break;
        case UAM_FUZZ_HANG:         reason = SZR_CRASH_HANG; break;
        case UAM_FUZZ_REJECTED:     reason = SZR_CRASH_DELIVERY; status = 0; break;
        default:                    reason = SZR_CRASH_TIMEOUT; status = 0; break;
        }
        ok = send_crash(z, id, reason, status);
    }
    return ok ? UAM_OK : UAM_ERR_INVALID;
}

static void szr_bye(void *ctx)
{
    sz_szr *z = ctx;
    if (!z->peer_bye && !z->broken && z->out)
        send_frame(z, SZR_BYE, 0);
    shutdown(z->fd, SHUT_WR);
}

const uam_fuzz_frontend sz_szr_frontend = {
    .hello = szr_hello,
    .next_input = szr_next_input,
    .report = szr_report,
    .bye = szr_bye,
};
