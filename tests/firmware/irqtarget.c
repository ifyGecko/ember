/*
 * irqtarget.c - A fuzz target that takes timer interrupts while it works, for rv32-ref
 * (tests/fuzz/cov-blocks-check.sh).
 *
 * fuzz_iteration drains the UART into a buffer. The first byte sets the machine timer's
 * period; the rest is parsed by the same code whatever that byte is, while the timer
 * interrupts it every `period` ticks at whatever instruction it has reached. So two
 * inputs that differ only in their first byte take one path with interrupts landing in
 * different places: with block coverage's interrupt contexts (--cov-irq isolate) they
 * must set the same buckets. The timer is off again when the iteration ends.
 */
#include <stdint.h>

#define UART0     0x10000000u
#define CLINT     0x02000000u
#define REG8(a)   (*(volatile uint8_t *)(uintptr_t)(a))
#define REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
#define UART_THR  REG8(UART0 + 0)
#define UART_LSR  REG8(UART0 + 5)
#define MTIMECMP_LO REG32(CLINT + 0x4000)
#define MTIMECMP_HI REG32(CLINT + 0x4004)
#define MTIME_LO  REG32(CLINT + 0xBFF8)
#define MTIME_HI  REG32(CLINT + 0xBFFC)
#define MIE_MTIE  (1u << 7)
#define MSTATUS_MIE (1u << 3)
#define BUF_SIZE  256

#define csr_read(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define csr_set(csr, v) __asm__ volatile("csrs " #csr ", %0" :: "r"(v))
#define csr_clear(csr, v) __asm__ volatile("csrc " #csr ", %0" :: "r"(v))

static uint8_t g_buf[BUF_SIZE];
static uint32_t period;
volatile uint32_t g_sink, g_ticks;

static void timer_in(uint32_t delta)
{
    uint32_t lo = MTIME_LO, hi = MTIME_HI;
    uint64_t t = ((uint64_t)hi << 32 | lo) + delta;
    MTIMECMP_HI = 0xFFFFFFFFu;            /* no spurious interrupt while the halves change */
    MTIMECMP_LO = (uint32_t)t;
    MTIMECMP_HI = (uint32_t)(t >> 32);
}

static void timer_off(void)
{
    MTIMECMP_HI = 0xFFFFFFFFu;
    MTIMECMP_LO = 0xFFFFFFFFu;
}

void fuzz_panic(void) __attribute__((noinline, noreturn));
void fuzz_panic(void)
{
    for (;;)
        g_sink++;
}

/* start.S: returns the pc to resume at. The timer re-arms itself; anything else is fatal. */
uint32_t trap_handler(void *frame);
uint32_t trap_handler(void *frame)
{
    (void)frame;
    if (csr_read(mcause) != 0x80000007u)
        fuzz_panic();
    g_ticks++;
    timer_in(period);
    return csr_read(mepc);
}

/* Some branches per byte, so that the interrupts land in many different blocks. */
static __attribute__((noinline)) uint32_t work(const uint8_t *p, unsigned n)
{
    uint32_t x = 0x9E3779B9u;
    for (unsigned i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c >= 'a' && c <= 'z')
            x = x * 31 + c;
        else if (c >= '0' && c <= '9')
            x ^= (uint32_t)c << (i & 15);
        else if (c == ' ')
            x = (x << 3) | (x >> 29);
        else
            x += 7;
        for (unsigned k = 0; k < 4; k++)
            x = x * 1103515245u + 12345u;
    }
    return x;
}

void fuzz_iteration(void) __attribute__((noinline));
void fuzz_iteration(void)
{
    unsigned n = 0;
    while ((UART_LSR & 0x01) && n < BUF_SIZE)
        g_buf[n++] = UART_THR;
    if (n < 2)
        return;
    period = 37 + 11u * g_buf[0];
    timer_in(period);
    csr_set(mie, MIE_MTIE);
    csr_set(mstatus, MSTATUS_MIE);
    g_sink = work(g_buf + 1, n - 1);
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MTIE);
    timer_off();
}

int main(void);
int main(void)
{
    timer_off();
    for (;;)
        fuzz_iteration();
}
