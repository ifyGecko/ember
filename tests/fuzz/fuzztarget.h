/*
 * fuzztarget.h - The ISA-neutral body of the fuzz target firmware.
 *
 * Included by tests/firmware/fuzztarget.c (RV32) and tests/firmware-mips/fuzztarget.c
 * (MIPS32), which define UART_THR, UART_LSR and BAD_ADDR for their board first. The
 * start code (start.S in each directory) calls main and, on any exception,
 * trap_handler.
 */
#define BUF_SIZE 256

static uint8_t g_buf[BUF_SIZE];
volatile uint32_t g_sink;

static void putc_(char c)
{
    while (!(UART_LSR & 0x20))
        ;
    UART_THR = (uint8_t)c;
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
    return sum;
}

/* One iteration: everything the UART holds is one input. */
void fuzz_iteration(void) __attribute__((noinline));
void fuzz_iteration(void)
{
    unsigned n = 0;
    while ((UART_LSR & 0x01) && n < BUF_SIZE)
        g_buf[n++] = UART_THR;
    g_sink = parse(g_buf, n);
}

int main(void);
int main(void)
{
    puts_("fuzztarget: ready\n");
    for (;;)
        fuzz_iteration();
}
