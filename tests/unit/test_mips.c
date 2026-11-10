/*
 * test_mips.c - The MIPS32 module's parts the self-test firmware cannot reach on mips-ref
 * and pic32-ref: model selection and its validation, the reset state of each model and
 * byte order, the CP0 registers' writable bits, the vector each exception enters (BEV,
 * EBase, the TLB refill vector with EXL clear and set), EPC kept while EXL is set, bus
 * errors against address errors, faults in branch delay slots other than a TLB miss,
 * kuseg unmapped while ERL is set, CP0 in user mode, the interrupt inputs through the
 * board's API in compatibility, vectored and external-controller modes (masking, the
 * vector, the shadow set, the lines reported), WAIT, Count and Compare in virtual time
 * and as a controller input, the TLB's ASIDs, page sizes, Wired and Random, fixed mapping,
 * the ISA mode in pc bit 0 (JALX, ERET into MIPS16e, ISAOnExc, a microMIPS-only core),
 * an instruction of a feature the model lacks, the debugger view, the dump, a snapshot
 * of the ISA state, and one program run big- and little-endian.
 *
 * Each test runs on a bare machine: RAM at physical 0 (kseg0 0x80000000) and at the boot
 * vectors (0x1fc00000), every exception and interrupt vector a branch to itself, so an
 * exception leaves the hart spinning where it was delivered. The tests that execute code
 * run interpreted and under the JIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../isa/mips32/mips.h"
#include "uam/bus.h"
#include "uam/isa/mips32.h"
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

#define CODE     0x80001000u
#define DATA     0x80008000u
#define SCRATCH  0x80000F00u   /* the TLB-instruction and ERET programs */
#define EBASE2   0x80010000u   /* a moved exception base */
#define BEV_BASE 0xBFC00200u   /* the boot exception vectors */
#define GENERAL  0x80000180u
#define NOWHERE  0xB0000000u   /* kseg1, physical 0x10000000: nothing there */

enum { V0 = 2, T0 = 8, T1 = 9, T2 = 10, T3 = 11, RA = 31 };
#define LO_G 0x1u
#define LO_V 0x2u
#define LO_D 0x4u
#define PFN(pa) (((uint32_t)(pa) >> 12) << 6)

/* ---- instruction encodings (MIPS32) ---------------------------------------------- */

/* Macros: the case tables are static. */
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFF))
#define ADDIU(rt, rs, imm) I(0x09, rs, rt, imm)
#define ORI(rt, rs, imm)   I(0x0D, rs, rt, imm)
#define LUI(rt, imm)       I(0x0F, 0, rt, imm)
#define LW(rt, off, base)  I(0x23, base, rt, off)
#define LBU(rt, off, base) I(0x24, base, rt, off)
#define LHU(rt, off, base) I(0x25, base, rt, off)
#define SW(rt, off, base)  I(0x2B, base, rt, off)
#define BEQ(rs, rt, off)   I(0x04, rs, rt, off)   /* off: instructions from the slot */
#define BNE(rs, rt, off)   I(0x05, rs, rt, off)
#define ADD(rd, rs, rt)    ((uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | 0x20u)
#define JR(rs)             ((uint32_t)(rs) << 21 | 0x08u)
#define JAL(t)             (0x0C000000u | (((uint32_t)(t) >> 2) & 0x03FFFFFFu))
#define JALX(t)            (0x74000000u | (((uint32_t)(t) >> 2) & 0x03FFFFFFu))
#define MFC0(rt, rd, sel)  (0x40000000u | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (sel))
#define MTC0(rt, rd, sel)  (0x40800000u | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (sel))
#define DI(rt)             (0x41606000u | (uint32_t)(rt) << 16)
#define NOP     0x00000000u
#define SPIN    0x1000FFFFu   /* b . */
#define SYSCALL 0x0000000Cu
#define BREAK   0x0000000Du
#define ERET    0x42000018u
#define WAIT    0x42000020u
#define TLBR    0x42000001u
#define TLBWI   0x42000002u
#define TLBWR   0x42000006u
#define TLBP    0x42000008u
#define IRET    0x42000038u   /* MCU ASE */
#define ADDU_QB 0x7C641010u   /* addu.qb v0, v1, a0 (DSP ASE) */
#define MFC1    0x44020000u   /* mfc1 v0, $f0 */
#define LWC2    0xC8620000u   /* lwc2 $2, 0(v1) */
/* MIPS16e and microMIPS halfwords */
#define M16_LI_V0_5 0x6A05u
#define M16_BREAK   0xE805u
#define M16_B_SELF  0x17FFu
#define UM_B16_SELF 0xCFFFu
#define UM_NOP16    0x0C00u

/* ---- the machine ------------------------------------------------------------------ */

static bool is_be(uam_machine *m) { return m->cpu.isa == &uam_isa_mips32be; }
static mips_state *st(uam_machine *m) { return mips_st(&m->cpu); }

static void load(uam_machine *m, uint32_t va, const uint8_t *b, size_t n)
{
    CHECK(uam_bus_load_bytes(&m->bus, va & 0x1FFFFFFFu, b, n) == UAM_OK, "load at 0x%x", (unsigned)va);
}
/* A word or halfword at kseg0/kseg1 address a, in the hart's byte order; returns the next. */
static uint32_t put(uam_machine *m, uint32_t a, uint32_t w)
{
    uint8_t b[4];
    for (unsigned i = 0; i < 4; i++)
        b[i] = (uint8_t)(w >> (is_be(m) ? 24 - 8 * i : 8 * i));
    load(m, a, b, 4);
    return a + 4;
}
static uint32_t put16(uam_machine *m, uint32_t a, uint16_t h)
{
    uint8_t b[2] = { (uint8_t)(is_be(m) ? h >> 8 : h), (uint8_t)(is_be(m) ? h : h >> 8) };
    load(m, a, b, 2);
    return a + 2;
}
static void prog(uam_machine *m, uint32_t a, const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        a = put(m, a, w[i]);
}
#define PROG(m, a, ...) do { const uint32_t w_[] = { __VA_ARGS__ }; prog((m), (a), w_, UAM_ARRAY_LEN(w_)); } while (0)

/* Exception handlers run in microMIPS (Config3.ISAOnExc). */
static bool umips_handlers(uam_machine *m)
{
    return mips_has_umips(st(m)) && (st(m)->c.config3 & CFG3_ISAONEXC);
}
static uint32_t vec(uam_machine *m, uint32_t a) { return a | (umips_handlers(m) ? 1u : 0u); }
/* A branch to itself at offset off of buf, in the ISA the handlers run in. */
static void spin_at(uam_machine *m, uint8_t *buf, uint32_t off)
{
    const bool um = umips_handlers(m);
    const uint32_t w = um ? (uint32_t)UM_B16_SELF << 16 | UM_NOP16 : SPIN;
    for (unsigned i = 0; i < 4; i++) {
        /* microMIPS little-endian: each halfword little-endian, the first one first */
        const unsigned sh = is_be(m) ? 24 - 8 * i : um ? (i ^ 2) * 8 : 8 * i;
        buf[off + i] = (uint8_t)(w >> sh);
    }
    memset(buf + off + 4, 0, 4);   /* MIPS32: the delay slot's nop */
}
static void spin(uam_machine *m, uint32_t a)
{
    uint8_t b[8];
    spin_at(m, b, 0);
    load(m, a, b, sizeof(b));
}
static uint32_t mem32(uam_machine *m, uint32_t va)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, va & 0x1FFFFFFFu, 4, is_be(m) ? UAM_ACC_BE : UAM_ACC_LE, &v);
    return (uint32_t)v;
}
static uint8_t mem8(uam_machine *m, uint32_t va)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, va & 0x1FFFFFFFu, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}
static uint32_t gpr(uam_machine *m, unsigned r) { return (uint32_t)uam_cpu_reg_read(&m->cpu, MIPS_REG_OFF(r), 32); }
static void setr(uam_machine *m, unsigned r, uint32_t v) { uam_cpu_reg_write(&m->cpu, MIPS_REG_OFF(r), 32, v); }
static void sysw(uam_machine *m, uint32_t id, uint32_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "write CP0 %u.%u", (unsigned)id >> 3, (unsigned)id & 7);
}
static uint32_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, id, &v) == UAM_OK, "read CP0 %u.%u", (unsigned)id >> 3, (unsigned)id & 7);
    return (uint32_t)v;
}
static unsigned exccode(uam_machine *m) { return (sysr(m, CP0_CAUSE) & CAUSE_EXC_MASK) >> CAUSE_EXC_SHIFT; }
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static uint64_t lines_taken(uam_machine *m) { return m->cpu.isa->irq_lines(&m->cpu); }
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

/* Model cfg (NULL: the default), RAM mapped, reset at CODE; nothing else. */
static void machine_bare(uam_machine *m, const uam_isa *isa, const uam_mips_config *cfg, bool jit)
{
    CHECK(uam_machine_init(m, isa) == UAM_OK, "machine init");
    if (cfg)
        CHECK(uam_mips_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 0x100000) == UAM_OK, "map RAM");
    CHECK(uam_bus_map_ram(&m->bus, "boot", 0x1FC00000u, 0x10000) == UAM_OK, "map boot RAM");
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    uam_machine_reset(m, CODE);
}

/* And every vector spinning (both exception bases and the boot vectors), Status 0:
 * kernel mode, BEV, ERL, EXL and interrupts off. */
static void machine(uam_machine *m, const uam_isa *isa, const uam_mips_config *cfg, bool jit)
{
    machine_bare(m, isa, cfg, jit);
    static uint8_t v[0xA00];
    memset(v, 0, sizeof(v));
    spin_at(m, v, 0);
    spin_at(m, v, 0x180);
    for (uint32_t k = 0; k < 64; k++)
        spin_at(m, v, 0x200 + 32 * k);
    load(m, 0x80000000u, v, sizeof(v));
    load(m, EBASE2, v, sizeof(v));
    memset(v, 0, 0x208);
    spin_at(m, v, 0);
    spin_at(m, v, 0x180);
    spin_at(m, v, 0x200);
    load(m, BEV_BASE, v, 0x208);
    sysw(m, CP0_STATUS, 0);
}

/* The hart took exception `code` at EPC epc (BD as said) and sits at vector. */
static void expect_exc(uam_machine *m, uint32_t vector, unsigned code, uint32_t epc, bool bd,
                       const char *what)
{
    CHECK(m->cpu.pc == vec(m, vector), "%s: at 0x%x (pc 0x%llx)", what, (unsigned)vec(m, vector),
          (unsigned long long)m->cpu.pc);
    CHECK(exccode(m) == code, "%s: ExcCode %u want %u", what, exccode(m), code);
    CHECK(sysr(m, CP0_EPC) == epc, "%s: EPC 0x%x want 0x%x", what, (unsigned)sysr(m, CP0_EPC), (unsigned)epc);
    CHECK(!(sysr(m, CP0_CAUSE) & CAUSE_BD) == !bd, "%s: Cause.BD %s", what, bd ? "set" : "clear");
    CHECK(sysr(m, CP0_STATUS) & ST_EXL, "%s: Status.EXL", what);
}

/* A TLB instruction (TLBWI, TLBWR, TLBR, TLBP), run at SCRATCH. */
static void tlb_op(uam_machine *m, uint32_t insn)
{
    PROG(m, SCRATCH, insn, SPIN, NOP);
    run(m, SCRATCH, 4);
    CHECK(m->cpu.pc == SCRATCH + 4, "TLB instruction 0x%08x ran", (unsigned)insn);
}
static void tlb_set(uam_machine *m, unsigned idx, uint32_t hi, uint32_t lo0, uint32_t lo1, uint32_t mask)
{
    sysw(m, CP0_INDEX, idx);
    sysw(m, CP0_ENTRYHI, hi);
    sysw(m, CP0_ENTRYLO0, lo0);
    sysw(m, CP0_ENTRYLO1, lo1);
    sysw(m, CP0_PAGEMASK, mask);
    tlb_op(m, TLBWI);
}

static char *dump(uam_machine *m)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    m->cpu.isa->dump_state(&m->cpu, f);
    fclose(f);
    return buf;
}

static const uam_mips_config *const models[] = { &uam_mips_ref, &uam_mips_m16_ref, &uam_mips_umips_ref,
                                                 &uam_mips_mz_ref };
enum { REF, M16, UMIPS, MZ };   /* models[] */

/* ---- models and reset -------------------------------------------------------------- */

static void test_models(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_mips32be) == UAM_OK, "init");
    EQ64(sysr(&m, CP0_PRID), uam_mips_ref.prid, "without uam_mips_configure: the reference identity");
    CHECK(uam_mips_configure(&m.cpu, NULL) == UAM_ERR_INVALID, "configure needs a model");
    uam_mips_config b;
#define BAD(base, stmt, what) do {                                                 \
        b = *(base);                                                               \
        stmt;                                                                      \
        CHECK(uam_mips_configure(&m.cpu, &b) == UAM_ERR_INVALID, "refused: %s", what); \
    } while (0)
    BAD(&uam_mips_ref, b.tlb_entries = 0, "a TLB of no entries");
    BAD(&uam_mips_ref, b.tlb_entries = 65, "65 TLB entries");
    BAD(&uam_mips_m16_ref, b.tlb_entries = 16, "TLB entries with fixed mapping");
    BAD(&uam_mips_ref, b.config0 = (b.config0 & ~(7u << 7)) | 2u << 7, "Config.MT 2 (BAT)");
    BAD(&uam_mips_ref, b.shadow_sets = 16, "16 shadow sets");
    BAD(&uam_mips_m16_ref, b.config3 |= 1u << CFG3_ISA_SHIFT, "MIPS16e and microMIPS");
    BAD(&uam_mips_ref, b.config3 |= CFG3_DSP2P, "DSP rev 2 without DSP");
#undef BAD
    EQ64(sysr(&m, CP0_PRID), uam_mips_ref.prid, "the model is unchanged");
    b = uam_mips_ref;
    b.name = "ref-64";
    b.tlb_entries = 64;
    CHECK(uam_mips_configure(&m.cpu, &b) == UAM_OK, "a 64-entry TLB is a model");
    uam_machine_reset(&m, CODE);
    EQ64((sysr(&m, CP0_CONFIG1) >> 25) & 0x3F, 63, "Config1.MMUSize 63");
    CHECK(uam_mips_swint_line(&m.cpu, 2) == NULL, "two software interrupt lines");
    /* each processor's number in EBase.CPUNum */
    CHECK(uam_machine_add_cpus(&m, 1) == UAM_OK, "a second processor");
    uam_machine_reset(&m, CODE);
    uint64_t v = 0;
    uam_cpu *c1 = uam_machine_cpu(&m, 1);
    CHECK(c1->isa->sysreg_read(c1, CP0_EBASE, &v) == UAM_OK && (v & 0x3FF) == 1, "EBase.CPUNum 1");
    EQ64(sysr(&m, CP0_EBASE) & 0x3FF, 0, "EBase.CPUNum 0");
    uam_machine_fini(&m);

    CHECK(uam_machine_init(&m, &uam_isa_z80) == UAM_OK, "a Z80");
    CHECK(uam_mips_configure(&m.cpu, &uam_mips_ref) == UAM_ERR_INVALID, "a MIPS model on a Z80");
    CHECK(!uam_mips_timer_line(&m.cpu) && !uam_mips_swint_line(&m.cpu, 0), "a Z80 has no MIPS lines");
    uam_machine_fini(&m);

    /* the reset state of each model, both byte orders */
    for (size_t k = 0; k < UAM_ARRAY_LEN(models) * 2; k++) {
        const uam_mips_config *cfg = models[k / 2];
        const bool be = k & 1;
        const unsigned isa = (cfg->config3 & CFG3_ISA_MASK) >> CFG3_ISA_SHIFT;
        char what[96];
        snprintf(what, sizeof(what), "%s %s", cfg->name, be ? "BE" : "LE");
        machine_bare(&m, be ? &uam_isa_mips32be : &uam_isa_mips32le, cfg, false);
        CHECK(m.cpu.pc == (CODE | (isa == 1 || isa == 3)), "%s: reset pc 0x%llx", what, (unsigned long long)m.cpu.pc);
        CHECK(sysr(&m, CP0_STATUS) == (ST_BEV | ST_ERL), "%s: Status BEV, ERL", what);
        CHECK(MIPS_PRIV_MODE(m.cpu.priv) == MIPS_KERNEL, "%s: kernel mode", what);
        CHECK(sysr(&m, CP0_CAUSE) == 0, "%s: Cause 0", what);
        CHECK(sysr(&m, CP0_PRID) == cfg->prid, "%s: PRId", what);
        CHECK(sysr(&m, CP0_CONFIG) == ((cfg->config0 & ~(1u << 15)) | (be ? 1u << 15 : 0)), "%s: Config.BE", what);
        const uint32_t mmu = cfg->tlb_entries ? cfg->tlb_entries - 1 : 0;
        CHECK(sysr(&m, CP0_CONFIG1) == ((cfg->config1 & ~(0x3Fu << 25)) | mmu << 25), "%s: Config1", what);
        CHECK(sysr(&m, CP0_CONFIG2) == cfg->config2 && sysr(&m, CP0_CONFIG3) == cfg->config3, "%s: Config2/3", what);
        CHECK(sysr(&m, CP0_EBASE) == 0x80000000u, "%s: EBase 0x80000000", what);
        CHECK(((sysr(&m, CP0_SRSCTL) >> 26) & 0xF) == cfg->shadow_sets, "%s: SRSCtl.HSS", what);
        CHECK((sysr(&m, CP0_SRSCTL) & 0x3C3CFu) == 0, "%s: SRSCtl ESS, PSS, CSS 0", what);
        CHECK(sysr(&m, CP0_WIRED) == 0, "%s: Wired 0", what);
        CHECK(sysr(&m, CP0_RANDOM) <= mmu, "%s: Random within the TLB", what);
        uam_machine_fini(&m);
    }
    /* a microMIPS reset (Config3.ISA 3, as -o bootisa=micromips makes it) */
    b = uam_mips_mz_ref;
    b.config3 = (b.config3 & ~CFG3_ISA_MASK) | 3u << CFG3_ISA_SHIFT;
    machine_bare(&m, &uam_isa_mips32le, &b, false);
    EQ64(m.cpu.pc, CODE | 1, "Config3.ISA 3: reset into microMIPS");
    uam_machine_fini(&m);
}

/* ---- CP0 writable bits --------------------------------------------------------------- */

static void test_cp0_masks(void)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, false);
    sysw(&m, CP0_STATUS, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_STATUS), uam_mips_ref.status_rw, "Status: the model's writable bits");
    sysw(&m, CP0_STATUS, 1u << ST_KSU_SHIFT);
    EQ64(sysr(&m, CP0_STATUS), 0, "KSU 01: no supervisor mode");
    EQ64(MIPS_PRIV_MODE(m.cpu.priv), MIPS_KERNEL, "so kernel mode");
    sysw(&m, CP0_STATUS, ST_CU1 | ST_FR | ST_MX);
    EQ64(sysr(&m, CP0_STATUS), 0, "no CU1, FR or MX without an FPU or the DSP");
    sysw(&m, CP0_CAUSE, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_CAUSE), CAUSE_IV | 3u << CAUSE_IP_SHIFT, "Cause: IV and IP1:0 writable");
    sysw(&m, CP0_CAUSE, 0);
    EQ64(sysr(&m, CP0_CAUSE), 0, "and cleared");
    sysw(&m, CP0_EBASE, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_EBASE), 0xBFFFF000u, "EBase: bits 29:12; 31 reads 1, 30 and CPUNum 0");
    sysw(&m, CP0_EBASE, 0x80000000u);
    EQ64(sysr(&m, CP0_EBASE), 0x80000000u, "EBase rewritten: every base bit cleared again");
    sysw(&m, CP0_EBASE, 0x80010000u);
    sysw(&m, CP0_EBASE, 0x80020000u);
    EQ64(sysr(&m, CP0_EBASE), 0x80020000u, "EBase moved twice: the second base alone");
    sysw(&m, CP0_PRID, 0);
    EQ64(sysr(&m, CP0_PRID), uam_mips_ref.prid, "PRId is read-only");
    sysw(&m, CP0_INDEX, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_INDEX), 15, "Index: 4 bits, P read-only");
    sysw(&m, CP0_ENTRYHI, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_ENTRYHI), 0xFFFFE0FFu, "EntryHi: VPN2 and ASID");
    sysw(&m, CP0_ENTRYLO0, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_ENTRYLO0), 0x03FFFFFFu, "EntryLo0: PFN C D V G");
    sysw(&m, CP0_PAGEMASK, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_PAGEMASK), 0x1FFFE000u, "PageMask: bits 28:13");
    sysw(&m, CP0_CONTEXT, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_CONTEXT), 0xFF800000u, "Context: PTEBase only");
    sysw(&m, CP0_WIRED, 5);
    EQ64(sysr(&m, CP0_WIRED), 5, "Wired");
    CHECK(sysr(&m, CP0_RANDOM) >= 5 && sysr(&m, CP0_RANDOM) <= 15, "Random in [Wired, 15]");
    const uint32_t cfg0 = sysr(&m, CP0_CONFIG);
    sysw(&m, CP0_CONFIG, 0xFFFFFFF8u);
    EQ64(sysr(&m, CP0_CONFIG), cfg0 & ~7u, "Config: only K0 writable");
    sysw(&m, CP0_CONFIG1, 0);
    sysw(&m, CP0_CONFIG3, CFG3_ISAONEXC);
    CHECK(sysr(&m, CP0_CONFIG1) && sysr(&m, CP0_CONFIG3) == 0, "Config1 and Config3 read-only");
    sysw(&m, CP0_SRSCTL, 0xFFFFFFFFu);
    sysw(&m, CP0_SRSMAP, 0xFFFFFFFFu);
    CHECK(sysr(&m, CP0_SRSCTL) == 0 && sysr(&m, CP0_SRSMAP) == 0, "no shadow sets: SRSCtl, SRSMap 0");
    uam_machine_fini(&m);

    machine(&m, &uam_isa_mips32le, &uam_mips_mz_ref, false);
    sysw(&m, CP0_STATUS, ST_CU1 | ST_FR | ST_MX);
    EQ64(sysr(&m, CP0_STATUS), ST_CU1 | ST_FR | ST_MX, "mz: CU1, FR, MX writable");
    EQ64(m.cpu.priv, MIPS_PRIV_CU1 | MIPS_PRIV_FR | MIPS_PRIV_MX, "and in the translation tag");
    sysw(&m, CP0_CONFIG3, CFG3_ISAONEXC);
    EQ64(sysr(&m, CP0_CONFIG3), uam_mips_mz_ref.config3 | CFG3_ISAONEXC, "mz: ISAOnExc writable");
    sysw(&m, CP0_SRSCTL, 7u << 12 | 3u << 6 | 5u);
    EQ64(sysr(&m, CP0_SRSCTL), 7u << 26 | 7u << 12 | 3u << 6, "mz: SRSCtl ESS, PSS writable, CSS not");
    sysw(&m, CP0_SRSCTL, 8u << 12);
    EQ64((sysr(&m, CP0_SRSCTL) >> 12) & 0xF, 7, "mz: ESS beyond HSS ignored");
    sysw(&m, CP0_SRSMAP, 0x76543210u);
    EQ64(sysr(&m, CP0_SRSMAP), 0x76543210u, "mz: SRSMap");
    sysw(&m, CP0_INTCTL, 0xFFFFFFFFu);
    EQ64(sysr(&m, CP0_INTCTL), 0x3E0, "mz: IntCtl.VS (no IPTI with a controller)");
    uam_machine_fini(&m);

    machine(&m, &uam_isa_mips32le, &uam_mips_umips_ref, false);
    sysw(&m, CP0_CONFIG3, 0);
    CHECK(sysr(&m, CP0_CONFIG3) & CFG3_ISAONEXC, "microMIPS only: ISAOnExc fixed");
    sysw(&m, CP0_ENTRYHI, 0xFFFFE0FFu);
    sysw(&m, CP0_INDEX, 3);
    CHECK(sysr(&m, CP0_ENTRYHI) == 0 && sysr(&m, CP0_INDEX) == 0, "fixed mapping: no TLB registers");
    uam_machine_fini(&m);
}

/* ---- exception entry --------------------------------------------------------------- */

static void test_vectors(bool jit)
{
    static const struct {
        uint32_t status, ebase, insn, t0, vector; unsigned code; uint32_t epc, bad; const char *what;
    } cases[] = {
        { ST_BEV, 0,      SYSCALL,      0,          BEV_BASE + 0x180, EXC_SYS,  CODE,   0,      "SYSCALL with BEV: 0xbfc00380" },
        { 0,      EBASE2, SYSCALL,      0,          EBASE2 + 0x180,   EXC_SYS,  CODE,   0,      "SYSCALL: EBase + 0x180" },
        { 0,      0,      LW(T1,0,T0),  0x4000,     0x80000000u,      EXC_TLBL, CODE,   0x4000, "TLB refill: EBase + 0" },
        { 0,      EBASE2, SW(T1,0,T0),  0x4000,     EBASE2,           EXC_TLBS, CODE,   0x4000, "store refill at a moved EBase" },
        { ST_BEV, 0,      LW(T1,0,T0),  0x4000,     BEV_BASE,         EXC_TLBL, CODE,   0x4000, "TLB refill with BEV: 0xbfc00200" },
        { ST_EXL, 0,      LW(T1,0,T0),  0x4000,     GENERAL,          EXC_TLBL, 0x1234, 0x4000, "TLB refill with EXL: EBase + 0x180, EPC kept" },
        { ST_EXL, 0,      BREAK,        0,          GENERAL,          EXC_BP,   0x1234, 0,      "BREAK with EXL: EPC kept" },
        { 0,      0,      LW(T1,0,T0),  0x1000,     GENERAL,          EXC_TLBL, CODE,   0x1000, "the reset TLB: entry 0 matches, invalid" },
        { 0,      0,      LW(T1,0,T0),  NOWHERE,    GENERAL,          EXC_DBE,  CODE,   0,      "a load from nothing: bus error" },
        { 0,      0,      SW(T1,0,T0),  NOWHERE,    GENERAL,          EXC_DBE,  CODE,   0,      "a store to nothing: bus error" },
        { 0,      0,      JR(T0),       NOWHERE,    GENERAL,          EXC_IBE,  NOWHERE, 0,     "a fetch from nothing: bus error" },
        { 0,      0,      JR(T0),       CODE + 0x42, GENERAL,         EXC_ADEL, CODE + 0x42, CODE + 0x42, "a fetch from a halfword" },
        { 0,      0,      JR(T0),       CODE + 0x41, GENERAL,         EXC_ADEL, CODE + 0x41, CODE + 0x41, "a fetch from an odd address" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        uam_machine m;
        machine(&m, &uam_isa_mips32be, NULL, jit);
        PROG(&m, CODE, cases[i].insn, NOP, SPIN, NOP);
        if (cases[i].ebase)
            sysw(&m, CP0_EBASE, cases[i].ebase);
        sysw(&m, CP0_EPC, 0x1234);
        sysw(&m, CP0_STATUS, cases[i].status);
        setr(&m, T0, cases[i].t0);
        run(&m, CODE, 8);
        expect_exc(&m, cases[i].vector, cases[i].code, cases[i].epc, false, cases[i].what);
        CHECK(sysr(&m, CP0_BADVADDR) == cases[i].bad, "%s: BadVAddr 0x%x", cases[i].what,
              (unsigned)sysr(&m, CP0_BADVADDR));
        uam_machine_fini(&m);
    }
}

/* Faults in a branch delay slot: EPC is the branch, Cause.BD set, taken or not. */
static void test_delay_slots(bool jit)
{
    static const struct { uint32_t br, slot; unsigned code; const char *what; } cases[] = {
        { BEQ(0, 0, 3), LW(T1, 0, T0),  EXC_DBE, "taken BEQ, a load from nothing in its slot" },
        { BNE(0, 0, 3), LW(T1, 0, T0),  EXC_DBE, "BNE not taken, a load from nothing in its slot" },
        { JR(T2),       SW(T1, 0, T0),  EXC_DBE, "JR, a store to nothing in its slot" },
        { BEQ(0, 0, 3), ADD(T1, T3, T3), EXC_OV, "an overflow in a delay slot" },
        { BEQ(0, 0, 3), LW(T1, 2, T2),  EXC_ADEL, "a misaligned load in a delay slot" },
        { BEQ(0, 0, 3), SYSCALL,        EXC_SYS,  "SYSCALL in a delay slot" },
        { BNE(0, 0, 3), BREAK,          EXC_BP,   "BREAK in the slot of a branch not taken" },
        { BEQ(0, 0, 3), ERET,           EXC_RI,   "ERET in a delay slot (UNPREDICTABLE): RI" },
        { JR(T2),       WAIT,           EXC_RI,   "WAIT in a delay slot (UNPREDICTABLE): RI" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        uam_machine m;
        machine(&m, &uam_isa_mips32be, NULL, jit);
        PROG(&m, CODE, cases[i].br, cases[i].slot, SPIN, NOP, SPIN, NOP);
        setr(&m, T0, NOWHERE);
        setr(&m, T2, CODE + 16);
        setr(&m, T3, 0x7FFFFFFFu);
        run(&m, CODE, 8);
        expect_exc(&m, GENERAL, cases[i].code, CODE, true, cases[i].what);
        uam_machine_fini(&m);
    }
}

/* COP0 instructions in a delay slot run, and the branch still goes where it goes. */
static void test_cop0_in_slot(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, jit);
    /* MFC0 Status in the slot of a taken BEQ, MTC0 EPC in the slot of JR, DI in the slot
     * of a BNE not taken */
    PROG(&m, CODE, BEQ(0, 0, 3), MFC0(T1, 12, 0), SPIN, NOP, SPIN, NOP);
    sysw(&m, CP0_STATUS, 0x0000FF00u);
    run(&m, CODE, 8);
    EQ64(m.cpu.pc, CODE + 16, "MFC0 in a delay slot: the branch taken");
    EQ64(gpr(&m, T1), 0x0000FF00u, "and the slot read Status");
    PROG(&m, CODE + 0x40, JR(T2), MTC0(T3, 14, 0), SPIN, NOP);
    setr(&m, T2, CODE + 16);
    setr(&m, T3, 0x80002468u);
    run(&m, CODE + 0x40, 8);
    EQ64(m.cpu.pc, CODE + 16, "MTC0 in a delay slot: JR still jumps");
    EQ64(sysr(&m, CP0_EPC), 0x80002468u, "and the slot wrote EPC");
    PROG(&m, CODE + 0x80, BNE(0, 0, 3), DI(T1), SPIN, NOP);
    sysw(&m, CP0_STATUS, ST_IE);
    run(&m, CODE + 0x80, 8);
    EQ64(m.cpu.pc, CODE + 0x88, "DI in a delay slot: the branch not taken falls through");
    CHECK(!(sysr(&m, CP0_STATUS) & ST_IE) && (gpr(&m, T1) & ST_IE), "DI cleared IE, returned the old Status");
    /* MTC0 enabling a pending interrupt in the slot: taken after the branch, at its target */
    PROG(&m, CODE + 0xC0, BEQ(0, 0, 3), MTC0(T3, 12, 0), SPIN, NOP, SPIN, NOP);
    sysw(&m, CP0_STATUS, 0);
    sysw(&m, CP0_CAUSE, 1u << 8);                                /* software interrupt 0 */
    setr(&m, T3, 1u << 8 | ST_IE);
    run(&m, CODE + 0xC0, 8);
    expect_exc(&m, GENERAL, EXC_INT, CODE + 0xD0, false, "an interrupt the slot's MTC0 enabled");
    uam_machine_fini(&m);
}

/* kuseg is unmapped while ERL is set; CP0 needs kernel mode or Status.CU0. */
static void test_modes(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32le, NULL, jit);
    PROG(&m, CODE, LW(T1, 0, T0), SPIN, NOP);
    setr(&m, T0, 0x1000);
    sysw(&m, CP0_STATUS, ST_ERL);
    run(&m, CODE, 8);
    EQ64(m.cpu.pc, CODE + 4, "ERL: kuseg is unmapped");
    EQ64(gpr(&m, T1), LW(T1, 0, T0), "ERL: kuseg 0x1000 is physical 0x1000");
    uam_machine_fini(&m);

    machine(&m, &uam_isa_mips32le, NULL, jit);
    tlb_set(&m, 1, 0x00400000u, PFN(0x20000) | LO_V | LO_D | LO_G, LO_G, 0);
    PROG(&m, 0x80020000u, MFC0(T1, 12, 0), SPIN, NOP, NOP, ERET, NOP);
    sysw(&m, CP0_STATUS, ST_UM);
    run(&m, 0x00400000u, 8);
    expect_exc(&m, GENERAL, EXC_CPU, 0x00400000u, false, "MFC0 in user mode");
    EQ64((sysr(&m, CP0_CAUSE) & CAUSE_CE_MASK) >> CAUSE_CE_SHIFT, 0, "Cause.CE 0");
    CHECK(sysr(&m, CP0_STATUS) & ST_UM, "KSU stays user under EXL");
    char *d = dump(&m);
    CHECK(d && strstr(d, "mode=kernel"), "dump: EXL is kernel mode");
    free(d);
    sysw(&m, CP0_STATUS, ST_UM | ST_CU0);
    d = dump(&m);
    CHECK(d && strstr(d, "mode=user") && strstr(d, "endian=little"), "dump: user mode, little-endian");
    free(d);
    run(&m, 0x00400000u, 8);
    EQ64(m.cpu.pc, 0x00400004u, "MFC0 in user mode with CU0");
    EQ64(gpr(&m, T1), ST_UM | ST_CU0, "it read Status");
    sysw(&m, CP0_STATUS, ST_UM);
    run(&m, 0x00400010u, 8);
    expect_exc(&m, GENERAL, EXC_CPU, 0x00400010u, false, "ERET in user mode");
    uam_machine_fini(&m);
}

/* ---- interrupts ---------------------------------------------------------------------- */

static void test_irq_compat(void)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, false);
    line(&m, 2, 1);
    EQ64(sysr(&m, CP0_CAUSE), 1u << 10, "line 2 is Cause.IP2");
    CHECK(!take(&m) && !wake(&m), "IM2 clear: neither taken nor waking");
    sysw(&m, CP0_STATUS, 1u << 10);
    CHECK(wake(&m) && !take(&m), "IM2: wakes WAIT, not taken with IE clear");
    sysw(&m, CP0_STATUS, 1u << 10 | ST_IE | ST_EXL);
    CHECK(!take(&m), "EXL masks it");
    sysw(&m, CP0_STATUS, 1u << 10 | ST_IE | ST_ERL);
    CHECK(!take(&m), "ERL masks it");
    line(&m, 5, 1);
    sysw(&m, CP0_STATUS, 0xFF00u | ST_IE);
    EQ64(lines_taken(&m), 1u << 2 | 1u << 5, "lines 2 and 5 reported");
    CHECK(take(&m), "taken");
    expect_exc(&m, GENERAL, EXC_INT, CODE, false, "an interrupt with Cause.IV clear");
    CHECK(!take(&m), "not again while EXL");
    line(&m, 5, 0);
    line(&m, 2, 0);
    EQ64(sysr(&m, CP0_CAUSE) & CAUSE_IP_MASK, 0, "the lines dropped");
    line(&m, 0, 1);
    line(&m, 1, 1);
    line(&m, 8, 1);
    EQ64(sysr(&m, CP0_CAUSE) & CAUSE_IP_MASK, 0, "lines 0, 1 (software) and 8 are not inputs");
    /* software interrupts and Cause.IV, then BEV */
    sysw(&m, CP0_CAUSE, CAUSE_IV | 1u << 9);
    CHECK(uam_mips_swint_line(&m.cpu, 1)->level == 1 && uam_mips_swint_line(&m.cpu, 0)->level == 0,
          "the software interrupt lines follow Cause.IP1:0");
    sysw(&m, CP0_STATUS, 0xFF00u | ST_IE);
    m.cpu.pc = CODE;
    EQ64(lines_taken(&m), 1u << 1, "IP1 reported");
    CHECK(take(&m), "IP1 taken");
    EQ64(m.cpu.pc, 0x80000200u, "Cause.IV: EBase + 0x200");
    sysw(&m, CP0_STATUS, 0xFF00u | ST_IE | ST_BEV);
    CHECK(take(&m), "with BEV");
    EQ64(m.cpu.pc, BEV_BASE + 0x200, "Cause.IV with BEV: 0xbfc00400");
    uam_machine_fini(&m);
}

/* Vectored interrupts (Config3.VInt without a controller), with shadow sets. */
/* ERET clears EXL while an interrupt is pending: it is taken at once, also by native
 * code that has linked the ERET's exit to the interrupted loop. The handler raises the
 * software interrupt again on every other entry, so some returns find nothing pending
 * (they link) and the others must not run on into the loop. */
static void eret_run(int mode, uint32_t *rounds, uint32_t *entries, uint64_t *icount)
{
    enum { S0 = 16, S1 = 17 };
    uam_machine m;
    machine(&m, &uam_isa_mips32le, NULL, mode != 0);
    m.exec.chain = mode == 2;
    PROG(&m, CODE, ADDIU(S0, S0, 1), I(0x0C, S0, T0, 0x3FF) /* andi */, BNE(T0, 0, -3), NOP,
         MTC0(T1, 13, 0) /* every 1024 rounds: raise it */, BEQ(0, 0, -6), NOP);
    PROG(&m, GENERAL, ADDIU(S1, S1, 1), MTC0(0, 13, 0) /* lower it */, I(0x0C, S1, T2, 1),
         BEQ(T2, 0, 2), NOP, MTC0(T1, 13, 0) /* odd entries: raise it again */, ERET);
    setr(&m, T1, 1u << 8);                         /* Cause.IP0 */
    setr(&m, S0, 0);
    setr(&m, S1, 0);
    sysw(&m, CP0_STATUS, 1u << 8 | 1u);            /* IM0, IE */
    run(&m, CODE, 40000);
    *rounds = gpr(&m, S0);
    *entries = gpr(&m, S1);
    *icount = m.cpu.icount;
    uam_machine_fini(&m);
}

static void test_eret_pending(void)
{
    uint32_t rounds[3], entries[3];
    uint64_t icount[3];
    for (int mode = 0; mode < 3; mode++)   /* interpreter, native, native and chained */
        eret_run(mode, &rounds[mode], &entries[mode], &icount[mode]);
    CHECK(entries[0] >= 10, "the handler ran (%u entries)", entries[0]);
    for (int mode = 1; mode < 3; mode++)
        CHECK(rounds[mode] == rounds[0] && entries[mode] == entries[0] && icount[mode] == icount[0],
              "ERET with an interrupt pending, %s: %u rounds, %u entries (interpreter %u, %u)",
              mode == 1 ? "native" : "chained", rounds[mode], entries[mode], rounds[0], entries[0]);
}

static void test_irq_vectored(bool jit)
{
    uam_mips_config c = uam_mips_ref;
    c.name = "ref-vint";
    c.config3 = CFG3_VINT;
    c.shadow_sets = 2;
    uam_machine m;
    machine(&m, &uam_isa_mips32le, &c, jit);
    PROG(&m, CODE, SPIN, NOP);
    EQ64(sysr(&m, CP0_INTCTL) >> 29, 7, "IntCtl.IPTI: the timer on IP7");
    sysw(&m, CP0_STATUS, 0xFF00u | ST_IE);
    sysw(&m, CP0_CAUSE, CAUSE_IV);
    line(&m, 3, 1);
    line(&m, 6, 1);
    CHECK(take(&m), "IntCtl.VS 0: not vectored");
    EQ64(m.cpu.pc, 0x80000200u, "VS 0: EBase + 0x200");
    m.cpu.pc = CODE;
    sysw(&m, CP0_STATUS, 0xFF00u | ST_IE);
    sysw(&m, CP0_INTCTL, 1u << 5);
    sysw(&m, CP0_SRSMAP, 2u << 24);
    setr(&m, T0, 0x1111);
    CHECK(take(&m), "taken vectored");
    EQ64(m.cpu.pc, 0x80000200u + 6 * 32, "the highest line, 6: EBase + 0x200 + 6 * 32");
    EQ64(sysr(&m, CP0_SRSCTL) & 0x3CF, 2, "SRSMap: vector 6 runs in shadow set 2, PSS 0");
    EQ64(gpr(&m, T0), 0, "the shadow set's t0");
    setr(&m, T0, 0x2222);
    line(&m, 3, 0);
    line(&m, 6, 0);
    PROG(&m, 0x80000200u + 6 * 32, ERET);
    run(&m, 0x80000200u + 6 * 32, 4);
    EQ64(m.cpu.pc, CODE, "ERET");
    EQ64(sysr(&m, CP0_SRSCTL) & 0xF, 0, "back in set 0");
    EQ64(gpr(&m, T0), 0x1111, "with its t0");
    uam_machine_fini(&m);
}

/* An external interrupt controller (uam_mips_eic_input). */
static void test_irq_eic(void)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32le, &uam_mips_m16_ref, false);
    sysw(&m, CP0_INTCTL, 1u << 5);
    sysw(&m, CP0_CAUSE, CAUSE_IV);
    sysw(&m, CP0_STATUS, ST_IE);
    line(&m, 2, 1);
    CHECK(!wake(&m) && !take(&m), "a controller: the hardware lines are not requests");
    uam_mips_eic_input(&m.cpu, 3, 10, UAM_MIPS_EIC_NO_OFFSET, 1);
    EQ64((sysr(&m, CP0_CAUSE) >> 10) & 0x3F, 3, "Cause.RIPL");
    EQ64(lines_taken(&m), 1u << 10, "vector 10 reported");
    CHECK(wake(&m) && take(&m), "RIPL 3 above IPL 0: taken");
    EQ64(m.cpu.pc, 0x80000200u + 10 * 32, "EBase + 0x200 + vector * spacing");
    EQ64(sysr(&m, CP0_SRSCTL), 1u << 26 | 1u << 18 | 1u, "SRSCtl: HSS 1, EICSS 1, CSS 1, PSS 0");
    m.cpu.pc = CODE;
    sysw(&m, CP0_STATUS, ST_IE | 3u << ST_IPL_SHIFT);
    CHECK(!wake(&m) && !take(&m) && !lines_taken(&m), "RIPL 3 at IPL 3: masked");
    uam_mips_eic_input(&m.cpu, 4, 70, 0x600, 0);
    EQ64(lines_taken(&m), UINT64_C(1) << 63, "vector 70 reported as line 63");
    CHECK(take(&m), "RIPL 4 above IPL 3");
    EQ64(m.cpu.pc, 0x80000600u, "an explicit offset from EBase");
    sysw(&m, CP0_CAUSE, 0);
    sysw(&m, CP0_STATUS, ST_IE);
    CHECK(take(&m), "Cause.IV clear");
    EQ64(m.cpu.pc, GENERAL, "Cause.IV clear: the general vector");
    /* the timer is the controller's input: Cause.TI, not IP7 */
    sysw(&m, CP0_COMPARE, sysr(&m, CP0_COUNT) + 10);
    tick(&m, 22);
    CHECK(uam_mips_timer_line(&m.cpu)->level == 1, "the timer line is raised");
    EQ64(sysr(&m, CP0_CAUSE) & (CAUSE_TI | 1u << 15), CAUSE_TI, "Cause.TI; IP7 is RIPL's");
    sysw(&m, CP0_COMPARE, 0);
    CHECK(uam_mips_timer_line(&m.cpu)->level == 0, "writing Compare lowers it");
    uam_machine_fini(&m);
}

/* WAIT waits; a pending, unmasked interrupt wakes it, taken only with IE. */
static void test_wait(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, jit);
    PROG(&m, CODE, WAIT, SPIN, NOP);
    sysw(&m, CP0_STATUS, 1u << 10);
    line(&m, 3, 1);
    run(&m, CODE, 20);
    CHECK(m.cpu.waiting, "WAIT waits");
    EQ64(m.cpu.pc, CODE + 4, "after the WAIT");
    uam_exec_run(&m.exec, 20);
    CHECK(m.cpu.waiting, "a masked line does not wake it");
    line(&m, 2, 1);
    uam_exec_run(&m.exec, 20);
    CHECK(!m.cpu.waiting, "an unmasked line wakes it with IE clear");
    EQ64(m.cpu.pc, CODE + 4, "and it goes on");
    CHECK(!(sysr(&m, CP0_STATUS) & ST_EXL), "no interrupt taken");
    line(&m, 2, 0);
    sysw(&m, CP0_STATUS, 1u << 10 | ST_IE);
    run(&m, CODE, 20);
    CHECK(m.cpu.waiting, "WAIT again");
    line(&m, 2, 1);
    uam_exec_run(&m.exec, 20);
    expect_exc(&m, GENERAL, EXC_INT, CODE + 4, false, "woken with IE: the interrupt");
    uam_machine_fini(&m);
}

/* ---- Count and Compare ----------------------------------------------------------- */

static void test_timer(void)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, false);
    sysw(&m, CP0_COUNT, 1000);
    EQ64(sysr(&m, CP0_COUNT), 1000, "Count written");
    tick(&m, 20);
    EQ64(sysr(&m, CP0_COUNT), 1010, "Count: one per two ticks of virtual time");
    sysw(&m, CP0_COMPARE, 1020);
    tick(&m, 19);
    EQ64(sysr(&m, CP0_COUNT), 1019, "Count 1019");
    CHECK(!(sysr(&m, CP0_CAUSE) & 1u << 15), "no match before Compare");
    tick(&m, 2);
    EQ64(sysr(&m, CP0_COUNT), 1020, "Count reached Compare");
    EQ64(sysr(&m, CP0_CAUSE) & (CAUSE_TI | 1u << 15), CAUSE_TI | 1u << 15, "the match: Cause.IP7 and TI");
    CHECK(uam_mips_timer_line(&m.cpu)->level == 1, "the timer line");
    sysw(&m, CP0_STATUS, 1u << 15 | ST_IE);
    EQ64(lines_taken(&m), 1u << 7, "line 7 reported");
    CHECK(take(&m), "the timer interrupt is taken");
    tick(&m, 100);
    CHECK(sysr(&m, CP0_CAUSE) & 1u << 15, "it stays until Compare is written");
    sysw(&m, CP0_COMPARE, sysr(&m, CP0_COMPARE));
    CHECK(!(sysr(&m, CP0_CAUSE) & (CAUSE_TI | 1u << 15)), "writing Compare clears it, and TI");
    CHECK(uam_mips_timer_line(&m.cpu)->level == 0, "and the line");
    /* across the wrap of Count */
    sysw(&m, CP0_COUNT, 0xFFFFFFF0u);
    sysw(&m, CP0_COMPARE, 0x10);
    tick(&m, 2 * 0x1F);
    CHECK(!(sysr(&m, CP0_CAUSE) & 1u << 15), "Count wrapping: not before 0x10");
    tick(&m, 4);
    CHECK(sysr(&m, CP0_CAUSE) & 1u << 15, "Count wrapped to Compare 0x10");
    /* the match is when Count becomes Compare, whichever half of a count Compare is written in */
    for (unsigned odd = 0; odd < 2; odd++) {
        if ((uam_sched_now(&m.sched) & 1) != odd)
            tick(&m, 1);
        const uint32_t cmp = sysr(&m, CP0_COUNT) + 5;
        sysw(&m, CP0_COMPARE, cmp);
        while (sysr(&m, CP0_COUNT) != cmp - 1)
            tick(&m, 1);
        CHECK(!(sysr(&m, CP0_CAUSE) & 1u << 15), "written on an %s tick: no match at Compare - 1",
              odd ? "odd" : "even");
        tick(&m, 1);
        while (sysr(&m, CP0_COUNT) == cmp - 1)
            tick(&m, 1);
        EQ64(sysr(&m, CP0_COUNT), cmp, "Count reached Compare");
        CHECK(sysr(&m, CP0_CAUSE) & 1u << 15, "written on an %s tick: the match as Count reaches Compare",
              odd ? "odd" : "even");
    }
    uam_machine_fini(&m);
}

/* ---- the TLB, fixed mapping ------------------------------------------------------------ */

static void test_tlb(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, jit);
    put(&m, 0x80020000u, 0xA0A0A0A0u);
    put(&m, 0x80021004u, 0xB1B1B1B1u);
    put(&m, 0x80042004u, 0xC2C2C2C2u);
    put(&m, 0x80046008u, 0xD3D3D3D3u);
    PROG(&m, CODE, LUI(T0, 0x0040), LW(T1, 0, T0), LW(T2, 0x1004, T0), SPIN, NOP);
    /* an entry of ASID 5 */
    tlb_set(&m, 2, 0x00400000u | 5, PFN(0x20000) | LO_V | LO_D, PFN(0x21000) | LO_V, 0);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, CODE + 12, "ASID 5: both pages map");
    CHECK(gpr(&m, T1) == 0xA0A0A0A0u && gpr(&m, T2) == 0xB1B1B1B1u, "the even and the odd page");
    sysw(&m, CP0_CONTEXT, 0xFF800000u);
    sysw(&m, CP0_ENTRYHI, 6);
    run(&m, CODE, 10);
    expect_exc(&m, 0x80000000u, EXC_TLBL, CODE + 4, false, "ASID 6: a refill");
    EQ64(sysr(&m, CP0_BADVADDR), 0x00400000u, "BadVAddr");
    EQ64(sysr(&m, CP0_ENTRYHI), 0x00400006u, "EntryHi: the VPN2 that missed, ASID kept");
    EQ64(sysr(&m, CP0_CONTEXT), 0xFF800000u | 0x00400000u >> 9, "Context: PTEBase kept, BadVPN2");
    /* the same entry global */
    tlb_set(&m, 2, 0x00400000u | 5, PFN(0x20000) | LO_V | LO_D | LO_G, PFN(0x21000) | LO_V | LO_G, 0);
    sysw(&m, CP0_ENTRYHI, 6);
    sysw(&m, CP0_STATUS, 0);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, CODE + 12, "a global entry maps for ASID 6");
    /* 16 KiB pages */
    tlb_set(&m, 3, 0x00800000u | 6, PFN(0x40000) | LO_V | LO_G, PFN(0x44000) | LO_V | LO_G, 0x6000);
    PROG(&m, CODE, LUI(T0, 0x0080), LW(T1, 0x2004, T0), LW(T2, 0x6008, T0), SPIN, NOP);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, CODE + 12, "16 KiB pages map");
    CHECK(gpr(&m, T1) == 0xC2C2C2C2u && gpr(&m, T2) == 0xD3D3D3D3u, "16 KiB: even and odd page");
    sysw(&m, CP0_INDEX, 3);
    sysw(&m, CP0_PAGEMASK, 0);
    tlb_op(&m, TLBR);
    EQ64(sysr(&m, CP0_PAGEMASK), 0x6000, "TLBR: PageMask");
    EQ64(sysr(&m, CP0_ENTRYHI), 0x00800006u, "TLBR: EntryHi");
    EQ64(sysr(&m, CP0_ENTRYLO1), PFN(0x44000) | LO_V | LO_G, "TLBR: EntryLo1");
    /* TLBWR stays above Wired */
    sysw(&m, CP0_WIRED, 4);
    sysw(&m, CP0_ENTRYHI, 0x00C00000u);
    sysw(&m, CP0_PAGEMASK, 0);
    sysw(&m, CP0_ENTRYLO0, LO_G);
    sysw(&m, CP0_ENTRYLO1, LO_G);
    tlb_op(&m, TLBWR);
    sysw(&m, CP0_INDEX, 0);
    tlb_op(&m, TLBP);
    CHECK(sysr(&m, CP0_INDEX) >= 4 && sysr(&m, CP0_INDEX) <= 15, "TLBWR wrote entry %u, at or above Wired",
          (unsigned)sysr(&m, CP0_INDEX));
    uam_machine_fini(&m);

    /* fixed mapping: kuseg is physical + 0x40000000 (unmapped with ERL), no TLB instructions */
    machine(&m, &uam_isa_mips32le, &uam_mips_m16_ref, jit);
    CHECK(uam_bus_map_ram(&m.bus, "useg", 0x40000000u, 0x10000) == UAM_OK, "map RAM at 0x40000000");
    const uint8_t w[4] = { 0x44, 0x33, 0x22, 0x11 };
    CHECK(uam_bus_load_bytes(&m.bus, 0x40000100u, w, 4) == UAM_OK, "a word at 0x40000100");
    PROG(&m, CODE, LW(T1, 0x100, 0), SPIN, NOP);
    run(&m, CODE, 8);
    EQ64(gpr(&m, T1), 0x11223344u, "fixed mapping: kuseg 0x100 is physical 0x40000100");
    put(&m, 0x80000100u, 0x5A5A5A5Au);
    sysw(&m, CP0_STATUS, ST_ERL);
    run(&m, CODE, 8);
    EQ64(gpr(&m, T1), 0x5A5A5A5Au, "with ERL: physical 0x100");
    sysw(&m, CP0_STATUS, 0);
    PROG(&m, CODE, TLBWI, SPIN, NOP);
    run(&m, CODE, 8);
    expect_exc(&m, GENERAL, EXC_RI, CODE, false, "TLBWI with fixed mapping");
    uam_machine_fini(&m);
}

/* ---- ISA modes and features ------------------------------------------------------------ */

static void test_isa_modes(bool jit)
{
    uam_machine m;
    /* MIPS16e: JALX in, BREAK reports bit 0 in EPC, the handler runs MIPS32, ERET back */
    machine(&m, &uam_isa_mips32le, &uam_mips_m16_ref, jit);
    const uint32_t m16 = CODE + 0x100;
    PROG(&m, CODE, JALX(m16), NOP);
    put16(&m, put16(&m, put16(&m, m16, M16_LI_V0_5), M16_BREAK), M16_B_SELF);
    run(&m, CODE, 10);
    expect_exc(&m, GENERAL, EXC_BP, (m16 + 2) | 1, false, "BREAK in MIPS16e");
    EQ64(gpr(&m, V0), 5, "JALX ran MIPS16e");
    EQ64(gpr(&m, RA), CODE + 8, "JALX's link: MIPS32");
    sysw(&m, CP0_EPC, (m16 + 4) | 1);
    PROG(&m, SCRATCH, ERET);
    run(&m, SCRATCH, 6);
    EQ64(m.cpu.pc, (m16 + 4) | 1, "ERET to EPC bit 0 set: MIPS16e");
    char *d = dump(&m);
    CHECK(d && strstr(d, "isa=mips16e"), "dump: isa=mips16e");
    free(d);
    uam_machine_fini(&m);

    /* the longest SAVE: ra, seven xsregs, s1, s0 and four static argument registers */
    machine(&m, &uam_isa_mips32le, &uam_mips_m16_ref, jit);
    static const uint8_t saved[14] = { 31, 30, 23, 22, 21, 20, 19, 18, 17, 16, 7, 6, 5, 4 };
    for (unsigned k = 0; k < 14; k++)
        setr(&m, saved[k], 0xA5000000u + saved[k]);
    setr(&m, 29, 0x80008000u);
    put16(&m, put16(&m, put16(&m, m16, 0xF70B), 0x64F8), M16_B_SELF);   /* save 64, ra, s0-s8, a0-a3 */
    run(&m, m16 | 1, 1);
    EQ64(m.cpu.pc, (m16 + 4) | 1, "SAVE with fourteen registers ran");
    EQ64(gpr(&m, 29), 0x80008000u - 64, "SAVE: the frame");
    for (unsigned k = 0; k < 14; k++)
        CHECK(mem32(&m, 0x80008000u - 4 * (k + 1)) == 0xA5000000u + saved[k],
              "SAVE: register %u in slot %u (0x%x)", saved[k], k, mem32(&m, 0x80008000u - 4 * (k + 1)));
    uam_machine_fini(&m);

    /* microMIPS only: handlers in microMIPS, no MIPS32 */
    machine(&m, &uam_isa_mips32le, &uam_mips_umips_ref, jit);
    put16(&m, put16(&m, put16(&m, put16(&m, CODE, 0x0000), 0x8B7C), UM_B16_SELF), UM_NOP16);
    run(&m, CODE | 1, 6);
    expect_exc(&m, GENERAL, EXC_SYS, CODE | 1, false, "SYSCALL in microMIPS");
    d = dump(&m);
    CHECK(d && strstr(d, "isa=micromips"), "dump: isa=micromips");
    free(d);
    sysw(&m, CP0_STATUS, 0);
    run(&m, CODE + 0x40, 6);
    expect_exc(&m, GENERAL, EXC_ADEL, CODE + 0x40, false, "microMIPS only: an even pc");
    EQ64(sysr(&m, CP0_BADVADDR), CODE + 0x40, "its BadVAddr");
    uam_machine_fini(&m);

    /* both: Config3.ISAOnExc picks the handlers' ISA; IRET (MCU ASE) is ERET */
    machine(&m, &uam_isa_mips32le, &uam_mips_mz_ref, jit);
    PROG(&m, CODE, SYSCALL, NOP, SPIN, NOP);
    sysw(&m, CP0_CONFIG3, CFG3_ISAONEXC);
    spin(&m, GENERAL);
    run(&m, CODE, 6);
    expect_exc(&m, GENERAL, EXC_SYS, CODE, false, "ISAOnExc: a MIPS32 SYSCALL, a microMIPS handler");
    sysw(&m, CP0_EPC, CODE + 8);
    PROG(&m, SCRATCH, IRET);
    run(&m, SCRATCH, 6);
    EQ64(m.cpu.pc, CODE + 8, "IRET returns to EPC");
    CHECK(!(sysr(&m, CP0_STATUS) & ST_EXL), "and clears EXL");
    uam_machine_fini(&m);
}

/* An instruction of a feature the model lacks traps. */
static void test_features(bool jit)
{
    static const struct { unsigned model; uint32_t insn; unsigned code, ce; const char *what; } cases[] = {
        { REF, JALX(CODE + 0x100), EXC_RI, 0, "JALX without a compressed ISA" },
        { REF, ADDU_QB, EXC_RI,  0, "a DSP instruction without the DSP ASE" },
        { M16, ADDU_QB, EXC_RI,  0, "a DSP instruction on the MIPS16e model" },
        { REF, IRET,    EXC_RI,  0, "IRET without the MCU ASE" },
        { M16, IRET,    EXC_RI,  0, "IRET on the MIPS16e model" },
        { M16, MFC1,    EXC_CPU, 1, "an FPU instruction without the FPU" },
        { REF, LWC2,    EXC_CPU, 2, "COP2 load: coprocessor 2 unusable" },
        { MZ,  LWC2,    EXC_CPU, 2, "COP2 load on mz" },
        { M16, TLBP,    EXC_RI,  0, "TLBP with fixed mapping" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        uam_machine m;
        machine(&m, &uam_isa_mips32le, models[cases[i].model], jit);
        PROG(&m, CODE, cases[i].insn, NOP, SPIN, NOP);
        run(&m, CODE, 8);
        expect_exc(&m, GENERAL, cases[i].code, CODE, false, cases[i].what);
        CHECK(((sysr(&m, CP0_CAUSE) & CAUSE_CE_MASK) >> CAUSE_CE_SHIFT) == cases[i].ce, "%s: Cause.CE %u",
              cases[i].what, cases[i].ce);
        uam_machine_fini(&m);
    }
}

/* ---- the debugger view ------------------------------------------------------------------ */

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, &uam_isa_mips32be, NULL, false);
    const uam_gdb_arch *g = m.cpu.isa->gdb;
    CHECK(g && g->n_regs == 72 && g->reg_bytes == 4 && g->pc_state_bits == 1, "72 registers of 4 bytes, pc bit 0");
    CHECK(strstr(g->target_xml, "<architecture>mips:isa32r2</architecture>") &&
          strstr(g->target_xml, "org.gnu.gdb.mips.fpu"), "target description");
    uint64_t v = 0;
    setr(&m, T1, 0xBEEF);
    uam_cpu_reg_write(&m.cpu, MIPS_LO_OFF, 32, 0x10);
    uam_cpu_reg_write(&m.cpu, MIPS_HI_OFF, 32, 0x20);
    sysw(&m, CP0_STATUS, 0xFF01u);
    line(&m, 4, 1);
    CHECK(g->read_reg(&m.cpu, 9, &v) && v == 0xBEEF, "gdb r9");
    CHECK(g->read_reg(&m.cpu, 32, &v) && v == 0xFF01u, "gdb sr");
    CHECK(g->read_reg(&m.cpu, 33, &v) && v == 0x10, "gdb lo");
    CHECK(g->read_reg(&m.cpu, 34, &v) && v == 0x20, "gdb hi");
    CHECK(g->read_reg(&m.cpu, 36, &v) && v == 1u << 12, "gdb cause");
    CHECK(g->read_reg(&m.cpu, 37, &v) && v == CODE, "gdb pc");
    CHECK(g->read_reg(&m.cpu, 40, &v) && v == 0 && g->read_reg(&m.cpu, 71, &v) && v == 0, "no FPU: zeros");
    CHECK(!g->read_reg(&m.cpu, 72, &v), "no register 72");
    CHECK(g->write_reg(&m.cpu, 0, 5) && gpr(&m, 0) == 0, "r0 stays 0");
    CHECK(g->write_reg(&m.cpu, 35, 5) && sysr(&m, CP0_BADVADDR) == 0, "badvaddr is read-only");
    CHECK(g->write_reg(&m.cpu, 37, 0x80002000u) && m.cpu.pc == 0x80002000u, "gdb write pc");
    CHECK(g->write_reg(&m.cpu, 32, ST_UM) && MIPS_PRIV_MODE(m.cpu.priv) == MIPS_USER, "gdb sr: user mode");
    CHECK(!g->write_reg(&m.cpu, 72, 0), "no register 72 to write");
    char *d = dump(&m);
    CHECK(d && strstr(d, "endian=big") && !strstr(d, "fcsr="), "dump: big-endian, no FPU");
    free(d);
    uam_machine_fini(&m);

    /* the FPU's registers follow Status.FR */
    machine(&m, &uam_isa_mips32le, &uam_mips_mz_ref, false);
    uam_cpu_reg_write(&m.cpu, MIPS_FPR_OFF(2), 64, 0x1111111122222222ull);
    uam_cpu_reg_write(&m.cpu, MIPS_FPR_OFF(3), 64, 0x3333333344444444ull);
    CHECK(g->read_reg(&m.cpu, 38 + 3, &v) && v == 0x11111111u, "FR 0: f3 is the high word of slot 2");
    sysw(&m, CP0_STATUS, ST_CU1 | ST_FR);
    CHECK(g->read_reg(&m.cpu, 38 + 3, &v) && v == 0x44444444u, "FR 1: f3 is slot 3");
    CHECK(g->write_reg(&m.cpu, 38 + 2, 0x55u) && uam_cpu_reg_read(&m.cpu, MIPS_FPR_OFF(2), 32) == 0x55,
          "gdb write f2");
    CHECK(g->write_reg(&m.cpu, 70, 0x3) && uam_cpu_reg_read(&m.cpu, MIPS_FCSR_OFF, 32) == 3, "gdb fcsr");
    CHECK(g->read_reg(&m.cpu, 71, &v) && v == uam_mips_mz_ref.fir, "gdb fir");
    d = dump(&m);
    CHECK(d && strstr(d, "fcsr=00000003") && strstr(d, "dspcontrol=") && strstr(d, "srsctl="),
          "dump: the FPU, the DSP ASE, the shadow sets");
    free(d);
    uam_machine_fini(&m);
}

/* ---- snapshots ------------------------------------------------------------------------- */

static void test_snapshot(void)
{
    static const struct { uint32_t id; const char *name; } regs[] = {
        { CP0_STATUS, "Status" }, { CP0_CAUSE, "Cause" }, { CP0_EPC, "EPC" }, { CP0_ERROREPC, "ErrorEPC" },
        { CP0_BADVADDR, "BadVAddr" }, { CP0_EBASE, "EBase" }, { CP0_INTCTL, "IntCtl" },
        { CP0_SRSCTL, "SRSCtl" }, { CP0_SRSMAP, "SRSMap" }, { CP0_CONFIG, "Config" },
        { CP0_CONFIG3, "Config3" }, { CP0_INDEX, "Index" }, { CP0_ENTRYHI, "EntryHi" },
        { CP0_ENTRYLO0, "EntryLo0" }, { CP0_WIRED, "Wired" }, { CP0_PAGEMASK, "PageMask" },
        { CP0_CONTEXT, "Context" }, { CP0_COMPARE, "Compare" }, { CP0_COUNT, "Count" },
    };
    uam_machine m;
    machine(&m, &uam_isa_mips32le, &uam_mips_mz_ref, false);
    PROG(&m, CODE, SPIN, NOP);
    tlb_set(&m, 2, 0x00400000u | 5, PFN(0x20000) | LO_V | LO_D, PFN(0x21000) | LO_V, 0);
    sysw(&m, CP0_STATUS, 0);
    m.cpu.pc = CODE;
    run(&m, 0x00004000u, 4);                            /* a refill: BadVAddr, Context */
    sysw(&m, CP0_EBASE, EBASE2);
    sysw(&m, CP0_INTCTL, 1u << 5);
    sysw(&m, CP0_CAUSE, CAUSE_IV | 1u << 8);
    sysw(&m, CP0_ERROREPC, 0x80005678u);
    sysw(&m, CP0_CONFIG, (sysr(&m, CP0_CONFIG) & ~7u) | 3);
    sysw(&m, CP0_SRSMAP, 0x76543210u);
    sysw(&m, CP0_WIRED, 3);
    sysw(&m, CP0_PAGEMASK, 0x6000);
    sysw(&m, CP0_ENTRYHI, 0x00400005u);
    sysw(&m, CP0_ENTRYLO0, 0x1234);
    sysw(&m, CP0_INDEX, 7);
    const uint32_t cmp = sysr(&m, CP0_COUNT) + 100;
    sysw(&m, CP0_COMPARE, cmp);
    sysw(&m, CP0_STATUS, ST_IE | 2u << ST_IPL_SHIFT | ST_CU1 | ST_FR);
    m.cpu.pc = CODE;
    setr(&m, T0, 0x1111);
    uam_mips_eic_input(&m.cpu, 4, 3, UAM_MIPS_EIC_NO_OFFSET, 5);
    CHECK(take(&m), "an interrupt into shadow set 5");
    setr(&m, T0, 0x5555);
    sysw(&m, CP0_SRSCTL, (sysr(&m, CP0_SRSCTL) & ~(0xFu << 12)) | 6u << 12);
    sysw(&m, CP0_CONFIG3, CFG3_ISAONEXC);
    line(&m, 4, 1);
    uint32_t want[UAM_ARRAY_LEN(regs)];
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        want[i] = sysr(&m, regs[i].id);
    const uint64_t pc = m.cpu.pc;

    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");
    uam_machine_reset(&m, CODE);
    line(&m, 4, 0);
    CHECK(sysr(&m, CP0_STATUS) != want[0], "the reset changed Status");
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        CHECK(sysr(&m, regs[i].id) == want[i], "restored %s: 0x%x want 0x%x", regs[i].name,
              (unsigned)sysr(&m, regs[i].id), (unsigned)want[i]);
    EQ64(m.cpu.pc, pc, "pc");
    EQ64(m.cpu.priv, MIPS_PRIV_CU1 | MIPS_PRIV_FR, "the mode (kernel by EXL, CU1, FR)");
    EQ64(st(&m)->hwlines, 1u << 4, "the hardware lines");
    EQ64(lines_taken(&m), 1u << 3, "the controller's request");
    EQ64(gpr(&m, T0), 0x5555, "the current shadow set's t0");
    uam_xlate x = { 0 };
    CHECK(m.cpu.isa->translate(&m.cpu, 0x00400004u, UAM_XLATE_LOAD, &x) == UAM_OK && x.paddr == 0x20004,
          "the TLB entry, ASID 5");
    CHECK(m.cpu.isa->translate(&m.cpu, 0x00401000u, UAM_XLATE_STORE, &x) == UAM_ERR_XLATE &&
          x.fault == MIPS_XF_MOD, "its odd page clean");
    /* ERET to the previous set; the restored timer fires at Compare */
    uam_mips_eic_input(&m.cpu, 0, 0, UAM_MIPS_EIC_NO_OFFSET, 0);
    PROG(&m, SCRATCH, ERET);
    run(&m, SCRATCH, 2);
    EQ64(m.cpu.pc, CODE, "ERET");
    EQ64(gpr(&m, T0), 0x1111, "the previous set's t0");
    tick(&m, 2 * (cmp - sysr(&m, CP0_COUNT)) - 2);
    CHECK(!(sysr(&m, CP0_CAUSE) & CAUSE_TI), "the restored timer: not before Compare");
    tick(&m, 4);
    CHECK(sysr(&m, CP0_CAUSE) & CAUSE_TI, "the restored timer fires at Compare");
    /* a snapshot of another model is refused */
    CHECK(uam_mips_configure(&m.cpu, &uam_mips_ref) == UAM_OK, "another model");
    CHECK(uam_snapshot_restore(&m, s) != UAM_OK, "a snapshot of mz on the reference model is refused");
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

/* ---- byte order ------------------------------------------------------------------------ */

static void test_endian(const uam_isa *isa, bool jit)
{
    const bool be = isa == &uam_isa_mips32be;
    const char *name = be ? "big-endian" : "little-endian";
    uam_machine m;
    machine(&m, isa, NULL, jit);
    const uint32_t sub = CODE + 0x100;
    PROG(&m, CODE, LUI(T0, DATA >> 16), ORI(T0, T0, DATA & 0xFFFF), LUI(T1, 0x1122), ORI(T1, T1, 0x3344),
         SW(T1, 0, T0), LBU(T2, 0, T0), LHU(T3, 2, T0), JAL(sub), NOP, SPIN, NOP);
    PROG(&m, sub, JR(RA), ADDIU(V0, 0, 0x5A));
    run(&m, CODE, 30);
    CHECK(m.cpu.pc == CODE + 36, "%s: ran to the end (pc 0x%llx)", name, (unsigned long long)m.cpu.pc);
    CHECK(mem8(&m, DATA) == (be ? 0x11 : 0x44), "%s: SW byte order", name);
    CHECK(mem32(&m, DATA) == 0x11223344u, "%s: the word", name);
    CHECK(gpr(&m, T2) == (be ? 0x11u : 0x44u), "%s: LBU", name);
    CHECK(gpr(&m, T3) == (be ? 0x3344u : 0x1122u), "%s: LHU 0x%x", name, (unsigned)gpr(&m, T3));
    CHECK(gpr(&m, V0) == 0x5A && gpr(&m, RA) == CODE + 36, "%s: JAL, JR and the delay slot", name);
    CHECK(!(sysr(&m, CP0_CONFIG) & 1u << 15) == !be, "%s: Config.BE", name);
    char *d = dump(&m);
    CHECK(d && strstr(d, be ? "endian=big" : "endian=little"), "%s: the dump says so", name);
    free(d);
    uam_machine_fini(&m);
}

int main(void)
{
    test_models();
    test_cp0_masks();
    test_irq_compat();
    test_irq_eic();
    test_timer();
    test_gdb();
    test_snapshot();
    test_eret_pending();
    for (int jit = 0; jit < 2; jit++) {
        test_vectors(jit);
        test_delay_slots(jit);
        test_cop0_in_slot(jit);
        test_modes(jit);
        test_irq_vectored(jit);
        test_wait(jit);
        test_tlb(jit);
        test_isa_modes(jit);
        test_features(jit);
        test_endian(&uam_isa_mips32be, jit);
        test_endian(&uam_isa_mips32le, jit);
    }
    printf("test_mips: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
