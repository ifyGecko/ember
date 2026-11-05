/*
 * test_xtensa_fpu.c - The Xtensa FP coprocessor's computational instructions
 * (isa/xtensa/xtensa_fpu.c) against vectors computed with exact rational arithmetic
 * (tests/isa-xtensa/gen-fpu-vectors.py): every instruction in the four rounding modes,
 * the result, the sticky FSR flags and that nothing else in the register file changes;
 * then libgcc's __divsf3, __ieee754_sqrtf, __recipsf2 and __rsqrtsf2 run as instruction
 * sequences. The vectors run twice, the second time under another host rounding mode
 * with every host exception flag raised, which must not change anything.
 *
 * Needs only xtensa_fpu.c: the hart is a bare register file.
 */
#include <fenv.h>
#include <stdio.h>
#include <string.h>

#include "../../isa/xtensa/xtensa.h"
#include "../../isa/xtensa/xtensa_opc.h"
#include "xtensa_fpu_vectors.h"   /* generated: tests/isa-xtensa/gen-fpu-vectors.py */

uam_status xt_fpu_exec(uam_cpu *cpu, unsigned id, uint32_t insn);

static int n_checks, n_failed;

#define CHECK(cond, ...) do {                                              \
        n_checks++;                                                        \
        if (!(cond)) {                                                     \
            n_failed++;                                                    \
            if (n_failed <= 40) {                                          \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
                fprintf(stderr, __VA_ARGS__);                              \
                fputc('\n', stderr);                                       \
            }                                                              \
        }                                                                  \
    } while (0)

static uint8_t regs[XT_REGFILE_SIZE];
static uam_cpu cpu;

static uint32_t rd(uint32_t off) { return xt_reg(&cpu, off); }
static void wr(uint32_t off, uint32_t v) { xt_set_reg(&cpu, off, v); }

/* Fill the register file with a different pattern for each vector. */
static void scramble(uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (unsigned i = 0; i < XT_REGFILE_SIZE; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        regs[i] = (uint8_t)x;
    }
}

static bool is_cmp(unsigned id)
{
    return id == XTO_OEQ_S || id == XTO_OLT_S || id == XTO_OLE_S || id == XTO_UEQ_S ||
           id == XTO_ULT_S || id == XTO_ULE_S || id == XTO_UN_S;
}

static bool is_toint(unsigned id)
{
    return id == XTO_TRUNC_S || id == XTO_ROUND_S || id == XTO_FLOOR_S || id == XTO_CEIL_S ||
           id == XTO_UTRUNC_S;
}

/* FR[t] is an operand (t is not part of the opcode). */
static bool reads_ft(unsigned id)
{
    return id == XTO_ADD_S || id == XTO_SUB_S || id == XTO_MUL_S || id == XTO_MADD_S ||
           id == XTO_MSUB_S || id == XTO_MADDN_S || id == XTO_DIVN_S;
}

static void run_vectors(const char *pass)
{
    unsigned nv = sizeof xt_fpu_vecs / sizeof xt_fpu_vecs[0];
    for (unsigned i = 0; i < nv; i++) {
        const xt_fpu_vec *v = &xt_fpu_vecs[i];
        unsigned r = (v->insn >> 12) & 15, s = (v->insn >> 8) & 15, t = (v->insn >> 4) & 15;
        uint32_t dest;
        scramble(i);
        wr(XT_FCR, v->fcr);
        wr(XT_FSR, v->fsr_in);
        if (is_cmp(v->id)) {
            dest = XT_BR;
            wr(XT_BR, v->in[0]);
            wr(XT_F(s), v->in[1]);
            wr(XT_F(t), v->in[2]);
        } else if (is_toint(v->id)) {
            dest = XT_A(r);
            wr(XT_A(r), v->in[0]);
            wr(XT_F(s), v->in[1]);
        } else if (v->id == XTO_FLOAT_S || v->id == XTO_UFLOAT_S) {
            dest = XT_F(r);
            wr(XT_F(r), v->in[0]);
            wr(XT_A(s), v->in[1]);
        } else {
            dest = XT_F(r);
            wr(XT_F(r), v->in[0]);
            if (v->id != XTO_CONST_S)
                wr(XT_F(s), v->in[1]);
            if (reads_ft(v->id))
                wr(XT_F(t), v->in[2]);
        }
        uint8_t want[XT_REGFILE_SIZE];
        memcpy(want, regs, sizeof want);
        for (unsigned b = 0; b < 4; b++) {
            want[dest + b] = (uint8_t)(v->out >> (8 * b));
            want[XT_FSR + b] = (uint8_t)(v->fsr_out >> (8 * b));
        }
        uam_status st = xt_fpu_exec(&cpu, v->id, v->insn);
        bool ok = st == UAM_OK && !memcmp(regs, want, sizeof want);
        unsigned bad = 0;
        if (!ok)
            while (bad < XT_REGFILE_SIZE && regs[bad] == want[bad])
                bad++;
        CHECK(ok, "%s vector %u: %s 0x%06x fcr 0x%x in %08x %08x %08x: status %d, "
              "result %08x want %08x, fsr %08x want %08x, first differing byte %u",
              pass, i, xt_opnames[v->id], (unsigned)v->insn, (unsigned)v->fcr,
              (unsigned)v->in[0], (unsigned)v->in[1], (unsigned)v->in[2], (int)st,
              (unsigned)rd(dest), (unsigned)v->out, (unsigned)rd(XT_FSR),
              (unsigned)v->fsr_out, bad);
    }
}

static void run_sequences(const char *pass)
{
    unsigned nv = sizeof xt_fpu_seqvecs / sizeof xt_fpu_seqvecs[0];
    for (unsigned i = 0; i < nv; i++) {
        const xt_fpu_seqvec *v = &xt_fpu_seqvecs[i];
        const xt_fpu_step *p = xt_fpu_progs[v->prog].steps;
        bool ok = true;
        scramble(~i);
        for (unsigned f = 0; f < 16; f++)
            wr(XT_F(f), 0);
        wr(XT_F(1), v->a);
        wr(XT_F(2), v->b);
        wr(XT_FCR, v->rm);
        wr(XT_FSR, 0);
        for (unsigned k = 0; k < xt_fpu_progs[v->prog].n; k++) {
            unsigned r = (p[k].insn >> 12) & 15, s = (p[k].insn >> 8) & 15;
            if (p[k].id == XTO_MOV_S)           /* moves are lifted inline, not ours */
                wr(XT_F(r), rd(XT_F(s)));
            else if (p[k].id == XTO_NEG_S)
                wr(XT_F(r), rd(XT_F(s)) ^ 0x80000000u);
            else
                ok &= xt_fpu_exec(&cpu, p[k].id, p[k].insn) == UAM_OK;
        }
        ok &= rd(XT_F(0)) == v->out && rd(XT_FSR) == v->fsr_out;
        CHECK(ok, "%s %s(%08x, %08x) rm %u: got %08x fsr %08x, want %08x fsr %08x", pass,
              xt_fpu_progs[v->prog].name, (unsigned)v->a, (unsigned)v->b, (unsigned)v->rm,
              (unsigned)rd(XT_F(0)), (unsigned)rd(XT_FSR), (unsigned)v->out,
              (unsigned)v->fsr_out);
    }
}

/* Instructions that are not the FPU helper's: refused, nothing written. */
static void test_others(void)
{
    static const unsigned ids[] = { XTO_MOV_S, XTO_NEG_S, XTO_ABS_S, XTO_RFR, XTO_WFR,
                                    XTO_LSI, XTO_MOVEQZ_S, XTO_MOVT_S, XTO_ADD, XTO_NOP };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        uint8_t want[XT_REGFILE_SIZE];
        scramble(1000 + i);
        memcpy(want, regs, sizeof want);
        CHECK(xt_fpu_exec(&cpu, ids[i], 0xfa1230) == UAM_ERR_INVALID &&
              !memcmp(regs, want, sizeof want), "%s refused", xt_opnames[ids[i]]);
    }
}

int main(void)
{
    cpu.regfile = regs;
    cpu.regfile_size = sizeof regs;

    run_vectors("pass 1");
    run_sequences("pass 1");
    test_others();

    /* The host's rounding mode and exception flags are not inputs. */
    fenv_t env;
    fegetenv(&env);
    fesetround(FE_DOWNWARD);
    feraiseexcept(FE_ALL_EXCEPT);
    run_vectors("pass 2");
    run_sequences("pass 2");
    fesetenv(&env);

    printf("test_xtensa_fpu: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
