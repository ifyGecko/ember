/*
 * spin.c - Spin loops whose passes the run loop may skip (spin.h).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"

#include "../ir/ir_eval.h"
#include "execops.h"
#include "spin.h"

#define SPIN_MAX_OPS   64u     /* longer blocks are not spin loops worth the evaluation */
#define SPIN_MAX_PUTS  16u
#define SPIN_MAX_REGFILE 8192u /* register files up to this size (bytes) */

bool uam_spin_candidate(const uam_block *blk)
{
    const char *off = getenv("UAM_SPIN_SKIP");   /* UAM_SPIN_SKIP=0: never skip */
    if (off && strcmp(off, "0") == 0)
        return false;
    if (blk->n_insn > SPIN_MAX_OPS || blk->n_guest == 0)
        return false;
    const uint64_t self = blk->guest_pc & uam_mask(blk->addr_width);

    /* The register bytes the block writes. */
    struct { uint32_t lo, hi; } put[SPIN_MAX_PUTS];
    unsigned n_put = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if (in->op == UAM_OP_PUT) {
            if (n_put == SPIN_MAX_PUTS)
                return false;
            put[n_put].lo = in->aux;
            put[n_put].hi = in->aux + (in->width + 7) / 8;
            n_put++;
        }
    }

    /* Which temps may differ from one pass to the next: those that depend on a register
     * the block writes. A load's value is the same while memory is (nothing else runs). */
    bool var[SPIN_MAX_OPS] = { false };   /* per temp index below n_insn's temps */
    if (blk->n_temp > SPIN_MAX_OPS)
        return false;
#define VAR(r) (!uam_ref_is_imm(r) && !uam_ref_is_none(r) && var[r])
    bool loops = false;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        bool v = VAR(in->guard);
        switch ((uam_opcode)in->op) {
        case UAM_OP_IMARK:
            if (in->flags & UAM_IMARK_BREAK)
                return false;
            continue;
        case UAM_OP_FENCE:
            continue;
        case UAM_OP_GET: {
            const uint32_t lo = in->aux, hi = in->aux + (in->width + 7) / 8;
            for (unsigned k = 0; k < n_put; k++)
                v |= lo < put[k].hi && put[k].lo < hi;
            break;
        }
        case UAM_OP_PUT:
            if (v || VAR(in->a))
                return false;
            continue;
        case UAM_OP_LD:
            if (in->flags & UAM_ACC_X)
                return false;   /* a load-reserved sets the reservation */
            v |= VAR(in->a);
            break;
        case UAM_OP_EXIT:
            if (v || VAR(in->a))
                return false;
            if (uam_ref_is_imm(in->a)
                && (blk->imm[uam_ref_index(in->a)] & uam_mask(blk->addr_width)) == self)
                loops = true;
            continue;
        default: {
            const uam_opinfo *oi = &uam_opinfo_table[in->op];
            if (oi->props != 0)
                return false;   /* stores, system registers, helpers, traps, waits, coverage */
            v |= VAR(in->a) || (oi->n_src > 1 && VAR(in->b)) || (oi->n_src > 2 && VAR(in->c));
            break;
        }
        }
        if (!uam_ref_is_none(in->dst))
            var[in->dst] = v;
    }
#undef VAR
    return loops;
}

static inline uint64_t opnd(const uam_block *blk, const uint64_t *t, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        return blk->imm[uam_ref_index(r)] & uam_mask(w);
    return t[r];
}

/* As uam_interp_block would run one pass, but with no effect: see spin.h. */
bool uam_spin_fixed(uam_cpu *cpu, const uam_block *blk, uint64_t *t, bool *never)
{
    const unsigned aw = blk->addr_width;
    *never = false;
    /* The pass runs on a copy of the register file: a later GET reads an earlier PUT,
     * and only the state at the end matters (a register may be written twice). */
    uint8_t rf[SPIN_MAX_REGFILE];
    if (cpu->regfile_size > sizeof(rf)) {
        *never = true;
        return false;
    }
    memcpy(rf, cpu->regfile, cpu->regfile_size);
    uint32_t lo = UINT32_MAX, hi = 0;   /* bytes written */
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        const unsigned W = in->width;
        if (!uam_ref_is_none(in->guard) && !opnd(blk, t, in->guard, 1)) {
            if (!uam_ref_is_none(in->dst))
                t[in->dst] = 0;
            continue;
        }
        switch ((uam_opcode)in->op) {
        case UAM_OP_IMARK:
        case UAM_OP_FENCE:
            break;
        case UAM_OP_GET: {   /* as uam_cpu_reg_read */
            uint64_t v = 0;
            for (unsigned k = 0; k < W / 8; k++)
                v |= (uint64_t)rf[in->aux + k] << (8 * k);
            t[in->dst] = v;
            break;
        }
        case UAM_OP_PUT: {   /* as uam_cpu_reg_write */
            const uint64_t v = opnd(blk, t, in->a, W);
            for (unsigned k = 0; k < W / 8; k++)
                rf[in->aux + k] = (uint8_t)(v >> (8 * k));
            if (W >= 8) {
                lo = in->aux < lo ? in->aux : lo;
                hi = in->aux + W / 8 > hi ? in->aux + W / 8 : hi;
            }
            break;
        }
        case UAM_OP_LD: {
            const unsigned size = W / 8;
            const uint64_t addr = opnd(blk, t, in->a, aw);
            if ((in->flags & UAM_ACC_X) || uam_execops_crosses(cpu, addr, size, in->flags)) {
                *never = true;
                return false;
            }
            uint64_t pa = 0, v = 0;
            uint32_t xf = 0;
            if (uam_execops_xlate(cpu, addr, UAM_XLATE_LOAD, in->flags, size, &pa, &xf) != UAM_OK)
                return false;   /* the pass would fault */
            /* Only memory that reads without effect and that nothing watches: a RAM or
             * ROM page owned by one region (uam_bus_page_host). */
            if (!uam_bus_page_host(cpu->bus, pa, false)
                || (pa & UAM_PAGE_MASK) != ((pa + size - 1) & UAM_PAGE_MASK)) {
                *never = true;
                return false;
            }
            if (uam_bus_read(cpu->bus, pa, size, in->flags, &v) != UAM_OK) {
                *never = true;
                return false;
            }
            t[in->dst] = v;
            break;
        }
        case UAM_OP_EXIT:
            return (opnd(blk, t, in->a, aw) & uam_mask(aw)) == (blk->guest_pc & uam_mask(aw))
                && (lo >= hi || memcmp(rf + lo, cpu->regfile + lo, hi - lo) == 0);
        default: {
            const uam_opinfo *oi = &uam_opinfo_table[in->op];
            if (oi->props != 0)
                return false;
            unsigned src_w = W;
            if (in->op == UAM_OP_ZEXT || in->op == UAM_OP_SEXT || in->op == UAM_OP_TRUNC)
                src_w = blk->temp_width[in->a];
            uint64_t x = oi->n_src > 0 ? opnd(blk, t, in->a, in->op == UAM_OP_SELECT ? 1 : src_w) : 0;
            uint64_t y = oi->n_src > 1 ? opnd(blk, t, in->b, W) : 0;
            uint64_t z = oi->n_src > 2 ? opnd(blk, t, in->c, W) : 0;
            uint64_t r;
            if (!uam_ir_eval_pure((uam_opcode)in->op, W, x, y, z, src_w, &r))
                return false;
            t[in->dst] = r;
            break;
        }
        }
    }
    return false;
}
