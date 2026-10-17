/*
 * ir_eval.h - Reference semantics of the pure UAM-IR operations (docs/uam-isa.md
 * section 7), shared by the interpreter and the optimizer's constant folder so the
 * two can never disagree. Internal to the core.
 *
 * Operands are zero-extended W-bit values (the source width for conversions);
 * results are zero-extended to their width.
 */
#ifndef UAM_IR_EVAL_H
#define UAM_IR_EVAL_H

#include "uam/ir.h"

static inline uint64_t uam_ir_bswap(uint64_t v, unsigned w)
{
    uint64_t r = 0;
    for (unsigned i = 0; i < w / 8; i++)
        r |= ((v >> (8 * i)) & 0xFF) << (w - 8 - 8 * i);
    return r;
}

/* Evaluate a pure op. Returns false for opcodes that are not pure. */
static inline bool uam_ir_eval_pure(uam_opcode op, unsigned W, uint64_t x, uint64_t y, uint64_t z,
                                    unsigned src_w, uint64_t *out)
{
    const uint64_t m = uam_mask(W);
    const int64_t sx = uam_sext(x, W), sy = uam_sext(y, W);
    const int64_t smin = uam_sext(UINT64_C(1) << (W - 1), W);
    uint64_t r;

    switch (op) {
    case UAM_OP_ADD:  r = (x + y) & m; break;
    case UAM_OP_SUB:  r = (x - y) & m; break;
    case UAM_OP_MUL:  r = (x * y) & m; break;
    case UAM_OP_MULHU:
        r = W == 64 ? (uint64_t)(((unsigned __int128)x * y) >> 64) : ((x * y) >> W) & m;
        break;
    case UAM_OP_MULHS:
        r = W == 64 ? (uint64_t)(((__int128)sx * sy) >> 64) : (uint64_t)((sx * sy) >> W) & m;
        break;
    case UAM_OP_UDIV: r = y == 0 ? m : x / y; break;
    case UAM_OP_UREM: r = y == 0 ? x : x % y; break;
    case UAM_OP_SDIV:
        r = y == 0 ? m : (sx == smin && sy == -1) ? x : (uint64_t)(sx / sy) & m;
        break;
    case UAM_OP_SREM:
        r = y == 0 ? x : (sx == smin && sy == -1) ? 0 : (uint64_t)(sx % sy) & m;
        break;
    case UAM_OP_AND:  r = x & y; break;
    case UAM_OP_OR:   r = x | y; break;
    case UAM_OP_XOR:  r = x ^ y; break;
    case UAM_OP_SHL:  r = y >= W ? 0 : (x << y) & m; break;
    case UAM_OP_LSHR: r = y >= W ? 0 : x >> y; break;
    case UAM_OP_ASHR: r = y >= W ? (sx < 0 ? m : 0) : (uint64_t)(sx >> y) & m; break;
    case UAM_OP_ROTR: {
        unsigned n = (unsigned)(y % W);
        r = n == 0 ? x : ((x >> n) | (x << (W - n))) & m;
        break;
    }
    case UAM_OP_EQ:   r = x == y; break;
    case UAM_OP_NE:   r = x != y; break;
    case UAM_OP_ULT:  r = x < y; break;
    case UAM_OP_ULE:  r = x <= y; break;
    case UAM_OP_SLT:  r = sx < sy; break;
    case UAM_OP_SLE:  r = sx <= sy; break;
    case UAM_OP_CARRY:   r = ((x + y) & m) < x; break;
    case UAM_OP_SCARRY: {
        uint64_t s = (x + y) & m;
        r = ((~(x ^ y) & (x ^ s)) >> (W - 1)) & 1;
        break;
    }
    case UAM_OP_SBORROW: {
        uint64_t s = (x - y) & m;
        r = (((x ^ y) & (x ^ s)) >> (W - 1)) & 1;
        break;
    }
    case UAM_OP_CLZ:    r = x == 0 ? W : (uint64_t)(__builtin_clzll(x) - (64 - (int)W)); break;
    case UAM_OP_CTZ:    r = x == 0 ? W : (uint64_t)__builtin_ctzll(x); break;
    case UAM_OP_POPCNT: r = (uint64_t)__builtin_popcountll(x); break;
    case UAM_OP_BSWAP:  r = uam_ir_bswap(x, W); break;
    case UAM_OP_ZEXT:   r = x; break;
    case UAM_OP_SEXT:   r = (uint64_t)uam_sext(x, src_w) & m; break;
    case UAM_OP_TRUNC:  r = x & m; break;
    case UAM_OP_SELECT: r = x ? y : z; break;
    default:
        return false;
    }
    *out = r;
    return true;
}

#endif /* UAM_IR_EVAL_H */
