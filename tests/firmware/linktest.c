/*
 * linktest.c - Two of these talk to each other over a link (uam/link.h), on the
 * `rv32-test` board. What an instance does is in its network port's MAC address
 * (-o net0.mac=02:00:HH:LL:MODE:ROLE):
 *
 *   HH:LL  how many messages go back and forth in all
 *   MODE   bit 0: 0 = frames through the virtio-net port (net0)
 *                 1 = bytes through the UART (the console is the link)
 *          bit 1: 1 = poll for input in a busy loop instead of sleeping in WFI
 *                 (frames only; the UART is always polled)
 *   ROLE   1 = sends the first message; anything else answers
 *
 * Each message carries a number; whoever receives n answers n + 1, and both
 * stop at the count. A frame is 17 bytes long, shorter than anything Ethernet
 * allows, and the receiver checks that it arrives at exactly that length.
 *
 * With frames the console is free: the last line gives the count, the time
 * (mtime) and the interrupts taken - the same in every run when the hub keeps
 * exact time. The exit status is the finisher's: 0 passed, 2 a frame of the
 * wrong length, 3 a number out of order, 1 no network port.
 */
#include <stddef.h>
#include <stdint.h>

#define UART0     0x10000000u
#define CLINT     0x02000000u
#define PLIC      0x0C000000u
#define VIRTIO0   0x10001000u
#define VIRTIO_IRQ 1u

#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define VREG(o)  REG32(VIRTIO0 + (o))
#define MTIME_LO REG32(CLINT + 0xBFF8)

#define PLIC_PRIORITY(s)  REG32(PLIC + 4u * (s))
#define PLIC_ENABLE0      REG32(PLIC + 0x2000)
#define PLIC_THRESHOLD0   REG32(PLIC + 0x200000)
#define PLIC_CLAIM0       REG32(PLIC + 0x200004)

#define csr_set(csr, v)   __asm__ volatile("csrs " #csr ", %0" :: "r"((uint32_t)(v)))
#define csr_clear(csr, v) __asm__ volatile("csrc " #csr ", %0" :: "r"((uint32_t)(v)))
#define MIE_MEIE    (1u << 11)
#define MSTATUS_MIE (1u << 3)

#define PASS      0x5555
#define FAIL(c)   (((c) << 16) | 0x3333)

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
static uint8_t my_mac[6];
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

/* ---- frames --------------------------------------------------------------- */

#define FRAME_LEN 17u

static unsigned rounds, wrong_len, wrong_seq, done;

static void send_num(unsigned n)
{
    uint8_t f[FRAME_LEN];
    memset(f, 0xff, 6);
    memcpy(f + 6, my_mac, 6);
    f[12] = 0x88;
    f[13] = 0xb5;                    /* an experimental EtherType */
    f[14] = (uint8_t)n;
    f[15] = (uint8_t)(n >> 8);
    f[16] = 0x5a;
    net_send(f, FRAME_LEN);
}

static unsigned expect_num;

static void handle(const uint8_t *f, unsigned len)
{
    if (len != FRAME_LEN) {
        wrong_len = 1;
        done = 1;
        return;
    }
    unsigned n = f[14] | (unsigned)f[15] << 8;
    if (n != expect_num) {
        wrong_seq = 1;
        done = 1;
        return;
    }
    expect_num = n + 2;
    if (n >= rounds) {
        done = 1;
        return;
    }
    send_num(n + 1);
    if (n + 1 >= rounds)
        done = 1;
}

static void receive(int sleep)
{
    if (sleep) {
        csr_clear(mstatus, MSTATUS_MIE);
        if (*(volatile uint16_t *)&rxq.used.idx == rxq.last_used)
            __asm__ volatile("wfi");
        csr_set(mstatus, MSTATUS_MIE);
    }
    fence();
    while (*(volatile uint16_t *)&rxq.used.idx != rxq.last_used) {
        unsigned slot = rxq.last_used % QN;
        uint32_t id = rxq.used.ring[slot].id, len = rxq.used.ring[slot].len;
        if (id < QN && len >= HDR)
            handle(rxbuf[id] + HDR, len - HDR);
        rxq.avail.ring[rxq.avail.idx % QN] = (uint16_t)id;
        fence();
        rxq.avail.idx++;
        rxq.last_used++;
        VREG(0x050) = 0;
    }
}

/* ---- bytes through the UART ------------------------------------------------ */

static int uart_bytes(int first)
{
    unsigned want = first ? 1 : 0, limit = rounds > 250 ? 250 : rounds;
    if (first)
        putc_(0);
    for (;;) {
        while (!(REG8(UART0 + 5) & 0x01))
            ;
        unsigned n = REG8(UART0);
        if (n != want)
            return FAIL(3);
        want = n + 2;
        if (n >= limit)
            return PASS;
        putc_((char)(n + 1));
        if (n + 1 >= limit)
            return PASS;
    }
}

int main(void);
int main(void)
{
    if (net_init() != 0)
        return FAIL(1);
    rounds = (unsigned)my_mac[2] << 8 | my_mac[3];
    int mode = my_mac[4], first = my_mac[5] == 1;
    if (mode & 1)
        return uart_bytes(first);
    expect_num = first ? 1 : 0;
    if (first)
        send_num(0);
    while (!done)
        receive(!(mode & 2));
    if (wrong_len)
        return FAIL(2);
    if (wrong_seq)
        return FAIL(3);
    puts_("linktest: PASS rounds=");
    putdec(rounds);
    puts_(" mtime=");
    putdec(MTIME_LO);
    puts_(" irqs=");
    putdec((unsigned)irq_count);
    puts_("\n");
    return PASS;
}
