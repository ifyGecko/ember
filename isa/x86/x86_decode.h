/*
 * x86_decode.h - x86 instruction decoding (prefixes, opcode maps, ModRM/SIB,
 * displacement, immediates) into a flat record. Decoding is separate from
 * lifting: the whole instruction is fetched and decoded first, so a fetch
 * fault on a page boundary never leaves a half-emitted instruction and the
 * IMARK length is exact.
 */
#ifndef UAM_X86_DECODE_H
#define UAM_X86_DECODE_H

#include "x86.h"

typedef enum { X86_MAP_1 = 0, X86_MAP_0F, X86_MAP_0F38, X86_MAP_0F3A } x86_map;

typedef struct x86_insn {
    /* prefixes */
    bool     p66, p67, lock, rep, repne;   /* rep = F3, repne = F2 (last one wins) */
    int      seg;                          /* -1 or X86_SEG_* override */
    uint8_t  rex;                          /* 0 if none, else 0x40..0x4F */
    bool     rex_w;
    uint8_t  rex_r, rex_x, rex_b;          /* 0 or 8 */
    uint8_t  last_rep;                     /* 0, 0xF2 or 0xF3 (mandatory-prefix SSE) */

    x86_map  map;
    uint8_t  op;

    bool     has_modrm;
    uint8_t  modrm, mod, reg, rm;          /* reg includes REX.R; rm includes REX.B */
    bool     has_sib;
    uint8_t  sib;
    int64_t  disp;
    bool     rip_rel;
    /* decoded effective address (memory forms, mod != 3) */
    int      ea_base, ea_index;            /* GPR numbers, -1 = none */
    unsigned ea_scale;                     /* 1, 2, 4, 8 */
    unsigned ea_dseg;                      /* default segment (DS, or SS for rBP/rSP bases) */

    uint64_t imm, imm2;                    /* imm2: far pointer selector / ENTER level */

    unsigned code;                         /* X86_CODE16/32/64 */
    unsigned osz;                          /* effective operand size: 16/32/64 */
    unsigned asz;                          /* effective address size: 16/32/64 */
    unsigned len;
} x86_insn;

typedef enum {
    X86_DEC_OK = 0,
    X86_DEC_FAULT,     /* fetch faulted */
    X86_DEC_UD,        /* invalid encoding (length still >= 1) */
} x86_dec_status;

/* Fetch callback: read one code byte at offset `off` from the instruction start. */
typedef bool (*x86_fetch_fn)(void *ctx, unsigned off, uint8_t *byte);

x86_dec_status x86_decode(x86_insn *in, unsigned code, x86_fetch_fn fetch, void *ctx);

#endif /* UAM_X86_DECODE_H */
