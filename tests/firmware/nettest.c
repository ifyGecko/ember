/*
 * nettest.c - Bare-metal network test for the UAM `rv32-ref` board: a minimal
 * virtio-net (virtio-mmio v2) driver in slot 0 plus just enough IPv4 to
 *   1. resolve the host (ARP request for HOST_IP, wait for the reply),
 *   2. ping the host (ICMP echo request, wait for the reply),
 *   3. answer ARP and ICMP echo requests until the host has pinged us
 *      PINGS_TO_SERVE times,
 * then report through the sifive,test finisher. The driver sleeps in WFI
 * and is woken by the virtio interrupt (PLIC source 1), so the host loop's
 * idle wait on the network backend is exercised too.
 *
 * Run by tests/net/net-check.sh against a real tap or tun interface.
 */
#include <stddef.h>
#include <stdint.h>

#define UART0     0x10000000u
#define PLIC      0x0C000000u
#define VIRTIO0   0x10001000u
#define VIRTIO_IRQ 1u

#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define VREG(o)  REG32(VIRTIO0 + (o))

#define PLIC_PRIORITY(s)  REG32(PLIC + 4u * (s))
#define PLIC_ENABLE0      REG32(PLIC + 0x2000)
#define PLIC_THRESHOLD0   REG32(PLIC + 0x200000)
#define PLIC_CLAIM0       REG32(PLIC + 0x200004)

#define csr_set(csr, v)   __asm__ volatile("csrs " #csr ", %0" :: "r"((uint32_t)(v)))
#define csr_clear(csr, v) __asm__ volatile("csrc " #csr ", %0" :: "r"((uint32_t)(v)))
#define MIE_MEIE    (1u << 11)
#define MSTATUS_MIE (1u << 3)

static const uint8_t MY_IP[4]   = { 10, 0, 2, 15 };
static const uint8_t HOST_IP[4] = { 10, 0, 2, 2 };
#define PINGS_TO_SERVE 3

/* ---- freestanding support ------------------------------------------------ */

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

static void putc_(char c)
{
    while (!(REG8(UART0 + 5) & 0x20))
        ;
    REG8(UART0) = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putc_('\r');
        putc_(*s++);
    }
}

static void putdec(unsigned v)
{
    char b[12];
    int i = 0;
    do
        b[i++] = (char)('0' + v % 10);
    while (v /= 10);
    while (i)
        putc_(b[--i]);
}

static void putmac(const uint8_t *m)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i)
            putc_(':');
        putc_(hex[m[i] >> 4]);
        putc_(hex[m[i] & 15]);
    }
}

static void putip(const uint8_t *ip)
{
    for (int i = 0; i < 4; i++) {
        if (i)
            putc_('.');
        putdec(ip[i]);
    }
}

/* ---- virtio-net ---------------------------------------------------------- */

#define QN       8u
#define HDR      12u
#define BUFSZ    (HDR + 1514u)

typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } vdesc;
typedef struct { uint16_t flags, idx, ring[QN], event; } vavail;
typedef struct { uint16_t flags, idx; struct { uint32_t id, len; } ring[QN]; uint16_t event; } vused;

typedef struct {
    vdesc  desc[QN] __attribute__((aligned(16)));
    vavail avail    __attribute__((aligned(4)));
    vused  used     __attribute__((aligned(4)));
    uint16_t last_used;
} vqueue;

static vqueue rxq, txq;
static uint8_t rxbuf[QN][BUFSZ] __attribute__((aligned(4)));
static uint8_t txbuf[BUFSZ] __attribute__((aligned(4)));
static uint8_t my_mac[6], host_mac[6];
static volatile int irq_count;

#define fence() __asm__ volatile("fence" ::: "memory")

static void setup_queue(unsigned i, vqueue *q)
{
    VREG(0x030) = i;
    VREG(0x038) = QN;
    VREG(0x080) = (uint32_t)(uintptr_t)q->desc;
    VREG(0x084) = 0;
    VREG(0x090) = (uint32_t)(uintptr_t)&q->avail;
    VREG(0x094) = 0;
    VREG(0x0a0) = (uint32_t)(uintptr_t)&q->used;
    VREG(0x0a4) = 0;
    VREG(0x044) = 1;
}

static int net_init(void)
{
    if (VREG(0x000) != 0x74726976u || VREG(0x004) != 2 || VREG(0x008) != 1) {
        puts_("nettest: no virtio-net device in slot 0 (run with --nic net0=...)\n");
        return -1;
    }
    VREG(0x070) = 0;                 /* reset */
    VREG(0x070) = 1 | 2;             /* ACKNOWLEDGE | DRIVER */
    VREG(0x014) = 1;
    if (!(VREG(0x010) & 1))
        return -1;                   /* VERSION_1 required */
    VREG(0x024) = 0;
    VREG(0x020) = 1u << 5;           /* MAC */
    VREG(0x024) = 1;
    VREG(0x020) = 1;                 /* VERSION_1 */
    VREG(0x070) = 1 | 2 | 8;         /* FEATURES_OK */
    if (!(VREG(0x070) & 8))
        return -1;
    for (int i = 0; i < 6; i++)
        my_mac[i] = REG8(VIRTIO0 + 0x100 + i);

    for (unsigned i = 0; i < QN; i++) {
        rxq.desc[i] = (vdesc){ (uint32_t)(uintptr_t)rxbuf[i], BUFSZ, 2 /* WRITE */, 0 };
        rxq.avail.ring[i] = (uint16_t)i;
    }
    rxq.avail.idx = QN;
    setup_queue(0, &rxq);
    setup_queue(1, &txq);
    fence();
    VREG(0x070) = 1 | 2 | 8 | 4;     /* DRIVER_OK */
    VREG(0x050) = 0;

    PLIC_PRIORITY(VIRTIO_IRQ) = 1;
    PLIC_ENABLE0 = 1u << VIRTIO_IRQ;
    PLIC_THRESHOLD0 = 0;
    csr_set(mie, MIE_MEIE);
    return 0;
}

static void net_send(const uint8_t *frame, uint32_t len)
{
    memset(txbuf, 0, HDR);
    memcpy(txbuf + HDR, frame, len);
    uint16_t slot = txq.avail.idx;
    txq.desc[0] = (vdesc){ (uint32_t)(uintptr_t)txbuf, HDR + len, 0, 0 };
    txq.avail.ring[slot % QN] = 0;
    fence();
    txq.avail.idx = (uint16_t)(slot + 1);
    fence();
    VREG(0x050) = 1;
    while (*(volatile uint16_t *)&txq.used.idx != (uint16_t)(slot + 1))
        ;                            /* the device transmits synchronously */
}

/* Interrupt: acknowledge the device and the PLIC; the main loop does the work. */
uint32_t trap_handler(uint32_t *frame);
uint32_t trap_handler(uint32_t *frame)
{
    (void)frame;
    uint32_t mepc;
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    uint32_t src = PLIC_CLAIM0;
    if (src == VIRTIO_IRQ) {
        VREG(0x064) = VREG(0x060);
        irq_count++;
    }
    if (src)
        PLIC_CLAIM0 = src;
    return mepc;
}

/* ---- IPv4 ------------------------------------------------------------------ */

static int got_host_mac, got_pong, pings_served;

static uint16_t csum(const uint8_t *p, unsigned n)
{
    uint32_t s = 0;
    for (unsigned i = 0; i + 1 < n; i += 2)
        s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void send_arp(uint16_t op, const uint8_t *tha, const uint8_t *tpa)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t f[60];
    memset(f, 0, sizeof(f));
    memcpy(f, op == 1 ? bcast : tha, 6);
    memcpy(f + 6, my_mac, 6);
    put16(f + 12, 0x0806);
    uint8_t *a = f + 14;
    put16(a, 1);
    put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, my_mac, 6);
    memcpy(a + 14, MY_IP, 4);
    if (op == 2)
        memcpy(a + 18, tha, 6);
    memcpy(a + 24, tpa, 4);
    net_send(f, sizeof(f));
}

/* ICMP echo (type 8 request / 0 reply) of `len` payload bytes. */
static void send_icmp(const uint8_t *dmac, const uint8_t *dip, uint8_t type,
                      const uint8_t *icmp_rest, unsigned rest_len)
{
    static uint16_t ip_id;
    uint8_t f[1514];
    memcpy(f, dmac, 6);
    memcpy(f + 6, my_mac, 6);
    put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *ic = ip + 20;
    unsigned icmp_len = 4 + rest_len;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + icmp_len));
    put16(ip + 4, ip_id++);
    ip[8] = 64;
    ip[9] = 1;
    memcpy(ip + 12, MY_IP, 4);
    memcpy(ip + 16, dip, 4);
    put16(ip + 10, csum(ip, 20));
    ic[0] = type;
    ic[1] = 0;
    ic[2] = ic[3] = 0;
    memcpy(ic + 4, icmp_rest, rest_len);
    put16(ic + 2, csum(ic, icmp_len));
    unsigned len = 14 + 20 + icmp_len;
    if (len < 60) {
        memset(f + len, 0, 60 - len);
        len = 60;
    }
    net_send(f, len);
}

static void handle(const uint8_t *f, unsigned len)
{
    if (len < 14)
        return;
    const uint8_t *p = f + 14;
    if (get16(f + 12) == 0x0806 && len >= 14 + 28) {
        if (get16(p + 6) == 1 && !memcmp(p + 24, MY_IP, 4)) {
            send_arp(2, p + 8, p + 14);
        } else if (get16(p + 6) == 2 && !memcmp(p + 14, HOST_IP, 4) && !got_host_mac) {
            memcpy(host_mac, p + 8, 6);
            got_host_mac = 1;
        }
        return;
    }
    if (get16(f + 12) != 0x0800 || len < 14 + 20 || (p[0] >> 4) != 4 || p[9] != 1
        || memcmp(p + 16, MY_IP, 4))
        return;
    unsigned ihl = (p[0] & 15) * 4u, tot = get16(p + 2);
    if (tot > len - 14 || tot < ihl + 8)
        return;
    const uint8_t *ic = p + ihl;
    if (ic[0] == 8) {                /* echo request: answer from where it came */
        send_icmp(f + 6, p + 12, 0, ic + 4, tot - ihl - 4);
        pings_served++;
        puts_("nettest: answered ping ");
        putdec((unsigned)pings_served);
        puts_(" from ");
        putip(p + 12);
        puts_("\n");
    } else if (ic[0] == 0 && !memcmp(p + 12, HOST_IP, 4)) {
        got_pong = 1;
    }
}

/* Handle every received frame, then sleep until the device interrupts. */
static void poll_or_sleep(void)
{
    csr_clear(mstatus, MSTATUS_MIE);
    if (*(volatile uint16_t *)&rxq.used.idx == rxq.last_used)
        __asm__ volatile("wfi");     /* wakes on a pending interrupt even with MIE clear */
    csr_set(mstatus, MSTATUS_MIE);
    fence();
    while (*(volatile uint16_t *)&rxq.used.idx != rxq.last_used) {
        unsigned slot = rxq.last_used % QN;
        uint32_t id = rxq.used.ring[slot].id, len = rxq.used.ring[slot].len;
        if (id < QN && len > HDR)
            handle(rxbuf[id] + HDR, len - HDR);
        rxq.avail.ring[rxq.avail.idx % QN] = (uint16_t)id;
        fence();
        rxq.avail.idx++;
        rxq.last_used++;
        VREG(0x050) = 0;
    }
}

int main(void);
int main(void)
{
    if (net_init() != 0)
        return (1 << 16) | 0x3333;
    puts_("nettest: virtio-net mac ");
    putmac(my_mac);
    puts_(", ip ");
    putip(MY_IP);
    puts_("\n");

    /* 1. resolve the host; repeat the request now and then in case it was lost. */
    for (unsigned n = 0; !got_host_mac; n++) {
        if (n % 64 == 0)
            send_arp(1, NULL, HOST_IP);
        poll_or_sleep();
    }
    puts_("nettest: ");
    putip(HOST_IP);
    puts_(" is at ");
    putmac(host_mac);
    puts_("\n");

    /* 2. ping the host. */
    static const uint8_t echo[] = { 0x12, 0x34, 0x00, 0x01, 'u', 'a', 'm', '-', 'n', 'e', 't' };
    for (unsigned n = 0; !got_pong; n++) {
        if (n % 64 == 0)
            send_icmp(host_mac, HOST_IP, 8, echo, sizeof(echo));
        poll_or_sleep();
    }
    puts_("nettest: host answered our ping\n");

    /* 3. serve the host's pings. */
    while (pings_served < PINGS_TO_SERVE)
        poll_or_sleep();
    puts_("nettest: PASS (");
    putdec((unsigned)irq_count);
    puts_(" interrupts)\n");
    return 0x5555;
}
