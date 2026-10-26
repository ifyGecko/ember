/*
 * test_ppc.c - The 32-bit PowerPC module's board-facing parts that the self-test
 * firmware cannot reach on ppc-ref: the soft-reset and machine-check pins (and
 * checkstop), the wired-OR external interrupt sources, the decrementer while
 * MSR[EE] is clear, the implementation SPR handlers (uam_ppc_set_spr_ops), and the
 * CPU model (uam_ppc_configure: no FPU, eight BAT pairs, the cache block size, the
 * time base rate, the exception prefix at reset, the PVR).
 *
 * Each test runs tiny hand-encoded programs on a bare machine (RAM at 0).
 */
#include <stdio.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/isa/mips32.h"
#include "uam/isa/ppc.h"
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

/* System register ids of the module (isa/ppc32/ppc.h): SPR numbers, and the MSR. */
#define SR_MSR  0x1000u
#define SPR_DEC 22u
#define SPR_SRR0 26u
#define SPR_SRR1 27u
#define SPR_PVR 287u

#define MSR_EE 0x8000u
#define MSR_FP 0x2000u
#define MSR_ME 0x1000u
#define MSR_IP 0x0040u
#define MSR_DR 0x0010u

#define B_SELF 0x48000000u
static uint32_t MFSPR(unsigned rd, unsigned n) { return 0x7C0002A6u | rd << 21 | (n & 31) << 16 | (n >> 5) << 11; }
static uint32_t MTSPR(unsigned n, unsigned rs) { return 0x7C0003A6u | rs << 21 | (n & 31) << 16 | (n >> 5) << 11; }

static void put32(uam_machine *m, uint64_t pa, uint32_t w)
{
    uint8_t b[4] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 4) == UAM_OK, "load word at 0x%llx", (unsigned long long)pa);
}

static uint32_t gpr(uam_machine *m, unsigned r) { return (uint32_t)uam_cpu_reg_read(&m->cpu, r * 4, 32); }
static void sysw(uam_machine *m, uint32_t id, uint32_t v) { m->cpu.isa->sysreg_write(&m->cpu, id, v); }
static uint32_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    m->cpu.isa->sysreg_read(&m->cpu, id, &v);
    return (uint32_t)v;
}

/* RAM at 0, a spin at every vector this test takes and at 0x1000 (the program). With
 * `cfg`, the CPU model is set before the reset. MSR = msr afterwards. */
static void ppc_machine(uam_machine *m, const uam_ppc_config *cfg, uint32_t msr)
{
    CHECK(uam_machine_init(m, &uam_isa_ppc32) == UAM_OK, "machine init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1 << 20) == UAM_OK, "map ram");
    if (cfg)
        CHECK(uam_ppc_configure(&m->cpu, cfg) == UAM_OK, "configure");
    for (uint32_t v = 0x100; v <= 0xD00; v += 0x100)
        put32(m, v, B_SELF);
    put32(m, 0x1000, B_SELF);
    uam_machine_reset(m, 0x1000);
    sysw(m, SR_MSR, msr);
}

static uam_stop_reason run(uam_machine *m, uint64_t n) { return uam_exec_run(&m->exec, n); }

/* ---- pins ------------------------------------------------------------------ */

static void test_pins(void)
{
    uam_machine m;
    ppc_machine(&m, NULL, MSR_ME);
    run(&m, 10);
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_SRESET, 1);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x100, "soft reset vector");
    EQ64(sysr(&m, SPR_SRR0), 0x1000, "srr0");
    EQ64(sysr(&m, SR_MSR), MSR_ME, "msr after the interrupt");
    m.cpu.pc = 0x1000;
    run(&m, 10);
    EQ64(m.cpu.pc, 0x1000, "a held pin does not interrupt again");
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_SRESET, 0);
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_SRESET, 1);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x100, "a new edge does");

    m.cpu.pc = 0x1000;
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_MCP, 1);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x200, "machine check vector");
    EQ64(sysr(&m, SR_MSR) & MSR_ME, 0, "ME cleared");
    EQ64(sysr(&m, SPR_SRR1) & MSR_ME, MSR_ME, "srr1 keeps ME");
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_MCP, 0);
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_MCP, 1);   /* again, with ME = 0: checkstop */
    uam_stop_reason why = run(&m, 1000);
    EQ64(why, UAM_STOP_DEADLOCK, "checkstop: nothing runs again");
    uam_machine_fini(&m);

    /* A bus error with ME = 0 checkstops too. */
    ppc_machine(&m, NULL, 0);
    put32(&m, 0x1000, 0x3C80F000u);   /* lis r4,0xf000 */
    put32(&m, 0x1004, 0x80640000u);   /* lwz r3,0(r4): nothing there */
    put32(&m, 0x1008, B_SELF);
    why = run(&m, 1000);
    EQ64(why, UAM_STOP_DEADLOCK, "bus error without ME");
    EQ64(m.cpu.pc, 0x1004, "stopped at the access");
    uam_machine_fini(&m);
}

/* ---- the external interrupt -------------------------------------------------- */

static void test_external(void)
{
    uam_machine m;
    ppc_machine(&m, NULL, MSR_ME);
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_INT(0), 1);
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_INT(3), 1);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x1000, "masked while EE = 0");
    sysw(&m, SR_MSR, MSR_ME | MSR_EE);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x500, "external vector");
    EQ64(m.cpu.isa->irq_lines(&m.cpu), 0x9, "both sources reported");
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_INT(0), 0);
    m.cpu.pc = 0x1000;
    sysw(&m, SR_MSR, MSR_ME | MSR_EE);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x500, "wired-OR: one source still high");
    EQ64(m.cpu.isa->irq_lines(&m.cpu), 0x8, "that one");
    uam_cpu_irq_sink(&m.cpu, UAM_PPC_IRQ_INT(3), 0);
    m.cpu.pc = 0x1000;
    sysw(&m, SR_MSR, MSR_ME | MSR_EE);
    run(&m, 10);
    EQ64(m.cpu.pc, 0x1000, "all low: nothing taken");
    uam_machine_fini(&m);
}

/* ---- the decrementer --------------------------------------------------------- */

static void test_decrementer(void)
{
    uam_machine m;
    ppc_machine(&m, NULL, MSR_ME);
    EQ64(uam_sched_count(&m.sched), 0, "no timer while EE = 0");
    sysw(&m, SPR_DEC, 10);
    EQ64(uam_sched_count(&m.sched), 0, "still none after a DEC write");
    run(&m, 100);
    EQ64(m.cpu.pc, 0x1000, "not taken while EE = 0");
    CHECK(sysr(&m, SPR_DEC) > 0x80000000u, "DEC went negative: 0x%x", sysr(&m, SPR_DEC));
    sysw(&m, SR_MSR, MSR_ME | MSR_EE);
    run(&m, 1);
    EQ64(m.cpu.pc, 0x900, "latched, taken once enabled");
    EQ64(uam_sched_count(&m.sched), 0, "the handler runs with EE = 0");
    m.cpu.pc = 0x1000;
    sysw(&m, SR_MSR, MSR_ME | MSR_EE);
    EQ64(uam_sched_count(&m.sched), 1, "EE = 1: the next transition is timed");
    run(&m, 100);
    EQ64(m.cpu.pc, 0x1000, "taking it cancelled the request");
    sysw(&m, SPR_DEC, 50);
    uam_stop_reason why = run(&m, 40);
    EQ64(m.cpu.pc, 0x1000, "not yet");
    why = run(&m, 20);
    (void)why;
    EQ64(m.cpu.pc, 0x900, "at 0 -> -1");

    /* MSR[POW] with EE = 0 sleeps until a reset or machine check only. */
    uam_machine_reset(&m, 0x1000);
    sysw(&m, SR_MSR, MSR_ME);
    put32(&m, 0x1000, 0x3C600004u);   /* lis r3,4: POW */
    put32(&m, 0x1004, 0x7C600124u);   /* mtmsr r3 */
    put32(&m, 0x1008, B_SELF);
    sysw(&m, SPR_DEC, 5);
    why = run(&m, 1000);
    EQ64(why, UAM_STOP_DEADLOCK, "asleep with interrupts off: deadlock");
    uam_machine_fini(&m);
}

/* ---- implementation SPRs ----------------------------------------------------- */

typedef struct hooks {
    uint32_t hid0, writes;
} hooks;

static uam_status spr_read(void *ctx, uam_cpu *cpu, unsigned spr, uint32_t *val)
{
    (void)cpu;
    hooks *h = ctx;
    if (spr == 1008) { *val = h->hid0 | 0x1; return UAM_OK; }
    if (spr == 1010) return UAM_ERR_ILLEGAL;
    return UAM_ERR_INVALID;
}

static uam_status spr_write(void *ctx, uam_cpu *cpu, unsigned spr, uint32_t val)
{
    (void)cpu;
    hooks *h = ctx;
    if (spr == 1008) { h->hid0 = val; h->writes++; return UAM_OK; }
    if (spr == 1010) return UAM_ERR_ILLEGAL;
    return UAM_ERR_INVALID;
}

static const uam_ppc_spr_ops ops = { .read = spr_read, .write = spr_write };

static void test_spr_ops(void)
{
    uam_machine m;
    hooks h = { 0 };
    ppc_machine(&m, NULL, MSR_ME);
    CHECK(uam_ppc_set_spr_ops(&m.cpu, &ops, &h) == UAM_OK, "set ops");
    uam_cpu_reg_write(&m.cpu, 4 * 4, 32, 0x80000000u);
    put32(&m, 0x1000, MTSPR(1008, 4));   /* HID0 = r4: the board's */
    put32(&m, 0x1004, MFSPR(3, 1008));
    put32(&m, 0x1008, MTSPR(1011, 4));   /* nobody's: storage */
    put32(&m, 0x100C, MFSPR(5, 1011));
    put32(&m, 0x1010, MFSPR(6, 1010));   /* does not exist */
    put32(&m, 0x1014, B_SELF);
    run(&m, 20);
    EQ64(h.hid0, 0x80000000u, "the board saw the write"); EQ64(h.writes, 1, "once");
    EQ64(gpr(&m, 3), 0x80000001u, "and answered the read");
    EQ64(gpr(&m, 5), 0x80000000u, "unclaimed: reads back");
    EQ64(m.cpu.pc, 0x700, "missing SPR: program exception");
    EQ64(sysr(&m, SPR_SRR0), 0x1010, "at the mfspr");
    EQ64(sysr(&m, SPR_SRR1) & 0xFFFF0000u, 0x00080000u, "illegal instruction");
    uam_machine_fini(&m);

    uam_machine mm;
    CHECK(uam_machine_init(&mm, &uam_isa_mips32be) == UAM_OK, "mips init");
    CHECK(uam_ppc_set_spr_ops(&mm.cpu, &ops, &h) == UAM_ERR_INVALID, "not a PowerPC hart");
    uam_ppc_config c = uam_ppc_ref;
    CHECK(uam_ppc_configure(&mm.cpu, &c) == UAM_ERR_INVALID, "configure: not a PowerPC hart");
    uam_machine_fini(&mm);
}

/* ---- the CPU model ------------------------------------------------------------- */

static void test_config(void)
{
    uam_machine m;
    uam_ppc_config c = uam_ppc_ref;

    CHECK(uam_machine_init(&m, &uam_isa_ppc32) == UAM_OK, "init");
    c.cache_block = 24;
    CHECK(uam_ppc_configure(&m.cpu, &c) == UAM_ERR_INVALID, "cache block not a power of two");
    c = uam_ppc_ref;
    c.bat_pairs = 6;
    CHECK(uam_ppc_configure(&m.cpu, &c) == UAM_ERR_INVALID, "6 BAT pairs");
    c = uam_ppc_ref;
    c.tb_div = 0;
    CHECK(uam_ppc_configure(&m.cpu, &c) == UAM_ERR_INVALID, "tb_div 0");
    uam_machine_fini(&m);

    /* No FPU: FP instructions are illegal, MSR[FP] stays clear. */
    c = uam_ppc_ref;
    c.fpu = false;
    c.pvr = 0x12340101u;
    ppc_machine(&m, &c, MSR_ME | MSR_FP);
    EQ64(sysr(&m, SR_MSR) & MSR_FP, 0, "no FP bit without an FPU");
    EQ64(sysr(&m, SPR_PVR), 0x12340101u, "pvr");
    put32(&m, 0x1000, 0xFC22182Au);   /* fadd f1,f2,f3 */
    run(&m, 5);
    EQ64(m.cpu.pc, 0x700, "no FPU: program exception");
    EQ64(sysr(&m, SPR_SRR1) & 0xFFFF0000u, 0x00080000u, "illegal");
    uam_machine_fini(&m);

    /* With an FPU but MSR[FP] = 0: FP unavailable. */
    ppc_machine(&m, NULL, MSR_ME);
    put32(&m, 0x1000, 0xFC22182Au);
    run(&m, 5);
    EQ64(m.cpu.pc, 0x800, "FP unavailable");
    uam_machine_fini(&m);

    /* Eight BAT pairs: DBAT4 (SPRs 568/569) translates; with four they are plain SPRs. */
    for (unsigned pairs = 4; pairs <= 8; pairs += 4) {
        c = uam_ppc_ref;
        c.bat_pairs = pairs;
        ppc_machine(&m, &c, MSR_ME);
        put32(&m, 0x2000, 0x12345678u);
        sysw(&m, 568, 0x40000000u | 0x2u);       /* DBAT4U: 0x40000000, 128 KiB, Vs */
        sysw(&m, 569, 0x00000002u);              /* DBAT4L: -> 0, read/write */
        sysw(&m, SR_MSR, MSR_ME | MSR_DR);
        uam_cpu_reg_write(&m.cpu, 4 * 4, 32, 0x40002000u);
        put32(&m, 0x1000, 0x80640000u);          /* lwz r3,0(r4) */
        put32(&m, 0x1004, B_SELF);
        run(&m, 5);
        if (pairs == 8) {
            EQ64(gpr(&m, 3), 0x12345678u, "DBAT4 maps the load");
            EQ64(m.cpu.pc, 0x1004, "no fault");
        } else {
            EQ64(m.cpu.pc, 0x300, "four pairs: DSI");
            EQ64(sysr(&m, 568), 0x40000002u, "and SPR 568 is plain storage");
        }
        uam_machine_fini(&m);
    }

    /* A 64-byte cache block. */
    c = uam_ppc_ref;
    c.cache_block = 64;
    ppc_machine(&m, &c, MSR_ME);
    for (uint32_t a = 0x3000; a < 0x3100; a += 4)
        put32(&m, a, 0xFFFFFFFFu);
    uam_cpu_reg_write(&m.cpu, 4 * 4, 32, 0x3048u);
    put32(&m, 0x1000, 0x7C0027ECu);   /* dcbz 0,r4 */
    put32(&m, 0x1004, B_SELF);
    run(&m, 5);
    uint64_t v = 0;
    uam_bus_read(&m.bus, 0x303C, 4, UAM_ACC_BE, &v); EQ64(v, 0xFFFFFFFFu, "below the block");
    uam_bus_read(&m.bus, 0x3040, 4, UAM_ACC_BE, &v); EQ64(v, 0, "block start");
    uam_bus_read(&m.bus, 0x307C, 4, UAM_ACC_BE, &v); EQ64(v, 0, "block end");
    uam_bus_read(&m.bus, 0x3080, 4, UAM_ACC_BE, &v); EQ64(v, 0xFFFFFFFFu, "past the block");
    uam_machine_fini(&m);

    /* The time base at a quarter of the tick rate; vectors low from reset. */
    c = uam_ppc_ref;
    c.tb_div = 4;
    c.reset_ip = false;
    CHECK(uam_machine_init(&m, &uam_isa_ppc32) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1 << 20) == UAM_OK, "ram");
    CHECK(uam_ppc_configure(&m.cpu, &c) == UAM_OK, "configure");
    put32(&m, 0x1000, B_SELF);
    uam_machine_reset(&m, 0x1000);
    EQ64(sysr(&m, SR_MSR) & MSR_IP, 0, "reset_ip = false");
    run(&m, 400);
    uint32_t tb = sysr(&m, 268);
    CHECK(tb >= 99 && tb <= 101, "tb at a quarter rate: %u", tb);
    uam_machine_fini(&m);

    c = uam_ppc_ref;
    ppc_machine(&m, NULL, MSR_ME);
    uam_machine_reset(&m, 0x1000);
    EQ64(sysr(&m, SR_MSR), MSR_IP, "reference: vectors high from reset");
    EQ64(sysr(&m, SPR_PVR), uam_ppc_ref.pvr, "reference pvr");
    uam_machine_fini(&m);
}

int main(void)
{
    test_pins();
    test_external();
    test_decrementer();
    test_spr_ops();
    test_config();
    printf("test_ppc: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
