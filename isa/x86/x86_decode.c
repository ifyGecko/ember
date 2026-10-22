/*
 * x86_decode.c - x86 instruction decoder (see x86_decode.h).
 *
 * Length is found from a per-opcode attribute: whether a ModRM byte follows,
 * which immediate form trails it, and how 64-bit mode changes the default
 * operand size (D64: push/pop and friends default to 64 bits; F64: near
 * branches are always 64-bit) or invalidates the opcode (I64). The VEX/EVEX
 * spaces are not decoded: the processor does not report AVX, so those bytes
 * are LES/LDS/BOUND outside 64-bit mode and #UD inside it.
 */
#include "x86_decode.h"

enum {
    D_OK   = 1u << 0,   /* valid opcode */
    D_M    = 1u << 1,   /* ModRM follows */
    D_D64  = 1u << 2,   /* 64-bit mode: default operand size 64 */
    D_F64  = 1u << 3,   /* 64-bit mode: operand size forced to 64 */
    D_I64  = 1u << 4,   /* invalid in 64-bit mode */
    D_PFX  = 1u << 5,   /* a prefix byte (never looked up as an opcode) */
    /* immediate kind in bits 8-11 */
    I_SHIFT = 8,
};
enum { I_NONE = 0, I_B, I_W, I_Z, I_V, I_MO, I_AP, I_EN, I_G3 };

#define IM(k) ((unsigned)(k) << I_SHIFT)
#define x_  0u
#define N   D_OK
#define M   (D_OK | D_M)
#define MB  (D_OK | D_M | IM(I_B))
#define MZ  (D_OK | D_M | IM(I_Z))
#define B   (D_OK | IM(I_B))
#define W   (D_OK | IM(I_W))
#define Z   (D_OK | IM(I_Z))
#define V   (D_OK | IM(I_V))
#define MO  (D_OK | IM(I_MO))
#define AP  (D_OK | IM(I_AP) | D_I64)
#define EN  (D_OK | IM(I_EN) | D_D64)
#define G3  (D_OK | D_M | IM(I_G3))
#define I6  (D_OK | D_I64)
#define D6  (D_OK | D_D64)
#define BF  (D_OK | IM(I_B) | D_F64)
#define ZF  (D_OK | IM(I_Z) | D_F64)
#define P   D_PFX

static const uint16_t attr_1[256] = {
/*        0    1    2    3    4      5    6    7      8    9    A    B    C    D    E    F  */
/*0*/     M,   M,   M,   M,   B,     Z,   I6,  I6,    M,   M,   M,   M,   B,   Z,   I6,  N,
/*1*/     M,   M,   M,   M,   B,     Z,   I6,  I6,    M,   M,   M,   M,   B,   Z,   I6,  I6,
/*2*/     M,   M,   M,   M,   B,     Z,   P,   I6,    M,   M,   M,   M,   B,   Z,   P,   I6,
/*3*/     M,   M,   M,   M,   B,     Z,   P,   I6,    M,   M,   M,   M,   B,   Z,   P,   I6,
/*4*/     N,   N,   N,   N,   N,     N,   N,   N,     N,   N,   N,   N,   N,   N,   N,   N,
/*5*/     D6,  D6,  D6,  D6,  D6,    D6,  D6,  D6,    D6,  D6,  D6,  D6,  D6,  D6,  D6,  D6,
/*6*/     I6,  I6,  M|D_I64, M, P,   P,   P,   P,     Z|D_D64, MZ, B|D_D64, MB, N, N,  N,   N,
/*7*/     BF,  BF,  BF,  BF,  BF,    BF,  BF,  BF,    BF,  BF,  BF,  BF,  BF,  BF,  BF,  BF,
/*8*/     MB,  MZ,  MB|D_I64, MB, M, M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M|D_D64,
/*9*/     N,   N,   N,   N,   N,     N,   N,   N,     N,   N,   AP,  N,   D6,  D6,  N,   N,
/*A*/     MO,  MO,  MO,  MO,  N,     N,   N,   N,     B,   Z,   N,   N,   N,   N,   N,   N,
/*B*/     B,   B,   B,   B,   B,     B,   B,   B,     V,   V,   V,   V,   V,   V,   V,   V,
/*C*/     MB,  MB,  W|D_F64, N|D_F64, M|D_I64, M|D_I64, MB, MZ,  EN, D6, W,  N,   N,   B,   I6,  N,
/*D*/     M,   M,   M,   M,   B|D_I64, B|D_I64, I6, N,  M,   M,   M,   M,   M,   M,   M,   M,
/*E*/     BF,  BF,  BF,  BF,  B,     B,   B,   B,     ZF,  ZF,  AP,  BF,  N,   N,   N,   N,
/*F*/     P,   N,   P,   P,   N,     N,   G3,  G3,    N,   N,   N,   N,   N,   N,   M,   M,
};

static const uint16_t attr_0f[256] = {
/*        0    1    2    3    4    5    6    7      8    9    A    B    C    D    E    F  */
/*0*/     M,   M,   M,   M,   x_,  N,   N,   N,     N,   N,   x_,  N,   x_,  M,   x_,  x_,
/*1*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*2*/     M,   M,   M,   M,   x_,  x_,  x_,  x_,    M,   M,   M,   M,   M,   M,   M,   M,
/*3*/     N,   N,   N,   N,   N,   N,   x_,  N,     x_,  x_,  x_,  x_,  x_,  x_,  x_,  x_,
/*4*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*5*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*6*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*7*/     MB,  MB,  MB,  MB,  M,   M,   M,   N,     x_,  x_,  x_,  x_,  M,   M,   M,   M,
/*8*/     ZF,  ZF,  ZF,  ZF,  ZF,  ZF,  ZF,  ZF,    ZF,  ZF,  ZF,  ZF,  ZF,  ZF,  ZF,  ZF,
/*9*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*A*/     D6,  D6,  N,   M,   MB,  M,   x_,  x_,    D6,  D6,  N,   M,   MB,  M,   M,   M,
/*B*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   MB,  M,   M,   M,   M,   M,
/*C*/     M,   M,   MB,  M,   MB,  MB,  MB,  M,     N,   N,   N,   N,   N,   N,   N,   N,
/*D*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*E*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
/*F*/     M,   M,   M,   M,   M,   M,   M,   M,     M,   M,   M,   M,   M,   M,   M,   M,
};

#undef x_
#undef N
#undef M
#undef MB
#undef MZ
#undef B
#undef W
#undef Z
#undef V
#undef MO
#undef AP
#undef EN
#undef G3
#undef I6
#undef D6
#undef BF
#undef ZF
#undef P

typedef struct {
    x86_fetch_fn fetch;
    void        *ctx;
    unsigned     off;
    bool         fault;
} rd;

static uint8_t get8(rd *r)
{
    uint8_t b = 0;
    if (r->off >= 15) { r->off++; return 0; }          /* over-long: caught below */
    if (!r->fetch(r->ctx, r->off, &b))
        r->fault = true;
    r->off++;
    return b;
}

static uint64_t getn(rd *r, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= (uint64_t)get8(r) << (8 * i);
    return v;
}

static int64_t sx(uint64_t v, unsigned bytes)
{
    unsigned sh = 64 - 8 * bytes;
    return (int64_t)(v << sh) >> sh;
}

/* ModRM / SIB / displacement */
static void decode_ea(x86_insn *in, rd *r)
{
    in->ea_base = in->ea_index = -1;
    in->ea_scale = 1;
    in->ea_dseg = X86_SEG_DS;
    in->disp = 0;
    in->rip_rel = false;
    if (in->mod == 3)
        return;

    if (in->asz == 16) {
        static const int8_t b16[8] = { 3, 3, 5, 5, 6, 7, 5, 3 };   /* bx bx bp bp si di bp bx */
        static const int8_t i16[8] = { 6, 7, 6, 7, -1, -1, -1, -1 };
        unsigned rm = in->modrm & 7;
        if (in->mod == 0 && rm == 6) {
            in->disp = (int64_t)(uint16_t)getn(r, 2);
            return;
        }
        in->ea_base = b16[rm];
        in->ea_index = i16[rm];
        if (in->ea_base == 5)
            in->ea_dseg = X86_SEG_SS;
        if (in->mod == 1) in->disp = sx(getn(r, 1), 1);
        else if (in->mod == 2) in->disp = sx(getn(r, 2), 2);
        return;
    }

    unsigned rm = in->modrm & 7;
    bool nobase = false;
    if (rm == 4) {
        in->has_sib = true;
        in->sib = get8(r);
        unsigned base = in->sib & 7, idx = ((in->sib >> 3) & 7) | in->rex_x;
        in->ea_scale = 1u << (in->sib >> 6);
        if (idx != 4)
            in->ea_index = (int)idx;
        if (base == 5 && in->mod == 0)
            nobase = true;
        else
            in->ea_base = (int)(base | in->rex_b);
    } else if (rm == 5 && in->mod == 0) {
        nobase = true;
        if (in->code == X86_CODE64)
            in->rip_rel = true;
    } else {
        in->ea_base = (int)(rm | in->rex_b);
    }
    if (in->ea_base == 4 || in->ea_base == 5)
        in->ea_dseg = X86_SEG_SS;
    if (nobase || in->mod == 2) in->disp = sx(getn(r, 4), 4);
    else if (in->mod == 1)      in->disp = sx(getn(r, 1), 1);
}

x86_dec_status x86_decode(x86_insn *in, unsigned code, x86_fetch_fn fetch, void *ctx)
{
    rd r = { .fetch = fetch, .ctx = ctx };
    *in = (x86_insn){ .seg = -1, .code = code };
    const bool m64 = (code == X86_CODE64);

    /* prefixes; in 64-bit mode a REX byte counts only directly before the opcode */
    uint8_t c;
    for (;;) {
        c = get8(&r);
        if (r.fault) return X86_DEC_FAULT;
        if (m64 && (c & 0xF0) == 0x40) {
            in->rex = c;
            continue;
        }
        bool pfx = true;
        switch (c) {
        case 0x66: in->p66 = true; break;
        case 0x67: in->p67 = true; break;
        case 0xF0: in->lock = true; break;
        case 0xF2: in->repne = true; in->rep = false; in->last_rep = 0xF2; break;
        case 0xF3: in->rep = true; in->repne = false; in->last_rep = 0xF3; break;
        case 0x26: in->seg = X86_SEG_ES; break;
        case 0x2E: in->seg = X86_SEG_CS; break;
        case 0x36: in->seg = X86_SEG_SS; break;
        case 0x3E: in->seg = X86_SEG_DS; break;
        case 0x64: in->seg = X86_SEG_FS; break;
        case 0x65: in->seg = X86_SEG_GS; break;
        default: pfx = false; break;
        }
        if (!pfx) break;
        in->rex = 0;   /* a legacy prefix after REX cancels it */
        if (r.off >= 15) { in->len = 15; return X86_DEC_UD; }
    }
    if (in->rex) {
        in->rex_w = (in->rex & 8) != 0;
        in->rex_r = (in->rex & 4) ? 8 : 0;
        in->rex_x = (in->rex & 2) ? 8 : 0;
        in->rex_b = (in->rex & 1) ? 8 : 0;
    }
    /* In 64-bit mode CS/DS/ES/SS overrides are ignored (FS and GS remain). */
    if (m64 && in->seg >= 0 && in->seg < X86_SEG_FS)
        in->seg = -1;

    uint16_t a;
    in->op = c;
    if (c == 0x0F) {
        uint8_t c2 = get8(&r);
        if (c2 == 0x38 || c2 == 0x3A) {
            in->map = (c2 == 0x38) ? X86_MAP_0F38 : X86_MAP_0F3A;
            in->op = get8(&r);
            a = (c2 == 0x38) ? (D_OK | D_M) : (D_OK | D_M | IM(I_B));
        } else {
            in->map = X86_MAP_0F;
            in->op = c2;
            a = attr_0f[c2];
        }
    } else {
        in->map = X86_MAP_1;
        a = attr_1[c];
    }
    if (r.fault) return X86_DEC_FAULT;

    /* effective operand and address sizes */
    switch (code) {
    case X86_CODE16: in->osz = in->p66 ? 32 : 16; in->asz = in->p67 ? 32 : 16; break;
    case X86_CODE32: in->osz = in->p66 ? 16 : 32; in->asz = in->p67 ? 16 : 32; break;
    default:
        in->osz = in->rex_w ? 64 : (in->p66 ? 16 : 32);
        in->asz = in->p67 ? 32 : 64;
        if (a & D_F64) in->osz = 64;
        else if ((a & D_D64) && !in->rex_w) in->osz = in->p66 ? 16 : 64;
        break;
    }

    if (!(a & D_OK) || (m64 && (a & D_I64))) {
        in->len = r.off;
        return X86_DEC_UD;
    }

    if (a & D_M) {
        in->has_modrm = true;
        in->modrm = get8(&r);
        in->mod = in->modrm >> 6;
        in->reg = ((in->modrm >> 3) & 7) | in->rex_r;
        in->rm = (in->modrm & 7) | in->rex_b;
        /* MOV to/from control and debug registers always name a register */
        if (in->map == X86_MAP_0F && in->op >= 0x20 && in->op <= 0x23)
            in->mod = 3;
        decode_ea(in, &r);

        /* group 5 (FF) and 8F: near CALL/JMP are F64, PUSH/POP are D64 */
        if (m64 && in->map == X86_MAP_1) {
            unsigned sub = (in->modrm >> 3) & 7;
            if (in->op == 0xFF && (sub == 2 || sub == 4))
                in->osz = 64;
            else if ((in->op == 0xFF && sub == 6) || in->op == 0x8F)
                in->osz = in->rex_w ? 64 : (in->p66 ? 16 : 64);
        }
    }

    unsigned zbytes = (in->osz == 16) ? 2 : 4;
    switch ((a >> I_SHIFT) & 0xF) {
    case I_NONE: break;
    case I_B:  in->imm = getn(&r, 1); break;
    case I_W:  in->imm = getn(&r, 2); break;
    case I_Z:  in->imm = getn(&r, zbytes); break;
    case I_V:  in->imm = getn(&r, in->osz / 8); break;
    case I_MO: in->imm = getn(&r, in->asz / 8); break;
    case I_AP: in->imm = getn(&r, zbytes); in->imm2 = getn(&r, 2); break;
    case I_EN: in->imm = getn(&r, 2); in->imm2 = getn(&r, 1); break;
    case I_G3:
        if (((in->modrm >> 3) & 7) < 2)
            in->imm = getn(&r, (in->op & 1) ? zbytes : 1);
        break;
    }
    if (r.fault) return X86_DEC_FAULT;
    in->len = r.off;
    if (in->len > 15) { in->len = 15; return X86_DEC_UD; }
    return X86_DEC_OK;
}
