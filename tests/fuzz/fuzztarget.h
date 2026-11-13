/*
 * fuzztarget.h - The ISA-neutral body of the fuzz target firmware.
 *
 * Included by each board's fuzztarget.c (in tests/firmware-ISA), which defines UART_THR,
 * UART_LSR (a 16550's registers) or the UART_* functions below, and BAD_ADDR first. The
 * start code (start.S in each directory) calls main and, on any exception,
 * trap_handler.
 */
#define BUF_SIZE 256

/* A board whose UART is not a 16550 defines these four instead of UART_THR/UART_LSR. */
#ifndef UART_PUT
#define UART_TX_READY() (UART_LSR & 0x20)
#define UART_RX_READY() (UART_LSR & 0x01)
#define UART_PUT(c)     (UART_THR = (uint8_t)(c))
#define UART_GET()      ((uint8_t)UART_THR)
#endif

static uint8_t g_buf[BUF_SIZE];
volatile uint32_t g_sink;

static void putc_(char c)
{
    while (!UART_TX_READY())
        ;
    UART_PUT(c);
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

/* The firmware's fatal-error routine: a fuzz harness treats reaching it as a crash. */
void fuzz_panic(void) __attribute__((noinline, noreturn));
void fuzz_panic(void)
{
    for (;;)
        g_sink++;
}

uint32_t trap_handler(void *frame);
uint32_t trap_handler(void *frame)
{
    (void)frame;
    fuzz_panic();
}

#define FUZZ_MAGIC 0x6A3C91E5u

/* The magic value's planted bug: a bad store of its own, so it is a crash of its own
 * (another faulting pc than FUZ!'s). */
static __attribute__((noinline)) void magic_fault(uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(BAD_ADDR + 8) = v;
}

static __attribute__((noinline)) uint32_t parse(const uint8_t *p, unsigned n)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum = sum * 31 + p[i];
    if (n >= 4 && p[0] == 'F' && p[1] == 'U' && p[2] == 'Z') {
        if (p[3] == '!')
            *(volatile uint32_t *)(uintptr_t)BAD_ADDR = sum;   /* planted bug: bad store */
        else if (p[3] == '?')
            fuzz_panic();                                      /* planted bug: assertion */
    }
    if (n >= 1 && p[0] == 'L')
        for (;;)
            g_sink++;                                          /* planted bug: endless loop */
    if (n >= 2 && p[0] == 'o' && p[1] == 'k')
        sum ^= 0x5A5A;
    if (n >= 8 && p[0] == 'M') {
        /* planted bug behind a 32-bit magic value (little-endian at offset 4): one compare
         * that coverage alone cannot see progress on; compare logging and splitting can */
        uint32_t v = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16
                   | (uint32_t)p[7] << 24;
        if (v == FUZZ_MAGIC)
            magic_fault(v);
    }
    return sum;
}

/* One iteration: everything the UART holds is one input. */
void fuzz_iteration(void) __attribute__((noinline));
void fuzz_iteration(void)
{
    unsigned n = 0;
    while (UART_RX_READY() && n < BUF_SIZE)
        g_buf[n++] = UART_GET();
    g_sink = parse(g_buf, n);
}

int main(void);
int main(void)
{
    puts_("fuzztarget: ready\n");
    for (;;)
        fuzz_iteration();
}
