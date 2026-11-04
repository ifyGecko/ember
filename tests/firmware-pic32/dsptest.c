/*
 * dsptest.c - DSP ASE checks for the PIC32 self-test (included by selftest.c in the
 * MIPS32 mz image). Each vector's instruction word is written into a trampoline in
 * RAM ("insn; jr ra; nop"), run with $4, $5, $6, ac1 and DSPControl set from the
 * vector, and all five compared afterwards.
 */
#include "dspvectors.h"

static volatile uint32_t tramp[4] __attribute__((aligned(16)));
static volatile uint32_t dsp_io[6];   /* a b c acc_lo acc_hi ctl, in and out */

static NATIVE void dsp_run(void)
{
    __asm__ volatile(
        "lw $4, 0(%0)\n\tlw $5, 4(%0)\n\tlw $6, 8(%0)\n\t"
        "lw $8, 12(%0)\n\tmtlo $8, $ac1\n\tlw $8, 16(%0)\n\tmthi $8, $ac1\n\t"
        "lw $8, 20(%0)\n\twrdsp $8, 0x3f\n\t"
        "jalr %1\n\tnop\n\t"
        "sw $4, 0(%0)\n\tsw $5, 4(%0)\n\tsw $6, 8(%0)\n\t"
        "mflo $8, $ac1\n\tsw $8, 12(%0)\n\tmfhi $8, $ac1\n\tsw $8, 16(%0)\n\t"
        "rddsp $8, 0x3f\n\tsw $8, 20(%0)"
        :: "r"(dsp_io), "r"(tramp) : "$4", "$5", "$6", "$8", "$31", "memory");
}

static void t_dsp_vectors(void)
{
    unsigned bad = 0;
    for (unsigned k = 0; k < sizeof(DSPV) / sizeof(DSPV[0]); k++) {
        const struct dspvec *v = &DSPV[k];
        tramp[0] = v->w;
        tramp[1] = 0x03e00008u;   /* jr ra */
        tramp[2] = 0;
        dsp_io[0] = v->a;
        dsp_io[1] = v->b;
        dsp_io[2] = v->c;
        dsp_io[3] = (uint32_t)v->acc;
        dsp_io[4] = (uint32_t)(v->acc >> 32);
        dsp_io[5] = v->ctl;
        dsp_run();
        uint64_t acc = (uint64_t)dsp_io[4] << 32 | dsp_io[3];
        if ((dsp_io[0] != v->a2 || dsp_io[1] != v->b2 || dsp_io[2] != v->c2 || acc != v->acc2
             || dsp_io[5] != v->ctl2) && bad++ < 6) {
            check(0, "vector (word, index)", v->w, k);
            check(0, "  $4", dsp_io[0], v->a2);
            check(0, "  $5", dsp_io[1], v->b2);
            check(0, "  $6", dsp_io[2], v->c2);
            check(0, "  ac1 hi", (uint32_t)(acc >> 32), (uint32_t)(v->acc2 >> 32));
            check(0, "  ac1 lo", (uint32_t)acc, (uint32_t)v->acc2);
            check(0, "  DSPControl", dsp_io[5], v->ctl2);
        }
    }
    EQ(bad, 0, "vectors failing");
}

/* Status.MX = 0: DSP Disabled (ExcCode 26). */
static void t_dsp_disabled(void)
{
    trap_n = 0;
    wr_status(rd_status() & ~(1u << 24));
    __asm__ volatile("addu.qb $8, $8, $8" ::: "$8");
    wr_status(rd_status() | (1u << 24));
    EQ(trap_n, 1, "trapped");
    EQ(last_code, 26, "DSPDis");
}

static volatile uint32_t lx_buf[2] = { 0x89ABCDEFu, 0x01234567u };
static void t_dsp_loads(void)
{
    uint32_t w, h, b, base = (uint32_t)(uintptr_t)lx_buf, idx = 4;
    __asm__ volatile("lwx %0, %3(%4)\n\tlhx %1, %5(%4)\n\tlbux %2, %6(%4)"
                     : "=&r"(w), "=&r"(h), "=&r"(b)
                     : "r"(idx), "r"(base), "r"(2u), "r"(3u));
    EQ(w, 0x01234567u, "lwx");
    EQ(h, 0xFFFF89ABu, "lhx (sign-extended)");
    EQ(b, 0x89u, "lbux");
}

/* The DSP through the compiler's own code (builtins). */
typedef signed char v4i8 __attribute__((vector_size(4)));
typedef short v2q15 __attribute__((vector_size(4)));
static volatile uint32_t VA = 0x7F01FF80u, VB = 0x0102FF80u;
static void t_dsp_c(void)
{
    union { uint32_t u; v4i8 v; } a = { VA }, b = { VB }, r;
    r.v = __builtin_mips_addu_s_qb(a.v, b.v);
    EQ(r.u, 0x8003FFFFu, "addu_s.qb");
    union { uint32_t u; v2q15 v; } p = { 0x40008000u }, q = { 0x40008000u }, m;
    m.v = __builtin_mips_mulq_rs_ph(p.v, q.v);
    EQ(m.u, 0x20007FFFu, "mulq_rs.ph");
    long long ac = __builtin_mips_dpaq_s_w_ph(0, p.v, q.v);
    EQ((uint32_t)ac, 0x9FFFFFFFu, "dpaq_s.w.ph low");
}
