/*
 * netdev_backends.c - Host backends for uam_netdev (uam/host.h): null, Linux
 * tap, UDP socket pair, in-process pair, and pcap capture.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/if.h>
#include <linux/if_tun.h>
#endif

#include "uam/host.h"

/* ---- null ---------------------------------------------------------------- */

static bool null_send(uam_netdev *nd, const void *f, size_t len)
{
    (void)nd; (void)f; (void)len;
    return true;
}

static void free_nd(uam_netdev *nd) { free(nd); }

static const uam_netdev_ops null_ops = { .type = "null", .send = null_send, .destroy = free_nd };

uam_netdev *uam_netdev_null_create(void)
{
    uam_netdev *nd = malloc(sizeof(*nd));
    if (nd)
        uam_netdev_init(nd, &null_ops);
    return nd;
}

/* ---- file-descriptor backends: tap, udp ------------------------------------ */

typedef struct fd_nd {
    uam_netdev         nd;
    int                fd;
    bool               udp;
    struct sockaddr_storage peer;
    socklen_t          peer_len;
} fd_nd;

static fd_nd *to_fdn(uam_netdev *nd) { return UAM_CONTAINER_OF(nd, fd_nd, nd); }

static bool fdn_send(uam_netdev *nd, const void *frame, size_t len)
{
    fd_nd *f = to_fdn(nd);
    ssize_t n = f->udp ? sendto(f->fd, frame, len, 0, (struct sockaddr *)&f->peer, f->peer_len)
                       : write(f->fd, frame, len);
    return n == (ssize_t)len;
}

static int fdn_fd(uam_netdev *nd) { return to_fdn(nd)->fd; }

static void fdn_poll(uam_netdev *nd)
{
    fd_nd *f = to_fdn(nd);
    static uint8_t buf[UAM_NETDEV_MTU];
    for (unsigned i = 0; i < UAM_NETDEV_QUEUE; i++) {
        ssize_t n = f->udp ? recv(f->fd, buf, sizeof(buf), 0) : read(f->fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        uam_netdev_input(nd, buf, (size_t)n);
    }
}

static void fdn_destroy(uam_netdev *nd)
{
    fd_nd *f = to_fdn(nd);
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

static const uam_netdev_ops fdn_ops = {
    .type = "fd", .send = fdn_send, .fd = fdn_fd, .poll = fdn_poll, .destroy = fdn_destroy,
};

static fd_nd *fdn_new(int fd, bool udp)
{
    fd_nd *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    uam_netdev_init(&f->nd, &fdn_ops);
    f->fd = fd;
    f->udp = udp;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return f;
}

uam_netdev *uam_netdev_tap_create(const char *ifname)
{
#ifdef __linux__
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        uam_log(UAM_LOG_ERROR, "netdev", "cannot open /dev/net/tun: %s", strerror(errno));
        return NULL;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ifname)
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        uam_log(UAM_LOG_ERROR, "netdev", "cannot attach tap %s: %s", ifname ? ifname : "",
                strerror(errno));
        close(fd);
        return NULL;
    }
    fd_nd *f = fdn_new(fd, false);
    if (!f) {
        close(fd);
        return NULL;
    }
    uam_log(UAM_LOG_INFO, "netdev", "attached to tap %s", ifr.ifr_name);
    return &f->nd;
#else
    (void)ifname;
    uam_log(UAM_LOG_ERROR, "netdev", "tap is only supported on Linux");
    return NULL;
#endif
}

uam_netdev *uam_netdev_udp_create(uint16_t local_port, const char *remote_host, uint16_t remote_port)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", remote_port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(remote_host ? remote_host : "127.0.0.1", port, &hints, &res) != 0 || !res) {
        uam_log(UAM_LOG_ERROR, "netdev", "cannot resolve %s", remote_host);
        return NULL;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(local_port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        uam_log(UAM_LOG_ERROR, "netdev", "cannot bind udp port %u: %s", local_port, strerror(errno));
        if (fd >= 0)
            close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    fd_nd *f = fdn_new(fd, true);
    if (!f) {
        close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    memcpy(&f->peer, res->ai_addr, res->ai_addrlen);
    f->peer_len = res->ai_addrlen;
    freeaddrinfo(res);
    return &f->nd;
}

/* ---- in-process pair ------------------------------------------------------ */

typedef struct pair_nd {
    uam_netdev      nd;
    struct pair_nd *peer;
} pair_nd;

static bool pair_send(uam_netdev *nd, const void *frame, size_t len)
{
    pair_nd *p = UAM_CONTAINER_OF(nd, pair_nd, nd);
    if (!p->peer)
        return false;
    uam_netdev_input(&p->peer->nd, frame, len);
    return true;
}

static void pair_destroy(uam_netdev *nd)
{
    pair_nd *p = UAM_CONTAINER_OF(nd, pair_nd, nd);
    if (p->peer) {
        p->peer->peer = NULL;
        uam_netdev_set_link(&p->peer->nd, false);
    }
    free(p);
}

static const uam_netdev_ops pair_ops = { .type = "pair", .send = pair_send, .destroy = pair_destroy };

uam_status uam_netdev_pair_create(uam_netdev **a, uam_netdev **b)
{
    pair_nd *x = calloc(1, sizeof(*x)), *y = calloc(1, sizeof(*y));
    if (!x || !y) {
        free(x);
        free(y);
        return UAM_ERR_NOMEM;
    }
    uam_netdev_init(&x->nd, &pair_ops);
    uam_netdev_init(&y->nd, &pair_ops);
    x->peer = y;
    y->peer = x;
    *a = &x->nd;
    *b = &y->nd;
    return UAM_OK;
}

/* ---- pcap capture ----------------------------------------------------------- */

static void put_u32le(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void pcap_frame(void *ctx, const uint8_t *frame, size_t len, bool to_host)
{
    (void)to_host;
    FILE *f = ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    put_u32le(f, (uint32_t)tv.tv_sec);
    put_u32le(f, (uint32_t)tv.tv_usec);
    put_u32le(f, (uint32_t)len);
    put_u32le(f, (uint32_t)len);
    fwrite(frame, 1, len, f);
    fflush(f);
}

static void pcap_close(void *ctx) { fclose(ctx); }

uam_status uam_netdev_capture(uam_netdev *nd, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        uam_log(UAM_LOG_ERROR, "netdev", "cannot create %s", path);
        return UAM_ERR_INVALID;
    }
    put_u32le(f, 0xA1B2C3D4u);                 /* magic, microsecond timestamps */
    uint8_t ver[4] = { 2, 0, 4, 0 };           /* version 2.4, little-endian */
    fwrite(ver, 1, 4, f);
    put_u32le(f, 0);                           /* thiszone */
    put_u32le(f, 0);                           /* sigfigs */
    put_u32le(f, UAM_NETDEV_MTU);              /* snaplen */
    put_u32le(f, 1);                           /* LINKTYPE_ETHERNET */
    uam_netdev_set_tap(nd, pcap_frame, pcap_close, f);
    return UAM_OK;
}

/* ---- spec parser ------------------------------------------------------------ */

uam_netdev *uam_netdev_open(const char *spec)
{
    char *copy = strdup(spec);
    if (!copy)
        return NULL;
    char *pcap = strstr(copy, ",pcap=");
    if (pcap) {
        *pcap = 0;
        pcap += 6;
    }
    uam_netdev *nd = NULL;
    if (!strcmp(copy, "null")) {
        nd = uam_netdev_null_create();
    } else if (!strncmp(copy, "tap:", 4)) {
        nd = uam_netdev_tap_create(copy + 4);
    } else if (!strncmp(copy, "udp:", 4)) {
        unsigned lport = 0, rport = 0;
        char rhost[256] = { 0 };
        if (sscanf(copy + 4, "%u:%255[^:]:%u", &lport, rhost, &rport) == 3)
            nd = uam_netdev_udp_create((uint16_t)lport, rhost, (uint16_t)rport);
        else
            uam_log(UAM_LOG_ERROR, "netdev", "expected udp:LPORT:RHOST:RPORT, got '%s'", copy);
    } else {
        uam_log(UAM_LOG_ERROR, "netdev", "unknown backend '%s'", copy);
    }
    if (nd && pcap && uam_netdev_capture(nd, pcap) != UAM_OK) {
        uam_netdev_destroy(nd);
        nd = NULL;
    }
    free(copy);
    return nd;
}
