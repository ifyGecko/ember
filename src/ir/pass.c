/*
 * pass.c - Block editing for IR passes (uam/pass.h).
 *
 * Registration of passes lives with the executor (src/exec/run.c); this file only
 * changes blocks, so it depends on nothing but the IR.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/pass.h"

uam_ref uam_block_imm(uam_block *blk, uint64_t v)
{
    for (uint32_t i = 0; i < blk->n_imm; i++)
        if (blk->imm[i] == v)
            return UAM_IMM_BIT | i;
    uint64_t *ni = realloc(blk->imm, sizeof(*ni) * (blk->n_imm + 1));
    if (!ni)
        return UAM_REF_NONE;
    blk->imm = ni;
    blk->imm[blk->n_imm] = v;
    return UAM_IMM_BIT | blk->n_imm++;
}

uam_status uam_block_insert(uam_block *blk, const uint32_t *at, const uam_insn *ops, uint32_t n)
{
    if (!n)
        return UAM_OK;
    for (uint32_t k = 0; k < n; k++) {
        if (at[k] > blk->n_insn || (k && at[k] < at[k - 1]))
            return UAM_ERR_INVALID;
        if (ops[k].op == UAM_OP_IMARK || !uam_ref_is_none(ops[k].dst))
            return UAM_ERR_INVALID;
    }
    uam_insn *ni = malloc(sizeof(*ni) * ((size_t)blk->n_insn + n));
    if (!ni)
        return UAM_ERR_NOMEM;
    uint32_t w = 0, k = 0;
    for (uint32_t i = 0; i <= blk->n_insn; i++) {
        while (k < n && at[k] == i)
            ni[w++] = ops[k++];
        if (i < blk->n_insn)
            ni[w++] = blk->insn[i];
    }
    free(blk->insn);
    blk->insn = ni;
    blk->n_insn = w;
    return UAM_OK;
}
