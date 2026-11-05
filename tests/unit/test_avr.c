/*
 * test_avr.c - The 8-bit AVR module's parts the self-test firmware cannot reach on
 * avr-ref: interrupt entry on each core family (the vector taken, the return address on
 * the stack, SREG.I, the vector size, the hardware acknowledge), the AVRxm PMIC (levels
 * and their enables, preemption, NMI, round robin, IVSEL, RETI ending the highest level)
 * and the AVRxt CPUINT (LVL1VEC, the compact vector table), the wake rule, the CPU
 * models (uam_avr_configure), the general-purpose timer (uam/dev/gptimer.h), and guards
 * at data-space addresses.
 *
 * Each test runs on a bare machine: program memory as RAM at 0, the data space at
 * UAM_AVR_DATA_BASE.
 */
#include <stdio.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/dev/gptimer.h"
#include "uam/guard.h"
#include "uam/isa/avr.h"
#include "uam/machine.h"

static int n_checks, n_failed;

#define CHECK(cond, ...) do {                                              \
        n_checks++;                                                        \
        if (!(cond)) {                                                     \
            n_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

#define EQ64(got, want, what) do {                                         \
        uint64_t g_ = (got), w_ = (want);                                  \
        CHECK(g_ == w_, "%s: got 0x%llx want 0x%llx", what,                \
              (unsigned long long)g_, (unsigned long long)w_);             \
    } while (0)

/* The module's register file and system register ids (isa/avr8/avr.h). */
#define REG_I     39u          /* SREG.I's byte */
#define REG_SP    40u
#define SYS_SET_I 0x10000u
#define PMIC_STATUS 0xA0u
#define PMIC_INTPRI 0xA1u
#define PMIC_CTRL   0xA2u
#define CPUINT_CTRLA   0x110u
#define CPUINT_STATUS  0x111u
#define CPUINT_LVL1VEC 0x113u

#define RETI    0x9518u
#define RJMP_SELF 0xCFFFu
#define NOP     0x0000u

static void put16(uam_machine *m, uint64_t pa, uint16_t w)
{
    uint8_t b[2] = { (uint8_t)w, (uint8_t)(w >> 8) };
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 2) == UAM_OK, "load at 0x%llx", (unsigned long long)pa);
}

static uint8_t data(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, UAM_AVR_DATA_BASE + a, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}

static void sysw(uam_machine *m, uint32_t id, uint32_t v) { m->cpu.isa->sysreg_write(&m->cpu, id, v); }
static uint32_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    m->cpu.isa->sysreg_read(&m->cpu, id, &v);
    return (uint32_t)v;
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }

static unsigned acked[8], n_acked;
static void ack(void *ctx, unsigned vector)
{
    (void)ctx;
    if (n_acked < 8)
        acked[n_acked++] = vector;
}

/* A machine of model cfg: every vector slot of program memory holds RETI, and the code
 * at 0x400 spins. */
static void machine(uam_machine *m, const uam_avr_config *cfg)
{
    CHECK(uam_machine_init(m, &uam_isa_avr) == UAM_OK, "machine init");
    CHECK(uam_avr_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "flash", 0, 0x40000) == UAM_OK, "map program memory");
    CHECK(uam_bus_map_ram(&m->bus, "data", UAM_AVR_DATA_BASE, 0x10000) == UAM_OK, "map the data space");
    for (unsigned a = 0; a < 0x400; a += 2)
        put16(m, a, cfg->vector_size == 4 && (a & 2) ? NOP : RETI);
    for (unsigned a = 0x20000; a < 0x20400; a += 2)
        put16(m, a, (a & 2) ? NOP : RETI);
    put16(m, 0x400, RJMP_SELF);
    n_acked = 0;
    uam_avr_set_irq_ack(&m->cpu, ack, NULL);
    uam_machine_reset(m, 0x400);
}

/* Run n instructions from where the cpu is. */
static void step(uam_machine *m, uint64_t n) { uam_exec_run(&m->exec, n); }

static void test_classic(void)
{
    /* AVRe+: 22-bit PC, three-byte return addresses, 4-byte vectors */
    uam_machine m;
    machine(&m, &uam_avr_eplus_ref);
    m.cpu.pc = 0x1234;
    line(&m, 5, 1);
    line(&m, 3, 1);
    CHECK(!take(&m), "eplus: nothing while I is clear");
    CHECK(m.cpu.isa->irq_wakeup(&m.cpu), "eplus: but a pending vector wakes SLEEP");
    sysw(&m, SYS_SET_I, 1);
    CHECK(take(&m), "eplus: taken with I set");
    EQ64(m.cpu.pc, 3 * 4, "eplus: the lowest vector, 4 bytes each");
    EQ64(m.cpu.regfile[REG_I], 0, "eplus: I cleared");
    EQ64(uam_cpu_reg_read(&m.cpu, REG_SP, 16), 0x21FF - 3, "eplus: three bytes pushed");
    EQ64(data(&m, 0x21FF), 0x1A, "eplus: PC[7:0] at the old SP");      /* word 0x00091A */
    EQ64(data(&m, 0x21FE), 0x09, "eplus: PC[15:8]");
    EQ64(data(&m, 0x21FD), 0x00, "eplus: PC[21:16]");
    EQ64(n_acked == 1 && acked[0] == 3, 1, "eplus: the vector was acknowledged");
    CHECK(!take(&m), "eplus: I is clear in the handler");
    line(&m, 3, 0);
    step(&m, 1);                                                      /* RETI */
    EQ64(m.cpu.regfile[REG_I], 1, "eplus: RETI sets I");
    EQ64(uam_cpu_reg_read(&m.cpu, REG_SP, 16), 0x21FF, "eplus: RETI pops three bytes");
    CHECK(m.cpu.pc == 5 * 4 || m.cpu.pc == 0x1234, "eplus: back (or straight into vector 5): pc 0x%llx",
          (unsigned long long)m.cpu.pc);
    uam_machine_fini(&m);

    /* AVRe: 16-bit PC, two bytes */
    machine(&m, &uam_avr_e_ref);
    m.cpu.pc = 0x0400;
    sysw(&m, SYS_SET_I, 1);
    line(&m, 2, 1);
    CHECK(take(&m), "e: taken");
    EQ64(m.cpu.pc, 2 * 4, "e: vector 2");
    EQ64(uam_cpu_reg_read(&m.cpu, REG_SP, 16), 0x08FF - 2, "e: two bytes pushed");
    EQ64(data(&m, 0x08FF) | data(&m, 0x08FE) << 8, 0x0200, "e: the word address, high byte below");
    uam_machine_fini(&m);

    /* AVRrc: 2-byte vectors */
    machine(&m, &uam_avr_rc_ref);
    sysw(&m, SYS_SET_I, 1);
    line(&m, 6, 1);
    CHECK(take(&m), "rc: taken");
    EQ64(m.cpu.pc, 6 * 2, "rc: vector 6, 2 bytes each");
    uam_machine_fini(&m);
}

static void test_pmic(void)
{
    uam_machine m;
    machine(&m, &uam_avr_xm_ref);
    uam_avr_set_irq_level(&m.cpu, 2, 1);                 /* low */
    uam_avr_set_irq_level(&m.cpu, 3, 1);                 /* low */
    uam_avr_set_irq_level(&m.cpu, 5, 2);                 /* medium */
    uam_avr_set_irq_level(&m.cpu, 7, 3);                 /* high */
    uam_avr_set_irq_level(&m.cpu, 9, UAM_AVR_LEVEL_NMI);
    sysw(&m, SYS_SET_I, 1);
    line(&m, 2, 1);
    CHECK(!take(&m), "xm: no level enabled");
    sysw(&m, PMIC_CTRL, 0x07);                           /* HI, MED, LO enabled */
    CHECK(take(&m), "xm: low level taken");
    EQ64(m.cpu.pc, 2 * 4, "xm: vector 2");
    EQ64(sysr(&m, PMIC_STATUS), 0x01, "xm: LOLVLEX");
    EQ64(m.cpu.regfile[REG_I], 1, "xm: I is left alone");
    line(&m, 3, 1);
    CHECK(!take(&m), "xm: low does not preempt low");
    line(&m, 5, 1);
    CHECK(take(&m), "xm: medium preempts low");
    EQ64(m.cpu.pc, 5 * 4, "xm: vector 5");
    line(&m, 7, 1);
    CHECK(take(&m), "xm: high preempts medium");
    EQ64(sysr(&m, PMIC_STATUS), 0x07, "xm: three levels running");
    sysw(&m, SYS_SET_I, 0);
    line(&m, 9, 1);
    CHECK(take(&m), "xm: NMI regardless of I and levels");
    EQ64(m.cpu.pc, 9 * 4, "xm: vector 9");
    EQ64(sysr(&m, PMIC_STATUS), 0x87, "xm: NMIEX");
    line(&m, 9, 0); line(&m, 7, 0); line(&m, 5, 0); line(&m, 3, 0); line(&m, 2, 0);
    step(&m, 1);                                          /* RETI */
    EQ64(sysr(&m, PMIC_STATUS), 0x07, "xm: RETI ends NMI first");
    step(&m, 1);
    EQ64(sysr(&m, PMIC_STATUS), 0x03, "xm: then high");
    uam_machine_fini(&m);

    /* Round robin and IVSEL */
    machine(&m, &uam_avr_xm_ref);
    uam_avr_set_irq_level(&m.cpu, 2, 1);
    uam_avr_set_irq_level(&m.cpu, 3, 1);
    sysw(&m, SYS_SET_I, 1);
    sysw(&m, PMIC_CTRL, 0x80 | 0x40 | 0x01);             /* RREN, IVSEL, LOLVLEN */
    line(&m, 2, 1);
    line(&m, 3, 1);
    CHECK(take(&m), "rr: taken");
    EQ64(m.cpu.pc, 0x20000 + 2 * 4, "rr: vector 2 at the boot section (IVSEL)");
    EQ64(sysr(&m, PMIC_INTPRI), 2, "rr: INTPRI is the vector taken");
    step(&m, 1);                                          /* RETI (takes the next one) */
    if (sysr(&m, PMIC_STATUS) == 0)
        CHECK(take(&m), "rr: taken again");
    EQ64(m.cpu.pc, 0x20000 + 3 * 4, "rr: now vector 3 first");
    EQ64(sysr(&m, PMIC_INTPRI), 3, "rr: INTPRI follows");
    uam_machine_fini(&m);
}

static void test_cpuint(void)
{
    uam_machine m;
    machine(&m, &uam_avr_xt_ref);
    sysw(&m, SYS_SET_I, 1);
    line(&m, 4, 1);
    CHECK(take(&m), "xt: level 0 taken");
    EQ64(m.cpu.pc, 4 * 4, "xt: vector 4");
    EQ64(sysr(&m, CPUINT_STATUS), 0x01, "xt: LVL0EX");
    sysw(&m, CPUINT_LVL1VEC, 6);
    line(&m, 6, 1);
    CHECK(take(&m), "xt: LVL1VEC preempts level 0");
    EQ64(sysr(&m, CPUINT_STATUS), 0x03, "xt: LVL1EX");
    uam_machine_fini(&m);

    machine(&m, &uam_avr_xt_ref);
    sysw(&m, SYS_SET_I, 1);
    sysw(&m, CPUINT_CTRLA, 0x20);                        /* CVT */
    sysw(&m, CPUINT_LVL1VEC, 8);
    line(&m, 11, 1);
    CHECK(take(&m), "cvt: taken");
    EQ64(m.cpu.pc, 3 * 4, "cvt: every level 0 vector is slot 3");
    line(&m, 8, 1);
    CHECK(take(&m), "cvt: level 1");
    EQ64(m.cpu.pc, 2 * 4, "cvt: level 1 is slot 2");
    uam_avr_set_irq_level(&m.cpu, 13, UAM_AVR_LEVEL_NMI);
    line(&m, 13, 1);
    CHECK(take(&m), "cvt: NMI");
    EQ64(m.cpu.pc, 1 * 4, "cvt: NMI is slot 1");
    uam_machine_fini(&m);
}

static void test_configure(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_avr) == UAM_OK, "init");
    EQ64(uam_avr_config_of(&m.cpu)->core, UAM_AVR_EPLUS, "the default model is AVRe+");
    uam_avr_config bad = uam_avr_e_ref;
    bad.vector_size = 3;
    EQ64(uam_avr_configure(&m.cpu, &bad), UAM_ERR_INVALID, "a vector size of 3 is refused");
    EQ64(uam_avr_set_irq_level(&m.cpu, 0, 1), UAM_ERR_INVALID, "vector 0 is reset, not an interrupt");
    uam_machine_reset(&m, 0);
    EQ64(uam_cpu_reg_read(&m.cpu, REG_SP, 16), uam_avr_eplus_ref.sp_reset, "SP after reset");
    uam_machine_fini(&m);
}

static unsigned timer_level, timer_edges;
static void timer_sink(void *ctx, unsigned input, int level)
{
    (void)ctx; (void)input;
    timer_edges += level && !timer_level;
    timer_level = (unsigned)level;
}

static void test_gptimer(void)
{
    uam_machine m;
    machine(&m, &uam_avr_eplus_ref);
    uam_device *t = uam_gptimer_create("timer", &m.sched);
    CHECK(t != NULL, "create");
    uam_machine_add_device(&m, t);
    timer_level = timer_edges = 0;
    uam_signal_connect(uam_gptimer_irq(t), timer_sink, NULL, 0);
    t->ops->write(t, 0x8, 4, 100);                       /* LOAD */
    t->ops->write(t, 0x0, 4, 0x7);                       /* EN, PERIODIC, IE */
    uint64_t c = 0;
    t->ops->read(t, 0xC, 4, &c);
    EQ64(c, 100, "COUNT starts at LOAD");
    step(&m, 250);
    EQ64(timer_level, 1, "the line is up");
    t->ops->read(t, 0xC, 4, &c);
    CHECK(c > 0 && c <= 100, "COUNT counts down (%llu)", (unsigned long long)c);
    t->ops->write(t, 0x4, 4, 1);                         /* clear IF */
    EQ64(timer_level, 0, "writing 1 to IF clears it");
    step(&m, 100);
    EQ64(timer_level, 1, "periodic: it fires again");
    uam_gptimer_ack(t);
    EQ64(timer_level, 0, "the acknowledge clears IF");
    /* a byte write leaves the other bytes of a register alone */
    t->ops->write_masked(t, 0x8, 4, 0x0200, 0xFF00);
    uint64_t load = 0;
    t->ops->read(t, 0x8, 4, &load);
    EQ64(load, 0x0264, "a byte of LOAD");
    t->ops->write(t, 0x0, 4, 0x4);                       /* one-shot, stopped */
    t->ops->write(t, 0x0, 4, 0x5);                       /* one-shot, started */
    step(&m, 700);
    uint64_t ctrl = 0;
    t->ops->read(t, 0x0, 4, &ctrl);
    EQ64(ctrl, 0x4, "a one-shot timer clears EN when it expires");
    uam_machine_fini(&m);
}

/* A guard's address is a data-space address, as the firmware stores to it (and as the
 * fuzz harness writes a mem: route's input): it must not land in program memory. */
static void test_guard(void)
{
    uam_machine m;
    machine(&m, &uam_avr_e_ref);
    put16(&m, 0x400, 0xE50A);              /* LDI r16, 0x5A */
    put16(&m, 0x402, 0x9300);              /* STS 0x0201, r16 */
    put16(&m, 0x404, 0x0201);
    put16(&m, 0x406, RJMP_SELF);
    char err[256] = "";
    CHECK(uam_machine_add_guard_text(&m, "0x200+4:w,log", UAM_GUARD_STOP, err, sizeof(err)) > 0,
          "%s", err);
    FILE *quiet = fopen("/dev/null", "w");
    uam_machine_set_guard_log(&m, quiet);
    step(&m, 3);
    EQ64(uam_machine_guard_hits(&m), 1, "guard: the data store hit it");
    const uam_guard_hit *h = uam_machine_guard_last_hit(&m);
    CHECK(h != NULL, "guard: a hit recorded");
    if (h) {
        EQ64(h->addr, 0x201, "guard: data address");
        EQ64(h->paddr, UAM_AVR_DATA_BASE + 0x201, "guard: in the data space");
        EQ64(h->val, 0x5A, "guard: value");
    }
    EQ64(data(&m, 0x201), 0x5A, "guard: a log guard lets the store happen");
    uam_machine_fini(&m);
    if (quiet)
        fclose(quiet);
}

int main(void)
{
    test_classic();
    test_guard();
    test_pmic();
    test_cpuint();
    test_configure();
    test_gptimer();
    printf("test_avr: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
