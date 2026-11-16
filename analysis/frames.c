/*
 * frames.c - The frame pass (uam/frames.h): UAM_OP_FRAME before call and return exits.
 *
 * For every EXIT a lifter marked CALL, RET or EXC_RETURN it inserts one FRAME before the
 * exit (and the fences that must directly precede it), carrying the exit's guard as a
 * value. A call's return address is the end of the instruction the exit belongs to, the
 * delay slot included: the last IMARK before the exit, plus its length. A "call" to its
 * own return address (the get-pc idiom: `bcl 20,31,$+4`, `bal .+8`, `rcall .`) is not one.
 */
#include <stdlib.h>

#include "uam/frames.h"

struct uam_frame_pass {
    uam_pass  pass;
    uint64_t  pc_mask;
    void    (*site)(void *ctx, uint64_t ret);
    void     *ctx;
};

/* Where a marker for the EXIT at index e goes: before it, the fences that must directly
 * precede it and the markers already there. */
static uint32_t before_exit(const uam_block *blk, uint32_t e)
{
    while (e > 1 && (blk->insn[e - 1].op == UAM_OP_FENCE
                     || (blk->insn[e - 1].op == UAM_OP_COV && blk->insn[e - 1].flags)))
        e--;
    return e;
}

static uam_status frames_run(void *ctx, uam_block *blk)
{
    const uam_frame_pass *fp = ctx;
    uint32_t n_exit = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++)
        n_exit += blk->insn[i].op == UAM_OP_EXIT && blk->insn[i].aux != UAM_EXIT_JUMP
               && blk->insn[i].aux != UAM_EXIT_RETRY;
    if (!n_exit)
        return UAM_OK;
    uam_insn small_ops[4], *ops = small_ops;
    uint32_t small_at[4], *at = small_at, n = 0;
    if (n_exit > 4) {
        ops = malloc(sizeof(*ops) * n_exit);
        at = malloc(sizeof(*at) * n_exit);
        if (!ops || !at) {
            free(ops);
            free(at);
            return UAM_ERR_NOMEM;
        }
    }
    const uint64_t am = uam_mask(blk->addr_width);
    const uam_ref zero = uam_block_imm(blk, 0), one = uam_block_imm(blk, 1);
    uam_status st = uam_ref_is_none(zero) || uam_ref_is_none(one) ? UAM_ERR_NOMEM : UAM_OK;
    uint64_t pc = blk->guest_pc, end = blk->guest_pc;   /* the instruction an exit belongs to */
    for (uint32_t i = 0; st == UAM_OK && i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if (in->op == UAM_OP_IMARK) {
            const uint64_t p = blk->imm[uam_ref_index(in->a)];
            if (!(in->flags & UAM_IMARK_CONT))
                pc = p;
            end = (p + in->aux) & am;
            continue;
        }
        if (in->op != UAM_OP_EXIT)
            continue;
        uam_insn op = { .op = UAM_OP_FRAME, .width = blk->addr_width, .guard = UAM_REF_NONE,
                        .dst = UAM_REF_NONE, .a = zero,
                        .b = uam_ref_is_none(in->guard) ? one : in->guard, .c = zero };
        switch ((uam_exit_kind)in->aux) {
        case UAM_EXIT_CALL: {
            const uint64_t ret = end & fp->pc_mask;
            if (uam_ref_is_imm(in->a)
                && ((blk->imm[uam_ref_index(in->a)] & am) & fp->pc_mask) == ret)
                continue;   /* reads its own address: not a call */
            op.flags = UAM_FRAME_CALL;
            op.a = uam_block_imm(blk, ret);
            op.c = in->a;
            if (fp->site)
                fp->site(fp->ctx, ret);
            break;
        }
        case UAM_EXIT_RET:
            op.flags = UAM_FRAME_RET;
            op.a = in->a;
            op.c = uam_block_imm(blk, pc);
            break;
        case UAM_EXIT_EXC_RETURN:
            op.flags = UAM_FRAME_ERET;
            break;
        default:
            continue;
        }
        if (uam_ref_is_none(op.a) || uam_ref_is_none(op.c)) {
            st = UAM_ERR_NOMEM;
            break;
        }
        const uint32_t k = before_exit(blk, i);
        ops[n] = op;
        at[n] = n && k < at[n - 1] ? at[n - 1] : k;
        n++;
    }
    if (st == UAM_OK && n)
        st = uam_block_insert(blk, at, ops, n);
    if (ops != small_ops) {
        free(ops);
        free(at);
    }
    return st;
}

uam_frame_pass *uam_frame_pass_create(uint64_t pc_mask, void (*site)(void *ctx, uint64_t ret),
                                      void *ctx)
{
    uam_frame_pass *fp = calloc(1, sizeof(*fp));
    if (!fp)
        return NULL;
    fp->pass = (uam_pass){ .name = "frames", .run = frames_run, .ctx = fp };
    fp->pc_mask = pc_mask;
    fp->site = site;
    fp->ctx = ctx;
    return fp;
}

void uam_frame_pass_destroy(uam_frame_pass *fp)
{
    free(fp);
}

const uam_pass *uam_frame_pass_get(const uam_frame_pass *fp)
{
    return &fp->pass;
}
