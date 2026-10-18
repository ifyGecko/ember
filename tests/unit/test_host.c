/*
 * test_host.c - Unit tests for the host side and its interfaces: chardev
 * buffering and flow control, the 16550 over a chardev, netdev pairs and
 * pcap capture, the tun Ethernet adapter, bus DMA and the virtio-net device,
 * the host loop (idle hart woken by socket input, deadlock detection, a
 * stalled hart blocking on host input instead of livelocking), the
 * board registry and board configuration lookups.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uam/boards.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/virtio_net.h"
#include "uam/host.h"
#include "uam/isa/mips32.h"
#include "uam/machine.h"
#include "uam/signal.h"
#include "uam/snapshot.h"

static int n_checks, n_failed;

#define CHECK(cond, ...) do {                                              \
        n_checks++;                                                        \
        if (!(cond)) {                                                     \
            n_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

/* ---- chardev flow control ---------------------------------------------- */

typedef struct sink {
    uint8_t data[8192];
    size_t  len;
    size_t  window;   /* bytes accepted before the next accept_input */
} sink;

static size_t sink_can(void *ctx) { return ((sink *)ctx)->window; }

static void sink_rx(void *ctx, const uint8_t *buf, size_t len)
{
    sink *s = ctx;
    memcpy(s->data + s->len, buf, len);
    s->len += len;
    s->window -= len;
}

static void test_chardev_flow(void)
{
    static uint8_t input[6000];
    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)(i * 7);
    uam_chardev *cd = uam_chardev_buffer_create(NULL, input, sizeof(input));
    static sink s;
    s.window = 10;
    uam_chardev_set_frontend(cd, sink_can, sink_rx, &s);
    CHECK(s.len == 10, "frontend got only its window (%zu)", s.len);
    for (int i = 0; i < 2000 && s.len < sizeof(input); i++) {
        s.window = 7;
        uam_chardev_accept_input(cd);
    }
    CHECK(s.len == sizeof(input), "all input delivered (%zu)", s.len);
    CHECK(memcmp(s.data, input, sizeof(input)) == 0, "input in order, nothing lost");
    uam_chardev_destroy(cd);
}

/* ---- 16550 over a chardev ---------------------------------------------- */

static void test_uart_chardev(void)
{
    FILE *out = tmpfile();
    uam_chardev *cd = uam_chardev_buffer_create(out, "ok", 2);
    uam_device *u = uam_uart16550_create("uart", cd);
    uint64_t v = 0;
    u->ops->read(u, 5, 1, &v);
    CHECK(v & 1, "LSR.DR set by chardev input");
    u->ops->read(u, 0, 1, &v);
    CHECK(v == 'o', "RBR = first byte");
    u->ops->read(u, 0, 1, &v);
    CHECK(v == 'k', "RBR = second byte");
    u->ops->write(u, 0, 1, 'Z');
    u->ops->destroy(u);
    uam_chardev_destroy(cd);   /* flushes */
    rewind(out);
    CHECK(fgetc(out) == 'Z', "THR reaches the chardev output");
    fclose(out);
}

/* ---- held console input ------------------------------------------------- */

/* Fixed input held while the machine is set up survives whatever setup does
 * to the UART's receive FIFO (a device reset, a snapshot restore). */
static void test_console_hold(void)
{
    uam_chardev *cd = uam_chardev_buffer_create(NULL, "ok", 2);
    uam_chardev_hold(cd, true);
    uam_device *u = uam_uart16550_create("uart", cd);
    uint64_t v = 0;
    u->ops->read(u, 5, 1, &v);
    CHECK(!(v & 1), "held: nothing delivered when the UART attaches");
    u->ops->write(u, 2, 1, 0x03);        /* FCR: FIFO on, clear RX (what a reset may do) */
    uam_chardev_hold(cd, false);
    u->ops->read(u, 5, 1, &v);
    CHECK(v & 1, "released: input delivered");
    u->ops->read(u, 0, 1, &v);
    CHECK(v == 'o', "first byte after release");
    u->ops->read(u, 0, 1, &v);
    CHECK(v == 'k', "second byte after release");
    u->ops->destroy(u);
    uam_chardev_destroy(cd);
}

#define RV32REF_UART UINT64_C(0x10000000)

/* rv32-ref with `cd` as its console, restored to `snap`; returns the bytes
 * then waiting in the UART (up to cap). */
static size_t restored_uart_input(uam_chardev *cd, const uam_snapshot *snap, bool hold, char *buf,
                                  size_t cap)
{
    uam_board_config cfg = { .console = cd };
    uam_machine m;
    if (hold)
        uam_chardev_hold(cd, true);
    CHECK(uam_board_find("rv32-ref")->build(&m, &cfg) == UAM_OK, "build rv32-ref");
    CHECK(uam_snapshot_restore(&m, snap) == UAM_OK, "restore");
    if (hold)
        uam_chardev_hold(cd, false);
    size_t n = 0;
    uint64_t lsr = 0, c = 0;
    while (n < cap && uam_bus_read(&m.bus, RV32REF_UART + 5, 1, 0, &lsr) == UAM_OK && (lsr & 1)) {
        uam_bus_read(&m.bus, RV32REF_UART, 1, 0, &c);
        buf[n++] = (char)c;
    }
    uam_machine_fini(&m);
    return n;
}

static void test_console_snapshot(void)
{
    uam_boards_register_stock();
    uam_chardev *quiet = uam_chardev_buffer_create(NULL, NULL, 0);
    uam_board_config cfg = { .console = quiet };
    uam_machine m;
    CHECK(uam_board_find("rv32-ref")->build(&m, &cfg) == UAM_OK, "build rv32-ref");
    uam_snapshot *snap = NULL;
    CHECK(uam_snapshot_take(&m, &snap) == UAM_OK, "take");
    uam_machine_fini(&m);
    uam_chardev_destroy(quiet);
    if (!snap)
        return;

    char buf[16];
    uam_chardev *cd = uam_chardev_buffer_create(NULL, "help\n", 5);
    size_t n = restored_uart_input(cd, snap, true, buf, sizeof(buf));
    CHECK(n == 5 && !memcmp(buf, "help\n", 5), "held input survives the restore (%zu bytes)", n);
    uam_chardev_destroy(cd);

    /* Unheld, the restore replaces the FIFO the input went into at build time. */
    cd = uam_chardev_buffer_create(NULL, "help\n", 5);
    n = restored_uart_input(cd, snap, false, buf, sizeof(buf));
    CHECK(n == 0, "unheld input is lost to the restore (the bug holding prevents)");
    uam_chardev_destroy(cd);
    uam_snapshot_free(snap);
}

/* ---- netdev ------------------------------------------------------------- */

typedef struct nic {
    unsigned frames, last_len;
    bool     ready, link;
} nic;

static bool nic_can(void *ctx) { return ((nic *)ctx)->ready; }
static void nic_rx(void *ctx, const uint8_t *f, size_t len)
{
    (void)f;
    nic *n = ctx;
    n->frames++;
    n->last_len = (unsigned)len;
}
static void nic_link(void *ctx, bool up) { ((nic *)ctx)->link = up; }

static void test_netdev(const char *dir)
{
    uam_netdev *a, *b;
    CHECK(uam_netdev_pair_create(&a, &b) == UAM_OK, "pair");
    nic na = { .ready = true }, nb = { .ready = false };
    uam_netdev_set_frontend(a, nic_can, nic_rx, nic_link, &na);
    uam_netdev_set_frontend(b, nic_can, nic_rx, nic_link, &nb);
    CHECK(na.link && nb.link, "links up");

    char pcap[512];
    snprintf(pcap, sizeof(pcap), "%s/cap.pcap", dir);
    CHECK(uam_netdev_capture(a, pcap) == UAM_OK, "capture");

    uint8_t frame[60] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(uam_netdev_send(a, frame, sizeof(frame)), "send a->b");
    CHECK(uam_netdev_send(a, frame, 42), "send a->b again");
    CHECK(nb.frames == 0, "b not ready: frames queued");
    nb.ready = true;
    uam_netdev_accept_input(b);
    CHECK(nb.frames == 2 && nb.last_len == 42, "queued frames delivered in order");
    CHECK(uam_netdev_send(b, frame, 60), "send b->a");
    CHECK(na.frames == 1, "a received");

    uam_netdev_destroy(a);     /* closes the capture */
    CHECK(!nb.link, "peer link down when the other end goes away");
    CHECK(!uam_netdev_send(b, frame, 60), "send on a dead link fails");
    uam_netdev_destroy(b);

    struct stat st;
    CHECK(stat(pcap, &st) == 0 && st.st_size == 24 + 3 * 16 + 60 + 42 + 60,
          "pcap holds 3 frames (size %lld)", (long long)st.st_size);
}

/* ---- tun adapter (over a datagram socketpair standing in for /dev/net/tun) -- */

typedef struct grab {
    unsigned frames;
    size_t   len;
    uint8_t  f[UAM_NETDEV_MTU];
} grab;

static bool grab_can(void *ctx) { (void)ctx; return true; }
static void grab_rx(void *ctx, const uint8_t *f, size_t len)
{
    grab *g = ctx;
    g->frames++;
    g->len = len;
    memcpy(g->f, f, len);
}

static const uint8_t GUEST_MAC[6] = { 0x02, 0x75, 0x61, 0x6d, 0x01, 0x00 };

static size_t eth(uint8_t *f, const uint8_t *dst, uint16_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, GUEST_MAC, 6);
    f[12] = (uint8_t)(type >> 8);
    f[13] = (uint8_t)type;
    return 14;
}

static uint16_t sum16(const uint8_t *p, size_t n, uint32_t sum)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}

static void test_tun_adapter(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair");
    uam_netdev *nd = uam_netdev_tap_fd_create(sv[0], true);
    CHECK(nd != NULL && !strcmp(nd->ops->type, "tun"), "tun over an fd");
    if (!nd)
        return;
    grab g = { 0 };
    uam_netdev_set_frontend(nd, grab_can, grab_rx, NULL, &g);
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t f[128], r[128];

    /* ARP request 10.0.0.2 -> who has 10.0.0.1: answered locally, after send returns. */
    memset(f, 0, sizeof(f));
    size_t n = eth(f, bcast, 0x0806);
    static const uint8_t arp[28] = { 0, 1, 8, 0, 6, 4, 0, 1, 0x02, 0x75, 0x61, 0x6d, 0x01, 0x00,
                                     10, 0, 0, 2, 0, 0, 0, 0, 0, 0, 10, 0, 0, 1 };
    memcpy(f + n, arp, 28);
    CHECK(uam_netdev_send(nd, f, 60), "ARP request accepted");
    CHECK(g.frames == 1 && g.len == 60, "ARP reply delivered, padded (%u, %zu)", g.frames, g.len);
    CHECK(!memcmp(g.f, GUEST_MAC, 6) && g.f[12] == 0x08 && g.f[13] == 0x06 && g.f[21] == 2,
          "ARP reply to the guest");
    CHECK(!memcmp(g.f + 14 + 8, g.f + 6, 6) && !memcmp(g.f + 14 + 14, arp + 24, 4)
          && !memcmp(g.f + 14 + 24, arp + 14, 4), "ARP reply: target IP is at the host MAC");
    CHECK(recv(sv[1], r, sizeof(r), MSG_DONTWAIT) < 0, "ARP never reaches the tun");

    /* A probe (sender 0.0.0.0) is duplicate-address detection: no answer. */
    memset(f + n + 14, 0, 4);
    uam_netdev_send(nd, f, 60);
    CHECK(g.frames == 1, "ARP probe not answered");

    /* IPv4 frame -> tun packet, Ethernet padding removed. */
    memset(f, 0, sizeof(f));
    n = eth(f, g.f + 6, 0x0800);
    f[n] = 0x45;
    f[n + 3] = 28;                   /* total length */
    f[n + 9] = 17;
    CHECK(uam_netdev_send(nd, f, 60), "IPv4 sent");
    CHECK(recv(sv[1], r, sizeof(r), MSG_DONTWAIT) == 28 && r[0] == 0x45, "tun got the bare packet");

    /* Tun packet -> guest frame addressed to the learned MAC. */
    memset(r, 0, 28);
    r[0] = 0x45;
    r[3] = 28;
    r[16] = 10; r[19] = 2;
    CHECK(send(sv[1], r, 28, 0) == 28, "host packet");
    uam_netdev_poll(nd);
    CHECK(g.frames == 2 && g.len == 60 && !memcmp(g.f, GUEST_MAC, 6) && g.f[12] == 0x08
          && g.f[13] == 0 && g.f[14] == 0x45, "host packet framed for the guest");

    /* IPv6 neighbour solicitation -> advertisement with a valid checksum. */
    memset(f, 0, sizeof(f));
    n = eth(f, bcast, 0x86DD);
    uint8_t *ip = f + n;
    ip[0] = 0x60;
    ip[5] = 24;
    ip[6] = 58;
    ip[7] = 255;
    ip[8] = 0xfe; ip[9] = 0x80; ip[23] = 2;     /* src fe80::2 */
    ip[24] = 0xff; ip[25] = 0x02; ip[39] = 1;   /* dst (any multicast) */
    ip[40] = 135;
    ip[48] = 0xfe; ip[49] = 0x80; ip[63] = 1;   /* target fe80::1 */
    CHECK(uam_netdev_send(nd, f, n + 64), "NS accepted");
    CHECK(g.frames == 3 && g.f[12] == 0x86 && g.f[14 + 40] == 136, "NA delivered");
    uint32_t pseudo = 32 + 58 + sum16(g.f + 14 + 8, 32, 0);
    CHECK(sum16(g.f + 14 + 40, 32, pseudo) == 0xffff, "NA checksum valid");
    CHECK(recv(sv[1], r, sizeof(r), MSG_DONTWAIT) < 0, "NS never reaches the tun");

    /* Other ethertypes have no meaning on a tun link. */
    eth(f, bcast, 0x88cc);
    CHECK(!uam_netdev_send(nd, f, 60), "LLDP dropped");

    uam_netdev_set_frontend(nd, NULL, NULL, NULL, NULL);
    uam_netdev_destroy(nd);
    close(sv[1]);
}

/* ---- bus DMA and virtio-net ------------------------------------------------ */

#define VQ_RAM    0x1000u
#define RXQ_DESC  0x1000u
#define RXQ_AVAIL 0x1100u
#define RXQ_USED  0x1200u
#define TXQ_DESC  0x2000u
#define TXQ_AVAIL 0x2100u
#define TXQ_USED  0x2200u
#define RX_BUF    0x3000u
#define TX_BUF    0x4000u

static int irq_level;
static void irq_sink(void *ctx, unsigned in, int level) { (void)ctx; (void)in; irq_level = level; }

static uint32_t reg(uam_device *d, unsigned off)
{
    uint64_t v = 0;
    d->ops->read(d, off, 4, &v);
    return (uint32_t)v;
}
static void wreg(uam_device *d, unsigned off, uint32_t v) { d->ops->write(d, off, 4, v); }

static void put_le16(uam_bus *bus, uint64_t a, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    uam_bus_dma_write(bus, a, b, 2);
}
static uint16_t get_le16(uam_bus *bus, uint64_t a)
{
    uint8_t b[2] = { 0 };
    uam_bus_dma_read(bus, a, b, 2);
    return (uint16_t)(b[0] | b[1] << 8);
}
static void put_desc(uam_bus *bus, uint64_t a, uint32_t addr, uint32_t len, uint16_t flags)
{
    uint8_t d[16] = { (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
                      0, 0, 0, 0, (uint8_t)len, (uint8_t)(len >> 8), 0, 0, (uint8_t)flags, 0, 0, 0 };
    uam_bus_dma_write(bus, a, d, 16);
}

static void setup_queue(uam_device *d, unsigned i, uint32_t desc, uint32_t avail, uint32_t used)
{
    wreg(d, 0x030, i);
    CHECK(reg(d, 0x034) >= 8, "queue %u max", i);
    wreg(d, 0x038, 8);
    wreg(d, 0x080, desc);
    wreg(d, 0x090, avail);
    wreg(d, 0x0a0, used);
    wreg(d, 0x044, 1);
}

static void test_virtio_net(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    CHECK(uam_bus_map_ram(&bus, "ram", 0, 0x10000) == UAM_OK, "ram");
    uint8_t x[4] = { 1, 2, 3, 4 }, y[4] = { 0 };
    CHECK(uam_bus_dma_write(&bus, 0xfffe, x, 2) == UAM_OK, "dma write");
    CHECK(uam_bus_dma_read(&bus, 0xfffe, y, 2) == UAM_OK && y[1] == 2, "dma read back");
    CHECK(uam_bus_dma_write(&bus, 0xfffe, x, 4) == UAM_ERR_FAULT, "dma past RAM faults");

    CHECK(uam_virtio_net_create("virtio1", &bus, NULL, NULL) == NULL, "a netdev is required");

    uam_netdev *a, *b;
    uam_netdev_pair_create(&a, &b);
    grab peer = { 0 };
    uam_netdev_set_frontend(b, grab_can, grab_rx, NULL, &peer);
    uam_device *d = uam_virtio_net_create("virtio0", &bus, a, NULL);
    uam_signal_connect(uam_virtio_net_irq_line(d), irq_sink, NULL, 0);
    CHECK(reg(d, 0x004) == 2 && reg(d, 0x008) == 1, "version 2, net");
    uint64_t mac = 0;
    d->ops->read(d, 0x100, 1, &mac);
    CHECK(mac == 0x02, "config MAC byte 0");
    wreg(d, 0x014, 1);
    CHECK(reg(d, 0x010) & 1, "VERSION_1 offered");

    /* Driver bring-up: ACK | DRIVER, features, FEATURES_OK, queues, DRIVER_OK. */
    wreg(d, 0x070, 3);
    wreg(d, 0x024, 0); wreg(d, 0x020, (1u << 5) | (1u << 16));
    wreg(d, 0x024, 1); wreg(d, 0x020, 1);
    wreg(d, 0x070, 0xb);
    setup_queue(d, 0, RXQ_DESC, RXQ_AVAIL, RXQ_USED);
    setup_queue(d, 1, TXQ_DESC, TXQ_AVAIL, TXQ_USED);
    wreg(d, 0x070, 0xf);

    /* Host -> guest before any receive buffer: held in the netdev queue. */
    uint8_t frame[60];
    for (unsigned i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)i;
    CHECK(uam_netdev_send(b, frame, sizeof(frame)), "peer sends");
    CHECK(get_le16(&bus, RXQ_USED + 2) == 0, "no buffer, nothing received yet");
    put_desc(&bus, RXQ_DESC, RX_BUF, 1526, 2 /* WRITE */);
    put_le16(&bus, RXQ_AVAIL + 4, 0);
    put_le16(&bus, RXQ_AVAIL + 2, 1);
    wreg(d, 0x050, 0);
    CHECK(get_le16(&bus, RXQ_USED + 2) == 1, "frame received into the new buffer");
    uint8_t got[12 + 60];
    uam_bus_dma_read(&bus, RX_BUF, got, sizeof(got));
    CHECK(get_le16(&bus, RXQ_USED + 8) == 72 && got[10] == 1 && !memcmp(got + 12, frame, 60),
          "header (num_buffers 1) and frame");
    CHECK(irq_level == 1 && (reg(d, 0x060) & 1), "used-buffer interrupt");
    wreg(d, 0x064, 1);
    CHECK(irq_level == 0, "interrupt acknowledged");

    /* Guest -> host: header and frame in two descriptors. */
    uam_bus_dma_write(&bus, TX_BUF, got, 12);
    uam_bus_dma_write(&bus, TX_BUF + 0x100, frame, 42);
    put_desc(&bus, TXQ_DESC, TX_BUF, 12, 1 /* NEXT */);
    uint8_t nx[2] = { 1, 0 };
    uam_bus_dma_write(&bus, TXQ_DESC + 14, nx, 2);
    put_desc(&bus, TXQ_DESC + 16, TX_BUF + 0x100, 42, 0);
    put_le16(&bus, TXQ_AVAIL + 4, 0);
    put_le16(&bus, TXQ_AVAIL + 2, 1);
    wreg(d, 0x050, 1);
    CHECK(peer.frames == 1 && peer.len == 42 && !memcmp(peer.f, frame, 42), "frame transmitted");
    CHECK(get_le16(&bus, TXQ_USED + 2) == 1, "tx buffer returned");

    /* Link down: config status and a configuration-change interrupt. */
    wreg(d, 0x064, 3);
    uam_netdev_set_link(a, false);
    uint64_t st = 1;
    d->ops->read(d, 0x106, 2, &st);
    CHECK(st == 0 && (reg(d, 0x060) & 2), "link down reported");

    /* A ring pointing outside RAM: the device stops rather than crashing. */
    put_desc(&bus, TXQ_DESC, 0x900000, 60, 0);
    put_le16(&bus, TXQ_AVAIL + 6, 0);
    put_le16(&bus, TXQ_AVAIL + 2, 2);
    wreg(d, 0x050, 1);
    CHECK(reg(d, 0x070) & 0x40, "DEVICE_NEEDS_RESET on a DMA fault");
    wreg(d, 0x070, 0);
    CHECK(reg(d, 0x070) == 0 && irq_level == 0, "status 0 resets");

    d->ops->destroy(d);
    uam_netdev_destroy(a);
    uam_netdev_destroy(b);
    uam_bus_fini(&bus);
}

/* ---- host loop: an idle hart woken by socket input ----------------------- */

typedef struct wake_dev {
    uam_device   dev;
    uam_machine *m;
    int          got;
} wake_dev;

static size_t wake_can(void *ctx) { (void)ctx; return 1; }
static void wake_rx(void *ctx, const uint8_t *buf, size_t len)
{
    (void)len;
    wake_dev *w = ctx;
    w->got = buf[0];
    uam_machine_stop_cb(w->m, buf[0]);   /* the device ends the run */
}
static uam_status wake_read(uam_device *d, uint64_t o, unsigned s, uint64_t *v)
{ (void)d; (void)o; (void)s; *v = 0; return UAM_OK; }
static uam_status wake_write(uam_device *d, uint64_t o, unsigned s, uint64_t v)
{ (void)d; (void)o; (void)s; (void)v; return UAM_OK; }
static void wake_destroy(uam_device *d) { free(d); }
static const uam_device_ops wake_ops = { .type = "test,wake", .read = wake_read,
                                         .write = wake_write, .destroy = wake_destroy };

/* A MIPS machine that executes `wait` forever with no timer armed. */
static void waiting_machine(uam_machine *m)
{
    CHECK(uam_machine_init(m, &uam_isa_mips32be) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1 << 20) == UAM_OK, "ram");
    static const uint8_t prog[] = { 0x42, 0x00, 0x00, 0x20,   /* wait */
                                    0x10, 0x00, 0xFF, 0xFE,   /* b wait */
                                    0x00, 0x00, 0x00, 0x00 };
    uam_bus_load_bytes(&m->bus, 0, prog, sizeof(prog));
    uam_machine_reset(m, 0x80000000u);
}

static void test_host_loop(const char *dir)
{
    /* No host: waiting with nothing pending is a deadlock. */
    uam_machine m;
    waiting_machine(&m);
    CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_DEADLOCK, "deadlock without host");
    uam_machine_fini(&m);

    /* A host loop with no fds cannot wake the hart either. */
    waiting_machine(&m);
    uam_host *h = uam_host_create();
    uam_host_attach(h, &m.exec);
    CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_DEADLOCK, "deadlock with an fd-less host");
    uam_host_destroy(h);
    uam_machine_fini(&m);

    /* A socket chardev: the idle hart blocks in the host loop until input. */
    char path[512], spec[520];
    snprintf(path, sizeof(path), "%s/console.sock", dir);
    snprintf(spec, sizeof(spec), "unix:%s", path);
    uam_chardev *cd = uam_chardev_open(spec);
    CHECK(cd != NULL, "unix socket chardev");
    if (!cd)
        return;
    waiting_machine(&m);
    wake_dev *w = calloc(1, sizeof(*w));
    uam_device_init(&w->dev, &wake_ops, "wake");
    w->m = &m;
    uam_machine_add_device(&m, &w->dev);
    uam_chardev_set_frontend(cd, wake_can, wake_rx, w);

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) < sizeof(a.sun_path))
        memcpy(a.sun_path, path, strlen(path) + 1);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0, "client connects");
    CHECK(write(c, "!", 1) == 1, "client writes");

    h = uam_host_create();
    uam_host_add_chardev(h, cd);
    uam_host_attach(h, &m.exec);
    uam_stop_reason why = uam_exec_run(&m.exec, 1000);
    CHECK(why == UAM_STOP_REQUEST && w->got == '!', "host input woke the run (why=%d got=%d)",
          why, w->got);
    uam_host_destroy(h);
    uam_chardev_set_frontend(cd, NULL, NULL, NULL);
    uam_machine_fini(&m);
    uam_chardev_destroy(cd);
    close(c);
}

/* ---- host loop: a stalled hart blocks on host input ---------------------- */

/* A compare match that re-arms itself forever, its interrupt line unwired. */
typedef struct rearm {
    uam_timer  t;
    uam_sched *s;
    uint64_t   fires;
} rearm;

static void rearm_fire(void *ctx)
{
    rearm *r = ctx;
    r->fires++;
    uam_timer_arm(r->s, &r->t, uam_sched_now(r->s) + 100);
}

static void stalled_machine(uam_machine *m, rearm *r)
{
    waiting_machine(m);
    m->exec.max_stall = 1000;
    *r = (rearm){ .s = &m->sched };
    uam_timer_init(&r->t, rearm_fire, r);
    uam_timer_arm(&m->sched, &r->t, 100);
}

static uam_machine *volatile g_sig_machine;
static void on_sigusr1(int sig) { (void)sig; uam_exec_stop(&g_sig_machine->exec, 130); }

/* Fork a child that, after 300 ms, writes '!' to fd (fd >= 0) or signals us. */
static pid_t later(int fd)
{
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        usleep(300000);
        if (fd >= 0) {
            if (write(fd, "!", 1) != 1)
                _exit(1);
        } else {
            kill(parent, SIGUSR1);
        }
        _exit(0);
    }
    CHECK(pid > 0, "fork");
    return pid;
}

static void test_host_stall(const char *dir)
{
    /* No input source: timers cannot wake the hart, nothing else can. */
    uam_machine m;
    rearm r;
    stalled_machine(&m, &r);
    uam_host *h = uam_host_create();
    uam_host_attach(h, &m.exec);
    uam_stop_reason why = uam_exec_run(&m.exec, 1000);
    CHECK(why == UAM_STOP_LIVELOCK, "livelock with an fd-less host (why=%d)", why);
    CHECK(r.fires == 1000, "after max_stall timer events (%llu)", (unsigned long long)r.fires);
    uam_host_destroy(h);
    uam_machine_fini(&m);

    /* A console that may still deliver input: after max_stall timer events
     * the hart stops skipping time and sleeps until input arrives. */
    char path[512], spec[520];
    snprintf(path, sizeof(path), "%s/stall.sock", dir);
    snprintf(spec, sizeof(spec), "unix:%s", path);
    uam_chardev *cd = uam_chardev_open(spec);
    CHECK(cd != NULL, "unix socket chardev");
    if (!cd)
        return;
    stalled_machine(&m, &r);
    wake_dev *w = calloc(1, sizeof(*w));
    uam_device_init(&w->dev, &wake_ops, "wake");
    w->m = &m;
    uam_machine_add_device(&m, &w->dev);
    uam_chardev_set_frontend(cd, wake_can, wake_rx, w);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) < sizeof(a.sun_path))
        memcpy(a.sun_path, path, strlen(path) + 1);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0, "client connects");
    h = uam_host_create();
    uam_host_add_chardev(h, cd);
    uam_host_attach(h, &m.exec);

    pid_t pid = later(c);
    why = uam_exec_run(&m.exec, 1000);
    waitpid(pid, NULL, 0);
    CHECK(why == UAM_STOP_REQUEST && w->got == '!', "late input reached the stalled guest "
          "(why=%d got=%d)", why, w->got);
    /* Spinning for 300 ms would have skipped far more than max_stall periods. */
    CHECK(r.fires <= 1001, "blocked instead of spinning (%llu timer events)",
          (unsigned long long)r.fires);

    /* A signal interrupts that wait (SIGINT/SIGTERM in the runner). */
    w->got = 0;
    m.exec.stop_request = false;
    r.fires = 0;
    g_sig_machine = &m;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;      /* no SA_RESTART: poll() returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old);
    pid = later(-1);
    why = uam_exec_run(&m.exec, 1000);
    waitpid(pid, NULL, 0);
    sigaction(SIGUSR1, &old, NULL);
    g_sig_machine = NULL;
    CHECK(why == UAM_STOP_REQUEST && m.exec.stop_status == 130 && w->got == 0,
          "a signal ends the wait (why=%d)", why);
    CHECK(r.fires <= 1001, "blocked until the signal (%llu timer events)",
          (unsigned long long)r.fires);

    uam_host_destroy(h);
    uam_chardev_set_frontend(cd, NULL, NULL, NULL);
    uam_machine_fini(&m);
    uam_chardev_destroy(cd);
    close(c);
}

/* ---- registry and board configuration ---------------------------------- */

static void test_registry(void)
{
    CHECK(uam_board_find("rv32-ref") == NULL, "library has no boards by default");
    uam_boards_register_stock();
    CHECK(uam_board_find("rv32-ref") == &uam_board_rv32_ref, "stock boards registered");
    CHECK(uam_board_find("mipsel-ref") != NULL, "mipsel-ref registered");
    CHECK(uam_board_register(&uam_board_rv32_ref) == UAM_OK, "re-registering replaces");

    uam_board_option opts[] = { { "boot", "flash" }, { "boot", "ram" } };
    uam_board_image imgs[] = { { "flash", "fw.bin" } };
    uam_board_config cfg = { .options = opts, .n_options = 2, .images = imgs, .n_images = 1 };
    CHECK(!strcmp(uam_board_config_option(&cfg, "boot"), "ram"), "last option wins");
    CHECK(uam_board_config_option(&cfg, "x") == NULL, "missing option");
    CHECK(!strcmp(uam_board_config_image(&cfg, "flash"), "fw.bin"), "named image");
    uam_board_netdev nds[] = { { "wan", (uam_netdev *)&opts[0] } };
    cfg.netdevs = nds;
    cfg.n_netdevs = 1;
    CHECK(uam_board_config_netdev(&cfg, "wan") == (uam_netdev *)&opts[0], "named network port");
    CHECK(uam_board_config_netdev(&cfg, "lan1") == NULL, "unattached port is unplugged");
}

int main(void)
{
    char dir[] = "/tmp/uam-test-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_chardev_flow();
    test_uart_chardev();
    test_console_hold();
    test_netdev(dir);
    test_tun_adapter();
    test_virtio_net();
    test_host_loop(dir);
    test_host_stall(dir);
    test_registry();
    test_console_snapshot();   /* uses the stock boards test_registry registers */

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
    printf("test_host: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
