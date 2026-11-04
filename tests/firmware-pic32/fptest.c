/*
 * fptest.c - FPU checks for the PIC32 self-test (included by selftest.c on mz).
 * Operands go through GPRs into $f2/$f4/$f6 (MTC1/MTHC1, so the same code works
 * with FR = 1 and FR = 0, where MTHC1 writes the odd register of the pair) and the
 * result comes back from $f0; FCSR.RM is set first and FCSR read after, so each
 * vector checks the result bits and FCSR.Cause.
 */
#include "fpvectors.h"

#define FP_IN  "mtc1 %[al],$f2\n\tmthc1 %[ah],$f2\n\tmtc1 %[bl],$f4\n\tmthc1 %[bh],$f4\n\t" \
               "mtc1 %[cl],$f6\n\tmthc1 %[ch],$f6\n\t"
#define FP_OUT "\n\tmfc1 %[rl],$f0\n\tmfhc1 %[rh],$f0"
#define FP(insn) __asm__ volatile(FP_IN insn FP_OUT \
    : [rl] "=r"(rl), [rh] "=r"(rh) \
    : [al] "r"(al), [ah] "r"(ah), [bl] "r"(bl), [bh] "r"(bh), [cl] "r"(cl), [ch] "r"(ch) \
    : "$f0", "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7", "memory")

static NATIVE uint32_t cfc1_fcsr(void) { uint32_t v; __asm__ volatile("cfc1 %0,$31" : "=r"(v)); return v; }
static NATIVE void ctc1_fcsr(uint32_t v) { __asm__ volatile("ctc1 %0,$31" :: "r"(v)); }

static NATIVE uint64_t fp_run(unsigned op, uint64_t a, uint64_t b, uint64_t c)
{
    uint32_t al = (uint32_t)a, ah = (uint32_t)(a >> 32), bl = (uint32_t)b, bh = (uint32_t)(b >> 32);
    uint32_t cl = (uint32_t)c, ch = (uint32_t)(c >> 32), rl = 0, rh = 0;
    switch (op) {
    case FP_ADD_S:     FP("add.s $f0,$f2,$f4"); break;
    case FP_SUB_S:     FP("sub.s $f0,$f2,$f4"); break;
    case FP_MUL_S:     FP("mul.s $f0,$f2,$f4"); break;
    case FP_DIV_S:     FP("div.s $f0,$f2,$f4"); break;
    case FP_SQRT_S:    FP("sqrt.s $f0,$f2"); break;
    case FP_ADD_D:     FP("add.d $f0,$f2,$f4"); break;
    case FP_SUB_D:     FP("sub.d $f0,$f2,$f4"); break;
    case FP_MUL_D:     FP("mul.d $f0,$f2,$f4"); break;
    case FP_DIV_D:     FP("div.d $f0,$f2,$f4"); break;
    case FP_SQRT_D:    FP("sqrt.d $f0,$f2"); break;
    case FP_CVT_S_D:   FP("cvt.s.d $f0,$f2"); break;
    case FP_CVT_D_S:   FP("cvt.d.s $f0,$f2"); break;
    case FP_CVT_W_D:   FP("cvt.w.d $f0,$f2"); break;
    case FP_ROUND_W_D: FP("round.w.d $f0,$f2"); break;
    case FP_TRUNC_W_D: FP("trunc.w.d $f0,$f2"); break;
    case FP_CEIL_W_D:  FP("ceil.w.d $f0,$f2"); break;
    case FP_FLOOR_W_D: FP("floor.w.d $f0,$f2"); break;
    case FP_CVT_W_S:   FP("cvt.w.s $f0,$f2"); break;
    case FP_CVT_L_D:   FP("cvt.l.d $f0,$f2"); break;
    case FP_CVT_D_W:   FP("cvt.d.w $f0,$f2"); break;
    case FP_CVT_S_W:   FP("cvt.s.w $f0,$f2"); break;
    case FP_CVT_D_L:   FP("cvt.d.l $f0,$f2"); break;
    case FP_CVT_S_L:   FP("cvt.s.l $f0,$f2"); break;
    case FP_MADD_D:    FP("madd.d $f0,$f6,$f2,$f4"); break;
    case FP_NMSUB_S:   FP("nmsub.s $f0,$f6,$f2,$f4"); break;
    case FP_ABS_D:     FP("abs.d $f0,$f2"); break;
    case FP_NEG_S:     FP("neg.s $f0,$f2"); break;
    case FP_C_OLT_D:   FP("c.olt.d $fcc3,$f2,$f4"); return (cfc1_fcsr() >> 27) & 1;   /* FCC3 */
    case FP_C_UEQ_D:   FP("c.ueq.d $fcc3,$f2,$f4"); return (cfc1_fcsr() >> 27) & 1;   /* FCC3 */
    case FP_C_LT_D:    FP("c.lt.d $fcc3,$f2,$f4"); return (cfc1_fcsr() >> 27) & 1;   /* FCC3 */
    case FP_C_NGLE_S:  FP("c.ngle.s $fcc3,$f2,$f4"); return (cfc1_fcsr() >> 27) & 1;   /* FCC3 */
    default: break;
    }
    return ((uint64_t)rh << 32) | rl;
}

/* Which results are 32 bits wide (the rest are 64; compares are 0/1). */
static int fp_single_result(unsigned op)
{
    switch (op) {
    case FP_ADD_S: case FP_SUB_S: case FP_MUL_S: case FP_DIV_S: case FP_SQRT_S: case FP_CVT_S_D:
    case FP_CVT_W_D: case FP_ROUND_W_D: case FP_TRUNC_W_D: case FP_CEIL_W_D: case FP_FLOOR_W_D:
    case FP_CVT_W_S: case FP_CVT_S_W: case FP_CVT_S_L: case FP_NMSUB_S: case FP_NEG_S:
        return 1;
    default:
        return 0;
    }
}

static void t_fpu_vectors(void)
{
    unsigned bad = 0;
    for (unsigned k = 0; k < sizeof(FPV) / sizeof(FPV[0]); k++) {
        const struct fpvec *v = &FPV[k];
        ctc1_fcsr(v->rm);
        uint64_t r = fp_run(v->op, v->a, v->b, v->c);
        uint32_t cause = (cfc1_fcsr() >> 12) & 0x1F;
        uint64_t want = v->r;
        if (fp_single_result(v->op)) {
            r &= 0xFFFFFFFFu;
            want &= 0xFFFFFFFFu;
        }
        if ((r != want || cause != v->cause) && bad++ < 8) {
            check(0, "vector (op, rm, index)", (v->op << 16) | (v->rm << 12) | k, 0);
            check(0, "  result low", (uint32_t)r, (uint32_t)want);
            check(0, "  result high", (uint32_t)(r >> 32), (uint32_t)(want >> 32));
            check(0, "  cause", cause, v->cause);
        }
    }
    EQ(bad, 0, "vectors failing");
}

/* An enabled exception: FPE (ExcCode 15), Cause set, the destination unchanged. */
static void t_fpu_trap(void)
{
    trap_n = 0;
    ctc1_fcsr(1u << 10);                          /* enable divide-by-zero */
    uint32_t rl, rh;
    __asm__ volatile("li %0,0x3f800000\n\tmtc1 %0,$f2\n\tmtc1 $0,$f4\n\tli %0,0x12345678\n\t"
                     "mtc1 %0,$f0\n\tdiv.s $f0,$f2,$f4\n\tmfc1 %0,$f0\n\tmove %1,$0"
                     : "=&r"(rl), "=&r"(rh) :: "$f0", "$f2", "$f4", "memory");
    uint32_t f = cfc1_fcsr();
    ctc1_fcsr(0);
    EQ(trap_n, 1, "one trap");
    EQ(last_code, 15, "FPE");
    EQ((f >> 12) & 0x1F, 8, "FCSR.Cause = Z");
    EQ((f >> 2) & 0x1F, 0, "flags not set by a trapped operation");
    EQ(rl, 0x12345678u, "destination unchanged");
    (void)rh;
}

/* Status.CU1 = 0: coprocessor unusable, CE = 1. */
static void t_fpu_unusable(void)
{
    trap_n = 0;
    wr_status(rd_status() & ~(1u << 29));
    __asm__ volatile("mov.s $f0,$f2" ::: "$f0");
    wr_status(rd_status() | (1u << 29));
    EQ(trap_n, 1, "trapped");
    EQ(last_code, 11, "CpU");
    EQ((last_cause >> 28) & 3, 1, "Cause.CE = 1");
}

/* Compiled C floating point, through the ABI's own FPU code. */
static volatile double DA = 3.0, DB = 7.0, DC = -0.5;
static volatile float FA = 1.25f, FB = 4.0f;
static void t_fpu_c(void)
{
    double x = DA / DB;
    EQ((uint32_t)(x * 1e9), 428571428u, "double divide");
    EQ((uint32_t)(int32_t)(DC * 10.0), 0xFFFFFFFBu, "double to int");
    float y = FA * FB + 0.5f;
    EQ((uint32_t)(y * 100.0f), 550u, "float madd");
    EQ((DA < DB) + (DB < DA) * 2 + (DC < 0) * 4, 5, "compares");
    double z = 0;
    for (int k = 1; k <= 10; k++)
        z += 1.0 / (k * k);
    EQ((uint32_t)(z * 1e6), 1549767u, "series");
}
