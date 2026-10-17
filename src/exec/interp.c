/*
 * interp.c - Reference interpreter for UAM-IR.
 *
 * Executes one verified block. This file is a direct transcription of the
 * semantics in docs/uam-isa.md section 7; every edge case (division by zero,
 * over-wide shifts, guards) is handled here exactly as specified, and other
 * backends are validated against it.
 *
 * Temporaries are stored zero-extended to 64 bits and always masked to their
 * width, so operands can be read without re-masking.
 */
#include <stdio.h>
#include <stdlib.h>

#include "uam/bus.h"
#include "uam/exec.h"

#include "../ir/ir_eval.h"
#include "execops.h"

/* Value of an operand at width w. */
static inline uint64_t opnd(const uam_block *blk, const uint64_t *t, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        return blk->imm[uam_ref_index(r)] & uam_mask(w);
    return t[r];
}

/* Pure operations: everything that only computes (semantics in src/ir/ir_eval.h). */
static uint64_t eval_pure(const uam_insn *in, uint64_t x, uint64_t y, uint64_t z, unsigned src_w)
{
    uint64_t r;
    if (uam_ir_eval_pure((uam_opcode)in->op, in->width, x, y, z, src_w, &r))
        return r;
    fprintf(stderr, "uam interp: opcode %u is not pure\n", in->op);
    abort();
}

static uint64_t amo_apply(uam_amo_op op, uint64_t old, uint64_t v, unsigned W)
{
    int64_t so = uam_sext(old, W), sv = uam_sext(v, W);
    switch (op) {
    case UAM_AMO_XCHG: return v;
    case UAM_AMO_ADD:  return (old + v) & uam_mask(W);
    case UAM_AMO_AND:  return old & v;
    case UAM_AMO_OR:   return old | v;
    case UAM_AMO_XOR:  return old ^ v;
    case UAM_AMO_SMIN: return so < sv ? old : v;
    case UAM_AMO_SMAX: return so > sv ? old : v;
    case UAM_AMO_UMIN: return old < v ? old : v;
    case UAM_AMO_UMAX: return old > v ? old : v;
    case UAM_AMO__COUNT: break;
    }
    return v;
}

/* Where the current guest instruction stands, for precise exception reports. */
typedef struct insn_pos {
    uint64_t pc;        /* the instruction being executed */
    uint64_t parent_pc; /* the instruction it continues (UAM_IMARK_CONT), else pc */
    bool     cont;
} insn_pos;

static uam_block_result fault(uam_exc_kind kind, const insn_pos *p, uint64_t tval, uint64_t cause)
{
    uam_block_result r = { .kind = UAM_BR_FAULT, .pc = p->pc };
    r.exc.kind = kind;
    r.exc.flags = p->cont ? UAM_EXC_F_CONT : 0;
    r.exc.pc = p->pc;
    r.exc.parent_pc = p->parent_pc;
    r.exc.cause = cause;
    r.exc.tval = tval;
    return r;
}

#define mem_fault_kind(st, store) uam_execops_fault_kind((st), (store))

uam_block_result uam_interp_block(uam_cpu *cpu, const uam_block *blk,
                                  const uam_helper_table *helpers, uint64_t *t)
{
    const unsigned aw = blk->addr_width;
    insn_pos pos = { .pc = blk->guest_pc, .parent_pc = blk->guest_pc, .cont = false };
    uint32_t xf = 0;  /* translation fault sub-cause */
    bool first = true;

    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        const unsigned W = in->width;
        const unsigned size = W / 8;

        /* Guarded op with a false guard: no effect, dst = 0 (section 4.1). */
        if (!uam_ref_is_none(in->guard) && !t[in->guard]) {
            if (!uam_ref_is_none(in->dst))
                t[in->dst] = 0;
            continue;
        }

        switch ((uam_opcode)in->op) {
        case UAM_OP_IMARK: {
            uint64_t pc = opnd(blk, t, in->a, aw);
            const bool cont = in->flags & UAM_IMARK_CONT;
            /* Asynchronous stop point (section 9). Never before the first
             * instruction (so every block makes progress) and never at a
             * continuation boundary such as a branch delay slot, which must
             * retire with the branch (UAM_IMARK_CONT). UAM_IMARK_BREAK ends
             * the block here for an executor hook. */
            if (!first && !cont
                && ((in->flags & UAM_IMARK_BREAK) || cpu->exit_request
                    || cpu->icount >= cpu->icount_deadline))
                return (uam_block_result){ .kind = UAM_BR_INTERRUPTED, .pc = pc };
            first = false;
            /* A continuation keeps the parent of the instruction it continues. */
            pos.parent_pc = cont ? pos.parent_pc : pc;
            pos.pc = pc;
            pos.cont = cont;
            cpu->icount++;
            break;
        }

        case UAM_OP_GET:
            t[in->dst] = uam_cpu_reg_read(cpu, in->aux, W);
            break;
        case UAM_OP_PUT:
            uam_cpu_reg_write(cpu, in->aux, W, opnd(blk, t, in->a, W));
            break;

        case UAM_OP_LD: {
            uint64_t addr = opnd(blk, t, in->a, aw), v = 0;
            uam_status st = uam_execops_load(cpu, addr, in->flags, size, &v, &xf);
            if (st != UAM_OK)
                return fault(mem_fault_kind(st, false), &pos, addr, xf);
            t[in->dst] = v;
            break;
        }
        case UAM_OP_ST: {
            uint64_t addr = opnd(blk, t, in->a, aw);
            uint64_t v = opnd(blk, t, in->b, W);
            if (in->flags & UAM_ACC_X) {
                bool ok = false;
                uam_status st = uam_execops_store_x(cpu, addr, v, in->flags, size, &ok, &xf);
                if (st != UAM_OK)
                    return fault(mem_fault_kind(st, true), &pos, addr, xf);
                t[in->dst] = ok;
                break;
            }
            uam_status st = uam_execops_store(cpu, addr, v, in->flags, size, &xf);
            if (st != UAM_OK)
                return fault(mem_fault_kind(st, true), &pos, addr, xf);
            break;
        }
        case UAM_OP_AMO:
        case UAM_OP_CAS: {
            /* Read-modify-write. Faults on either half are reported as store
             * faults, as architectures do for atomics. */
            uint64_t addr = opnd(blk, t, in->a, aw), pa, old = 0;
            uam_status st = uam_execops_xlate(cpu, addr, UAM_XLATE_STORE, in->flags, size, &pa, &xf);
            if (st == UAM_OK)
                st = uam_bus_read(cpu->bus, pa, size, in->flags, &old);
            if (st != UAM_OK)
                return fault(mem_fault_kind(st, true), &pos, addr, xf);
            bool do_write = true;
            uint64_t nv;
            if (in->op == UAM_OP_AMO) {
                nv = amo_apply((uam_amo_op)in->aux, old, opnd(blk, t, in->b, W), W);
            } else {
                do_write = old == opnd(blk, t, in->b, W);
                nv = opnd(blk, t, in->c, W);
            }
            if (do_write) {
                st = uam_bus_write(cpu->bus, pa, size, in->flags, nv);
                if (st != UAM_OK)
                    return fault(mem_fault_kind(st, true), &pos, addr, 0);
            }
            t[in->dst] = old;
            break;
        }
        case UAM_OP_FENCE:
            /* A single interpreted hart is sequentially consistent, so
             * ordering fences are no-ops. FENCE.INSN needs no work either:
             * the bus invalidates translations on any write to a code page,
             * and the verifier guarantees the block ends right here. */
            break;

        case UAM_OP_EXIT:
            return (uam_block_result){ .kind = UAM_BR_NEXT, .pc = opnd(blk, t, in->a, aw) };

        case UAM_OP_SYSRD: {
            uint64_t v = 0;
            uam_status st = cpu->isa->sysreg_read(cpu, in->aux, &v);
            if (st != UAM_OK)
                return fault(UAM_EXC_SYSREG, &pos, in->aux, 0);
            t[in->dst] = v & uam_mask(W);
            break;
        }
        case UAM_OP_SYSWR: {
            uam_status st = cpu->isa->sysreg_write(cpu, in->aux, opnd(blk, t, in->a, W));
            if (st != UAM_OK)
                return fault(UAM_EXC_SYSREG, &pos, in->aux, 0);
            break;
        }

        case UAM_OP_TRAP: {
            uam_block_result r = { .kind = UAM_BR_TRAP, .pc = pos.pc,
                                   .trap_class = (uam_trap_class)in->aux };
            r.exc.kind = UAM_EXC_TRAP;
            r.exc.flags = pos.cont ? UAM_EXC_F_CONT : 0;
            r.exc.pc = pos.pc;
            r.exc.parent_pc = pos.parent_pc;
            r.exc.cause = opnd(blk, t, in->a, aw);
            r.exc.tval = opnd(blk, t, in->b, aw);
            return r;
        }
        case UAM_OP_WAIT:
            return (uam_block_result){ .kind = UAM_BR_WAIT, .pc = opnd(blk, t, in->a, aw) };

        case UAM_OP_HELPER: {
            const uam_helper_desc *hd = &helpers->desc[in->aux];
            uint64_t args[3] = { 0, 0, 0 };
            const uam_ref refs[3] = { in->a, in->b, in->c };
            for (unsigned k = 0; k < hd->n_args; k++)
                args[k] = opnd(blk, t, refs[k], hd->arg_width[k]);
            uam_status st = UAM_OK;
            uint64_t v = hd->fn(cpu, args[0], args[1], args[2], &st);
            if (st != UAM_OK)
                return fault(UAM_EXC_HELPER, &pos, in->aux, 0);
            if (!uam_ref_is_none(in->dst))
                t[in->dst] = v & uam_mask(hd->result_width);
            break;
        }

        default: {
            /* Pure computation. */
            const uam_opinfo *oi = &uam_opinfo_table[in->op];
            unsigned src_w = W;
            if (in->op == UAM_OP_ZEXT || in->op == UAM_OP_SEXT || in->op == UAM_OP_TRUNC)
                src_w = blk->temp_width[in->a];
            uint64_t x = oi->n_src > 0 ? opnd(blk, t, in->a, in->op == UAM_OP_SELECT ? 1 : src_w) : 0;
            uint64_t y = oi->n_src > 1 ? opnd(blk, t, in->b, W) : 0;
            uint64_t z = oi->n_src > 2 ? opnd(blk, t, in->c, W) : 0;
            t[in->dst] = eval_pure(in, x, y, z, src_w);
            break;
        }
        }
    }

    /* A verified block always ends in a terminator. */
    fprintf(stderr, "uam interp: block 0x%llx fell off the end\n", (unsigned long long)blk->guest_pc);
    abort();
}
