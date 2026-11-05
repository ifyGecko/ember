/*
 * xtensa_decode.c - Instruction lengths, big-endian field order and the opcode table
 * lookup. A pure function of the instruction bytes and the CPU model, so the decoder can
 * be checked against the GNU disassemblers (tests/isa-xtensa/xt-check.py).
 */
#include "xtensa.h"
#include "xtensa_opc.h"

unsigned xt_insn_len(const uam_xtensa_config *cfg, unsigned op0)
{
    if (op0 < 8)
        return 3;
    if (op0 < 14)
        return (cfg->options & UAM_XT_OPT_DENSITY) ? 2 : 3;   /* 3: reserved, illegal */
    return (cfg->options & UAM_XT_OPT_LONG_INSN) ? 4 : 3;
}

/*
 * A big-endian core lays an instruction out as the little-endian one cut into its
 * fields, the fields put back in the opposite order, each keeping its own bit order (ISA
 * manual 7.1). Which cuts there are depends on the format, so rearrange per format: the
 * result is the little-endian word, which the table and the lifter read. `w` is the word
 * read big-endian (b0 << 16 | b1 << 8 | b2, or b0 << 8 | b1 for 16 bits).
 */
uint32_t xt_be_to_le(uint32_t w, unsigned len)
{
    if (len == 2) {   /* RRRN, RI7, RI6: op0 t s r, every piece a nibble */
        return ((w >> 12) & 15) | ((w >> 8) & 15) << 4 | ((w >> 4) & 15) << 8 | (w & 15) << 12;
    }
    uint32_t op0 = (w >> 20) & 15, t = (w >> 16) & 15, s = (w >> 12) & 15, r = (w >> 8) & 15;
    uint32_t op1 = (w >> 4) & 15, op2 = w & 15;
    uint32_t n = t >> 2, m = t & 3;          /* where a format splits t into n and m */
    uint32_t nm = m << 2 | n;                /* the little-endian t nibble of n and m */
    switch (op0) {
    case 0:
        if (op1 == 0 && op2 == 0 && r == 0)  /* CALLX: n, m */
            return s << 8 | nm << 4;
        if ((op1 == 3 && (op2 == 0 || op2 == 1 || op2 == 15)) || (op1 == 1 && op2 == 6))
            /* RSR (RSR, WSR, XSR, WUR): sr = w[15:8] is one piece, r its high nibble */
            return op2 << 20 | op1 << 16 | (w & 0xFF00) | t << 4;
        return op2 << 20 | op1 << 16 | r << 12 | s << 8 | t << 4;   /* RRR, RRI4 */
    case 4:          /* MAC16: RRR */
        return op2 << 20 | op1 << 16 | r << 12 | s << 8 | t << 4 | op0;
    case 1:          /* RI16 */
        return (w & 0xFFFF) << 8 | t << 4 | op0;
    case 2: case 3: case 7:   /* RRI8 */
        return (w & 0xFF) << 16 | r << 12 | s << 8 | t << 4 | op0;
    case 5:          /* CALL: n, offset18 */
        return (w & 0x3FFFF) << 6 | n << 4 | op0;
    case 6:
        if (n == 0)                          /* J: CALL */
            return (w & 0x3FFFF) << 6 | n << 4 | op0;
        if (n == 1 || (n == 3 && m == 0))    /* BRI12: BZ, ENTRY */
            return (w & 0xFFF) << 12 | s << 8 | nm << 4 | op0;
        return (w & 0xFF) << 16 | r << 12 | s << 8 | nm << 4 | op0;   /* BRI8 */
    default:         /* op0 14, 15: reserved or designer formats, never decoded */
        return op2 << 20 | op1 << 16 | r << 12 | s << 8 | t << 4 | op0;
    }
}

/* Does the model have the option group's instructions? */
static bool group_ok(const uam_xtensa_config *cfg, unsigned group, unsigned id)
{
    uint32_t o = cfg->options;
    switch (group) {
    case XTG_CORE:
        if (id == XTO_RER || id == XTO_WER)
            return o & UAM_XT_OPT_EXTERN_REGS;
        return true;
    case XTG_DENSITY:    /* RETW.N is also the Windowed option's */
        return (o & UAM_XT_OPT_DENSITY) && (id != XTO_RETW_N || (o & UAM_XT_OPT_WINDOWED));
    case XTG_WINDOWED:   return o & UAM_XT_OPT_WINDOWED;
    case XTG_LOOP:       return o & UAM_XT_OPT_LOOPS;
    case XTG_BOOLEAN:    return o & UAM_XT_OPT_BOOLEANS;
    case XTG_FP:
        if (id == XTO_LSIP || id == XTO_SSIP || id == XTO_LSXP || id == XTO_SSXP)
            return (o & UAM_XT_OPT_FP) && (o & UAM_XT_OPT_FP_EXT);
        return o & UAM_XT_OPT_FP;
    case XTG_FP_EXT:     return (o & UAM_XT_OPT_FP) && (o & UAM_XT_OPT_FP_EXT);
    case XTG_MAC16:      return o & UAM_XT_OPT_MAC16;
    case XTG_MUL16:      return o & UAM_XT_OPT_MUL16;
    case XTG_MUL32:      return o & UAM_XT_OPT_MUL32;
    case XTG_MUL32_HIGH: return o & UAM_XT_OPT_MUL32_HIGH;
    case XTG_DIV32:      return o & UAM_XT_OPT_DIV32;
    case XTG_NSA:        return o & UAM_XT_OPT_NSA;
    case XTG_MINMAX:     return o & UAM_XT_OPT_MINMAX;
    case XTG_SEXT:       return o & UAM_XT_OPT_SEXT;
    case XTG_CLAMPS:     return o & UAM_XT_OPT_CLAMPS;
    case XTG_S32C1I:     return o & UAM_XT_OPT_S32C1I;
    case XTG_MP_SYNC:    return o & UAM_XT_OPT_SYNC;
    case XTG_EXCEPTION:  /* XEA2: RFUE is XEA1's, RFME the memory ECC option's */
        return id != XTO_RFUE && id != XTO_RFME;
    case XTG_INTERRUPT:  return cfg->ninterrupt > 0;
    case XTG_DEBUG:      /* RFDO, RFDD, LDDR32.P, SDDR32.P exist only in OCD mode */
        return (o & UAM_XT_OPT_DEBUG) && (id == XTO_BREAK || id == XTO_BREAK_N);
    case XTG_TLB:        return o & UAM_XT_OPT_REGION_PROT;
    case XTG_SALT:       return o & UAM_XT_OPT_SALT;
    case XTG_S32NB:      return o & UAM_XT_OPT_S32NB;
    default:             /* caches, cache locks and tests, SIMCALL: none of the models */
        return false;
    }
}

const xt_opc *xt_decode(const uam_xtensa_config *cfg, uint32_t w, unsigned len)
{
    static uint16_t first[17];
    static bool indexed;
    if (!indexed) {   /* the table is sorted by op0 */
        unsigned n = (unsigned)UAM_ARRAY_LEN(xt_opcodes), op0 = 0;
        for (unsigned i = 0; i < n; i++)
            while (op0 <= (xt_opcodes[i].value & 15))
                first[op0++] = (uint16_t)i;
        while (op0 <= 16)
            first[op0++] = (uint16_t)n;
        indexed = true;
    }
    unsigned op0 = w & 15;
    for (unsigned i = first[op0]; i < first[op0 + 1]; i++) {
        const xt_opc *e = &xt_opcodes[i];
        if (e->len == len && (w & e->mask) == e->value)
            return group_ok(cfg, e->group, e->id) ? e : NULL;
    }
    return NULL;
}

const char *xt_opname(unsigned id) { return id < XTO__COUNT ? xt_opnames[id] : "?"; }
