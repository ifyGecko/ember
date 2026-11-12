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
 * The boot map goes out once, unsolicited, as COVERAGE id 0 kind INIT. A malformed INPUT
 * is answered with CRASH MALFORMED and nothing runs. A BYE from the brain ends the run;
 * ember sends BYE when it stops first (an error, an interrupt). EOF before a BYE is an
 * error. Unknown frame types are ignored.
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
    unsigned  timeout_s;   /* per-frame wait for the brain; 0: none */
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
    free(z);
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
    if (!reserve(&z->out, &z->out_cap, cov > hello ? cov : hello))
        return UAM_ERR_NOMEM;
    if (!write_all(z, (const uint8_t *)UAM_FUZZ_NET_MAGIC, 4))
        return UAM_ERR_INVALID;
    uint8_t *p = put16(z->out + 5, UAM_FUZZ_NET_VERSION);
    p = put16(p, z->flags);
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
        case UAM_NET_INPUT: {
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
            return 1;
        }
        default:
            continue;   /* any other frame type is ignored */
        }
    }
}

static uam_status net_report(void *ctx, uint64_t id, uam_fuzz_verdict v, uint64_t detail,
                             const uint8_t *map, size_t map_size, int kind)
{
    cf_net *z = ctx;
    bool ok;
    if (kind == UAM_FUZZ_KIND_INIT || v == UAM_FUZZ_OK) {
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

const uam_fuzz_frontend cf_net_frontend = {
    .hello = net_hello,
    .next_input = net_next_input,
    .report = net_report,
    .bye = net_bye,
};
