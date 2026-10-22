/*
 * smp_test.h - The multiprocessor self-test every reference board runs with
 * -o cpus=SMP_NCPU, shared by the per-architecture firmware (include it once).
 *
 * Every processor starts at the reset vector; the architecture's start-up code
 * gives each one a stack and calls smp_main(id) with its processor number, and
 * parks the ones beyond SMP_NCPU. Then each processor
 *   - takes an interrupt from its own timer,
 *   - appends its number to a shared log SMP_K times through an atomic index
 *     (__atomic: LR/SC or AMOs, LL/SC, LDREX/STREX) and bumps a counter with a
 *     compare-and-swap loop (always a reserved pair), with a delay of varying
 *     length in between,
 *   - and (all but processor 0) waits for an inter-processor interrupt from
 *     processor 0.
 * Processor 0 checks that every processor arrived, every entry is there, and
 * every timer and IPI interrupt was taken, prints an FNV-1a hash of the log
 * (which depends on exactly how the processors were interleaved, so split and
 * restored runs must print the same one) and reports through the finisher.
 *
 * The architecture provides, before including this file:
 *   static void arch_init_cpu(unsigned id);           vectors, enable timer + IPI
 *   static void arch_wait(void);                      wait for an interrupt
 *   static void arch_arm_timer(unsigned id, unsigned ticks);
 *   static void arch_send_ipi(unsigned target);
 *   static void arch_putc(char c);
 *   static void arch_finish(int pass, unsigned code); never returns
 * and its interrupt handler calls smp_on_timer(id) / smp_on_ipi(id) after
 * silencing the source.
 */
#ifndef SMP_TEST_H
#define SMP_TEST_H

#include <stdint.h>

#ifndef SMP_NCPU
#define SMP_NCPU 4
#endif
#define SMP_K    400u

static volatile uint32_t smp_arrived, smp_next, smp_done, smp_acked, smp_cas;
static volatile uint32_t smp_timer_seen[SMP_NCPU], smp_ipi_seen[SMP_NCPU];
static volatile uint8_t  smp_log[SMP_NCPU * SMP_K];

void smp_on_timer(unsigned id);
void smp_on_ipi(unsigned id);
void smp_main(unsigned id);

void smp_on_timer(unsigned id) { if (id < SMP_NCPU) smp_timer_seen[id]++; }
void smp_on_ipi(unsigned id)   { if (id < SMP_NCPU) smp_ipi_seen[id]++; }

static void smp_delay(unsigned n)
{
    for (volatile unsigned i = 0; i < n; i++)
        ;
}

static void smp_puts(const char *s)
{
    while (*s)
        arch_putc(*s++);
}

static void smp_hex(uint32_t v)
{
    for (int i = 7; i >= 0; i--)
        arch_putc("0123456789abcdef"[(v >> (4 * i)) & 15]);
}

/* Spin (deterministically bounded) until *p == want; 0 if it never got there. */
static int smp_await(volatile uint32_t *p, uint32_t want)
{
    for (uint32_t n = 0; n < 20000000u; n++)
        if (*p == want)
            return 1;
    return 0;
}

void smp_main(unsigned id)
{
    if (id >= SMP_NCPU)
        for (;;)
            arch_wait();
    arch_init_cpu(id);
    __atomic_fetch_or(&smp_arrived, 1u << id, __ATOMIC_SEQ_CST);

    arch_arm_timer(id, 2000u + 300u * id);
    while (!smp_timer_seen[id])
        arch_wait();

    for (uint32_t i = 0; i < SMP_K; i++) {
        uint32_t at = __atomic_fetch_add(&smp_next, 1u, __ATOMIC_SEQ_CST);
        if (at < SMP_NCPU * SMP_K)
            smp_log[at] = (uint8_t)id;
        /* a compare-and-swap loop: a load-reserved / store-conditional pair */
        uint32_t old = smp_cas;
        while (!__atomic_compare_exchange_n(&smp_cas, &old, old + 1u, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST))
            ;
        smp_delay(((i * 7u + id * 13u) & 31u) + 1u);
    }
    __atomic_fetch_add(&smp_done, 1u, __ATOMIC_SEQ_CST);

    if (id != 0) {
        while (!smp_ipi_seen[id])
            arch_wait();
        __atomic_fetch_add(&smp_acked, 1u, __ATOMIC_SEQ_CST);
        for (;;)
            arch_wait();
    }

    if (!smp_await(&smp_done, SMP_NCPU))
        arch_finish(0, 1);                      /* 1: a processor never finished */
    for (unsigned t = 1; t < SMP_NCPU; t++)
        arch_send_ipi(t);
    if (!smp_await(&smp_acked, SMP_NCPU - 1))
        arch_finish(0, 2);                      /* 2: an IPI was not taken */
    if (smp_arrived != (1u << SMP_NCPU) - 1)
        arch_finish(0, 3);                      /* 3: wrong processor numbers */
    if (smp_next != SMP_NCPU * SMP_K)
        arch_finish(0, 4);                      /* 4: the atomic index lost updates */
    if (smp_cas != SMP_NCPU * SMP_K)
        arch_finish(0, 7);                      /* 7: compare-and-swap lost updates */
    uint32_t count[SMP_NCPU] = { 0 };
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < SMP_NCPU * SMP_K; i++) {
        if (smp_log[i] < SMP_NCPU)
            count[smp_log[i]]++;
        h = (h ^ smp_log[i]) * 0x01000193u;
    }
    for (unsigned t = 0; t < SMP_NCPU; t++) {
        if (count[t] != SMP_K)
            arch_finish(0, 5);                  /* 5: log entries lost */
        if (!smp_timer_seen[t])
            arch_finish(0, 6);                  /* 6: a timer interrupt was not taken */
    }
    smp_puts("smp log hash ");
    smp_hex(h);
    arch_putc('\n');
    arch_finish(1, 0);
}

#endif /* SMP_TEST_H */
