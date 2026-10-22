/*
 * arm_lift.c - ARM (A32) decoder and lifter, and the emitters it shares with the
 * Thumb decoder (arm_thumb.c).
 *
 * Covers the ARMv7-A integer instruction set: data processing with every shifter
 * operand form and flag rule, multiplies (including the long, halfword, word-by-
 * halfword, most-significant-word and dual forms), divides, saturating
 * arithmetic, the media group (parallel add/subtract, pack, extend, select,
 * reverse, bit-field), loads and stores in all addressing modes, LDM/STM (with ^),
 * exclusives, SWP, branches with interworking, status-register access (MRS, MSR,
 * CPS, SRS, RFE), CP14/CP15 transfers, hints and barriers. Everything else -
 * VFP/NEON and other coprocessors, Jazelle, SMC/HVC, banked-register MRS/MSR -
 * takes the Undefined Instruction exception.
 *
 * Data accesses use the byte order CPSR.E selects (ARM_PRIV_BE in cpu->priv, so code
 * lifted for one order is never run in the other); instructions are always fetched
 * little-endian (BE8).
 *
 * Conditional execution: the condition becomes a guard on every side effect of the
 * instruction (PUT, LD, ST, SYSWR, HELPER, TRAP, EXIT), so a block runs straight
 * through conditional code; an instruction that may branch ends the block with a
 * guarded exit and an unguarded fall-through.
 *
 * Thumb state is bit 0 of the pc (arm.h). IT blocks (arm_thumb.c) are lifted as
 * continuations (UAM_IMARK_CONT) so an interrupt cannot split one; a block that has
 * to stop inside one records the remaining IT state in cpu->priv
 * (ARM_HELPER_SET_IT), and the next block is lifted for it.
 */
#include "arm.h"

#define NONE UAM_REF_NONE

/* ---- operand helpers --------------------------------------------------- */

uam_ref arm_imm(arm_lift *L, uint32_t v) { return uam_imm(L->b, v); }
uam_ref arm_bin(arm_lift *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 32, a, b); }

static uam_ref imm(arm_lift *L, uint32_t v) { return uam_imm(L->b, v); }
static uam_ref bin(arm_lift *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 32, a, b); }
static uam_ref bin64(arm_lift *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 64, a, b); }
static uam_ref bin1(arm_lift *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 1, a, b); }
static uam_ref add(arm_lift *L, uam_ref a, uam_ref b) { return bin(L, UAM_OP_ADD, a, b); }
static uam_ref sub(arm_lift *L, uam_ref a, uam_ref b) { return bin(L, UAM_OP_SUB, a, b); }
static uam_ref and_(arm_lift *L, uam_ref a, uint32_t m) { return bin(L, UAM_OP_AND, a, imm(L, m)); }
static uam_ref or_(arm_lift *L, uam_ref a, uam_ref b) { return bin(L, UAM_OP_OR, a, b); }
static uam_ref shl(arm_lift *L, uam_ref a, unsigned n) { return bin(L, UAM_OP_SHL, a, imm(L, n)); }
static uam_ref lshr(arm_lift *L, uam_ref a, unsigned n) { return bin(L, UAM_OP_LSHR, a, imm(L, n)); }
static uam_ref ashr(arm_lift *L, uam_ref a, unsigned n) { return bin(L, UAM_OP_ASHR, a, imm(L, n)); }
static uam_ref not_(arm_lift *L, uam_ref a) { return bin(L, UAM_OP_XOR, a, imm(L, 0xFFFFFFFFu)); }
static uam_ref not1(arm_lift *L, uam_ref c) { return bin1(L, UAM_OP_XOR, c, imm(L, 1)); }
static uam_ref sel(arm_lift *L, uam_ref c, uam_ref a, uam_ref b) { return uam_select(L->b, 32, c, a, b); }

static bool is_imm(uam_ref r) { return uam_ref_is_imm(r); }
static uint64_t imm_val(arm_lift *L, uam_ref r) { return L->b->imm[uam_ref_index(r)]; }

/* Conversions need a temporary source; immediates are converted as constants. */
static uam_ref conv(arm_lift *L, uam_opcode op, unsigned w, uam_ref v, unsigned from)
{
    if (is_imm(v)) {
        uint64_t x = imm_val(L, v) & uam_mask(from);
        if (op == UAM_OP_SEXT)
            x = (uint64_t)uam_sext(x, from);
        return imm(L, (uint32_t)(x & uam_mask(w)));
    }
    return uam_unop(L->b, op, w, v);
}
static uam_ref trunc_(arm_lift *L, uam_ref v, unsigned w) { return conv(L, UAM_OP_TRUNC, w, v, 64); }
static uam_ref to1(arm_lift *L, uam_ref v) { return trunc_(L, v, 1); }
static uam_ref z32(arm_lift *L, uam_ref c) { return conv(L, UAM_OP_ZEXT, 32, c, 1); }
static uam_ref ext64(arm_lift *L, uam_ref v, bool sign)
{
    if (is_imm(v)) {
        uint64_t x = (uint32_t)imm_val(L, v);
        return uam_imm(L->b, sign ? (uint64_t)(int64_t)(int32_t)x : x);
    }
    return uam_unop(L->b, sign ? UAM_OP_SEXT : UAM_OP_ZEXT, 64, v);
}
static uam_ref lo32(arm_lift *L, uam_ref v64) { return trunc_(L, v64, 32); }
static uam_ref hi32(arm_lift *L, uam_ref v64) { return trunc_(L, bin64(L, UAM_OP_LSHR, v64, imm(L, 32)), 32); }
/* Sign/zero-extend the low `from` bits of a 32-bit value. */
static uam_ref ext_from(arm_lift *L, uam_ref v, unsigned from, bool sign)
{
    if (from >= 32)
        return v;
    if (!sign)
        return and_(L, v, (uint32_t)uam_mask(from));
    return conv(L, UAM_OP_SEXT, 32, trunc_(L, v, from), from);
}

uam_ref arm_reg(arm_lift *L, unsigned r)
{
    if (r == ARM_PC)
        return imm(L, L->pc + (L->thumb ? 4 : 8));
    return uam_get(L->b, 32, ARM_REG_OFF(r));
}

uam_ref arm_reg_align(arm_lift *L, unsigned r)
{
    if (r == ARM_PC)
        return imm(L, (L->pc + (L->thumb ? 4 : 8)) & ~3u);
    return uam_get(L->b, 32, ARM_REG_OFF(r));
}

void arm_put(arm_lift *L, unsigned r, uam_ref v)
{
    if (r < ARM_PC)
        uam_put(L->b, 32, ARM_REG_OFF(r), v, L->guard);
}

uam_ref arm_flag(arm_lift *L, uint32_t off)
{
    return uam_unop(L->b, UAM_OP_TRUNC, 1, uam_get(L->b, 8, off));
}

void arm_put_flag(arm_lift *L, uint32_t off, uam_ref v1)
{
    uam_ref v8 = is_imm(v1) ? imm(L, (uint32_t)imm_val(L, v1) & 1)
                            : uam_unop(L->b, UAM_OP_ZEXT, 8, v1);
    uam_put(L->b, 8, off, v8, L->guard);
}

static void put_nz(arm_lift *L, uam_ref r)
{
    arm_put_flag(L, ARM_N_OFF, bin(L, UAM_OP_SLT, r, imm(L, 0)));
    arm_put_flag(L, ARM_Z_OFF, bin(L, UAM_OP_EQ, r, imm(L, 0)));
}

/* Q |= c */
static void set_q(arm_lift *L, uam_ref c)
{
    arm_put_flag(L, ARM_Q_OFF, bin1(L, UAM_OP_OR, arm_flag(L, ARM_Q_OFF), c));
}

uam_ref arm_cond(arm_lift *L, unsigned cond)
{
    uam_ref n, z, c, v;
    switch (cond) {
    case 0x0: return arm_flag(L, ARM_Z_OFF);
    case 0x1: return not1(L, arm_flag(L, ARM_Z_OFF));
    case 0x2: return arm_flag(L, ARM_C_OFF);
    case 0x3: return not1(L, arm_flag(L, ARM_C_OFF));
    case 0x4: return arm_flag(L, ARM_N_OFF);
    case 0x5: return not1(L, arm_flag(L, ARM_N_OFF));
    case 0x6: return arm_flag(L, ARM_V_OFF);
    case 0x7: return not1(L, arm_flag(L, ARM_V_OFF));
    case 0x8:
    case 0x9:
        c = arm_flag(L, ARM_C_OFF);
        z = arm_flag(L, ARM_Z_OFF);
        c = bin1(L, UAM_OP_AND, c, not1(L, z));
        return cond == 0x8 ? c : not1(L, c);
    case 0xA:
    case 0xB:
        n = arm_flag(L, ARM_N_OFF);
        v = arm_flag(L, ARM_V_OFF);
        return bin1(L, cond == 0xA ? UAM_OP_EQ : UAM_OP_NE, n, v);
    case 0xC:
    case 0xD:
        n = arm_flag(L, ARM_N_OFF);
        v = arm_flag(L, ARM_V_OFF);
        z = arm_flag(L, ARM_Z_OFF);
        c = bin1(L, UAM_OP_AND, not1(L, z), bin1(L, UAM_OP_EQ, n, v));
        return cond == 0xC ? c : not1(L, c);
    default:
        return NONE;
    }
}

uint32_t arm_next_pc(const arm_lift *L)
{
    return (L->pc + L->len) | (L->thumb ? 1u : 0u);
}

void arm_trap(arm_lift *L, uint32_t cause, uint32_t tval)
{
    uam_trap(L->b, L->guard, UAM_TRAP_EXCEPTION, imm(L, cause), imm(L, tval));
}

arm_lift_result arm_undef(arm_lift *L)
{
    arm_trap(L, ARM_EXC_UNDEF, L->raw);
    return ARM_DONE;
}

/* ---- control flow ------------------------------------------------------ */

/*
 * Leave the block for a branch target. In a block that started inside an IT block
 * (cpu->priv carries IT state), a branch also leaves the IT block: clear the state
 * on the way out.
 */
static void exit_to(arm_lift *L, uam_ref target, uam_exit_kind kind)
{
    if (ARM_PRIV_IT(L->lc->cpu->priv))
        uam_helper(L->b, &L->lc->cpu->isa->helpers, ARM_HELPER_SET_IT, imm(L, 0), NONE, NONE,
                   L->guard);
    uam_exit(L->b, L->guard, target, kind);
}

arm_lift_result arm_branch(arm_lift *L, uam_ref target, uam_exit_kind kind)
{
    exit_to(L, target, kind);
    return ARM_DONE;
}

/* BXWritePC: bit 0 selects Thumb; an ARM target is word aligned. */
uam_ref arm_bx_target(arm_lift *L, uam_ref addr)
{
    if (is_imm(addr)) {
        uint32_t a = (uint32_t)imm_val(L, addr);
        return imm(L, (a & 1) ? a : a & ~3u);
    }
    return sel(L, to1(L, addr), addr, and_(L, addr, ~3u));
}

arm_lift_result arm_bx(arm_lift *L, uam_ref addr, uam_exit_kind kind)
{
    return arm_branch(L, arm_bx_target(L, addr), kind);
}

/* Write the pc from a data-processing result: ARM interworks, Thumb stays Thumb. */
static arm_lift_result alu_write_pc(arm_lift *L, uam_ref v)
{
    if (L->thumb)
        return arm_branch(L, or_(L, v, imm(L, 1)), UAM_EXIT_JUMP);
    return arm_bx(L, v, UAM_EXIT_JUMP);
}

/* Exception return: CPSR = psr (or SPSR), continue at pc. */
static arm_lift_result eret(arm_lift *L, uam_ref pc, uam_ref psr)
{
    uam_ref target = uam_helper(L->b, &L->lc->cpu->isa->helpers, ARM_HELPER_ERET, pc,
                                uam_ref_is_none(psr) ? imm(L, 0) : psr,
                                imm(L, uam_ref_is_none(psr) ? 0 : 1), L->guard);
    /* The helper sets the IT state from the PSR itself. */
    uam_exit(L->b, L->guard, target, UAM_EXIT_EXC_RETURN);
    return ARM_DONE;
}

/* ---- shifter operands --------------------------------------------------- */

static uam_ref bit_of(arm_lift *L, uam_ref x, unsigned k) { return to1(L, lshr(L, x, k)); }

arm_shres arm_shift_imm(arm_lift *L, uam_ref x, unsigned type, unsigned amt)
{
    arm_shres r = { x, NONE };
    switch (type) {
    case 0: /* LSL */
        if (amt) {
            r.v = shl(L, x, amt);
            r.c = bit_of(L, x, 32 - amt);
        }
        break;
    case 1: /* LSR; #0 means #32 */
        if (!amt) {
            r.v = imm(L, 0);
            r.c = bit_of(L, x, 31);
        } else {
            r.v = lshr(L, x, amt);
            r.c = bit_of(L, x, amt - 1);
        }
        break;
    case 2: /* ASR; #0 means #32 */
        if (!amt) {
            r.v = ashr(L, x, 31);
            r.c = bit_of(L, x, 31);
        } else {
            r.v = ashr(L, x, amt);
            r.c = bit_of(L, x, amt - 1);
        }
        break;
    default: /* ROR; #0 means RRX */
        if (!amt) {
            r.v = or_(L, shl(L, z32(L, arm_flag(L, ARM_C_OFF)), 31), lshr(L, x, 1));
            r.c = to1(L, x);
        } else {
            r.v = bin(L, UAM_OP_ROTR, x, imm(L, amt));
            r.c = bit_of(L, r.v, 31);
        }
        break;
    }
    return r;
}

/* Register-specified amounts use the bottom byte, unmasked: the IR's shift rule
 * (an amount >= 32 gives 0, or all sign bits) is ARM's. */
arm_shres arm_shift_reg(arm_lift *L, uam_ref x, unsigned type, uam_ref amt)
{
    uam_ref n = and_(L, amt, 0xFF);
    uam_ref zero = bin(L, UAM_OP_EQ, n, imm(L, 0));
    uam_ref c = arm_flag(L, ARM_C_OFF);
    arm_shres r;
    switch (type) {
    case 0:
        r.v = bin(L, UAM_OP_SHL, x, n);
        r.c = to1(L, bin(L, UAM_OP_LSHR, x, sub(L, imm(L, 32), n)));
        break;
    case 1:
        r.v = bin(L, UAM_OP_LSHR, x, n);
        r.c = to1(L, bin(L, UAM_OP_LSHR, x, sub(L, n, imm(L, 1))));
        break;
    case 2:
        r.v = bin(L, UAM_OP_ASHR, x, n);
        r.c = to1(L, bin(L, UAM_OP_ASHR, x, sub(L, n, imm(L, 1))));
        break;
    default:
        r.v = bin(L, UAM_OP_ROTR, x, n);
        r.c = bit_of(L, r.v, 31);
        break;
    }
    r.c = uam_select(L->b, 1, zero, c, r.c);
    return r;
}

/* ---- data processing --------------------------------------------------- */

/* a + b + cin with ARM's C and V. kind: 0 add, 1 sub (b is the subtrahend),
 * 2 adc, 3 sbc (b is the subtrahend). */
static uam_ref addsub(arm_lift *L, uam_ref a, uam_ref b, int kind, uam_ref *c, uam_ref *v)
{
    uam_ref r;
    switch (kind) {
    case 0:
        r = add(L, a, b);
        *c = bin(L, UAM_OP_CARRY, a, b);
        *v = bin(L, UAM_OP_SCARRY, a, b);
        return r;
    case 1:
        r = sub(L, a, b);
        *c = bin(L, UAM_OP_ULE, b, a);         /* no borrow */
        *v = bin(L, UAM_OP_SBORROW, a, b);
        return r;
    default: {
        uam_ref bb = kind == 3 ? not_(L, b) : b;
        uam_ref cin = z32(L, arm_flag(L, ARM_C_OFF));
        uam_ref s = add(L, a, bb);
        r = add(L, s, cin);
        *c = bin1(L, UAM_OP_OR, bin(L, UAM_OP_CARRY, a, bb), bin(L, UAM_OP_CARRY, s, cin));
        *v = bin(L, UAM_OP_SLT, bin(L, UAM_OP_AND, bin(L, UAM_OP_XOR, a, r),
                                    bin(L, UAM_OP_XOR, bb, r)), imm(L, 0));
        return r;
    }
    }
}

arm_lift_result arm_dp(arm_lift *L, unsigned op, bool s, unsigned rd, uam_ref a, arm_shres b)
{
    uam_ref r = NONE, c = NONE, v = NONE;
    bool logical = true, write = true;
    switch (op) {
    case DP_AND: r = bin(L, UAM_OP_AND, a, b.v); break;
    case DP_EOR: r = bin(L, UAM_OP_XOR, a, b.v); break;
    case DP_ORR: r = bin(L, UAM_OP_OR, a, b.v); break;
    case DP_ORN: r = bin(L, UAM_OP_OR, a, not_(L, b.v)); break;
    case DP_BIC: r = bin(L, UAM_OP_AND, a, not_(L, b.v)); break;
    case DP_MOV: r = b.v; break;
    case DP_MVN: r = not_(L, b.v); break;
    case DP_TST: r = bin(L, UAM_OP_AND, a, b.v); write = false; break;
    case DP_TEQ: r = bin(L, UAM_OP_XOR, a, b.v); write = false; break;
    default:
        logical = false;
        switch (op) {
        case DP_ADD: r = addsub(L, a, b.v, 0, &c, &v); break;
        case DP_SUB: r = addsub(L, a, b.v, 1, &c, &v); break;
        case DP_RSB: r = addsub(L, b.v, a, 1, &c, &v); break;
        case DP_ADC: r = addsub(L, a, b.v, 2, &c, &v); break;
        case DP_SBC: r = addsub(L, a, b.v, 3, &c, &v); break;
        case DP_RSC: r = addsub(L, b.v, a, 3, &c, &v); break;
        case DP_CMP: r = addsub(L, a, b.v, 1, &c, &v); write = false; break;
        default:     r = addsub(L, a, b.v, 0, &c, &v); write = false; break; /* CMN */
        }
        break;
    }
    if (write && rd == ARM_PC) {
        if (s)
            return eret(L, r, NONE);   /* e.g. SUBS pc, lr, #4 */
        return alu_write_pc(L, r);
    }
    if (s) {
        put_nz(L, r);
        if (logical) {
            if (!uam_ref_is_none(b.c))
                arm_put_flag(L, ARM_C_OFF, b.c);
        } else {
            arm_put_flag(L, ARM_C_OFF, c);
            arm_put_flag(L, ARM_V_OFF, v);
        }
    }
    if (write)
        arm_put(L, rd, r);
    return ARM_SEQ;
}

/* ---- loads and stores -------------------------------------------------- */

static uam_ref load(arm_lift *L, unsigned size, unsigned flags, uam_ref addr)
{
    return uam_ld(L->b, size * 8, L->endian | flags, addr, L->guard);
}

static void store(arm_lift *L, unsigned size, unsigned flags, uam_ref addr, uam_ref v)
{
    if (size < 4)
        v = trunc_(L, v, size * 8);
    uam_st(L->b, size * 8, L->endian | flags, addr, v, L->guard);
}

/* The value a store of r15 writes (ARM: pc+8; Thumb cannot store pc). */
static uam_ref store_val(arm_lift *L, unsigned r) { return arm_reg(L, r); }

arm_lift_result arm_ldr(arm_lift *L, unsigned rt, uam_ref addr, unsigned size, bool sign,
                        int wb_rn, uam_ref wb_val)
{
    uam_ref v = load(L, size, 0, addr);
    if (size < 4)
        v = uam_unop(L->b, sign ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, v);
    if (wb_rn >= 0)
        arm_put(L, (unsigned)wb_rn, wb_val);
    if (rt == ARM_PC)
        return arm_bx(L, v, UAM_EXIT_JUMP);   /* LoadWritePC */
    arm_put(L, rt, v);
    return ARM_SEQ;
}

arm_lift_result arm_str(arm_lift *L, unsigned rt, uam_ref addr, unsigned size,
                        int wb_rn, uam_ref wb_val)
{
    store(L, size, 0, addr, store_val(L, rt));
    if (wb_rn >= 0)
        arm_put(L, (unsigned)wb_rn, wb_val);
    return ARM_SEQ;
}

arm_lift_result arm_ldrd(arm_lift *L, unsigned rt, unsigned rt2, uam_ref addr,
                         int wb_rn, uam_ref wb_val)
{
    uam_ref v1 = load(L, 4, UAM_ACC_ALIGNED, addr);
    uam_ref v2 = load(L, 4, UAM_ACC_ALIGNED, add(L, addr, imm(L, 4)));
    if (wb_rn >= 0)
        arm_put(L, (unsigned)wb_rn, wb_val);
    arm_put(L, rt, v1);
    arm_put(L, rt2, v2);
    return ARM_SEQ;
}

arm_lift_result arm_strd(arm_lift *L, unsigned rt, unsigned rt2, uam_ref addr,
                         int wb_rn, uam_ref wb_val)
{
    uam_ref v1 = store_val(L, rt), v2 = store_val(L, rt2);
    store(L, 4, UAM_ACC_ALIGNED, addr, v1);
    store(L, 4, UAM_ACC_ALIGNED, add(L, addr, imm(L, 4)), v2);
    if (wb_rn >= 0)
        arm_put(L, (unsigned)wb_rn, wb_val);
    return ARM_SEQ;
}

static unsigned popcount16(uint32_t list)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++)
        n += (list >> i) & 1;
    return n;
}

/* Lowest address of a block transfer of n words, and the written-back base. */
static void ldm_addrs(arm_lift *L, uam_ref base, unsigned n, bool inc, bool before,
                      uam_ref *start, uam_ref *wb)
{
    int32_t off;
    if (inc)
        off = before ? 4 : 0;
    else
        off = before ? -(int32_t)(4 * n) : -(int32_t)(4 * n) + 4;
    *start = add(L, base, imm(L, (uint32_t)off));
    *wb = inc ? add(L, base, imm(L, 4 * n)) : sub(L, base, imm(L, 4 * n));
}

arm_lift_result arm_ldm(arm_lift *L, unsigned rn, uint32_t list, bool inc, bool before,
                        bool wb, bool user)
{
    unsigned n = popcount16(list);
    if (n == 0)
        return arm_undef(L);   /* UNPREDICTABLE */
    bool exc_return = user && (list & (1u << ARM_PC));
    uam_ref base = arm_reg(L, rn), start, wbv;
    ldm_addrs(L, base, n, inc, before, &start, &wbv);
    uam_ref val[16] = { 0 };
    unsigned k = 0;
    for (unsigned r = 0; r < 16; r++)
        if (list & (1u << r))
            val[r] = load(L, 4, UAM_ACC_ALIGNED, add(L, start, imm(L, 4 * k++)));
    /* User-bank transfer without the pc: r8-r14 go through the system code, which
     * knows whether they are banked in the current mode. Faulting ops come first. */
    if (user && !exc_return)
        for (unsigned r = 8; r < 15; r++)
            if (list & (1u << r))
                uam_syswr(L->b, 32, ARM_SR_USRREG | r, val[r], L->guard);
    if (wb)
        arm_put(L, rn, wbv);
    for (unsigned r = 0; r < 15; r++)
        if ((list & (1u << r)) && !(user && !exc_return && r >= 8))
            arm_put(L, r, val[r]);
    if (list & (1u << ARM_PC)) {
        if (exc_return)
            return eret(L, val[ARM_PC], NONE);
        return arm_bx(L, val[ARM_PC], rn == ARM_SP && wb ? UAM_EXIT_RET : UAM_EXIT_JUMP);
    }
    if (user && !exc_return)
        L->end = true;   /* the system code wrote registers behind the block's back */
    return ARM_SEQ;
}

arm_lift_result arm_stm(arm_lift *L, unsigned rn, uint32_t list, bool inc, bool before,
                        bool wb, bool user)
{
    unsigned n = popcount16(list);
    if (n == 0)
        return arm_undef(L);
    uam_ref base = arm_reg(L, rn), start, wbv;
    ldm_addrs(L, base, n, inc, before, &start, &wbv);
    uam_ref val[16];
    for (unsigned r = 0; r < 16; r++) {
        if (!(list & (1u << r)))
            continue;
        if (user && r >= 8 && r < 15)
            val[r] = uam_sysrd(L->b, 32, ARM_SR_USRREG | r, L->guard);
        else
            val[r] = store_val(L, r);
    }
    unsigned k = 0;
    for (unsigned r = 0; r < 16; r++)
        if (list & (1u << r))
            store(L, 4, UAM_ACC_ALIGNED, add(L, start, imm(L, 4 * k++)), val[r]);
    if (wb)
        arm_put(L, rn, wbv);
    return ARM_SEQ;
}

arm_lift_result arm_ldrex(arm_lift *L, unsigned rt, unsigned rt2, uam_ref addr, unsigned size)
{
    unsigned fl = UAM_ACC_ALIGNED | UAM_ACC_X;
    if (size == 8) {   /* Rt is the word at addr: the low half in LE, the high half in BE */
        uam_ref v = load(L, 8, fl, addr);
        bool be = L->endian & UAM_ACC_BE;
        arm_put(L, rt, be ? hi32(L, v) : lo32(L, v));
        arm_put(L, rt2, be ? lo32(L, v) : hi32(L, v));
        return ARM_SEQ;
    }
    uam_ref v = load(L, size, fl, addr);
    if (size < 4)
        v = uam_unop(L->b, UAM_OP_ZEXT, 32, v);
    arm_put(L, rt, v);
    return ARM_SEQ;
}

arm_lift_result arm_strex(arm_lift *L, unsigned rd, unsigned rt, unsigned rt2, uam_ref addr,
                          unsigned size)
{
    uam_ref v = arm_reg(L, rt);
    if (size == 8) {   /* Rt goes to the word at addr */
        bool be = L->endian & UAM_ACC_BE;
        uam_ref lo = be ? arm_reg(L, rt2) : v, hi = be ? v : arm_reg(L, rt2);
        v = bin64(L, UAM_OP_OR, ext64(L, lo, false),
                  bin64(L, UAM_OP_SHL, ext64(L, hi, false), imm(L, 32)));
    }
    else if (size < 4)
        v = trunc_(L, v, size * 8);
    uam_ref ok = uam_stx(L->b, size * 8, L->endian | UAM_ACC_ALIGNED, addr, v, L->guard);
    arm_put(L, rd, bin(L, UAM_OP_XOR, z32(L, ok), imm(L, 1)));   /* 0 = success */
    return ARM_SEQ;
}

/* ---- media ------------------------------------------------------------- */

void arm_extend(arm_lift *L, unsigned rd, unsigned rn, uam_ref x, unsigned rot, bool sign,
                unsigned from)
{
    uam_ref v = rot ? bin(L, UAM_OP_ROTR, x, imm(L, rot)) : x;
    uam_ref r;
    if (from == 0) { /* XTB16: bytes 0 and 2 into halfwords */
        uam_ref b0 = ext_from(L, v, 8, sign);
        uam_ref b2 = ext_from(L, lshr(L, v, 16), 8, sign);
        if (rn != ARM_PC) {
            uam_ref a = arm_reg(L, rn);
            b0 = add(L, a, b0);
            b2 = add(L, lshr(L, a, 16), b2);
        }
        r = or_(L, and_(L, b0, 0xFFFF), shl(L, b2, 16));
    } else {
        r = ext_from(L, v, from, sign);
        if (rn != ARM_PC)
            r = add(L, arm_reg(L, rn), r);
    }
    arm_put(L, rd, r);
}

void arm_rev(arm_lift *L, unsigned rd, uam_ref x, unsigned kind)
{
    uam_ref r;
    switch (kind) {
    case 0:
        r = uam_unop(L->b, UAM_OP_BSWAP, 32, x);
        break;
    case 1:
        r = or_(L, and_(L, shl(L, x, 8), 0xFF00FF00u), and_(L, lshr(L, x, 8), 0x00FF00FFu));
        break;
    case 2: { /* REVSH */
        uam_ref h = or_(L, and_(L, shl(L, x, 8), 0xFF00u), and_(L, lshr(L, x, 8), 0xFFu));
        r = ext_from(L, h, 16, true);
        break;
    }
    default: { /* RBIT */
        static const uint32_t m[] = { 0x55555555u, 0x33333333u, 0x0F0F0F0Fu };
        r = x;
        for (unsigned s = 0; s < 3; s++) {
            unsigned n = 1u << s;
            r = or_(L, and_(L, lshr(L, r, n), m[s]), shl(L, and_(L, r, m[s]), n));
        }
        r = uam_unop(L->b, UAM_OP_BSWAP, 32, r);
        break;
    }
    }
    arm_put(L, rd, r);
}

void arm_bfx(arm_lift *L, unsigned rd, uam_ref x, unsigned lsb, unsigned width, bool sign)
{
    if (lsb + width > 32)
        return;   /* UNPREDICTABLE */
    uam_ref r;
    if (sign)
        r = ashr(L, shl(L, x, 32 - lsb - width), 32 - width);
    else
        r = and_(L, lshr(L, x, lsb), (uint32_t)uam_mask(width));
    arm_put(L, rd, r);
}

void arm_bfi(arm_lift *L, unsigned rd, uam_ref src, unsigned lsb, unsigned msb)
{
    if (msb < lsb)
        return;   /* UNPREDICTABLE */
    uint32_t mask = (uint32_t)(uam_mask(msb - lsb + 1) << lsb);
    uam_ref ins = is_imm(src) ? imm(L, ((uint32_t)imm_val(L, src) << lsb) & mask)
                              : and_(L, shl(L, src, lsb), mask);
    arm_put(L, rd, or_(L, and_(L, arm_reg(L, rd), ~mask), ins));
}

/* Saturate x to a signed (or unsigned) n-bit range; returns the value and sets *ov. */
static uam_ref saturate(arm_lift *L, uam_ref x, unsigned n, bool usat, uam_ref *ov)
{
    if (usat) {
        uint32_t max = (uint32_t)uam_mask(n);
        uam_ref neg = bin(L, UAM_OP_SLT, x, imm(L, 0));
        uam_ref big = bin(L, UAM_OP_SLT, imm(L, max), x);
        *ov = bin1(L, UAM_OP_OR, neg, big);
        return sel(L, neg, imm(L, 0), sel(L, big, imm(L, max), x));
    }
    if (n >= 32) {
        *ov = imm(L, 0);
        return x;
    }
    uint32_t max = (uint32_t)uam_mask(n - 1), min = ~max;
    uam_ref lo = bin(L, UAM_OP_SLT, x, imm(L, min));
    uam_ref hi = bin(L, UAM_OP_SLT, imm(L, max), x);
    *ov = bin1(L, UAM_OP_OR, lo, hi);
    return sel(L, lo, imm(L, min), sel(L, hi, imm(L, max), x));
}

void arm_sat(arm_lift *L, unsigned rd, uam_ref x, unsigned bits, bool usat)
{
    uam_ref ov;
    uam_ref r = saturate(L, x, bits, usat, &ov);
    set_q(L, ov);
    arm_put(L, rd, r);
}

/* Signed 32-bit a +/- b, saturated; *ov reports saturation. */
static uam_ref qadd32(arm_lift *L, uam_ref a, uam_ref b, bool subtract, uam_ref *ov)
{
    uam_ref r = subtract ? sub(L, a, b) : add(L, a, b);
    *ov = subtract ? bin(L, UAM_OP_SBORROW, a, b) : bin(L, UAM_OP_SCARRY, a, b);
    uam_ref satv = sel(L, bin(L, UAM_OP_SLT, a, imm(L, 0)), imm(L, 0x80000000u), imm(L, 0x7FFFFFFFu));
    return sel(L, *ov, satv, r);
}

void arm_qaddsub(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool subtract, bool dbl)
{
    uam_ref ov1 = imm(L, 0), ov2;
    if (dbl)
        b = qadd32(L, b, b, false, &ov1);
    uam_ref r = qadd32(L, a, b, subtract, &ov2);
    set_q(L, bin1(L, UAM_OP_OR, ov1, ov2));
    arm_put(L, rd, r);
}

/* ---- multiply and divide ---------------------------------------------- */

void arm_mul(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, int acc_reg, bool subtract, bool s)
{
    uam_ref r = bin(L, UAM_OP_MUL, a, b);
    if (acc_reg >= 0) {
        uam_ref acc = arm_reg(L, (unsigned)acc_reg);
        r = subtract ? sub(L, acc, r) : add(L, acc, r);
    }
    if (s)
        put_nz(L, r);
    arm_put(L, rd, r);
}

static uam_ref pair64(arm_lift *L, unsigned lo, unsigned hi)
{
    return bin64(L, UAM_OP_OR, ext64(L, arm_reg(L, lo), false),
                 bin64(L, UAM_OP_SHL, ext64(L, arm_reg(L, hi), false), imm(L, 32)));
}

static void put64(arm_lift *L, unsigned rdlo, unsigned rdhi, uam_ref p, bool s)
{
    if (s) {
        arm_put_flag(L, ARM_N_OFF, bin64(L, UAM_OP_SLT, p, imm(L, 0)));
        arm_put_flag(L, ARM_Z_OFF, bin64(L, UAM_OP_EQ, p, imm(L, 0)));
    }
    arm_put(L, rdlo, lo32(L, p));
    arm_put(L, rdhi, hi32(L, p));
}

void arm_mull(arm_lift *L, unsigned rdlo, unsigned rdhi, uam_ref a, uam_ref b, bool sign,
              bool acc, bool s)
{
    uam_ref p = bin64(L, UAM_OP_MUL, ext64(L, a, sign), ext64(L, b, sign));
    if (acc)
        p = bin64(L, UAM_OP_ADD, p, pair64(L, rdlo, rdhi));
    put64(L, rdlo, rdhi, p, s);
}

void arm_umaal(arm_lift *L, unsigned rdlo, unsigned rdhi, uam_ref a, uam_ref b)
{
    uam_ref p = bin64(L, UAM_OP_MUL, ext64(L, a, false), ext64(L, b, false));
    p = bin64(L, UAM_OP_ADD, p, ext64(L, arm_reg(L, rdlo), false));
    p = bin64(L, UAM_OP_ADD, p, ext64(L, arm_reg(L, rdhi), false));
    put64(L, rdlo, rdhi, p, false);
}

static uam_ref half(arm_lift *L, uam_ref x, bool hi)
{
    return hi ? ashr(L, x, 16) : ext_from(L, x, 16, true);
}

void arm_mulxy(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool nhi, bool mhi, int acc_reg)
{
    uam_ref p = bin(L, UAM_OP_MUL, half(L, a, nhi), half(L, b, mhi));
    if (acc_reg >= 0) {
        uam_ref acc = arm_reg(L, (unsigned)acc_reg);
        set_q(L, bin(L, UAM_OP_SCARRY, p, acc));
        p = add(L, p, acc);
    }
    arm_put(L, rd, p);
}

void arm_mulwy(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool mhi, int acc_reg)
{
    uam_ref p64 = bin64(L, UAM_OP_MUL, ext64(L, a, true), ext64(L, half(L, b, mhi), true));
    uam_ref p = lo32(L, bin64(L, UAM_OP_ASHR, p64, imm(L, 16)));
    if (acc_reg >= 0) {
        uam_ref acc = arm_reg(L, (unsigned)acc_reg);
        set_q(L, bin(L, UAM_OP_SCARRY, p, acc));
        p = add(L, p, acc);
    }
    arm_put(L, rd, p);
}

void arm_mmul(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, int acc_reg, bool subtract, bool round)
{
    uam_ref p = bin64(L, UAM_OP_MUL, ext64(L, a, true), ext64(L, b, true));
    if (acc_reg >= 0) {
        uam_ref acc = bin64(L, UAM_OP_SHL, ext64(L, arm_reg(L, (unsigned)acc_reg), false), imm(L, 32));
        p = subtract ? bin64(L, UAM_OP_SUB, acc, p) : bin64(L, UAM_OP_ADD, acc, p);
    }
    if (round)
        p = bin64(L, UAM_OP_ADD, p, imm(L, 0x80000000u));
    arm_put(L, rd, hi32(L, p));
}

/* SMLAD/SMUAD/SMLSD/SMUSD (and with lo/hi set, SMLALD/SMLSLD). */
void arm_dual_mul(arm_lift *L, unsigned rd, unsigned ra, unsigned rm, unsigned rn, bool swap,
                  bool subtract, bool lng)
{
    uam_ref a = arm_reg(L, rn), b = arm_reg(L, rm);
    if (swap)
        b = bin(L, UAM_OP_ROTR, b, imm(L, 16));
    uam_ref p1 = bin(L, UAM_OP_MUL, half(L, a, false), half(L, b, false));
    uam_ref p2 = bin(L, UAM_OP_MUL, half(L, a, true), half(L, b, true));
    if (lng) {
        uam_ref s = bin64(L, subtract ? UAM_OP_SUB : UAM_OP_ADD, ext64(L, p1, true), ext64(L, p2, true));
        s = bin64(L, UAM_OP_ADD, s, pair64(L, ra, rd));   /* ra = RdLo, rd = RdHi */
        put64(L, ra, rd, s, false);
        return;
    }
    uam_ref s = subtract ? sub(L, p1, p2) : add(L, p1, p2);
    uam_ref ov = subtract ? bin(L, UAM_OP_SBORROW, p1, p2) : bin(L, UAM_OP_SCARRY, p1, p2);
    if (ra != ARM_PC) {
        uam_ref acc = arm_reg(L, ra);
        ov = bin1(L, UAM_OP_OR, ov, bin(L, UAM_OP_SCARRY, s, acc));
        s = add(L, s, acc);
    }
    set_q(L, ov);
    arm_put(L, rd, s);
}

void arm_usad8(arm_lift *L, unsigned rd, uam_ref x, uam_ref y, unsigned ra)
{
    uam_ref s = imm(L, 0);
    for (unsigned k = 0; k < 4; k++) {
        uam_ref xb = and_(L, lshr(L, x, 8 * k), 0xFF), yb = and_(L, lshr(L, y, 8 * k), 0xFF);
        uam_ref d = sub(L, xb, yb);
        s = add(L, s, sel(L, bin(L, UAM_OP_SLT, d, imm(L, 0)), sub(L, imm(L, 0), d), d));
    }
    if (ra != ARM_PC)
        s = add(L, s, arm_reg(L, ra));
    arm_put(L, rd, s);
}

void arm_div(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool sign)
{
    /* Division by zero gives 0 (no trap: the A profile has no SCTLR.DZ). */
    uam_ref q = bin(L, sign ? UAM_OP_SDIV : UAM_OP_UDIV, a, b);
    arm_put(L, rd, sel(L, bin(L, UAM_OP_EQ, b, imm(L, 0)), imm(L, 0), q));
}

void arm_sel(arm_lift *L, unsigned rd, uam_ref a, uam_ref b)
{
    uam_ref ge = uam_unop(L->b, UAM_OP_ZEXT, 32, uam_get(L->b, 8, ARM_GE_OFF));
    uam_ref m = imm(L, 0);
    for (unsigned k = 0; k < 4; k++)
        m = or_(L, m, bin(L, UAM_OP_MUL, and_(L, lshr(L, ge, k), 1), imm(L, 0xFFu << (8 * k))));
    arm_put(L, rd, or_(L, bin(L, UAM_OP_AND, a, m), bin(L, UAM_OP_AND, b, not_(L, m))));
}

void arm_pkh(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool tb, unsigned sh)
{
    uam_ref r;
    if (tb) /* PKHTB: top of a, bottom of b ASR sh (0 means 32) */
        r = or_(L, and_(L, a, 0xFFFF0000u), and_(L, ashr(L, b, sh ? sh : 31), 0xFFFF));
    else
        r = or_(L, and_(L, a, 0xFFFF), and_(L, shl(L, b, sh), 0xFFFF0000u));
    arm_put(L, rd, r);
}

bool arm_parallel(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, unsigned op1, unsigned op2)
{
    bool sign = op1 < 4;
    unsigned kind = op1 & 3;          /* 1 plain (GE), 2 saturating, 3 halving */
    if (kind == 0 || op1 == 0 || op1 == 4 || op2 == 5 || op2 == 6)
        return false;
    bool b8 = op2 >= 4;
    unsigned lw = b8 ? 8 : 16, nl = b8 ? 4 : 2;
    uam_ref res = imm(L, 0), ge = imm(L, 0);
    for (unsigned k = 0; k < nl; k++) {
        bool subtract;
        unsigned kb = k;              /* lane of b */
        switch (op2) {
        case 0: case 4: subtract = false; break;      /* ADD16 / ADD8 */
        case 3: case 7: subtract = true; break;       /* SUB16 / SUB8 */
        case 1: subtract = k == 0; kb = 1 - k; break; /* ASX: lo - b.hi, hi + b.lo */
        default: subtract = k == 1; kb = 1 - k; break;/* SAX: lo + b.hi, hi - b.lo */
        }
        uam_ref x = ext_from(L, lshr(L, a, lw * k), lw, sign);
        uam_ref y = ext_from(L, lshr(L, b, lw * kb), lw, sign);
        uam_ref s = subtract ? sub(L, x, y) : add(L, x, y);
        uam_ref lane;
        if (kind == 2) {
            uam_ref ov;
            lane = saturate(L, s, sign ? lw : lw, !sign, &ov);
        } else if (kind == 3) {
            lane = ashr(L, s, 1);
        } else {
            lane = s;
            uam_ref g = sign || subtract ? bin(L, UAM_OP_SLE, imm(L, 0), s)
                                         : bin(L, UAM_OP_ULE, imm(L, 1u << lw), s);
            uint32_t gm = b8 ? 1u << k : 3u << (2 * k);
            ge = or_(L, ge, bin(L, UAM_OP_MUL, z32(L, g), imm(L, gm)));
        }
        res = or_(L, res, shl(L, and_(L, lane, (uint32_t)uam_mask(lw)), lw * k));
    }
    if (kind == 1)
        uam_put(L->b, 8, ARM_GE_OFF, trunc_(L, ge, 8), L->guard);
    arm_put(L, rd, res);
    return true;
}

/* ---- status registers, coprocessors, hints ------------------------------ */

void arm_mrs(arm_lift *L, unsigned rd, bool spsr)
{
    uam_ref v;
    if (spsr) {
        v = uam_sysrd(L->b, 32, ARM_SR_SPSR, L->guard);
    } else {
        v = uam_sysrd(L->b, 32, ARM_SR_CPSR, L->guard);
        static const uint32_t off[] = { ARM_N_OFF, ARM_Z_OFF, ARM_C_OFF, ARM_V_OFF, ARM_Q_OFF };
        for (unsigned k = 0; k < 5; k++)
            v = or_(L, v, shl(L, z32(L, arm_flag(L, off[k])), 31 - k));
        v = or_(L, v, shl(L, uam_unop(L->b, UAM_OP_ZEXT, 32, uam_get(L->b, 8, ARM_GE_OFF)),
                          CPSR_GE_SHIFT));
    }
    arm_put(L, rd, v);
}

arm_lift_result arm_msr(arm_lift *L, uam_ref v, unsigned mask, bool spsr)
{
    if (spsr) {
        uam_syswr(L->b, 32, ARM_SR_SPSR | mask, v, L->guard);
        return ARM_SEQ;
    }
    bool ctrl = mask & 3;              /* c or x field: mode, interrupt masks, E */
    if (ctrl)
        uam_syswr(L->b, 32, ARM_SR_CPSR | mask, v, L->guard);
    if (mask & 8) {                    /* f: NZCVQ */
        static const uint32_t off[] = { ARM_N_OFF, ARM_Z_OFF, ARM_C_OFF, ARM_V_OFF, ARM_Q_OFF };
        for (unsigned k = 0; k < 5; k++)
            arm_put_flag(L, off[k], bit_of(L, v, 31 - k));
    }
    if (mask & 4)                      /* s: GE[3:0] */
        uam_put(L->b, 8, ARM_GE_OFF, trunc_(L, and_(L, lshr(L, v, CPSR_GE_SHIFT), 0xF), 8), L->guard);
    if (ctrl)
        L->end = true;
    return ARM_SEQ;
}

arm_lift_result arm_cps(arm_lift *L, unsigned imod, bool m, unsigned aif, unsigned mode)
{
    uint32_t v = (imod << 6) | ((m ? 1u : 0u) << 5) | (aif & 7) | (mode << 8);
    uam_syswr(L->b, 32, ARM_SR_CPS, imm(L, v), L->guard);
    L->end = true;
    return ARM_SEQ;
}

arm_lift_result arm_coproc(arm_lift *L, bool load, unsigned cp, unsigned opc1, unsigned crn,
                           unsigned crm, unsigned opc2, unsigned rt)
{
    if (cp != 14 && cp != 15)
        return arm_undef(L);   /* no VFP/NEON (cp10/cp11) or other coprocessors */
    uint32_t id = ARM_SR_CP(cp, opc1, crn, crm, opc2);
    if (load) {
        uam_ref v = uam_sysrd(L->b, 32, id, L->guard);
        if (rt == ARM_PC) {    /* APSR_nzcv */
            static const uint32_t off[] = { ARM_N_OFF, ARM_Z_OFF, ARM_C_OFF, ARM_V_OFF };
            for (unsigned k = 0; k < 4; k++)
                arm_put_flag(L, off[k], bit_of(L, v, 31 - k));
        } else {
            arm_put(L, rt, v);
        }
        return ARM_SEQ;
    }
    uam_syswr(L->b, 32, id, arm_reg(L, rt), L->guard);
    L->end = true;             /* may change translation, interrupts or the timer */
    return ARM_SEQ;
}

arm_lift_result arm_coproc64(arm_lift *L, bool load, unsigned cp, unsigned opc1, unsigned crm,
                             unsigned rt, unsigned rt2)
{
    if (cp != 14 && cp != 15)
        return arm_undef(L);
    uint32_t id = ARM_SR_CP64(cp, opc1, crm);
    if (load) {
        uam_ref v = uam_sysrd(L->b, 64, id, L->guard);
        arm_put(L, rt, lo32(L, v));
        arm_put(L, rt2, hi32(L, v));
        return ARM_SEQ;
    }
    uam_syswr(L->b, 64, id, pair64(L, rt, rt2), L->guard);
    L->end = true;
    return ARM_SEQ;
}

arm_lift_result arm_hint(arm_lift *L, unsigned op)
{
    if (op != 3)
        return ARM_SEQ;   /* NOP, YIELD, SEV, DBG; WFE: the event register is always set */
    /* WFI: WAIT is an unguarded terminator, so a condition skips it with a side exit. */
    if (!uam_ref_is_none(L->guard)) {
        uam_exit(L->b, not1(L, L->guard), imm(L, arm_next_pc(L)), UAM_EXIT_JUMP);
        L->guard = NONE;
    }
    uam_wait(L->b, imm(L, arm_next_pc(L)));
    return ARM_DONE;
}

/* SETEND: the data byte order changes, and with it the code lifted after it. */
arm_lift_result arm_setend(arm_lift *L, bool be)
{
    uam_syswr(L->b, 32, ARM_SR_SETEND, imm(L, be ? 1 : 0), L->guard);
    L->end = true;
    return ARM_SEQ;
}

arm_lift_result arm_barrier(arm_lift *L, unsigned op)
{
    switch (op) {
    case 1: /* CLREX */
        uam_helper(L->b, &L->lc->cpu->isa->helpers, ARM_HELPER_CLREX, NONE, NONE, NONE, L->guard);
        return ARM_SEQ;
    case 4: case 5:
        uam_fence(L->b, UAM_FENCE_FULL);
        return ARM_SEQ;
    case 6: /* ISB: resynchronise the instruction stream */
        if (L->it_next != ARM_PRIV_IT(L->lc->cpu->priv))
            uam_helper(L->b, &L->lc->cpu->isa->helpers, ARM_HELPER_SET_IT, imm(L, L->it_next),
                       NONE, NONE, NONE);
        uam_fence(L->b, UAM_FENCE_INSN);
        uam_exit(L->b, NONE, imm(L, arm_next_pc(L)), UAM_EXIT_JUMP);
        L->guard = NONE;
        return ARM_DONE;
    default:
        return arm_undef(L);
    }
}

/* SRS: store LR and SPSR to the stack of `mode`. */
arm_lift_result arm_srs(arm_lift *L, unsigned mode, bool inc, bool before, bool wb)
{
    uint32_t sp_id = ARM_SR_BANKSP | mode;
    uam_ref sp = uam_sysrd(L->b, 32, sp_id, L->guard);
    uam_ref lr = arm_reg(L, ARM_LR);
    uam_ref spsr = uam_sysrd(L->b, 32, ARM_SR_SPSR, L->guard);
    uam_ref start, wbv;
    ldm_addrs(L, sp, 2, inc, before, &start, &wbv);
    store(L, 4, UAM_ACC_ALIGNED, start, lr);
    store(L, 4, UAM_ACC_ALIGNED, add(L, start, imm(L, 4)), spsr);
    if (wb) {
        uam_syswr(L->b, 32, sp_id, wbv, L->guard);
        L->end = true;
    }
    return ARM_SEQ;
}

/* RFE: load the pc and CPSR from memory. */
arm_lift_result arm_rfe(arm_lift *L, unsigned rn, bool inc, bool before, bool wb)
{
    uam_ref start, wbv;
    ldm_addrs(L, arm_reg(L, rn), 2, inc, before, &start, &wbv);
    uam_ref pc = load(L, 4, UAM_ACC_ALIGNED, start);
    uam_ref psr = load(L, 4, UAM_ACC_ALIGNED, add(L, start, imm(L, 4)));
    if (wb)
        arm_put(L, rn, wbv);
    return eret(L, pc, psr);
}

/* ---- A32 decoder -------------------------------------------------------- */

#define BITS(i, hi, lo) (((i) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(i, n)       (((i) >> (n)) & 1u)

static uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* Shifter operand of a data-processing or load/store register form. */
static arm_shres a32_reg_operand(arm_lift *L, uint32_t i)
{
    uam_ref rm = arm_reg(L, BITS(i, 3, 0));
    unsigned type = BITS(i, 6, 5);
    if (BIT(i, 4))
        return arm_shift_reg(L, rm, type, arm_reg(L, BITS(i, 11, 8)));
    return arm_shift_imm(L, rm, type, BITS(i, 11, 7));
}

static arm_lift_result a32_dp(arm_lift *L, uint32_t i)
{
    unsigned op = BITS(i, 24, 21), rn = BITS(i, 19, 16), rd = BITS(i, 15, 12);
    bool s = BIT(i, 20);
    arm_shres b;
    if (BIT(i, 25)) {
        unsigned rot = BITS(i, 11, 8) * 2;
        uint32_t v = ror32(BITS(i, 7, 0), rot);
        b.v = imm(L, v);
        b.c = rot ? imm(L, v >> 31) : NONE;
    } else {
        b = a32_reg_operand(L, i);
    }
    uam_ref a = (op == DP_MOV || op == DP_MVN) ? imm(L, 0) : arm_reg(L, rn);
    return arm_dp(L, op, s, rd, a, b);
}

/* Multiply group (bits 7:4 = 1001, bit 24 = 0). */
static arm_lift_result a32_mul(arm_lift *L, uint32_t i)
{
    unsigned rd = BITS(i, 19, 16), ra = BITS(i, 15, 12), rm = BITS(i, 11, 8), rn = BITS(i, 3, 0);
    bool s = BIT(i, 20);
    uam_ref a = arm_reg(L, rn), b = arm_reg(L, rm);
    switch (BITS(i, 23, 21)) {
    case 0: arm_mul(L, rd, a, b, -1, false, s); return ARM_SEQ;          /* MUL */
    case 1: arm_mul(L, rd, a, b, (int)ra, false, s); return ARM_SEQ;     /* MLA */
    case 2:
        if (s) break;
        arm_umaal(L, ra, rd, a, b); return ARM_SEQ;                      /* UMAAL */
    case 3:
        if (s) break;
        arm_mul(L, rd, a, b, (int)ra, true, false); return ARM_SEQ;      /* MLS */
    case 4: arm_mull(L, ra, rd, a, b, false, false, s); return ARM_SEQ;  /* UMULL */
    case 5: arm_mull(L, ra, rd, a, b, false, true, s); return ARM_SEQ;   /* UMLAL */
    case 6: arm_mull(L, ra, rd, a, b, true, false, s); return ARM_SEQ;   /* SMULL */
    default: arm_mull(L, ra, rd, a, b, true, true, s); return ARM_SEQ;   /* SMLAL */
    }
    return arm_undef(L);
}

/* Synchronization primitives (bits 7:4 = 1001, bit 24 = 1). */
static arm_lift_result a32_sync(arm_lift *L, uint32_t i)
{
    unsigned rn = BITS(i, 19, 16), rd = BITS(i, 15, 12), rt = BITS(i, 3, 0);
    uam_ref addr = arm_reg(L, rn);
    if (!BIT(i, 23)) {       /* SWP / SWPB */
        if (BITS(i, 21, 20) != 0)
            return arm_undef(L);
        unsigned sz = BIT(i, 22) ? 8 : 32;
        uam_ref v = arm_reg(L, rt);
        if (sz == 8)
            v = trunc_(L, v, 8);
        uam_ref old = uam_amo(L->b, sz, L->endian | (sz == 32 ? UAM_ACC_ALIGNED : 0),
                              UAM_AMO_XCHG, addr, v, L->guard);
        if (sz == 8)
            old = uam_unop(L->b, UAM_OP_ZEXT, 32, old);
        arm_put(L, rd, old);
        return ARM_SEQ;
    }
    static const unsigned sizes[] = { 4, 8, 1, 2 };
    unsigned size = sizes[BITS(i, 22, 21)];
    if (BIT(i, 20))
        return arm_ldrex(L, rd, rd + 1, addr, size);
    return arm_strex(L, rd, rt, rt + 1, addr, size);
}

/* Index/writeback arithmetic shared by the load/store forms. */
static void a32_index(arm_lift *L, uint32_t i, uam_ref offset, uam_ref *addr, int *wb_rn, uam_ref *wb)
{
    unsigned rn = BITS(i, 19, 16);
    bool p = BIT(i, 24), u = BIT(i, 23), w = BIT(i, 21);
    uam_ref base = arm_reg_align(L, rn);
    uam_ref off_addr = u ? add(L, base, offset) : sub(L, base, offset);
    *addr = p ? off_addr : base;
    *wb_rn = (!p || w) ? (int)rn : -1;
    *wb = off_addr;
}

/* LDRH/STRH/LDRSB/LDRSH/LDRD/STRD (and their unprivileged forms). */
static arm_lift_result a32_extra_ls(arm_lift *L, uint32_t i)
{
    unsigned rt = BITS(i, 15, 12), op2 = BITS(i, 6, 5);
    bool l = BIT(i, 20);
    uam_ref off = BIT(i, 22) ? imm(L, (BITS(i, 11, 8) << 4) | BITS(i, 3, 0)) : arm_reg(L, BITS(i, 3, 0));
    uam_ref addr, wb;
    int wb_rn;
    a32_index(L, i, off, &addr, &wb_rn, &wb);
    switch (op2) {
    case 1:
        return l ? arm_ldr(L, rt, addr, 2, false, wb_rn, wb) : arm_str(L, rt, addr, 2, wb_rn, wb);
    case 2:
        if (l)
            return arm_ldr(L, rt, addr, 1, true, wb_rn, wb);
        if (rt & 1)
            return arm_undef(L);
        return arm_ldrd(L, rt, rt + 1, addr, wb_rn, wb);
    default:
        if (l)
            return arm_ldr(L, rt, addr, 2, true, wb_rn, wb);
        if (rt & 1)
            return arm_undef(L);
        return arm_strd(L, rt, rt + 1, addr, wb_rn, wb);
    }
}

/* Miscellaneous instructions (op1 = 10xx0, bits 7:4 = 0xxx). */
static arm_lift_result a32_misc(arm_lift *L, uint32_t i)
{
    unsigned op = BITS(i, 22, 21), op2 = BITS(i, 6, 4);
    unsigned rd = BITS(i, 15, 12), rm = BITS(i, 3, 0);
    switch (op2) {
    case 0:
        if (BIT(i, 9))
            return arm_undef(L);              /* banked-register MRS/MSR */
        if (!(op & 1)) {
            arm_mrs(L, rd, BIT(i, 22));
            return ARM_SEQ;
        }
        return arm_msr(L, arm_reg(L, rm), BITS(i, 19, 16), BIT(i, 22));
    case 1:
        if (op == 1)
            return arm_bx(L, arm_reg(L, rm), rm == ARM_LR ? UAM_EXIT_RET : UAM_EXIT_JUMP);
        if (op == 3) {
            arm_put(L, rd, uam_unop(L->b, UAM_OP_CLZ, 32, arm_reg(L, rm)));
            return ARM_SEQ;
        }
        break;
    case 2:
        if (op == 1)                          /* BXJ: no Jazelle, acts as BX */
            return arm_bx(L, arm_reg(L, rm), UAM_EXIT_JUMP);
        break;
    case 3:
        if (op == 1) {                        /* BLX register */
            uam_ref t = arm_bx_target(L, arm_reg(L, rm));
            arm_put(L, ARM_LR, imm(L, arm_next_pc(L)));
            return arm_branch(L, t, UAM_EXIT_CALL);
        }
        break;
    case 5:                                   /* QADD QSUB QDADD QDSUB */
        arm_qaddsub(L, rd, arm_reg(L, rm), arm_reg(L, BITS(i, 19, 16)), op & 1, op & 2);
        return ARM_SEQ;
    case 7:
        if (op == 1) {                        /* BKPT: a debug event */
            arm_trap(L, ARM_EXC_PABT, ARM_FS_DEBUG);
            return ARM_DONE;
        }
        break;
    default:
        break;
    }
    return arm_undef(L);
}

/* Halfword multiplies (op1 = 10xx0, bits 7:4 = 1yx0). */
static arm_lift_result a32_halfmul(arm_lift *L, uint32_t i)
{
    unsigned rd = BITS(i, 19, 16), ra = BITS(i, 15, 12), rm = BITS(i, 11, 8), rn = BITS(i, 3, 0);
    bool x = BIT(i, 5), y = BIT(i, 6);
    uam_ref a = arm_reg(L, rn), b = arm_reg(L, rm);
    switch (BITS(i, 22, 21)) {
    case 0: arm_mulxy(L, rd, a, b, x, y, (int)ra); return ARM_SEQ;               /* SMLAxy */
    case 1: arm_mulwy(L, rd, a, b, y, x ? -1 : (int)ra); return ARM_SEQ;         /* SMULWy / SMLAWy */
    case 2: {                                                                    /* SMLALxy */
        uam_ref p = bin(L, UAM_OP_MUL, half(L, a, x), half(L, b, y));
        put64(L, ra, rd, bin64(L, UAM_OP_ADD, ext64(L, p, true), pair64(L, ra, rd)), false);
        return ARM_SEQ;
    }
    default: arm_mulxy(L, rd, a, b, x, y, -1); return ARM_SEQ;                   /* SMULxy */
    }
}

static arm_lift_result a32_load_store(arm_lift *L, uint32_t i)
{
    unsigned rt = BITS(i, 15, 12);
    bool l = BIT(i, 20), byte = BIT(i, 22);
    uam_ref off;
    if (BIT(i, 25)) {
        off = a32_reg_operand(L, i).v;   /* bit 4 is 0: shift by immediate */
    } else {
        off = imm(L, BITS(i, 11, 0));
    }
    uam_ref addr, wb;
    int wb_rn;
    a32_index(L, i, off, &addr, &wb_rn, &wb);
    unsigned size = byte ? 1 : 4;
    if (l)
        return arm_ldr(L, rt, addr, size, false, wb_rn, wb);
    return arm_str(L, rt, addr, size, wb_rn, wb);
}

static arm_lift_result a32_media(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 24, 20), op2 = BITS(i, 7, 5);
    unsigned rn = BITS(i, 19, 16), rd = BITS(i, 15, 12), rm = BITS(i, 3, 0);
    unsigned ra = BITS(i, 15, 12);

    if (op1 < 8) {                                   /* parallel add/subtract */
        if (!arm_parallel(L, rd, arm_reg(L, rn), arm_reg(L, rm), op1, op2))
            return arm_undef(L);
        return ARM_SEQ;
    }
    if (op1 < 16) {                                  /* pack, unpack, saturate, reverse */
        unsigned rot = BITS(i, 11, 10) * 8;
        if (op1 == 8 && !(op2 & 1)) {
            arm_pkh(L, rd, arm_reg(L, rn), arm_reg(L, rm), BIT(i, 6), BITS(i, 11, 7));
            return ARM_SEQ;
        }
        if ((op1 & 0xE) == 0xA && !(op2 & 1)) {      /* SSAT */
            arm_shres sh = arm_shift_imm(L, arm_reg(L, rm), BIT(i, 6) ? 2 : 0, BITS(i, 11, 7));
            arm_sat(L, rd, sh.v, BITS(i, 20, 16) + 1, false);
            return ARM_SEQ;
        }
        if ((op1 & 0xE) == 0xE && !(op2 & 1)) {      /* USAT */
            arm_shres sh = arm_shift_imm(L, arm_reg(L, rm), BIT(i, 6) ? 2 : 0, BITS(i, 11, 7));
            arm_sat(L, rd, sh.v, BITS(i, 20, 16), true);
            return ARM_SEQ;
        }
        if (op2 == 3) {                              /* extends */
            bool sign = op1 < 12;
            unsigned from;
            switch (op1 & 3) {
            case 0: from = 0; break;                 /* XTB16 */
            case 2: from = 8; break;
            case 3: from = 16; break;
            default: return arm_undef(L);
            }
            arm_extend(L, rd, rn, arm_reg(L, rm), rot, sign, from);
            return ARM_SEQ;
        }
        if (op1 == 8 && op2 == 5) {
            arm_sel(L, rd, arm_reg(L, rn), arm_reg(L, rm));
            return ARM_SEQ;
        }
        if (op1 == 11 && op2 == 1) { arm_rev(L, rd, arm_reg(L, rm), 0); return ARM_SEQ; }
        if (op1 == 11 && op2 == 5) { arm_rev(L, rd, arm_reg(L, rm), 1); return ARM_SEQ; }
        if (op1 == 15 && op2 == 1) { arm_rev(L, rd, arm_reg(L, rm), 3); return ARM_SEQ; }
        if (op1 == 15 && op2 == 5) { arm_rev(L, rd, arm_reg(L, rm), 2); return ARM_SEQ; }
        return arm_undef(L);                         /* SSAT16, USAT16 */
    }
    if (op1 < 24) {                                  /* signed multiplies, divide */
        unsigned d = BITS(i, 19, 16), a = BITS(i, 15, 12), m = BITS(i, 11, 8), n = BITS(i, 3, 0);
        bool swap = BIT(i, 5);
        switch (op1) {
        case 16:
            if (op2 >= 4)
                break;
            arm_dual_mul(L, d, a, m, n, swap, op2 >= 2, false);
            return ARM_SEQ;
        case 17:
            if (op2 != 0 || a != 15)
                break;
            arm_div(L, d, arm_reg(L, n), arm_reg(L, m), true);
            return ARM_SEQ;
        case 19:
            if (op2 != 0 || a != 15)
                break;
            arm_div(L, d, arm_reg(L, n), arm_reg(L, m), false);
            return ARM_SEQ;
        case 20:
            if (op2 >= 4)
                break;
            arm_dual_mul(L, d, a, m, n, swap, op2 >= 2, true);
            return ARM_SEQ;
        case 21:
            if (op2 < 2)
                arm_mmul(L, d, arm_reg(L, n), arm_reg(L, m), a == 15 ? -1 : (int)a, false, swap);
            else if (op2 >= 6)
                arm_mmul(L, d, arm_reg(L, n), arm_reg(L, m), (int)a, true, swap);
            else
                break;
            return ARM_SEQ;
        default:
            break;
        }
        return arm_undef(L);
    }
    if (op1 == 24 && op2 == 0) {                     /* USAD8 / USADA8 */
        arm_usad8(L, BITS(i, 19, 16), arm_reg(L, BITS(i, 3, 0)), arm_reg(L, BITS(i, 11, 8)), ra);
        return ARM_SEQ;
    }
    unsigned lsb = BITS(i, 11, 7), w = BITS(i, 20, 16);
    if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) {   /* SBFX */
        arm_bfx(L, rd, arm_reg(L, rm), lsb, w + 1, true);
        return ARM_SEQ;
    }
    if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) {   /* UBFX */
        arm_bfx(L, rd, arm_reg(L, rm), lsb, w + 1, false);
        return ARM_SEQ;
    }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {   /* BFC / BFI */
        arm_bfi(L, rd, rm == 15 ? imm(L, 0) : arm_reg(L, rm), lsb, w);
        return ARM_SEQ;
    }
    return arm_undef(L);                             /* UDF and the rest */
}

static arm_lift_result a32_coproc(arm_lift *L, uint32_t i)
{
    unsigned cp = BITS(i, 11, 8);
    if (BITS(i, 27, 24) == 0xF) {                    /* SVC */
        arm_trap(L, ARM_EXC_SVC, BITS(i, 23, 0));
        return ARM_DONE;
    }
    if (BITS(i, 27, 21) == 0x62)                     /* MCRR / MRRC */
        return arm_coproc64(L, BIT(i, 20), cp, BITS(i, 7, 4), BITS(i, 3, 0), BITS(i, 15, 12),
                            BITS(i, 19, 16));
    if (BITS(i, 27, 24) == 0xE && BIT(i, 4))         /* MCR / MRC */
        return arm_coproc(L, BIT(i, 20), cp, BITS(i, 23, 21), BITS(i, 19, 16), BITS(i, 3, 0),
                          BITS(i, 7, 5), BITS(i, 15, 12));
    return arm_undef(L);                             /* LDC, STC, CDP */
}

/* Instructions with cond = 1111. */
static arm_lift_result a32_uncond(arm_lift *L, uint32_t i)
{
    if ((i & 0x0FF10020u) == 0x01000000u)           /* CPS */
        return arm_cps(L, BITS(i, 19, 18), BIT(i, 17), BITS(i, 8, 6), BITS(i, 4, 0));
    if ((i & 0x0FFFFDFFu) == 0x01010000u)           /* SETEND */
        return arm_setend(L, BIT(i, 9));
    if ((i & 0x0FFFFF00u) == 0x057FF000u) {          /* CLREX DSB DMB ISB */
        switch (BITS(i, 7, 4)) {
        case 1: return arm_barrier(L, 1);
        case 4: return arm_barrier(L, 4);
        case 5: return arm_barrier(L, 5);
        case 6: return arm_barrier(L, 6);
        default: return arm_undef(L);
        }
    }
    if ((i & 0x0F300000u) == 0x05100000u || (i & 0x0F700000u) == 0x04500000u)
        return ARM_SEQ;                              /* PLD, PLDW, PLI (immediate) */
    if ((i & 0x0F300010u) == 0x07100000u || (i & 0x0F700010u) == 0x06500000u)
        return ARM_SEQ;                              /* PLD, PLDW, PLI (register) */
    if ((i & 0x0E5FFFE0u) == 0x084D0500u)           /* SRS */
        return arm_srs(L, BITS(i, 4, 0), BIT(i, 23), BIT(i, 24), BIT(i, 21));
    if ((i & 0x0E50FFFFu) == 0x08100A00u)           /* RFE */
        return arm_rfe(L, BITS(i, 19, 16), BIT(i, 23), BIT(i, 24), BIT(i, 21));
    if (BITS(i, 27, 25) == 5) {                      /* BLX immediate */
        uint32_t off = ((uint32_t)uam_sext(BITS(i, 23, 0), 24) << 2) | (BIT(i, 24) << 1);
        arm_put(L, ARM_LR, imm(L, L->pc + 4));
        return arm_branch(L, imm(L, (L->pc + 8 + off) | 1), UAM_EXIT_CALL);
    }
    return arm_undef(L);
}

arm_lift_result arm_lift_a32(arm_lift *L, uint32_t i)
{
    if (BITS(i, 31, 28) == 0xF)
        return a32_uncond(L, i);

    switch (BITS(i, 27, 25)) {
    case 0: {
        unsigned op1 = BITS(i, 24, 20), op2 = BITS(i, 7, 4);
        if ((op2 & 9) == 9) {
            if (op2 == 9)
                return BIT(i, 24) ? a32_sync(L, i) : a32_mul(L, i);
            if ((op1 & 0x12) == 0x02 && BIT(i, 24))
                return arm_undef(L);
            return a32_extra_ls(L, i);
        }
        if ((op1 & 0x19) == 0x10)
            return (op2 & 8) ? a32_halfmul(L, i) : a32_misc(L, i);
        return a32_dp(L, i);
    }
    case 1: {
        unsigned op1 = BITS(i, 24, 20);
        if (op1 == 0x10 || op1 == 0x14) {            /* MOVW / MOVT */
            uint32_t v = (BITS(i, 19, 16) << 12) | BITS(i, 11, 0);
            unsigned rd = BITS(i, 15, 12);
            if (op1 == 0x10)
                arm_put(L, rd, imm(L, v));
            else
                arm_put(L, rd, or_(L, and_(L, arm_reg(L, rd), 0xFFFF), imm(L, v << 16)));
            return ARM_SEQ;
        }
        if (op1 == 0x12 || op1 == 0x16) {            /* MSR immediate, hints */
            unsigned mask = BITS(i, 19, 16);
            if (op1 == 0x12 && mask == 0)
                return arm_hint(L, BITS(i, 7, 0) >= 0xF0 ? 0 : BITS(i, 7, 0));
            uint32_t v = ror32(BITS(i, 7, 0), BITS(i, 11, 8) * 2);
            return arm_msr(L, imm(L, v), mask, op1 == 0x16);
        }
        return a32_dp(L, i);
    }
    case 2:
        return a32_load_store(L, i);
    case 3:
        if (BIT(i, 4))
            return a32_media(L, i);
        return a32_load_store(L, i);
    case 4: {
        unsigned rn = BITS(i, 19, 16);
        bool inc = BIT(i, 23), before = BIT(i, 24);
        if (BIT(i, 20))
            return arm_ldm(L, rn, BITS(i, 15, 0), inc, before, BIT(i, 21), BIT(i, 22));
        return arm_stm(L, rn, BITS(i, 15, 0), inc, before, BIT(i, 21), BIT(i, 22));
    }
    case 5: {
        uint32_t target = L->pc + 8 + ((uint32_t)uam_sext(BITS(i, 23, 0), 24) << 2);
        if (BIT(i, 24))
            arm_put(L, ARM_LR, imm(L, L->pc + 4));
        return arm_branch(L, imm(L, target), BIT(i, 24) ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
    }
    default:
        return a32_coproc(L, i);
    }
}

/* ---- the block loop ----------------------------------------------------- */

/* IT state after one instruction of an IT block. */
static uint8_t it_advance(uint8_t it)
{
    if ((it & 7) == 0)
        return 0;
    return (uint8_t)((it & 0xE0) | ((it << 1) & 0x1F));
}

/* Number of instructions an IT state still covers. */
static unsigned it_count(uint8_t it)
{
    unsigned n = 0;
    while (it) {
        n++;
        it = it_advance(it);
    }
    return n;
}

/* Continue at pc in the next block, with the IT state it must start with. */
static void fall_through(arm_lift *L, uint32_t pc, uint8_t it)
{
    if (it != ARM_PRIV_IT(L->lc->cpu->priv))
        uam_helper(L->b, &L->lc->cpu->isa->helpers, ARM_HELPER_SET_IT, imm(L, it), NONE, NONE, NONE);
    uam_exit(L->b, NONE, imm(L, pc), UAM_EXIT_JUMP);
}

uam_status arm_lift_block(uam_lift_ctx *lc)
{
    arm_lift L = { .lc = lc, .b = lc->b, .thumb = lc->pc & 1,
                   .user = lc->cpu->priv & ARM_PRIV_USER,
                   .endian = (lc->cpu->priv & ARM_PRIV_BE) ? UAM_ACC_BE : UAM_ACC_LE };
    uint32_t tbit = L.thumb ? 1u : 0u;
    uint32_t pc = (uint32_t)lc->pc & ~1u;
    uint8_t it = (uint8_t)ARM_PRIV_IT(lc->cpu->priv);
    /* A block that starts inside an IT block (after an exception return into one)
     * runs that one instruction; each of the rest gets a block of its own, so the
     * IT state in cpu->priv is exact at every block boundary. */
    bool resumed = L.thumb && it != 0;
    if (!L.thumb)
        it = 0;

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns || (resumed && n > 0)) {
            fall_through(&L, pc | tbit, it);
            return UAM_OK;
        }

        uint64_t raw = 0;
        unsigned len = 4;
        uam_status st;
        if (L.thumb) {
            st = lc->fetch(lc, pc, 2, &raw);
            if (st == UAM_OK && arm_t32_prefix((uint32_t)raw)) {
                uint64_t lo = 0;
                st = lc->fetch(lc, pc + 2, 2, &lo);
                raw = (raw << 16) | lo;
            } else {
                len = 2;
            }
        } else {
            st = lc->fetch(lc, pc, 4, &raw);
        }
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            fall_through(&L, pc | tbit, it);
            return UAM_OK;
        }
        uint32_t i = (uint32_t)raw;

        /* An IT instruction brings its whole block into this one if it fits. */
        if (L.thumb && len == 2 && it == 0 && (i & 0xFF00u) == 0xBF00u && (i & 0xF) && n > 0
            && n + 1 + it_count((uint8_t)(i & 0xFF)) > lc->max_insns) {
            fall_through(&L, pc | tbit, 0);
            return UAM_OK;
        }

        if (it && n > 0)
            uam_imark_cont(lc->b, pc | tbit, len);
        else
            uam_imark(lc->b, pc | tbit, len);
        L.pc = pc;
        L.len = len;
        L.raw = i;
        L.end = false;
        L.it = it;
        L.it_next = it_advance(it);

        arm_lift_result r;
        if (L.thumb) {
            L.guard = it ? arm_cond(&L, it >> 4) : NONE;
            r = len == 2 ? arm_lift_t16(&L, i) : arm_lift_t32(&L, i);
        } else {
            unsigned cond = i >> 28;
            L.guard = cond < 0xE ? arm_cond(&L, cond) : NONE;
            r = arm_lift_a32(&L, i);
        }

        if (r == ARM_DONE) {
            if (!uam_ref_is_none(L.guard))
                fall_through(&L, arm_next_pc(&L), L.it_next);
            return UAM_OK;
        }
        if (L.end) {
            fall_through(&L, arm_next_pc(&L), L.it_next);
            return UAM_OK;
        }
        it = L.it_next;
        pc += len;
    }
}
