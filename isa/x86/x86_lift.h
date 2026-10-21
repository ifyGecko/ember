/*
 * x86_lift.h - Lifter context and emission helpers shared by the x86 lifter
 * files (x86_lift.c: one-byte map and block driver; x86_lift0f.c: the 0F map;
 * x86_sse.c: SSE and x87). Not part of the public interface.
 */
#ifndef UAM_X86_LIFT_H
#define UAM_X86_LIFT_H

#include "x86_decode.h"

#define NONE UAM_REF_NONE

typedef struct xl {
    uam_lift_ctx *lc;
    uam_builder  *b;
    x86_insn      in;
    unsigned      priv;
    unsigned      code;      /* X86_CODE16/32/64 */
    unsigned      cpl;
    unsigned      sw;        /* stack-pointer width: 16/32/64 */
    uint64_t      cs_base;   /* CS base when the block was lifted */
    uint64_t      pc;        /* linear address of this instruction */
    uint64_t      next;      /* linear address of the next instruction */
    uam_ref       sp;        /* stack pointer as updated so far in this insn, or NONE */
    bool          end;       /* this instruction ends the block */
    bool          sti_shadow;/* the next instruction must retire with this one */
} xl;

typedef enum { LR_CONT, LR_END } lr;

/* ---- immediates and arithmetic ---- */
static inline uint64_t xmask(unsigned w) { return w >= 64 ? ~0ull : ((1ull << w) - 1); }
uam_ref x_imm(xl *x, uint64_t v);
uam_ref x_bin(xl *x, uam_opcode op, unsigned w, uam_ref a, uam_ref b);
uam_ref x_un(xl *x, uam_opcode op, unsigned w, uam_ref a);
/* width conversions that also accept immediates (from = source width) */
uam_ref x_zext(xl *x, unsigned to, unsigned from, uam_ref v);
uam_ref x_sext(xl *x, unsigned to, unsigned from, uam_ref v);
uam_ref x_trunc(xl *x, unsigned to, unsigned from, uam_ref v);
uam_ref x_resize(xl *x, unsigned to, unsigned from, uam_ref v);   /* zext or trunc */

/* ---- registers ---- */
uam_ref x_reg(xl *x, unsigned r, unsigned w);
void    x_setreg(xl *x, unsigned r, unsigned w, uam_ref v);
uam_ref x_flag(xl *x, unsigned f);              /* 1-bit value */
void    x_setflag(xl *x, unsigned f, uam_ref b1);
void    x_set_szp(xl *x, unsigned w, uam_ref res);
void    x_flags_logic(xl *x, unsigned w, uam_ref res);
void    x_flags_add(xl *x, unsigned w, uam_ref a, uam_ref b, uam_ref res, uam_ref cin);
void    x_flags_sub(xl *x, unsigned w, uam_ref a, uam_ref b, uam_ref res, uam_ref bin);
uam_ref x_cond(xl *x, unsigned cc);

/* ---- memory operands ---- */
typedef struct xop { bool mem; unsigned reg; uam_ref lin; } xop;
uam_ref x_seg_base(xl *x, unsigned seg);        /* 64-bit; 0 for flat segments in 64-bit code */
uam_ref x_ea_off(xl *x);                        /* offset at asz */
uam_ref x_lin(xl *x, unsigned seg, uam_ref off, unsigned offw);
xop     x_rm(xl *x);                            /* ModRM r/m operand */
uam_ref x_ld(xl *x, unsigned w, uam_ref lin);
void    x_st(xl *x, unsigned w, uam_ref lin, uam_ref v);
uam_ref x_rd(xl *x, xop *o, unsigned w);
void    x_wr(xl *x, xop *o, unsigned w, uam_ref v);
static inline unsigned x_seg(xl *x, unsigned dflt) { return x->in.seg >= 0 ? (unsigned)x->in.seg : dflt; }

/* ---- stack ---- */
uam_ref x_sp(xl *x);                            /* current stack pointer (sw bits) */
void    x_push(xl *x, unsigned w, uam_ref v);   /* store; sp committed by x_commit_sp */
uam_ref x_pop(xl *x, unsigned w);
void    x_commit_sp(xl *x);

/* ---- control ---- */
uam_ref x_ip_of(xl *x, uint64_t lin);           /* rIP for a linear pc, at operand width */
uint64_t x_rel_target(xl *x, int64_t rel, unsigned osz);
uam_ref x_ind_target(xl *x, uam_ref ip);        /* linear pc for an indirect rIP */
void    x_jump(xl *x, uam_ref target, uam_exit_kind k);
lr      x_trap(xl *x, unsigned vec);            /* raise an exception before the insn */
lr      x_trap_err(xl *x, unsigned vec, uint32_t err);
lr      x_ud(xl *x);
uam_ref x_helper(xl *x, unsigned id, uam_ref a, uam_ref b, uam_ref c);
uam_ref x_io_addr(xl *x, uam_ref port16);
void    x_iopl_check(xl *x);                    /* IN/OUT/CLI/STI at CPL > 0 */

/* ---- shared one-byte-map pieces (x86_lift.c) ---- */
uint64_t x_sx(uint64_t v, unsigned from_bits);              /* sign-extend to 64 */
lr      x_jcc(xl *x, unsigned cc, int64_t rel);
void    x_imul2(xl *x, unsigned w, uam_ref a, uam_ref b);   /* Gv = a * b, IMUL flags */
void    x_load_seg(xl *x, unsigned seg, uam_ref sel16);
uam_ref x_seg_sel(xl *x, unsigned seg);
lr      x_push_seg(xl *x, unsigned seg);
lr      x_pop_seg(xl *x, unsigned seg);
lr      x_load_far(xl *x, unsigned seg);                    /* LDS LES LSS LFS LGS */

/* ---- per-map emitters ---- */
lr x_lift_0f(xl *x);
lr x_lift_sse(xl *x);
lr x_lift_x87(xl *x);
lr x_lift_string(xl *x);

#endif /* UAM_X86_LIFT_H */
