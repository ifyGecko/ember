/*
 * smptest.c - The multiprocessor self-test (tests/smp/smp_test.h) on arm-ref
 * with -o cpus=4: each processor's physical generic timer (PPI 30), SGI 1 for the
 * IPIs, the banked GIC CPU interface, the console UART and the test finisher.
 */
#include <stdint.h>

#if defined(__ARMEB__) || defined(__ARM_BIG_ENDIAN)
#define IS_BE 1
#else
#define IS_BE 0
#endif

#define REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
#define UART0     0x1c000000u
#define FINISHER  0x1c001000u
#define GICD      0x1c010000u
#define GICC      0x1c020000u
#define PPI_TIMER 30u
#define SGI_IPI   1u

/* The GIC's registers are little-endian: a big-endian CPU swaps, as readl/writel do. */
static uint32_t le32(uint32_t v) { return IS_BE ? __builtin_bswap32(v) : v; }
#define GIC_W(a, v) (REG32(a) = le32(v))
#define GIC_R(a)    le32(REG32(a))

static unsigned aff0(void)
{
    unsigned v;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v));
    return v & 0xFFu;
}

static void arch_init_cpu(unsigned id)
{
    (void)id;
    GIC_W(GICD + 0x000, 1);                                 /* distributor on */
    GIC_W(GICD + 0x100, (1u << PPI_TIMER) | (1u << SGI_IPI)); /* this CPU's bank */
    GIC_W(GICC + 0x004, 0xF0);                              /* priority mask */
    GIC_W(GICC + 0x000, 1);                                 /* CPU interface on */
    __asm__ volatile("cpsie i");
}

static void arch_wait(void) { __asm__ volatile("wfi"); }

static void arch_arm_timer(unsigned id, unsigned ticks)
{
    (void)id;
    __asm__ volatile("mcr p15, 0, %0, c14, c2, 0" :: "r"(ticks));   /* CNTP_TVAL */
    __asm__ volatile("mcr p15, 0, %0, c14, c2, 1" :: "r"(1u));      /* CNTP_CTL: on */
    __asm__ volatile("isb");
}

static void arch_send_ipi(unsigned target) { GIC_W(GICD + 0xF00, (1u << (16 + target)) | SGI_IPI); }

static void arch_putc(char c)
{
    while (!(*(volatile uint8_t *)(UART0 + 5) & 0x20))
        ;
    *(volatile uint8_t *)UART0 = (uint8_t)c;
}

static void arch_finish(int pass, unsigned code)
{
    REG32(FINISHER) = pass ? 0x5555u : ((code << 16) | 0x3333u);
    for (;;)
        arch_wait();
}

#include "../smp/smp_test.h"

void smp_irq(void);
void smp_irq(void)
{
    uint32_t iar = GIC_R(GICC + 0x0C), irq = iar & 0x3FF;
    unsigned id = aff0();
    if (irq == PPI_TIMER) {
        __asm__ volatile("mcr p15, 0, %0, c14, c2, 1" :: "r"(0u));  /* timer off */
        __asm__ volatile("isb");
        smp_on_timer(id);
    } else if (irq == SGI_IPI) {
        if (((iar >> 10) & 7) != 0)
            arch_finish(0, 0x20);                               /* the sender is CPU 0 */
        smp_on_ipi(id);
    } else if (irq != 1023) {
        arch_finish(0, 0x100 | irq);
    }
    if (irq != 1023)
        GIC_W(GICC + 0x10, iar);                                /* EOI */
}
