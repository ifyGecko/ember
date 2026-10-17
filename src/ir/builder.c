/*
 * builder.c - Opcode table and block builder (uam/ir.h).
 *
 * The builder is deliberately dumb: it appends instructions and allocates
 * temporaries. All validation lives in verify.c so that one place defines
 * what a well-formed block is.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/ir.h"

const uam_opinfo uam_opinfo_table[UAM_OP__COUNT] = {
#define UAM_X_INFO(name, mn, nsrc, rk, props) \
    [UAM_OP_##name] = { mn, nsrc, rk, props },
    UAM_OPCODES(UAM_X_INFO)
#undef UAM_X_INFO
};

/* ---- growth helpers ---------------------------------------------------- */

static bool grow(void **ptr, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    uint32_t ncap = *cap ? *cap : 16;
    while (ncap < need)
        ncap *= 2;
    void *p = realloc(*ptr, (size_t)ncap * elem);
    if (!p)
        return false;
    *ptr = p;
    *cap = ncap;
    return true;
}

/* ---- lifecycle --------------------------------------------------------- */

void uam_builder_init(uam_builder *b, unsigned addr_width)
{
    memset(b, 0, sizeof(*b));
    b->addr_width = (uint8_t)addr_width;
}

void uam_builder_free(uam_builder *b)
{
    free(b->insn);
    free(b->tw);
    free(b->imm);
    memset(b, 0, sizeof(*b));
}

void uam_builder_begin(uam_builder *b, uint64_t guest_pc)
{
    b->guest_pc = guest_pc;
    b->n_guest = 0;
    b->n_insn = 0;
    b->n_temp = 0;
    b->n_imm = 0;
    b->n_pages = 0;
    b->oom = false;
}

bool uam_builder_add_page(uam_builder *b, uint64_t page_base)
{
    for (uint32_t i = 0; i < b->n_pages; i++)
        if (b->page[i] == page_base)
            return true;
    if (b->n_pages == 2)
        return false;
    b->page[b->n_pages++] = page_base;
    return true;
}

uam_block *uam_builder_finish(uam_builder *b)
{
    if (b->oom)
        return NULL;
    uam_block *blk = calloc(1, sizeof(*blk));
    if (!blk)
        return NULL;
    blk->guest_pc = b->guest_pc;
    blk->addr_width = b->addr_width;
    blk->n_insn = b->n_insn;
    blk->n_temp = b->n_temp;
    blk->n_imm = b->n_imm;
    blk->n_guest = b->n_guest;
    blk->n_pages = b->n_pages;
    memcpy(blk->page, b->page, sizeof(blk->page));
    blk->insn = malloc(sizeof(uam_insn) * (b->n_insn ? b->n_insn : 1));
    blk->temp_width = malloc(b->n_temp ? b->n_temp : 1);
    blk->imm = malloc(sizeof(uint64_t) * (b->n_imm ? b->n_imm : 1));
    if (!blk->insn || !blk->temp_width || !blk->imm) {
        uam_block_free(blk);
        return NULL;
    }
    /* Pools may be empty (and their pointers NULL); memcpy needs valid pointers. */
    if (b->n_insn)
        memcpy(blk->insn, b->insn, sizeof(uam_insn) * b->n_insn);
    if (b->n_temp)
        memcpy(blk->temp_width, b->tw, b->n_temp);
    if (b->n_imm)
        memcpy(blk->imm, b->imm, sizeof(uint64_t) * b->n_imm);
    return blk;
}

void uam_block_free(uam_block *blk)
{
    if (!blk)
        return;
    if (blk->backend_free)
        blk->backend_free(blk->backend);
    free(blk->insn);
    free(blk->temp_width);
    free(blk->imm);
    free(blk);
}

/* ---- operands ---------------------------------------------------------- */

uam_ref uam_builder_new_temp(uam_builder *b, unsigned width)
{
    if (!grow((void **)&b->tw, &b->cap_temp, b->n_temp + 1, 1)) {
        b->oom = true;
        return UAM_REF_NONE;
    }
    b->tw[b->n_temp] = (uint8_t)width;
    return b->n_temp++;
}

unsigned uam_builder_ref_width(const uam_builder *b, uam_ref r)
{
    if (uam_ref_is_none(r) || uam_ref_is_imm(r) || r >= b->n_temp)
        return 0;
    return b->tw[r];
}

uam_ref uam_imm(uam_builder *b, uint64_t value)
{
    /* Reuse an identical constant if it is already in the pool. */
    for (uint32_t i = 0; i < b->n_imm; i++)
        if (b->imm[i] == value)
            return UAM_IMM_BIT | i;
    if (!grow((void **)&b->imm, &b->cap_imm, b->n_imm + 1, sizeof(uint64_t))) {
        b->oom = true;
        return UAM_REF_NONE;
    }
    b->imm[b->n_imm] = value;
    return UAM_IMM_BIT | b->n_imm++;
}

/* ---- emitters ---------------------------------------------------------- */

uam_ref uam_emit(uam_builder *b, uam_opcode op, unsigned width, unsigned flags,
                 uint32_t aux, uam_ref guard, uam_ref a, uam_ref bb, uam_ref c,
                 unsigned dst_width)
{
    if (!grow((void **)&b->insn, &b->cap_insn, b->n_insn + 1, sizeof(uam_insn))) {
        b->oom = true;
        return UAM_REF_NONE;
    }
    uam_ref dst = dst_width ? uam_builder_new_temp(b, dst_width) : UAM_REF_NONE;
    uam_insn *in = &b->insn[b->n_insn++];
    in->op = (uint8_t)op;
    in->width = (uint8_t)width;
    in->flags = (uint16_t)flags;
    in->aux = aux;
    in->guard = guard;
    in->dst = dst;
    in->a = a;
    in->b = bb;
    in->c = c;
    return dst;
}

#define NONE UAM_REF_NONE

void uam_imark(uam_builder *b, uint64_t pc, unsigned len)
{
    b->n_guest++;
    uam_emit(b, UAM_OP_IMARK, b->addr_width, 0, len, NONE, uam_imm(b, pc), NONE, NONE, 0);
}

void uam_imark_cont(uam_builder *b, uint64_t pc, unsigned len)
{
    b->n_guest++;
    uam_emit(b, UAM_OP_IMARK, b->addr_width, UAM_IMARK_CONT, len, NONE, uam_imm(b, pc), NONE, NONE, 0);
}

uam_ref uam_get(uam_builder *b, unsigned w, uint32_t reg_off)
{
    return uam_emit(b, UAM_OP_GET, w, 0, reg_off, NONE, NONE, NONE, NONE, w);
}

void uam_put(uam_builder *b, unsigned w, uint32_t reg_off, uam_ref v, uam_ref guard)
{
    uam_emit(b, UAM_OP_PUT, w, 0, reg_off, guard, v, NONE, NONE, 0);
}

uam_ref uam_ld(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref guard)
{
    return uam_emit(b, UAM_OP_LD, w, flags, 0, guard, addr, NONE, NONE, w);
}

void uam_st(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref v, uam_ref guard)
{
    uam_emit(b, UAM_OP_ST, w, flags & ~UAM_ACC_X, 0, guard, addr, v, NONE, 0);
}

uam_ref uam_stx(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref v, uam_ref guard)
{
    return uam_emit(b, UAM_OP_ST, w, flags | UAM_ACC_X, 0, guard, addr, v, NONE, 1);
}

uam_ref uam_amo(uam_builder *b, unsigned w, unsigned flags, uam_amo_op sub, uam_ref addr,
                uam_ref v, uam_ref guard)
{
    return uam_emit(b, UAM_OP_AMO, w, flags, (uint32_t)sub, guard, addr, v, NONE, w);
}

uam_ref uam_cas(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref expect,
                uam_ref nv, uam_ref guard)
{
    return uam_emit(b, UAM_OP_CAS, w, flags, 0, guard, addr, expect, nv, w);
}

void uam_fence(uam_builder *b, uam_fence_kind kind)
{
    uam_emit(b, UAM_OP_FENCE, b->addr_width, 0, (uint32_t)kind, NONE, NONE, NONE, NONE, 0);
}

uam_ref uam_binop(uam_builder *b, uam_opcode op, unsigned w, uam_ref x, uam_ref y)
{
    unsigned dw = uam_opinfo_table[op].result == UAM_RK_BOOL ? 1 : w;
    return uam_emit(b, op, w, 0, 0, NONE, x, y, NONE, dw);
}

uam_ref uam_unop(uam_builder *b, uam_opcode op, unsigned w, uam_ref x)
{
    return uam_emit(b, op, w, 0, 0, NONE, x, NONE, NONE, w);
}

uam_ref uam_select(uam_builder *b, unsigned w, uam_ref cond, uam_ref x, uam_ref y)
{
    return uam_emit(b, UAM_OP_SELECT, w, 0, 0, NONE, cond, x, y, w);
}

void uam_exit(uam_builder *b, uam_ref guard, uam_ref target, uam_exit_kind kind)
{
    uam_emit(b, UAM_OP_EXIT, b->addr_width, 0, (uint32_t)kind, guard, target, NONE, NONE, 0);
}

uam_ref uam_sysrd(uam_builder *b, unsigned w, uint32_t id, uam_ref guard)
{
    return uam_emit(b, UAM_OP_SYSRD, w, 0, id, guard, NONE, NONE, NONE, w);
}

void uam_syswr(uam_builder *b, unsigned w, uint32_t id, uam_ref v, uam_ref guard)
{
    uam_emit(b, UAM_OP_SYSWR, w, 0, id, guard, v, NONE, NONE, 0);
}

void uam_trap(uam_builder *b, uam_ref guard, uam_trap_class cls, uam_ref cause, uam_ref tval)
{
    uam_emit(b, UAM_OP_TRAP, b->addr_width, 0, (uint32_t)cls, guard, cause, tval, NONE, 0);
}

void uam_wait(uam_builder *b, uam_ref next_pc)
{
    uam_emit(b, UAM_OP_WAIT, b->addr_width, 0, 0, NONE, next_pc, NONE, NONE, 0);
}

uam_ref uam_helper(uam_builder *b, const uam_helper_table *ht, uint32_t id,
                   uam_ref a, uam_ref bb, uam_ref c, uam_ref guard)
{
    unsigned rw = (ht && id < ht->count) ? ht->desc[id].result_width : 0;
    return uam_emit(b, UAM_OP_HELPER, rw ? rw : b->addr_width, 0, id, guard, a, bb, c, rw);
}
