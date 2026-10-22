/*
 * smptest.c - The multiprocessor self-test (tests/smp/smp_test.h) on rv32-ref
 * with -o cpus=4: the CLINT's per-hart mtimecmp for the timers, its per-hart
 * msip for the IPIs, the console UART and the test finisher.
 */
#include <stdint.h>

#define CLINT     0x02000000u
#define MSIP(h)   (CLINT + 4u * (h))
#define MTIMECMP(h) (CLINT + 0x4000u + 8u * (h))
#define MTIME     (CLINT + 0xBFF8u)
#define UART0     0x10000000u
#define FINISHER  0x00100000u

static inline void w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t r32(uint32_t a) { return *(volatile uint32_t *)a; }

static unsigned hartid(void)
{
    unsigned id;
    __asm__ volatile("csrr %0, mhartid" : "=r"(id));
    return id;
}

static void arch_init_cpu(unsigned id)
{
    (void)id;
    __asm__ volatile("csrw mie, %0" :: "r"((1u << 3) | (1u << 7)));   /* MSIE | MTIE */
    __asm__ volatile("csrsi mstatus, 8");                               /* MIE */
}

static void arch_wait(void) { __asm__ volatile("wfi"); }

static void arch_arm_timer(unsigned id, unsigned ticks)
{
    uint32_t when = r32(MTIME) + ticks;
    w32(MTIMECMP(id) + 4, 0xFFFFFFFFu);   /* no match while the halves change */
    w32(MTIMECMP(id), when);
    w32(MTIMECMP(id) + 4, 0);
}

static void arch_send_ipi(unsigned target) { w32(MSIP(target), 1); }

static void arch_putc(char c) { w32(UART0, (uint8_t)c); }

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
    uint32_t cause;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    unsigned id = hartid();
    if (cause == 0x80000003u) {            /* machine software interrupt */
        w32(MSIP(id), 0);
        smp_on_ipi(id);
    } else if (cause == 0x80000007u) {     /* machine timer interrupt */
        w32(MTIMECMP(id) + 4, 0xFFFFFFFFu);
        smp_on_timer(id);
    } else {
        arch_finish(0, 0x100 | cause);     /* unexpected trap */
    }
}
