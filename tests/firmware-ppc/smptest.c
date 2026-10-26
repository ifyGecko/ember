/*
 * smptest.c - The multiprocessor self-test (tests/smp/smp_test.h) on ppc-ref with
 * -o cpus=4: each processor's decrementer (vector 0x900), the IPI doorbells on the
 * external interrupt (vector 0x500), the console UART and the test finisher.
 */
#include <stdint.h>

#define UART0    0xf0000000u
#define FINISHER 0xf0000100u
#define IPI(n)   (0xf0001000u + 4u * (n))

static inline void w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static unsigned cpunum(void)
{
    unsigned v;
    __asm__ volatile("mfspr %0, 1023" : "=r"(v));
    return v;
}

static uint32_t msr(void)
{
    uint32_t v;
    __asm__ volatile("mfmsr %0" : "=r"(v));
    return v;
}

static void arch_init_cpu(unsigned id)
{
    (void)id;
    __asm__ volatile("mtmsr %0; isync" :: "r"(msr() | 0x8000u) : "memory");   /* EE */
}

/* MSR[POW]: sleep until an interrupt, which returns past the mtmsr. */
static void arch_wait(void) { __asm__ volatile("mtmsr %0; isync" :: "r"(msr() | 0x40000u) : "memory"); }

static void arch_arm_timer(unsigned id, unsigned ticks)
{
    (void)id;
    __asm__ volatile("mtdec %0" :: "r"(ticks));
}

static void arch_send_ipi(unsigned target) { w32(IPI(target), 1); }

static void arch_putc(char c) { *(volatile uint8_t *)UART0 = (uint8_t)c; }

static void arch_finish(int pass, unsigned code)
{
    w32(FINISHER, pass ? 0x5555u : ((code << 16) | 0x3333u));
    for (;;)
        arch_wait();
}

#include "../smp/smp_test.h"

void smp_trap(unsigned vec);
void smp_trap(unsigned vec)
{
    unsigned id = cpunum();
    if (vec == 0x900) {             /* decrementer: push it far out */
        __asm__ volatile("mtdec %0" :: "r"(0x7FFFFFFFu));
        smp_on_timer(id);
    } else {                        /* IPI doorbell */
        w32(IPI(id), 0);
        smp_on_ipi(id);
    }
}
