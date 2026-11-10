/*
 * jit_x86.c - The x86-64 code generator of the native backend (jit.h, jit_impl.h).
 *
 * Compiles one verified, optimized block, analysed by jit.c, to a host function
 *
 *     void code(uam_jit_ctx *jc);
 *
 * that leaves the cpu in exactly the state uam_interp_block would and writes the
 * block's outcome into jc->res. Every op is emitted inline except where C must run
 * anyway: memory accesses the inline data-TLB path cannot do go through execops.h,
 * sysregs and helpers through the ISA, and POPCNT on a host without it through the
 * interpreter's evaluator (ir_eval.h). test_jitdiff checks the two backends agree.
 * Only a block whose code overflows the scratch buffer runs interpreted
 * (uam_jit_compile sets UAM_JIT_INTERP).
 *
 * Host register use (System V AMD64):
 *   rbx = jc    rbp = jc->temps    r14 = jc->cpu    r15 = jc->cpu->regfile
 *   r13 = icount - jc->ic_base (cpu->icount is brought up to date before bridges and
 *         at the epilogue)
 *   r12, rsi, rdi, r8, r9, r10 = guest temporaries (register allocation, jit.c)
 *   rax, rcx, rdx, r11 = scratch
 * The fixed ones and r12 are callee-saved, so they survive bridge calls. Temporaries
 * are kept zero-extended to their width (the interpreter's invariant) in a register or,
 * when spilled, in jc->temps[]; they are dead at every block exit, so only the register
 * file, memory and icount are guest-visible state.
 * jc->ic_base is cpu->icount_stop (exit_request ? 0 : icount_deadline, set on entry and
 * zeroed by uam_cpu_request_exit) as native code last read it: on entry and after every
 * bridge, the only places it can change under a running block. So r13 is negative until
 * the block must stop, and every IMARK that may stop counts and tests in one add and an
 * untaken branch to an out-of-line stub, with nothing read from memory.
 *
 * Layout of a compiled block: prologue | body | epilogue | link slots. Every block
 * keeps the same frame and register assignment, so a chained jump (jit.h) goes from an
 * EXIT straight to the next block's body, skipping its prologue; the last block's
 * epilogue returns to the run loop.
 *
 * This file builds only on x86-64 (jit_a64.c is the AArch64 one).
 */
#include "jit_impl.h"

#if defined(__x86_64__)

#include <stdlib.h>
#include <string.h>

/* ---- the compiler ------------------------------------------------------ */

/* Host registers. */
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R8 = 8, R9 = 9,
       R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15 };

/* Registers that hold temps (register allocation, jit.c), r12 first: it is
 * callee-saved, so its value survives bridge calls; the others are saved around a call
 * while they hold a value still needed after it. rax, rcx, rdx and r11 stay scratch:
 * templates compute in rax, shifts count in cl, mul/div use rdx, and the memory fast
 * paths use rdx (address) and r11 (store value). */
#define RA_POOL_MAX 6
static const int ra_regs[RA_POOL_MAX] = { R12, RSI, RDI, R8, R9, R10 };

static void g8(uam_jit *j, uint8_t b)
{
    if (j->n < CBUF_MAX)
        j->cb[j->n] = b;
    else
        j->overflow = true;
    j->n++;
}
static void g32(uam_jit *j, uint32_t v) { for (int i = 0; i < 4; i++) g8(j, (uint8_t)(v >> (8 * i))); }
static void g64(uam_jit *j, uint64_t v) { for (int i = 0; i < 8; i++) g8(j, (uint8_t)(v >> (8 * i))); }

static void rex(uam_jit *j, int w, int r, int x, int b)
{
    if (w || r || x || b)
        g8(j, (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}
static void modrm_reg(uam_jit *j, int reg, int rm) { g8(j, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7))); }

static void modrm_mem(uam_jit *j, int reg, int base, int32_t disp)
{
    int b = base & 7;
    int mod = (disp == 0 && b != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    int sib = (b == 4);
    g8(j, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (sib ? 4 : b)));
    if (sib)
        g8(j, (uint8_t)(0x20 | b));         /* scale 0, index none, base b */
    if (mod == 1)
        g8(j, (uint8_t)(int8_t)disp);
    else if (mod == 2)
        g32(j, (uint32_t)disp);
}

/* [base + index<<scale + disp]; caller emits REX (r=reg>=8, x=index>=8, b=base>=8). */
static void modrm_sib(uam_jit *j, int reg, int base, int index, int scale, int32_t disp)
{
    int b = base & 7;
    int mod = (disp == 0 && b != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    g8(j, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
    g8(j, (uint8_t)((scale << 6) | ((index & 7) << 3) | b));
    if (mod == 1)
        g8(j, (uint8_t)(int8_t)disp);
    else if (mod == 2)
        g32(j, (uint32_t)disp);
}

/* mov reg64, [base + disp] */
static void ld_mem(uam_jit *j, int reg, int base, int32_t disp)
{
    rex(j, 1, reg >= 8, 0, base >= 8);
    g8(j, 0x8B);
    modrm_mem(j, reg, base, disp);
}
/* mov [base + disp], reg64 */
static void st_mem(uam_jit *j, int reg, int base, int32_t disp)
{
    rex(j, 1, reg >= 8, 0, base >= 8);
    g8(j, 0x89);
    modrm_mem(j, reg, base, disp);
}
static void movabs(uam_jit *j, int reg, uint64_t imm)
{
    rex(j, 1, 0, 0, reg >= 8);
    g8(j, (uint8_t)(0xB8 + (reg & 7)));
    g64(j, imm);
}
static void mov_rr(uam_jit *j, int dst, int src)   /* mov dst64, src64 */
{
    rex(j, 1, src >= 8, 0, dst >= 8);
    g8(j, 0x89);
    modrm_reg(j, src, dst);
}
static void st_temp(uam_jit *j, uint32_t idx, int reg) { st_mem(j, reg, RBP, (int32_t)(8 * idx)); }

/* ---- operands ------------------------------------------------------------------- */

/* Where an IR operand lives: an immediate, a host register, or a temp's slot in
 * jc->temps (a temp not allocated a register, or spilled). */
enum { O_IMM, O_MEM, O_REG };
typedef struct opnd { int kind; uint64_t imm; int32_t disp; int reg; } opnd;

static opnd opnd_of(const uam_jit *j, const uam_block *blk, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        return (opnd){ O_IMM, blk->imm[uam_ref_index(r)] & uam_mask(w), 0, 0 };
    if (j->reg_of[r] >= 0)
        return (opnd){ O_REG, 0, 0, j->reg_of[r] };
    return (opnd){ O_MEM, 0, (int32_t)(8 * r), 0 };
}

/* mov dst32, src32 (zero-extends) */
static void mov_rr32(uam_jit *j, int dst, int src)
{
    rex(j, 0, src >= 8, 0, dst >= 8);
    g8(j, 0x89);
    modrm_reg(j, src, dst);
}

static bool fits_s32(uint64_t v) { return (int64_t)v == (int32_t)v; }
static bool fits_s8(int64_t v) { return v >= -128 && v <= 127; }

/* reg = v without touching the host flags (no xor-zeroing): the shortest of
 * mov r32,imm32 (zero-extends), mov r64,simm32 and movabs. */
static void mov_imm(uam_jit *j, int reg, uint64_t v)
{
    if (v <= 0xFFFFFFFFu) {
        rex(j, 0, 0, 0, reg >= 8);
        g8(j, (uint8_t)(0xB8 + (reg & 7)));
        g32(j, (uint32_t)v);
    } else if (fits_s32(v)) {
        rex(j, 1, 0, 0, reg >= 8);
        g8(j, 0xC7); modrm_reg(j, 0, reg);
        g32(j, (uint32_t)v);
    } else {
        movabs(j, reg, v);
    }
}

/* reg = the operand, a value of width w (temps are kept zero-extended, so a 32-bit
 * load of a narrow one is enough). */
static void load(uam_jit *j, int reg, opnd o, unsigned w)
{
    if (o.kind == O_IMM) {
        mov_imm(j, reg, o.imm);
    } else if (o.kind == O_REG) {
        if (o.reg != reg) {
            if (w <= 32) mov_rr32(j, reg, o.reg);
            else         mov_rr(j, reg, o.reg);
        }
    } else if (w <= 32) {
        rex(j, 0, reg >= 8, 0, 0);
        g8(j, 0x8B); modrm_mem(j, reg, RBP, o.disp);           /* mov r32, [rbp+d] */
    } else {
        ld_mem(j, reg, RBP, o.disp);
    }
}

static void load_operand(uam_jit *j, int reg, const uam_block *blk, uam_ref r, unsigned w)
{
    load(j, reg, opnd_of(j, blk, r, w), w);
}

/* Prefixes for an instruction at operand size os (8/16/32/64) with ModRM reg `r` and
 * rm `rm` (a register if rm_reg, else a memory base). Byte access to rsp..rdi as
 * registers needs a REX prefix. */
static void opsize(uam_jit *j, unsigned os, int r, int rm, bool rm_reg)
{
    if (os == 16)
        g8(j, 0x66);
    int w = os == 64, R = r >= 8, B = rm >= 8;
    bool byte_rex = os == 8 && ((r >= 4 && r < 8) || (rm_reg && rm >= 4 && rm < 8));
    if (w || R || B || byte_rex)
        g8(j, (uint8_t)(0x40 | (w << 3) | (R << 2) | B));
}

/* x86 group-1 ALU operations (the /digit of opcodes 0x80..0x83). */
enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };

static void alu_rr(uam_jit *j, int op, unsigned os, int dst, int src)
{
    opsize(j, os, src, dst, true);
    g8(j, (uint8_t)((op << 3) | (os == 8 ? 0 : 1)));        /* op r/m, r */
    modrm_reg(j, src, dst);
}

/* reg = reg OP operand, at operand size os. */
static void alu(uam_jit *j, int op, unsigned os, int reg, opnd o)
{
    if (o.kind == O_REG) {
        alu_rr(j, op, os, reg, o.reg);
        return;
    }
    if (o.kind == O_MEM) {
        opsize(j, os, reg, RBP, false);
        g8(j, (uint8_t)((op << 3) | (os == 8 ? 2 : 3)));    /* op r, [rbp+d] */
        modrm_mem(j, reg, RBP, o.disp);
        return;
    }
    if (os == 64 && !fits_s32(o.imm)) {
        int s = reg == RCX ? RDX : RCX;
        movabs(j, s, o.imm);
        alu_rr(j, op, os, reg, s);
        return;
    }
    opsize(j, os, 0, reg, true);
    if (os == 8) {
        g8(j, 0x80); modrm_reg(j, op, reg); g8(j, (uint8_t)o.imm);
        return;
    }
    int64_t sv = uam_sext(o.imm, os);
    if (fits_s8(sv)) {
        g8(j, 0x83); modrm_reg(j, op, reg); g8(j, (uint8_t)sv);
    } else {
        g8(j, 0x81); modrm_reg(j, op, reg);
        if (os == 16) { g8(j, (uint8_t)o.imm); g8(j, (uint8_t)(o.imm >> 8)); }
        else g32(j, (uint32_t)o.imm);
    }
}

/* reg = reg * operand (imul), at operand size 32 or 64. */
static void imul(uam_jit *j, unsigned os, int reg, opnd o)
{
    if (o.kind == O_REG) {
        opsize(j, os, reg, o.reg, true);
        g8(j, 0x0F); g8(j, 0xAF); modrm_reg(j, reg, o.reg);
    } else if (o.kind == O_MEM) {
        opsize(j, os, reg, RBP, false);
        g8(j, 0x0F); g8(j, 0xAF); modrm_mem(j, reg, RBP, o.disp);
    } else if (os == 64 && !fits_s32(o.imm)) {
        movabs(j, RCX, o.imm);
        opsize(j, os, reg, RCX, true);
        g8(j, 0x0F); g8(j, 0xAF); modrm_reg(j, reg, RCX);
    } else {
        int64_t sv = uam_sext(o.imm, os);
        opsize(j, os, reg, reg, true);
        if (fits_s8(sv)) { g8(j, 0x6B); modrm_reg(j, reg, reg); g8(j, (uint8_t)sv); }
        else { g8(j, 0x69); modrm_reg(j, reg, reg); g32(j, (uint32_t)o.imm); }
    }
}

/* Shift group (C1 /digit ib) by an immediate, at operand size os. */
enum { SH_ROR = 1, SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };
static void shift_imm(uam_jit *j, int op, unsigned os, int reg, unsigned n)
{
    opsize(j, os, 0, reg, true);
    g8(j, os == 8 ? 0xC0 : 0xC1); modrm_reg(j, op, reg); g8(j, (uint8_t)n);
}

/* The REX prefix of a 32-bit instruction with ModRM reg `r` and the register `rm`, which
 * it reads as a byte if rm_byte (sil, dil need the prefix then). */
static void rex_rb(uam_jit *j, int r, int rm, bool rm_byte)
{
    if (r >= 8 || rm >= 8 || (rm_byte && rm >= 4))
        g8(j, (uint8_t)(0x40 | ((r >= 8) << 2) | (rm >= 8)));
}
/* dst = the low `w` bits of src, zero-extended to 64 bits (w = 64: a 64-bit move). */
static void zext_rr(uam_jit *j, int dst, int src, unsigned w)
{
    switch (w) {
    case 8:  rex_rb(j, dst, src, true); g8(j, 0x0F); g8(j, 0xB6); modrm_reg(j, dst, src); break; /* movzx r32,r8 */
    case 16: rex_rb(j, dst, src, false); g8(j, 0x0F); g8(j, 0xB7); modrm_reg(j, dst, src); break; /* movzx r32,r16 */
    case 32: mov_rr32(j, dst, src); break;
    case 1:
        if (dst != src) mov_rr32(j, dst, src);
        rex_rb(j, 0, dst, false); g8(j, 0x83); modrm_reg(j, 4, dst); g8(j, 1);          /* and r32,1 */
        break;
    default:
        if (dst != src) mov_rr(j, dst, src);
        break;
    }
}
/* Zero-extend the low `w` bits of reg to 64 bits (mask to width). */
static void mask_reg(uam_jit *j, int reg, unsigned w) { zext_rr(j, reg, reg, w); }
/* Sign-extend the low `w` bits of `reg` to 64 bits. A 1-bit value is 0 or 1 (temps are
 * masked to their width), so negating it sign-extends it. */
static void sext_reg(uam_jit *j, int reg, unsigned w)
{
    const int b = reg >= 8;
    switch (w) {
    case 1:  rex(j, 1, 0, 0, b); g8(j, 0xF7); modrm_reg(j, 3, reg); break;              /* neg */
    case 8:  rex(j, 1, b, 0, b); g8(j, 0x0F); g8(j, 0xBE); modrm_reg(j, reg, reg); break; /* movsx */
    case 16: rex(j, 1, b, 0, b); g8(j, 0x0F); g8(j, 0xBF); modrm_reg(j, reg, reg); break;
    case 32: rex(j, 1, b, 0, b); g8(j, 0x63); modrm_reg(j, reg, reg); break;              /* movsxd */
    default: break;
    }
}
/* reg = 1 if the condition cc (the low nibble of its jcc opcode) holds, else 0. Leaves
 * the flags alone. */
static void setcc(uam_jit *j, int reg, uint8_t cc)
{
    rex_rb(j, 0, reg, true); g8(j, 0x0F); g8(j, 0x90 | cc); modrm_reg(j, 0, reg);       /* setcc r8 */
    zext_rr(j, reg, reg, 8);
}

/* lea rd, [base + disp] / [base + index], at operand size os (32 zero-extends). */
static void lea_disp(uam_jit *j, unsigned os, int rd, int base, int32_t disp)
{
    rex(j, os == 64, rd >= 8, 0, base >= 8);
    g8(j, 0x8D); modrm_mem(j, rd, base, disp);
}
static void lea_index(uam_jit *j, unsigned os, int rd, int base, int index)
{
    rex(j, os == 64, rd >= 8, index >= 8, base >= 8);
    g8(j, 0x8D); modrm_sib(j, rd, base, index, 0, 0);
}

static size_t jmp_fwd(uam_jit *j);
static size_t jcc_fwd(uam_jit *j, uint8_t cc);
static void   patch(uam_jit *j, size_t pos);

/* Set ZF from a boolean operand (temp or slot): test r32,r32 / cmp byte [slot],0. */
static void test_bool(uam_jit *j, opnd o)
{
    if (o.kind == O_REG) {
        opsize(j, 32, o.reg, o.reg, true);
        g8(j, 0x85); modrm_reg(j, o.reg, o.reg);
    } else {
        g8(j, 0x80); modrm_mem(j, 7, RBP, o.disp); g8(j, 0);
    }
}

/* call fn (through rax) */
static void call_bridge(uam_jit *j, const void *fn)
{
    mov_imm(j, RAX, (uint64_t)(uintptr_t)fn);
    g8(j, 0xFF); modrm_reg(j, 2, RAX);             /* call rax */
}

/* rd = [rcx + ra], `size` bytes, zero-extended; byte-swapped if big-endian. */
static void emit_fast_load(uam_jit *j, unsigned size, bool be, int rd, int ra)
{
    const int r = rd >= 8, x = ra >= 8;
    switch (size) {
    case 1: rex(j, 0, r, x, 0); g8(j, 0x0F); g8(j, 0xB6); modrm_sib(j, rd, RCX, ra, 0, 0); break; /* movzx r32,byte */
    case 2:
        rex(j, 0, r, x, 0); g8(j, 0x0F); g8(j, 0xB7); modrm_sib(j, rd, RCX, ra, 0, 0);    /* movzx r32,word */
        if (be) { g8(j, 0x66); rex(j, 0, 0, 0, r); g8(j, 0xC1); modrm_reg(j, 0, rd); g8(j, 8); } /* rol r16,8 */
        break;
    case 4:
        rex(j, 0, r, x, 0); g8(j, 0x8B); modrm_sib(j, rd, RCX, ra, 0, 0);                 /* mov r32,[..] */
        if (be) { rex(j, 0, 0, 0, r); g8(j, 0x0F); g8(j, (uint8_t)(0xC8 + (rd & 7))); }   /* bswap r32 */
        break;
    default:
        rex(j, 1, r, x, 0); g8(j, 0x8B); modrm_sib(j, rd, RCX, ra, 0, 0);                 /* mov r64,[..] */
        if (be) { rex(j, 1, 0, 0, r); g8(j, 0x0F); g8(j, (uint8_t)(0xC8 + (rd & 7))); }   /* bswap r64 */
        break;
    }
}

/* [rcx + ra] = rv, `size` bytes. A big-endian store swaps rv in place first: the caller
 * passes r11 then, a copy. */
static void emit_fast_store(uam_jit *j, unsigned size, bool be, int rv, int ra)
{
    const int r = rv >= 8, x = ra >= 8;
    switch (size) {
    case 1:
        if (r || x || rv >= 4)                                 /* sil, dil need the prefix */
            g8(j, (uint8_t)(0x40 | (r << 2) | (x << 1)));
        g8(j, 0x88); modrm_sib(j, rv, RCX, ra, 0, 0);          /* mov [..],r8 */
        break;
    case 2:
        if (be) { g8(j, 0x66); rex(j, 0, 0, 0, r); g8(j, 0xC1); modrm_reg(j, 0, rv); g8(j, 8); } /* rol r16,8 */
        g8(j, 0x66); rex(j, 0, r, x, 0); g8(j, 0x89); modrm_sib(j, rv, RCX, ra, 0, 0);    /* mov [..],r16 */
        break;
    case 4:
        if (be) { rex(j, 0, 0, 0, r); g8(j, 0x0F); g8(j, (uint8_t)(0xC8 + (rv & 7))); }   /* bswap r32 */
        rex(j, 0, r, x, 0); g8(j, 0x89); modrm_sib(j, rv, RCX, ra, 0, 0);                 /* mov [..],r32 */
        break;
    default:
        if (be) { rex(j, 1, 0, 0, r); g8(j, 0x0F); g8(j, (uint8_t)(0xC8 + (rv & 7))); }   /* bswap r64 */
        rex(j, 1, r, x, 0); g8(j, 0x89); modrm_sib(j, rv, RCX, ra, 0, 0);                 /* mov [..],r64 */
        break;
    }
}

/* Can a store of the constant v be one instruction with an immediate? (A 64-bit one
 * takes a sign-extended 32-bit immediate; byte-swapped, only 0 and -1 stay that.) */
static bool store_imm_ok(uint64_t v, unsigned size, bool be)
{
    return size < 8 || (be ? v == 0 || v == UINT64_MAX : fits_s32(v));
}

/* [rcx + ra] = v, `size` bytes (store_imm_ok), the bytes in guest order if big-endian. */
static void emit_fast_store_imm(uam_jit *j, unsigned size, bool be, uint64_t v, int ra)
{
    const int x = ra >= 8;
    if (be && size == 2) v = __builtin_bswap16((uint16_t)v);
    if (be && size == 4) v = __builtin_bswap32((uint32_t)v);
    if (size == 2) g8(j, 0x66);
    rex(j, size == 8, 0, x, 0);
    g8(j, size == 1 ? 0xC6 : 0xC7); modrm_sib(j, 0, RCX, ra, 0, 0);                       /* mov [..],imm */
    if (size == 1) g8(j, (uint8_t)v);
    else if (size == 2) { g8(j, (uint8_t)v); g8(j, (uint8_t)(v >> 8)); }
    else g32(j, (uint32_t)v);
}

/* Emit the data-TLB probe of the access at the address in register ra (rdx, or a temp's
 * register: preserved). On a hit, rcx = host base (data at [rcx + ra]) and control falls
 * through; on a miss, jumps are taken to be patched to the bridge path. `tag_off`/
 * `base_off` select the read or write set. Returns miss-jump positions in miss[] (up to
 * 2) and their count in *n_miss.
 *
 * The entry is that of the first byte's page, and the tag compared with it the page of
 * the last byte: an access that crosses into the next page finds another tag there (an
 * entry's tag is a page of that entry's index, or none) and misses, with no test of its
 * own. An access that must be aligned (ACC_ALIGNED) cannot cross a page once it is
 * aligned, so that test is all it needs. */
static void emit_dtlb_probe(uam_jit *j, unsigned size, bool aligned, int32_t tag_off,
                            int32_t base_off, const opnd *addr, int ra, size_t *miss,
                            unsigned *n_miss)
{
    /* Uses only rax and rcx as scratch; ra and r11 (a store's value) are preserved, and
     * so is every register that holds a temp. rcx ends holding the host base. */
    *n_miss = 0;
    if (addr->kind == O_IMM) {
        /* A constant address: its entry, page and alignment are known, so the probe
         * is one compare with that entry's tag (the caller sends a page-crossing or
         * misaligned constant access straight to the bridge). */
        const uint64_t vpage = addr->imm >> UAM_PAGE_SHIFT;
        const int32_t e = 8 * (int32_t)(vpage & (UAM_DTLB_SIZE - 1));
        if (fits_s32(vpage)) {
            rex(j, 1, 0, 0, 1); g8(j, 0x81); modrm_mem(j, 7, R14, tag_off + e); g32(j, (uint32_t)vpage);
        } else {                                           /* cmp qword [tag], vpage */
            movabs(j, RAX, vpage);
            rex(j, 1, 0, 0, 1); g8(j, 0x3B); modrm_mem(j, RAX, R14, tag_off + e);
        }
        miss[(*n_miss)++] = jcc_fwd(j, 0x85);              /* jne miss */
        ld_mem(j, RCX, R14, base_off + e);                 /* rcx = host base */
        return;
    }
    if (size > 1 && !aligned) {
        lea_disp(j, 64, RAX, ra, (int32_t)size - 1);       /* rax = the last byte's address */
        rex(j, 1, 0, 0, 0); g8(j, 0xC1); modrm_reg(j, 5, RAX); g8(j, UAM_PAGE_SHIFT); /* shr rax,12 */
        mov_rr32(j, RCX, ra);                              /* ecx = addr (index bits) */
        g8(j, 0xC1); modrm_reg(j, 5, RCX); g8(j, UAM_PAGE_SHIFT);                 /* shr ecx,12 */
    } else {
        mov_rr(j, RAX, ra);                                /* rax = addr */
        rex(j, 1, 0, 0, 0); g8(j, 0xC1); modrm_reg(j, 5, RAX); g8(j, UAM_PAGE_SHIFT); /* shr rax,12 (vpage) */
        g8(j, 0x89); modrm_reg(j, RAX, RCX);               /* mov ecx, eax */
    }
    g8(j, 0x81); modrm_reg(j, 4, RCX); g32(j, UAM_DTLB_SIZE - 1);  /* and ecx, SIZE-1 (index) */
    rex(j, 1, 0, 0, 1); g8(j, 0x3B); modrm_sib(j, RAX, R14, RCX, 3, tag_off);  /* cmp rax,[r14+rcx*8+tag] */
    miss[(*n_miss)++] = jcc_fwd(j, 0x85);                  /* jne miss */
    if (size > 1 && aligned) {
        rex(j, 0, 0, 0, ra >= 8); g8(j, 0xF7); modrm_reg(j, 0, ra); g32(j, size - 1); /* test r32, size-1 */
        miss[(*n_miss)++] = jcc_fwd(j, 0x85);              /* jne miss (misaligned) */
    }
    rex(j, 1, 0, 0, 1); g8(j, 0x8B); modrm_sib(j, RCX, R14, RCX, 3, base_off);  /* mov rcx,[r14+rcx*8+base] */
}

/*
 * After a write-set miss (rdx = addr, r11 = value): the page may be one that holds
 * translated code (tag vpage | DTLB_CODE, see dtlb_fill). Hit if so, and the store
 * neither crosses the page nor (ACC_ALIGNED) is misaligned, and none of its bytes is in
 * a 64-byte line holding code (the bus's mask, read now: a write there must go through
 * the bus, which drops that code). Then rcx = host base as after emit_dtlb_probe; else
 * jumps are taken to be patched to the bridge path. Uses rax and rcx only.
 */
static void emit_code_probe(uam_jit *j, unsigned size, bool aligned, size_t *miss,
                            unsigned *n_miss)
{
    *n_miss = 0;
    mov_rr(j, RAX, RDX);                                   /* rax = addr */
    rex(j, 1, 0, 0, 0); g8(j, 0xC1); modrm_reg(j, 5, RAX); g8(j, UAM_PAGE_SHIFT); /* shr rax,12 */
    g8(j, 0x89); modrm_reg(j, RAX, RCX);                   /* mov ecx, eax */
    g8(j, 0x81); modrm_reg(j, 4, RCX); g32(j, UAM_DTLB_SIZE - 1);  /* and ecx, SIZE-1 (index) */
    rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xBA); modrm_reg(j, 5, RAX); g8(j, 62);  /* bts rax, 62 */
    rex(j, 1, 0, 0, 1); g8(j, 0x3B); modrm_sib(j, RAX, R14, RCX, 3, CPU(dtlb_wt)); /* cmp rax,[tag] */
    miss[(*n_miss)++] = jcc_fwd(j, 0x85);                  /* jne miss */
    if (size > 1) {
        g8(j, 0x89); modrm_reg(j, RDX, RAX);               /* mov eax, edx */
        g8(j, 0x25); g32(j, UAM_PAGE_SIZE - 1);            /* and eax, 0xfff */
        g8(j, 0x3D); g32(j, UAM_PAGE_SIZE - size);         /* cmp eax, 0x1000-size */
        miss[(*n_miss)++] = jcc_fwd(j, 0x87);              /* ja miss (page cross) */
        if (aligned) {
            g8(j, 0xF7); modrm_reg(j, 0, RDX); g32(j, size - 1);       /* test edx, size-1 */
            miss[(*n_miss)++] = jcc_fwd(j, 0x85);          /* jne miss (misaligned) */
        }
    }
    rex(j, 1, 0, 0, 0); g8(j, 0x8B); modrm_sib(j, RAX, RBX, RCX, 3, OFF(clines)); /* rax = lines */
    rex(j, 1, 0, 0, 0); g8(j, 0x8B); modrm_mem(j, RAX, RAX, 0);        /* rax = [rax] (mask) */
    /* the lines of the first and the last byte (bt takes the bit number mod 64) */
    g8(j, 0x89); modrm_reg(j, RDX, RCX);                   /* mov ecx, edx */
    g8(j, 0xC1); modrm_reg(j, 5, RCX); g8(j, 6);           /* shr ecx, 6 */
    rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xA3); modrm_reg(j, RCX, RAX);   /* bt rax, rcx */
    miss[(*n_miss)++] = jcc_fwd(j, 0x82);                  /* jc miss (code) */
    if (size > 1) {
        g8(j, 0x8D); modrm_mem(j, RCX, RDX, (int32_t)size - 1);         /* lea ecx, [rdx+size-1] */
        g8(j, 0xC1); modrm_reg(j, 5, RCX); g8(j, 6);       /* shr ecx, 6 */
        rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xA3); modrm_reg(j, RCX, RAX);   /* bt rax, rcx */
        miss[(*n_miss)++] = jcc_fwd(j, 0x82);              /* jc miss (code) */
    }
    mov_rr(j, RAX, RDX);
    rex(j, 1, 0, 0, 0); g8(j, 0xC1); modrm_reg(j, 5, RAX); g8(j, UAM_PAGE_SHIFT); /* shr rax,12 */
    g8(j, 0x25); g32(j, UAM_DTLB_SIZE - 1);                /* and eax, SIZE-1 */
    rex(j, 1, 0, 0, 1); g8(j, 0x8B); modrm_sib(j, RCX, R14, RAX, 3, CPU(dtlb_wb)); /* rcx = base */
}

/* Which code-page store stub serves a store of `size` bytes. */
static unsigned code_stub(unsigned size, bool be, bool aligned)
{
    unsigned s = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    return s << 2 | (unsigned)be << 1 | (unsigned)aligned;
}

/*
 * The stores to pages that hold code, shared by all blocks (called from a store's miss
 * path, so a store site costs a call and a branch): emit_code_probe, then the store.
 * Return with ZF set if the store was done, clear if it must take the bridge (rdx and
 * r11 unchanged). One stub per size, byte order and alignment rule, placed at the start
 * of the arena by uam_jit_create.
 */
void jit_host_stubs(uam_jit *j, size_t off[16])
{
    for (unsigned v = 0; v < 16; v++) {
        const unsigned size = 1u << (v >> 2);
        const bool be = v & 2, aligned = v & 1;
        while (j->n & 15)
            g8(j, 0xCC);
        off[v] = j->n;
        size_t miss[5];
        unsigned n_miss;
        emit_code_probe(j, size, aligned, miss, &n_miss);
        emit_fast_store(j, size, be, R11, RDX);
        g8(j, 0x31); modrm_reg(j, RAX, RAX);               /* xor eax, eax (ZF = 1) */
        g8(j, 0xC3);                                       /* ret */
        for (unsigned k = 0; k < n_miss; k++)
            patch(j, miss[k]);
        g8(j, 0x85); modrm_reg(j, RSP, RSP);               /* test esp, esp (ZF = 0) */
        g8(j, 0xC3);                                       /* ret */
    }
}

/* Forward jump: emit opcode(s) + rel32 placeholder; returns the patch position. */
static size_t jmp_fwd(uam_jit *j) { g8(j, 0xE9); size_t p = j->n; g32(j, 0); return p; }
static size_t jcc_fwd(uam_jit *j, uint8_t cc) { g8(j, 0x0F); g8(j, cc); size_t p = j->n; g32(j, 0); return p; }
static void patch(uam_jit *j, size_t pos)
{
    int32_t rel = (int32_t)(j->n - (pos + 4));
    if (pos + 4 <= CBUF_MAX)
        memcpy(&j->cb[pos], &rel, 4);
}

/* cpu->icount = r13 + jc->ic_base: before a bridge (it may read time) and at the
 * epilogue. Clobbers rax. */
static void ic_flush(uam_jit *j)
{
    ld_mem(j, RAX, RBX, OFF(ic_base));
    rex(j, 1, 1, 0, 0); g8(j, 0x01); modrm_reg(j, R13, RAX);       /* add rax, r13 */
    st_mem(j, RAX, R14, CPU(icount));
}
/* After a bridge, which may have asked the block to stop (uam_cpu_request_exit): take
 * cpu->icount_stop as the new jc->ic_base, r13 following. Clobbers rax. */
static void ic_rebase(uam_jit *j)
{
    rex(j, 1, 1, 0, 0); g8(j, 0x03); modrm_mem(j, R13, RBX, OFF(ic_base));   /* add r13, [ic_base] */
    ld_mem(j, RAX, R14, CPU(icount_stop));
    st_mem(j, RAX, RBX, OFF(ic_base));
    rex(j, 1, 0, 0, 1); g8(j, 0x29); modrm_reg(j, RAX, R13);       /* sub r13, rax */
}

/* Before a bridge call that may fault: jc->site = the current instruction's site
 * record (one per instruction, emitted after the code). Clobbers rax. */
static void emit_site(uam_jit *j)
{
    g8(j, 0x48); g8(j, 0x8D); g8(j, 0x05);              /* lea rax, [rip + site] */
    if (!jit_site_ref(j, j->n))
        return;
    g32(j, 0);
    st_mem(j, RAX, RBX, OFF(site));
}

/*
 * UAM_OP_COV A, B (docs/uam-isa.md section 7.5; uam_cov_count in cpu.h is the
 * reference), inline: no bridge, so chained blocks count too. cpu->cov is read at run
 * time (NULL: nothing), but its history shape (depth, rot, bits) is compiled in: it is
 * fixed while coverage is on, and uam_exec_set_coverage, the only way to change it,
 * flushes every translation. The default shape (depth <= 1, no rotation) is the
 * classic edge, about ten host instructions; deeper histories unroll with immediate
 * shifts. skip_repeat adds a compare of last_pc with the instruction's pc. Clobbers
 * rax, rcx, rdx, r11 (scratch between ops).
 */
static void emit_cov(uam_jit *j, uint64_t pc, uint32_t a, uint32_t b)
{
    const uam_cov_state *cs = j->x->cpu->cov;
    const uint32_t d = cs && cs->depth > 1 ? cs->depth : 1;
    ld_mem(j, RCX, R14, CPU(cov));                         /* rcx = cpu->cov */
    rex(j, 1, 0, 0, 0); g8(j, 0x85); modrm_reg(j, RCX, RCX); /* test rcx, rcx */
    size_t off = jcc_fwd(j, 0x84);                         /* jz skip */
    size_t rep = 0;
    if (cs && cs->skip_repeat) {
        mov_imm(j, RAX, pc);                                /* rax = pc */
        rex(j, 1, 0, 0, 0); g8(j, 0x39); modrm_mem(j, RAX, RCX, COV(last_pc)); /* cmp last_pc, rax */
        rep = jcc_fwd(j, 0x84);                            /* je skip: the same site again */
        st_mem(j, RAX, RCX, COV(last_pc));                 /* last_pc = pc */
    }
    if (d == 1 && !(cs && cs->rot[0])) {
        g8(j, 0x8B); modrm_mem(j, RAX, RCX, COV(prev));    /* eax = prev */
        g8(j, 0x35); g32(j, a);                            /* xor eax, A */
    } else {
        g8(j, 0xB8); g32(j, a);                            /* eax = A */
        for (uint32_t k = 0; k < d; k++) {
            unsigned r = cs->rot[k];
            g8(j, 0x8B); modrm_mem(j, RDX, RCX, COV(hist) + 4 * (int32_t)k); /* edx = hist[k] */
            if (r) {                                       /* rotl within bits, & mask */
                rex(j, 0, 0, 0, 1); g8(j, 0x89); modrm_reg(j, RDX, R11);   /* r11d = edx */
                g8(j, 0xC1); modrm_reg(j, 4, RDX); g8(j, (uint8_t)r);            /* shl edx, r */
                rex(j, 0, 0, 0, 1); g8(j, 0xC1); modrm_reg(j, 5, R11);
                g8(j, (uint8_t)(cs->bits - r));                                  /* shr r11d, bits-r */
                rex(j, 0, 1, 0, 0); g8(j, 0x09); modrm_reg(j, R11, RDX);   /* edx |= r11d */
                g8(j, 0x23); modrm_mem(j, RDX, RCX, COV(mask)); /* edx &= mask */
            }
            g8(j, 0x31); modrm_reg(j, RDX, RAX);           /* eax ^= edx */
        }
    }
    g8(j, 0x23); modrm_mem(j, RAX, RCX, COV(mask));        /* and eax, mask (zero-extends) */
    ld_mem(j, RDX, RCX, COV(map));                         /* rdx = map */
    g8(j, 0x80); modrm_sib(j, 0, RDX, RAX, 0, 0); g8(j, 1);  /* add byte [rdx+rax], 1 */
    g8(j, 0x80); modrm_sib(j, 3, RDX, RAX, 0, 0); g8(j, 0);  /* sbb byte [rdx+rax], 0: saturate */
    for (uint32_t k = d - 1; k > 0; k--) {                 /* hist[k] = hist[k-1] */
        g8(j, 0x8B); modrm_mem(j, RDX, RCX, COV(hist) + 4 * (int32_t)(k - 1));
        g8(j, 0x89); modrm_mem(j, RDX, RCX, COV(hist) + 4 * (int32_t)k);
    }
    g8(j, 0xC7); modrm_mem(j, 0, RCX, COV(prev)); g32(j, b); /* hist[0] = B */
    patch(j, off);
    if (rep)
        patch(j, rep);
}

/* Store a block result into jc and jump to the epilogue. */
static void set_res_pc(uam_jit *j, uint32_t kind, const uam_block *blk, uam_ref target, unsigned aw)
{
    load_operand(j, RAX, blk, target, aw);
    st_mem(j, RAX, RBX, RES(pc));
    g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, kind);  /* mov dword[jc+res.kind], kind */
}
static void set_res_pc_imm(uam_jit *j, uint32_t kind, uint64_t pc)
{
    if (fits_s32(pc)) {
        g8(j, 0x48); g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(pc)); g32(j, (uint32_t)pc); /* mov qword */
    } else {
        mov_imm(j, RAX, pc);
        st_mem(j, RAX, RBX, RES(pc));
    }
    g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, kind);
}

/*
 * An EXIT to `target` (its pc operand): jump straight into the next block if its link is
 * current and the run loop has nothing to do (jit.h); else store the block's result,
 * res = (NEXT, pc), and go to the epilogue. Only those ways out store the result: a
 * chained jump leaves none behind. A miss also records the slot in jc->link_slot so
 * uam_jit_run can link it. For a constant target the slot only ever links that pc, and
 * the path taken reads its two fields rip-relative. A computed one's slot caches its
 * first target of the epoch (compared against the pc, in rcx); any other target is
 * looked up in, and on a miss linked into, the jump table.
 */
static void emit_chain(uam_jit *j, const uam_block *blk, uam_ref target, unsigned aw,
                       size_t *epi, unsigned *n_epi)
{
    const bool fixed = uam_ref_is_imm(target);
    const uint64_t pc = fixed ? blk->imm[uam_ref_index(target)] & uam_mask(blk->addr_width) : LINK_NONE;
    size_t miss[4];
    unsigned n_miss = 0;
    j->slot_pc[j->n_slots++] = pc;
    if (!fixed)
        load_operand(j, RCX, blk, target, aw);                      /* rcx = the target pc */
    /* if (icount >= icount_stop (exit request or deadline)) goto out */
    g8(j, 0x4D); g8(j, 0x85); g8(j, 0xED);                          /* test r13, r13 */
    const size_t stop = jcc_fwd(j, 0x89);                           /* jns */
    /* if (!chain_left--) goto spent: the hop is counted here, and given back on a miss */
    g8(j, 0x83); modrm_mem(j, 5, RBX, OFF(chain_left)); g8(j, 1);   /* sub dword [chain_left], 1 */
    const size_t spent = jcc_fwd(j, 0x82);                          /* jb */
    if (fixed) {
        ld_mem(j, RAX, RBX, OFF(epoch));
        g8(j, 0x48); g8(j, 0x3B); g8(j, 0x05);                      /* cmp rax, [rip + slot.epoch] */
        jit_slot_ref(j, j->n, SLOT(epoch)); g32(j, 0);
        miss[n_miss++] = jcc_fwd(j, 0x85);
        g8(j, 0xFF); g8(j, 0x25);                                   /* jmp [rip + slot.entry] */
        jit_slot_ref(j, j->n, SLOT(entry)); g32(j, 0);
        patch(j, miss[0]);
        g8(j, 0x48); g8(j, 0x8D); g8(j, 0x05);                      /* lea rax, [rip + slot] */
        jit_slot_ref(j, j->n, 0); g32(j, 0);
    } else {
        g8(j, 0x48); g8(j, 0x8D); g8(j, 0x05);                      /* lea rax, [rip + slot] */
        jit_slot_ref(j, j->n, 0); g32(j, 0);
        ld_mem(j, RDX, RBX, OFF(epoch));
        rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RCX, RAX, SLOT(pc));       /* cmp rcx, [slot.pc] */
        size_t other = jcc_fwd(j, 0x85);
        rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RDX, RAX, SLOT(epoch));    /* cmp rdx, [slot.epoch] */
        miss[n_miss++] = jcc_fwd(j, 0x85);
        g8(j, 0xFF); modrm_mem(j, 4, RAX, SLOT(entry));             /* jmp [slot.entry] */
        /* Another target: if the slot is unused this epoch, link it there. */
        patch(j, other);
        rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RDX, RAX, SLOT(epoch));    /* cmp rdx, [slot.epoch] */
        miss[n_miss++] = jcc_fwd(j, 0x85);
        /* rax = &jtab[(uint32_t)(pc * JTAB_HASH) >> (32 - bits)] */
        g8(j, 0x69); g8(j, 0xC1); g32(j, JTAB_HASH);                    /* imul eax, ecx, JTAB_HASH */
        g8(j, 0xC1); g8(j, 0xE8); g8(j, (uint8_t)(32 - j->jtab_bits));  /* shr eax, 32 - bits */
        g8(j, 0x48); g8(j, 0x8D); g8(j, 0x04); g8(j, 0x40);             /* lea rax, [rax + rax*2] */
        g8(j, 0x48); g8(j, 0x8D); g8(j, 0x84); g8(j, 0xC3);             /* lea rax, [rbx + rax*8 + jtab] */
        g32(j, (uint32_t)OFF(jtab));
        rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RCX, RAX, SLOT(pc));       /* cmp rcx, [e.pc] */
        miss[n_miss++] = jcc_fwd(j, 0x85);
        rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RDX, RAX, SLOT(epoch));    /* cmp rdx, [e.epoch] */
        miss[n_miss++] = jcc_fwd(j, 0x85);
        g8(j, 0xFF); modrm_mem(j, 4, RAX, SLOT(entry));             /* jmp [e.entry] */
        for (unsigned k = 0; k < n_miss; k++)
            patch(j, miss[k]);
    }
    /* a miss: no hop after all; rax = the slot, or the jump table entry, to link */
    g8(j, 0xFF); modrm_mem(j, 0, RBX, OFF(chain_left));             /* inc dword [chain_left] */
    st_mem(j, RAX, RBX, OFF(link_slot));
    const size_t missed = jmp_fwd(j);
    patch(j, spent);
    g8(j, 0xC7); modrm_mem(j, 0, RBX, OFF(chain_left)); g32(j, 0);  /* chain_left = 0 again */
    patch(j, stop);
    patch(j, missed);
    /* out: res = (NEXT, pc) */
    if (fixed) {
        set_res_pc_imm(j, UAM_BR_NEXT, pc);
    } else {
        st_mem(j, RCX, RBX, RES(pc));
        g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, UAM_BR_NEXT);
    }
    epi[(*n_epi)++] = jmp_fwd(j);
}

/*
 * Pure opcodes the native compiler emits inline; any other pure op is a call to
 * jit_br_eval. Only POPCNT depends on an optional host feature.
 */
bool jit_host_pure_inline(const uam_jit *j, uam_opcode op)
{
    switch (op) {
    case UAM_OP_POPCNT:
        return j->popcnt;
    case UAM_OP_COV: case UAM_OP__COUNT:
        return false;
    default:
        return true;
    }
}

static bool op_supported(uam_opcode op)
{
    switch (op) {
    case UAM_OP_IMARK: case UAM_OP_GET: case UAM_OP_PUT: case UAM_OP_LD: case UAM_OP_ST:
    case UAM_OP_EXIT: case UAM_OP_COV:
        return true;
    case UAM_OP_SYSRD: case UAM_OP_SYSWR: case UAM_OP_TRAP: case UAM_OP_WAIT:
    case UAM_OP_HELPER: case UAM_OP_AMO: case UAM_OP_CAS: case UAM_OP_FENCE:
        return true;
    default:
        break;
    }
    /* pure ops (inline or via jit_br_eval) */
    const uam_opinfo *oi = &uam_opinfo_table[op];
    return !(oi->props & (UAM_P_EFFECT | UAM_P_GUARD)) && op != UAM_OP_GET;
}

/* The register of the pool an operand is in, or -1. */
static int reg_of_ref(const uam_jit *j, uam_ref r)
{
    return uam_ref_is_none(r) || uam_ref_is_imm(r) ? -1 : j->reg_of[r];
}

/* The registers an insn's result is best left in, if they are free for it (jit_ra_dst):
 * where an operand is that a two-operand instruction can then work on in place. */
static void dst_wants(const uam_jit *j, const uam_insn *in, int *w0, int *w1)
{
    switch ((uam_opcode)in->op) {
    case UAM_OP_ADD: case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR: case UAM_OP_MUL:
        *w0 = reg_of_ref(j, in->a);
        *w1 = reg_of_ref(j, in->b);
        break;
    case UAM_OP_SELECT:
        *w0 = reg_of_ref(j, in->c);
        *w1 = reg_of_ref(j, in->b);
        break;
    default:
        *w0 = reg_of_ref(j, in->a);
        *w1 = -1;
        break;
    }
}

/* Emit a pure op; its W-bit result is left in rd: rax, or a register of the pool, which
 * may be that of an operand read here for the last time (jit_ra_dst) - so every template
 * writes rd only after it has read the operands it still needs, or computes in rax and
 * moves the result at the end. A fused compare (j->fuse) leaves only the flags. */
static void emit_pure(uam_jit *j, const uam_block *blk, const uam_insn *in, int rd)
{
    const uam_opcode op = (uam_opcode)in->op;
    const unsigned W = in->width;
    const unsigned n_src = uam_opinfo_table[op].n_src;
    unsigned src_w = W;
    if (op == UAM_OP_ZEXT || op == UAM_OP_SEXT || op == UAM_OP_TRUNC)
        src_w = uam_ref_is_imm(in->a) ? W : blk->temp_width[in->a];

    if (!jit_host_pure_inline(j, op)) {
        /* call jit_br_eval(jc, packed, a, b, c); r8 (a temp register) is written last,
         * after its own value is saved */
        jit_ra_save(j, false);
        load_operand(j, RDX, blk, in->a, op == UAM_OP_SELECT ? 1 : src_w);
        if (n_src > 1) load_operand(j, RCX, blk, in->b, W); else mov_imm(j, RCX, 0);
        if (n_src > 2) load_operand(j, R8, blk, in->c, W); else mov_imm(j, R8, 0);
        mov_rr(j, RDI, RBX);
        mov_imm(j, RSI, (uint32_t)op | (W << 8) | (src_w << 16));
        call_bridge(j, jit_br_eval);
        jit_ra_save(j, true);
        zext_rr(j, rd, RAX, W);
        return;
    }

    /* Natural operand size: 32-bit operations zero-extend their result, so only
     * narrower widths need masking after an add, sub or multiply. AND/OR/XOR of
     * zero-extended values never need it. */
    const unsigned os = W == 64 ? 64 : 32;
    switch (op) {
    case UAM_OP_ADD: case UAM_OP_SUB: case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR: {
        static const int alu_op[] = { [UAM_OP_ADD] = ALU_ADD, [UAM_OP_SUB] = ALU_SUB,
                                      [UAM_OP_AND] = ALU_AND, [UAM_OP_OR] = ALU_OR,
                                      [UAM_OP_XOR] = ALU_XOR };
        const opnd a = opnd_of(j, blk, in->a, W), b = opnd_of(j, blk, in->b, W);
        const bool lea = op == UAM_OP_ADD && W >= 32 && a.kind == O_REG;
        if (a.kind == O_REG && a.reg == rd) {
            alu(j, alu_op[op], os, rd, b);                   /* in place */
        } else if (b.kind == O_REG && b.reg == rd) {
            if (op == UAM_OP_SUB) {                          /* rd holds the subtrahend */
                load(j, RAX, a, W);
                alu(j, ALU_SUB, os, RAX, b);
                load(j, rd, (opnd){ O_REG, 0, 0, RAX }, W);
            } else {
                alu(j, alu_op[op], os, rd, a);               /* commutative: in place */
            }
        } else if (lea && b.kind == O_REG) {
            lea_index(j, os, rd, a.reg, b.reg);
        } else if (lea && b.kind == O_IMM && (os == 32 || fits_s32(b.imm))) {
            lea_disp(j, os, rd, a.reg, (int32_t)b.imm);
        } else {
            load(j, rd, a, W);
            alu(j, alu_op[op], os, rd, b);
        }
        if ((op == UAM_OP_ADD || op == UAM_OP_SUB) && W < 32)
            mask_reg(j, rd, W);
        return;
    }
    case UAM_OP_MUL: {
        const opnd a = opnd_of(j, blk, in->a, W), b = opnd_of(j, blk, in->b, W);
        if (b.kind == O_REG && b.reg == rd && !(a.kind == O_REG && a.reg == rd)) {
            imul(j, os, rd, a);                              /* commutative: in place */
        } else {
            load(j, rd, a, W);
            imul(j, os, rd, b);
        }
        if (W < 32)
            mask_reg(j, rd, W);
        return;
    }
    case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_ULT: case UAM_OP_ULE:
    case UAM_OP_SLT: case UAM_OP_SLE: {
        /* compare at width W itself: the signed conditions then see W-bit values */
        const opnd a = opnd_of(j, blk, in->a, W);
        int ra = RAX;
        if (a.kind == O_REG)
            ra = a.reg;
        else
            load(j, RAX, a, W);
        alu(j, ALU_CMP, W == 1 ? 8 : W, ra, opnd_of(j, blk, in->b, W));
        uint8_t cc = op == UAM_OP_EQ ? 0x4 : op == UAM_OP_NE ? 0x5 : op == UAM_OP_ULT ? 0x2
                   : op == UAM_OP_ULE ? 0x6 : op == UAM_OP_SLT ? 0xC : 0xE;
        if (j->fuse)
            j->flag_cc = cc;                                       /* the reader tests the flags */
        else
            setcc(j, rd, cc);
        return;
    }
    case UAM_OP_ZEXT:   /* the source temp is already zero-extended */
        load_operand(j, rd, blk, in->a, src_w);
        return;
    case UAM_OP_TRUNC: {
        const opnd a = opnd_of(j, blk, in->a, src_w);
        if (a.kind == O_REG) {
            zext_rr(j, rd, a.reg, W);
        } else {
            load(j, rd, a, src_w);
            mask_reg(j, rd, W);
        }
        return;
    }
    case UAM_OP_SEXT: {
        const opnd a = opnd_of(j, blk, in->a, src_w);
        if (src_w == 1 || a.kind == O_IMM) {
            load(j, rd, a, src_w);
            sext_reg(j, rd, src_w);
        } else {                                       /* movsx/movsxd rd, reg / [rbp+d] */
            rex(j, 1, rd >= 8, 0, a.kind == O_REG && a.reg >= 8);
            if (src_w == 32) g8(j, 0x63);
            else { g8(j, 0x0F); g8(j, src_w == 8 ? 0xBE : 0xBF); }
            if (a.kind == O_REG) modrm_reg(j, rd, a.reg);
            else                 modrm_mem(j, rd, RBP, a.disp);
        }
        mask_reg(j, rd, W);
        return;
    }
    case UAM_OP_SELECT: {
        const opnd c = opnd_of(j, blk, in->a, 1);
        opnd x = opnd_of(j, blk, in->b, W);
        opnd y = opnd_of(j, blk, in->c, W);
        if (c.kind == O_IMM) {
            load(j, rd, c.imm ? x : y, W);
            return;
        }
        /* The condition first (rd may be its register); the moves leave the flags. */
        uint8_t cc = 0x5;                                            /* ne: cond != 0 */
        if (in->a == j->flag_temp) {
            cc = j->flag_cc;                                         /* cond is in the flags */
            j->flag_temp = UAM_REF_NONE;
        } else {
            test_bool(j, c);
        }
        if (x.kind == O_REG && x.reg == rd) {                        /* rd = x: take y if false */
            const opnd t = x;
            x = y;
            y = t;
            cc ^= 1;
        }
        load(j, rd, y, W);                                           /* the default */
        if (x.kind == O_IMM) {
            mov_imm(j, RCX, x.imm);
            x = (opnd){ O_REG, 0, 0, RCX };
        }
        rex(j, 1, rd >= 8, 0, x.kind == O_REG && x.reg >= 8);
        g8(j, 0x0F); g8(j, 0x40 | cc);                               /* cmovcc rd, x */
        if (x.kind == O_REG) modrm_reg(j, rd, x.reg);
        else                 modrm_mem(j, rd, RBP, x.disp);
        return;
    }
    case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR: {
        if (!uam_ref_is_imm(in->b)) {
            /* Variable amount y (a W-bit value, so possibly >= 64): shift by cl (the
             * host uses cl & 63), then fix up y >= W. For SHL/LSHR an amount in
             * [W, 64) already gives 0 in the low W bits; beyond, select 0. ASHR clamps
             * the amount to 63, which fills with the sign for any y >= W - 1. */
            load_operand(j, RAX, blk, in->a, W);
            load_operand(j, RCX, blk, in->b, W);
            if (op == UAM_OP_ASHR) {
                sext_reg(j, RAX, W);
                g8(j, 0xBA); g32(j, 63);                                  /* mov edx, 63 */
                g8(j, 0x48); g8(j, 0x83); g8(j, 0xF9); g8(j, 63);         /* cmp rcx, 63 */
                g8(j, 0x48); g8(j, 0x0F); g8(j, 0x47); g8(j, 0xCA);       /* cmova rcx, rdx */
                g8(j, 0x48); g8(j, 0xD3); g8(j, 0xF8);                    /* sar rax, cl */
            } else {
                g8(j, 0x48); g8(j, 0xD3); g8(j, op == UAM_OP_SHL ? 0xE0 : 0xE8); /* shl/shr rax, cl */
                g8(j, 0x31); g8(j, 0xD2);                                 /* xor edx, edx */
                g8(j, 0x48); g8(j, 0x83); g8(j, 0xF9); g8(j, (uint8_t)W); /* cmp rcx, W */
                g8(j, 0x48); g8(j, 0x0F); g8(j, 0x43); g8(j, 0xC2);       /* cmovae rax, rdx */
            }
            mask_reg(j, RAX, W);
                break;
        }
        uint64_t n = blk->imm[uam_ref_index(in->b)];   /* any amount >= W shifts everything out */
        if (n >= W) {
            if (op == UAM_OP_ASHR) {
                load_operand(j, rd, blk, in->a, W);
                sext_reg(j, rd, W);
                shift_imm(j, SH_SAR, 64, rd, 63);
                mask_reg(j, rd, W);
            } else {
                mov_imm(j, rd, 0);
            }
            return;
        }
        load_operand(j, rd, blk, in->a, W);
        if (op == UAM_OP_SHL) {
            shift_imm(j, SH_SHL, os, rd, (unsigned)n);
            if (W < 32)
                mask_reg(j, rd, W);
        } else if (op == UAM_OP_LSHR) {
            shift_imm(j, SH_SHR, os, rd, (unsigned)n);      /* zero-extended: nothing to mask */
        } else if (W >= 32) {
            shift_imm(j, SH_SAR, os, rd, (unsigned)n);      /* sar r32 zero-extends the result */
        } else {
            rex_rb(j, rd, rd, W == 8);
            g8(j, 0x0F); g8(j, W == 8 ? 0xBE : 0xBF); modrm_reg(j, rd, rd);   /* movsx r32, r8/r16 */
            shift_imm(j, SH_SAR, 32, rd, (unsigned)n);
            mask_reg(j, rd, W);
        }
        return;
    }
    case UAM_OP_CARRY: case UAM_OP_SCARRY: case UAM_OP_SBORROW: {
        /* The W-bit add/sub sets the host's CF/OF exactly as section 7.2 defines
         * carry and signed overflow (operands are zero-extended W-bit values). */
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        const bool sub = op == UAM_OP_SBORROW;
        if (W == 16) g8(j, 0x66);
        if (W == 64) g8(j, 0x48);
        g8(j, (uint8_t)((sub ? 0x28 : 0x00) | (W == 8 ? 0 : 1)));    /* add/sub al|ax|eax|rax, cl.. */
        modrm_reg(j, RCX, RAX);
        const uint8_t cc = op == UAM_OP_CARRY ? 0x2 : 0x0;             /* c / o */
        if (j->fuse)
            j->flag_cc = cc;
        else
            setcc(j, rd, cc);                                          /* setc/seto */
        return;
    }
    case UAM_OP_POPCNT:
        load_operand(j, RAX, blk, in->a, W);
        g8(j, 0xF3); g8(j, 0x48); g8(j, 0x0F); g8(j, 0xB8); modrm_reg(j, RAX, RAX); /* popcnt rax, rax */
        break;
    case UAM_OP_CLZ:
        load_operand(j, RAX, blk, in->a, W);
        if (W < 64) {
            /* bsr of 2x+1 is (index of x's top bit) + 1, or 0 for x = 0: clz = W - that */
            g8(j, 0x48); g8(j, 0x8D); g8(j, 0x44); g8(j, 0x00); g8(j, 0x01); /* lea rax, [rax+rax+1] */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBD); modrm_reg(j, RAX, RAX);    /* bsr rax, rax */
            g8(j, 0x48); g8(j, 0xF7); g8(j, 0xD8);                            /* neg rax */
            g8(j, 0x48); g8(j, 0x83); g8(j, 0xC0); g8(j, (uint8_t)W);         /* add rax, W */
        } else {
            g8(j, 0x48); g8(j, 0xC7); g8(j, 0xC1); g32(j, 0xFFFFFFFFu);       /* mov rcx, -1 */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBD); modrm_reg(j, RAX, RAX);    /* bsr rax, rax */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0x44); g8(j, 0xC1);               /* cmovz rax, rcx */
            g8(j, 0x48); g8(j, 0xF7); g8(j, 0xD8);                            /* neg rax */
            g8(j, 0x48); g8(j, 0x83); g8(j, 0xC0); g8(j, 63);                 /* add rax, 63 */
        }
        break;
    case UAM_OP_CTZ:
        load_operand(j, RAX, blk, in->a, W);
        if (W < 64) {
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBA); g8(j, 0xE8); g8(j, (uint8_t)W); /* bts rax, W */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBC); modrm_reg(j, RAX, RAX);    /* bsf rax, rax */
        } else {
            g8(j, 0xB9); g32(j, 64);                                          /* mov ecx, 64 */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBC); modrm_reg(j, RAX, RAX);    /* bsf rax, rax */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0x44); g8(j, 0xC1);               /* cmovz rax, rcx */
        }
        break;
    case UAM_OP_BSWAP:
        load_operand(j, RAX, blk, in->a, W);
        if (W == 16) { g8(j, 0x66); g8(j, 0xC1); modrm_reg(j, 0, RAX); g8(j, 8); } /* rol ax, 8 */
        else if (W == 32) { g8(j, 0x0F); g8(j, 0xC8); }                     /* bswap eax */
        else { g8(j, 0x48); g8(j, 0x0F); g8(j, 0xC8); }                     /* bswap rax */
        break;
    case UAM_OP_ROTR: {
        /* ror at width W: the host rotates by (count & 31 or 63) mod W, which is
         * the amount mod W for every W, as section 7.1 defines */
        load_operand(j, RAX, blk, in->a, W);
        const bool imm = uam_ref_is_imm(in->b);
        if (!imm)
            load_operand(j, RCX, blk, in->b, W);
        if (W == 16) g8(j, 0x66);
        if (W == 64) g8(j, 0x48);
        if (imm) {
            g8(j, W == 8 ? 0xC0 : 0xC1); modrm_reg(j, 1, RAX);
            g8(j, (uint8_t)(blk->imm[uam_ref_index(in->b)] % W));         /* ror r, imm8 */
        } else {
            g8(j, W == 8 ? 0xD2 : 0xD3); modrm_reg(j, 1, RAX);            /* ror r, cl */
        }
        mask_reg(j, RAX, W);
        break;
    }
    case UAM_OP_MULHU: case UAM_OP_MULHS:
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        if (W < 64) {
            /* the 2W-bit product fits in 64 bits */
            if (op == UAM_OP_MULHS) { sext_reg(j, RAX, W); sext_reg(j, RCX, W); }
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xAF); modrm_reg(j, RAX, RCX);    /* imul rax, rcx */
            g8(j, 0x48); g8(j, 0xC1); modrm_reg(j, op == UAM_OP_MULHS ? 7 : 5, RAX);
            g8(j, (uint8_t)W);                                                /* sar/shr rax, W */
            mask_reg(j, RAX, W);
        } else {
            g8(j, 0x48); g8(j, 0xF7); modrm_reg(j, op == UAM_OP_MULHS ? 5 : 4, RCX); /* imul/mul rcx */
            mov_rr(j, RAX, RDX);                                              /* rax = high half */
        }
        break;
    case UAM_OP_UDIV: case UAM_OP_UREM: case UAM_OP_SDIV: case UAM_OP_SREM: {
        /* Section 7.1: x / 0 = all ones, x % 0 = x; SMIN / -1 = SMIN, SMIN % -1 = 0.
         * Signed ops divide the sign-extended operands at 64 bits, where only
         * W = 64 can overflow (y = -1: the quotient is -x mod 2^64, the remainder 0). */
        const bool sgn = op == UAM_OP_SDIV || op == UAM_OP_SREM;
        const bool rem = op == UAM_OP_UREM || op == UAM_OP_SREM;
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        if (sgn) { sext_reg(j, RAX, W); sext_reg(j, RCX, W); }
        g8(j, 0x48); g8(j, 0x85); modrm_reg(j, RCX, RCX);                    /* test rcx, rcx */
        size_t by_zero = jcc_fwd(j, 0x84);                                    /* jz */
        size_t by_m1 = 0;
        if (sgn && W == 64) {
            g8(j, 0x48); g8(j, 0x83); g8(j, 0xF9); g8(j, 0xFF);               /* cmp rcx, -1 */
            by_m1 = jcc_fwd(j, 0x84);                                         /* je */
        }
        if (sgn) {
            g8(j, 0x48); g8(j, 0x99);                                         /* cqo */
            g8(j, 0x48); g8(j, 0xF7); modrm_reg(j, 7, RCX);                   /* idiv rcx */
        } else {
            g8(j, 0x31); g8(j, 0xD2);                                         /* xor edx, edx */
            if (W == 64) g8(j, 0x48);
            g8(j, 0xF7); modrm_reg(j, 6, RCX);                                /* div ecx / rcx */
        }
        if (rem)
            mov_rr(j, RAX, RDX);
        size_t done = jmp_fwd(j);
        patch(j, by_zero);
        if (!rem)
            mov_imm(j, RAX, uam_mask(W));                                      /* x / 0 */
        size_t done0 = rem ? 0 : jmp_fwd(j);                                  /* x % 0 = x: rax */
        if (by_m1) {
            if (rem) { done0 = jmp_fwd(j); }
            patch(j, by_m1);
            if (rem) { g8(j, 0x31); g8(j, 0xC0); }                            /* xor eax, eax */
            else { g8(j, 0x48); g8(j, 0xF7); g8(j, 0xD8); }                   /* neg rax */
        }
        patch(j, done);
        if (done0)
            patch(j, done0);
        mask_reg(j, RAX, W);
        break;
    }
    default:
        break;
    }
    /* computed in rax */
    if (rd != RAX)
        load(j, rd, (opnd){ O_REG, 0, 0, RAX }, W);
}

/* mov [r15 + aux], v (width w) */
static void emit_put(uam_jit *j, uint32_t aux, unsigned w, opnd v)
{
    if (v.kind == O_IMM && (w < 64 || fits_s32(v.imm))) {
        if (w == 16) g8(j, 0x66);
        rex(j, w == 64, 0, 0, 1);
        g8(j, w == 8 ? 0xC6 : 0xC7);                     /* mov [r15+off], imm */
        modrm_mem(j, 0, R15, (int32_t)aux);
        if (w == 8) g8(j, (uint8_t)v.imm);
        else if (w == 16) { g8(j, (uint8_t)v.imm); g8(j, (uint8_t)(v.imm >> 8)); }
        else g32(j, (uint32_t)v.imm);
        return;
    }
    /* store straight from the temp's register (the REX prefix r15 needs also makes
     * sil/dil addressable as bytes) */
    int src = RAX;
    if (v.kind == O_REG)
        src = v.reg;
    else
        load(j, RAX, v, w);
    if (w == 16) g8(j, 0x66);
    rex(j, w == 64, src >= 8, 0, 1);
    g8(j, w == 8 ? 0x88 : 0x89);                         /* mov [r15+off], src */
    modrm_mem(j, src, R15, (int32_t)aux);
}

/* Replay, here, what soft point i needs: compute its cold ops into their slots (in
 * program order, so each finds its cold operands there), then do its pending writes,
 * each under its guard. The temps it reads are where opnd_of says (an IMARK stub sets
 * that up). Uses rax, rcx and rdx (the cold ops) and the flags; returns whether rcx and
 * rdx were touched. `back` is the region kind to resume in the op map. */
static bool emit_replays(uam_jit *j, const uam_block *blk, uint32_t i, unsigned back)
{
    if (j->obs_start[i] == j->obs_start[i + 1])
        return false;
    jit_mark(j, MK_REPLAY, i);
    const bool fuse = j->fuse;
    const uam_ref ft = j->flag_temp;
    j->fuse = false;                   /* every result to rax, nothing held in the flags */
    j->flag_temp = UAM_REF_NONE;
    for (uint32_t e = j->clo_start[i]; e < j->clo_start[i + 1]; e++) {
        const uam_insn *c = &blk->insn[j->clo_list[e]];
        emit_pure(j, blk, c, RAX);
        st_temp(j, c->dst, RAX);
    }
    j->fuse = fuse;
    j->flag_temp = ft;
    for (uint32_t e = j->obs_start[i]; e < j->obs_start[i + 1]; e++) {
        const uam_insn *d = &blk->insn[j->obs_list[e]];
        size_t skip = 0;
        if (jit_guard_temp(d)) {
            test_bool(j, opnd_of(j, blk, d->guard, 1));
            skip = jcc_fwd(j, 0x84);                         /* je: guard false */
        }
        emit_put(j, d->aux, d->width, opnd_of(j, blk, d->a, d->width));
        if (skip)
            patch(j, skip);
    }
    jit_mark(j, back, i);
    return j->clo_start[i] != j->clo_start[i + 1];
}

bool jit_host_compile(uam_jit *j, const uam_block *blk)
{
    const unsigned aw = blk->addr_width;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        uam_opcode op = (uam_opcode)blk->insn[i].op;
        if (!op_supported(op))
            return false;
    }

    /* prologue */
    jit_mark(j, MK_PROLOGUE, 0);
    g8(j, 0x53);                                   /* push rbx */
    g8(j, 0x55);                                   /* push rbp */
    g8(j, 0x41); g8(j, 0x54);                      /* push r12 */
    g8(j, 0x41); g8(j, 0x55);                      /* push r13 */
    g8(j, 0x41); g8(j, 0x56);                      /* push r14 */
    g8(j, 0x41); g8(j, 0x57);                      /* push r15 */
    g8(j, 0x48); g8(j, 0x83); g8(j, 0xEC); g8(j, 0x08);  /* sub rsp, 8 (align) */
    mov_rr(j, RBX, RDI);                           /* rbx = jc */
    ld_mem(j, RBP, RBX, OFF(temps));
    ld_mem(j, R14, RBX, OFF(cpu));
    ld_mem(j, R15, RBX, OFF(regfile));
    ld_mem(j, R13, R14, CPU(icount));              /* r13 = icount - ic_base (kept in a register) */
    rex(j, 1, 1, 0, 0); g8(j, 0x2B); modrm_mem(j, R13, RBX, OFF(ic_base));
    j->body_off = j->n;                            /* chained jumps enter here */

    /* epilogue jumps: at most 1 per op */
    size_t *epi = malloc(sizeof(size_t) * (blk->n_insn + 1));
    if (!epi || !jit_defer_analyze(j, blk) || !jit_ra_begin(j, blk)) {
        free(epi);
        return false;
    }
    unsigned n_epi = 0;
    bool first = true;
    uint64_t imark_pc = blk->guest_pc;   /* the pc of the instruction being emitted */

    for (uint32_t i = 0; i < blk->n_insn && !j->overflow; i++) {
        const uam_insn *in = &blk->insn[i];
        const uam_opcode op = (uam_opcode)in->op;
        const unsigned W = in->width;
        const bool has_dst = !uam_ref_is_none(in->dst);
        j->cur_i = i;
        if (j->dfr[i] || j->cold[i])
            continue;                          /* replayed on the cold paths, if at all */
        j->fuse = jit_fusable(j, blk, i);
        jit_mark(j, MK_OP, i);
        /* where the result goes: picked now, as it may spill a register */
        int rd = RAX;
        if (has_dst && !j->fuse) {
            int w0, w1;
            dst_wants(j, in, &w0, &w1);
            rd = jit_ra_dst(j, in->dst, w0, w1);
        }

        /* Guard on an effectful op. */
        size_t guard_skip = 0;
        bool guarded = false;
        if (!uam_ref_is_none(in->guard)) {
            if (uam_ref_is_imm(in->guard)) {
                if (!(blk->imm[uam_ref_index(in->guard)] & 1)) {
                    jit_ra_free_dead(j, blk);  /* guard false: op does nothing, dst = 0 */
                    if (has_dst) {
                        mov_imm(j, rd, 0);
                        jit_ra_def(j, in->dst, rd);
                    }
                    continue;
                }
                /* guard true: fall through unguarded */
            } else {
                if (in->guard == j->flag_temp) {  /* the guard is in the flags */
                    guard_skip = jcc_fwd(j, (uint8_t)(0x80 | (j->flag_cc ^ 1)));   /* j!cc skip */
                    j->flag_temp = UAM_REF_NONE;
                } else {
                    test_bool(j, opnd_of(j, blk, in->guard, 1));
                    guard_skip = jcc_fwd(j, 0x84);    /* je skip */
                }
                guarded = true;
            }
        }

        switch (op) {
        case UAM_OP_IMARK: {
            uint64_t pc = blk->imm[uam_ref_index(in->a)];
            bool cont = in->flags & UAM_IMARK_CONT;
            imark_pc = pc;
            const bool brk = in->flags & UAM_IMARK_BREAK;
            const bool check = !first && !cont && !brk;
            if (!first && !cont && brk) {
                set_res_pc_imm(j, UAM_BR_INTERRUPTED, pc);
                epi[n_epi++] = jmp_fwd(j);
            }
            first = false;
            /* A continuation keeps the parent of the instruction it continues. */
            j->cur.parent_pc = cont ? j->cur.parent_pc : pc;
            j->cur.pc = pc;
            j->cur.cont = cont;
            j->cur_emitted = false;
            jit_mark(j, MK_IMARK_PC, i);
            if (j->flag_temp != UAM_REF_NONE) {   /* keep the flags (a CONT imark: no check) */
                g8(j, 0x4D); g8(j, 0x8D); g8(j, 0x6D); g8(j, 0x01);  /* lea r13, [r13+1] */
            } else {
                g8(j, 0x49); g8(j, 0xFF); modrm_reg(j, 0, R13);      /* inc r13 (icount) */
            }
            /* if (icount >= icount_stop) stop here, before this instruction: r13 was not
             * negative, so it is positive now. An out-of-line stub, which takes the count
             * back and replays the deferred writes still pending. */
            if (check)
                jit_stub_add(j, blk, jcc_fwd(j, 0x8F), pc, i);       /* jg */
            break;
        }
        case UAM_OP_GET:
            if (W == 1) {
                mov_imm(j, rd, 0);
            } else {
                rex(j, W == 64, rd >= 8, 0, 1);
                if (W == 8) { g8(j, 0x0F); g8(j, 0xB6); }            /* movzx r32, byte [r15+aux] */
                else if (W == 16) { g8(j, 0x0F); g8(j, 0xB7); }      /* movzx r32, word */
                else g8(j, 0x8B);                                    /* mov r32 / r64 */
                modrm_mem(j, rd, R15, (int32_t)in->aux);
            }
            break;
        case UAM_OP_PUT:
            emit_put(j, in->aux, W, opnd_of(j, blk, in->a, W));
            break;
        case UAM_OP_LD: {
            unsigned size = W / 8;
            bool be = in->flags & UAM_ACC_BE;
            bool fast = !(in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV));
            const opnd ao = opnd_of(j, blk, in->a, aw);
            fast = fast && !(ao.kind == O_IMM && jit_const_slow(ao.imm, size, in->flags));
            /* the address: in its own register on the inline path, in rdx for the bridge */
            const int ra = fast && ao.kind == O_REG ? ao.reg : RDX;
            if (ra == RDX)
                load(j, RDX, ao, aw);
            size_t done = 0, miss[2]; unsigned n_miss = 0;
            if (fast) {
                emit_dtlb_probe(j, size, in->flags & UAM_ACC_ALIGNED, CPU(dtlb_rt), CPU(dtlb_rb),
                                &ao, ra, miss, &n_miss);
                emit_fast_load(j, size, be, rd, ra);    /* rd = value: its last instruction,
                                                         * as rd may be the address's register */
                done = jmp_fwd(j);
                for (unsigned k = 0; k < n_miss; k++)
                    patch(j, miss[k]);
            }
            jit_mark(j, MK_SLOW, i);
            ic_flush(j);                                /* the bridge may read time */
            if (emit_replays(j, blk, i, MK_SLOW) || ra != RDX)
                load(j, RDX, ao, aw);                   /* rdx = addr (again) */
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, RSI, RDX);                        /* miss / slow path: the bridge */
            mov_rr(j, RDI, RBX);
            mov_imm(j, RDX, in->flags);
            mov_imm(j, RCX, size);
            call_bridge(j, jit_br_load);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);            /* jnz epilogue (fault) */
            ic_rebase(j);
            jit_ra_save(j, true);
            ld_mem(j, rd, RBX, OFF(scratch));
            if (fast)
                patch(j, done);
            break;
        }
        case UAM_OP_ST:
            if (in->flags & UAM_ACC_X) {               /* store-conditional: always the bridge */
                load_operand(j, R11, blk, in->b, W);
                load_operand(j, RDX, blk, in->a, aw);
                ic_flush(j);
                emit_site(j);
                jit_ra_save(j, false);
                mov_rr(j, RSI, RDX);
                mov_rr(j, RDX, R11);
                mov_rr(j, RDI, RBX);
                mov_imm(j, RCX, in->flags);
                mov_imm(j, R8, W / 8);
                call_bridge(j, jit_br_store_x);
                g8(j, 0x85); modrm_reg(j, RAX, RAX);
                epi[n_epi++] = jcc_fwd(j, 0x85);
            ic_rebase(j);
                ic_rebase(j);
                jit_ra_save(j, true);
                if (has_dst)
                    ld_mem(j, rd, RBX, OFF(scratch));
                break;
            }
            {
            unsigned size = W / 8;
            bool be = in->flags & UAM_ACC_BE;
            bool fast = !(in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV));
            const opnd vo = opnd_of(j, blk, in->b, W);
            const opnd ao = opnd_of(j, blk, in->a, aw);
            fast = fast && !(ao.kind == O_IMM && jit_const_slow(ao.imm, size, in->flags));
            /* On the inline path the address and the value stay where they are: a
             * temp's register, an immediate in the store itself. A big-endian store of a
             * register swaps a copy, in r11. The stubs and the bridge take them in rdx
             * and r11. */
            const bool swap = be && size > 1;
            const bool vimm = fast && vo.kind == O_IMM && store_imm_ok(vo.imm, size, be);
            const int ra = fast && ao.kind == O_REG ? ao.reg : RDX;
            const int rv = fast && vo.kind == O_REG && !swap ? vo.reg : R11;
            if (rv == R11 && !vimm)
                load(j, R11, vo, W);
            if (ra == RDX)
                load(j, RDX, ao, aw);
            size_t done = 0, done_code = 0, miss[2]; unsigned n_miss = 0;
            if (fast) {
                emit_dtlb_probe(j, size, in->flags & UAM_ACC_ALIGNED, CPU(dtlb_wt), CPU(dtlb_wb),
                                &ao, ra, miss, &n_miss);
                if (vimm)
                    emit_fast_store_imm(j, size, be, vo.imm, ra);
                else
                    emit_fast_store(j, size, be, rv, ra);
                done = jmp_fwd(j);
                for (unsigned k = 0; k < n_miss; k++)
                    patch(j, miss[k]);
                /* not in the write set: maybe a page that holds code, away from it */
                jit_mark(j, MK_CODE_ST, i);
                if (rv != R11 || vimm)
                    load(j, R11, vo, W);
                if (ra != RDX)
                    load(j, RDX, ao, aw);
                g8(j, 0xFF); modrm_mem(j, 2, RBX, OFF(cstub)       /* call [jc.cstub[v]] */
                                       + 8 * (int32_t)code_stub(size, be, in->flags & UAM_ACC_ALIGNED));
                done_code = jcc_fwd(j, 0x84);                      /* je done */
            }
            jit_mark(j, MK_SLOW, i);
            ic_flush(j);
            if (emit_replays(j, blk, i, MK_SLOW))       /* r11 (the value) is kept */
                load(j, RDX, ao, aw);                   /* rdx = addr again */
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, RSI, RDX);                        /* r11 still holds the unswapped value */
            mov_rr(j, RDX, R11);
            mov_rr(j, RDI, RBX);
            mov_imm(j, RCX, in->flags);
            mov_imm(j, R8, size);
            call_bridge(j, jit_br_store);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            ic_rebase(j);
            jit_ra_save(j, true);
            if (fast) {
                patch(j, done);
                patch(j, done_code);
            }
            break;
            }
        case UAM_OP_AMO:
        case UAM_OP_CAS: {
            const bool cas = op == UAM_OP_CAS;
            load_operand(j, RDX, blk, in->b, W);        /* operand / expected */
            if (cas)
                load_operand(j, RCX, blk, in->c, W);    /* new value */
            else
                mov_imm(j, RCX, 0);
            load_operand(j, R11, blk, in->a, aw);       /* addr */
            ic_flush(j);
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, RSI, R11);
            mov_imm(j, R8, in->flags);
            mov_imm(j, R9, (uint32_t)op | (cas ? 0 : (uint32_t)in->aux << 8) | (uint32_t)W << 16);
            mov_rr(j, RDI, RBX);
            call_bridge(j, jit_br_rmw);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            ic_rebase(j);
            jit_ra_save(j, true);
            if (has_dst)
                ld_mem(j, rd, RBX, OFF(scratch));
            break;
        }
        case UAM_OP_FENCE:
            /* Ordering is a no-op: a hart's blocks run one at a time and harts are
             * interleaved only between blocks. FENCE.INSN needs nothing either: code
             * writes invalidate translations and the block ends here (see interp.c). */
            break;
        case UAM_OP_EXIT:
            jit_mark(j, MK_CHAIN, i);
            if (j->n_slots < MAX_SLOTS) {
                emit_chain(j, blk, in->a, aw, epi, &n_epi);
            } else {
                set_res_pc(j, UAM_BR_NEXT, blk, in->a, aw);
                epi[n_epi++] = jmp_fwd(j);
            }
            break;
        case UAM_OP_SYSRD:
            ic_flush(j);
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, RDI, RBX);
            mov_imm(j, RSI, in->aux);
            call_bridge(j, jit_br_sysrd);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);          /* jnz epilogue (fault) */
            ic_rebase(j);
            jit_ra_save(j, true);
            ld_mem(j, rd, RBX, OFF(scratch));
            mask_reg(j, rd, W);
            break;
        case UAM_OP_SYSWR:
            load_operand(j, RDX, blk, in->a, W);
            ic_flush(j);
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, RDI, RBX);
            mov_imm(j, RSI, in->aux);
            call_bridge(j, jit_br_syswr);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            ic_rebase(j);
            jit_ra_save(j, true);
            break;
        case UAM_OP_HELPER: {
            /* the arguments go to rdx, rcx, r8: r8 (a temp register) is written last,
             * after the live registers are saved */
            const uam_ref args[3] = { in->a, in->b, in->c };
            const int regs[3] = { RDX, RCX, R8 };
            ic_flush(j);
            emit_site(j);
            jit_ra_save(j, false);
            for (unsigned k = 0; k < 3; k++) {
                if (uam_ref_is_none(args[k]))
                    mov_imm(j, regs[k], 0);
                else
                    load_operand(j, regs[k], blk, args[k], 64);
            }
            mov_rr(j, RDI, RBX);
            mov_imm(j, RSI, in->aux);
            call_bridge(j, jit_br_helper);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);          /* jnz epilogue (fault) */
            ic_rebase(j);
            jit_ra_save(j, true);
            if (has_dst)
                ld_mem(j, rd, RBX, OFF(scratch));
            break;
        }
        case UAM_OP_TRAP:
            /* res = { TRAP, class, exc{TRAP, flags=cont, pc, parent_pc, cause, tval} } */
            mov_imm(j, RAX, j->cur.pc);         st_mem(j, RAX, RBX, RES(pc));
            st_mem(j, RAX, RBX, REXC(pc));
            mov_imm(j, RAX, j->cur.parent_pc);  st_mem(j, RAX, RBX, REXC(parent_pc));
            g8(j, 0xC7); modrm_mem(j, 0, RBX, REXC(flags)); g32(j, j->cur.cont ? UAM_EXC_F_CONT : 0);
            load_operand(j, RAX, blk, in->a, aw); st_mem(j, RAX, RBX, REXC(cause));
            load_operand(j, RAX, blk, in->b, aw); st_mem(j, RAX, RBX, REXC(tval));
            g8(j, 0xC7); modrm_mem(j, 0, RBX, REXC(kind)); g32(j, UAM_EXC_TRAP);
            g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(trap_class)); g32(j, in->aux);
            g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, UAM_BR_TRAP);
            epi[n_epi++] = jmp_fwd(j);
            break;
        case UAM_OP_WAIT:
            set_res_pc(j, UAM_BR_WAIT, blk, in->a, aw);
            epi[n_epi++] = jmp_fwd(j);
            break;
        case UAM_OP_COV:
            emit_cov(j, imark_pc, (uint32_t)blk->imm[uam_ref_index(in->a)],
                     (uint32_t)blk->imm[uam_ref_index(in->b)]);
            break;
        default:
            emit_pure(j, blk, in, rd);
            break;
        }

        if (guarded) {
            /* guard false: no effect, dst = 0 (section 4.1) */
            if (has_dst) {
                size_t over = jmp_fwd(j);
                patch(j, guard_skip);
                mov_imm(j, rd, 0);
                patch(j, over);
            } else {
                patch(j, guard_skip);
            }
        }
        /* the paths have joined: free what this insn read last, define its result */
        jit_ra_free_dead(j, blk);
        if (j->fuse)
            j->flag_temp = in->dst;              /* in the flags, not in a register */
        else if (has_dst)
            jit_ra_def(j, in->dst, rd);
    }

    /* out-of-line stops at IMARKs */
    jit_mark(j, MK_STUB, 0);
    for (uint32_t k = 0; k < j->n_stubs; k++) {
        patch(j, j->stubs[k].pos);
        g8(j, 0x49); g8(j, 0xFF); modrm_reg(j, 1, R13);              /* dec r13: not executed */
        /* the replay, with the temps it reads where they were at the IMARK (a replay of
         * immediates reads none) */
        jit_stub_regs(j, k, true);
        emit_replays(j, blk, j->stubs[k].i, MK_STUB);
        jit_stub_regs(j, k, false);
        set_res_pc_imm(j, UAM_BR_INTERRUPTED, j->stubs[k].pc);
        epi[n_epi++] = jmp_fwd(j);
    }

    /* epilogue */
    for (unsigned k = 0; k < n_epi; k++)
        patch(j, epi[k]);
    free(epi);
    jit_mark(j, MK_EPILOGUE, 0);
    ic_flush(j);
    g8(j, 0x48); g8(j, 0x83); g8(j, 0xC4); g8(j, 0x08);  /* add rsp, 8 */
    g8(j, 0x41); g8(j, 0x5F);                            /* pop r15 */
    g8(j, 0x41); g8(j, 0x5E);                            /* pop r14 */
    g8(j, 0x41); g8(j, 0x5D);                            /* pop r13 */
    g8(j, 0x41); g8(j, 0x5C);                            /* pop r12 */
    g8(j, 0x5D);                                         /* pop rbp */
    g8(j, 0x5B);                                         /* pop rbx */
    g8(j, 0xC3);                                         /* ret */

    /* link slots, 8-byte aligned after the code (the code itself starts 16-aligned) */
    jit_mark(j, MK_SLOTS, 0);
    while (j->n & 7)
        g8(j, 0xCC);                                     /* int3 padding */
    for (unsigned k = 0; k < j->n_slot_refs; k++) {
        const size_t pos = j->slot_ref[k].pos;
        int32_t rel = (int32_t)(j->n + sizeof(jit_slot) * j->slot_ref[k].slot + j->slot_ref[k].off
                                - (pos + 4));
        if (pos + 4 <= CBUF_MAX)
            memcpy(&j->cb[pos], &rel, 4);
    }
    for (unsigned k = 0; k < j->n_slots; k++) {
        g64(j, 0);                                       /* entry */
        g64(j, LINK_NONE);                               /* epoch: never linked */
        g64(j, j->slot_pc[k]);                           /* pc */
    }
    /* fault sites (8-aligned too: slots are 24 bytes) */
    size_t sites_at = j->n;
    for (uint32_t k = 0; k < j->n_sites; k++) {
        g64(j, j->sites[k].pc);
        g64(j, j->sites[k].parent_pc);
        g64(j, j->sites[k].cont);
    }
    for (uint32_t k = 0; k < j->n_site_refs; k++) {
        int32_t rel = (int32_t)(sites_at + sizeof(jit_site) * j->site_refs[k].site
                                - (j->site_refs[k].pos + 4));
        if (j->site_refs[k].pos + 4 <= CBUF_MAX)
            memcpy(&j->cb[j->site_refs[k].pos], &rel, 4);
    }

    return !j->overflow;
}

/* ---- the interface to jit.c ---------------------------------------------------- */

bool jit_host_init(uam_jit *j)
{
    const char *base = getenv("UAM_JIT_BASELINE");
    __builtin_cpu_init();
    j->popcnt = __builtin_cpu_supports("popcnt") && !(base && *base && strcmp(base, "0") != 0);
    j->pool = ra_regs;
    j->pool_max = RA_POOL_MAX;
    j->pool_kept = 1;                              /* r12 */
    j->res_reg = RAX;
    return true;
}

void jit_host_spill(uam_jit *j, uint32_t t, int reg) { st_temp(j, t, reg); }
void jit_host_fill(uam_jit *j, uint32_t t, int reg) { ld_mem(j, reg, RBP, (int32_t)(8 * t)); }

#endif /* __x86_64__ */
