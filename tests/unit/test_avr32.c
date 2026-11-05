/*
 * test_avr32.c - The AVR32 module's parts the self-test firmware cannot reach on
 * avr32-ref (and the only AVR32 check on a host without avr32-gcc): model selection and
 * its validation, the reset state and CONFIG0/1 of each microarchitecture, interrupts
 * through the board's API (the four levels and their masks, nesting, NMI, the autovector,
 * what is saved: SR, PC and R8-R12/LR on the system stack on AVR32A, RSR/RAR on AVR32B,
 * the INT3 shadow registers of AVR32B, the lines reported), rete back to application mode
 * with the banked SP, exceptions while EM is set (unrecoverable), exception frames from
 * either mode, breakpoint with DM set, SLEEP waiting and waking, COUNT/COMPARE in virtual
 * time, the MPU's application-mode permissions and subregions (A), the segments, the
 * ASID, page sizes, ITLB misses, multiple hits and application access to P1-P4 (B), the
 * avr32-ref board's interrupt controller, the debugger view, and snapshots.
 *
 * Each test runs on a bare machine: RAM at physical 0, EVBA at 0x4000 (AVR32B: 0x80004000,
 * in P1) with every halfword of the event table a branch to itself, so an exception or
 * interrupt leaves the hart spinning where it was delivered. Privileged code runs at 0x1000
 * (AVR32B: 0x80001000), application code at 0x2000 (U0 untranslated on AVR32B). The tests
 * that execute code run interpreted and under the JIT. Instructions are hand-encoded from
 * the AVR32 Architecture Manual (chapter 9); test_encodings checks each against the decoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../isa/avr32/avr32_op.h"
#include "uam/boards.h"
#include "uam/bus.h"
#include "uam/dev/avr32_intc.h"
#include "uam/isa/avr32.h"
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

/* SR (Architecture Manual 2.x). */
#define SR_C   (1u << 0)
#define SR_Z   (1u << 1)
#define SR_N   (1u << 2)
#define SR_L   (1u << 5)
#define SR_GM  (1u << 16)
#define IM(l)  (1u << (17 + (l)))     /* I0M..I3M */
#define SR_EM  (1u << 21)
#define SR_D   (1u << 26)
#define SR_DM  (1u << 27)
enum { M_APP, M_SUP, M_INT0, M_INT1, M_INT2, M_INT3, M_EX, M_NMI };
#define MODE(m) ((uint32_t)(m) << 22)
#define MODE_OF(sr) (((sr) >> 22) & 7)

/* System registers by address (Table 2-x). */
enum { A_SR = 0, A_EVBA = 4, A_ACBA = 8, A_ECR = 16, A_RSR_SUP = 20, A_RSR_DBG = 48, A_RAR_SUP = 52,
       A_RAR_DBG = 80, A_CONFIG0 = 256, A_CONFIG1 = 260, A_COUNT = 264, A_COMPARE = 268,
       A_TLBEHI = 272, A_TLBELO = 276, A_TLBEAR = 284, A_MMUCR = 288, A_MPUAR0 = 320,
       A_MPUPSR0 = 352, A_MPUAPRA = 400, A_MPUAPRB = 404, A_MPUCR = 408 };
#define RSR(mode) (A_RSR_SUP + 4u * ((mode) - 1))
#define RAR(mode) (A_RAR_SUP + 4u * ((mode) - 1))

/* Event handler offsets from EVBA (Table 8-1). */
enum { EV_UNREC = 0x00, EV_MULTI = 0x04, EV_NMI = 0x10, EV_BKPT = 0x1C, EV_PRIV = 0x28,
       EV_ADDR_R = 0x34, EV_DPROT_R = 0x3C, EV_DPROT_W = 0x40, EV_ITLB_MISS = 0x50 };
/* The autovectors this test's controller answers: 0x200 + 0x10 * level. */
#define VEC_INT(l) (0x200u + 0x10u * (l))

#define RAM_SIZE  0x00100000u
#define EVBA_OFF  0x00004000u
#define CODE_OFF  0x00001000u
#define DATA_OFF  0x00008000u
#define SSTACK    0x00010000u
#define UCODE     0x00002000u
#define USTACK    0x00020000u

/* ---- instruction encodings (Architecture Manual chapter 9) ------------------------- */

#define SPIN  0xC008u                 /* rjmp . */
#define RETE  0xD603u
#define BKPT  0xD673u                 /* breakpoint */
#define TLBW  0xD663u
static uint32_t MOVI(unsigned rd, unsigned k8) { return 0x3000u | (k8 & 0xFF) << 4 | rd; }
static uint32_t LDW(unsigned rd, unsigned rp, unsigned d) { return 0x6000u | rp << 9 | (d >> 2) << 4 | rd; }
static uint32_t STW(unsigned rp, unsigned d, unsigned rs) { return 0x8100u | rp << 9 | (d >> 2) << 4 | rs; }
static uint32_t MFSR(unsigned rd, unsigned a) { return 0xE1B00000u | rd << 16 | a >> 2; }
static uint32_t SLEEP(unsigned op8) { return 0xE9B00000u | op8; }

static void enc(uint32_t w, unsigned len, unsigned op, const char *what)
{
    unsigned got = avr32_decode(len == 2 ? w << 16 : w);
    CHECK(got == op, "%s (%0*x) decodes as op %u, want %u", what, (int)len * 2, (unsigned)w, got, op);
}

static void test_encodings(void)
{
    enc(SPIN, 2, A32_RJMP_I_16, "rjmp .");
    enc(RETE, 2, A32_RETE_16, "rete");
    enc(BKPT, 2, A32_BREAKPOINT_16, "breakpoint");
    enc(TLBW, 2, A32_TLBW_16, "tlbw");
    enc(MOVI(8, 0x55), 2, A32_MOV_RI_16, "mov r8, 0x55");
    enc(LDW(0, 1, 0), 2, A32_LD_W_DISP_16, "ld.w r0, r1[0]");
    enc(LDW(12, 12, 0), 2, A32_LD_W_DISP_16, "ld.w r12, r12[0]");
    EQ64(LDW(12, 12, 0), 0x780C, "ld.w r12, r12[0] is 780c");
    enc(STW(1, 0, 0), 2, A32_ST_W_DISP_16, "st.w r1[0], r0");
    EQ64(STW(12, 0, 11), 0x990B, "st.w r12[0], r11 is 990b");
    enc(MFSR(0, A_EVBA), 4, A32_MFSR_RI_32, "mfsr r0, EVBA");
    enc(SLEEP(0x80), 4, A32_SLEEP_I_32, "sleep 0x80");
}

/* ---- the machine ----------------------------------------------------------------- */

static uint32_t sysr(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, a / 4, &v) == UAM_OK, "read sysreg 0x%x", (unsigned)a);
    return (uint32_t)v;
}
static void sysw(uam_machine *m, uint32_t a, uint32_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, a / 4, v) == UAM_OK, "write sysreg 0x%x", (unsigned)a);
}
static bool arch_b(uam_machine *m) { return (sysr(m, A_CONFIG0) >> 13 & 7) == 1; }
/* The privileged view of the bare machine's memory: P1 on AVR32B. */
static uint32_t base(uam_machine *m) { return arch_b(m) ? 0x80000000u : 0; }
static uint32_t ev(uam_machine *m, uint32_t off) { return base(m) + EVBA_OFF + off; }

/* The module's register file (isa/avr32/avr32.h): R0-R15 at 4 * n. */
static uint32_t r(uam_machine *m, unsigned n) { return (uint32_t)uam_cpu_reg_read(&m->cpu, 4 * n, 32); }
static void setr(uam_machine *m, unsigned n, uint32_t v) { uam_cpu_reg_write(&m->cpu, 4 * n, 32, v); }
static uint32_t put(uam_machine *m, uint32_t a, uint32_t w, unsigned len)
{
    uint8_t b[4];
    for (unsigned i = 0; i < len; i++)
        b[i] = (uint8_t)(w >> (8 * (len - 1 - i)));
    CHECK(uam_bus_load_bytes(&m->bus, a & 0x1FFFFFFFu, b, len) == UAM_OK, "load at 0x%x", (unsigned)a);
    return a + len;
}
static uint32_t p16(uam_machine *m, uint32_t a, uint32_t w) { return put(m, a, w, 2); }
static uint32_t p32(uam_machine *m, uint32_t a, uint32_t w) { return put(m, a, w, 4); }
static uint32_t mem32(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a & 0x1FFFFFFFu, 4, UAM_ACC_BE, &v);
    return (uint32_t)v;
}
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
static bool xlate(uam_machine *m, uint32_t va, uam_xlate_kind k, uint32_t *pa)
{
    uam_xlate x = { 0 };
    bool ok = m->cpu.isa->translate(&m->cpu, va, k, &x) == UAM_OK;
    if (pa)
        *pa = (uint32_t)x.paddr;
    return ok;
}
/* The physical address va reaches with access k, or ~0 for a fault. */
static uint32_t pa_of(uam_machine *m, uint32_t va, uam_xlate_kind k)
{
    uint32_t pa = 0;
    return xlate(m, va, k, &pa) ? pa : 0xFFFFFFFFu;
}

static unsigned av_level, av_asked;
static bool av_wide;   /* answer 0x3FFE: every autovector bit a halfword address can have */
static uint32_t autovector(void *ctx, unsigned level)
{
    (void)ctx;
    av_level = level;
    av_asked++;
    return (av_wide ? 0x3FFEu : VEC_INT(level)) | 0xC000u;   /* bits above 13 are not the autovector's */
}

/* A machine of model cfg, reset (SR as reset leaves it), every event spinning, SP the
 * system stack. */
static void machine(uam_machine *m, const uam_avr32_config *cfg, bool jit)
{
    CHECK(uam_machine_init(m, &uam_isa_avr32) == UAM_OK, "machine init");
    CHECK(uam_avr32_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, RAM_SIZE) == UAM_OK, "map RAM");
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    uam_avr32_set_autovector(&m->cpu, autovector, NULL);
    uam_machine_reset(m, (cfg->arch == UAM_AVR32_B ? 0x80000000u : 0) + CODE_OFF);
    static uint8_t table[0x400];
    for (size_t i = 0; i < sizeof(table); i += 2) {
        table[i] = SPIN >> 8;
        table[i + 1] = SPIN & 0xFF;
    }
    CHECK(uam_bus_load_bytes(&m->bus, EVBA_OFF, table, sizeof(table)) == UAM_OK, "the event table");
    sysw(m, A_EVBA, ev(m, 0));
    setr(m, 13, base(m) + SSTACK);
}

/* The end of a test that executed code: under the JIT, it ran natively. */
static void done(uam_machine *m, bool jit)
{
    if (jit)
        CHECK(m->exec.stats.blocks_native > 0, "the JIT ran the code");
    uam_machine_fini(m);
}

static const uam_avr32_config *const models[] = { &uam_avr32a_ref, &uam_avr32b_ref };
static const unsigned saved_regs[6] = { 8, 9, 10, 11, 12, 14 };   /* R8-R12, LR */

/* ---- models and reset -------------------------------------------------------------- */

static void test_models(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_avr32) == UAM_OK, "init");
    CHECK(uam_avr32_is(m.cpu.isa) && !uam_avr32_is(&uam_isa_z80), "the descriptor is AVR32's");
    uam_machine_reset(&m, 0x80000000u);
    EQ64(sysr(&m, A_CONFIG0) >> 13 & 7, 0, "without uam_avr32_configure the hart is an AVR32A");
    CHECK(uam_avr32_configure(&m.cpu, NULL) == UAM_ERR_INVALID, "configure needs a model");
    uam_avr32_config b;
#define BAD(base_, stmt, what) do {                                                \
        b = *(base_);                                                              \
        stmt;                                                                      \
        CHECK(uam_avr32_configure(&m.cpu, &b) == UAM_ERR_INVALID, "refused: %s", what); \
    } while (0)
    BAD(&uam_avr32a_ref, b.arch = 2, "architecture 2");
    BAD(&uam_avr32a_ref, b.mmu_entries = 4, "an MPU of 4 regions");
    BAD(&uam_avr32b_ref, b.mmu_entries = 65, "65 TLB entries");
    BAD(&uam_avr32a_ref, b.simd = true, "SIMD on AVR32A");
#undef BAD
    EQ64(sysr(&m, A_CONFIG0) >> 13 & 7, 0, "the model is unchanged");
    b = uam_avr32b_ref;
    b.mmu_entries = 64;
    CHECK(uam_avr32_configure(&m.cpu, &b) == UAM_OK, "64 TLB entries");
    uam_machine_fini(&m);

    CHECK(uam_machine_init(&m, &uam_isa_z80) == UAM_OK, "a Z80");
    CHECK(uam_avr32_configure(&m.cpu, &uam_avr32a_ref) == UAM_ERR_INVALID, "an AVR32 model on a Z80");
    CHECK(uam_avr32_compare_line(&m.cpu) == NULL, "a Z80 has no COMPARE line");
    uam_avr32_set_autovector(&m.cpu, autovector, NULL);   /* ignored, no crash */
    uam_machine_fini(&m);

    for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++) {
        const bool B = models[i]->arch == UAM_AVR32_B;
        const uint32_t rst = B ? 0xA0000000u : 0x80000000u;
        machine(&m, models[i], false);
        setr(&m, 8, 0x1234);
        sysw(&m, A_COMPARE, 77);
        line(&m, UAM_AVR32_IRQ_INT2, 1);
        uam_machine_reset(&m, rst);
        EQ64(m.cpu.pc, rst, "at the entry");
        EQ64(sysr(&m, A_SR), SR_GM | SR_EM | MODE(M_SUP), "SR: supervisor mode, GM and EM");
        EQ64(sysr(&m, A_EVBA), 0, "EVBA 0");
        EQ64(sysr(&m, A_COUNT) | sysr(&m, A_COMPARE) | sysr(&m, A_ECR), 0, "COUNT, COMPARE, ECR 0");
        EQ64(r(&m, 8), 0, "the registers cleared");
        EQ64(sysr(&m, A_MMUCR), B ? 0x10 : 0, "MMUCR: AVR32B segmented, paging off");
        const uint32_t c0 = sysr(&m, A_CONFIG0);
        EQ64(c0 >> 13 & 7, B, "CONFIG0.AT");
        EQ64(c0 >> 10 & 7, 2, "CONFIG0.AR: revision 2");
        EQ64(c0 >> 7 & 7, B ? 1 : 3, "CONFIG0.MMUT: ITLB and DTLB (B), MPU (A)");
        EQ64(c0 & 7, B ? 7 : 3, "CONFIG0.S (B only), D, R");
        if (B)
            EQ64(sysr(&m, A_CONFIG1) >> 20, 31u << 6 | 31u, "CONFIG1: IMMUSZ, DMMUSZ 31 (32 entries)");
        /* the input lines and the board's autovector outlive the reset */
        sysw(&m, A_SR, MODE(M_SUP));
        sysw(&m, A_EVBA, ev(&m, 0));
        CHECK(take(&m), "the INT2 line held across the reset is taken");
        EQ64(m.cpu.pc, ev(&m, VEC_INT(2)), "through the board's autovector");
        uam_machine_fini(&m);
    }

    /* an AVR32A without an MPU: addresses are physical, and fixed */
    b = uam_avr32a_ref;
    b.name = "avr32a-nompu";
    b.mmu_entries = 0;
    machine(&m, &b, false);
    EQ64(sysr(&m, A_CONFIG0) >> 7 & 7, 0, "no MPU: MMUT 0");
    uam_xlate x = { 0 };
    CHECK(m.cpu.isa->translate(&m.cpu, 0x12345678u, UAM_XLATE_STORE, &x) == UAM_OK && x.fixed
          && x.paddr == 0x12345678u, "no MPU: identity, fixed");
    uam_machine_fini(&m);
}

/* ---- interrupts through the board's API ---------------------------------------------- */

/* The autovector is bits 13:0 of what the board answers, all of them. */
static void test_autovector_bits(const uam_avr32_config *cfg)
{
    uam_machine m;
    machine(&m, cfg, false);
    sysw(&m, A_SR, MODE(M_SUP));
    line(&m, UAM_AVR32_IRQ_INT0, 1);
    av_wide = true;
    CHECK(take(&m), "%s: INT0 taken", cfg->name);
    av_wide = false;
    EQ64(m.cpu.pc, ev(&m, 0x3FFE), "EVBA + autovector 0x3FFE (bits 13:1)");
    uam_machine_fini(&m);
}

static void test_irq(const uam_avr32_config *cfg)
{
    uam_machine m;
    machine(&m, cfg, false);
    const bool B = arch_b(&m), shadow = B && cfg->int3_shadow;
    const uint32_t S = base(&m) + SSTACK, pc0 = (uint32_t)m.cpu.pc;
    const uint32_t all = IM(0) | IM(1) | IM(2) | IM(3);
    for (unsigned i = 0; i < 6; i++)
        setr(&m, saved_regs[i], 0x80 + saved_regs[i]);
    sysw(&m, A_SR, SR_GM | MODE(M_SUP) | SR_C);
    line(&m, UAM_AVR32_IRQ_INT0, 1);
    CHECK(!take(&m), "GM masks INT0");
    EQ64(lines_taken(&m), 0, "no line reported in supervisor mode");
    sysw(&m, A_SR, MODE(M_SUP) | SR_C);
    av_asked = 0;
    CHECK(wake(&m) && take(&m), "INT0 wakes and is taken");
    CHECK(av_asked == 1 && av_level == 0, "the autovector asked for level 0");
    EQ64(m.cpu.pc, ev(&m, VEC_INT(0)), "INT0: EVBA + the autovector's bits 13:0");
    EQ64(sysr(&m, A_SR), MODE(M_INT0) | IM(0) | SR_C, "INT0 mode, I0M set, GM and EM not");
    EQ64(lines_taken(&m), 1u << 0, "line INT0 reported");
    if (B) {
        EQ64(sysr(&m, RSR(M_INT0)), MODE(M_SUP) | SR_C, "RSR_INT0");
        EQ64(sysr(&m, RAR(M_INT0)), pc0, "RAR_INT0");
        EQ64(r(&m, 13), S, "AVR32B stacks nothing");
    } else {
        EQ64(r(&m, 13), S - 32, "AVR32A: eight words on the system stack");
        EQ64(mem32(&m, S - 32), MODE(M_SUP) | SR_C, "stacked SR");
        EQ64(mem32(&m, S - 28), pc0, "stacked PC");
        for (unsigned i = 0; i < 6; i++)
            EQ64(mem32(&m, S - 24 + 4 * i), 0x80 + saved_regs[5 - i], "stacked LR, R12 .. R8");
    }
    CHECK(!take(&m), "I0M masks INT0 in its handler");

    line(&m, UAM_AVR32_IRQ_INT1, 1);
    CHECK(take(&m), "INT1 interrupts the INT0 handler");
    EQ64(m.cpu.pc, ev(&m, VEC_INT(1)), "INT1's autovector");
    EQ64(sysr(&m, A_SR), MODE(M_INT1) | IM(0) | IM(1) | SR_C, "INT1 masks its level and those below");
    EQ64(lines_taken(&m), 1u << 1, "line INT1 reported");
    if (B) {
        EQ64(sysr(&m, RSR(M_INT1)), MODE(M_INT0) | IM(0) | SR_C, "RSR_INT1: the INT0 handler's SR");
        EQ64(sysr(&m, RAR(M_INT1)), ev(&m, VEC_INT(0)), "RAR_INT1");
        EQ64(sysr(&m, RSR(M_INT0)), MODE(M_SUP) | SR_C, "RSR_INT0 kept");
    } else {
        EQ64(r(&m, 13), S - 64, "a second frame");
        EQ64(mem32(&m, S - 64), MODE(M_INT0) | IM(0) | SR_C, "its SR");
        EQ64(mem32(&m, S - 60), ev(&m, VEC_INT(0)), "its PC");
    }

    line(&m, UAM_AVR32_IRQ_INT2, 1);
    line(&m, UAM_AVR32_IRQ_INT3, 1);
    CHECK(take(&m), "INT3 and INT2 pending: INT3 taken");
    EQ64(m.cpu.pc, ev(&m, VEC_INT(3)), "INT3's autovector");
    EQ64(sysr(&m, A_SR), MODE(M_INT3) | all | SR_C, "INT3 masks the four levels");
    EQ64(lines_taken(&m), 1u << 3, "line INT3 reported");
    CHECK(!take(&m) && !wake(&m), "INT2 waits");
    CHECK((r(&m, 8) == 0x88 && r(&m, 14) == 0x8E) == !shadow,
          shadow ? "INT3: R8-R12 and LR are its shadow registers" : "INT3: R8 and LR as they were");

    /* NMI: not maskable */
    sysw(&m, A_SR, sysr(&m, A_SR) | SR_GM);
    line(&m, UAM_AVR32_IRQ_NMI, 1);
    av_asked = 0;
    CHECK(take(&m), "NMI taken with GM and every IxM set");
    EQ64(av_asked, 0, "no autovector for the NMI");
    EQ64(m.cpu.pc, ev(&m, EV_NMI), "NMI: EVBA + 0x10");
    EQ64(sysr(&m, A_SR), MODE(M_NMI) | all | SR_GM | SR_EM | SR_C, "NMI mode, EM and GM set");
    EQ64(lines_taken(&m), 1u << UAM_AVR32_IRQ_NMI, "line NMI reported");
    if (B) {
        EQ64(sysr(&m, RSR(M_NMI)), MODE(M_INT3) | all | SR_GM | SR_C, "RSR_NMI");
        EQ64(sysr(&m, RAR(M_NMI)), ev(&m, VEC_INT(3)), "RAR_NMI");
        EQ64(r(&m, 8), 0x88, "leaving INT3: R8 of the normal bank");
    } else {
        EQ64(r(&m, 13), S - 104, "AVR32A NMI: SR and PC only");
        EQ64(mem32(&m, S - 104), MODE(M_INT3) | all | SR_GM | SR_C, "stacked SR");
        EQ64(mem32(&m, S - 100), ev(&m, VEC_INT(3)), "stacked PC");
    }
    CHECK(!take(&m), "the NMI is not taken again in its handler");

    /* a level masked by its IxM alone: a lower one is taken, the entry's masks add up */
    line(&m, UAM_AVR32_IRQ_NMI, 0);
    line(&m, UAM_AVR32_IRQ_INT3, 0);
    line(&m, UAM_AVR32_IRQ_INT2, 0);
    sysw(&m, A_SR, MODE(M_SUP) | IM(1));
    CHECK(take(&m), "INT1 masked: INT0 taken");
    EQ64(sysr(&m, A_SR), MODE(M_INT0) | IM(0) | IM(1), "I1M kept, I0M set");
    line(&m, UAM_AVR32_IRQ_NMI, 1);
    CHECK(take(&m), "NMI from the INT0 handler");
    EQ64(sysr(&m, A_SR), MODE(M_NMI) | IM(0) | IM(1) | SR_GM | SR_EM, "the NMI sets GM and EM");
    uam_machine_fini(&m);
}

/* Interrupted application code, rete back to it; AVR32B's INT3 bank across rete. */
static void test_rete(const uam_avr32_config *cfg, bool jit)
{
    uam_machine m;
    machine(&m, cfg, jit);
    const bool B = arch_b(&m);
    const uint32_t S = base(&m) + SSTACK, code = base(&m) + CODE_OFF;
    sysw(&m, A_SR, MODE(M_APP) | SR_C);
    EQ64(r(&m, 13), 0, "application mode: SP_APP");
    setr(&m, 13, USTACK);
    for (unsigned i = 0; i < 6; i++)
        setr(&m, saved_regs[i], 0x80 + saved_regs[i]);
    p16(&m, UCODE, SPIN);
    m.cpu.pc = UCODE;
    line(&m, UAM_AVR32_IRQ_INT1, 1);
    CHECK(take(&m), "INT1 from application mode");
    line(&m, UAM_AVR32_IRQ_INT1, 0);
    EQ64(r(&m, 13), B ? S : S - 32, "the handler runs on SP_SYS");
    if (B) {
        EQ64(sysr(&m, RSR(M_INT1)), MODE(M_APP) | SR_C, "RSR_INT1");
        sysw(&m, RSR(M_INT1), MODE(M_APP) | SR_C | SR_L);
    } else {
        EQ64(mem32(&m, S - 32), MODE(M_APP) | SR_C, "stacked SR on the system stack");
        EQ64(mem32(&m, S - 28), UCODE, "stacked PC");
        p32(&m, S - 32, MODE(M_APP) | SR_C | SR_L);
    }
    /* the handler: R8, R9 changed, rete */
    p16(&m, p16(&m, p16(&m, ev(&m, VEC_INT(1)), MOVI(8, 0x55)), MOVI(9, 0x66)), RETE);
    uam_exec_run(&m.exec, 10);
    EQ64(m.cpu.pc, UCODE, "rete: back at the interrupted instruction");
    EQ64(sysr(&m, A_SR), MODE(M_APP) | SR_C, "SR restored, L cleared");
    EQ64(r(&m, 13), USTACK, "SP_APP again");
    EQ64(r(&m, 8), B ? 0x55 : 0x88, B ? "AVR32B: R8 as the handler left it" : "AVR32A: R8 popped");
    EQ64(r(&m, 9), B ? 0x66 : 0x89, "R9");
    EQ64(r(&m, 14), 0x8E, "LR");
    sysw(&m, A_SR, MODE(M_SUP));
    EQ64(r(&m, 13), S, "SP_SYS: the frame popped");

    if (B && cfg->int3_shadow) {
        setr(&m, 8, 0x11);
        p16(&m, p16(&m, ev(&m, VEC_INT(3)), MOVI(8, 0x77)), RETE);
        p16(&m, code, SPIN);
        m.cpu.pc = code;
        line(&m, UAM_AVR32_IRQ_INT3, 1);
        CHECK(take(&m), "INT3");
        line(&m, UAM_AVR32_IRQ_INT3, 0);
        CHECK(r(&m, 8) != 0x11, "INT3: the shadow R8");
        uam_exec_run(&m.exec, 10);
        EQ64(m.cpu.pc, code, "rete from INT3");
        EQ64(r(&m, 8), 0x11, "the normal R8 again");
        line(&m, UAM_AVR32_IRQ_INT3, 1);
        CHECK(take(&m), "INT3 again");
        EQ64(r(&m, 8), 0x77, "the shadow R8 kept what the handler wrote");
    }
    done(&m, jit);
}

/* ---- exceptions ------------------------------------------------------------------------ */

static void test_exceptions(const uam_avr32_config *cfg, bool jit)
{
    uam_machine m;
    machine(&m, cfg, jit);
    const bool B = arch_b(&m);
    const uint32_t S = base(&m) + SSTACK, code = base(&m) + CODE_OFF, x = base(&m) + 0x3000;
    p16(&m, p16(&m, code, LDW(0, 1, 0)), SPIN);
    setr(&m, 1, base(&m) + DATA_OFF + 2);
    /* EM set, as after reset */
    run(&m, code, 10);
    EQ64(m.cpu.pc, ev(&m, EV_UNREC), "an exception while EM is set: unrecoverable");
    EQ64(sysr(&m, A_ECR), 0, "ECR 0");
    EQ64(MODE_OF(sysr(&m, A_SR)), M_EX, "exception mode");
    EQ64(B ? sysr(&m, RAR(M_EX)) : mem32(&m, S - 4), code, "the faulting instruction saved");

    sysw(&m, A_SR, MODE(M_SUP) | IM(2) | SR_C);
    setr(&m, 13, S);
    run(&m, code, 10);
    EQ64(m.cpu.pc, ev(&m, EV_ADDR_R), "misaligned ld.w: data address (read)");
    EQ64(sysr(&m, A_ECR), EV_ADDR_R >> 2, "ECR");
    EQ64(sysr(&m, A_SR), MODE(M_EX) | SR_EM | SR_GM | IM(2) | SR_C, "EX mode, EM and GM set, the rest kept");
    EQ64(lines_taken(&m), 0, "no interrupt line reported");
    if (B) {
        EQ64(sysr(&m, RSR(M_EX)), MODE(M_SUP) | IM(2) | SR_C, "RSR_EX");
        EQ64(sysr(&m, RAR(M_EX)), code, "RAR_EX");
        EQ64(r(&m, 13), S, "nothing stacked");
        sysw(&m, RAR(M_EX), code + 2);
    } else {
        EQ64(r(&m, 13), S - 8, "SR and PC stacked, not R8-R12 and LR");
        EQ64(mem32(&m, S - 8), MODE(M_SUP) | IM(2) | SR_C, "stacked SR");
        EQ64(mem32(&m, S - 4), code, "stacked PC");
        p32(&m, S - 4, code + 2);
    }
    p16(&m, x, RETE);
    run(&m, x, 10);
    EQ64(m.cpu.pc, code + 2, "rete: at the address saved");
    EQ64(sysr(&m, A_SR), MODE(M_SUP) | IM(2) | SR_C, "SR restored");
    EQ64(r(&m, 13), S, "SP");

    /* from application mode: the frame on the system stack */
    p16(&m, p32(&m, UCODE, MFSR(0, A_EVBA)), SPIN);
    sysw(&m, A_SR, MODE(M_APP));
    setr(&m, 13, USTACK);
    run(&m, UCODE, 10);
    EQ64(m.cpu.pc, ev(&m, EV_PRIV), "mfsr in application mode: privilege violation");
    EQ64(sysr(&m, A_ECR), EV_PRIV >> 2, "ECR");
    EQ64(r(&m, 13), B ? S : S - 8, "the handler on SP_SYS");
    EQ64(B ? sysr(&m, RSR(M_EX)) : mem32(&m, S - 8), MODE(M_APP), "saved SR: application mode");
    EQ64(B ? sysr(&m, RAR(M_EX)) : mem32(&m, S - 4), UCODE, "saved PC");
    setr(&m, 13, S);
    p16(&m, UCODE + 0x10, RETE);
    sysw(&m, A_SR, MODE(M_APP));
    run(&m, UCODE + 0x10, 10);
    EQ64(m.cpu.pc, ev(&m, EV_PRIV), "rete in application mode: privilege violation");
    EQ64(B ? sysr(&m, RAR(M_EX)) : mem32(&m, S - 4), UCODE + 0x10, "at the rete");

    /* breakpoint: debug entry, a no-op while DM is set */
    p16(&m, p16(&m, x + 0x10, BKPT), SPIN);
    setr(&m, 13, S);
    sysw(&m, A_SR, MODE(M_SUP) | SR_DM);
    run(&m, x + 0x10, 10);
    EQ64(m.cpu.pc, x + 0x12, "breakpoint with DM set: nothing");
    sysw(&m, A_SR, MODE(M_SUP) | SR_C);
    run(&m, x + 0x10, 10);
    EQ64(m.cpu.pc, ev(&m, EV_BKPT), "breakpoint: EVBA + 0x1C");
    EQ64(sysr(&m, A_RAR_DBG), x + 0x10, "RAR_DBG: the breakpoint");
    EQ64(sysr(&m, A_RSR_DBG), MODE(M_SUP) | SR_C, "RSR_DBG");
    EQ64(sysr(&m, A_SR) & (SR_D | SR_DM), SR_D | SR_DM, "debug state, debug masked");
    EQ64(r(&m, 13), S, "nothing stacked");
    done(&m, jit);
}

/* ---- SLEEP --------------------------------------------------------------------------- */

static void test_sleep(const uam_avr32_config *cfg, bool jit)
{
    uam_machine m;
    machine(&m, cfg, jit);
    const bool B = arch_b(&m);
    const uint32_t code = base(&m) + CODE_OFF;
    p16(&m, p32(&m, code, SLEEP(0)), SPIN);
    sysw(&m, A_SR, MODE(M_SUP) | IM(0));
    line(&m, UAM_AVR32_IRQ_INT0, 1);
    run(&m, code, 20);
    CHECK(m.cpu.waiting, "sleep: waiting");
    EQ64(m.cpu.pc, code + 4, "after the sleep");
    CHECK(!wake(&m), "INT0, masked by I0M, does not wake it");
    line(&m, UAM_AVR32_IRQ_INT1, 1);
    CHECK(wake(&m), "INT1 does");
    uam_exec_run(&m.exec, 20);
    CHECK(!m.cpu.waiting, "awake");
    EQ64(m.cpu.pc, ev(&m, VEC_INT(1)), "in the INT1 handler");
    EQ64(B ? sysr(&m, RAR(M_INT1)) : mem32(&m, base(&m) + SSTACK - 28), code + 4, "returning after the sleep");
    done(&m, jit);

    /* sleep 0x80 clears GM as it starts waiting */
    machine(&m, cfg, jit);
    p16(&m, p32(&m, code, SLEEP(0x80)), SPIN);
    sysw(&m, A_SR, MODE(M_SUP) | SR_GM);
    run(&m, code, 20);
    CHECK(m.cpu.waiting, "sleep 0x80: waiting");
    EQ64(sysr(&m, A_SR), MODE(M_SUP), "sleep 0x80: GM cleared");
    line(&m, UAM_AVR32_IRQ_INT0, 1);
    uam_exec_run(&m.exec, 20);
    EQ64(m.cpu.pc, ev(&m, VEC_INT(0)), "INT0 taken");
    done(&m, jit);
}

/* ---- COUNT and COMPARE --------------------------------------------------------------- */

static void test_timer(void)
{
    uam_machine m;
    machine(&m, &uam_avr32a_ref, false);
    const uam_signal *cl = uam_avr32_compare_line(&m.cpu);
    CHECK(cl && !cl->level, "the COMPARE request: low");
    tick(&m, 100);
    EQ64(sysr(&m, A_COUNT), 100, "COUNT counts virtual time");
    CHECK(!cl->level, "COMPARE 0: no request");
    sysw(&m, A_COUNT, 1000);
    tick(&m, 5);
    EQ64(sysr(&m, A_COUNT), 1005, "COUNT written");
    sysw(&m, A_COMPARE, 1010);
    tick(&m, 4);
    CHECK(!cl->level, "not before COUNT == COMPARE");
    tick(&m, 1);
    CHECK(cl->level, "requested at COUNT == COMPARE");
    EQ64(sysr(&m, A_COUNT), 0, "COUNT cleared by the match");
    tick(&m, 3);
    CHECK(cl->level, "the request stays");
    sysw(&m, A_COMPARE, 1010);
    CHECK(!cl->level, "writing COMPARE clears it");
    tick(&m, 1006);
    CHECK(!cl->level, "COUNT 1009");
    tick(&m, 1);
    CHECK(cl->level, "the next match, COMPARE ticks after the last");
    sysw(&m, A_COMPARE, 0);
    tick(&m, 5000);
    CHECK(!cl->level, "COMPARE 0 stops it");
    sysw(&m, A_COUNT, 0);
    sysw(&m, A_COMPARE, 5);
    tick(&m, 5);
    CHECK(cl->level, "requested");
    uam_machine_reset(&m, CODE_OFF);
    CHECK(!cl->level, "reset clears the request");
    uam_machine_fini(&m);
}

/* ---- the avr32-ref board's interrupt controller --------------------------------------- */

#define INTC 0xFFFF0800u
#define IPR(g) (INTC + 4u * (g))
#define IRR(g) (INTC + 0x100u + 4u * (g))
#define ICR(l) (INTC + 0x200u + 4u * (3 - (l)))

static uint32_t rd32(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    CHECK(uam_bus_read(&m->bus, a, 4, UAM_ACC_BE, &v) == UAM_OK, "read 0x%x", (unsigned)a);
    return (uint32_t)v;
}
static void wr32(uam_machine *m, uint32_t a, uint32_t v) { uam_bus_write(&m->bus, a, 4, UAM_ACC_BE, v); }

static void test_board(void)
{
    for (int b = 0; b < 2; b++) {
        const uam_board_option opt = { "model", "b" };
        const uam_board_config cfg = { .options = b ? &opt : NULL, .n_options = b ? 1u : 0u };
        uam_machine m;
        CHECK(uam_board_avr32_ref.build(&m, &cfg) == UAM_OK, "build avr32-ref");
        EQ64(m.cpu.pc, b ? 0xA0000000u : 0x80000000u, "the reset address");
        EQ64(arch_b(&m), b, "-o model");
        uam_device *intc = uam_machine_find_device(&m, "intc");
        CHECK(intc != NULL, "the controller");
        if (!intc) {
            uam_machine_fini(&m);
            continue;
        }
        const uint32_t pc0 = (uint32_t)m.cpu.pc, evba = 0x80000000u;
        wr32(&m, IPR(3), 0xFFFFFFFFu);
        EQ64(rd32(&m, IPR(3)), 0xC0003FFFu, "IPR: INTLEVEL and AUTOVECTOR");
        wr32(&m, IPR(3), 1u << 30 | 0x130);
        wr32(&m, IPR(5), 1u << 30 | 0x150);
        wr32(&m, IPR(2), 3u << 30 | 0x120);
        uam_avr32_intc_input(intc, 5 * 32 + 7, 1);
        EQ64(rd32(&m, IRR(5)), 1u << 7, "IRR5: its line 7");
        wr32(&m, IRR(5), 0);
        EQ64(rd32(&m, IRR(5)), 1u << 7, "IRR is read-only");
        uam_avr32_intc_input(intc, 3 * 32, 1);
        EQ64(rd32(&m, ICR(1)), 3, "ICR1: the lowest-numbered group requesting level 1");
        EQ64(rd32(&m, ICR(3)), 0, "ICR3: none");
        sysw(&m, A_EVBA, evba);
        sysw(&m, A_SR, MODE(M_SUP));
        CHECK(take(&m), "the level-1 output drives INT1");
        EQ64(m.cpu.pc, evba + 0x130, "group 3's autovector");
        EQ64(MODE_OF(sysr(&m, A_SR)), M_INT1, "INT1 mode");
        uam_avr32_intc_input(intc, 3 * 32, 0);
        EQ64(rd32(&m, ICR(1)), 5, "then group 5");
        uam_avr32_intc_input(intc, 2 * 32 + 31, 1);
        CHECK(take(&m), "level 3 interrupts the level-1 handler");
        EQ64(m.cpu.pc, evba + 0x120, "group 2's autovector");
        /* COUNT == COMPARE is group 0, line 0 */
        sysw(&m, A_COUNT, 0);
        sysw(&m, A_COMPARE, 10);
        tick(&m, 10);
        EQ64(rd32(&m, IRR(0)), 1, "COMPARE: group 0, line 0");
        sysw(&m, A_COMPARE, 0);
        EQ64(rd32(&m, IRR(0)), 0, "writing COMPARE withdraws it");
        /* the controller's snapshot and reset */
        uam_snapshot *s = NULL;
        CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");
        uam_machine_reset(&m, pc0);
        EQ64(rd32(&m, IPR(2)), 0, "reset clears the priorities");
        EQ64(rd32(&m, IRR(2)), 1u << 31, "the requests are the inputs': they stay");
        CHECK(s && uam_snapshot_restore(&m, s) == UAM_OK, "restore");
        EQ64(rd32(&m, IPR(2)), 3u << 30 | 0x120, "IPR2 restored");
        EQ64(MODE_OF(sysr(&m, A_SR)), M_INT3, "the processor's mode restored");
        uam_snapshot_free(s);
        uam_machine_fini(&m);
    }
}

/* ---- the MPU (AVR32A) ------------------------------------------------------------------ */

static void test_mpu(bool jit)
{
    uam_machine m;
    machine(&m, &uam_avr32a_ref, jit);
    const uint32_t NO = 0xFFFFFFFFu;
    EQ64(pa_of(&m, 0x00050000u, UAM_XLATE_STORE), 0x00050000u, "MPU off: everything accessible");
    sysw(&m, A_MPUAR0 + 28, 0xFFFFFFFFu);
    EQ64(sysr(&m, A_MPUAR0 + 28), 0xFFFFF03Fu, "MPUAR7: BASE, SIZE, V");
    sysw(&m, A_MPUAR0 + 28, 0);
    sysw(&m, A_MPUPSR0 + 28, 0xFFFFFFFFu);
    EQ64(sysr(&m, A_MPUPSR0 + 28), 0xFFFF, "MPUPSR7: 16 subregions");
    sysw(&m, A_MPUPSR0 + 28, 0);
    /* 0: 64 KB at 0, RWX for both; 1: 4 KB at 0x20000, privileged RW, application R;
     * 2: 64 KB at 0x40000, privileged R only, but subregion 3 (MPUAPRB) RW for both;
     * 3: 4 KB at 0x60000, not valid */
    sysw(&m, A_MPUAR0 + 0, 0x00000000u | 15u << 1 | 1);
    sysw(&m, A_MPUAR0 + 4, 0x00020000u | 11u << 1 | 1);
    sysw(&m, A_MPUAR0 + 8, 0x00040000u | 15u << 1 | 1);
    sysw(&m, A_MPUAR0 + 12, 0x00060000u | 11u << 1);
    sysw(&m, A_MPUPSR0 + 8, 1u << 3);
    sysw(&m, A_MPUAPRA, 7u | 8u << 4 | 0u << 8 | 7u << 12);
    sysw(&m, A_MPUAPRB, 6u << 8);
    sysw(&m, A_MPUCR, 1);
    EQ64(pa_of(&m, 0x00020010u, UAM_XLATE_STORE), 0x00020010u, "AP 8: privileged store");
    EQ64(pa_of(&m, 0x00020010u, UAM_XLATE_FETCH), NO, "AP 8: no execute");
    EQ64(pa_of(&m, 0x00040010u, UAM_XLATE_LOAD), 0x00040010u, "AP 0: privileged load");
    EQ64(pa_of(&m, 0x00040010u, UAM_XLATE_STORE), NO, "AP 0: no privileged store");
    EQ64(pa_of(&m, 0x00043010u, UAM_XLATE_STORE), 0x00043010u, "subregion 3 (MPUAPRB, AP 6): store");
    EQ64(pa_of(&m, 0x00044000u, UAM_XLATE_STORE), NO, "subregion 4: MPUAPRA");
    EQ64(pa_of(&m, 0x00060000u, UAM_XLATE_LOAD), NO, "an invalid region maps nothing");
    EQ64(pa_of(&m, 0x00060000u, UAM_XLATE_DEBUG), 0x00060000u, "the debugger's view: no MPU");
    sysw(&m, A_SR, MODE(M_APP));
    EQ64(pa_of(&m, 0x00020010u, UAM_XLATE_LOAD), 0x00020010u, "AP 8: application load");
    EQ64(pa_of(&m, 0x00020010u, UAM_XLATE_STORE), NO, "AP 8: no application store");
    EQ64(pa_of(&m, 0x00040010u, UAM_XLATE_LOAD), NO, "AP 0: nothing for application");
    EQ64(pa_of(&m, 0x00043010u, UAM_XLATE_STORE), 0x00043010u, "subregion 3: application store");
    EQ64(pa_of(&m, UCODE, UAM_XLATE_FETCH), UCODE, "region 0: application fetch");
    /* executed in application mode: a load and a store in region 1 */
    p32(&m, 0x00020010u, 0xCAFEF00Du);
    p16(&m, p16(&m, p16(&m, UCODE, LDW(0, 1, 0)), STW(1, 0, 0)), SPIN);
    setr(&m, 1, 0x00020010u);
    setr(&m, 13, USTACK);
    run(&m, UCODE, 10);
    EQ64(r(&m, 0), 0xCAFEF00Du, "application load from AP 8");
    EQ64(m.cpu.pc, ev(&m, EV_DPROT_W), "application store to AP 8: DTLB protection (write)");
    EQ64(sysr(&m, A_ECR), EV_DPROT_W >> 2, "ECR");
    EQ64(sysr(&m, A_TLBEAR), 0x00020010u, "TLBEAR");
    EQ64(mem32(&m, SSTACK - 4), UCODE + 2, "the store's address saved");
    EQ64(mem32(&m, 0x00020010u), 0xCAFEF00Du, "nothing stored");
    done(&m, jit);
}

/* ---- the MMU (AVR32B) ------------------------------------------------------------------ */

/* TLBELO: PFN, G, AP, SZ (0 1 KB, 1 4 KB, 2 64 KB, 3 1 MB), D. */
#define LO(pfn, g, ap, sz) ((pfn) | (uint32_t)(g) << 8 | (uint32_t)(ap) << 4 | (uint32_t)(sz) << 2 | 2u)

/* Write ITLB or DTLB entry idx with tlbw. */
static void tlb_write(uam_machine *m, bool itlb, unsigned idx, uint32_t hi, uint32_t lo)
{
    sysw(m, A_MMUCR, (sysr(m, A_MMUCR) & ~(63u << 26 | 63u << 14)) | idx << (itlb ? 26 : 14));
    sysw(m, A_TLBEHI, hi | 1u << 9 | (itlb ? 1u << 8 : 0));
    sysw(m, A_TLBELO, lo);
    p16(m, p16(m, 0x80003000u, TLBW), SPIN);
    run(m, 0x80003000u, 2);
}

static void test_mmu(bool jit)
{
    uam_machine m;
    machine(&m, &uam_avr32b_ref, jit);
    const uint32_t NO = 0xFFFFFFFFu, code = 0x80000000u + CODE_OFF;
    sysw(&m, A_SR, MODE(M_SUP));
    EQ64(pa_of(&m, 0x80001234u, UAM_XLATE_LOAD), 0x1234, "P1");
    EQ64(pa_of(&m, 0xA0001234u, UAM_XLATE_STORE), 0x1234, "P2");
    EQ64(pa_of(&m, 0xC0001234u, UAM_XLATE_LOAD), 0x1234, "P3 without paging");
    EQ64(pa_of(&m, 0xFFFF0800u, UAM_XLATE_LOAD), 0xFFFF0800u, "P4: untranslated");
    EQ64(pa_of(&m, 0x00001234u, UAM_XLATE_FETCH), 0x1234, "P0 without paging");
    sysw(&m, A_MMUCR, 0);
    EQ64(pa_of(&m, 0x80001234u, UAM_XLATE_LOAD), 0x80001234u, "MMUCR.S = 0: no segments");
    sysw(&m, A_MMUCR, 0x10);
    sysw(&m, A_SR, MODE(M_APP));
    EQ64(pa_of(&m, 0x80001234u, UAM_XLATE_LOAD), NO, "application mode: P1 refused");
    EQ64(pa_of(&m, 0xFFFF0800u, UAM_XLATE_LOAD), NO, "and P4");
    EQ64(pa_of(&m, 0x00001234u, UAM_XLATE_LOAD), 0x1234, "U0 allowed");
    sysw(&m, A_SR, MODE(M_SUP));

    tlb_write(&m, false, 0, 0x00400000u | 5, LO(0x00030000u, 0, 2, 1));   /* ASID 5, 4 KB, priv RW */
    tlb_write(&m, false, 1, 0x00500000u, LO(0x00040000u, 1, 6, 2));       /* global, 64 KB, RW/RW */
    tlb_write(&m, false, 2, 0x00600000u, LO(0x00050400u, 1, 0, 0));       /* global, 1 KB, priv R */
    tlb_write(&m, false, 3, 0x00700000u, LO(0x00200000u, 1, 6, 3));       /* global, 1 MB */
    tlb_write(&m, false, 4, 0x00800000u, LO(0x00060000u, 1, 2, 1));
    tlb_write(&m, false, 5, 0x00800000u, LO(0x00070000u, 1, 2, 1));       /* the same page again */
    tlb_write(&m, true, 0, 0x00900000u, LO(0x00001000u, 1, 5, 1));        /* ITLB: RX for both */
    sysw(&m, A_MMUCR, sysr(&m, A_MMUCR) | 1);
    sysw(&m, A_TLBEHI, 5);
    EQ64(pa_of(&m, 0x00400123u, UAM_XLATE_LOAD), 0x00030123u, "ASID 5: its private page");
    sysw(&m, A_TLBEHI, 6);
    EQ64(pa_of(&m, 0x00400123u, UAM_XLATE_LOAD), NO, "ASID 6: not its page");
    sysw(&m, A_MMUCR, sysr(&m, A_MMUCR) | 2);
    EQ64(pa_of(&m, 0x00400123u, UAM_XLATE_LOAD), 0x00030123u, "MMUCR.M (shared virtual memory): no ASIDs");
    sysw(&m, A_MMUCR, sysr(&m, A_MMUCR) & ~2u);
    EQ64(pa_of(&m, 0x0050ABCDu, UAM_XLATE_LOAD), 0x0004ABCDu, "a global 64 KB page");
    EQ64(pa_of(&m, 0x00600123u, UAM_XLATE_LOAD), 0x00050523u, "a 1 KB page");
    EQ64(pa_of(&m, 0x00600400u, UAM_XLATE_LOAD), NO, "past it");
    EQ64(pa_of(&m, 0x007ABCDEu, UAM_XLATE_STORE), 0x002ABCDEu, "a 1 MB page");
    EQ64(pa_of(&m, 0x00600123u, UAM_XLATE_STORE), NO, "AP 0: read-only");
    EQ64(pa_of(&m, 0x00500000u, UAM_XLATE_FETCH), NO, "the DTLB does not map instructions");
    EQ64(pa_of(&m, 0x00900010u, UAM_XLATE_FETCH), 0x1010, "the ITLB does");
    EQ64(pa_of(&m, 0x00900010u, UAM_XLATE_LOAD), NO, "and not data");
    EQ64(pa_of(&m, 0x00500000u, UAM_XLATE_DEBUG), 0x00040000u, "the debugger: the DTLB");
    EQ64(pa_of(&m, 0x00900010u, UAM_XLATE_DEBUG), 0x1010, "and the ITLB");
    EQ64(pa_of(&m, 0x80001234u, UAM_XLATE_LOAD), 0x1234, "P1 untranslated with paging");
    EQ64(pa_of(&m, 0xC0500000u, UAM_XLATE_LOAD), NO, "P3 paged");
    sysw(&m, A_SR, MODE(M_APP));
    EQ64(pa_of(&m, 0x0050ABCDu, UAM_XLATE_STORE), 0x0004ABCDu, "application: an RW/RW page");
    EQ64(pa_of(&m, 0x00600123u, UAM_XLATE_LOAD), NO, "application: a privileged page");
    sysw(&m, A_SR, MODE(M_SUP));

    run(&m, 0x00A00000u, 10);
    EQ64(m.cpu.pc, ev(&m, EV_ITLB_MISS), "a fetch nothing maps: ITLB miss");
    EQ64(sysr(&m, A_ECR), EV_ITLB_MISS >> 2, "ECR");
    EQ64(sysr(&m, A_TLBEAR), 0x00A00000u, "TLBEAR");
    EQ64(sysr(&m, A_TLBEHI) & 0xFFFFFC00u, 0x00A00000u, "TLBEHI.VPN");
    CHECK(sysr(&m, A_TLBEHI) & 1u << 8, "TLBEHI.I: the ITLB");
    EQ64(sysr(&m, RAR(M_EX)), 0x00A00000u, "RAR_EX");
    sysw(&m, A_SR, MODE(M_SUP));
    p16(&m, p16(&m, code, LDW(0, 1, 0)), SPIN);
    setr(&m, 1, 0x00800000u);
    run(&m, code, 10);
    EQ64(m.cpu.pc, ev(&m, EV_MULTI), "two DTLB entries match: TLB multiple hit");
    EQ64(sysr(&m, A_ECR), EV_MULTI >> 2, "ECR");

    /* application mode reaching for P1 (paging off) */
    sysw(&m, A_MMUCR, sysr(&m, A_MMUCR) & ~1u);
    sysw(&m, A_SR, MODE(M_APP));
    p16(&m, p16(&m, UCODE, LDW(0, 1, 0)), SPIN);
    setr(&m, 1, 0x80008000u);
    run(&m, UCODE, 10);
    EQ64(m.cpu.pc, ev(&m, EV_DPROT_R), "application load from P1: DTLB protection (read)");
    EQ64(sysr(&m, A_ECR), EV_DPROT_R >> 2, "ECR");
    EQ64(sysr(&m, A_TLBEAR), 0x80008000u, "TLBEAR");
    EQ64(sysr(&m, RAR(M_EX)), UCODE, "RAR_EX");
    done(&m, jit);
}

/* ---- the debugger view --------------------------------------------------------------- */

enum { G_SP = 13, G_LR = 14, G_PC = 15, G_SR = 16, G_N = 17 };

static bool dump_has(uam_machine *m, const char *a, const char *b)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    m->cpu.isa->dump_state(&m->cpu, f);
    fclose(f);
    bool ok = buf && strstr(buf, a) && strstr(buf, b);
    free(buf);
    return ok;
}

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, &uam_avr32b_ref, false);
    const uint32_t S = 0x80000000u + SSTACK;
    const uam_gdb_arch *g = m.cpu.isa->gdb;
    CHECK(g && g->n_regs == G_N && g->reg_bytes == 4, "17 registers of 4 bytes");
    CHECK(strstr(g->target_xml, "<architecture>avr32</architecture>") && strstr(g->target_xml, "name=\"sr\""),
          "target description");
    uint64_t v = 0;
    setr(&m, 5, 0x55555555u);
    setr(&m, 14, 0x1414);
    sysw(&m, A_SR, MODE(M_SUP) | SR_GM | SR_Z | SR_C);
    CHECK(g->read_reg(&m.cpu, 5, &v) && v == 0x55555555u, "gdb r5");
    CHECK(g->read_reg(&m.cpu, G_SP, &v) && v == S, "gdb sp");
    CHECK(g->read_reg(&m.cpu, G_LR, &v) && v == 0x1414, "gdb lr");
    CHECK(g->read_reg(&m.cpu, G_PC, &v) && v == m.cpu.pc, "gdb pc");
    CHECK(g->read_reg(&m.cpu, G_SR, &v) && v == (MODE(M_SUP) | SR_GM | SR_Z | SR_C), "gdb sr: flags and the rest");
    CHECK(!g->read_reg(&m.cpu, G_N, &v), "no register 17");
    /* writing SR changes the mode, and with it the SP */
    CHECK(g->write_reg(&m.cpu, G_SR, MODE(M_APP) | SR_N), "gdb write sr");
    EQ64(sysr(&m, A_SR), MODE(M_APP) | SR_N, "SR");
    CHECK(g->read_reg(&m.cpu, G_SP, &v) && v == 0, "gdb sp: SP_APP");
    CHECK(!xlate(&m, 0x80001000u, UAM_XLATE_LOAD, NULL), "application mode in effect: P1 refused");
    CHECK(g->write_reg(&m.cpu, G_SP, USTACK) && g->write_reg(&m.cpu, G_SR, MODE(M_SUP)), "gdb write sp, sr");
    CHECK(g->read_reg(&m.cpu, G_SP, &v) && v == S, "gdb sp: SP_SYS again");
    sysw(&m, A_SR, MODE(M_APP));
    EQ64(r(&m, 13), USTACK, "SP_APP as gdb wrote it");
    sysw(&m, A_SR, MODE(M_SUP));
    CHECK(g->write_reg(&m.cpu, G_PC, 0x80001235u) && m.cpu.pc == 0x80001234u, "gdb pc: halfword aligned");
    CHECK(g->write_reg(&m.cpu, 7, 0x77) && r(&m, 7) == 0x77, "gdb write r7");
    CHECK(!g->write_reg(&m.cpu, G_N, 0), "no register 17 to write");
    CHECK(dump_has(&m, "arch=AVR32B mode=sup", "mmucr=00000010"), "dump: AVR32B, its mode, the MMU");
    uam_machine_fini(&m);

    machine(&m, &uam_avr32a_ref, false);
    sysw(&m, A_SR, MODE(M_INT2));
    CHECK(dump_has(&m, "arch=AVR32A mode=int2", "mpucr="), "dump: AVR32A, its mode, the MPU");
    uam_machine_fini(&m);
}

/* ---- snapshots ------------------------------------------------------------------------- */

static void test_snapshot(const uam_avr32_config *cfg)
{
    uam_machine m;
    machine(&m, cfg, false);
    const bool B = arch_b(&m);
    const uint32_t S = base(&m) + SSTACK;
    if (B) {
        tlb_write(&m, false, 7, 0x00400000u | 7, LO(0x00030000u, 0, 2, 1));
        sysw(&m, A_MMUCR, sysr(&m, A_MMUCR) | 1);
        sysw(&m, A_TLBEHI, 7);
        sysw(&m, RSR(M_INT2), 0x1234);
        sysw(&m, RAR(M_INT2), 0x5678);
    } else {
        sysw(&m, A_MPUAR0, 0x00000000u | 15u << 1 | 1);
        sysw(&m, A_MPUAPRA, 8);
        sysw(&m, A_MPUCR, 1);
    }
    sysw(&m, A_SR, MODE(M_APP));
    setr(&m, 13, USTACK);
    sysw(&m, A_SR, MODE(M_INT1) | IM(0) | IM(1) | SR_C);
    setr(&m, 8, 0x88);
    sysw(&m, A_ACBA, 0x1230);
    sysw(&m, A_ECR, 0x0D);
    sysw(&m, A_COUNT, 100);
    sysw(&m, A_COMPARE, 500);
    line(&m, UAM_AVR32_IRQ_INT3, 1);

    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");
    uam_machine_reset(&m, 0);
    line(&m, UAM_AVR32_IRQ_INT3, 0);
    tick(&m, 50);
    CHECK(s && uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    EQ64(sysr(&m, A_SR), MODE(M_INT1) | IM(0) | IM(1) | SR_C, "SR");
    EQ64(r(&m, 13), S, "SP_SYS");
    EQ64(r(&m, 8), 0x88, "R8");
    EQ64(sysr(&m, A_EVBA), ev(&m, 0), "EVBA");
    EQ64(sysr(&m, A_ACBA), 0x1230, "ACBA");
    EQ64(sysr(&m, A_ECR), 0x0D, "ECR");
    EQ64(sysr(&m, A_COUNT), 100, "COUNT");
    EQ64(sysr(&m, A_COMPARE), 500, "COMPARE");
    if (B) {
        EQ64(pa_of(&m, 0x00400123u, UAM_XLATE_LOAD), 0x00030123u, "the TLB entry, its ASID, paging on");
        EQ64(sysr(&m, RSR(M_INT2)), 0x1234, "RSR_INT2");
        EQ64(sysr(&m, RAR(M_INT2)), 0x5678, "RAR_INT2");
    } else {
        EQ64(pa_of(&m, 0x00000100u, UAM_XLATE_STORE), 0x100, "MPU region 0");
        EQ64(pa_of(&m, 0x00020000u, UAM_XLATE_LOAD), 0xFFFFFFFFu, "the MPU enabled");
    }
    CHECK(take(&m), "the INT3 line");
    EQ64(m.cpu.pc, ev(&m, VEC_INT(3)), "INT3 taken");
    sysw(&m, A_SR, MODE(M_APP));
    EQ64(r(&m, 13), USTACK, "SP_APP");
    tick(&m, 399);
    CHECK(!uam_avr32_compare_line(&m.cpu)->level, "COMPARE not yet");
    tick(&m, 1);
    CHECK(uam_avr32_compare_line(&m.cpu)->level, "the COMPARE match restored");

    uam_machine o;
    machine(&o, B ? &uam_avr32a_ref : &uam_avr32b_ref, false);
    CHECK(!s || uam_snapshot_restore(&o, s) != UAM_OK, "the other microarchitecture refuses it");
    uam_machine_fini(&o);
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

int main(void)
{
    uam_avr32_config b_noshadow = uam_avr32b_ref;
    b_noshadow.name = "avr32b-noshadow";
    b_noshadow.int3_shadow = false;
    test_encodings();
    test_models();
    for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++) {
        test_irq(models[k]);
        test_autovector_bits(models[k]);
    }
    test_irq(&b_noshadow);
    test_timer();
    test_board();
    test_gdb();
    for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++)
        test_snapshot(models[k]);
    for (int jit = 0; jit < 2; jit++) {
        for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++) {
            test_rete(models[k], jit);
            test_exceptions(models[k], jit);
            test_sleep(models[k], jit);
        }
        test_rete(&b_noshadow, jit);
        test_mpu(jit);
        test_mmu(jit);
    }
    printf("test_avr32: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
