/*
 * test_xtensa.c - The Xtensa module's parts the self-test firmware cannot reach on
 * xtensa-ref: model selection and its validation, the reset state of each model, which
 * special registers each model has (RSR/WSR/XSR of a missing one is IllegalInstruction),
 * an option the model lacks, the interrupt lines through the board's API (level lines
 * read as they are, edge and NMI lines latch, INTSET/INTCLEAR, INTENABLE's width, the
 * level chosen, EPCn/EPSn, EXCMLEVEL, the user vector, RFI, the lines reported), WAITI,
 * CCOUNT and CCOMPARE in virtual time, the window exceptions' vector choice and the
 * register file behind A0-A15, bus errors and misaligned accesses per model, BREAK below
 * and at DEBUGLEVEL, the ERI hook, the debugger view, a snapshot of the ISA state, and
 * one program run little- and big-endian (the big-endian layout made here and checked
 * against the decoder's converter).
 *
 * Each test runs on a bare machine: RAM at 0x40000000 (VECBASE), every vector a branch to
 * itself, so an exception or interrupt leaves the hart spinning where it was delivered.
 * The tests that execute code run interpreted and under the JIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../isa/xtensa/xtensa.h"
#include "uam/bus.h"
#include "uam/isa/xtensa.h"
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

#define RAM      0x40000000u
#define RAM_SIZE 0x00100000u
#define CODE     0x40001000u
#define DATA     0x40008000u
#define STACK    0x40080000u

/* Special registers by number. */
enum {
    SR_LBEG = 0, SR_LITBASE = 5, SR_SCOMPARE1 = 12, SR_WB = 72, SR_WS = 73, SR_MEMCTL = 97,
    SR_ATOMCTL = 99, SR_CONFIGID0 = 176, SR_EPC1 = 177, SR_DEPC = 192, SR_CPENABLE = 224,
    SR_INTERRUPT = 226, SR_INTSET = 226, SR_INTCLEAR = 227, SR_INTENABLE = 228, SR_PS = 230,
    SR_VECBASE = 231, SR_EXCCAUSE = 232, SR_DEBUGCAUSE = 233, SR_CCOUNT = 234, SR_PRID = 235,
    SR_EXCVADDR = 238, SR_CCOMPARE0 = 240, SR_MISC0 = 244,
};
#define SR_EPC(n)     (176u + (n))
#define SR_EPS(n)     (192u + (n))
#define SR_EXCSAVE(n) (208u + (n))

/* ---- instruction encodings (the little-endian word) ------------------------------- */

static uint32_t rri8(unsigned op0, unsigned r, unsigned s, unsigned t, unsigned imm8)
{
    return (imm8 & 0xFF) << 16 | r << 12 | s << 8 | t << 4 | op0;
}
static uint32_t RSR(unsigned t, unsigned sr) { return 0x030000u | sr << 8 | t << 4; }
static uint32_t WSR(unsigned t, unsigned sr) { return 0x130000u | sr << 8 | t << 4; }
static uint32_t XSR(unsigned t, unsigned sr) { return 0x610000u | sr << 8 | t << 4; }
static uint32_t MOVI(unsigned t, int imm) { return rri8(2, 0xA, ((unsigned)imm >> 8) & 15, t, (unsigned)imm); }
static uint32_t ADDI(unsigned t, unsigned s, int imm) { return rri8(2, 0xC, s, t, (unsigned)imm); }
static uint32_t L8UI(unsigned t, unsigned s, unsigned off) { return rri8(2, 0x0, s, t, off); }
static uint32_t L16UI(unsigned t, unsigned s, unsigned off) { return rri8(2, 0x1, s, t, off >> 1); }
static uint32_t L32I(unsigned t, unsigned s, unsigned off) { return rri8(2, 0x2, s, t, off >> 2); }
static uint32_t S32I(unsigned t, unsigned s, unsigned off) { return rri8(2, 0x6, s, t, off >> 2); }
static uint32_t ADD(unsigned r, unsigned s, unsigned t) { return 0x800000u | r << 12 | s << 8 | t << 4; }
static uint32_t ADD_N(unsigned r, unsigned s, unsigned t) { return r << 12 | s << 8 | t << 4 | 0xA; }
static uint32_t ENTRY(unsigned s, unsigned frame) { return (frame >> 3) << 12 | s << 8 | 0x36; }
static uint32_t RER(unsigned t, unsigned s) { return 0x406000u | s << 8 | t << 4; }
static uint32_t WER(unsigned t, unsigned s) { return 0x407000u | s << 8 | t << 4; }
static uint32_t ROTW(int n) { return 0x408000u | ((unsigned)n & 15) << 4; }
static uint32_t BREAK(unsigned s, unsigned t) { return 0x004000u | s << 8 | t << 4; }
static uint32_t BREAK_N(unsigned s) { return 0xF02Du | s << 8; }
static uint32_t WAITI(unsigned lv) { return 0x007000u | lv << 8; }
static uint32_t JX(unsigned s) { return 0x0000A0u | s << 8; }
#define RET  0x000080u
#define RETW 0x000090u
#define ILL  0x000000u
/* PC-relative: the offset from the instruction at `pc`. */
static uint32_t J(uint32_t pc, uint32_t to) { return ((to - pc - 4) & 0x3FFFF) << 6 | 0x06; }
static uint32_t CALLn(unsigned n, uint32_t pc, uint32_t to)
{
    return (((to - ((pc & ~3u) + 4)) >> 2) & 0x3FFFF) << 6 | n << 4 | 0x05;
}

/*
 * The big-endian layout of a little-endian word, for the formats used here: the fields
 * in the opposite order, each keeping its bit order (the inverse of xt_be_to_le).
 */
static uint32_t le_to_be(uint32_t w, unsigned len)
{
    unsigned op0 = w & 15, t = (w >> 4) & 15, s = (w >> 8) & 15, r = (w >> 12) & 15;
    unsigned op1 = (w >> 16) & 15, op2 = (w >> 20) & 15;
    unsigned n = t & 3, m = t >> 2, bt = n << 2 | m;   /* t split into n and m */
    if (len == 2)
        return op0 << 12 | t << 8 | s << 4 | r;
    switch (op0) {
    case 0:
        if (op1 == 0 && op2 == 0 && r == 0)                         /* CALLX */
            return bt << 16 | s << 12;
        if ((op1 == 3 && (op2 == 0 || op2 == 1 || op2 == 15)) || (op1 == 1 && op2 == 6))
            return t << 16 | (w & 0xFF00) | op1 << 4 | op2;          /* RSR: sr one piece */
        return t << 16 | s << 12 | r << 8 | op1 << 4 | op2;          /* RRR */
    case 2: case 3: case 7:                                          /* RRI8 */
        return op0 << 20 | t << 16 | s << 12 | r << 8 | (w >> 16);
    case 5:                                                          /* CALL */
        return op0 << 20 | (n << 2) << 16 | (w >> 6);
    case 6:
        if (n == 0)                                                  /* J */
            return op0 << 20 | (w >> 6);
        if (n == 1 || (n == 3 && m == 0))                            /* BRI12: ENTRY */
            return op0 << 20 | bt << 16 | s << 12 | (w >> 12);
        return op0 << 20 | bt << 16 | s << 12 | r << 8 | (w >> 16);  /* BRI8 */
    default:
        return op0 << 20 | t << 16 | s << 12 | r << 8 | op1 << 4 | op2;
    }
}

/* ---- the machine ----------------------------------------------------------------- */

static bool is_be(uam_machine *m) { return m->cpu.isa == &uam_isa_xtensaeb; }

static void code(uam_machine *m, uint32_t a, const uint8_t *b, size_t n)
{
    CHECK(uam_bus_load_bytes(&m->bus, a, b, n) == UAM_OK, "load at 0x%x", (unsigned)a);
}

/* Put an instruction of `len` bytes at a, in the hart's byte order; returns the next. */
static uint32_t put_len(uam_machine *m, uint32_t a, uint32_t w, unsigned len)
{
    uint8_t b[3];
    if (is_be(m)) {
        uint32_t v = le_to_be(w, len);
        CHECK(xt_be_to_le(v, len) == w, "big-endian layout of %06x: %06x goes back to %06x",
              (unsigned)w, (unsigned)v, (unsigned)xt_be_to_le(v, len));
        for (unsigned i = 0; i < len; i++)
            b[i] = (uint8_t)(v >> (8 * (len - 1 - i)));
    } else {
        for (unsigned i = 0; i < len; i++)
            b[i] = (uint8_t)(w >> (8 * i));
    }
    code(m, a, b, len);
    return a + len;
}
static uint32_t put(uam_machine *m, uint32_t a, uint32_t w) { return put_len(m, a, w, 3); }
static uint32_t put_n(uam_machine *m, uint32_t a, uint32_t w) { return put_len(m, a, w, 2); }
/* A branch to itself at a. */
static void spin(uam_machine *m, uint32_t a) { put(m, a, J(a, a)); }

static uint32_t mem32(uam_machine *m, uint64_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, 4, is_be(m) ? UAM_ACC_BE : UAM_ACC_LE, &v);
    return (uint32_t)v;
}
static uint8_t mem8(uam_machine *m, uint64_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}
static uint32_t reg(uam_machine *m, uint32_t off) { return xt_reg(&m->cpu, off); }
static void set(uam_machine *m, uint32_t off, uint32_t v) { xt_set_reg(&m->cpu, off, v); }
static uint32_t a_(uam_machine *m, unsigned n) { return reg(m, XT_A(n)); }
static void seta(uam_machine *m, unsigned n, uint32_t v) { set(m, XT_A(n), v); }
static void sysw(uam_machine *m, uint32_t id, uint32_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "write sysreg %u", (unsigned)id);
}
static uint32_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, id, &v) == UAM_OK, "read sysreg %u", (unsigned)id);
    return (uint32_t)v;
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static uint64_t lines_taken(uam_machine *m) { return m->cpu.isa->irq_lines(&m->cpu); }
/* Run up to n instructions from pc (the programs end spinning). */
static void run(uam_machine *m, uint32_t pc, uint64_t n)
{
    m->cpu.pc = pc;
    uam_exec_run(&m->exec, n);
}
static uint32_t vec(uam_machine *m, unsigned v)
{
    const uam_xtensa_config *c = uam_xtensa_config_of(&m->cpu);
    return c->vecbase + c->vec[v];
}
static bool windowed(uam_machine *m)
{
    return uam_xtensa_config_of(&m->cpu)->options & UAM_XT_OPT_WINDOWED;
}
/* PS as the tests start: no EXCM, level 0, window overflow detection on a windowed core. */
static uint32_t ps0(uam_machine *m) { return windowed(m) ? PS_WOE : 0; }

/* A machine of model cfg, reset at CODE, each vector spinning, PS = ps0, a1 the stack. */
static void machine(uam_machine *m, const uam_isa *isa, const uam_xtensa_config *cfg, bool jit)
{
    CHECK(uam_machine_init(m, isa) == UAM_OK, "machine init");
    CHECK(uam_xtensa_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "ram", RAM, RAM_SIZE) == UAM_OK, "map RAM");
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    uam_machine_reset(m, CODE);
    for (unsigned v = 0; v < UAM_XT_VEC__COUNT; v++)
        spin(m, cfg->vecbase + cfg->vec[v]);
    sysw(m, SR_PS, ps0(m));
    seta(m, 1, STACK);
}

/* The hart took a general exception of `cause` at epc and sits at the kernel vector. */
static void expect_exc(uam_machine *m, unsigned cause, uint32_t epc, const char *what)
{
    CHECK(m->cpu.pc == vec(m, UAM_XT_VEC_KERNEL), "%s: at the kernel vector (pc 0x%llx)", what,
          (unsigned long long)m->cpu.pc);
    CHECK(sysr(m, SR_EXCCAUSE) == cause, "%s: EXCCAUSE %u want %u", what,
          (unsigned)sysr(m, SR_EXCCAUSE), cause);
    CHECK(sysr(m, SR_EPC1) == epc, "%s: EPC1 0x%x want 0x%x", what, (unsigned)sysr(m, SR_EPC1),
          (unsigned)epc);
    CHECK(sysr(m, SR_PS) & PS_EXCM, "%s: PS.EXCM set", what);
}

static const uam_xtensa_config *const models[] = { &uam_xtensa_lx106, &uam_xtensa_lx6, &uam_xtensa_lx7 };

/* ---- models and reset -------------------------------------------------------------- */

static void test_models(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_xtensa) == UAM_OK, "init");
    CHECK(uam_xtensa_is(m.cpu.isa) && uam_xtensa_is(&uam_isa_xtensaeb), "both descriptors are Xtensa");
    const uam_xtensa_config *c = uam_xtensa_config_of(&m.cpu);
    CHECK(c && strcmp(c->name, "lx6") == 0, "without uam_xtensa_configure the hart is an LX6");
    CHECK(uam_xtensa_configure(&m.cpu, NULL) == UAM_ERR_INVALID, "configure needs a model");

    /* inconsistent models are refused, and the one in effect stays */
    uam_xtensa_config b;
#define BAD(base, stmt, what) do {                                                 \
        b = *(base);                                                               \
        stmt;                                                                      \
        CHECK(uam_xtensa_configure(&m.cpu, &b) == UAM_ERR_INVALID, "refused: %s", what); \
    } while (0)
    BAD(&uam_xtensa_lx6, b.nareg = 16, "windows with 16 registers");
    BAD(&uam_xtensa_lx6, b.nareg = 48, "48 physical registers");
    BAD(&uam_xtensa_lx106, b.nareg = 32, "32 registers without windows");
    BAD(&uam_xtensa_lx6, b.ninterrupt = 33, "33 interrupts");
    BAD(&uam_xtensa_lx6, b.nlevel = 7, "7 levels");
    BAD(&uam_xtensa_lx6, b.excm_level = 7, "EXCMLEVEL above NLEVEL");
    BAD(&uam_xtensa_lx6, b.debug_level = 1, "DEBUGLEVEL 1");
    BAD(&uam_xtensa_lx6, b.irq[14].level = 6, "the NMI not at NLEVEL + 1");
    BAD(&uam_xtensa_lx6, b.nmi = false, "an NMI line without the NMI");
    BAD(&uam_xtensa_lx6, b.irq[4].level = 0, "a level-0 interrupt");
    BAD(&uam_xtensa_lx6, b.timer_int[1] = 4, "CCOMPARE1 raising a level line");
    BAD(&uam_xtensa_lx6, b.nccompare = 4, "four CCOMPAREs");
    BAD(&uam_xtensa_lx6, b.nmisc = 5, "five MISC registers");
#undef BAD
    CHECK(strcmp(uam_xtensa_config_of(&m.cpu)->name, "lx6") == 0, "the model is unchanged");
    /* a model that is not one of the three, but consistent */
    b = uam_xtensa_lx6;
    b.name = "lx6-32";
    b.nareg = 32;
    CHECK(uam_xtensa_configure(&m.cpu, &b) == UAM_OK, "32 windowed registers are a model");
    CHECK(strcmp(uam_xtensa_config_of(&m.cpu)->name, "lx6-32") == 0, "and it is in effect");
    uam_machine_fini(&m);

    /* not an Xtensa hart */
    CHECK(uam_machine_init(&m, &uam_isa_z80) == UAM_OK, "a Z80");
    CHECK(uam_xtensa_configure(&m.cpu, &uam_xtensa_lx6) == UAM_ERR_INVALID, "an Xtensa model on a Z80");
    CHECK(uam_xtensa_config_of(&m.cpu) == NULL, "a Z80 has no Xtensa model");
    uam_xtensa_set_prid(&m.cpu, 1);                    /* ignored, no crash */
    uam_xtensa_set_eri(&m.cpu, NULL, NULL, NULL);
    uam_machine_fini(&m);

    /* the reset state of each model */
    for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++) {
        const uam_xtensa_config *cfg = models[i];
        const bool win = cfg->options & UAM_XT_OPT_WINDOWED;
        CHECK(uam_machine_init(&m, &uam_isa_xtensa) == UAM_OK, "init");
        CHECK(uam_xtensa_configure(&m.cpu, cfg) == UAM_OK, "configure %s", cfg->name);
        uam_machine_reset(&m, cfg->reset_vector);
        CHECK(m.cpu.pc == cfg->reset_vector, "%s: at the reset vector", cfg->name);
        CHECK(sysr(&m, SR_PS) == (PS_EXCM | PS_INTLEVEL), "%s: PS = EXCM, INTLEVEL 15", cfg->name);
        CHECK(sysr(&m, SR_VECBASE) == cfg->vecbase, "%s: VECBASE", cfg->name);
        CHECK(sysr(&m, SR_INTENABLE) == 0 && sysr(&m, SR_INTERRUPT) == 0, "%s: nothing enabled or pending", cfg->name);
        CHECK(sysr(&m, SR_CCOUNT) == 0, "%s: CCOUNT 0", cfg->name);
        CHECK(sysr(&m, SR_PRID) == cfg->prid, "%s: PRID", cfg->name);
        CHECK(sysr(&m, SR_EXCCAUSE) == 0 && sysr(&m, SR_DEBUGCAUSE) == 0, "%s: causes clear", cfg->name);
        if (win) {
            CHECK(sysr(&m, SR_WB) == 0 && sysr(&m, SR_WS) == 1, "%s: WINDOWBASE 0, WINDOWSTART 1", cfg->name);
            CHECK(sysr(&m, SR_ATOMCTL) == 0x28, "%s: ATOMCTL", cfg->name);
            CHECK(sysr(&m, SR_CONFIGID0) == cfg->configid0, "%s: CONFIGID0", cfg->name);
            CHECK(reg(&m, XT_WCHECK) == 0, "%s: no overflow check with PS.EXCM", cfg->name);
        }
        CHECK(reg(&m, XT_LENDA) == 0xFFFFFFFFu, "%s: no loop-back after reset", cfg->name);
        /* PRID per hart outlives the reset, as a board sets it once */
        uam_xtensa_set_prid(&m.cpu, 0xABAB);
        uam_machine_reset(&m, cfg->reset_vector);
        CHECK(sysr(&m, SR_PRID) == 0xABAB, "%s: PRID set by the board", cfg->name);
        uam_machine_fini(&m);
    }
}

/* ---- which special registers each model has ------------------------------------- */

enum { OP_RSR, OP_WSR, OP_XSR };

static void test_special_registers(bool jit)
{
    static const struct { uint8_t op; uint8_t sr; bool has[3]; const char *what; } cases[] = {
        /*                                lx106  lx6    lx7 */
        { OP_RSR, SR_LBEG,      { false, true,  true  }, "LBEG (Loops)" },
        { OP_RSR, SR_LITBASE,   { true,  false, false }, "LITBASE (Extended L32R)" },
        { OP_RSR, SR_SCOMPARE1, { false, true,  true  }, "SCOMPARE1 (Conditional Store)" },
        { OP_RSR, SR_WB,        { false, true,  true  }, "WINDOWBASE" },
        { OP_RSR, SR_MEMCTL,    { false, true,  true  }, "MEMCTL" },
        { OP_RSR, SR_ATOMCTL,   { false, true,  true  }, "ATOMCTL" },
        { OP_RSR, SR_CONFIGID0, { false, true,  true  }, "CONFIGID0" },
        { OP_WSR, SR_CONFIGID0, { false, false, false }, "WSR CONFIGID0 (read-only)" },
        { OP_RSR, SR_CPENABLE,  { false, true,  true  }, "CPENABLE" },
        { OP_RSR, SR_INTERRUPT, { true,  true,  true  }, "INTERRUPT" },
        { OP_XSR, SR_INTERRUPT, { false, false, false }, "XSR INTERRUPT" },
        { OP_RSR, SR_INTCLEAR,  { false, false, false }, "RSR INTCLEAR (write-only)" },
        { OP_WSR, SR_INTCLEAR,  { true,  true,  true  }, "WSR INTCLEAR" },
        { OP_RSR, SR_PRID,      { true,  true,  true  }, "PRID" },
        { OP_WSR, SR_PRID,      { false, false, false }, "WSR PRID (read-only)" },
        { OP_RSR, SR_DEBUGCAUSE,{ true,  true,  true  }, "DEBUGCAUSE" },
        { OP_WSR, SR_DEBUGCAUSE,{ false, false, false }, "WSR DEBUGCAUSE (read-only)" },
        { OP_RSR, SR_CCOMPARE0, { true,  true,  true  }, "CCOMPARE0" },
        { OP_RSR, SR_CCOMPARE0 + 1, { false, true, true }, "CCOMPARE1" },
        { OP_RSR, SR_MISC0,     { false, true,  true  }, "MISC0" },
        { OP_RSR, SR_EPC(3),    { true,  true,  true  }, "EPC3 (the LX106's NMI level)" },
        { OP_RSR, SR_EPC(4),    { false, true,  true  }, "EPC4" },
        { OP_RSR, SR_EPS(7),    { false, true,  true  }, "EPS7" },
        { OP_RSR, SR_EXCSAVE(7),{ false, true,  true  }, "EXCSAVE7" },
        { OP_RSR, 100,          { false, false, false }, "special register 100" },
    };
    for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++) {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, models[k], jit);
        for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
            const uint32_t at = CODE + 16 * (uint32_t)i;
            const unsigned sr = cases[i].sr;
            const uint32_t insn = cases[i].op == OP_RSR ? RSR(2, sr) : cases[i].op == OP_WSR ? WSR(2, sr) : XSR(2, sr);
            spin(&m, put(&m, at, insn));
            sysw(&m, SR_PS, ps0(&m));
            sysw(&m, SR_EXCCAUSE, 63);
            seta(&m, 2, 0);
            run(&m, at, 8);
            if (cases[i].has[k]) {
                CHECK(m.cpu.pc == at + 3, "%s: %s exists (pc 0x%llx)", models[k]->name, cases[i].what,
                      (unsigned long long)m.cpu.pc);
                CHECK(sysr(&m, SR_EXCCAUSE) == 63, "%s: %s raised nothing", models[k]->name, cases[i].what);
            } else {
                char what[96];
                snprintf(what, sizeof(what), "%s: %s", models[k]->name, cases[i].what);
                expect_exc(&m, XT_EXC_ILLEGAL, at, what);
            }
        }
        uam_machine_fini(&m);
    }
}

/* A model without an option: its instructions are illegal, as on hardware. */
static void test_missing_option(bool jit)
{
    uam_xtensa_config b = uam_xtensa_lx6;
    b.name = "lx6-no-eri";
    b.options &= ~UAM_XT_OPT_EXTERN_REGS;
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &b, jit);
    spin(&m, put(&m, CODE, RER(3, 2)));
    run(&m, CODE, 8);
    expect_exc(&m, XT_EXC_ILLEGAL, CODE, "RER without the External Registers option");
    uam_machine_fini(&m);

    b = uam_xtensa_lx6;
    b.name = "lx6-no-windows";
    b.options &= ~UAM_XT_OPT_WINDOWED;
    b.nareg = 16;
    machine(&m, &uam_isa_xtensa, &b, jit);
    spin(&m, put(&m, CODE, ENTRY(1, 32)));
    run(&m, CODE, 8);
    expect_exc(&m, XT_EXC_ILLEGAL, CODE, "ENTRY without the Windowed Registers option");
    sysw(&m, SR_PS, PS_WOE | 1u << PS_CALLINC_SH | 3u << PS_OWB_SH);
    EQ64(sysr(&m, SR_PS), 0, "PS has no WOE, CALLINC or OWB without windows");
    uam_machine_fini(&m);
}

/* ---- interrupts -------------------------------------------------------------------- */

static void test_irq_lines(void)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    /* level line 4 */
    line(&m, 4, 1);
    EQ64(sysr(&m, SR_INTERRUPT), 1u << 4, "a level line reads as it is");
    CHECK(!take(&m) && !wake(&m), "not enabled: neither taken nor waking");
    sysw(&m, SR_INTCLEAR, 1u << 4);
    EQ64(sysr(&m, SR_INTERRUPT), 1u << 4, "INTCLEAR does not clear a level line");
    line(&m, 4, 0);
    EQ64(sysr(&m, SR_INTERRUPT), 0, "it drops with the line");
    /* edge line 10 */
    line(&m, 10, 1);
    line(&m, 10, 0);
    EQ64(sysr(&m, SR_INTERRUPT), 1u << 10, "an edge line latches its rising edge");
    sysw(&m, SR_INTCLEAR, 1u << 10);
    EQ64(sysr(&m, SR_INTERRUPT), 0, "INTCLEAR clears it");
    line(&m, 10, 1);
    sysw(&m, SR_INTCLEAR, 1u << 10);
    line(&m, 10, 1);
    EQ64(sysr(&m, SR_INTERRUPT), 0, "a line held high is one edge");
    line(&m, 10, 0);
    /* software interrupts 7 and 29; INTSET sets nothing else */
    sysw(&m, SR_INTSET, 1u << 7 | 1u << 4 | 1u << 10 | 1u << 29 | 1u << 6);
    EQ64(sysr(&m, SR_INTERRUPT), 1u << 7 | 1u << 29, "INTSET sets the software interrupts only");
    sysw(&m, SR_INTCLEAR, 0xFFFFFFFFu);
    EQ64(sysr(&m, SR_INTERRUPT), 0, "INTCLEAR clears them");
    sysw(&m, SR_INTENABLE, 0xFFFFFFFFu);
    EQ64(sysr(&m, SR_INTENABLE), 0xFFFFFFFFu, "32 interrupts: INTENABLE is 32 bits");
    line(&m, 40, 1);
    EQ64(sysr(&m, SR_INTERRUPT), 0, "a line beyond the interrupts is ignored");
    uam_machine_fini(&m);

    /* LX106: 15 interrupts, the NMI at level 3 */
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx106, false);
    sysw(&m, SR_INTENABLE, 0xFFFFFFFFu);
    EQ64(sysr(&m, SR_INTENABLE), 0x7FFF, "lx106: INTENABLE has 15 bits");
    line(&m, 20, 1);
    EQ64(sysr(&m, SR_INTERRUPT), 0, "lx106: line 20 does not exist");
    sysw(&m, SR_PS, PS_EXCM | PS_INTLEVEL);
    line(&m, 14, 1);
    CHECK(wake(&m), "lx106: the NMI wakes whatever PS says");
    CHECK(take(&m), "lx106: and is taken");
    EQ64(m.cpu.pc, vec(&m, UAM_XT_VEC_LEVEL3), "lx106: the NMI is level 3");
    EQ64(sysr(&m, SR_EPC(3)), CODE, "lx106: EPC3");
    EQ64(sysr(&m, SR_EPS(3)), PS_EXCM | PS_INTLEVEL, "lx106: EPS3 is PS before");
    EQ64(sysr(&m, SR_PS) & PS_INTLEVEL, 3, "lx106: PS.INTLEVEL 3");
    EQ64(lines_taken(&m), 1u << 14, "lx106: line 14 reported");
    CHECK(!take(&m), "lx106: one NMI per edge");
    uam_machine_fini(&m);
}

static void test_irq_level1(void)
{
    for (int um = 0; um < 2; um++) {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
        sysw(&m, SR_PS, PS_WOE | (um ? PS_UM : 0));
        sysw(&m, SR_INTENABLE, 1u << 4);
        line(&m, 4, 1);
        CHECK(wake(&m), "an enabled level-1 line wakes");
        CHECK(take(&m), "and is taken");
        EQ64(m.cpu.pc, vec(&m, um ? UAM_XT_VEC_USER : UAM_XT_VEC_KERNEL),
             um ? "level 1 from user mode: the user vector" : "level 1: the kernel vector");
        EQ64(sysr(&m, SR_EXCCAUSE), XT_EXC_LEVEL1, "EXCCAUSE Level1Interrupt");
        EQ64(sysr(&m, SR_EPC1), CODE, "EPC1");
        CHECK(sysr(&m, SR_PS) & PS_EXCM, "PS.EXCM set");
        EQ64(lines_taken(&m), 1u << 4, "line 4 reported");
        CHECK(!take(&m), "PS.EXCM masks level 1");
        uam_machine_fini(&m);
    }
}

static void test_irq_levels(void)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    sysw(&m, SR_INTENABLE, 0xFFFFFFFFu);
    line(&m, 4, 1);    /* level 1 */
    line(&m, 19, 1);   /* level 2 */
    line(&m, 26, 1);   /* level 5 */
    CHECK(take(&m), "taken");
    const uint32_t l5 = vec(&m, UAM_XT_VEC_LEVEL5);
    EQ64(m.cpu.pc, l5, "the highest pending level: 5");
    EQ64(sysr(&m, SR_EPC(5)), CODE, "EPC5");
    EQ64(sysr(&m, SR_EPS(5)), PS_WOE, "EPS5");
    EQ64(sysr(&m, SR_PS), PS_WOE | PS_EXCM | 5, "PS: EXCM, INTLEVEL 5");
    EQ64(lines_taken(&m), 1u << 26, "line 26 reported");
    CHECK(!take(&m), "levels 1 and 2 wait");
    line(&m, 14, 1);
    CHECK(take(&m), "the NMI interrupts level 5");
    EQ64(m.cpu.pc, vec(&m, UAM_XT_VEC_LEVEL7), "the NMI: level 7");
    EQ64(sysr(&m, SR_EPC(7)), l5, "EPC7");
    EQ64(sysr(&m, SR_EPS(7)), PS_WOE | PS_EXCM | 5, "EPS7");
    EQ64(lines_taken(&m), 1u << 14, "line 14 reported");
    CHECK(!take(&m), "one NMI per edge");
    line(&m, 14, 0);
    /* RFI 7, RFI 5: back to where each was taken */
    sysw(&m, XT_SYS_RFI, 7);
    EQ64(sysr(&m, XT_SYS_RET_PC), l5, "RFI 7 returns to the level-5 handler");
    EQ64(sysr(&m, SR_PS), PS_WOE | PS_EXCM | 5, "with its PS");
    sysw(&m, XT_SYS_RFI, 5);
    EQ64(sysr(&m, XT_SYS_RET_PC), CODE, "RFI 5 returns to the code");
    EQ64(sysr(&m, SR_PS), PS_WOE, "with its PS");
    m.cpu.pc = CODE;
    line(&m, 26, 0);
    CHECK(take(&m), "level 2 next");
    EQ64(m.cpu.pc, vec(&m, UAM_XT_VEC_LEVEL2), "the level-2 vector");
    EQ64(lines_taken(&m), 1u << 19, "line 19 reported (not line 4, level 1)");
    uam_machine_fini(&m);

    /* EXCMLEVEL 3: with PS.EXCM, levels up to 3 wait, 4 is taken */
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    sysw(&m, SR_PS, PS_WOE | PS_EXCM);
    sysw(&m, SR_INTENABLE, 1u << 23 | 1u << 24);
    line(&m, 23, 1);
    CHECK(!take(&m) && !wake(&m), "level 3 waits while PS.EXCM");
    line(&m, 24, 1);
    CHECK(take(&m), "level 4 is taken");
    EQ64(m.cpu.pc, vec(&m, UAM_XT_VEC_LEVEL4), "the level-4 vector");
    uam_machine_fini(&m);
}

/* WAITI sets PS.INTLEVEL and waits; only an interrupt above it wakes the hart. */
static void test_waiti(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
    spin(&m, put(&m, CODE, WAITI(1)));
    sysw(&m, SR_INTENABLE, 1u << 4 | 1u << 19);
    run(&m, CODE, 20);
    CHECK(m.cpu.waiting, "WAITI waits");
    EQ64(m.cpu.pc, CODE + 3, "after the WAITI");
    EQ64(sysr(&m, SR_PS) & PS_INTLEVEL, 1, "PS.INTLEVEL 1");
    line(&m, 4, 1);
    CHECK(!wake(&m), "a level-1 line does not wake it");
    line(&m, 19, 1);
    CHECK(wake(&m), "a level-2 line does");
    uam_exec_run(&m.exec, 20);
    CHECK(!m.cpu.waiting, "no longer waiting");
    EQ64(m.cpu.pc, vec(&m, UAM_XT_VEC_LEVEL2), "the level-2 handler");
    EQ64(sysr(&m, SR_EPC(2)), CODE + 3, "EPC2: the instruction after WAITI");
    EQ64(sysr(&m, SR_EPS(2)) & PS_INTLEVEL, 1, "EPS2: WAITI's level");
    uam_machine_fini(&m);
}

/* ---- CCOUNT and CCOMPARE ----------------------------------------------------------- */

static void tick(uam_machine *m, uint64_t n)
{
    m->sched.idle += n;
    uam_sched_run_due(&m->sched);
}
static bool pending(uam_machine *m, unsigned n) { return sysr(m, SR_INTERRUPT) >> n & 1; }

static void test_timers(void)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    EQ64(sysr(&m, SR_CCOUNT), 0, "CCOUNT 0 after reset");
    sysw(&m, SR_CCOUNT, 1000);
    EQ64(sysr(&m, SR_CCOUNT), 1000, "CCOUNT written");
    tick(&m, 7);
    EQ64(sysr(&m, SR_CCOUNT), 1007, "CCOUNT counts virtual time");
    sysw(&m, SR_CCOMPARE0, 1017);
    sysw(&m, SR_CCOMPARE0 + 2, 1027);
    tick(&m, 9);
    CHECK(!pending(&m, 6), "CCOMPARE0: not before the match");
    tick(&m, 1);
    CHECK(pending(&m, 6), "CCOMPARE0 raises interrupt 6 at the match");
    CHECK(!pending(&m, 16), "CCOMPARE2 not yet");
    tick(&m, 10);
    CHECK(pending(&m, 16), "CCOMPARE2 raises interrupt 16");
    sysw(&m, SR_INTCLEAR, 1u << 6);
    CHECK(pending(&m, 6), "INTCLEAR does not clear a timer interrupt");
    sysw(&m, SR_CCOMPARE0, 0);
    CHECK(!pending(&m, 6), "writing CCOMPARE0 does");
    /* writing CCOUNT moves the matches */
    sysw(&m, SR_CCOMPARE0 + 1, 5000);
    sysw(&m, SR_CCOUNT, 4990);
    tick(&m, 9);
    CHECK(!pending(&m, 15), "CCOMPARE1: 10 ticks after CCOUNT = 4990, not 9");
    tick(&m, 1);
    CHECK(pending(&m, 15), "CCOMPARE1 raises interrupt 15");
    /* interrupt 15 is level 3 */
    sysw(&m, SR_INTENABLE, 1u << 15);
    CHECK(take(&m), "the timer interrupt is taken");
    EQ64(m.cpu.pc, vec(&m, UAM_XT_VEC_LEVEL3), "at level 3");
    EQ64(lines_taken(&m), 1u << 15, "line 15 reported");
    uam_machine_fini(&m);

    /* LX106: one CCOMPARE, interrupt 6 */
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx106, false);
    sysw(&m, SR_CCOMPARE0, 50);
    tick(&m, 50);
    CHECK(pending(&m, 6), "lx106: CCOMPARE0 raises interrupt 6");
    uam_machine_fini(&m);
}

/* ---- register windows ---------------------------------------------------------------- */

static void test_window_file(void)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    seta(&m, 0, 0x1000);
    seta(&m, 4, 0x44);
    seta(&m, 8, 0x88);
    sysw(&m, SR_WB, 1);
    EQ64(a_(&m, 0), 0x44, "WINDOWBASE 1: a0 is AR4");
    EQ64(a_(&m, 4), 0x88, "a4 is AR8");
    EQ64(reg(&m, XT_AR(4)), 0x44, "the window went back to the physical file");
    sysw(&m, SR_WB, 15);
    EQ64(a_(&m, 4), 0x1000, "WINDOWBASE 15: a4 wraps to AR0");
    sysw(&m, SR_WB, 0);
    EQ64(a_(&m, 4), 0x44, "back at 0");
    /* ROTW */
    sysw(&m, SR_WB, 3);
    spin(&m, put(&m, CODE, ROTW(-1)));
    run(&m, CODE, 4);
    EQ64(sysr(&m, SR_WB), 2, "ROTW -1");
    EQ64(a_(&m, 0), reg(&m, XT_AR(8)), "a0 is AR8 there");
    /* WCHECK: WindowStart of the next three frames, while overflow detection is on */
    sysw(&m, SR_WB, 0);
    sysw(&m, SR_WS, 1u | 1u << 2);
    EQ64(reg(&m, XT_WCHECK), 1u << 2, "WCHECK: frame 2 live");
    sysw(&m, SR_WS, 1u | 1u << 1 | 1u << 3 | 1u << 4);
    EQ64(reg(&m, XT_WCHECK), 1u << 1 | 1u << 3, "WCHECK: frames 1 and 3, not 4");
    sysw(&m, SR_PS, PS_WOE | PS_EXCM);
    EQ64(reg(&m, XT_WCHECK), 0, "no check with PS.EXCM");
    sysw(&m, SR_PS, 0);
    EQ64(reg(&m, XT_WCHECK), 0, "nor without PS.WOE");
    sysw(&m, SR_WB, 15);
    sysw(&m, SR_PS, PS_WOE);
    EQ64(reg(&m, XT_WCHECK), 1u << 1 | 1u << 2, "WINDOWBASE 15: frames 0 and 1 wrap");
    sysw(&m, SR_WS, 0xFFFFFFFFu);
    EQ64(sysr(&m, SR_WS), 0xFFFF, "WINDOWSTART: 16 frames");
    uam_machine_fini(&m);
}

/* CALL4, ENTRY, RETW: the callee runs in the window four registers on. */
static void test_window_call(bool jit)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
    const uint32_t f = CODE + 0x40;
    spin(&m, put(&m, CODE, CALLn(1, CODE, f)));
    uint32_t a = put(&m, f, ENTRY(1, 32));
    a = put(&m, a, RSR(3, SR_WB));
    a = put(&m, a, MOVI(2, 0x123));
    put(&m, a, RETW);
    run(&m, CODE, 40);
    EQ64(m.cpu.pc, CODE + 3, "returned after the CALL4");
    EQ64(sysr(&m, SR_WB), 0, "WINDOWBASE back to 0");
    EQ64(sysr(&m, SR_WS), 1, "the callee's frame is gone");
    EQ64(a_(&m, 7), 1, "the callee ran at WINDOWBASE 1 (its a3 is our a7)");
    EQ64(a_(&m, 6), 0x123, "its a2 is our a6");
    EQ64(a_(&m, 5), STACK - 32, "ENTRY: its a1 = our a1 - 32");
    EQ64(a_(&m, 4), 1u << 30 | ((CODE + 3) & 0x3FFFFFFFu), "a4: the return address, window increment 1");
    EQ64(a_(&m, 1), STACK, "our a1 kept");
    uam_machine_fini(&m);
}

static void test_window_exceptions(bool jit)
{
    /* ENTRY's overflow: the nearest live frame is spilled; WindowStart past it picks the vector */
    static const struct { unsigned callinc; uint32_t ws; unsigned vec, wb; const char *what; } ovf[] = {
        { 1, 0x07, UAM_XT_VEC_WO4,  1, "CALL4 frame, its caller's next: WindowOverflow4" },
        { 1, 0x0B, UAM_XT_VEC_WO8,  1, "frame 1, then frame 3: WindowOverflow8" },
        { 1, 0x13, UAM_XT_VEC_WO12, 1, "frame 1, then frame 4: WindowOverflow12" },
        { 2, 0x05, UAM_XT_VEC_WO12, 2, "CALL8 over frame 2" },
        { 3, 0x09, UAM_XT_VEC_WO12, 3, "CALL12 over frame 3" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(ovf); i++) {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
        spin(&m, put(&m, CODE, ENTRY(1, 32)));
        sysw(&m, SR_WS, ovf[i].ws);
        sysw(&m, SR_PS, PS_WOE | ovf[i].callinc << PS_CALLINC_SH);
        run(&m, CODE, 4);
        CHECK(m.cpu.pc == vec(&m, ovf[i].vec), "%s: pc 0x%llx", ovf[i].what, (unsigned long long)m.cpu.pc);
        CHECK(sysr(&m, SR_WB) == ovf[i].wb, "%s: WINDOWBASE %u", ovf[i].what, (unsigned)sysr(&m, SR_WB));
        const uint32_t ps = sysr(&m, SR_PS);
        CHECK((ps & PS_OWB) == 0 && (ps & PS_EXCM), "%s: PS.OWB 0, EXCM", ovf[i].what);
        CHECK(sysr(&m, SR_EPC1) == CODE, "%s: EPC1 is the ENTRY", ovf[i].what);
        uam_machine_fini(&m);
    }
    /* ENTRY with nothing live in the way: no overflow */
    {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
        spin(&m, put(&m, CODE, ENTRY(1, 32)));
        sysw(&m, SR_WS, 0x11);                    /* frame 4 live: past the three CALL12 checks */
        sysw(&m, SR_PS, PS_WOE | 3u << PS_CALLINC_SH);
        run(&m, CODE, 4);
        EQ64(m.cpu.pc, CODE + 3, "CALL12 frame, frames 1-3 dead: no overflow");
        EQ64(sysr(&m, SR_WB), 3, "WINDOWBASE 3");
        EQ64(sysr(&m, SR_WS), 0x19, "its frame marked live");
        uam_machine_fini(&m);
    }

    /* RETW's underflow: the caller's frame is not resident */
    static const struct { uint32_t a0top; unsigned vec, wb; } unf[] = {
        { 1u << 30, UAM_XT_VEC_WU4, 1 },
        { 2u << 30, UAM_XT_VEC_WU8, 0 },
        { 3u << 30, UAM_XT_VEC_WU12, 15 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(unf); i++) {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
        sysw(&m, SR_WB, 2);
        sysw(&m, SR_WS, 1u << 2);
        spin(&m, put(&m, CODE, RETW));
        seta(&m, 0, unf[i].a0top | 0x1100);
        run(&m, CODE, 4);
        CHECK(m.cpu.pc == vec(&m, unf[i].vec), "underflow %zu: pc 0x%llx", i + 1, (unsigned long long)m.cpu.pc);
        CHECK(sysr(&m, SR_WB) == unf[i].wb, "underflow %zu: WINDOWBASE the caller's (%u)", i + 1,
              (unsigned)sysr(&m, SR_WB));
        CHECK(((sysr(&m, SR_PS) & PS_OWB) >> PS_OWB_SH) == 2, "underflow %zu: PS.OWB the callee's", i + 1);
        CHECK(sysr(&m, SR_EPC1) == CODE, "underflow %zu: EPC1 the RETW", i + 1);
        uam_machine_fini(&m);
    }
    /* RETW returning: the top bits of the target are the RETW's own */
    {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
        sysw(&m, SR_WB, 2);
        sysw(&m, SR_WS, 1u << 1 | 1u << 2);
        spin(&m, CODE + 0x100);
        put(&m, CODE, RETW);
        seta(&m, 0, 1u << 30 | 0x1100);
        run(&m, CODE, 4);
        EQ64(m.cpu.pc, CODE + 0x100, "RETW: pc[31:30] kept, a0[29:0]");
        EQ64(sysr(&m, SR_WB), 1, "WINDOWBASE 1");
        EQ64(sysr(&m, SR_WS), 1u << 1, "the callee's frame cleared");
        uam_machine_fini(&m);
    }
    /* RETW that cannot be right: IllegalInstruction */
    static const struct { uint32_t ws, a0; const char *what; } ill[] = {
        { 1u << 1 | 1u << 2, 2u << 30 | 0x1100, "a0 says CALL8, the live frame below is CALL4's" },
        { 1u << 1 | 1u << 2, 0x00001100, "a0 says CALL0" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(ill); i++) {
        uam_machine m;
        machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
        sysw(&m, SR_WB, 2);
        sysw(&m, SR_WS, ill[i].ws);
        spin(&m, put(&m, CODE, RETW));
        seta(&m, 0, ill[i].a0);
        run(&m, CODE, 4);
        expect_exc(&m, XT_EXC_ILLEGAL, CODE, ill[i].what);
        uam_machine_fini(&m);
    }
}

/* ---- bus errors, alignment ------------------------------------------------------------ */

static void test_faults(bool jit)
{
    for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++) {
        const uam_xtensa_config *cfg = models[k];
        char what[96];
        uam_machine m;
        /* a load from nothing: LoadStoreError, EXCVADDR the address */
        machine(&m, &uam_isa_xtensa, cfg, jit);
        spin(&m, put(&m, CODE, L32I(3, 2, 8)));
        seta(&m, 2, 0x20000000u);
        run(&m, CODE, 4);
        snprintf(what, sizeof(what), "%s: load from nothing", cfg->name);
        expect_exc(&m, XT_EXC_LS_ERR, CODE, what);
        CHECK(sysr(&m, SR_EXCVADDR) == 0x20000008u, "%s: EXCVADDR", what);
        uam_machine_fini(&m);
        /* a store */
        machine(&m, &uam_isa_xtensa, cfg, jit);
        spin(&m, put(&m, CODE, S32I(3, 2, 0)));
        seta(&m, 2, 0x20000100u);
        run(&m, CODE, 4);
        snprintf(what, sizeof(what), "%s: store to nothing", cfg->name);
        expect_exc(&m, XT_EXC_LS_ERR, CODE, what);
        CHECK(sysr(&m, SR_EXCVADDR) == 0x20000100u, "%s: EXCVADDR", what);
        uam_machine_fini(&m);
        /* a jump to nothing: InstFetchError at the target */
        machine(&m, &uam_isa_xtensa, cfg, jit);
        put(&m, CODE, JX(2));
        seta(&m, 2, 0x50000000u);
        run(&m, CODE, 4);
        snprintf(what, sizeof(what), "%s: fetch from nothing", cfg->name);
        expect_exc(&m, XT_EXC_IFETCH_ERR, 0x50000000u, what);
        CHECK(sysr(&m, SR_EXCVADDR) == 0x50000000u, "%s: EXCVADDR", what);
        uam_machine_fini(&m);
        /* a misaligned word: the LX106 raises LoadStoreAlignment, the others load it */
        machine(&m, &uam_isa_xtensa, cfg, jit);
        const uint8_t bytes[] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 };
        code(&m, DATA, bytes, sizeof(bytes));
        spin(&m, put(&m, CODE, L32I(3, 2, 0)));
        seta(&m, 2, DATA + 2);
        seta(&m, 3, 0xDEAD);
        run(&m, CODE, 4);
        snprintf(what, sizeof(what), "%s: misaligned load", cfg->name);
        if (cfg->options & UAM_XT_OPT_UNALIGNED_EXC) {
            expect_exc(&m, XT_EXC_ALIGN, CODE, what);
            CHECK(sysr(&m, SR_EXCVADDR) == DATA + 2, "%s: EXCVADDR", what);
            CHECK(a_(&m, 3) == 0xDEAD, "%s: a3 unchanged", what);
        } else {
            CHECK(m.cpu.pc == CODE + 3, "%s: no exception", what);
            CHECK(a_(&m, 3) == 0x15141312u, "%s: a3 0x%x", what, (unsigned)a_(&m, 3));
        }
        uam_machine_fini(&m);
        /* an exception with PS.EXCM set: the double vector, DEPC */
        machine(&m, &uam_isa_xtensa, cfg, jit);
        spin(&m, put(&m, CODE, ILL));
        sysw(&m, SR_PS, ps0(&m) | PS_EXCM);
        sysw(&m, SR_EPC1, 0x1234);
        run(&m, CODE, 4);
        snprintf(what, sizeof(what), "%s: ILL with PS.EXCM", cfg->name);
        CHECK(m.cpu.pc == vec(&m, UAM_XT_VEC_DOUBLE), "%s: the double vector", what);
        CHECK(sysr(&m, SR_DEPC) == CODE, "%s: DEPC", what);
        CHECK(sysr(&m, SR_EPC1) == 0x1234, "%s: EPC1 kept", what);
        uam_machine_fini(&m);
    }
}

/* ---- BREAK --------------------------------------------------------------------------- */

static void test_break(bool jit)
{
    for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++) {
        const uam_xtensa_config *cfg = models[k];
        const unsigned dl = cfg->debug_level;
        uam_machine m;
        machine(&m, &uam_isa_xtensa, cfg, jit);
        spin(&m, put(&m, CODE, BREAK(1, 2)));
        run(&m, CODE, 4);
        CHECK(m.cpu.pc == vec(&m, UAM_XT_VEC_LEVEL2 + dl - 2), "%s: BREAK: the DEBUGLEVEL vector", cfg->name);
        CHECK(sysr(&m, SR_EPC(dl)) == CODE, "%s: EPC at DEBUGLEVEL", cfg->name);
        CHECK(sysr(&m, SR_EPS(dl)) == ps0(&m), "%s: EPS at DEBUGLEVEL", cfg->name);
        CHECK(sysr(&m, SR_DEBUGCAUSE) == 8, "%s: DEBUGCAUSE.BI", cfg->name);
        CHECK((sysr(&m, SR_PS) & (PS_INTLEVEL | PS_EXCM)) == (dl | PS_EXCM), "%s: PS.INTLEVEL DEBUGLEVEL", cfg->name);
        uam_machine_fini(&m);

        machine(&m, &uam_isa_xtensa, cfg, jit);
        spin(&m, put_n(&m, CODE, BREAK_N(3)));
        run(&m, CODE, 4);
        CHECK(m.cpu.pc == vec(&m, UAM_XT_VEC_LEVEL2 + dl - 2), "%s: BREAK.N", cfg->name);
        CHECK(sysr(&m, SR_DEBUGCAUSE) == 16, "%s: DEBUGCAUSE.BN", cfg->name);
        uam_machine_fini(&m);

        /* at DEBUGLEVEL nothing happens: the instruction is skipped */
        machine(&m, &uam_isa_xtensa, cfg, jit);
        uint32_t a = put(&m, CODE, BREAK(1, 2));
        spin(&m, put_n(&m, a, BREAK_N(3)));
        sysw(&m, SR_PS, ps0(&m) | dl);
        run(&m, CODE, 6);
        CHECK(m.cpu.pc == CODE + 5, "%s: BREAK at DEBUGLEVEL is nothing (pc 0x%llx)", cfg->name,
              (unsigned long long)m.cpu.pc);
        CHECK(sysr(&m, SR_DEBUGCAUSE) == 0, "%s: DEBUGCAUSE untouched", cfg->name);
        uam_machine_fini(&m);
    }
}

/* ---- the External Register Interface --------------------------------------------------- */

static uint32_t eri_last_addr, eri_last_val;
static unsigned eri_reads, eri_writes;
static uint32_t eri_rd(void *ctx, uint32_t addr)
{
    eri_reads++;
    eri_last_addr = addr;
    return addr ^ *(uint32_t *)ctx;
}
static void eri_wr(void *ctx, uint32_t addr, uint32_t val)
{
    (void)ctx;
    eri_writes++;
    eri_last_addr = addr;
    eri_last_val = val;
}

static void test_eri(bool jit)
{
    static uint32_t key = 0x5A5A5A5Au;
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, jit);
    uam_xtensa_set_eri(&m.cpu, eri_rd, eri_wr, &key);
    eri_reads = eri_writes = 0;
    uint32_t a = put(&m, CODE, RER(3, 2));
    a = put(&m, a, WER(4, 5));
    spin(&m, a);
    seta(&m, 2, 0x3FF00010u);
    seta(&m, 4, 0xCAFEF00Du);
    seta(&m, 5, 0x3FF02000u);
    run(&m, CODE, 6);
    EQ64(eri_reads, 1, "RER reads the board once");
    EQ64(a_(&m, 3), 0x3FF00010u ^ key, "RER: the board's value");
    EQ64(eri_writes, 1, "WER writes once");
    EQ64(eri_last_addr, 0x3FF02000u, "WER: the address from as");
    EQ64(eri_last_val, 0xCAFEF00Du, "WER: the value from at");
    /* the hook outlives a reset */
    uam_machine_reset(&m, CODE);
    sysw(&m, SR_PS, PS_WOE);
    seta(&m, 2, 0x100);
    run(&m, CODE, 6);
    EQ64(a_(&m, 3), 0x100 ^ key, "after a reset RER still reaches the board");
    uam_machine_fini(&m);
}

/* ---- the debugger view --------------------------------------------------------------- */

enum { G_PC = 0, G_A0 = 1, G_SAR = 17, G_PS = 18, G_WB = 19, G_WS = 20, G_LBEG = 21, G_LEND = 22,
       G_LCOUNT = 23, G_BR = 24, G_M0 = 28, G_F0 = 33, G_FCR = 49, G_FSR = 50, G_N = 51 };

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    const uam_gdb_arch *g = m.cpu.isa->gdb;
    CHECK(g && g->n_regs == G_N && g->reg_bytes == 4, "51 registers of 4 bytes");
    CHECK(strstr(g->target_xml, "<architecture>xtensa</architecture>"), "target description");
    uint64_t v = 0;
    seta(&m, 2, 0xBEEF);
    set(&m, XT_F(3), 0x3F800000u);
    set(&m, XT_M(1), 0x77);
    set(&m, XT_SAR, 17);
    CHECK(g->read_reg(&m.cpu, G_PC, &v) && v == CODE, "gdb pc");
    CHECK(g->read_reg(&m.cpu, G_A0 + 2, &v) && v == 0xBEEF, "gdb a2");
    CHECK(g->read_reg(&m.cpu, G_F0 + 3, &v) && v == 0x3F800000u, "gdb f3");
    CHECK(g->read_reg(&m.cpu, G_M0 + 1, &v) && v == 0x77, "gdb m1");
    CHECK(g->read_reg(&m.cpu, G_SAR, &v) && v == 17, "gdb sar");
    set(&m, XT_CALLINC, 2);
    CHECK(g->read_reg(&m.cpu, G_PS, &v) && v == (PS_WOE | 2u << PS_CALLINC_SH), "gdb ps has CALLINC");
    CHECK(!g->read_reg(&m.cpu, G_N, &v), "no register 51");
    /* writes */
    seta(&m, 4, 0x44);
    CHECK(g->write_reg(&m.cpu, G_WB, 1), "gdb write windowbase");
    EQ64(a_(&m, 0), 0x44, "the window rotated");
    CHECK(g->read_reg(&m.cpu, G_A0, &v) && v == 0x44, "gdb a0 is the current window's");
    CHECK(g->write_reg(&m.cpu, G_WB, 0), "and back");
    CHECK(g->write_reg(&m.cpu, G_WS, 0xFFFFFFFFu) && g->read_reg(&m.cpu, G_WS, &v) && v == 0xFFFF,
          "gdb windowstart: 16 frames");
    CHECK(g->write_reg(&m.cpu, G_PS, PS_WOE | 1u << PS_CALLINC_SH | 3), "gdb write ps");
    EQ64(reg(&m, XT_CALLINC), 1, "CALLINC from PS");
    EQ64(sysr(&m, SR_PS), PS_WOE | 1u << PS_CALLINC_SH | 3, "PS");
    CHECK(g->write_reg(&m.cpu, G_LCOUNT, 5) && g->write_reg(&m.cpu, G_LEND, 0x40001234u), "gdb loop registers");
    EQ64(reg(&m, XT_LENDA), 0x40001234u, "a loop-back is now in effect");
    CHECK(g->write_reg(&m.cpu, G_PC, 0x140002000ull) && m.cpu.pc == 0x40002000u, "gdb pc: 32 bits");
    CHECK(!g->write_reg(&m.cpu, G_N, 0), "no register 51 to write");
    /* the dump says which core it is */
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    m.cpu.isa->dump_state(&m.cpu, f);
    fclose(f);
    CHECK(buf && strstr(buf, "model=lx6 little-endian"), "dump: model and byte order");
    free(buf);
    uam_machine_fini(&m);
}

/* ---- snapshots ----------------------------------------------------------------------- */

static void test_snapshot(void)
{
    uam_machine m;
    machine(&m, &uam_isa_xtensa, &uam_xtensa_lx6, false);
    sysw(&m, SR_WS, 0x0909);
    seta(&m, 4, 0x4444);
    sysw(&m, SR_WB, 3);
    sysw(&m, SR_PS, PS_WOE | 2);
    sysw(&m, SR_INTENABLE, 0x00F0F0F0u);
    sysw(&m, SR_INTSET, 1u << 7);
    line(&m, 4, 1);
    line(&m, 14, 1);                                   /* an NMI pending */
    sysw(&m, SR_EPC(3), 0x40003333u);
    sysw(&m, SR_EXCSAVE(2), 0x2222);
    sysw(&m, SR_VECBASE, 0x40010000u);
    sysw(&m, SR_CCOMPARE0 + 1, 100);
    sysw(&m, SR_MISC0 + 2, 0x6262);
    uam_xtensa_set_prid(&m.cpu, 0x1357);

    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");

    sysw(&m, SR_WB, 0);
    sysw(&m, SR_WS, 1);
    seta(&m, 0, 0);
    sysw(&m, SR_PS, PS_EXCM);
    sysw(&m, SR_INTENABLE, 0);
    sysw(&m, SR_INTCLEAR, 0xFFFFFFFFu);
    line(&m, 4, 0);
    line(&m, 14, 0);
    take(&m);
    sysw(&m, SR_EPC(3), 0);
    sysw(&m, SR_EXCSAVE(2), 0);
    sysw(&m, SR_VECBASE, RAM);
    sysw(&m, SR_CCOMPARE0 + 1, 5000);
    sysw(&m, SR_MISC0 + 2, 0);
    uam_xtensa_set_prid(&m.cpu, 0);

    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    EQ64(sysr(&m, SR_WB), 3, "WINDOWBASE");
    EQ64(sysr(&m, SR_WS), 0x0909, "WINDOWSTART");
    EQ64(reg(&m, XT_AR(4)), 0x4444, "the physical registers");
    EQ64(sysr(&m, SR_PS), PS_WOE | 2, "PS");
    EQ64(sysr(&m, SR_INTENABLE), 0x00F0F0F0u, "INTENABLE");
    EQ64(sysr(&m, SR_INTERRUPT), 1u << 7 | 1u << 4, "INTERRUPT: the latch and the line");
    EQ64(sysr(&m, SR_EPC(3)), 0x40003333u, "EPC3");
    EQ64(sysr(&m, SR_EXCSAVE(2)), 0x2222, "EXCSAVE2");
    EQ64(sysr(&m, SR_VECBASE), 0x40010000u, "VECBASE");
    EQ64(sysr(&m, SR_MISC0 + 2), 0x6262, "MISC2");
    EQ64(sysr(&m, SR_PRID), 0x1357, "PRID");
    EQ64(sysr(&m, SR_CCOMPARE0 + 1), 100, "CCOMPARE1");
    CHECK(take(&m), "the pending NMI");
    EQ64(m.cpu.pc, 0x40010000u + uam_xtensa_lx6.vec[UAM_XT_VEC_LEVEL7], "at level 7 from the restored VECBASE");
    /* the CCOMPARE1 timer is the snapshot's: it fires at 100 */
    tick(&m, 100 - sysr(&m, SR_CCOUNT) - 1);
    CHECK(!pending(&m, 15), "CCOMPARE1 not yet");
    tick(&m, 1);
    CHECK(pending(&m, 15), "CCOMPARE1 fires at the restored match");
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

/* ---- byte order ---------------------------------------------------------------------- */

/* The same program on a little- and a big-endian core: the instructions in each layout,
 * the data in each byte order. */
static void test_endian(const uam_isa *isa, const uam_xtensa_config *cfg, bool jit)
{
    const bool be = isa == &uam_isa_xtensaeb;
    const char *name = be ? "big-endian" : "little-endian";
    uam_machine m;
    machine(&m, isa, cfg, jit);
    const uint32_t g = CODE + 0x80;
    uint32_t a = CODE;
    a = put(&m, a, S32I(3, 2, 0));
    a = put(&m, a, L8UI(4, 2, 0));
    a = put(&m, a, L16UI(5, 2, 2));
    a = put(&m, a, MOVI(6, -2));
    a = put(&m, a, ADDI(7, 6, 5));
    a = put(&m, a, ADD(8, 3, 7));
    a = put(&m, a, RSR(9, SR_PRID));
    a = put_n(&m, a, ADD_N(11, 3, 3));
    a = put(&m, a, CALLn(0, a, g));
    spin(&m, a);
    put(&m, put(&m, g, MOVI(10, 0x5A)), RET);
    seta(&m, 2, DATA);
    seta(&m, 3, 0x11223344u);
    uam_xtensa_set_prid(&m.cpu, 0xC0DE);
    run(&m, CODE, 30);
    CHECK(m.cpu.pc == a, "%s %s: ran to the end (pc 0x%llx want 0x%x)", cfg->name, name,
          (unsigned long long)m.cpu.pc, (unsigned)a);
    CHECK(mem8(&m, DATA) == (be ? 0x11 : 0x44), "%s %s: S32I byte order", cfg->name, name);
    CHECK(a_(&m, 4) == (be ? 0x11u : 0x44u), "%s %s: L8UI", cfg->name, name);
    CHECK(a_(&m, 5) == (be ? 0x3344u : 0x1122u), "%s %s: L16UI 0x%x", cfg->name, name, (unsigned)a_(&m, 5));
    CHECK(mem32(&m, DATA) == 0x11223344u, "%s %s: the word", cfg->name, name);
    CHECK(a_(&m, 6) == 0xFFFFFFFEu, "%s %s: MOVI -2", cfg->name, name);
    CHECK(a_(&m, 7) == 3, "%s %s: ADDI", cfg->name, name);
    CHECK(a_(&m, 8) == 0x11223347u, "%s %s: ADD", cfg->name, name);
    CHECK(a_(&m, 9) == 0xC0DE, "%s %s: RSR PRID", cfg->name, name);
    CHECK(a_(&m, 11) == 0x22446688u, "%s %s: ADD.N", cfg->name, name);
    CHECK(a_(&m, 10) == 0x5A, "%s %s: CALL0 / RET", cfg->name, name);
    CHECK(a_(&m, 0) == a, "%s %s: CALL0's return address", cfg->name, name);
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    m.cpu.isa->dump_state(&m.cpu, f);
    fclose(f);
    CHECK(buf && strstr(buf, name), "%s: the dump says %s", cfg->name, name);
    free(buf);
    uam_machine_fini(&m);
}

int main(void)
{
    test_models();
    test_irq_lines();
    test_irq_level1();
    test_irq_levels();
    test_timers();
    test_window_file();
    test_gdb();
    test_snapshot();
    for (int jit = 0; jit < 2; jit++) {
        test_special_registers(jit);
        test_missing_option(jit);
        test_waiti(jit);
        test_window_call(jit);
        test_window_exceptions(jit);
        test_faults(jit);
        test_break(jit);
        test_eri(jit);
        for (size_t k = 0; k < UAM_ARRAY_LEN(models); k++) {
            test_endian(&uam_isa_xtensa, models[k], jit);
            test_endian(&uam_isa_xtensaeb, models[k], jit);
        }
    }
    printf("test_xtensa: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
