/*
 * netdev_backends.c - Host backends for uam_netdev (uam/host.h): null, Linux
 * tap and tun (with an Ethernet adapter), an inherited tap/tun fd, UDP socket
 * pair, in-process pair, and pcap capture.
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

/* ---- file-descriptor backends: tap, tun, udp ------------------------------- */

/*
 * A tun interface carries IP packets, not Ethernet frames. The adapter makes
 * it look like a cable to one host-side station (TUN_HOST_MAC) that owns
 * every IP address the guest asks for: it answers ARP requests and IPv6
 * neighbour solicitations itself, strips the Ethernet header from the
 * guest's IPv4/IPv6 frames, and wraps packets from the host in a header
 * addressed to the guest's MAC (learned from its frames). Everything else
 * the guest sends (other ethertypes, VLAN tags) has no meaning on a tun
 * link and is dropped.
 */
static const uint8_t TUN_HOST_MAC[6] = { 0x02, 0x75, 0x61, 0x6d, 0x00, 0x01 };

typedef enum { FDN_TAP, FDN_TUN, FDN_UDP } fdn_kind;

typedef struct fd_nd {
    uam_netdev         nd;
    int                fd;
    fdn_kind           kind;
    struct sockaddr_storage peer;
    socklen_t          peer_len;
    uint8_t            guest_mac[6];   /* tun: learned from the guest's frames */
    bool               have_guest_mac;
} fd_nd;

static fd_nd *to_fdn(uam_netdev *nd) { return UAM_CONTAINER_OF(nd, fd_nd, nd); }

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* Host frames enter the guest padded to the Ethernet minimum, as on a wire.
 * `frame` must have room for UAM_NETDEV_MIN_FRAME bytes. */
static void host_input(uam_netdev *nd, uint8_t *frame, size_t len)
{
    if (len < UAM_NETDEV_MIN_FRAME) {
        memset(frame + len, 0, UAM_NETDEV_MIN_FRAME - len);
        len = UAM_NETDEV_MIN_FRAME;
    }
    uam_netdev_input(nd, frame, len);
}

/* An answer from the tun adapter's host station to the guest. */
static void tun_reply(fd_nd *f, uint8_t *frame, size_t len, uint16_t ethertype)
{
    memcpy(frame, f->guest_mac, 6);
    memcpy(frame + 6, TUN_HOST_MAC, 6);
    put16(frame + 12, ethertype);
    host_input(&f->nd, frame, len);
}

/* ARP request (Ethernet/IPv4) for any address: "it is at TUN_HOST_MAC". Probes
 * (sender 0.0.0.0) and announcements (sender = target) are duplicate-address
 * checks and get no answer, or the guest would see a conflict. */
static void tun_arp(fd_nd *f, const uint8_t *a, size_t len)
{
    if (len < 28 || get16(a) != 1 || get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4
        || get16(a + 6) != 1)
        return;
    static const uint8_t zero[4];
    const uint8_t *sha = a + 8, *spa = a + 14, *tpa = a + 24;
    if (!memcmp(spa, zero, 4) || !memcmp(spa, tpa, 4))
        return;
    uint8_t r[UAM_NETDEV_MIN_FRAME];
    uint8_t *p = r + 14;
    put16(p, 1);
    put16(p + 2, 0x0800);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, 2);                 /* reply */
    memcpy(p + 8, TUN_HOST_MAC, 6);
    memcpy(p + 14, tpa, 4);
    memcpy(p + 18, sha, 6);
    memcpy(p + 24, spa, 4);
    tun_reply(f, r, 14 + 28, 0x0806);
}

/* ICMPv6 checksum over the IPv6 pseudo-header. */
static uint16_t icmp6_csum(const uint8_t *src, const uint8_t *dst, const uint8_t *msg, size_t len)
{
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2)
        sum += get16(src + i) + get16(dst + i);
    sum += (uint32_t)len + 58;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += get16(msg + i);
    if (len & 1)
        sum += (uint32_t)msg[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* IPv6 neighbour solicitation for any address: answer with TUN_HOST_MAC.
 * Duplicate-address detection (source ::) gets no answer. Returns true if
 * the packet was a solicitation (handled here, not passed to the host). */
static bool tun_ndp(fd_nd *f, const uint8_t *ip, size_t len)
{
    if (len < 40 + 24 || ip[6] != 58 || ip[7] != 255 || ip[40] != 135)
        return false;
    static const uint8_t unspecified[16];
    const uint8_t *src = ip + 8, *target = ip + 48;
    if (!memcmp(src, unspecified, 16) || !memcmp(src, target, 16))
        return true;
    uint8_t r[14 + 40 + 32];
    uint8_t *h = r + 14, *m = h + 40;
    memset(h, 0, 40 + 32);
    h[0] = 0x60;
    put16(h + 4, 32);
    h[6] = 58;                       /* ICMPv6 */
    h[7] = 255;
    memcpy(h + 8, target, 16);
    memcpy(h + 24, src, 16);
    m[0] = 136;                      /* neighbour advertisement */
    m[4] = 0x60;                     /* solicited, override */
    memcpy(m + 8, target, 16);
    m[24] = 2;                       /* target link-layer address option */
    m[25] = 1;
    memcpy(m + 26, TUN_HOST_MAC, 6);
    put16(m + 2, icmp6_csum(h + 8, h + 24, m, 32));
    tun_reply(f, r, sizeof(r), 0x86DD);
    return true;
}

/* Guest frame -> tun packet. */
static bool tun_send(fd_nd *f, const uint8_t *frame, size_t len)
{
    if (len < 14)
        return false;
    if (!(frame[6] & 1)) {           /* a unicast source: the guest's MAC */
        memcpy(f->guest_mac, frame + 6, 6);
        f->have_guest_mac = true;
    }
    const uint8_t *ip = frame + 14;
    size_t n = len - 14;
    switch (get16(frame + 12)) {
    case 0x0806:
        tun_arp(f, ip, n);
        return true;
    case 0x0800:
        if (n < 20 || (ip[0] >> 4) != 4)
            return false;
        if (get16(ip + 2) < n)
            n = get16(ip + 2);      /* drop the Ethernet padding */
        break;
    case 0x86DD:
        if (n < 40 || (ip[0] >> 4) != 6)
            return false;
        if (40u + get16(ip + 4) < n)
            n = 40u + get16(ip + 4);
        if (tun_ndp(f, ip, n))
            return true;
        break;
    default:
        return false;
    }
    return write(f->fd, ip, n) == (ssize_t)n;
}

/* Tun packet (at buf + 14) -> guest frame. */
static void tun_input(fd_nd *f, uint8_t *buf, size_t n)
{
    const uint8_t *ip = buf + 14;
    uint8_t *dst = buf;
    uint16_t type;
    if (n >= 20 && (ip[0] >> 4) == 4) {
        type = 0x0800;
        if ((ip[16] & 0xf0) == 0xe0) {               /* 224.0.0.0/4 multicast */
            static const uint8_t pfx[3] = { 0x01, 0x00, 0x5e };
            memcpy(dst, pfx, 3);
            dst[3] = ip[17] & 0x7f;
            dst[4] = ip[18];
            dst[5] = ip[19];
        } else if (f->have_guest_mac && get16(ip + 16) != 0xffff) {
            memcpy(dst, f->guest_mac, 6);
        } else {
            memset(dst, 0xff, 6);
        }
    } else if (n >= 40 && (ip[0] >> 4) == 6) {
        type = 0x86DD;
        if (ip[24] == 0xff) {                        /* ff00::/8 multicast */
            dst[0] = dst[1] = 0x33;
            memcpy(dst + 2, ip + 36, 4);
        } else if (f->have_guest_mac) {
            memcpy(dst, f->guest_mac, 6);
        } else {
            memset(dst, 0xff, 6);
        }
    } else {
        return;
    }
    memcpy(buf + 6, TUN_HOST_MAC, 6);
    put16(buf + 12, type);
    host_input(&f->nd, buf, n + 14);
}

static bool fdn_send(uam_netdev *nd, const void *frame, size_t len)
{
    fd_nd *f = to_fdn(nd);
    ssize_t n;
    switch (f->kind) {
    case FDN_TUN: return tun_send(f, frame, len);
    case FDN_UDP: n = sendto(f->fd, frame, len, 0, (struct sockaddr *)&f->peer, f->peer_len); break;
    default:      n = write(f->fd, frame, len); break;
    }
    return n == (ssize_t)len;
}

static int fdn_fd(uam_netdev *nd) { return to_fdn(nd)->fd; }

static void fdn_poll(uam_netdev *nd)
{
    fd_nd *f = to_fdn(nd);
    /* Room for a tun packet behind a synthesized Ethernet header. */
    static uint8_t buf[14 + UAM_NETDEV_MTU];
    uint8_t *rd = f->kind == FDN_TUN ? buf + 14 : buf;
    while (uam_netdev_can_input(nd)) {
        ssize_t n = f->kind == FDN_UDP ? recv(f->fd, rd, UAM_NETDEV_MTU, 0)
                                       : read(f->fd, rd, UAM_NETDEV_MTU);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        if (f->kind == FDN_TUN)
            tun_input(f, buf, (size_t)n);
        else
            host_input(nd, buf, (size_t)n);
    }
}

static void fdn_destroy(uam_netdev *nd)
{
    fd_nd *f = to_fdn(nd);
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

static const uam_netdev_ops tap_ops = {
    .type = "tap", .send = fdn_send, .fd = fdn_fd, .poll = fdn_poll, .destroy = fdn_destroy,
};
static const uam_netdev_ops tun_ops = {
    .type = "tun", .send = fdn_send, .fd = fdn_fd, .poll = fdn_poll, .destroy = fdn_destroy,
};
static const uam_netdev_ops udp_ops = {
    .type = "udp", .send = fdn_send, .fd = fdn_fd, .poll = fdn_poll, .destroy = fdn_destroy,
};

static fd_nd *fdn_new(int fd, fdn_kind kind)
{
    fd_nd *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    uam_netdev_init(&f->nd, kind == FDN_TUN ? &tun_ops : kind == FDN_UDP ? &udp_ops : &tap_ops);
    f->fd = fd;
    f->kind = kind;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return f;
}

#ifdef __linux__
/* Attach to (or create) a tap/tun interface. An interface created with
 * `ip tuntap add ... multi_queue` only accepts multi-queue attachments. */
static int tuntap_open(const char *ifname, short mode, char name_out[IFNAMSIZ])
{
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        uam_log(UAM_LOG_ERROR, "netdev", "cannot open /dev/net/tun: %s", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = (short)(mode | IFF_NO_PI);
    if (ifname)
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    int rc = ioctl(fd, TUNSETIFF, &ifr);
    if (rc < 0 && errno == EINVAL) {
        ifr.ifr_flags |= IFF_MULTI_QUEUE;
        rc = ioctl(fd, TUNSETIFF, &ifr);
        if (rc < 0)
            errno = EINVAL;
    }
    if (rc < 0) {
        int err = errno;
        uam_log(UAM_LOG_ERROR, "netdev", "cannot attach %s %s: %s%s", mode == IFF_TAP ? "tap" : "tun",
                ifname && *ifname ? ifname : "(new)", strerror(err),
                err == EPERM ? " (create it first: ip tuntap add dev NAME mode tap|tun user $USER,"
                               " or run with CAP_NET_ADMIN)"
                : err == EINVAL || err == EBUSY ? " (does the interface exist with the other mode,"
                                                  " or is it in use?)" : "");
        close(fd);
        return -1;
    }
    snprintf(name_out, IFNAMSIZ, "%s", ifr.ifr_name);
    return fd;
}
#endif

static uam_netdev *tuntap_create(const char *ifname, bool tun)
{
#ifdef __linux__
    char name[IFNAMSIZ];
    int fd = tuntap_open(ifname, tun ? IFF_TUN : IFF_TAP, name);
    if (fd < 0)
        return NULL;
    fd_nd *f = fdn_new(fd, tun ? FDN_TUN : FDN_TAP);
    if (!f) {
        close(fd);
        return NULL;
    }
    uam_log(UAM_LOG_INFO, "netdev", "attached to %s %s", tun ? "tun" : "tap", name);
    return &f->nd;
#else
    (void)ifname;
    uam_log(UAM_LOG_ERROR, "netdev", "%s is only supported on Linux", tun ? "tun" : "tap");
    return NULL;
#endif
}

uam_netdev *uam_netdev_tap_create(const char *ifname)
{
    return tuntap_create(ifname, false);
}

uam_netdev *uam_netdev_tun_create(const char *ifname)
{
    return tuntap_create(ifname, true);
}

uam_netdev *uam_netdev_tap_fd_create(int fd, bool tun)
{
    if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
        uam_log(UAM_LOG_ERROR, "netdev", "fd %d is not open", fd);
        return NULL;
    }
    fd_nd *f = fdn_new(fd, tun ? FDN_TUN : FDN_TAP);
    return f ? &f->nd : NULL;
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
    fd_nd *f = fdn_new(fd, FDN_UDP);
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
    } else if (!strncmp(copy, "tun:", 4)) {
        nd = uam_netdev_tun_create(copy + 4);
    } else if (!strncmp(copy, "tap-fd:", 7) || !strncmp(copy, "tun-fd:", 7)) {
        char *end;
        long fd = strtol(copy + 7, &end, 10);
        if (end != copy + 7 && !*end)
            nd = uam_netdev_tap_fd_create((int)fd, copy[1] == 'u');
        else
            uam_log(UAM_LOG_ERROR, "netdev", "expected %.6s:FD, got '%s'", copy, copy);
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
