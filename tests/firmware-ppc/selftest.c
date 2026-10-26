/*
 * selftest.c - Bare-metal 32-bit PowerPC self-test for the UAM ppc-ref board.
 *
 * Exercises the emulator end to end: UART output, integer arithmetic with
 * XER[CA], XER[OV] and XER[SO] and the record forms, compares, logical, rotate
 * and shift instructions, the condition register instructions, branches with the
 * count register, every load and store form (update, indexed, byte-reversed,
 * multiple, string, misaligned), lwarx/stwcx., dcbz, synchronous exceptions
 * (trap, system call, illegal and privileged instructions, alignment, machine
 * check, FP unavailable) with SRR0/SRR1/DAR/DSISR, problem state, the
 * decrementer and time base, MSR[POW] sleep, the external interrupt (from the
 * UART), the MMU (BATs, segments, the hashed page table with primary and
 * secondary groups, referenced and changed bits, protection keys, no-execute and
 * guarded pages, tlbie), the FPU (arithmetic in double and single precision,
 * conversions, compares, estimates, the FPSCR and its exceptions), the trace
 * exception and the identification registers.
 *
 * The result is reported through the board's test finisher.
 */
#include <stdint.h>

#define UART0 0xf0000000u
#define UART_THR (*(volatile uint8_t *)(UART0 + 0))
#define UART_IER (*(volatile uint8_t *)(UART0 + 1))
#define UART_LSR (*(volatile uint8_t *)(UART0 + 5))

static void putc_(char c) { while (!(UART_LSR & 0x20)) {} UART_THR = (uint8_t)c; }
static void puts_(const char *s) { for (; *s; s++) { if (*s == '\n') putc_('\r'); putc_(*s); } }
static void puthex(uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    puts_("0x");
    for (int i = 28; i >= 0; i -= 4)
        putc_(d[(v >> i) & 0xF]);
}

static unsigned n_pass, n_fail;
static const char *current;

static void check(int ok, const char *what, uint32_t got, uint32_t want)
{
    if (ok)
        return;
    n_fail++;
    puts_("  FAIL ");
    puts_(current);
    puts_(": ");
    puts_(what);
    puts_(" got ");
    puthex(got);
    puts_(" want ");
    puthex(want);
    puts_("\n");
}
#define EQ(got, want, what) do { uint32_t g_ = (got), w_ = (want); check(g_ == w_, what, g_, w_); } while (0)
#define EQ64(got, want, what) do { uint64_t g6_ = (got), w6_ = (want); \
        check(g6_ == w6_, what " (high)", (uint32_t)(g6_ >> 32), (uint32_t)(w6_ >> 32)); \
        check(g6_ == w6_, what " (low)", (uint32_t)g6_, (uint32_t)w6_); } while (0)

static void run(const char *name, void (*fn)(void))
{
    unsigned before = n_fail;
    current = name;
    fn();
    puts_(n_fail == before ? "[ ok ] " : "[FAIL] ");
    puts_(name);
    puts_("\n");
    if (n_fail == before)
        n_pass++;
}

/* Operands the compiler cannot fold. */
static volatile uint32_t V0 = 0, V1 = 1, V2 = 2, V3 = 3, V5 = 5, V7 = 7, VM1 = 0xFFFFFFFFu,
                         VMIN = 0x80000000u, VMAX = 0x7FFFFFFFu;

/* ---- special registers --------------------------------------------------- */
#define mfspr(n) ({ uint32_t v_; __asm__ volatile("mfspr %0, %1" : "=r"(v_) : "i"(n)); v_; })
#define mtspr(n, v) __asm__ volatile("mtspr %0, %1" :: "i"(n), "r"((uint32_t)(v)) : "memory")
static inline uint32_t mfmsr(void) { uint32_t v; __asm__ volatile("mfmsr %0" : "=r"(v)); return v; }
static inline void mtmsr(uint32_t v) { __asm__ volatile("mtmsr %0; isync" :: "r"(v) : "memory"); }
static inline uint32_t mfcr(void) { uint32_t v; __asm__ volatile("mfcr %0" : "=r"(v)); return v; }
static inline uint32_t mfxer(void) { uint32_t v; __asm__ volatile("mfxer %0" : "=r"(v)); return v; }
static inline void mtxer(uint32_t v) { __asm__ volatile("mtxer %0" :: "r"(v) : "xer"); }

#define MSR_EE  0x8000u
#define MSR_PR  0x4000u
#define MSR_FP  0x2000u
#define MSR_ME  0x1000u
#define MSR_FE0 0x0800u
#define MSR_SE  0x0400u
#define MSR_BE  0x0200u
#define MSR_FE1 0x0100u
#define MSR_IR  0x0020u
#define MSR_DR  0x0010u
#define MSR_POW 0x40000u

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

/* ---- traps ---------------------------------------------------------------- */
typedef struct frame {
    uint32_t r[32];
    uint32_t cr, lr, ctr, xer, srr0, srr1, dar, dsisr, vec;
} frame;

static volatile uint32_t trap_n, last_vec, last_srr0, last_srr1, last_dar, last_dsisr;
static volatile uint32_t dec_n, ext_n, trace_n, user_r4;
static volatile uint32_t trap_srr1[4];        /* SRR1 of the first traps since trap_n = 0 */
static volatile int user_test;
void user_resume(void);
void enter_user(uint32_t pc, uint32_t arg);

uint32_t trap_handler(frame *f, uint32_t vec);
uint32_t trap_handler(frame *f, uint32_t vec)
{
    switch (vec) {
    case 0x900:                 /* decrementer: count, push it far out */
        dec_n++;
        mtspr(22, 0x7FFFFFFFu);
        return f->srr0;
    case 0x500:                 /* external: the UART's THRE interrupt; silence it */
        ext_n++;
        UART_IER = 0;
        return f->srr0;
    case 0xD00:                 /* trace */
        trace_n++;
        return f->srr0;
    default:
        break;
    }
    if (trap_n < 4)
        trap_srr1[trap_n] = f->srr1;
    trap_n++;
    last_vec = vec;
    last_srr0 = f->srr0;
    last_srr1 = f->srr1;
    last_dar = f->dar;
    last_dsisr = f->dsisr;
    if (vec == 0xC00) {         /* system call: SRR0 is already the next instruction */
        if (user_test) {
            user_test = 0;
            user_r4 = f->r[4];
            f->srr1 &= ~MSR_PR;
            return (uint32_t)(uintptr_t)user_resume;
        }
        return f->srr0;
    }
    if (vec == 0x400)           /* ISI: back to the caller that branched there */
        return f->lr;
    return f->srr0 + 4;         /* skip the faulting instruction */
}

/* One synchronous exception since trap_n was cleared, at `vec`. */
static void expect_trap(uint32_t vec, const char *what)
{
    EQ(trap_n, 1, what);
    EQ(last_vec, vec, what);
    trap_n = 0;
}

/* ---- integer arithmetic --------------------------------------------------- */

typedef struct res { uint32_t r, xer, cr0; } res;

#define ARITH2(insn, a, b, xin) ({ uint32_t r_, x_, c_; \
    __asm__ volatile("mtxer %5\n\t" insn " %0,%3,%4\n\tmfxer %1\n\tmfcr %2" \
        : "=&r"(r_), "=&r"(x_), "=&r"(c_) : "r"(a), "r"(b), "r"(xin) : "cr0", "xer"); \
    (res){ r_, x_, c_ >> 28 }; })
#define ARITH1(insn, a, xin) ({ uint32_t r_, x_, c_; \
    __asm__ volatile("mtxer %4\n\t" insn " %0,%3\n\tmfxer %1\n\tmfcr %2" \
        : "=&r"(r_), "=&r"(x_), "=&r"(c_) : "r"(a), "r"(xin) : "cr0", "xer"); \
    (res){ r_, x_, c_ >> 28 }; })
#define ARITHI(insn, a, imm, xin) ({ uint32_t r_, x_, c_; \
    __asm__ volatile("mtxer %4\n\t" insn " %0,%3," #imm "\n\tmfxer %1\n\tmfcr %2" \
        : "=&r"(r_), "=&r"(x_), "=&r"(c_) : "b"(a), "r"(xin) : "cr0", "xer"); \
    (res){ r_, x_, c_ >> 28 }; })

static void t_arith(void)
{
    res r;
    r = ARITH2("add", V7, V3, 0);           EQ(r.r, 10, "add");
    r = ARITH2("addc", VM1, V1, 0);         EQ(r.r, 0, "addc"); EQ(r.xer, XER_CA, "addc ca");
    r = ARITH2("addc", V7, V1, XER_CA);     EQ(r.xer, 0, "addc clears ca");
    r = ARITH2("adde", VM1, V0, XER_CA);    EQ(r.r, 0, "adde"); EQ(r.xer, XER_CA, "adde ca");
    r = ARITH2("adde", V1, V2, XER_CA);     EQ(r.r, 4, "adde +ca"); EQ(r.xer, 0, "adde no ca");
    r = ARITH2("subf", V1, V0, 0);          EQ(r.r, 0xFFFFFFFFu, "subf");
    r = ARITH2("subfc", V1, V0, 0);         EQ(r.r, 0xFFFFFFFFu, "subfc"); EQ(r.xer, 0, "subfc borrow");
    r = ARITH2("subfc", V1, V5, 0);         EQ(r.r, 4, "subfc 5-1"); EQ(r.xer, XER_CA, "subfc no borrow");
    r = ARITH2("subfe", V1, V5, 0);         EQ(r.r, 3, "subfe"); EQ(r.xer, XER_CA, "subfe ca");
    r = ARITH1("addme", V0, 0);             EQ(r.r, 0xFFFFFFFFu, "addme"); EQ(r.xer, 0, "addme ca");
    r = ARITH1("addme", V5, XER_CA);        EQ(r.r, 5, "addme +ca"); EQ(r.xer, XER_CA, "addme ca out");
    r = ARITH1("addze", VM1, XER_CA);       EQ(r.r, 0, "addze"); EQ(r.xer, XER_CA, "addze ca");
    r = ARITH1("subfme", V0, XER_CA);       EQ(r.r, 0xFFFFFFFFu, "subfme"); EQ(r.xer, XER_CA, "subfme ca");
    r = ARITH1("subfze", V0, XER_CA);       EQ(r.r, 0, "subfze"); EQ(r.xer, XER_CA, "subfze ca");
    r = ARITH1("neg", V5, 0);               EQ(r.r, (uint32_t)-5, "neg");
    r = ARITH1("nego", VMIN, 0);            EQ(r.r, VMIN, "nego min"); EQ(r.xer, XER_SO | XER_OV, "nego ov");
    r = ARITH2("addo.", VMAX, V1, 0);       EQ(r.r, VMIN, "addo."); EQ(r.xer, XER_SO | XER_OV, "addo. ov");
    EQ(r.cr0, 0x9, "addo. cr0 lt so");
    r = ARITH2("addo", V1, V1, XER_SO);     EQ(r.xer, XER_SO, "so is sticky, ov cleared");
    r = ARITH2("subfo", V1, VMIN, 0);       EQ(r.r, VMAX, "subfo"); EQ(r.xer, XER_SO | XER_OV, "subfo ov");
    r = ARITH2("add.", V1, VM1, 0);         EQ(r.cr0, 0x2, "add. eq");
    r = ARITH2("add.", V1, V1, XER_SO);     EQ(r.cr0, 0x5, "add. gt so");
    r = ARITHI("addic.", VM1, 1, 0);        EQ(r.r, 0, "addic."); EQ(r.xer, XER_CA, "addic. ca");
    EQ(r.cr0, 0x2, "addic. eq");
    r = ARITHI("subfic", V3, 5, 0);         EQ(r.r, 2, "subfic"); EQ(r.xer, XER_CA, "subfic ca");
    r = ARITHI("subfic", V7, 5, 0);         EQ(r.r, (uint32_t)-2, "subfic neg"); EQ(r.xer, 0, "subfic borrow");
    r = ARITHI("mulli", V7, -3, 0);         EQ(r.r, (uint32_t)-21, "mulli");

    r = ARITH2("mullw", V7, V3, 0);         EQ(r.r, 21, "mullw");
    r = ARITH2("mullwo", 0x10000u, 0x10000u, 0); EQ(r.r, 0, "mullwo"); EQ(r.xer, XER_SO | XER_OV, "mullwo ov");
    r = ARITH2("mullwo", VM1, VM1, 0);      EQ(r.xer, 0, "mullwo -1*-1 no ov");
    r = ARITH2("mulhw", VM1, VM1, 0);       EQ(r.r, 0, "mulhw");
    r = ARITH2("mulhwu", VM1, VM1, 0);      EQ(r.r, 0xFFFFFFFEu, "mulhwu");
    r = ARITH2("mulhw", VMIN, V2, 0);       EQ(r.r, 0xFFFFFFFFu, "mulhw neg");
    r = ARITH2("divw", (uint32_t)-7, V2, 0); EQ(r.r, (uint32_t)-3, "divw");
    r = ARITH2("divwu", V7, V3, 0);         EQ(r.r, 2, "divwu");
    r = ARITH2("divwo", V7, V0, 0);         EQ(r.xer, XER_SO | XER_OV, "divwo /0");
    r = ARITH2("divwo", VMIN, VM1, 0);      EQ(r.xer, XER_SO | XER_OV, "divwo min/-1");
    r = ARITH2("divwuo", V7, V0, 0);        EQ(r.xer, XER_SO | XER_OV, "divwuo /0");
    r = ARITH2("divwo", V7, V2, 0);         EQ(r.xer, 0, "divwo ok");

    /* compares */
    uint32_t cr;
    __asm__ volatile("cmpw cr3,%1,%2\n mfcr %0" : "=r"(cr) : "r"(VM1), "r"(V1) : "cr3");
    EQ((cr >> 16) & 0xF, 0x8, "cmpw lt");
    __asm__ volatile("cmplw cr3,%1,%2\n mfcr %0" : "=r"(cr) : "r"(VM1), "r"(V1) : "cr3");
    EQ((cr >> 16) & 0xF, 0x4, "cmplw gt");
    __asm__ volatile("cmpwi cr7,%1,7\n mfcr %0" : "=r"(cr) : "r"(V7) : "cr7");
    EQ(cr & 0xF, 0x2, "cmpwi eq");
    __asm__ volatile("cmplwi cr1,%1,0xffff\n mfcr %0" : "=r"(cr) : "r"(V3) : "cr1");
    EQ((cr >> 24) & 0xF, 0x8, "cmplwi lt");
    mtxer(XER_SO);
    __asm__ volatile("cmpw cr3,%1,%2\n mfcr %0" : "=r"(cr) : "r"(V1), "r"(V1) : "cr3");
    EQ((cr >> 16) & 0xF, 0x3, "cmpw eq so");
    mtxer(0);
}

static void t_logic(void)
{
    const uint32_t a = 0xF0F0F0F0u, b = 0xFF00FF00u;
    volatile uint32_t va = a, vb = b;
    uint32_t r;
#define L2(insn, want) do { __asm__ volatile(insn " %0,%1,%2" : "=r"(r) : "r"(va), "r"(vb)); \
                            EQ(r, want, insn); } while (0)
    L2("and", 0xF000F000u);
    L2("andc", 0x00F000F0u);
    L2("or", 0xFFF0FFF0u);
    L2("orc", 0xF0FFF0FFu);
    L2("xor", 0x0FF00FF0u);
    L2("nand", 0x0FFF0FFFu);
    L2("nor", 0x000F000Fu);
    L2("eqv", 0xF00FF00Fu);
#undef L2
    __asm__ volatile("extsb %0,%1" : "=r"(r) : "r"(0x1280u)); EQ(r, 0xFFFFFF80u, "extsb");
    __asm__ volatile("extsh %0,%1" : "=r"(r) : "r"(0x18000u)); EQ(r, 0xFFFF8000u, "extsh");
    __asm__ volatile("cntlzw %0,%1" : "=r"(r) : "r"(V1)); EQ(r, 31, "cntlzw 1");
    __asm__ volatile("cntlzw %0,%1" : "=r"(r) : "r"(V0)); EQ(r, 32, "cntlzw 0");
    uint32_t cr;
    __asm__ volatile("andi. %0,%2,0x0f0f\n mfcr %1" : "=r"(r), "=r"(cr) : "r"(0x1234u) : "cr0");
    EQ(r, 0x0204, "andi."); EQ(cr >> 28, 0x4, "andi. gt");
    __asm__ volatile("andis. %0,%2,0x00ff\n mfcr %1" : "=r"(r), "=r"(cr) : "r"(0x12345678u) : "cr0");
    EQ(r, 0x00340000u, "andis.");
    __asm__ volatile("andi. %0,%2,0\n mfcr %1" : "=r"(r), "=r"(cr) : "r"(VM1) : "cr0");
    EQ(cr >> 28, 0x2, "andi. eq");
    __asm__ volatile("ori %0,%1,0x8001" : "=r"(r) : "r"(0x10000u)); EQ(r, 0x18001u, "ori");
    __asm__ volatile("oris %0,%1,0x8001" : "=r"(r) : "r"(1u)); EQ(r, 0x80010001u, "oris");
    __asm__ volatile("xori %0,%1,0xffff" : "=r"(r) : "r"(VM1)); EQ(r, 0xFFFF0000u, "xori");
    __asm__ volatile("xoris %0,%1,0xffff" : "=r"(r) : "r"(VM1)); EQ(r, 0x0000FFFFu, "xoris");
    __asm__ volatile("lis %0,0xabcd" : "=r"(r)); EQ(r, 0xABCD0000u, "lis");
    __asm__ volatile("li %0,-2" : "=r"(r)); EQ(r, 0xFFFFFFFEu, "li");
}

static void t_rotate(void)
{
    volatile uint32_t x = 0x12345678u;
    uint32_t r, xer;
    __asm__ volatile("rlwinm %0,%1,8,0,31" : "=r"(r) : "r"(x)); EQ(r, 0x34567812u, "rotlwi");
    __asm__ volatile("rlwinm %0,%1,4,0,27" : "=r"(r) : "r"(x)); EQ(r, 0x23456780u, "slwi");
    __asm__ volatile("rlwinm %0,%1,28,4,31" : "=r"(r) : "r"(x)); EQ(r, 0x01234567u, "srwi");
    __asm__ volatile("rlwinm %0,%1,0,28,3" : "=r"(r) : "r"(VM1)); EQ(r, 0xF000000Fu, "wrapped mask");
    __asm__ volatile("rlwnm %0,%1,%2,0,31" : "=r"(r) : "r"(x), "r"(36u)); EQ(r, 0x23456781u, "rlwnm");
    r = 0xAAAAAAAAu;
    __asm__ volatile("rlwimi %0,%1,8,16,23" : "+r"(r) : "r"(0xFFu)); EQ(r, 0xAAAAFFAAu, "rlwimi");
    __asm__ volatile("slw %0,%1,%2" : "=r"(r) : "r"(V1), "r"(31u)); EQ(r, 0x80000000u, "slw 31");
    __asm__ volatile("slw %0,%1,%2" : "=r"(r) : "r"(V1), "r"(32u)); EQ(r, 0, "slw 32");
    __asm__ volatile("srw %0,%1,%2" : "=r"(r) : "r"(VMIN), "r"(31u)); EQ(r, 1, "srw 31");
    __asm__ volatile("srw %0,%1,%2" : "=r"(r) : "r"(VMIN), "r"(33u)); EQ(r, 0, "srw 33");
#define SRA(insn, a, n, wr, wx, what) do { \
        __asm__ volatile("mtxer %3\n " insn " %0,%2,%4\n mfxer %1" \
                         : "=&r"(r), "=&r"(xer) : "r"(a), "r"(0u), "r"(n) : "xer"); \
        EQ(r, wr, what); EQ(xer, wx, what " ca"); } while (0)
    SRA("sraw", VMIN, 4u, 0xF8000000u, 0, "sraw");
    SRA("sraw", 0x80000001u, 4u, 0xF8000000u, XER_CA, "sraw lost bits");
    SRA("sraw", VM1, 40u, 0xFFFFFFFFu, XER_CA, "sraw 40 neg");
    SRA("sraw", V5, 40u, 0, 0, "sraw 40 pos");
#undef SRA
    __asm__ volatile("mtxer %2\n srawi %0,%3,1\n mfxer %1" : "=&r"(r), "=&r"(xer) : "r"(0u), "r"((uint32_t)-5) : "xer");
    EQ(r, (uint32_t)-3, "srawi"); EQ(xer, XER_CA, "srawi ca");
}

static void t_cr(void)
{
    uint32_t cr;
    __asm__ volatile(
        "mtcrf 0xff,%1\n"
        "crand 31,0,2\n"
        "cror 30,1,3\n"
        "crxor 29,0,2\n"
        "crnand 28,0,2\n"
        "crnor 27,1,3\n"
        "creqv 26,0,1\n"
        "crandc 25,0,1\n"
        "crorc 24,1,0\n"
        "creqv 4,4,4\n"
        "mcrf cr7,cr0\n"
        "mfcr %0"
        : "=r"(cr) : "r"(0xA0000000u) : "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7");
    EQ(cr, 0xA800005Au, "cr logic");
    __asm__ volatile("mtcrf 0xff,%1\n mtcrf 0x01,%2\n mfcr %0" : "=r"(cr)
                     : "r"(0x12345678u), "r"(VM1) : "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7");
    EQ(cr, 0x1234567Fu, "mtcrf");
    uint32_t xer;
    __asm__ volatile("mtxer %2\n mcrxr cr2\n mfcr %0\n mfxer %1" : "=r"(cr), "=r"(xer)
                     : "r"(0xE0000000u) : "cr2", "xer");
    EQ((cr >> 20) & 0xF, 0xE, "mcrxr"); EQ(xer, 0, "mcrxr clears");
}

static uint32_t leaf(uint32_t x) { return x * 3; }
static uint32_t (*volatile leaf_ptr)(uint32_t) = leaf;

static void t_branch(void)
{
    uint32_t n, ctr;
    __asm__ volatile("li %0,0\n mtctr %2\n 1: addi %0,%0,1\n bdnz 1b\n mfctr %1"
                     : "=&r"(n), "=r"(ctr) : "r"(10u) : "ctr");
    EQ(n, 10, "bdnz loop"); EQ(ctr, 0, "ctr");
    __asm__ volatile("li %0,0\n mtctr %1\n bdz 1f\n li %0,5\n 1:" : "=&r"(n) : "r"(1u) : "ctr");
    EQ(n, 0, "bdz taken");
    __asm__ volatile("li %0,0\n mtctr %1\n bdnz 1f\n li %0,5\n 1:" : "=&r"(n) : "r"(1u) : "ctr");
    EQ(n, 5, "bdnz not taken");
    /* bdnzf: decrement and branch while the condition is false */
    __asm__ volatile("li %0,0\n cmpwi cr1,%1,1\n mtctr %2\n"
                     "1: addi %0,%0,1\n bdnzf 4*cr1+eq,1b" : "=&r"(n) : "r"(V1), "r"(4u) : "ctr", "cr1");
    EQ(n, 1, "bdnzf stops on the condition");
    uint32_t pc, lbl;
    __asm__ volatile("bcl 20,31,1f\n 1: mflr %0\n lis %1,1b@ha\n addi %1,%1,1b@l"
                     : "=r"(pc), "=b"(lbl) :: "lr");
    EQ(pc, lbl, "bcl 20,31 gives the pc");
    EQ(leaf_ptr(V7), 21, "bctrl");
    __asm__ volatile("li %0,0\n mtctr %1\n bdnzlr\n li %0,9" : "=&r"(n) : "r"(1u) : "ctr");
    EQ(n, 9, "bdnzlr falls through");
    uint32_t taken = 0;
    for (volatile int i = 0; i < 10; i++)
        taken += i & 1;
    EQ(taken, 5, "loop");
}

static void t_loadstore(void)
{
    static volatile uint8_t buf[32] __attribute__((aligned(16)));
    for (int i = 0; i < 32; i++)
        buf[i] = (uint8_t)(0x10 + i);
    uint8_t *p = (uint8_t *)buf;
    uint32_t r, q;
    __asm__ volatile("lbz %0,3(%1)" : "=r"(r) : "b"(p)); EQ(r, 0x13, "lbz");
    __asm__ volatile("lhz %0,2(%1)" : "=r"(r) : "b"(p)); EQ(r, 0x1213, "lhz");
    buf[30] = 0x80;
    __asm__ volatile("lha %0,30(%1)" : "=r"(r) : "b"(p)); EQ(r, 0xFFFF802Fu, "lha");
    __asm__ volatile("lwz %0,4(%1)" : "=r"(r) : "b"(p)); EQ(r, 0x14151617u, "lwz");
    __asm__ volatile("lwz %0,1(%1)" : "=r"(r) : "b"(p)); EQ(r, 0x11121314u, "lwz misaligned");
    __asm__ volatile("lhbrx %0,%1,%2" : "=r"(r) : "b"(p), "r"(2u)); EQ(r, 0x1312, "lhbrx");
    __asm__ volatile("lwbrx %0,%1,%2" : "=r"(r) : "b"(p), "r"(4u)); EQ(r, 0x17161514u, "lwbrx");
    __asm__ volatile("lwzx %0,%1,%2" : "=r"(r) : "b"(p), "r"(8u)); EQ(r, 0x18191A1Bu, "lwzx");
    uint8_t *u = p;
    __asm__ volatile("lwzu %0,4(%1)" : "=r"(r), "+b"(u)); EQ(r, 0x14151617u, "lwzu");
    EQ((uint32_t)(u - p), 4, "lwzu updates");
    __asm__ volatile("lbzux %0,%1,%2" : "=r"(r), "+b"(u) : "r"(2u)); EQ(r, 0x16, "lbzux");
    EQ((uint32_t)(u - p), 6, "lbzux updates");
    __asm__ volatile("lhau %0,2(%1)" : "=r"(r), "+b"(u)); EQ(r, 0x1819, "lhau");

    __asm__ volatile("stb %0,0(%1)" :: "r"(0xAAu), "b"(p) : "memory"); EQ(buf[0], 0xAA, "stb");
    __asm__ volatile("sth %0,0(%1)" :: "r"(0xBBCCu), "b"(p) : "memory");
    EQ(buf[0] << 8 | buf[1], 0xBBCC, "sth");
    __asm__ volatile("stw %0,1(%1)" :: "r"(0x01020304u), "b"(p) : "memory");
    EQ(buf[1] << 24 | buf[2] << 16 | buf[3] << 8 | buf[4], 0x01020304u, "stw misaligned");
    __asm__ volatile("stwbrx %0,%1,%2" :: "r"(0x01020304u), "b"(p), "r"(8u) : "memory");
    EQ(buf[8] << 24 | buf[9] << 16 | buf[10] << 8 | buf[11], 0x04030201u, "stwbrx");
    __asm__ volatile("sthbrx %0,%1,%2" :: "r"(0x0506u), "b"(p), "r"(12u) : "memory");
    EQ(buf[12] << 8 | buf[13], 0x0605, "sthbrx");
    u = p;
    __asm__ volatile("stwu %1,16(%0)" : "+b"(u) : "r"(0xDEADBEEFu) : "memory");
    EQ((uint32_t)(u - p), 16, "stwu updates");
    EQ(buf[16] << 24 | buf[17] << 16 | buf[18] << 8 | buf[19], 0xDEADBEEFu, "stwu stores");

    /* lmw / stmw */
    static volatile uint32_t words[4] __attribute__((aligned(16)));
    __asm__ volatile("li 28,1\n li 29,2\n li 30,3\n li 31,4\n stmw 28,0(%0)"
                     :: "b"(words) : "r28", "r29", "r30", "r31", "memory");
    EQ(words[0] + words[1] * 10 + words[2] * 100 + words[3] * 1000, 4321, "stmw");
    words[2] = 77;
    __asm__ volatile("lmw 29,4(%2)\n mr %0,30\n mr %1,31" : "=r"(r), "=r"(q) : "b"(words)
                     : "r29", "r30", "r31");
    EQ(r, 77, "lmw r30"); EQ(q, 4, "lmw r31");

    /* strings */
    for (int i = 0; i < 32; i++)
        buf[i] = (uint8_t)(0x40 + i);
    __asm__ volatile("lswi 5,%2,7\n mr %0,5\n mr %1,6" : "=r"(r), "=r"(q) : "b"(p) : "r5", "r6");
    EQ(r, 0x40414243u, "lswi first"); EQ(q, 0x44454600u, "lswi last zero filled");
    __asm__ volatile("mtxer %3\n lswx 5,%2,%4\n mr %0,5\n mr %1,6"
                     : "=&r"(r), "=&r"(q) : "b"(p), "r"(5u), "r"(8u) : "r5", "r6", "xer");
    EQ(r, 0x48494A4Bu, "lswx first"); EQ(q, 0x4C000000u, "lswx last");
    __asm__ volatile("lis 7,0x0102\n ori 7,7,0x0304\n lis 8,0x0506\n stswi 7,%0,6"
                     :: "b"(p + 20) : "r7", "r8", "memory");
    EQ(buf[20] << 24 | buf[21] << 16 | buf[22] << 8 | buf[23], 0x01020304u, "stswi");
    EQ(buf[24] << 8 | buf[25], 0x0506, "stswi tail");
    EQ(buf[26], 0x5A, "stswi stops");
    __asm__ volatile("mtxer %1\n lis 7,0x0a0b\n stswx 7,0,%0" :: "r"(p + 28), "r"(2u)
                     : "r7", "xer", "memory");
    EQ(buf[28] << 8 | buf[29], 0x0A0B, "stswx"); EQ(buf[30], 0x5E, "stswx stops");
}

static void t_atomic(void)
{
    static volatile uint32_t w __attribute__((aligned(8))) = 5;
    uint32_t old, cr;
    __asm__ volatile("lwarx %0,0,%2\n addi %0,%0,1\n stwcx. %0,0,%2\n mfcr %1"
                     : "=&r"(old), "=&r"(cr) : "r"(&w) : "cr0", "memory");
    EQ(w, 6, "lwarx/stwcx."); EQ(cr >> 28, 0x2, "stwcx. succeeds");
    __asm__ volatile("stwcx. %1,0,%2\n mfcr %0" : "=r"(cr) : "r"(9u), "r"(&w) : "cr0", "memory");
    EQ(w, 6, "no reservation: no store"); EQ(cr >> 28, 0x0, "stwcx. fails");
    EQ(__atomic_fetch_add(&w, 2, __ATOMIC_SEQ_CST), 6, "atomic add");
    uint32_t expect = 8;
    EQ(__atomic_compare_exchange_n(&w, &expect, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST), 1, "cas");
    EQ(w, 1, "cas stored");
}

static void t_dcbz(void)
{
    static volatile uint8_t blk[96] __attribute__((aligned(32)));
    for (int i = 0; i < 96; i++)
        blk[i] = 0xFF;
    __asm__ volatile("dcbz 0,%0" :: "r"(&blk[40]) : "memory");
    int ok = 1;
    for (int i = 0; i < 96; i++)
        ok &= blk[i] == ((i >= 32 && i < 64) ? 0 : 0xFF);
    EQ(ok, 1, "dcbz clears its 32-byte block");
    __asm__ volatile("dcbf 0,%0\n dcbst 0,%0\n dcbt 0,%0\n icbi 0,%0\n sync\n isync\n eieio"
                     :: "r"(&blk[0]) : "memory");
}

/* ---- exceptions ---------------------------------------------------------- */

void user_priv(void);
void user_badspr(void);
__asm__(".text\n"
        "user_priv:   mfmsr 4\n sc\n"
        "user_badspr: mfspr 4,256\n sc\n");

static void t_exceptions(void)
{
    uint32_t pc;
    trap_n = 0;
    __asm__ volatile("lis %0,1f@ha\n addi %0,%0,1f@l\n 1: tw 4,%0,%0" : "=&b"(pc) :: "memory");
    expect_trap(0x700, "tw");
    EQ(last_srr0, pc, "tw srr0"); EQ(last_srr1 & 0xFFFF0000u, 0x00020000u, "srr1 trap");
    EQ(last_srr1 & 0xFFFFu, mfmsr(), "srr1 holds the msr");
    __asm__ volatile("twi 4,%0,0" :: "r"(V1));
    EQ(trap_n, 0, "twi false");
    __asm__ volatile("twgti %0,0" :: "r"(V1));
    expect_trap(0x700, "twgti");
    __asm__ volatile("lis %0,1f@ha\n addi %0,%0,1f@l\n sc\n 1:" : "=&b"(pc) :: "memory");
    expect_trap(0xC00, "sc"); EQ(last_srr0, pc, "sc srr0 is the next instruction");
    __asm__ volatile(".long 0");
    expect_trap(0x700, "illegal"); EQ(last_srr1 & 0xFFFF0000u, 0x00080000u, "srr1 illegal");
    __asm__ volatile(".long 0xe8030000");   /* ld r0,0(r3): 64-bit only */
    expect_trap(0x700, "64-bit instruction");

    /* machine check: a load nothing answers */
    __asm__ volatile("lis %0,1f@ha\n addi %0,%0,1f@l\n lis 9,0xe000\n 1: lwz 9,0(9)"
                     : "=&b"(pc) :: "r9", "memory");
    expect_trap(0x200, "machine check"); EQ(last_srr0, pc, "machine check srr0");
    EQ(mfmsr() & MSR_ME, MSR_ME, "rfi restores ME");

    /* alignment */
    static volatile uint32_t al[4] __attribute__((aligned(16)));
    __asm__ volatile("lwarx 5,0,%0" :: "r"((uintptr_t)al + 1) : "r5", "memory");
    expect_trap(0x600, "lwarx misaligned");
    EQ(last_dar, (uint32_t)(uintptr_t)al + 1, "alignment dar");
    EQ(last_dsisr, 5u << 5, "alignment dsisr (lwarx: RT)");
    __asm__ volatile("lmw 30,2(%0)" :: "b"(al) : "r30", "r31", "memory");
    expect_trap(0x600, "lmw misaligned");
    EQ(last_dsisr & 0x3FE0u, 0x1C00u | (30u << 5), "alignment dsisr (lmw)");

    /* FP unavailable */
    uint32_t m = mfmsr();
    mtmsr(m & ~MSR_FP);
    __asm__ volatile("fmr 0,0");
    mtmsr(m);
    expect_trap(0x800, "fp unavailable");

    /* problem state: a privileged instruction, then sc back */
    user_test = 1;
    enter_user((uint32_t)(uintptr_t)user_priv, 0);
    EQ(trap_n, 2, "user: two traps");
    EQ(trap_srr1[0] & 0xFFFF0000u, 0x00040000u, "srr1 privileged");
    EQ(trap_srr1[0] & MSR_PR, MSR_PR, "from problem state");
    EQ(last_vec, 0xC00, "user: sc returns");
    EQ(mfmsr() & MSR_PR, 0, "back in supervisor state");
    trap_n = 0;
    /* an SPR the processor does not have */
    user_test = 1;
    enter_user((uint32_t)(uintptr_t)user_badspr, 0);
    EQ(trap_n, 2, "user spr: two traps");
    EQ(trap_srr1[0] & 0xFFFF0000u, 0x00080000u, "srr1 illegal spr");
    trap_n = 0;
}

/* ---- timers and interrupts ----------------------------------------------- */

static uint32_t mftb(void) { uint32_t v; __asm__ volatile("mftb %0" : "=r"(v)); return v; }
static uint32_t mftbu(void) { uint32_t v; __asm__ volatile("mftbu %0" : "=r"(v)); return v; }

static void t_timer(void)
{
    uint32_t t0 = mftb();
    for (volatile int i = 0; i < 100; i++) {}
    uint32_t t1 = mftb();
    EQ(t1 > t0, 1, "time base advances");
    mtspr(284, 0);
    mtspr(285, 0x12u);
    EQ(mftbu(), 0x12, "tbu written");
    EQ(mfspr(269), 0x12, "mfspr tbu");
    mtspr(22, 1000);
    uint32_t d0 = mfspr(22);
    EQ(d0 <= 1000 && d0 > 900, 1, "dec counts down");

    dec_n = 0;
    uint32_t m = mfmsr();
    mtspr(22, 200);
    mtmsr(m | MSR_EE);
    for (volatile int i = 0; i < 100000 && !dec_n; i++) {}
    mtmsr(m);
    EQ(dec_n, 1, "decrementer interrupt");
    EQ(mfspr(22) > 0x70000000u, 1, "handler rewrote dec");

    /* MSR[POW]: sleep until the decrementer */
    dec_n = 0;
    mtspr(22, 5000);
    __asm__ volatile("mtmsr %0\n isync" :: "r"(m | MSR_EE | MSR_POW) : "memory");
    mtmsr(m);
    EQ(dec_n, 1, "pow wakes on the decrementer");
    EQ(mfmsr() & MSR_POW, 0, "pow cleared");

    /* no interrupt while EE = 0; pending until enabled */
    dec_n = 0;
    mtspr(22, 10);
    for (volatile int i = 0; i < 1000; i++) {}
    EQ(dec_n, 0, "masked");
    mtmsr(m | MSR_EE);
    mtmsr(m);
    EQ(dec_n, 1, "taken when enabled");
}

static void t_external(void)
{
    ext_n = 0;
    uint32_t m = mfmsr();
    UART_IER = 0x02;          /* transmitter empty interrupt: asserted at once */
    mtmsr(m | MSR_EE);
    for (volatile int i = 0; i < 1000 && !ext_n; i++) {}
    mtmsr(m);
    EQ(ext_n, 1, "external interrupt");
    EQ(UART_IER, 0, "handler silenced it");
}

/* ---- the MMU --------------------------------------------------------------- */

#define HTAB 0x00400000u
static void pte_set(uint32_t ea, uint32_t vsid, uint32_t pa, uint32_t flags, int secondary, int slot)
{
    uint32_t pidx = (ea >> 12) & 0xFFFF;
    uint32_t hash = (vsid & 0x7FFFF) ^ pidx;
    if (secondary)
        hash = ~hash & 0x7FFFF;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(HTAB | ((hash & 0x3FF) << 6) | (8u * slot));
    p[1] = pa | flags;
    p[0] = 0x80000000u | (vsid << 7) | (secondary ? 0x40u : 0) | (pidx >> 10);
}

static volatile uint32_t *pte_w1(uint32_t ea, uint32_t vsid, int secondary, int slot)
{
    uint32_t pidx = (ea >> 12) & 0xFFFF, hash = (vsid & 0x7FFFF) ^ pidx;
    if (secondary)
        hash = ~hash & 0x7FFFF;
    return (volatile uint32_t *)(uintptr_t)(HTAB | ((hash & 0x3FF) << 6) | (8u * slot) | 4);
}

static inline uint32_t ld(uint32_t a) { uint32_t v; __asm__ volatile("lwz %0,0(%1)" : "=r"(v) : "b"(a) : "memory"); return v; }
static inline void st(uint32_t a, uint32_t v) { __asm__ volatile("stw %0,0(%1)" :: "r"(v), "b"(a) : "memory"); }
#define PW(a) (*(volatile uint32_t *)(uintptr_t)(a))

void user_load(void);
__asm__(".text\n user_load: li 4,0\n lwz 4,0(3)\n sc\n");

static void t_mmu(void)
{
    const uint32_t VS1 = 0x123, VS2 = 0x456;
    for (uint32_t a = HTAB; a < HTAB + 0x10000; a += 4)
        PW(a) = 0;
    mtspr(25, HTAB);                                        /* SDR1: 64 KiB table */
    mtspr(528, 0x00001FFFu); mtspr(529, 0x00000002u);       /* IBAT0: 0-256M, Vs Vp, rw */
    mtspr(536, 0x00001FFFu); mtspr(537, 0x00000002u);       /* DBAT0 */
    mtspr(538, 0xF0001FFEu); mtspr(539, 0xF000002Au);       /* DBAT1: devices, I G */
    __asm__ volatile("mtsr 1,%0\n mtsr 2,%1" :: "r"(0x20000000u | VS1), "r"(0x10000000u | VS2));

    PW(0x00500000) = 0x11111111u;
    PW(0x00501000) = 0x22222222u;
    PW(0x00502000) = 0x33333333u;
    PW(0x00505000) = 0x3860002Au;                           /* li r3,42 */
    PW(0x00505004) = 0x4E800020u;                           /* blr */
    PW(0x00505FFC) = 0x01020304u;
    pte_set(0x10000000, VS1, 0x00500000, 0x2, 0, 0);        /* rw */
    pte_set(0x10001000, VS1, 0x00501000, 0x3, 0, 0);        /* read only */
    pte_set(0x10002000, VS1, 0x00502000, 0x2, 1, 3);        /* secondary group */
    pte_set(0x10004000, VS1, 0x00505000, 0x8 | 0x2, 0, 0);  /* guarded */
    pte_set(0x10005000, VS1, 0x00505000, 0x2, 0, 0);        /* code */
    pte_set(0x10006000, VS1, 0x00506000, 0x0, 0, 0);        /* supervisor only */
    pte_set(0x20000000, VS2, 0x00505000, 0x2, 0, 0);        /* in a no-execute segment */

    uint32_t m = mfmsr();
    mtmsr(m | MSR_IR | MSR_DR);
    trap_n = 0;
    EQ(ld(0x10000000), 0x11111111u, "primary group");
    EQ(*pte_w1(0x10000000, VS1, 0, 0) & 0x180, 0x100, "referenced, not changed");
    st(0x10000004, 0x44);
    EQ(*pte_w1(0x10000000, VS1, 0, 0) & 0x180, 0x180, "changed after a store");
    EQ(PW(0x00500004), 0x44, "store reached the page");
    EQ(ld(0x10002000), 0x33333333u, "secondary group");
    EQ(ld(0x10001000), 0x22222222u, "read-only page reads");
    /* a word across two pages that map to unrelated frames */
    EQ(ld(0x10004FFE), 0x03043860u, "misaligned across pages");
    EQ(trap_n, 0, "no faults so far");
    st(0x10001000, 1);
    expect_trap(0x300, "store to a read-only page");
    EQ(last_dsisr, 0x0A000000u, "dsisr protection + store"); EQ(last_dar, 0x10001000u, "dar");
    (void)ld(0x10003008);
    expect_trap(0x300, "no pte");
    EQ(last_dsisr, 0x40000000u, "dsisr page fault"); EQ(last_dar, 0x10003008u, "dar no pte");
    (void)ld(0x10002FFE);
    expect_trap(0x300, "second page unmapped");
    EQ(last_dar, 0x10003000u, "dar: the page that faulted");
    uint32_t (*fn)(void) = (uint32_t (*)(void))0x10005000u;
    EQ(fn(), 42, "code through a page");
    fn = (uint32_t (*)(void))0x10004000u;
    (void)fn();
    expect_trap(0x400, "guarded page"); EQ(last_srr1 & 0xF8000000u, 0x10000000u, "isi g");
    EQ(last_srr0, 0x10004000u, "isi srr0");
    fn = (uint32_t (*)(void))0x20000000u;
    (void)fn();
    expect_trap(0x400, "no-execute segment"); EQ(last_srr1 & 0xF8000000u, 0x10000000u, "isi n");
    fn = (uint32_t (*)(void))0x10003000u;
    (void)fn();
    expect_trap(0x400, "isi no pte"); EQ(last_srr1 & 0xF8000000u, 0x40000000u, "isi page fault");

    /* a remapped page needs tlbie */
    *pte_w1(0x10000000, VS1, 0, 0) = 0x00501000u | 0x2;
    __asm__ volatile("sync\n tlbie %0\n sync\n tlbsync\n sync" :: "r"(0x10000000u) : "memory");
    EQ(ld(0x10000000), 0x22222222u, "remapped after tlbie");

    /* problem state: key Kp = 1 */
    user_test = 1;
    enter_user((uint32_t)(uintptr_t)user_load, 0x10006000u);
    EQ(trap_n, 2, "user: dsi + sc");
    EQ(user_r4, 0, "the load did not happen");
    trap_n = 0;
    user_test = 1;
    enter_user((uint32_t)(uintptr_t)user_load, 0x10002000u);
    EQ(trap_n, 1, "user: rw page");
    EQ(user_r4, 0x33333333u, "user load through the page table");
    trap_n = 0;

    uint32_t v;
    __asm__ volatile("mfsrin %0,%1" : "=r"(v) : "r"(0x10000000u)); EQ(v, 0x20000123u, "mfsrin");
    __asm__ volatile("mtsrin %1,%2\n mfsr %0,3" : "=r"(v) : "r"(0x777u), "r"(0x30000000u));
    EQ(v, 0x777u, "mtsrin");
    mtmsr(m);
    EQ(PW(0x00505000), 0x3860002Au, "back to real addressing");
    mtspr(528, 0); mtspr(536, 0); mtspr(538, 0);
}

/* ---- floating point ------------------------------------------------------- */

static inline uint64_t dbits(double d) { union { double d; uint64_t u; } x = { .d = d }; return x.u; }
static inline double bitsd(uint64_t u) { union { double d; uint64_t u; } x = { .u = u }; return x.d; }
static inline uint32_t fpscr(void) { double d; __asm__ volatile("mffs %0" : "=f"(d)); return (uint32_t)dbits(d); }
static inline void fpscr_set(uint32_t v) { double d = bitsd(v); __asm__ volatile("mtfsf 0xff,%0" :: "f"(d)); }
#define F2(insn, a, b) ({ double r_; __asm__ volatile(insn " %0,%1,%2" : "=f"(r_) : "f"(a), "f"(b)); r_; })
#define F1(insn, b) ({ double r_; __asm__ volatile(insn " %0,%1" : "=f"(r_) : "f"(b)); r_; })
#define F3(insn, a, c, b) ({ double r_; __asm__ volatile(insn " %0,%1,%2,%3" : "=f"(r_) : "f"(a), "f"(c), "f"(b)); r_; })
static inline uint32_t sbits(double d) { uint32_t v; __asm__ volatile("stfs %1,0(%0)" :: "b"(&v), "f"(d) : "memory"); return v; }
static inline uint32_t iwbits(double d) { uint32_t v; __asm__ volatile("stfiwx %1,0,%0" :: "r"(&v), "f"(d) : "memory"); return v; }

static volatile double D1 = 1.0, D2 = 2.0, D3 = 3.0, D4 = 4.0, D10 = 10.0, DH = 0.25, D15 = 1.5,
                       D225 = 2.25, DZ = 0.0, DM1 = -1.0, D27 = 2.7, D25 = 2.5, D35 = 3.5;

static void t_fpu(void)
{
    fpscr_set(0);
    EQ64(dbits(F2("fadd", D15, D225)), 0x400E000000000000ull, "fadd");
    EQ64(dbits(F2("fsub", D1, DH)), 0x3FE8000000000000ull, "fsub");
    EQ64(dbits(F2("fmul", D15, D4)), 0x4018000000000000ull, "fmul");
    EQ64(dbits(F2("fdiv", D10, D4)), 0x4004000000000000ull, "fdiv");
    EQ64(dbits(F1("fsqrt", D2)), 0x3FF6A09E667F3BCDull, "fsqrt");
    EQ64(dbits(F3("fmadd", D2, D3, D1)), 0x401C000000000000ull, "fmadd");
    EQ64(dbits(F3("fmsub", D2, D3, D1)), 0x4014000000000000ull, "fmsub");
    EQ64(dbits(F3("fnmadd", D2, D3, D1)), 0xC01C000000000000ull, "fnmadd");
    EQ64(dbits(F3("fnmsub", D2, D3, D1)), 0xC014000000000000ull, "fnmsub");
    EQ(sbits(F2("fdivs", D1, D3)), 0x3EAAAAABu, "fdivs");
    EQ64(dbits(F2("fdivs", D1, D3)), 0x3FD5555560000000ull, "fdivs is a single in double form");
    EQ(sbits(F1("frsp", F2("fdiv", D1, D3))), 0x3EAAAAABu, "frsp");
    EQ(iwbits(F1("fctiwz", -D27)), 0xFFFFFFFEu, "fctiwz");
    EQ(iwbits(F1("fctiw", D25)), 2, "fctiw ties to even");
    EQ(iwbits(F1("fctiw", D35)), 4, "fctiw 3.5");
    EQ64(dbits(F1("fabs", DM1)), 0x3FF0000000000000ull, "fabs");
    EQ64(dbits(F1("fneg", D1)), 0xBFF0000000000000ull, "fneg");
    EQ64(dbits(F1("fnabs", D1)), 0xBFF0000000000000ull, "fnabs");
    EQ64(dbits(F1("fmr", D2)), 0x4000000000000000ull, "fmr");
    EQ64(dbits(F3("fsel", DM1, D2, D3)), 0x4008000000000000ull, "fsel negative");
    EQ64(dbits(F3("fsel", DZ, D2, D3)), 0x4000000000000000ull, "fsel zero");
    EQ64(dbits(F1("fres", D4)), 0x3FD0000000000000ull, "fres");
    EQ64(dbits(F1("frsqrte", D4)), 0x3FE0000000000000ull, "frsqrte");
    uint32_t cr;
    __asm__ volatile("fcmpu cr2,%1,%2\n mfcr %0" : "=r"(cr) : "f"(D1), "f"(D2) : "cr2");
    EQ((cr >> 20) & 0xF, 0x8, "fcmpu lt"); EQ((fpscr() >> 12) & 0xF, 0x8, "fpcc");
    double nan = F2("fsub", F2("fdiv", D1, DZ), F2("fdiv", D1, DZ));
    __asm__ volatile("fcmpu cr2,%1,%2\n mfcr %0" : "=r"(cr) : "f"(nan), "f"(D2) : "cr2");
    EQ((cr >> 20) & 0xF, 0x1, "fcmpu unordered");

    /* the FPSCR */
    fpscr_set(0);
    EQ64(dbits(F2("fdiv", D1, DZ)), 0x7FF0000000000000ull, "1/0");
    EQ(fpscr(), 0x84005000u, "zero divide: FX ZX, class +inf");
    __asm__ volatile("mcrfs cr3,1\n mfcr %0" : "=r"(cr) :: "cr3");
    EQ((cr >> 16) & 0xF, 0x4, "mcrfs"); EQ(fpscr(), 0x80005000u, "mcrfs clears ZX");
    fpscr_set(0);
    double inf = F2("fdiv", D1, DZ);
    fpscr_set(0);
    EQ64(dbits(F2("fsub", inf, inf)), 0x7FF8000000000000ull, "inf-inf");
    EQ(fpscr(), 0xA0811000u, "VXISI");
    fpscr_set(0);
    (void)F2("fmul", DZ, inf);
    EQ(fpscr(), 0xA0111000u, "VXIMZ");
    fpscr_set(0);
    (void)F2("fdiv", DZ, DZ);
    EQ(fpscr(), 0xA0211000u, "VXZDZ");
    fpscr_set(0);
    EQ64(dbits(F2("fdiv", D1, D10)), 0x3FB999999999999Aull, "1/10");
    EQ(fpscr(), 0x82064000u, "inexact: XX FR FI, class +normal");
    fpscr_set(0);
    __asm__ volatile("mtfsfi 7,1");                         /* round toward zero */
    EQ64(dbits(F2("fdiv", D1, D10)), 0x3FB9999999999999ull, "1/10 toward zero");
    EQ(fpscr() & 0x00060000u, 0x00020000u, "FI without FR");
    fpscr_set(0);
    (void)F2("fmul", bitsd(0x7FEFFFFFFFFFFFFFull), D2);
    EQ(fpscr() & 0x12000000u, 0x12000000u, "overflow: OX XX");
    fpscr_set(0);
    (void)F2("fdiv", bitsd(0x0010000000000000ull), D3);
    EQ(fpscr() & 0x0A000000u, 0x0A000000u, "underflow: UX XX");
    fpscr_set(0);
    EQ(iwbits(F1("fctiwz", F2("fmul", D10, bitsd(0x41E0000000000000ull)))), 0x7FFFFFFFu, "fctiwz saturates");
    EQ(fpscr() & 0xA0000100u, 0xA0000100u, "VXCVI");

    /* single precision rounds once (round to odd underneath) */
    float fa = 1.0f + 0x1p-12f, fc = 1.0f + 0x1p-10f + 0x1p-12f, fb = 0x1p-60f;
    volatile double a = fa, c = fc, b = fb;
    EQ(sbits(F3("fmadds", a, c, b)), 0x3F803003u, "fmadds rounds once");

    /* single <-> double moves */
    static volatile uint32_t sw[2];
    double d;
    sw[0] = 0x00000001u;                                    /* the smallest denormal */
    __asm__ volatile("lfs %0,0(%1)" : "=f"(d) : "b"(sw) : "memory");
    EQ64(dbits(d), 0x36A0000000000000ull, "lfs denormal");
    EQ(sbits(d), 1, "stfs denormal");
    sw[1] = 0x7F800001u;                                    /* a signalling NaN stays one */
    __asm__ volatile("lfs %0,4(%1)" : "=f"(d) : "b"(sw) : "memory");
    EQ64(dbits(d), 0x7FF0000020000000ull, "lfs snan");

    /* enabled exceptions */
    fpscr_set(0x80);                                        /* VE */
    double t = 5.0;
    __asm__ volatile("fsub %0,%1,%1" : "+f"(t) : "f"(inf));
    EQ64(dbits(t), 0x4014000000000000ull, "VE: target unchanged");
    EQ(fpscr() & 0x60800000u, 0x60800000u, "FEX VX VXISI");
    trap_n = 0;
    uint32_t m = mfmsr();
    mtmsr(m | MSR_FE0 | MSR_FE1);
    __asm__ volatile("fsub %0,%1,%1" : "+f"(t) : "f"(inf));
    mtmsr(m);
    expect_trap(0x700, "fp enabled exception");
    EQ(last_srr1 & 0xFFFF0000u, 0x00100000u, "srr1 fp");
    __asm__ volatile("mtfsb0 24\n mtfsb1 31");
    EQ(fpscr() & 0xFFu, 0x01u, "mtfsb0/1");
    fpscr_set(0);
    EQ(fpscr(), 0, "cleared");
    /* record form: CR1 */
    (void)F2("fdiv", D1, DZ);
    __asm__ volatile("fadd. %0,%1,%1" : "=f"(t) : "f"(D1) : "cr1");
    __asm__ volatile("mfcr %0" : "=r"(cr));
    EQ((cr >> 24) & 0xF, 0x8, "cr1 = FX FEX VX OX");
    fpscr_set(0);
}

/* ---- trace ---------------------------------------------------------------- */

static void t_trace(void)
{
    uint32_t m = mfmsr();
    trace_n = 0;
    __asm__ volatile("mtmsr %0\n nop\n nop\n nop\n mtmsr %1" :: "r"(m | MSR_SE), "r"(m) : "memory");
    EQ(trace_n, 4, "single step");
    trace_n = 0;
    __asm__ volatile("mtmsr %0\n nop\n b 1f\n 1: nop\n mtmsr %1" :: "r"(m | MSR_BE), "r"(m) : "memory");
    EQ(trace_n, 1, "branch trace");
}

/* ---- identification and plain SPRs ---------------------------------------- */

static void t_ident(void)
{
    EQ(mfspr(287), 0x00FF0000u, "pvr: reference identity");
    EQ(mfspr(1023), 0, "pir");
    for (int i = 0; i < 4; i++) {
        mtspr(272, 0x100u); mtspr(273, 0x101u); mtspr(274, 0x102u); mtspr(275, 0x103u);
    }
    EQ(mfspr(272) + mfspr(273) + mfspr(274) + mfspr(275), 0x406u, "sprg0-3");
    mtspr(1008, 0x8000C000u);   /* an implementation register: plain storage by default */
    mtspr(1009, 0x5u);
    EQ(mfspr(1008), 0x8000C000u, "hid0 storage"); EQ(mfspr(1009), 0x5u, "hid1 storage");
    mtspr(19, 0x1234u); EQ(mfspr(19), 0x1234u, "dar");
    mtspr(18, 0x42000000u); EQ(mfspr(18), 0x42000000u, "dsisr");
    mtspr(282, 0xFFFFFFFFu); EQ(mfspr(282), 0x8000003Fu, "ear");
    mtspr(26, 0x100u); mtspr(27, 0x3000u);
    EQ(mfspr(26) + mfspr(27), 0x3100u, "srr0/srr1");
}

int main(void);
int main(void)
{
    puts_("UAM self-test (ppc32, ppc-ref)\n");
    run("arith", t_arith);
    run("logic", t_logic);
    run("rotate", t_rotate);
    run("cr", t_cr);
    run("branch", t_branch);
    run("loadstore", t_loadstore);
    run("atomic", t_atomic);
    run("dcbz", t_dcbz);
    run("exceptions", t_exceptions);
    run("timer", t_timer);
    run("external-irq", t_external);
    run("mmu", t_mmu);
    run("fpu", t_fpu);
    run("trace", t_trace);
    run("ident", t_ident);

    puts_("result: ");
    puthex(n_pass);
    puts_(" passed, ");
    puthex(n_fail);
    puts_(" failed\n");
    puts_(n_fail ? "SELFTEST FAIL\n" : "SELFTEST PASS\n");
    return n_fail ? (int)((n_fail << 16) | 0x3333) : 0x5555;
}
