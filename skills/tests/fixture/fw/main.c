#include <stdint.h>
volatile uint32_t g_ticks;
#define W(a) (*(volatile uint32_t *)(0xa0000000u | (a)))
#define B(a) (*(volatile uint8_t *)(0xa0000000u | (a)))
static void putc_stride(char c) { while (!(B(0x18020000 + (5 << 2) + 3) & 0x20)); B(0x18020003) = c; }
static void putc_soc(char c)    { while (!(W(0x18030004) & 0x20)); B(0x18030003) = c; } /* sb: BE low lane */
static void puts_(const char *s) { for (; *s; s++) { putc_stride(*s); putc_soc(*s); } }
static void hex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) { char c = "0123456789abcdef"[(v >> i) & 15]; putc_stride(c); putc_soc(c); } }
static void fail(const char *s) { puts_("FAIL "); puts_(s); puts_("\n"); W(0x1fd00010) = (2u << 16) | 0x3333; for (;;); }

/* Fuzz target for the harness check: reads what --fuzz-input uart0 queued (soc_uart RX
 * FIFO) and reaches fuzz_bug (the test's --fuzz-crash-pc) on the input "BUG". */
__attribute__((noinline)) void fuzz_bug(void) { for (;;) __asm__ volatile(""); }
__attribute__((noinline)) void fuzz_entry(void)
{
    char b[3] = { 0 };
    for (int i = 0; i < 3 && (W(0x18030004) & 1); i++)
        b[i] = (char)W(0x18030000);
    if (b[0] == 'B' && b[1] == 'U' && b[2] == 'G')
        fuzz_bug();
}

void cmain(uint32_t a0)
{
    uint32_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    puts_("boot a0="); hex(a0); puts_(" sp="); hex(sp); puts_("\n");
    if (a0 != 0x1234) fail("entry a0");
    volatile uint32_t *boots = (uint32_t *)0x80300000;
    if (*boots != 0xB007B007u) {
        *boots = 0xB007B007u;
        puts_("chip reset\n");
        W(0x1806001c) = 1u << 24;
        for (;;);
    }
    puts_("after reset\n");
    /* NOR flash: ID, program, read back through kseg1 / kseg0 / mirror, sector erase */
    #define H(a) (*(volatile uint16_t *)(0xa0000000u | (a)))
    B(0x1f000aaa) = 0xaa; B(0x1f000555) = 0x55; B(0x1f000aaa) = 0x90;
    if (H(0x1f000000) != 0x0001 || H(0x1f000002) != 0x22a8) fail("flash id");
    B(0x1f000000) = 0xf0;
    if (B(0x1f000100) != 0xff) fail("flash blank");
    B(0x1f000aaa) = 0xaa; B(0x1f000555) = 0x55; B(0x1f000aaa) = 0xa0; B(0x1f000100) = 0x5a;
    if (B(0x1f000100) != 0x5a || B(0x1f010100) != 0x5a || *(volatile uint8_t *)0x9f000100 != 0x5a)
        fail("flash program");
    B(0x1f000aaa) = 0xaa; B(0x1f000555) = 0x55; B(0x1f000aaa) = 0x80;
    B(0x1f000aaa) = 0xaa; B(0x1f000555) = 0x55; B(0x1f000000) = 0x30;
    if (B(0x1f000100) != 0xff || B(0x1f010100) != 0xff) fail("flash erase");
    puts_("flash ok\n");
    if (W(0x18070000) == 0x600d)                  /* unmodelled: only an --mmio-rule answers */
        puts_("rule ok\n");
    uint32_t c1 = W(0x18040000);
    for (volatile int i = 0; i < 200; i++);
    if (W(0x18040000) <= c1) fail("counter");
    W(0x18060000) = 1u << 16;
    while (!(W(0x18060000) & (1u << 31)));
    if (W(0x18060000) & (1u << 16)) fail("self-clear");
    if (((W(0x18060008) >> 2) & 3) == 0) fail("divider");
    W(0x18050004) = 1u << 3;                       /* intc mask: timer input */
    W(0x18040004) = W(0x18040000) + 1000;         /* compare */
    W(0x18040008) = 2;                            /* timer irq enable */
    __asm__ volatile("mtc0 %0, $12; ehb" :: "r"((1u << 10) | 1u));   /* IM2 | IE, BEV=ERL=0 */
    while (!g_ticks) __asm__ volatile("wait");
    puts_("irq ok\n");
    fuzz_entry();
    W(0x1fd00010) = 0x5555;
    for (;;);
}
