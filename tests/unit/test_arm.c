/*
 * test_arm.c - The ARMv7-A module's parts the self-test firmware cannot reach on arm-ref:
 * the reset state of both descriptors and the CPU model (uam_arm_configure, MPIDR per
 * processor), the register banks of every mode seen through the system registers (User
 * registers from FIQ mode, the banked SPs, SPSRs, reserved modes, User mode's writes), the
 * entry of each exception in ARM and Thumb state (mode, SPSR, the LR offset, the A/I/F
 * masks, IFSR/IFAR on a fetch bus error, DFSR/DFAR, SPSR.IT after an SVC in an IT block),
 * the vector base (VBAR, SCTLR.V), SCTLR.TE and EE at entry, CP15 from User mode, the IRQ
 * and FIQ inputs through the board's API (masks, FIQ above IRQ, WFI waking regardless of
 * CPSR.I, the lines reported), an interrupt return through SPSR, the generic timer's
 * outputs (the sink, IMASK, CNTVOFF, CNTKCTL), translation table formats the self-test
 * does not build (supersections, large pages, the TTBR0/TTBR1 split, PD1, manager domains,
 * walks that hit nothing, big-endian tables), the debugger view, the dump, a snapshot of
 * the ISA state, BE8 data against little-endian code, and the arm-ref board's GIC: SPI
 * priorities, the priority mask and running priority, level inputs after EOI, and the
 * generic timer's PPI.
 *
 * Each test runs on a bare machine: RAM at 0 (the vectors) and at 0xFFFF0000 (the high
 * vectors), every vector a branch to itself, so an exception or interrupt leaves the hart
 * spinning where it was delivered. The tests that execute code run interpreted and under
 * the JIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../isa/arm/arm.h"
#include "uam/boards.h"
#include "uam/bus.h"
#include "uam/dev/gic.h"
#include "uam/isa/arm.h"
#include "uam/isa/z80.h"
#include "uam/machine.h"
#include "uam/sched.h"
#include "uam/snapshot.h"

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

#define RAM_SIZE 0x00100000u
#define HIVEC    0xFFFF0000u
#define CODE     0x00001000u
#define DATA     0x00008000u
#define NOWHERE  0x20000000u   /* nothing mapped */

/* ---- instruction encodings (each checked with objdump) ------------------------------ */

#define A_UDF      0xE7F000F0u   /* udf #0 */
#define A_SVC      0xEF000012u   /* svc #0x12 */
#define A_BKPT     0xE1200073u   /* bkpt #3 */
#define A_LDR      0xE5921000u   /* ldr r1, [r2] */
#define A_STR4     0xE5821004u   /* str r1, [r2, #4] */
#define A_LDREX    0xE1921F9Fu   /* ldrex r1, [r2] */
#define A_BX_R2    0xE12FFF12u   /* bx r2 */
#define A_SPIN     0xEAFFFFFEu   /* b . */
#define A_WFI      0xE320F003u
#define A_WFINE    0x1320F003u
#define A_ERET     0xE25EF004u   /* subs pc, lr, #4 */
#define A_SPSR_C   0xE361F09Fu   /* msr spsr_c, #0x9f (System, I set) */
#define A_STR3     0xE5823000u   /* str r3, [r2] */
#define A_LDRB4    0xE5D24000u   /* ldrb r4, [r2] */
#define A_LDRH5    0xE1D250B0u   /* ldrh r5, [r2] */
#define A_LDR6     0xE5926000u   /* ldr r6, [r2] */
/* mrc/mcr p15, opc1, rt, CRn, CRm, opc2 */
static uint32_t MRC(unsigned o1, unsigned rt, unsigned n, unsigned m, unsigned o2)
{
    return 0xEE100F10u | o1 << 21 | n << 16 | rt << 12 | o2 << 5 | m;
}
static uint32_t MCR(unsigned o1, unsigned rt, unsigned n, unsigned m, unsigned o2)
{
    return MRC(o1, rt, n, m, o2) & ~(1u << 20);
}
#define T_UDF      0xDE00u       /* udf #0 */
#define T_UDF_W    0xF7F0A000u   /* udf.w #0 */
#define T_SVC      0xDF12u       /* svc #0x12 */
#define T_BKPT     0xBE03u       /* bkpt #3 */
#define T_LDR      0x6811u       /* ldr r1, [r2] */
#define T_STR4     0x6051u       /* str r1, [r2, #4] */
#define T_BX_R2    0x4710u       /* bx r2 */
#define T_SPIN     0xE7FEu       /* b.n . */
#define T_WFI      0xBF30u
#define T_ITT_EQ   0xBF04u       /* itt eq */
#define T_SVC1     0xDF01u       /* svc #1 */
#define T_ADD1     0x3101u       /* add(s) r1, #1 */

/* CP15 registers by system register id. */
#define SCTLR    ARM_SR_CP(15, 0, 1, 0, 0)
#define MIDR     ARM_SR_CP(15, 0, 0, 0, 0)
#define MPIDR    ARM_SR_CP(15, 0, 0, 0, 5)
#define TTBR0    ARM_SR_CP(15, 0, 2, 0, 0)
#define TTBR1    ARM_SR_CP(15, 0, 2, 0, 1)
#define TTBCR    ARM_SR_CP(15, 0, 2, 0, 2)
#define DACR     ARM_SR_CP(15, 0, 3, 0, 0)
#define DFSR     ARM_SR_CP(15, 0, 5, 0, 0)
#define IFSR     ARM_SR_CP(15, 0, 5, 0, 1)
#define DFAR     ARM_SR_CP(15, 0, 6, 0, 0)
#define IFAR     ARM_SR_CP(15, 0, 6, 0, 2)
#define PAR      ARM_SR_CP(15, 0, 7, 4, 0)
#define ATS1CUR  ARM_SR_CP(15, 0, 7, 8, 2)
#define VBAR     ARM_SR_CP(15, 0, 12, 0, 0)
#define ISR      ARM_SR_CP(15, 0, 12, 1, 0)
#define TPIDRURW ARM_SR_CP(15, 0, 13, 0, 2)
#define TPIDRPRW ARM_SR_CP(15, 0, 13, 0, 4)
#define CNTFRQ   ARM_SR_CP(15, 0, 14, 0, 0)
#define CNTKCTL  ARM_SR_CP(15, 0, 14, 1, 0)
#define CNTP_CTL ARM_SR_CP(15, 0, 14, 2, 1)
#define CNTV_CTL ARM_SR_CP(15, 0, 14, 3, 1)
#define CNTV_TVAL ARM_SR_CP(15, 0, 14, 3, 0)
#define CNTPCT   ARM_SR_CP64(15, 0, 14)
#define CNTVCT   ARM_SR_CP64(15, 1, 14)
#define CNTP_CVAL ARM_SR_CP64(15, 2, 14)
#define CNTV_CVAL ARM_SR_CP64(15, 3, 14)
#define CNTVOFF  ARM_SR_CP64(15, 4, 14)
#define CPSR_W   (ARM_SR_CPSR | 3)          /* the c and x fields: mode, A, I, F, E */
#define SPSR_W   (ARM_SR_SPSR | 0xF)

/* ---- the machine ----------------------------------------------------------------- */

static bool is_be(uam_machine *m) { return m->cpu.isa == &uam_isa_armeb; }

/* Instructions are little-endian on both descriptors (BE8). */
static uint32_t a32(uam_machine *m, uint32_t a, uint32_t w)
{
    const uint8_t b[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
    CHECK(uam_bus_load_bytes(&m->bus, a, b, 4) == UAM_OK, "load at 0x%x", (unsigned)a);
    return a + 4;
}
static uint32_t t16(uam_machine *m, uint32_t a, uint32_t hw)
{
    const uint8_t b[2] = { (uint8_t)hw, (uint8_t)(hw >> 8) };
    CHECK(uam_bus_load_bytes(&m->bus, a, b, 2) == UAM_OK, "load at 0x%x", (unsigned)a);
    return a + 2;
}
static uint32_t t32(uam_machine *m, uint32_t a, uint32_t w) { return t16(m, t16(m, a, w >> 16), w & 0xFFFF); }
/* A data word in the hart's reset byte order. */
static void data32(uam_machine *m, uint32_t a, uint32_t v)
{
    uam_bus_write(&m->bus, a, 4, is_be(m) ? UAM_ACC_BE : UAM_ACC_LE, v);
}
static uint8_t mem8(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}
static uint32_t r(uam_machine *m, unsigned n) { return (uint32_t)uam_cpu_reg_read(&m->cpu, ARM_REG_OFF(n), 32); }
static void setr(uam_machine *m, unsigned n, uint32_t v) { uam_cpu_reg_write(&m->cpu, ARM_REG_OFF(n), 32, v); }
static void sysw(uam_machine *m, uint32_t id, uint64_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "write sysreg 0x%x", (unsigned)id);
}
static uint64_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, id, &v) == UAM_OK, "read sysreg 0x%x", (unsigned)id);
    return v;
}
static unsigned mode(uam_machine *m) { return (unsigned)sysr(m, ARM_SR_CPSR) & CPSR_M_MASK; }
/* Switch mode (and set A, I, F, E) through the CPSR's control fields. */
static void to_mode(uam_machine *m, uint32_t cpsr) { sysw(m, CPSR_W, cpsr); }
static uint32_t cpsr_e(uam_machine *m) { return is_be(m) ? CPSR_E : 0; }
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static void run(uam_machine *m, uint32_t pc, uint64_t n)
{
    m->cpu.pc = pc;
    uam_exec_run(&m->exec, n);
}
static void tick(uam_machine *m, uint64_t n)
{
    m->sched.idle += n;
    uam_sched_run_due(&m->sched);
}
static void vectors(uam_machine *m, uint32_t base, bool thumb)
{
    for (uint32_t v = 0; v < 0x20; v += 4)
        thumb ? (void)t16(m, base + v, T_SPIN) : (void)a32(m, base + v, A_SPIN);
}

/* A hart reset at CODE, the vectors at 0 and HIVEC spinning, in System mode with A, I and
 * F clear and N and C set. */
static void machine(uam_machine *m, const uam_isa *isa, bool jit)
{
    CHECK(uam_machine_init(m, isa) == UAM_OK, "machine init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, RAM_SIZE) == UAM_OK, "map RAM");
    CHECK(uam_bus_map_ram(&m->bus, "hivec", HIVEC, 0x10000) == UAM_OK, "map the high vectors");
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    uam_machine_reset(m, CODE);
    vectors(m, 0, false);
    vectors(m, HIVEC, false);
    to_mode(m, ARM_MODE_SYS | cpsr_e(m));
    m->cpu.regfile[ARM_N_OFF] = m->cpu.regfile[ARM_C_OFF] = 1;
}
#define SPSR0 (CPSR_N | CPSR_C | ARM_MODE_SYS)   /* the CPSR the tests start with */

/* The hart entered `mode` at vector `pc` with LR, SPSR and the CPSR given. */
static void expect_entry(uam_machine *m, unsigned md, uint32_t pc, uint32_t lr, uint32_t spsr,
                         uint32_t cpsr, const char *what)
{
    CHECK(m->cpu.pc == pc, "%s: at 0x%x (pc 0x%llx)", what, (unsigned)pc, (unsigned long long)m->cpu.pc);
    CHECK(mode(m) == md, "%s: mode 0x%x want 0x%x", what, mode(m), md);
    CHECK(r(m, ARM_LR) == lr, "%s: LR 0x%x want 0x%x", what, (unsigned)r(m, ARM_LR), (unsigned)lr);
    CHECK(sysr(m, ARM_SR_SPSR) == spsr, "%s: SPSR 0x%llx want 0x%x", what,
          (unsigned long long)sysr(m, ARM_SR_SPSR), (unsigned)spsr);
    CHECK(sysr(m, ARM_SR_CPSR) == (cpsr | md), "%s: CPSR 0x%llx want 0x%x", what,
          (unsigned long long)sysr(m, ARM_SR_CPSR), (unsigned)(cpsr | md));
    CHECK(ARM_PRIV_IT(m->cpu.priv) == 0, "%s: outside any IT block", what);
}

/* ---- reset and the CPU model ------------------------------------------------------ */

static void test_reset(void)
{
    static const uam_isa *const isas[] = { &uam_isa_arm, &uam_isa_armeb };
    for (unsigned k = 0; k < 2; k++) {
        uam_machine m;
        const bool be = k == 1;
        CHECK(uam_machine_init(&m, isas[k]) == UAM_OK, "init");
        CHECK(uam_arm_is(m.cpu.isa), "an ARM descriptor");
        uam_machine_reset(&m, 0x8001);
        EQ64(m.cpu.pc, 0x8001, "an entry with bit 0 starts in Thumb state");
        EQ64(sysr(&m, ARM_SR_CPSR), ARM_MODE_SVC | CPSR_A | CPSR_I | CPSR_F | (be ? CPSR_E : 0),
             "reset CPSR: SVC, A I F masked, E = CFGEND");
        EQ64(sysr(&m, SCTLR), 0x00C50078u | (be ? SCTLR_EE : 0), "reset SCTLR");
        EQ64(sysr(&m, VBAR), 0, "VBAR 0");
        EQ64(sysr(&m, MIDR), uam_arm_ref.midr, "MIDR of uam_arm_ref");
        EQ64(sysr(&m, MPIDR), uam_arm_ref.mpidr, "MPIDR of processor 0");
        EQ64(sysr(&m, CNTFRQ), uam_arm_ref.cntfrq, "CNTFRQ");
        EQ64(sysr(&m, ARM_SR_SPSR), 0, "SPSR_svc 0");
        EQ64(m.cpu.priv, ARM_PRIV_IT(0) | (be ? ARM_PRIV_BE : 0), "privileged, data order tag");
        uam_machine_fini(&m);
    }

    /* a board's model: in effect after reset, the identity per processor */
    uam_machine m;
    uam_arm_config cfg = uam_arm_ref;
    cfg.name = "board";
    cfg.midr = 0x410FC075u;
    cfg.mpidr = 0x80000F00u;
    cfg.cntfrq = 24000000u;
    CHECK(uam_machine_init(&m, &uam_isa_arm) == UAM_OK, "init");
    CHECK(uam_machine_add_cpus(&m, 1) == UAM_OK, "a second processor");
    CHECK(uam_arm_configure(&m.cpu, NULL) == UAM_ERR_INVALID, "configure needs a model");
    CHECK(uam_arm_configure(NULL, &cfg) == UAM_ERR_INVALID, "and a hart");
    for (unsigned i = 0; i < 2; i++)
        CHECK(uam_arm_configure(uam_machine_cpu(&m, i), &cfg) == UAM_OK, "configure %u", i);
    uam_machine_reset(&m, 0);
    EQ64(sysr(&m, MIDR), 0x410FC075u, "the board's MIDR");
    EQ64(sysr(&m, CNTFRQ), 24000000u, "the board's CNTFRQ");
    uint64_t v = 0;
    uam_cpu *c1 = uam_machine_cpu(&m, 1);
    CHECK(c1->isa->sysreg_read(c1, MPIDR, &v) == UAM_OK && v == 0x80000F01u, "MPIDR.Aff0 of processor 1");
    sysw(&m, CNTFRQ, 1000);
    uam_machine_reset(&m, 0);
    EQ64(sysr(&m, CNTFRQ), 24000000u, "CNTFRQ back to the model's at reset");
    uam_machine_fini(&m);

    CHECK(uam_machine_init(&m, &uam_isa_z80) == UAM_OK, "a Z80");
    CHECK(uam_arm_configure(&m.cpu, &cfg) == UAM_ERR_INVALID, "an ARM model on a Z80");
    CHECK(uam_arm_set_timer_sink(&m.cpu, NULL, NULL) == UAM_ERR_INVALID, "no timer sink on a Z80");
    uam_machine_fini(&m);
}

/* ---- modes and banked registers ---------------------------------------------------- */

static void test_banks(void)
{
    static const unsigned modes[] = { ARM_MODE_USR, ARM_MODE_FIQ, ARM_MODE_IRQ, ARM_MODE_SVC,
                                      ARM_MODE_ABT, ARM_MODE_UND, ARM_MODE_SYS };
    const unsigned nm = UAM_ARRAY_LEN(modes);
    uam_machine m;
    /* User mode cannot leave by writing the CPSR */
    machine(&m, &uam_isa_arm, false);
    to_mode(&m, ARM_MODE_USR);
    to_mode(&m, ARM_MODE_SVC | CPSR_I);
    EQ64(sysr(&m, ARM_SR_CPSR), ARM_MODE_USR, "User mode ignores writes to the CPSR's mode and masks");
    uam_machine_fini(&m);

    /* r8-r14 and the SPSR set in each mode, User's through System and ARM_SR_USRREG */
    machine(&m, &uam_isa_arm, false);
    setr(&m, 0, 0x1234);
    for (unsigned i = 1; i < nm; i++) {
        to_mode(&m, modes[i] | CPSR_I | CPSR_F);
        for (unsigned n = 8; n < 15; n++)
            setr(&m, n, i << 8 | n);
        sysw(&m, SPSR_W, 0xF0000000u | i);
    }
    to_mode(&m, ARM_MODE_FIQ | CPSR_I | CPSR_F);
    for (unsigned n = 8; n < 15; n++)
        EQ64(sysr(&m, ARM_SR_USRREG | n), (6u << 8) | n, "User r8-r14 from FIQ mode (System's)");
    sysw(&m, ARM_SR_USRREG | 14, 0xAAAA);
    EQ64(r(&m, 14), 1u << 8 | 14, "FIQ lr kept");
    for (unsigned i = 1; i < nm; i++) {
        to_mode(&m, modes[i] | CPSR_I | CPSR_F);
        char what[64];
        snprintf(what, sizeof(what), "mode 0x%x", modes[i]);
        const unsigned lo = modes[i] == ARM_MODE_FIQ ? 1 : 6;      /* r8-r12: FIQ's or the rest's */
        for (unsigned n = 8; n < 13; n++)
            EQ64(r(&m, n), lo << 8 | n, what);
        EQ64(r(&m, 13), i << 8 | 13, what);
        EQ64(r(&m, 14), modes[i] == ARM_MODE_SYS ? 0xAAAA : (i << 8 | 14), what);
        EQ64(sysr(&m, ARM_SR_SPSR), modes[i] == ARM_MODE_SYS ? 0 : 0xF0000000u | i, what);
        EQ64(r(&m, 0), 0x1234, "r0 is not banked");
        EQ64(sysr(&m, ARM_SR_BANKSP | ARM_MODE_ABT), 4u << 8 | 13, "the Abort SP from any mode");
    }
    sysw(&m, ARM_SR_BANKSP | ARM_MODE_UND, 0x5000);
    to_mode(&m, ARM_MODE_UND | CPSR_I);
    EQ64(r(&m, 13), 0x5000, "the Undefined SP written from System mode");
    CHECK(m.cpu.isa->sysreg_read(&m.cpu, ARM_SR_BANKSP | 0x1A, &(uint64_t){0}) == UAM_ERR_ILLEGAL,
          "no bank for a reserved mode");
    to_mode(&m, 0x1A | CPSR_I);
    EQ64(mode(&m), ARM_MODE_UND, "a reserved mode is ignored");
    uam_machine_fini(&m);
}

/* ---- exception entry ---------------------------------------------------------------- */

/* One instruction at CODE that raises an exception; r2 = base. Returns the machine. */
static void raise1(uam_machine *m, bool jit, bool thumb, uint32_t insn, uint32_t base)
{
    machine(m, &uam_isa_arm, jit);
    if (!thumb)
        a32(m, a32(m, CODE, insn), A_SPIN);
    else if (insn > 0xFFFF)
        t16(m, t32(m, CODE, insn), T_SPIN);
    else
        t16(m, t16(m, CODE, insn), T_SPIN);
    setr(m, 2, base);
    run(m, CODE | thumb, 8);
}

static void test_exceptions(bool jit)
{
    uam_machine m;
    for (unsigned t = 0; t < 2; t++) {
        const uint32_t T = t ? CPSR_T : 0, sp = SPSR0 | T;
        char what[80];
#define CASE(s) snprintf(what, sizeof(what), "%s %s", t ? "thumb" : "arm", s)
        CASE("udf");
        raise1(&m, jit, t, t ? T_UDF : A_UDF, 0);
        expect_entry(&m, ARM_MODE_UND, 0x04, CODE + (t ? 2 : 4), sp, CPSR_I, what);
        uam_machine_fini(&m);
        if (t) {
            /* a 32-bit Thumb UDF: the link is still the instruction + 2 */
            raise1(&m, jit, t, T_UDF_W, 0);
            expect_entry(&m, ARM_MODE_UND, 0x04, CODE + 2, sp, CPSR_I, "thumb udf.w");
            uam_machine_fini(&m);
        }
        CASE("svc");
        raise1(&m, jit, t, t ? T_SVC : A_SVC, 0);
        expect_entry(&m, ARM_MODE_SVC, 0x08, CODE + (t ? 2 : 4), sp, CPSR_I, what);
        uam_machine_fini(&m);
        CASE("bkpt");
        raise1(&m, jit, t, t ? T_BKPT : A_BKPT, 0);
        expect_entry(&m, ARM_MODE_ABT, 0x0C, CODE + 4, sp, CPSR_A | CPSR_I, what);
        EQ64(sysr(&m, IFSR), ARM_FS_DEBUG, "bkpt: IFSR debug event");
        uam_machine_fini(&m);
        CASE("load from nothing");
        raise1(&m, jit, t, t ? T_LDR : A_LDR, NOWHERE);
        expect_entry(&m, ARM_MODE_ABT, 0x10, CODE + 8, sp, CPSR_A | CPSR_I, what);
        EQ64(sysr(&m, DFSR), ARM_FS_EXT, "load: DFSR external abort, read");
        EQ64(sysr(&m, DFAR), NOWHERE, "load: DFAR");
        uam_machine_fini(&m);
        CASE("store to nothing");
        raise1(&m, jit, t, t ? T_STR4 : A_STR4, NOWHERE);
        expect_entry(&m, ARM_MODE_ABT, 0x10, CODE + 8, sp, CPSR_A | CPSR_I, what);
        EQ64(sysr(&m, DFSR), ARM_FS_EXT | 1u << 11, "store: DFSR WnR");
        EQ64(sysr(&m, DFAR), NOWHERE + 4, "store: DFAR");
        uam_machine_fini(&m);
        /* a branch to nothing: the prefetch abort is the target's, in the target's state */
        CASE("fetch from nothing");
        raise1(&m, jit, t, t ? T_BX_R2 : A_BX_R2, NOWHERE | t);
        expect_entry(&m, ARM_MODE_ABT, 0x0C, NOWHERE + 4, sp, CPSR_A | CPSR_I, what);
        EQ64(sysr(&m, IFSR), ARM_FS_EXT, "fetch: IFSR external abort");
        EQ64(sysr(&m, IFAR), NOWHERE, "fetch: IFAR");
        uam_machine_fini(&m);
#undef CASE
    }
    /* an unaligned LDREX: alignment fault, DFAR the address */
    raise1(&m, jit, false, A_LDREX, DATA + 1);
    expect_entry(&m, ARM_MODE_ABT, 0x10, CODE + 8, SPSR0, CPSR_A | CPSR_I, "ldrex misaligned");
    EQ64(sysr(&m, DFSR), ARM_FS_ALIGN, "ldrex: DFSR alignment");
    EQ64(sysr(&m, DFAR), DATA + 1, "ldrex: DFAR");
    uam_machine_fini(&m);

    /* an SVC inside an IT block: SPSR.IT holds the state of the instruction after it */
    machine(&m, &uam_isa_arm, jit);
    t16(&m, t16(&m, t16(&m, t16(&m, CODE, T_ITT_EQ), T_SVC1), T_ADD1), T_SPIN);
    m.cpu.regfile[ARM_Z_OFF] = 1;
    run(&m, CODE | 1, 8);
    expect_entry(&m, ARM_MODE_SVC, 0x08, CODE + 4, SPSR0 | CPSR_Z | CPSR_T | 0x08u << 8, CPSR_I,
                 "svc in an IT block");
    uam_machine_fini(&m);

    /* the vector base: VBAR (bits 4:0 ignored), SCTLR.V over it; SCTLR.TE: Thumb handlers;
     * SCTLR.EE: the handler's data byte order */
    machine(&m, &uam_isa_arm, jit);
    vectors(&m, 0x4000, false);
    a32(&m, CODE, A_SVC);
    sysw(&m, VBAR, 0x401Fu);
    EQ64(sysr(&m, VBAR), 0x4000, "VBAR bits 4:0 RES0");
    run(&m, CODE, 4);
    EQ64(m.cpu.pc, 0x4008, "svc through VBAR");
    to_mode(&m, ARM_MODE_SYS);
    sysw(&m, SCTLR, sysr(&m, SCTLR) | SCTLR_V | SCTLR_EE);
    run(&m, CODE, 4);
    EQ64(m.cpu.pc, HIVEC + 8, "SCTLR.V: the high vectors, VBAR ignored");
    EQ64(sysr(&m, ARM_SR_CPSR), ARM_MODE_SVC | CPSR_I | CPSR_E, "SCTLR.EE: entered big-endian");
    EQ64(m.cpu.priv & ARM_PRIV_BE, ARM_PRIV_BE, "and translated as such");
    to_mode(&m, ARM_MODE_SYS);
    sysw(&m, SCTLR, (sysr(&m, SCTLR) & ~(SCTLR_V | SCTLR_EE)) | SCTLR_TE);
    vectors(&m, 0x4000, true);
    run(&m, CODE, 4);
    EQ64(m.cpu.pc, 0x4008 | 1, "SCTLR.TE: the handler runs in Thumb state");
    EQ64(sysr(&m, ARM_SR_SPSR), SPSR0, "from ARM state");
    uam_machine_fini(&m);

    /* CP15 from User mode: the thread registers it may use, Undefined for the rest */
    machine(&m, &uam_isa_arm, jit);
    uint32_t a = a32(&m, CODE, MRC(0, 0, 13, 0, 2));            /* TPIDRURW */
    a = a32(&m, a, MCR(0, 3, 13, 0, 2));
    a32(&m, a32(&m, a, MRC(0, 0, 1, 0, 0)), A_SPIN);            /* SCTLR */
    sysw(&m, TPIDRURW, 0x7777);
    setr(&m, 3, 0x8888);
    to_mode(&m, ARM_MODE_USR);
    run(&m, CODE, 8);
    expect_entry(&m, ARM_MODE_UND, 0x04, a + 4, CPSR_N | CPSR_C | ARM_MODE_USR, CPSR_I, "user mrc SCTLR");
    EQ64(r(&m, 0), 0x7777, "user mrc TPIDRURW");
    EQ64(sysr(&m, TPIDRURW), 0x8888, "user mcr TPIDRURW");
    uam_machine_fini(&m);
    machine(&m, &uam_isa_arm, jit);
    a32(&m, a32(&m, CODE, MCR(0, 0, 13, 0, 3)), A_SPIN);       /* TPIDRURO is read-only there */
    to_mode(&m, ARM_MODE_USR);
    run(&m, CODE, 8);
    expect_entry(&m, ARM_MODE_UND, 0x04, CODE + 4, CPSR_N | CPSR_C | ARM_MODE_USR, CPSR_I, "user mcr TPIDRURO");
    uam_machine_fini(&m);
}

/* ---- interrupts --------------------------------------------------------------------- */

static void test_irq_lines(void)
{
    for (unsigned t = 0; t < 2; t++) {
        uam_machine m;
        machine(&m, &uam_isa_arm, false);
        const uint32_t T = t ? CPSR_T : 0, at = 0x3000 | t;
        to_mode(&m, ARM_MODE_SYS | CPSR_I | CPSR_F);
        m.cpu.pc = at;
        CHECK(!take(&m) && !wake(&m), "nothing raised");
        line(&m, UAM_ARM_IRQ, 1);
        CHECK(!take(&m), "IRQ held by CPSR.I");
        CHECK(wake(&m), "but it wakes a WFI");
        EQ64(sysr(&m, ISR), 0x80, "ISR.I");
        to_mode(&m, ARM_MODE_SYS | CPSR_F);
        CHECK(take(&m), "IRQ taken with CPSR.I clear");
        expect_entry(&m, ARM_MODE_IRQ, 0x18, 0x3004, SPSR0 | T | CPSR_F, CPSR_A | CPSR_I | CPSR_F, "irq");
        EQ64(m.cpu.isa->irq_lines(&m.cpu), 1u << UAM_ARM_IRQ, "the IRQ line reported");
        CHECK(!take(&m), "not again inside the handler");
        /* FIQ: above IRQ, taken from the IRQ handler */
        line(&m, UAM_ARM_FIQ, 1);
        EQ64(sysr(&m, ISR), 0xC0, "ISR.I and ISR.F");
        CHECK(!take(&m), "FIQ held by CPSR.F");
        to_mode(&m, ARM_MODE_IRQ | CPSR_A | CPSR_I);
        setr(&m, 8, 0x0808);
        CHECK(take(&m), "FIQ preempts the IRQ handler");
        expect_entry(&m, ARM_MODE_FIQ, 0x1C, 0x1C, ARM_MODE_IRQ | CPSR_A | CPSR_I | CPSR_N | CPSR_C,
                     CPSR_A | CPSR_I | CPSR_F, "fiq");
        EQ64(m.cpu.isa->irq_lines(&m.cpu), 1u << UAM_ARM_FIQ, "the FIQ line reported");
        CHECK(r(&m, 8) != 0x0808, "FIQ mode has its own r8");
        /* both lines, both unmasked: FIQ first */
        to_mode(&m, ARM_MODE_SYS);
        m.cpu.pc = at;
        CHECK(take(&m) && mode(&m) == ARM_MODE_FIQ, "FIQ before IRQ");
        to_mode(&m, ARM_MODE_SYS | CPSR_F);
        m.cpu.pc = at;
        CHECK(take(&m) && mode(&m) == ARM_MODE_IRQ, "IRQ with FIQ masked");
        line(&m, UAM_ARM_IRQ, 0);
        line(&m, UAM_ARM_FIQ, 0);
        to_mode(&m, ARM_MODE_SYS);
        CHECK(!take(&m) && !wake(&m), "lines lowered");
        EQ64(sysr(&m, ISR), 0, "ISR clear");
        uam_machine_fini(&m);
    }
}

/* An interrupt taken between blocks, its handler setting SPSR.I and returning. */
static void test_irq_return(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_arm, jit);
    a32(&m, a32(&m, 0x4018, A_SPSR_C), A_ERET);
    a32(&m, CODE, A_SPIN);
    sysw(&m, VBAR, 0x4000);
    setr(&m, 13, 0x1313);
    setr(&m, 14, 0x1414);
    line(&m, UAM_ARM_IRQ, 1);
    run(&m, CODE, 20);
    uam_exec_stats st;
    uam_machine_get_stats(&m, &st);
    EQ64(st.interrupts, 1, "one interrupt");
    EQ64(m.cpu.pc, CODE, "returned to the loop");
    EQ64(sysr(&m, ARM_SR_CPSR), ARM_MODE_SYS | CPSR_I, "CPSR from the edited SPSR");
    CHECK(m.cpu.regfile[ARM_N_OFF] == 1 && m.cpu.regfile[ARM_C_OFF] == 1, "flags from SPSR (msr spsr_c leaves them)");
    EQ64(r(&m, 13), 0x1313, "System sp back");
    EQ64(r(&m, 14), 0x1414, "System lr back");
    to_mode(&m, ARM_MODE_IRQ | CPSR_I);
    EQ64(r(&m, 14), CODE + 4, "the IRQ lr");
    uam_machine_fini(&m);
}

/* WFI waits; an IRQ line wakes it with CPSR.I set (no interrupt) or enters the handler. */
static void test_wfi(bool jit)
{
    for (unsigned t = 0; t < 2; t++) {
        uam_machine m;
        machine(&m, &uam_isa_arm, jit);
        const uint32_t w = t ? t16(&m, CODE, T_WFI) : a32(&m, CODE, A_WFI);
        t ? (void)t16(&m, w, T_SPIN) : (void)a32(&m, w, A_SPIN);
        to_mode(&m, ARM_MODE_SYS | CPSR_I);
        run(&m, CODE | t, 10);
        CHECK(m.cpu.waiting, "WFI waits");
        EQ64(m.cpu.pc, w | t, "after the WFI");
        line(&m, UAM_ARM_IRQ, 1);
        uam_exec_run(&m.exec, 10);
        CHECK(!m.cpu.waiting, "a masked IRQ wakes it");
        EQ64(m.cpu.pc, w | t, "and it goes on (no interrupt)");
        EQ64(mode(&m), ARM_MODE_SYS, "still System");
        line(&m, UAM_ARM_IRQ, 0);
        to_mode(&m, ARM_MODE_SYS);
        run(&m, CODE | t, 10);
        CHECK(m.cpu.waiting, "waits again");
        line(&m, UAM_ARM_IRQ, 1);
        uam_exec_run(&m.exec, 10);
        expect_entry(&m, ARM_MODE_IRQ, 0x18, w + 4, SPSR0 | (t ? CPSR_T : 0), CPSR_A | CPSR_I,
                     t ? "thumb wfi irq" : "arm wfi irq");
        uam_machine_fini(&m);
    }
    /* a WFI whose condition fails does not wait */
    uam_machine m;
    machine(&m, &uam_isa_arm, jit);
    a32(&m, a32(&m, CODE, A_WFINE), A_SPIN);
    m.cpu.regfile[ARM_Z_OFF] = 1;
    run(&m, CODE, 10);
    CHECK(!m.cpu.waiting && m.cpu.pc == CODE + 4, "WFINE with Z set");
    uam_machine_fini(&m);
}

/* ---- the generic timer -------------------------------------------------------------- */

static int sink_calls, sink_level[2];
static void sink(void *ctx, unsigned timer, int level)
{
    (void)ctx;
    sink_calls++;
    sink_level[timer & 1] = level;
}

static void test_timer(void)
{
    uam_machine m;
    machine(&m, &uam_isa_arm, false);
    CHECK(uam_arm_set_timer_sink(&m.cpu, sink, NULL) == UAM_OK, "sink");
    sink_calls = 0;
    const uint64_t now = sysr(&m, CNTPCT);
    sysw(&m, CNTP_CVAL, now + 100);
    sysw(&m, CNTP_CTL, 1);
    tick(&m, 99);
    CHECK(sink_calls == 0 && (sysr(&m, CNTP_CTL) & 4) == 0, "not before the compare value");
    tick(&m, 1);
    CHECK(sink_calls == 1 && sink_level[UAM_ARM_TIMER_PHYS] == 1, "the physical timer's output at CVAL");
    EQ64(sysr(&m, CNTP_CTL), 5, "ENABLE, ISTATUS");
    sysw(&m, CNTP_CTL, 3);
    CHECK(sink_calls == 2 && sink_level[UAM_ARM_TIMER_PHYS] == 0, "IMASK lowers the output");
    EQ64(sysr(&m, CNTP_CTL), 7, "ISTATUS still set");
    sysw(&m, CNTP_CTL, 0);
    EQ64(sysr(&m, CNTP_CTL), 0, "disabled: no ISTATUS");
    /* the virtual timer counts CNTPCT - CNTVOFF */
    tick(&m, 5000);
    sysw(&m, CNTVOFF, 1000);
    EQ64(sysr(&m, CNTVCT), sysr(&m, CNTPCT) - 1000, "CNTVCT");
    sysw(&m, CNTV_TVAL, 20);
    EQ64(sysr(&m, CNTV_CVAL), sysr(&m, CNTVCT) + 20, "TVAL sets CVAL");
    sysw(&m, CNTV_CTL, 1);
    tick(&m, 19);
    CHECK(sink_level[UAM_ARM_TIMER_VIRT] == 0, "virtual: not yet");
    tick(&m, 1);
    CHECK(sink_level[UAM_ARM_TIMER_VIRT] == 1, "virtual: fires");
    EQ64(sysr(&m, CNTV_TVAL), 0, "TVAL 0 at the deadline");
    tick(&m, 3);
    EQ64(sysr(&m, CNTV_TVAL), 0xFFFFFFFDu, "TVAL counts down past it");
    sysw(&m, CNTVOFF, 0);
    CHECK(sink_level[UAM_ARM_TIMER_VIRT] == 1, "a smaller offset keeps it firing");
    uam_machine_reset(&m, CODE);
    CHECK(sink_level[UAM_ARM_TIMER_VIRT] == 0, "a reset lowers the output");
    /* User mode reaches the counters only as CNTKCTL allows */
    uint64_t v;
    to_mode(&m, ARM_MODE_USR);
    CHECK(m.cpu.isa->sysreg_read(&m.cpu, CNTPCT, &v) == UAM_ERR_ILLEGAL, "CNTPCT: not from User mode");
    CHECK(m.cpu.isa->sysreg_read(&m.cpu, CNTP_CTL, &v) == UAM_ERR_ILLEGAL, "CNTP_CTL: not from User mode");
    uam_machine_reset(&m, CODE);
    sysw(&m, CNTKCTL, 1u | 1u << 9);
    to_mode(&m, ARM_MODE_USR);
    CHECK(m.cpu.isa->sysreg_read(&m.cpu, CNTPCT, &v) == UAM_OK, "CNTKCTL.PL0PCTEN");
    CHECK(m.cpu.isa->sysreg_read(&m.cpu, CNTVCT, &v) == UAM_ERR_ILLEGAL, "but not CNTVCT");
    CHECK(m.cpu.isa->sysreg_write(&m.cpu, CNTP_CTL, 0) == UAM_OK, "CNTKCTL.PL0PTEN");
    CHECK(m.cpu.isa->sysreg_write(&m.cpu, CNTV_CTL, 0) == UAM_ERR_ILLEGAL, "but not the virtual timer");
    uam_machine_fini(&m);
}

/* ---- translation -------------------------------------------------------------------- */

#define L1A 0x10000u   /* TTBR0's table (TTBCR.N = 1: 8 KiB) */
#define L1B 0x14000u   /* TTBR1's */
#define L2  0x18000u

static bool xl(uam_machine *m, uint32_t va, uam_xlate_kind k, uint64_t *pa, uint32_t *fault)
{
    uam_xlate x = { 0 };
    bool ok = m->cpu.isa->translate(&m->cpu, va, k, &x) == UAM_OK;
    *pa = x.paddr;
    *fault = x.fault;
    return ok;
}

static void test_mmu(const uam_isa *isa, bool jit)
{
    uam_machine m;
    machine(&m, isa, jit);
    uint64_t pa;
    uint32_t f;
    CHECK(xl(&m, 0x12345678u, UAM_XLATE_LOAD, &pa, &f) && pa == 0x12345678u, "MMU off: flat");
    /* tables in the hart's byte order (SCTLR.EE) */
    data32(&m, L1A + 0 * 4, 0x00000000u | 3u << 10 | 2u);                 /* identity section */
    data32(&m, L1A + 1 * 4, 0x05000000u | 1u << 18 | 3u << 10 | 2u);      /* supersection */
    data32(&m, L1A + 2 * 4, L2 | 2u << 5 | 1u);                           /* page table, domain 2 */
    data32(&m, L1A + 3 * 4, NOWHERE | 1u);                                /* a table in nothing */
    data32(&m, L2 + 0x0A * 4, 0x06010000u | 3u << 4 | 1u);                /* large page */
    data32(&m, L2 + 0x0B * 4, 0x07000000u | 3u << 4 | 2u | 1u);           /* small page, XN */
    data32(&m, L1B + 0x800 * 4, 0x08000000u | 1u << 10 | 3u << 5 | 2u);   /* priv only, domain 3 */
    sysw(&m, TTBR0, L1A);
    sysw(&m, TTBR1, L1B);
    sysw(&m, TTBCR, 1);
    sysw(&m, DACR, 1u | 1u << 4 | 1u << 6);                               /* 0, 2, 3 clients */
    sysw(&m, SCTLR, sysr(&m, SCTLR) | SCTLR_M);
    CHECK(xl(&m, 0x00101234u, UAM_XLATE_LOAD, &pa, &f) && pa == 0x05101234u, "supersection 0x%llx",
          (unsigned long long)pa);
    CHECK(xl(&m, 0x0020ABCDu, UAM_XLATE_STORE, &pa, &f) && pa == 0x0601ABCDu, "large page 0x%llx",
          (unsigned long long)pa);
    CHECK(xl(&m, 0x0020B010u, UAM_XLATE_LOAD, &pa, &f) && pa == 0x07000010u, "small page");
    CHECK(!xl(&m, 0x0020B010u, UAM_XLATE_FETCH, &pa, &f) && f == ARM_XF(ARM_FS_PERM_PAGE, 2), "XN page fetch 0x%x", f);
    CHECK(!xl(&m, 0x0020C000u, UAM_XLATE_LOAD, &pa, &f) && f == ARM_XF(ARM_FS_XLATE_PAGE, 2), "no page");
    CHECK(!xl(&m, 0x00300000u, UAM_XLATE_LOAD, &pa, &f) && f == ARM_XF(0x0E, 0), "L2 walk in nothing 0x%x", f);
    CHECK(xl(&m, 0x80000ABCu, UAM_XLATE_LOAD, &pa, &f) && pa == 0x08000ABCu, "TTBR1 above the split");
    sysw(&m, ATS1CUR, 0x80000ABCu);
    EQ64(sysr(&m, PAR), 1u | ARM_FS_PERM_SEC << 1, "ATS1CUR: User has no access, PAR fault");
    sysw(&m, DACR, 1u | 1u << 4 | 3u << 6);
    sysw(&m, ATS1CUR, 0x80000ABCu);
    EQ64(sysr(&m, PAR), 0x08000000u, "a manager domain is not checked");
    sysw(&m, TTBCR, 1u | 1u << 5);
    CHECK(!xl(&m, 0x80000ABCu, UAM_XLATE_LOAD, &pa, &f) && f == ARM_XF(ARM_FS_XLATE_SEC, 0), "TTBCR.PD1");
    sysw(&m, TTBR0, NOWHERE);
    CHECK(!xl(&m, 0x00001000u, UAM_XLATE_LOAD, &pa, &f) && f == ARM_XF(0x0C, 0), "L1 walk in nothing");
    sysw(&m, TTBR0, L1A);
    /* a fetch through a section that is not there: prefetch abort, IFSR, IFAR */
    a32(&m, CODE, A_BX_R2);
    setr(&m, 2, 0x00400010u);
    run(&m, CODE, 8);
    expect_entry(&m, ARM_MODE_ABT, 0x0C, 0x00400014u, SPSR0 | cpsr_e(&m), CPSR_A | CPSR_I | cpsr_e(&m),
                 "fetch translation fault");
    EQ64(sysr(&m, IFSR), ARM_FS_XLATE_SEC, "IFSR translation fault");
    EQ64(sysr(&m, IFAR), 0x00400010u, "IFAR");
    uam_machine_fini(&m);
}

/* ---- byte order ---------------------------------------------------------------------- */

static void test_endian(const uam_isa *isa, bool jit)
{
    uam_machine m;
    machine(&m, isa, jit);
    const bool be = is_be(&m);
    const char *name = be ? "BE8" : "LE";
    uint32_t a = a32(&m, CODE, A_STR3);
    a = a32(&m, a, A_LDRB4);
    a = a32(&m, a, A_LDRH5);
    a = a32(&m, a, A_LDR6);
    a32(&m, a, A_SPIN);
    setr(&m, 2, DATA);
    setr(&m, 3, 0x11223344u);
    run(&m, CODE, 20);
    CHECK(m.cpu.pc == a, "%s: ran to the end (pc 0x%llx)", name, (unsigned long long)m.cpu.pc);
    EQ64(mem8(&m, DATA), be ? 0x11 : 0x44, be ? "BE8 str: big-endian data" : "LE str");
    EQ64(r(&m, 4), be ? 0x11 : 0x44, "ldrb");
    EQ64(r(&m, 5), be ? 0x1122 : 0x3344, "ldrh");
    EQ64(r(&m, 6), 0x11223344u, "ldr");
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    m.cpu.isa->dump_state(&m.cpu, fp);
    fclose(fp);
    CHECK(buf && strstr(buf, be ? "data=big" : "data=little"), "%s: the dump says the byte order", name);
    free(buf);
    uam_machine_fini(&m);
}

/* ---- the debugger view --------------------------------------------------------------- */

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, &uam_isa_armeb, false);
    const uam_gdb_arch *g = m.cpu.isa->gdb;
    CHECK(g && g->n_regs == 17 && g->reg_bytes == 4 && g->pc_state_bits == 1, "17 registers, Thumb in pc bit 0");
    CHECK(strstr(g->target_xml, "<architecture>arm</architecture>") && strstr(g->target_xml, "org.gnu.gdb.arm.core"),
          "target description");
    uint64_t v = 0;
    setr(&m, 3, 0x3333);
    setr(&m, 13, 0x5555);
    m.cpu.pc = 0x2001;
    CHECK(g->read_reg(&m.cpu, 3, &v) && v == 0x3333, "gdb r3");
    CHECK(g->read_reg(&m.cpu, 15, &v) && v == 0x2000, "gdb pc without the Thumb bit");
    CHECK(g->read_reg(&m.cpu, 16, &v) && v == (SPSR0 | CPSR_T | CPSR_E), "gdb cpsr 0x%llx", (unsigned long long)v);
    CHECK(!g->read_reg(&m.cpu, 17, &v), "no register 17");
    CHECK(g->write_reg(&m.cpu, 15, 0x3000) && m.cpu.pc == 0x3001, "gdb pc keeps the state");
    /* cpsr: IRQ mode in ARM state, Z and V, an IT state */
    const uint32_t c = CPSR_Z | CPSR_V | CPSR_I | CPSR_E | ARM_MODE_IRQ | 0x04u << 10 | 1u << 25;
    setr(&m, 13, 0x5555);
    CHECK(g->write_reg(&m.cpu, 16, c), "gdb write cpsr");
    EQ64(m.cpu.pc, 0x3000, "T clear: ARM state");
    EQ64(mode(&m), ARM_MODE_IRQ, "the mode");
    CHECK(r(&m, 13) != 0x5555, "the IRQ bank is in");
    CHECK(m.cpu.regfile[ARM_Z_OFF] == 1 && m.cpu.regfile[ARM_V_OFF] == 1 && m.cpu.regfile[ARM_N_OFF] == 0,
          "the flags");
    EQ64(ARM_PRIV_IT(m.cpu.priv), 0x11, "the IT state");
    CHECK(g->read_reg(&m.cpu, 16, &v) && v == c, "gdb cpsr round trip 0x%llx", (unsigned long long)v);
    CHECK(g->write_reg(&m.cpu, 16, CPSR_T | ARM_MODE_SYS | CPSR_E) && m.cpu.pc == 0x3001 && r(&m, 13) == 0x5555,
          "back to System, Thumb");
    CHECK(!g->write_reg(&m.cpu, 17, 0), "no register 17 to write");
    char *buf = NULL;
    size_t len = 0;
    to_mode(&m, ARM_MODE_IRQ | CPSR_E);
    FILE *fp = open_memstream(&buf, &len);
    m.cpu.isa->dump_state(&m.cpu, fp);
    fclose(fp);
    CHECK(buf && strstr(buf, "pc=00003000") && strstr(buf, "state=thumb mode=irq data=big"), "dump: %s", buf);
    free(buf);
    uam_machine_fini(&m);
}

/* ---- snapshots ----------------------------------------------------------------------- */

static void test_snapshot(void)
{
    uam_machine m;
    machine(&m, &uam_isa_arm, false);
    CHECK(uam_arm_set_timer_sink(&m.cpu, sink, NULL) == UAM_OK, "sink");
    to_mode(&m, ARM_MODE_FIQ | CPSR_I | CPSR_F);
    setr(&m, 8, 0xF8);
    setr(&m, 13, 0xF13);
    sysw(&m, SPSR_W, 0x600001D0u);
    to_mode(&m, ARM_MODE_ABT | CPSR_I);
    sysw(&m, SPSR_W, 0x80000017u);
    to_mode(&m, ARM_MODE_IRQ | CPSR_A | CPSR_I);
    setr(&m, 8, 0x88);
    setr(&m, 14, 0x1E14);
    sysw(&m, SCTLR, sysr(&m, SCTLR) | SCTLR_V | SCTLR_TE);
    sysw(&m, VBAR, 0x4000);
    sysw(&m, TTBR0, 0x10000);
    sysw(&m, DACR, 0x55);
    sysw(&m, DFSR, 0x808);
    sysw(&m, DFAR, 0xDFA);
    sysw(&m, IFSR, 0x5);
    sysw(&m, IFAR, 0x1FA);
    sysw(&m, TPIDRPRW, 0x7EAD);
    sysw(&m, CNTKCTL, 3);
    sysw(&m, CNTVOFF, 77);
    sysw(&m, CNTP_CVAL, sysr(&m, CNTPCT) + 500);
    sysw(&m, CNTP_CTL, 1);
    line(&m, UAM_ARM_FIQ, 1);
    m.cpu.pc = 0x2001;
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");

    /* scramble */
    line(&m, UAM_ARM_FIQ, 0);
    uam_machine_reset(&m, 0);
    sysw(&m, CNTP_CVAL, 5000);
    sysw(&m, CNTP_CTL, 1);
    setr(&m, 8, 0);
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    EQ64(m.cpu.pc, 0x2001, "pc and Thumb state");
    EQ64(sysr(&m, ARM_SR_CPSR), ARM_MODE_IRQ | CPSR_A | CPSR_I, "CPSR");
    EQ64(r(&m, 8), 0x88, "r8");
    EQ64(r(&m, 14), 0x1E14, "IRQ lr");
    EQ64(sysr(&m, SCTLR), 0x00C50078u | SCTLR_V | SCTLR_TE, "SCTLR");
    EQ64(sysr(&m, VBAR), 0x4000, "VBAR");
    EQ64(sysr(&m, TTBR0), 0x10000, "TTBR0");
    EQ64(sysr(&m, DACR), 0x55, "DACR");
    CHECK(sysr(&m, DFSR) == 0x808 && sysr(&m, DFAR) == 0xDFA && sysr(&m, IFSR) == 5 && sysr(&m, IFAR) == 0x1FA,
          "fault status and address registers");
    EQ64(sysr(&m, TPIDRPRW), 0x7EAD, "TPIDRPRW");
    EQ64(sysr(&m, CNTKCTL), 3, "CNTKCTL");
    EQ64(sysr(&m, CNTVOFF), 77, "CNTVOFF");
    EQ64(sysr(&m, ARM_SR_BANKSP | ARM_MODE_FIQ), 0xF13, "the FIQ sp in its bank");
    to_mode(&m, ARM_MODE_ABT | CPSR_A | CPSR_I);
    EQ64(sysr(&m, ARM_SR_SPSR), 0x80000017u, "SPSR_abt");
    to_mode(&m, ARM_MODE_SYS | CPSR_I);
    CHECK(wake(&m) && take(&m), "the FIQ line came back");
    expect_entry(&m, ARM_MODE_FIQ, HIVEC + 0x1C + 1, 0x2004, SPSR0 | CPSR_I | CPSR_T,
                 CPSR_A | CPSR_I | CPSR_F, "FIQ after restore: high vectors, Thumb");
    EQ64(r(&m, 8), 0xF8, "FIQ r8 in its bank");
    sink_calls = 0;
    const uint64_t left = sysr(&m, CNTP_CVAL) - sysr(&m, CNTPCT);
    tick(&m, left - 1);
    CHECK(sink_calls == 0, "the restored timer not before its deadline");
    tick(&m, 1);
    CHECK(sink_calls == 1 && sink_level[UAM_ARM_TIMER_PHYS] == 1, "the timer fires at the restored deadline");

    /* a snapshot of another CPU model does not restore (the core logs the mismatch) */
    uam_machine m2;
    uam_arm_config other = uam_arm_ref;
    other.midr = 0x410FC090u;
    CHECK(uam_machine_init(&m2, &uam_isa_arm) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m2.bus, "ram", 0, RAM_SIZE) == UAM_OK && uam_bus_map_ram(&m2.bus, "hivec", HIVEC, 0x10000) == UAM_OK,
          "map");
    uam_machine_reset(&m2, 0);
    CHECK(uam_snapshot_restore(&m2, s) == UAM_OK, "restores into a machine of the same model");
    CHECK(uam_arm_configure(&m2.cpu, &other) == UAM_OK, "another model");
    uam_machine_reset(&m2, 0);
    CHECK(uam_snapshot_restore(&m2, s) != UAM_OK, "a different MIDR is refused");
    uam_machine_fini(&m2);
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

/* ---- arm-ref's GIC ------------------------------------------------------------------- */

#define GICD 0x1c010000u
#define GICC 0x1c020000u

static void gw(uam_machine *m, uint32_t a, uint32_t v) { uam_bus_write(&m->bus, a, 4, UAM_ACC_LE, v); }
static uint32_t gr(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, 4, UAM_ACC_LE, &v);
    return (uint32_t)v;
}

static void test_gic(void)
{
    uam_machine m;
    uam_board_config cfg = { 0 };
    CHECK(uam_board_arm_ref.build(&m, &cfg) == UAM_OK, "build arm-ref");
    uam_device *gic = uam_machine_find_device(&m, "gic");
    CHECK(gic != NULL, "the GIC");
    if (!gic)
        return;
    EQ64(gr(&m, GICD + 0x004), 2, "TYPER: 96 ids, one CPU");
    gw(&m, GICD + 0x000, 1);
    gw(&m, GICD + 0x104, 1u << 8 | 1u << 9);           /* SPIs 40, 41 */
    gw(&m, GICD + 0x428, 0x00004080u);                  /* 40: 0x80, 41: 0x40 */
    gw(&m, GICC + 0x004, 0xF0);
    gw(&m, GICC + 0x000, 1);
    uam_gic_input(gic, 40, 1);
    CHECK(wake(&m), "SPI 40 raises the CPU's IRQ");
    EQ64(sysr(&m, ISR), 0x80, "ISR.I");
    uam_gic_input(gic, 41, 1);
    EQ64(gr(&m, GICC + 0x018), 41, "HPPIR: the higher priority");
    EQ64(gr(&m, GICC + 0x00C), 41, "IAR 41 first");
    EQ64(gr(&m, GICC + 0x014), 0x40, "running priority 0x40");
    CHECK(!wake(&m), "40 does not preempt 41");
    uam_gic_input(gic, 41, 0);
    gw(&m, GICC + 0x010, 41);
    CHECK(wake(&m), "after EOI, 40 is signalled");
    EQ64(gr(&m, GICC + 0x00C), 40, "IAR 40");
    gw(&m, GICC + 0x010, 40);
    CHECK(wake(&m), "a level input still high is pending again after EOI");
    gw(&m, GICC + 0x004, 0x80);
    CHECK(!wake(&m), "PMR 0x80 masks priority 0x80");
    EQ64(gr(&m, GICC + 0x00C), 1023, "IAR: spurious");
    uam_gic_input(gic, 40, 0);
    gw(&m, GICC + 0x004, 0xF0);
    CHECK(!wake(&m), "input lowered");

    /* the physical timer reaches PPI 30, the virtual timer PPI 27 */
    gw(&m, GICD + 0x100, 1u << 30 | 1u << 27);
    gw(&m, GICD + 0x41C, 0x00A00000u);                  /* 30: 0xA0 */
    gw(&m, GICD + 0x418, 0x90000000u);                  /* 27: 0x90 */
    sysw(&m, CNTP_CVAL, sysr(&m, CNTPCT) + 10);
    sysw(&m, CNTP_CTL, 1);
    tick(&m, 9);
    CHECK(!wake(&m), "timer: not yet");
    tick(&m, 1);
    CHECK(wake(&m), "the physical timer interrupts through the GIC");
    EQ64(gr(&m, GICC + 0x00C), 30, "IAR: PPI 30");
    sysw(&m, CNTP_CTL, 0);
    gw(&m, GICC + 0x010, 30);
    CHECK(!wake(&m), "timer stopped, EOI: quiet");
    sysw(&m, CNTV_TVAL, 5);
    sysw(&m, CNTV_CTL, 1);
    tick(&m, 5);
    EQ64(gr(&m, GICC + 0x00C), 27, "IAR: PPI 27, the virtual timer");
    uam_machine_fini(&m);
}

int main(void)
{
    test_reset();
    test_banks();
    test_irq_lines();
    test_timer();
    test_gdb();
    test_snapshot();
    test_gic();
    for (int jit = 0; jit < 2; jit++) {
        test_exceptions(jit);
        test_irq_return(jit);
        test_wfi(jit);
        test_mmu(&uam_isa_arm, jit);
        test_mmu(&uam_isa_armeb, jit);
        test_endian(&uam_isa_arm, jit);
        test_endian(&uam_isa_armeb, jit);
    }
    printf("test_arm: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
