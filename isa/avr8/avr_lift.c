/*
 * avr_lift.c - 8-bit AVR decoder and lifter (every core family of the AVR Instruction
 * Set Manual; which instructions exist is the CPU model's).
 *
 * Conventions (docs/uam-isa.md and isa/avr8/avr.h):
 *  - The pc is a byte address in program memory (twice the AVR's word PC); relative
 *    branches and calls are taken modulo the PC's width (16 or 22 bits).
 *  - SREG lives in the register file one byte per flag, so conditions are one TRUNC and
 *    every flag an instruction defines is one PUT.
 *  - The data space is addressed through isa->translate. An access whose address is a
 *    constant (IN/OUT, SBI/CBI/SBIC/SBIS, LDS/STS) to a register of the core (r0-r31 on
 *    AVRe/AVRe+, SREG, SP, RAMPx, EIND, the interrupt controller) becomes a GET/PUT or a
 *    SYSRD/SYSWR; an access through a pointer splits at run time into a guarded helper
 *    for those registers and a guarded LD/ST for the rest of the data space.
 *  - The stack holds return addresses most significant byte first ([SP+1] is the high
 *    byte), two bytes on a 16-bit PC and three on a 22-bit one.
 *  - SEI sets SREG.I with a SYSWR (which requests an exit) and lifts the next
 *    instruction as a continuation: it runs before any interrupt, as the core promises.
 *  - Every instruction does its loads, stores and system accesses before its register
 *    writes (the commit rule).
 * An encoding the model does not have stops into the debugger (TRAP DEBUG): an AVR
 * has no illegal-instruction exception.
 */
#include "avr.h"

#define NONE UAM_REF_NONE
#define LE   UAM_ACC_LE

typedef enum { LIFT_CONTINUE, LIFT_END, LIFT_CONT_NEXT } lift_result;

typedef struct avr_lifter {
    uam_lift_ctx            *lc;
    uam_builder             *b;
    const uam_avr_config    *cfg;
    uam_avr_core             core;
    uint32_t                 pc;      /* byte address of the instruction */
    unsigned                 len;     /* bytes: 2 or 4 */
    uint16_t                 w, w2;   /* the instruction's words */
    uint32_t                 pcmask;  /* byte addresses the PC can hold */
} avr_lifter;

/* ---- IR shorthands ------------------------------------------------------------- */

static uam_ref imm(avr_lifter *L, uint64_t v) { return uam_imm(L->b, v); }
static uam_ref bin(avr_lifter *L, uam_opcode op, unsigned w, uam_ref x, uam_ref y)
{
    return uam_binop(L->b, op, w, x, y);
}
/* Zero-extend a temp; an immediate is the same value at any width. */
static uam_ref zext(avr_lifter *L, unsigned w, uam_ref x)
{
    return uam_ref_is_imm(x) ? x : uam_unop(L->b, UAM_OP_ZEXT, w, x);
}
static uam_ref trunc_(avr_lifter *L, unsigned w, uam_ref x) { return uam_unop(L->b, UAM_OP_TRUNC, w, x); }
static uam_ref not1(avr_lifter *L, uam_ref c) { return bin(L, UAM_OP_XOR, 1, c, imm(L, 1)); }

static uam_ref get8(avr_lifter *L, unsigned r) { return uam_get(L->b, 8, AVR_R(r)); }
static void put8(avr_lifter *L, unsigned r, uam_ref v) { uam_put(L->b, 8, AVR_R(r), v, NONE); }
static uam_ref get16(avr_lifter *L, unsigned r) { return uam_get(L->b, 16, AVR_R(r)); }
static void put16(avr_lifter *L, unsigned r, uam_ref v) { uam_put(L->b, 16, AVR_R(r), v, NONE); }

/* SREG flag b as a boolean, and setting it from one. */
static uam_ref flag(avr_lifter *L, unsigned b) { return trunc_(L, 1, uam_get(L->b, 8, AVR_FLAG(b))); }
static void setf(avr_lifter *L, unsigned b, uam_ref c) { uam_put(L->b, 8, AVR_FLAG(b), zext(L, 8, c), NONE); }
static void setf_imm(avr_lifter *L, unsigned b, unsigned v) { uam_put(L->b, 8, AVR_FLAG(b), imm(L, v), NONE); }

/* Bit n of a w-bit value. */
static uam_ref bit(avr_lifter *L, unsigned w, uam_ref v, unsigned n)
{
    return trunc_(L, 1, n ? bin(L, UAM_OP_LSHR, w, v, imm(L, n)) : v);
}

static lift_result debug_stop(avr_lifter *L)
{
    uam_trap(L->b, NONE, UAM_TRAP_DEBUG, imm(L, 0), imm(L, L->pc));
    return LIFT_END;
}
#define ILLEGAL() return debug_stop(L)

static bool has(const avr_lifter *L, unsigned f) { return (L->cfg->features & f) != 0; }
static bool rc(const avr_lifter *L) { return L->core == UAM_AVR_RC; }
static bool xm(const avr_lifter *L) { return L->core == UAM_AVR_XM; }
static bool pc22(const avr_lifter *L) { return avr_pc22(L->cfg); }

/* ---- flags --------------------------------------------------------------------- */

/* N, Z, S (= N ^ V) and V from an 8-bit result and its overflow. */
static void nzvs(avr_lifter *L, unsigned w, uam_ref r, uam_ref v, uam_ref z)
{
    uam_ref n = bit(L, w, r, w - 1);
    if (uam_ref_is_none(z))
        z = bin(L, UAM_OP_EQ, w, r, imm(L, 0));
    setf(L, AVR_SREG_N, n);
    setf(L, AVR_SREG_Z, z);
    setf(L, AVR_SREG_V, v);
    setf(L, AVR_SREG_S, bin(L, UAM_OP_XOR, 1, n, v));
}

/* a + b + cin or a - b - cin (cin: a boolean or NONE), with every flag the AVR defines
 * for ADD/ADC/SUB/SBC/CP/CPC. keep_z: Z = old Z and (result == 0) (SBC, CPC, SBCI).
 * Returns the 8-bit result; the caller writes it (or not, for compares). */
static uam_ref arith8(avr_lifter *L, bool sub, uam_ref a, uam_ref b, uam_ref cin, bool keep_z)
{
    uam_ref s = bin(L, sub ? UAM_OP_SUB : UAM_OP_ADD, 16, zext(L, 16, a), zext(L, 16, b));
    if (!uam_ref_is_none(cin))
        s = bin(L, sub ? UAM_OP_SUB : UAM_OP_ADD, 16, s, zext(L, 16, cin));
    uam_ref r = trunc_(L, 8, s);
    uam_ref hx = bin(L, UAM_OP_XOR, 8, bin(L, UAM_OP_XOR, 8, a, b), r);
    uam_ref ov = sub ? bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, b), bin(L, UAM_OP_XOR, 8, a, r))
                     : bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, r), bin(L, UAM_OP_XOR, 8, b, r));
    uam_ref z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0));
    if (keep_z)
        z = bin(L, UAM_OP_AND, 1, z, flag(L, AVR_SREG_Z));
    setf(L, AVR_SREG_H, bit(L, 8, hx, 4));
    setf(L, AVR_SREG_C, bit(L, 16, s, 8));
    nzvs(L, 8, r, bit(L, 8, ov, 7), z);
    return r;
}

/* AND/OR/EOR/COM...: V = 0, N, Z, S = N. */
static void logic_flags(avr_lifter *L, uam_ref r)
{
    nzvs(L, 8, r, imm(L, 0), NONE);
}

/* ---- the data space --------------------------------------------------------------- */

/* The ranges of data addresses that are the core's own registers on this model (see
 * avr_core_addr in avr_system.c, which must agree). */
typedef struct { uint32_t lo, n; } core_range;
static unsigned core_ranges(const avr_lifter *L, core_range out[4])
{
    const unsigned io = avr_io_offset(L->core);
    unsigned k = 0;
    switch (L->core) {
    case UAM_AVR_E:
        out[k++] = (core_range){ 0, 0x20 };
        out[k++] = (core_range){ io + AVR_IO_SPL, 3 };
        break;
    case UAM_AVR_EPLUS:
        out[k++] = (core_range){ 0, 0x20 };
        out[k++] = (core_range){ io + AVR_IO_RAMPZ, 5 };
        break;
    case UAM_AVR_XM:
        out[k++] = (core_range){ AVR_IO_CCP, 1 };
        out[k++] = (core_range){ AVR_IO_RAMPD, 8 };
        out[k++] = (core_range){ AVR_PMIC_STATUS, 3 };
        break;
    case UAM_AVR_XT:
        out[k++] = (core_range){ AVR_IO_CCP, 1 };
        out[k++] = (core_range){ AVR_IO_SPL, 3 };
        out[k++] = (core_range){ AVR_CPUINT_CTRLA, 4 };
        break;
    case UAM_AVR_RC:
        out[k++] = (core_range){ AVR_IO_EIND, 4 };    /* CCP is at I/O 0x3C here */
        break;
    }
    return k;
}

static bool const_core(const avr_lifter *L, uint32_t a)
{
    return avr_core_addr(L->cfg, a);
}

/* SREG as one byte. */
static uam_ref sreg_get(avr_lifter *L)
{
    uam_ref v = uam_get(L->b, 8, AVR_FLAG(0));
    for (unsigned i = 1; i < 8; i++)
        v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, uam_get(L->b, 8, AVR_FLAG(i)), imm(L, i)));
    return v;
}

/* SREG from a byte: I through the system register (it may let an interrupt in), then
 * the other flags. */
static void sreg_put(avr_lifter *L, uam_ref v)
{
    uam_syswr(L->b, 8, AVR_SYS_SET_I, bin(L, UAM_OP_LSHR, 8, v, imm(L, 7)), NONE);
    for (unsigned i = 0; i < 7; i++)
        uam_put(L->b, 8, AVR_FLAG(i), bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, i)), imm(L, 1)),
                NONE);
}

/* The register-file offset of a core register at data address a, or ~0 for one the
 * system-register path handles (CCP, interrupt controller) or SREG. */
static uint32_t core_off(const avr_lifter *L, uint32_t a)
{
    if (avr_regs_mapped(L->core) && a < 0x20)
        return AVR_R(a);
    const uint32_t io = a - avr_io_offset(L->core);
    if (L->core == UAM_AVR_RC && io == 0x3C)
        return ~0u;                                   /* CCP */
    switch (io) {
    case AVR_IO_RAMPD: return AVR_RAMPD;
    case AVR_IO_RAMPX: return AVR_RAMPX;
    case AVR_IO_RAMPY: return AVR_RAMPY;
    case AVR_IO_RAMPZ: return AVR_RAMPZ;
    case AVR_IO_EIND:  return AVR_EIND;
    case AVR_IO_SPL:   return AVR_SP;
    case AVR_IO_SPH:   return AVR_SP + 1;
    default:           return ~0u;
    }
}

/* Load data byte at constant address a (data space). */
static uam_ref ld_const(avr_lifter *L, uint32_t a)
{
    if (const_core(L, a)) {
        if (a - avr_io_offset(L->core) == AVR_IO_SREG && !(avr_regs_mapped(L->core) && a < 0x20))
            return sreg_get(L);
        uint32_t off = core_off(L, a);
        if (off != ~0u)
            return uam_get(L->b, 8, off);
        return uam_sysrd(L->b, 8, AVR_SYS_DATA(a), NONE);
    }
    return uam_ld(L->b, 8, LE, imm(L, a), NONE);
}

/* Store data byte v at constant address a. Core registers are written with PUTs, which
 * the caller must emit after any of its faulting ops (it does: stores come last). */
static void st_const(avr_lifter *L, uint32_t a, uam_ref v)
{
    if (const_core(L, a)) {
        if (a - avr_io_offset(L->core) == AVR_IO_SREG && !(avr_regs_mapped(L->core) && a < 0x20)) {
            sreg_put(L, v);
            return;
        }
        uint32_t off = core_off(L, a);
        if (off != ~0u)
            uam_put(L->b, 8, off, v, NONE);
        else
            uam_syswr(L->b, 8, AVR_SYS_DATA(a), v, NONE);
        return;
    }
    uam_st(L->b, 8, LE, imm(L, a), v, NONE);
}

/* Is the (32-bit) data address in a core register range? */
static uam_ref is_core(avr_lifter *L, uam_ref a)
{
    core_range r[4];
    unsigned n = core_ranges(L, r);
    uam_ref c = NONE;
    for (unsigned k = 0; k < n; k++) {
        uam_ref in = r[k].lo ? bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, a, imm(L, r[k].lo)), imm(L, r[k].n))
                             : bin(L, UAM_OP_ULT, 32, a, imm(L, r[k].n));
        c = uam_ref_is_none(c) ? in : bin(L, UAM_OP_OR, 1, c, in);
    }
    return c;
}

/* Load a data byte through a pointer (a: 32-bit data address). */
static uam_ref ld_data(avr_lifter *L, uam_ref a)
{
    uam_ref c = is_core(L, a);
    uam_ref v1 = uam_helper(L->b, &uam_isa_avr.helpers, AVR_HELPER_CORE_RD, a, NONE, NONE, c);
    uam_ref v2 = uam_ld(L->b, 8, LE, a, not1(L, c));
    return bin(L, UAM_OP_OR, 8, v1, v2);   /* a guard that is false leaves 0 */
}

static void st_data(avr_lifter *L, uam_ref a, uam_ref v)
{
    uam_ref c = is_core(L, a);
    uam_helper(L->b, &uam_isa_avr.helpers, AVR_HELPER_CORE_WR, a, v, NONE, c);
    uam_st(L->b, 8, LE, a, v, not1(L, c));
}

/* ---- pointers ------------------------------------------------------------------- */

/* The pointer registers' low register, and the RAMP register that extends them. */
enum { PX = 26, PY = 28, PZ = 30 };
static uint32_t ramp_of(unsigned p) { return p == PX ? AVR_RAMPX : p == PY ? AVR_RAMPY : AVR_RAMPZ; }

/* The full data address in pointer p (24 bits with its RAMP register on AVRxm). */
static uam_ref ptr_get(avr_lifter *L, unsigned p)
{
    uam_ref v = zext(L, 32, get16(L, p));
    if (xm(L))
        v = bin(L, UAM_OP_OR, 32, v, bin(L, UAM_OP_SHL, 32, zext(L, 32, uam_get(L->b, 8, ramp_of(p))), imm(L, 16)));
    return v;
}

static void ptr_put(avr_lifter *L, unsigned p, uam_ref v32)
{
    put16(L, p, trunc_(L, 16, v32));
    if (xm(L))
        uam_put(L->b, 8, ramp_of(p), trunc_(L, 8, bin(L, UAM_OP_LSHR, 32, v32, imm(L, 16))), NONE);
}

static uam_ref ptr_mask(avr_lifter *L, uam_ref v32)
{
    return bin(L, UAM_OP_AND, 32, v32, imm(L, xm(L) ? 0xFFFFFFu : 0xFFFFu));
}

/* ---- the stack and the PC ----------------------------------------------------------- */

static unsigned pc_bytes(const avr_lifter *L) { return pc22(L) ? 3 : 2; }

static uam_ref sp_get(avr_lifter *L) { return zext(L, 32, uam_get(L->b, 16, AVR_SP)); }
static void sp_put(avr_lifter *L, uam_ref v32) { uam_put(L->b, 16, AVR_SP, trunc_(L, 16, v32), NONE); }

/* Push the return address `ret` (a byte pc; the stack holds the word PC) and set SP. */
static void push_pc(avr_lifter *L, uint32_t ret)
{
    const uint32_t w = ret >> 1;
    uam_ref sp = sp_get(L);
    const unsigned n = pc_bytes(L);
    for (unsigned k = 0; k < n; k++) {   /* PCL at SP, then PCH, then PC[21:16] */
        uam_ref a = k ? bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_SUB, 32, sp, imm(L, k)), imm(L, 0xFFFF)) : sp;
        uam_st(L->b, 8, LE, a, imm(L, (w >> (8 * k)) & 0xFF), NONE);
    }
    sp_put(L, bin(L, UAM_OP_SUB, 32, sp, imm(L, n)));
}

/* Pop a return address: its byte pc. SP is written by the caller (after its loads). */
static uam_ref pop_pc(avr_lifter *L, uam_ref *new_sp)
{
    uam_ref sp = sp_get(L);
    const unsigned n = pc_bytes(L);
    uam_ref w = imm(L, 0);
    for (unsigned k = 1; k <= n; k++) {   /* most significant first */
        uam_ref a = bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, sp, imm(L, k)), imm(L, 0xFFFF));
        uam_ref byte = zext(L, 32, uam_ld(L->b, 8, LE, a, NONE));
        w = bin(L, UAM_OP_OR, 32, bin(L, UAM_OP_SHL, 32, w, imm(L, 8)), byte);
    }
    *new_sp = bin(L, UAM_OP_ADD, 32, sp, imm(L, n));
    return bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_SHL, 32, w, imm(L, 1)), imm(L, L->pcmask));
}

static uint32_t rel_target(const avr_lifter *L, int32_t words)
{
    return (uint32_t)(L->pc + L->len + 2 * words) & L->pcmask;
}

static uint32_t next_pc(const avr_lifter *L) { return (L->pc + L->len) & L->pcmask; }

/* ---- instruction groups ---------------------------------------------------------- */

/* 0000 01..-0010 11.. and 1001 11..: two-register ALU ops. */
static lift_result lift_rr(avr_lifter *L, unsigned op4)
{
    const unsigned d = (L->w >> 4) & 0x1F, r = (L->w & 0xF) | ((L->w >> 5) & 0x10);
    if (rc(L) && (d < 16 || r < 16))
        ILLEGAL();
    uam_ref a = get8(L, d), b = get8(L, r), res;
    switch (op4) {
    case 0x1: arith8(L, true, a, b, flag(L, AVR_SREG_C), true); return LIFT_CONTINUE;   /* CPC */
    case 0x2: res = arith8(L, true, a, b, flag(L, AVR_SREG_C), true); break;            /* SBC */
    case 0x3: res = arith8(L, false, a, b, NONE, false); break;                         /* ADD */
    case 0x5: arith8(L, true, a, b, NONE, false); return LIFT_CONTINUE;                 /* CP */
    case 0x6: res = arith8(L, true, a, b, NONE, false); break;                          /* SUB */
    case 0x7: res = arith8(L, false, a, b, flag(L, AVR_SREG_C), false); break;          /* ADC */
    case 0x8: res = bin(L, UAM_OP_AND, 8, a, b); logic_flags(L, res); break;            /* AND */
    case 0x9: res = bin(L, UAM_OP_XOR, 8, a, b); logic_flags(L, res); break;            /* EOR */
    case 0xA: res = bin(L, UAM_OP_OR, 8, a, b); logic_flags(L, res); break;             /* OR */
    case 0xB: res = b; break;                                                           /* MOV */
    default: ILLEGAL();
    }
    put8(L, d, res);
    return LIFT_CONTINUE;
}

/* CPI, SBCI, SUBI, ORI, ANDI, LDI: Rd = r16..r31, K 8 bits. */
static lift_result lift_imm(avr_lifter *L, unsigned top)
{
    const unsigned d = 16 + ((L->w >> 4) & 0xF);
    const uint32_t K = ((L->w >> 4) & 0xF0) | (L->w & 0xF);
    uam_ref a = get8(L, d), k = imm(L, K), res;
    switch (top) {
    case 0x3: arith8(L, true, a, k, NONE, false); return LIFT_CONTINUE;                 /* CPI */
    case 0x4: res = arith8(L, true, a, k, flag(L, AVR_SREG_C), true); break;            /* SBCI */
    case 0x5: res = arith8(L, true, a, k, NONE, false); break;                          /* SUBI */
    case 0x6: res = bin(L, UAM_OP_OR, 8, a, k); logic_flags(L, res); break;             /* ORI */
    case 0x7: res = bin(L, UAM_OP_AND, 8, a, k); logic_flags(L, res); break;            /* ANDI */
    default:  res = k; break;                                                           /* LDI */
    }
    put8(L, d, res);
    return LIFT_CONTINUE;
}

/* MUL, MULS, MULSU, FMUL, FMULS, FMULSU: r1:r0 = product (<< 1 for the F forms),
 * C = bit 15 of the product before that shift, Z of the result. */
static lift_result lift_mul(avr_lifter *L, unsigned d, unsigned r, bool sd, bool sr, bool frac)
{
    if (!has(L, UAM_AVR_F_MUL))
        ILLEGAL();
    uam_ref a = uam_unop(L->b, sd ? UAM_OP_SEXT : UAM_OP_ZEXT, 16, get8(L, d));
    uam_ref b = uam_unop(L->b, sr ? UAM_OP_SEXT : UAM_OP_ZEXT, 16, get8(L, r));
    uam_ref p = bin(L, UAM_OP_MUL, 16, a, b);
    uam_ref res = frac ? bin(L, UAM_OP_SHL, 16, p, imm(L, 1)) : p;
    setf(L, AVR_SREG_C, bit(L, 16, p, 15));
    setf(L, AVR_SREG_Z, bin(L, UAM_OP_EQ, 16, res, imm(L, 0)));
    put16(L, 0, res);
    return LIFT_CONTINUE;
}

/* The one-operand group 1001 010d dddd xxxx (x = 0..7, A). */
static lift_result lift_one(avr_lifter *L, unsigned op)
{
    const unsigned d = (L->w >> 4) & 0x1F;
    if (rc(L) && d < 16)
        ILLEGAL();
    uam_ref a = get8(L, d), r;
    switch (op) {
    case 0x0:                                                       /* COM */
        r = bin(L, UAM_OP_XOR, 8, a, imm(L, 0xFF));
        logic_flags(L, r);
        setf_imm(L, AVR_SREG_C, 1);
        break;
    case 0x1:                                                       /* NEG */
        r = arith8(L, true, imm(L, 0), a, NONE, false);
        break;
    case 0x2:                                                       /* SWAP */
        r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, a, imm(L, 4)), bin(L, UAM_OP_LSHR, 8, a, imm(L, 4)));
        break;
    case 0x3:                                                       /* INC */
        r = bin(L, UAM_OP_ADD, 8, a, imm(L, 1));
        nzvs(L, 8, r, bin(L, UAM_OP_EQ, 8, r, imm(L, 0x80)), NONE);
        break;
    case 0xA:                                                       /* DEC */
        r = bin(L, UAM_OP_SUB, 8, a, imm(L, 1));
        nzvs(L, 8, r, bin(L, UAM_OP_EQ, 8, r, imm(L, 0x7F)), NONE);
        break;
    case 0x5: case 0x6: case 0x7: {                                 /* ASR, LSR, ROR */
        uam_ref c = bit(L, 8, a, 0);
        uam_ref hi = op == 0x5 ? bin(L, UAM_OP_AND, 8, a, imm(L, 0x80))
                   : op == 0x6 ? imm(L, 0)
                               : bin(L, UAM_OP_SHL, 8, zext(L, 8, flag(L, AVR_SREG_C)), imm(L, 7));
        r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_LSHR, 8, a, imm(L, 1)), hi);
        uam_ref n = bit(L, 8, r, 7);
        setf(L, AVR_SREG_C, c);
        nzvs(L, 8, r, bin(L, UAM_OP_XOR, 1, n, c), NONE);          /* V = N ^ C */
        break;
    }
    default:
        ILLEGAL();
    }
    put8(L, d, r);
    return LIFT_CONTINUE;
}

/* ADIW / SBIW: r(25..31 even) pair += / -= K (6 bits). */
static lift_result lift_adiw(avr_lifter *L, bool sub)
{
    if (rc(L))
        ILLEGAL();
    const unsigned d = 24 + 2 * ((L->w >> 4) & 3);
    const uint32_t K = ((L->w >> 2) & 0x30) | (L->w & 0xF);
    uam_ref a = get16(L, d);
    uam_ref s = bin(L, sub ? UAM_OP_SUB : UAM_OP_ADD, 32, zext(L, 32, a), imm(L, K));
    uam_ref r = trunc_(L, 16, s);
    uam_ref a15 = bit(L, 16, a, 15), r15 = bit(L, 16, r, 15);
    uam_ref v = sub ? bin(L, UAM_OP_AND, 1, a15, not1(L, r15)) : bin(L, UAM_OP_AND, 1, not1(L, a15), r15);
    setf(L, AVR_SREG_C, bit(L, 32, s, 16));
    nzvs(L, 16, r, v, NONE);
    put16(L, d, r);
    return LIFT_CONTINUE;
}

/* Words of the next instruction, for skips; 1 if it cannot be fetched. */
static unsigned next_words(avr_lifter *L)
{
    uint64_t w = 0;
    if (L->lc->fetch(L->lc, next_pc(L), 2, &w) != UAM_OK)
        return 1;
    return avr_insn_words((uint16_t)w, L->core);
}

/* Skip the next instruction if `cond`. */
static lift_result skip_if(avr_lifter *L, uam_ref cond)
{
    const uint32_t over = (next_pc(L) + 2 * next_words(L)) & L->pcmask;
    uam_exit(L->b, cond, imm(L, over), UAM_EXIT_JUMP);
    uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
    return LIFT_END;
}

/* LD/ST through X, Y, Z: mode 0 plain, 1 post-increment, 2 pre-decrement; disp for
 * LDD/STD. */
static lift_result lift_ldst_ptr(avr_lifter *L, bool store, unsigned reg, unsigned p, unsigned mode,
                                 unsigned disp)
{
    if (rc(L) && (reg < 16 || disp))
        ILLEGAL();
    uam_ref ptr = ptr_get(L, p);
    uam_ref a = ptr;
    if (mode == 2)
        a = ptr_mask(L, bin(L, UAM_OP_SUB, 32, ptr, imm(L, 1)));
    else if (disp)
        a = ptr_mask(L, bin(L, UAM_OP_ADD, 32, ptr, imm(L, disp)));
    uam_ref v = NONE;
    if (store)
        st_data(L, a, get8(L, reg));
    else
        v = ld_data(L, a);
    if (mode == 1)
        ptr_put(L, p, ptr_mask(L, bin(L, UAM_OP_ADD, 32, ptr, imm(L, 1))));
    else if (mode == 2)
        ptr_put(L, p, a);
    if (!store)
        put8(L, reg, v);
    return LIFT_CONTINUE;
}

/* LPM / ELPM: program memory at Z (with RAMPZ for ELPM). */
static lift_result lift_lpm(avr_lifter *L, unsigned d, bool ext, bool inc)
{
    if (rc(L) || (ext && !has(L, UAM_AVR_F_ELPM)))
        ILLEGAL();
    uam_ref z = zext(L, 32, get16(L, PZ));
    if (ext)
        z = bin(L, UAM_OP_OR, 32, z, bin(L, UAM_OP_SHL, 32, zext(L, 32, uam_get(L->b, 8, AVR_RAMPZ)), imm(L, 16)));
    uam_ref v = uam_ld(L->b, 8, LE | UAM_ACC_PHYS, z, NONE);
    if (inc) {
        uam_ref nz = bin(L, UAM_OP_ADD, 32, z, imm(L, 1));
        put16(L, PZ, trunc_(L, 16, nz));
        if (ext)
            uam_put(L->b, 8, AVR_RAMPZ, trunc_(L, 8, bin(L, UAM_OP_LSHR, 32, nz, imm(L, 16))), NONE);
    }
    put8(L, d, v);
    return LIFT_CONTINUE;
}

/* XCH, LAS, LAC, LAT (AVRxm): read-modify-write of (Z) with Rd; Rd gets the old byte. */
static lift_result lift_rmw(avr_lifter *L, unsigned d, unsigned op)
{
    if (!xm(L))
        ILLEGAL();
    uam_ref a = ptr_get(L, PZ);
    uam_ref old = ld_data(L, a), r = get8(L, d), nv;
    switch (op) {
    case 4:  nv = r; break;                                                          /* XCH */
    case 5:  nv = bin(L, UAM_OP_OR, 8, old, r); break;                                /* LAS */
    case 6:  nv = bin(L, UAM_OP_AND, 8, old, bin(L, UAM_OP_XOR, 8, r, imm(L, 0xFF))); break; /* LAC */
    default: nv = bin(L, UAM_OP_XOR, 8, old, r); break;                               /* LAT */
    }
    st_data(L, a, nv);
    put8(L, d, old);
    return LIFT_CONTINUE;
}

/* The 1001 000x group: loads, stores, LPM/ELPM, PUSH/POP, XCH/LAS/LAC/LAT, LDS/STS. */
static lift_result lift_1001_00(avr_lifter *L)
{
    const bool store = (L->w >> 9) & 1;
    const unsigned d = (L->w >> 4) & 0x1F, low = L->w & 0xF;
    if (rc(L) && d < 16)
        ILLEGAL();
    switch (low) {
    case 0x0: {                                                     /* LDS / STS (32-bit) */
        if (rc(L))
            ILLEGAL();
        uint32_t k = L->w2;
        if (xm(L)) {   /* RAMPD extends a constant address: not a constant then */
            uam_ref a = bin(L, UAM_OP_OR, 32, imm(L, k),
                            bin(L, UAM_OP_SHL, 32, zext(L, 32, uam_get(L->b, 8, AVR_RAMPD)), imm(L, 16)));
            if (store) { st_data(L, a, get8(L, d)); return LIFT_CONTINUE; }
            put8(L, d, ld_data(L, a));
            return LIFT_CONTINUE;
        }
        if (store) { st_const(L, k, get8(L, d)); return LIFT_CONTINUE; }
        put8(L, d, ld_const(L, k));
        return LIFT_CONTINUE;
    }
    case 0x1: return lift_ldst_ptr(L, store, d, PZ, 1, 0);
    case 0x2: return lift_ldst_ptr(L, store, d, PZ, 2, 0);
    case 0x4: case 0x5: case 0x6: case 0x7:
        if (store)
            return lift_rmw(L, d, low);
        return lift_lpm(L, d, low >= 6, low & 1);
    case 0x9: return lift_ldst_ptr(L, store, d, PY, 1, 0);
    case 0xA: return lift_ldst_ptr(L, store, d, PY, 2, 0);
    case 0xC: return lift_ldst_ptr(L, store, d, PX, 0, 0);
    case 0xD: return lift_ldst_ptr(L, store, d, PX, 1, 0);
    case 0xE: return lift_ldst_ptr(L, store, d, PX, 2, 0);
    case 0xF: {                                                     /* POP / PUSH */
        uam_ref sp = sp_get(L);
        if (store) {
            uam_st(L->b, 8, LE, sp, get8(L, d), NONE);
            sp_put(L, bin(L, UAM_OP_SUB, 32, sp, imm(L, 1)));
        } else {
            uam_ref nsp = bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, sp, imm(L, 1)), imm(L, 0xFFFF));
            uam_ref v = uam_ld(L->b, 8, LE, nsp, NONE);
            sp_put(L, nsp);
            put8(L, d, v);
        }
        return LIFT_CONTINUE;
    }
    default:
        ILLEGAL();
    }
}

/* Indirect jumps and calls; ext: EIJMP/EICALL (EIND). */
static lift_result lift_ijmp(avr_lifter *L, bool call, bool ext)
{
    if (ext && !pc22(L))
        ILLEGAL();
    uam_ref t = zext(L, 32, get16(L, PZ));
    if (ext)
        t = bin(L, UAM_OP_OR, 32, t, bin(L, UAM_OP_SHL, 32, zext(L, 32, uam_get(L->b, 8, AVR_EIND)), imm(L, 16)));
    t = bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_SHL, 32, t, imm(L, 1)), imm(L, L->pcmask));
    if (call)
        push_pc(L, next_pc(L));
    uam_exit(L->b, NONE, t, call ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
    return LIFT_END;
}

/* 1001 010x xxxx 1xxx: one-word control and system instructions, BSET/BCLR, DES,
 * JMP/CALL. */
static lift_result lift_1001_01_misc(avr_lifter *L)
{
    const uint16_t w = L->w;
    if ((w & 0xFF0F) == 0x9408) {                                   /* BSET / BCLR */
        const unsigned s = (w >> 4) & 7;
        const bool clr = (w >> 7) & 1;
        if (s == AVR_SREG_I) {
            uam_syswr(L->b, 8, AVR_SYS_SET_I, imm(L, clr ? 0 : 1), NONE);
            if (!clr)
                return LIFT_CONT_NEXT;      /* SEI: the next instruction runs first */
            return LIFT_CONTINUE;
        }
        setf_imm(L, s, clr ? 0 : 1);
        return LIFT_CONTINUE;
    }
    if ((w & 0xFF0F) == 0x940B) {                                   /* DES */
        if (!xm(L))
            ILLEGAL();
        uam_helper(L->b, &uam_isa_avr.helpers, AVR_HELPER_DES, imm(L, (w >> 4) & 0xF), NONE, NONE, NONE);
        return LIFT_CONTINUE;
    }
    if ((w & 0xFE0C) == 0x940C) {                                   /* JMP / CALL */
        if (!has(L, UAM_AVR_F_JMP))
            ILLEGAL();
        const uint32_t k = ((uint32_t)((w >> 4) & 0x1F) << 17 | (uint32_t)(w & 1) << 16 | L->w2);
        const uint32_t t = (2 * k) & L->pcmask;
        const bool call = (w >> 1) & 1;
        if (call)
            push_pc(L, next_pc(L));
        uam_exit(L->b, NONE, imm(L, t), call ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
        return LIFT_END;
    }
    switch (w) {
    case 0x9409: return lift_ijmp(L, false, false);                 /* IJMP */
    case 0x9419: return lift_ijmp(L, false, true);                  /* EIJMP */
    case 0x9509: return lift_ijmp(L, true, false);                  /* ICALL */
    case 0x9519: return lift_ijmp(L, true, true);                   /* EICALL */
    case 0x9508: case 0x9518: {                                     /* RET / RETI */
        uam_ref nsp;
        uam_ref t = pop_pc(L, &nsp);
        const bool reti = w == 0x9518;
        if (reti)   /* the interrupt controller (or SREG.I) first: it may let one in */
            uam_helper(L->b, &uam_isa_avr.helpers, AVR_HELPER_RETI, NONE, NONE, NONE, NONE);
        sp_put(L, nsp);
        uam_exit(L->b, NONE, t, reti ? UAM_EXIT_EXC_RETURN : UAM_EXIT_RET);
        return LIFT_END;
    }
    case 0x9588:                                                    /* SLEEP */
        uam_wait(L->b, imm(L, next_pc(L)));
        return LIFT_END;
    case 0x9598:                                                    /* BREAK */
        if (!has(L, UAM_AVR_F_BREAK))
            ILLEGAL();
        uam_trap(L->b, NONE, UAM_TRAP_DEBUG, imm(L, 0), imm(L, L->pc));
        return LIFT_END;
    case 0x95A8:                                                    /* WDR: no watchdog here */
        return LIFT_CONTINUE;
    case 0x95C8: return lift_lpm(L, 0, false, false);               /* LPM */
    case 0x95D8: return lift_lpm(L, 0, true, false);                /* ELPM */
    case 0x95E8: case 0x95F8: {                                     /* SPM, SPM Z+ */
        const bool inc = w == 0x95F8;
        if (!has(L, UAM_AVR_F_SPM) || (inc && !avr_levels(L->core)))
            ILLEGAL();
        uam_ref z = zext(L, 32, get16(L, PZ));
        if (has(L, UAM_AVR_F_ELPM))
            z = bin(L, UAM_OP_OR, 32, z, bin(L, UAM_OP_SHL, 32, zext(L, 32, uam_get(L->b, 8, AVR_RAMPZ)), imm(L, 16)));
        uam_helper(L->b, &uam_isa_avr.helpers, AVR_HELPER_SPM, z, zext(L, 32, get16(L, 0)), NONE, NONE);
        if (inc)
            put16(L, PZ, trunc_(L, 16, bin(L, UAM_OP_ADD, 32, z, imm(L, 2))));
        /* the flash may have changed under translated code */
        uam_fence(L->b, UAM_FENCE_INSN);
        uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
        return LIFT_END;
    }
    default:
        ILLEGAL();
    }
}

/* SBI, CBI, SBIC, SBIS: I/O addresses 0-31, bit b. */
static lift_result lift_io_bit(avr_lifter *L, unsigned op)
{
    const uint32_t a = ((L->w >> 3) & 0x1F) + avr_io_offset(L->core);
    const unsigned b = L->w & 7;
    uam_ref v = ld_const(L, a);
    switch (op) {
    case 0: st_const(L, a, bin(L, UAM_OP_AND, 8, v, imm(L, 0xFF & ~(1u << b)))); return LIFT_CONTINUE;  /* CBI */
    case 2: st_const(L, a, bin(L, UAM_OP_OR, 8, v, imm(L, 1u << b))); return LIFT_CONTINUE;             /* SBI */
    case 1: return skip_if(L, not1(L, bit(L, 8, v, b)));                                                 /* SBIC */
    default: return skip_if(L, bit(L, 8, v, b));                                                         /* SBIS */
    }
}

/* The AVRrc 16-bit LDS/STS: 1010 sKKK dddd KKKK, address 0x40..0xBF. */
static lift_result lift_lds16(avr_lifter *L)
{
    const unsigned d = 16 + ((L->w >> 4) & 0xF);
    const uint32_t a = (uint32_t)((~L->w >> 8) & 1) << 7 | (uint32_t)((L->w >> 8) & 1) << 6
                     | (uint32_t)((L->w >> 10) & 1) << 5 | (uint32_t)((L->w >> 9) & 1) << 4 | (L->w & 0xF);
    if ((L->w >> 11) & 1)
        st_const(L, a, get8(L, d));
    else
        put8(L, d, ld_const(L, a));
    return LIFT_CONTINUE;
}

unsigned avr_insn_words(uint16_t w, uam_avr_core c)
{
    if ((w & 0xFC0F) == 0x9000)                       /* LDS / STS */
        return c == UAM_AVR_RC ? 1 : 2;
    if ((w & 0xFE0C) == 0x940C)                       /* JMP / CALL */
        return 2;
    return 1;
}

static lift_result lift_insn(avr_lifter *L)
{
    const uint16_t w = L->w;
    switch (w >> 12) {
    case 0x0:
        if (w == 0)
            return LIFT_CONTINUE;                                   /* NOP */
        switch ((w >> 8) & 0xF) {
        case 0x0: ILLEGAL();
        case 0x1:                                                   /* MOVW */
            if (rc(L))
                ILLEGAL();
            put16(L, 2 * ((w >> 4) & 0xF), get16(L, 2 * (w & 0xF)));
            return LIFT_CONTINUE;
        case 0x2: return lift_mul(L, 16 + ((w >> 4) & 0xF), 16 + (w & 0xF), true, true, false);   /* MULS */
        case 0x3: {
            const unsigned d = 16 + ((w >> 4) & 7), r = 16 + (w & 7);
            switch (((w >> 6) & 2) | ((w >> 3) & 1)) {
            case 0: return lift_mul(L, d, r, true, false, false);  /* MULSU */
            case 1: return lift_mul(L, d, r, false, false, true);  /* FMUL */
            case 2: return lift_mul(L, d, r, true, true, true);    /* FMULS */
            default: return lift_mul(L, d, r, true, false, true);  /* FMULSU */
            }
        }
        default: return lift_rr(L, (w >> 10) & 3);                  /* CPC, SBC, ADD */
        }
    case 0x1: case 0x2:
        if ((w & 0xFC00) == 0x1000) {                               /* CPSE */
            const unsigned d = (w >> 4) & 0x1F, r = (w & 0xF) | ((w >> 5) & 0x10);
            if (rc(L) && (d < 16 || r < 16))
                ILLEGAL();
            return skip_if(L, bin(L, UAM_OP_EQ, 8, get8(L, d), get8(L, r)));
        }
        return lift_rr(L, (w >> 10) & 0xF);
    case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: case 0xE:
        return lift_imm(L, w >> 12);
    case 0x8: case 0xA: {                                           /* LDD / STD (q) */
        if (rc(L) && (w >> 12) == 0xA)
            return lift_lds16(L);
        const unsigned q = ((w >> 8) & 0x20) | ((w >> 7) & 0x18) | (w & 7);
        return lift_ldst_ptr(L, (w >> 9) & 1, (w >> 4) & 0x1F, (w & 8) ? PY : PZ, 0, q);
    }
    case 0x9:
        switch ((w >> 8) & 0xF) {
        case 0x0: case 0x1: case 0x2: case 0x3: return lift_1001_00(L);
        case 0x4: case 0x5:
            if ((w & 0x8) && (w & 0xF) != 0xA)
                return lift_1001_01_misc(L);
            return lift_one(L, w & 0xF);
        case 0x6: return lift_adiw(L, false);
        case 0x7: return lift_adiw(L, true);
        case 0x8: case 0x9: case 0xA: case 0xB: return lift_io_bit(L, (w >> 8) & 3);
        default:  {                                                 /* MUL */
            const unsigned d = (w >> 4) & 0x1F, r = (w & 0xF) | ((w >> 5) & 0x10);
            return lift_mul(L, d, r, false, false, false);
        }
        }
    case 0xB: {                                                     /* IN / OUT */
        const unsigned d = (w >> 4) & 0x1F;
        if (rc(L) && d < 16)
            ILLEGAL();
        const uint32_t a = (((w >> 5) & 0x30) | (w & 0xF)) + avr_io_offset(L->core);
        if (w & 0x800)
            st_const(L, a, get8(L, d));
        else
            put8(L, d, ld_const(L, a));
        return LIFT_CONTINUE;
    }
    case 0xC: case 0xD: {                                           /* RJMP / RCALL */
        const int32_t k = (int32_t)uam_sext(w & 0xFFF, 12);
        const bool call = (w >> 12) == 0xD;
        if (call)
            push_pc(L, next_pc(L));
        uam_exit(L->b, NONE, imm(L, rel_target(L, k)), call ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
        return LIFT_END;
    }
    default: {                                                      /* 0xF */
        const unsigned d = (w >> 4) & 0x1F, b = w & 7;
        if ((w & 0x0800) == 0) {                                    /* BRBS / BRBC */
            const int32_t k = (int32_t)uam_sext((w >> 3) & 0x7F, 7);
            uam_ref c = flag(L, w & 7);
            if (w & 0x0400)
                c = not1(L, c);
            uam_exit(L->b, c, imm(L, rel_target(L, k)), UAM_EXIT_JUMP);
            uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
            return LIFT_END;
        }
        if (w & 8)
            ILLEGAL();
        if (rc(L) && d < 16)
            ILLEGAL();
        switch ((w >> 9) & 3) {
        case 0: {                                                   /* BLD */
            uam_ref m = imm(L, 1u << b);
            uam_ref v = bin(L, UAM_OP_AND, 8, get8(L, d), imm(L, 0xFF & ~(1u << b)));
            uam_ref t = uam_select(L->b, 8, flag(L, AVR_SREG_T), m, imm(L, 0));
            put8(L, d, bin(L, UAM_OP_OR, 8, v, t));
            return LIFT_CONTINUE;
        }
        case 1: setf(L, AVR_SREG_T, bit(L, 8, get8(L, d), b)); return LIFT_CONTINUE;   /* BST */
        case 2: return skip_if(L, not1(L, bit(L, 8, get8(L, d), b)));                   /* SBRC */
        default: return skip_if(L, bit(L, 8, get8(L, d), b));                           /* SBRS */
        }
    }
    }
}

uam_status avr_lift(uam_lift_ctx *lc)
{
    const avr_state *s = avr_st(lc->cpu);
    const uint32_t pcmask = avr_pc22(&s->cfg) ? 0x7FFFFFu : 0x1FFFFu;
    uint32_t pc = (uint32_t)lc->pc & pcmask;
    bool cont = false;

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns && !cont) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint64_t w = 0, w2 = 0;
        uam_status st = lc->fetch(lc, pc, 2, &w);
        unsigned words = st == UAM_OK ? avr_insn_words((uint16_t)w, s->cfg.core) : 1;
        if (st == UAM_OK && words == 2)
            st = lc->fetch(lc, pc + 2, 2, &w2);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        avr_lifter L = { .lc = lc, .b = lc->b, .cfg = &s->cfg, .core = s->cfg.core, .pc = pc,
                         .len = 2 * words, .w = (uint16_t)w, .w2 = (uint16_t)w2, .pcmask = pcmask };
        if (cont)
            uam_imark_cont(lc->b, pc, L.len);
        else
            uam_imark(lc->b, pc, L.len);
        lift_result r = lift_insn(&L);
        pc = (pc + L.len) & pcmask;
        if (r == LIFT_END)
            return UAM_OK;
        if (cont) {   /* the instruction after SEI ran: let the interrupt in */
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        cont = r == LIFT_CONT_NEXT;
    }
}
