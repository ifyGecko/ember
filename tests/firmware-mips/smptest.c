/*
 * smptest.c - The multiprocessor self-test (tests/smp/smp_test.h) on mips-ref
 * with -o cpus=4: each processor's CP0 Count/Compare timer (Cause.IP7), the IPI
 * doorbells (Cause.IP3), the console UART and the test finisher.
 */
#include <stdint.h>

#define UART0    0xBFD003F8u
#define FINISHER 0xBFD00010u
#define IPI(n)   (0xBFD01000u + 4u * (n))

static inline void w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static unsigned cpunum(void)
{
    unsigned v;
    __asm__ volatile("mfc0 %0, $15, 1" : "=r"(v));
    return v & 0x3FFu;
}

static unsigned count(void)
{
    unsigned v;
    __asm__ volatile("mfc0 %0, $9" : "=r"(v));
    return v;
}

static void set_compare(unsigned v) { __asm__ volatile("mtc0 %0, $11; ehb" :: "r"(v)); }

static void arch_init_cpu(unsigned id)
{
    (void)id;
    /* IE, IM3 (the IPI), IM7 (the timer) */
    __asm__ volatile("mtc0 %0, $12; ehb" :: "r"((1u << 0) | (1u << 11) | (1u << 15)));
}

static void arch_wait(void) { __asm__ volatile("wait"); }

static void arch_arm_timer(unsigned id, unsigned ticks)
{
    (void)id;
    set_compare(count() + ticks);
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

void smp_trap(void);
void smp_trap(void)
{
    unsigned cause;
    __asm__ volatile("mfc0 %0, $13" : "=r"(cause));
    unsigned id = cpunum();
    if ((cause >> 2) & 0x1F)
        arch_finish(0, 0x100 | ((cause >> 2) & 0x1F));   /* not an interrupt */
    if (cause & (1u << 15)) {               /* timer: writing Compare clears it */
        set_compare(count() - 1);
        smp_on_timer(id);
    }
    if (cause & (1u << 11)) {               /* IPI doorbell */
        w32(IPI(id), 0);
        smp_on_ipi(id);
    }
}
