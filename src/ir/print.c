/*
 * print.c - Canonical text form of UAM-IR (docs/uam-isa.md section 12).
 */
#include "uam/ir.h"

static const char *const amo_names[] = {
    "xchg", "add", "and", "or", "xor", "smin", "smax", "umin", "umax",
};
static const char *const fence_names[] = { "acq", "rel", "full", "insn" };
static const char *const exit_names[]  = { "jump", "call", "ret", "exc_return" };
static const char *const trap_names[]  = { "exception", "debug", "halt" };

static void print_ref(FILE *out, const uam_block *blk, uam_ref r)
{
    if (uam_ref_is_none(r))
        fputs("_", out);
    else if (uam_ref_is_imm(r)) {
        uint32_t i = uam_ref_index(r);
        uint64_t v = i < blk->n_imm ? blk->imm[i] : 0;
        if (v < 10)
            fprintf(out, "%llu", (unsigned long long)v);
        else
            fprintf(out, "0x%llx", (unsigned long long)v);
    } else
        fprintf(out, "t%u", r);
}

static const char *reg_name(const uam_print_ctx *ctx, uint32_t off, unsigned w)
{
    if (!ctx || !ctx->regs)
        return NULL;
    for (size_t i = 0; i < ctx->n_regs; i++)
        if (ctx->regs[i].offset == off && ctx->regs[i].width == w)
            return ctx->regs[i].name;
    return NULL;
}

static void print_mem_flags(FILE *out, unsigned f)
{
    fputs((f & UAM_ACC_BE) ? ".be" : ".le", out);
    if (f & UAM_ACC_X)       fputs(".x", out);
    if (f & UAM_ACC_ALIGNED) fputs(".al", out);
    if ((f & UAM_ACC_ACQ) && (f & UAM_ACC_REL)) fputs(".sc", out);
    else if (f & UAM_ACC_ACQ) fputs(".acq", out);
    else if (f & UAM_ACC_REL) fputs(".rel", out);
    if (f & UAM_ACC_UNPRIV)  fputs(".unpriv", out);
    if (f & UAM_ACC_PHYS)    fputs(".phys", out);
}

void uam_print_insn(FILE *out, const uam_block *blk, const uam_insn *in, const uam_print_ctx *ctx)
{
    const uam_opinfo *oi = &uam_opinfo_table[in->op];
    char lhs[32] = "";

    if (in->op == UAM_OP_IMARK) {
        fputs("imark   ", out);
        print_ref(out, blk, in->a);
        fprintf(out, " {len=%u%s%s}\n", in->aux, (in->flags & UAM_IMARK_CONT) ? ",cont" : "",
                (in->flags & UAM_IMARK_BREAK) ? ",break" : "");
        return;
    }

    if (!uam_ref_is_none(in->guard))
        snprintf(lhs, sizeof(lhs), "[t%u] ", in->guard);
    fprintf(out, "  %-7s", lhs);
    if (!uam_ref_is_none(in->dst)) {
        char d[24];
        snprintf(d, sizeof(d), "t%u:%u", in->dst, blk->temp_width[in->dst]);
        fprintf(out, "%-6s = ", d);
    } else {
        fputs("         ", out);
    }

    fputs(oi->mnemonic, out);
    if (in->op == UAM_OP_LD || in->op == UAM_OP_ST || in->op == UAM_OP_AMO || in->op == UAM_OP_CAS)
        print_mem_flags(out, in->flags);
    if (in->op == UAM_OP_AMO && in->aux < UAM_ARRAY_LEN(amo_names))
        fprintf(out, ".%s", amo_names[in->aux]);
    fprintf(out, ".%u ", in->width);

    const uam_ref srcs[3] = { in->a, in->b, in->c };
    unsigned n = oi->n_src;
    if (in->op == UAM_OP_HELPER) {
        n = 0;
        for (unsigned i = 0; i < 3; i++)
            if (!uam_ref_is_none(srcs[i]))
                n = i + 1;
    }
    for (unsigned i = 0; i < n; i++) {
        if (i)
            fputs(", ", out);
        print_ref(out, blk, srcs[i]);
    }

    switch ((uam_opcode)in->op) {
    case UAM_OP_GET:
    case UAM_OP_PUT: {
        const char *nm = reg_name(ctx, in->aux, in->width);
        if (nm)
            fprintf(out, " {%s}", nm);
        else
            fprintf(out, " {reg+%u}", in->aux);
        break;
    }
    case UAM_OP_FENCE:
        fprintf(out, " {%s}", in->aux < 4 ? fence_names[in->aux] : "?");
        break;
    case UAM_OP_EXIT:
        fprintf(out, " {%s}", in->aux < 4 ? exit_names[in->aux] : "?");
        break;
    case UAM_OP_TRAP:
        fprintf(out, " {%s}", in->aux < 3 ? trap_names[in->aux] : "?");
        break;
    case UAM_OP_SYSRD:
    case UAM_OP_SYSWR:
        fprintf(out, " {sys 0x%x}", in->aux);
        break;
    case UAM_OP_HELPER:
        if (ctx && ctx->helpers && in->aux < ctx->helpers->count)
            fprintf(out, " {%s}", ctx->helpers->desc[in->aux].name);
        else
            fprintf(out, " {helper %u}", in->aux);
        break;
    default:
        break;
    }
    fputc('\n', out);
}

void uam_print_block(FILE *out, const uam_block *blk, const uam_print_ctx *ctx)
{
    fprintf(out, "block 0x%llx: %u guest insns, %u ops, %u temps\n",
            (unsigned long long)blk->guest_pc, blk->n_guest, blk->n_insn, blk->n_temp);
    for (uint32_t i = 0; i < blk->n_insn; i++)
        uam_print_insn(out, blk, &blk->insn[i], ctx);
}
