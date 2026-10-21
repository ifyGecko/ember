/*
 * x86_seg.c - Segment descriptors: GDT/LDT lookup, segment-register loads,
 * LLDT/LTR, LAR/LSL, and the far transfers (JMP/CALL far, RETF, IRET).
 *
 * Not modelled: call gates, task switches (task gates, IRET with NT),
 * virtual-8086 mode, and segment limit checks on ordinary accesses. The
 * accessed bit and the TSS busy bit are not written back to the tables.
 */
#include "x86.h"

#define GP(sel) x86_raise(cpu, X86_EXC_GP, true, (uint32_t)(sel) & ~3u)

void x86_desc_unpack(uint64_t d, uint64_t *base, uint32_t *limit, uint16_t *attr)
{
    *base = ((d >> 16) & 0xFFFFFFull) | ((d >> 32) & 0xFF000000ull);
    uint32_t l = (uint32_t)(d & 0xFFFF) | (uint32_t)((d >> 32) & 0xF0000);
    *attr = (uint16_t)((d >> 40) & 0xF0FF);
    if (*attr & SEGA_G)
        l = (l << 12) | 0xFFF;
    *limit = l;
}

uam_status x86_read_desc(uam_cpu *cpu, uint16_t sel, uint64_t *lo, uint64_t *hi, bool wide)
{
    x86_state *s = x86_st(cpu);
    uint64_t base;
    uint32_t limit;
    if (sel & 4) {
        if (!(s->ldtr.attr & SEGA_P) || (s->ldtr.sel & ~3u) == 0)
            return GP(sel);
        base = s->ldtr.base; limit = s->ldtr.limit;
    } else {
        base = s->gdtr.base; limit = s->gdtr.limit;
    }
    uint32_t idx = sel & ~7u;
    if ((uint64_t)idx + (wide ? 15 : 7) > limit)
        return GP(sel);
    uam_status st = x86_lin_read(cpu, base + idx, 8, lo, true);
    if (st == UAM_OK && wide)
        st = x86_lin_read(cpu, base + idx + 8, 8, hi, true);
    return st;
}

static bool code64(uam_cpu *cpu) { return x86_priv_code(cpu->priv) == X86_CODE64; }

uam_status x86_load_seg_nocheck(uam_cpu *cpu, unsigned seg, uint16_t sel)
{
    uint64_t lo = 0, hi = 0, base;
    uint32_t limit;
    uint16_t attr;
    if ((sel & ~3u) == 0) {
        x86_seg_set(cpu, seg, sel, 0, 0, 0);
        x86_update_priv(cpu);
        return UAM_OK;
    }
    uam_status st = x86_read_desc(cpu, sel, &lo, &hi, false);
    if (st != UAM_OK)
        return st;
    x86_desc_unpack(lo, &base, &limit, &attr);
    if (!(attr & SEGA_P))
        return x86_raise(cpu, seg == X86_SEG_SS ? X86_EXC_SS : X86_EXC_NP, true, sel & ~3u);
    x86_seg_set(cpu, seg, sel, base, limit, attr);
    x86_update_priv(cpu);
    return UAM_OK;
}

/* MOV/POP Sreg and LDS..LGS in protected and long mode. */
uam_status x86_load_seg(uam_cpu *cpu, unsigned seg, uint16_t sel)
{
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    if (x86_priv_mode(cpu->priv) == X86_MODE_REAL) {
        x86_seg_set_real(cpu, seg, sel);
        return UAM_OK;
    }
    if ((sel & ~3u) == 0) {
        if (seg == X86_SEG_SS && !(code64(cpu) && cpl < 3 && rpl == cpl))
            return GP(0);
        uint64_t keep = (code64(cpu) && seg >= X86_SEG_FS) ? x86_seg_base(cpu, seg) : 0;
        x86_seg_set(cpu, seg, sel, keep, 0, 0);
        x86_update_priv(cpu);
        return UAM_OK;
    }
    uint64_t lo = 0, hi = 0, base;
    uint32_t limit;
    uint16_t attr;
    uam_status st = x86_read_desc(cpu, sel, &lo, &hi, false);
    if (st != UAM_OK)
        return st;
    x86_desc_unpack(lo, &base, &limit, &attr);
    unsigned dpl = (attr & SEGA_DPL) >> 5;
    bool code = (attr & 0x8) != 0;
    if (!(attr & SEGA_S))
        return GP(sel);
    if (seg == X86_SEG_SS) {
        if (code || !(attr & 0x2) || rpl != cpl || dpl != cpl)
            return GP(sel);
        if (!(attr & SEGA_P))
            return x86_raise(cpu, X86_EXC_SS, true, sel & ~3u);
    } else {
        if (code && !(attr & 0x2))
            return GP(sel);                       /* execute-only code */
        bool conforming = code && (attr & 0x4);
        if (!conforming && dpl < (cpl > rpl ? cpl : rpl))
            return GP(sel);
        if (!(attr & SEGA_P))
            return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    }
    x86_seg_set(cpu, seg, sel, base, limit, attr);
    x86_update_priv(cpu);
    return UAM_OK;
}

uam_status x86_lldt(uam_cpu *cpu, uint16_t sel)
{
    x86_state *s = x86_st(cpu);
    if ((sel & ~3u) == 0) {
        s->ldtr = (x86_sysseg){ 0, 0, sel, 0 };
        return UAM_OK;
    }
    if (sel & 4)
        return GP(sel);
    bool lm = x86_priv_mode(cpu->priv) == X86_MODE_LONG;
    uint64_t lo = 0, hi = 0, base;
    uint32_t limit;
    uint16_t attr;
    uam_status st = x86_read_desc(cpu, sel, &lo, &hi, lm);
    if (st != UAM_OK)
        return st;
    x86_desc_unpack(lo, &base, &limit, &attr);
    if ((attr & SEGA_S) || (attr & SEGA_TYPE) != 0x2)
        return GP(sel);
    if (!(attr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    if (lm)
        base |= hi << 32;
    s->ldtr = (x86_sysseg){ base, limit, sel, attr };
    return UAM_OK;
}

uam_status x86_ltr(uam_cpu *cpu, uint16_t sel)
{
    x86_state *s = x86_st(cpu);
    if ((sel & ~3u) == 0 || (sel & 4))
        return GP(sel);
    bool lm = x86_priv_mode(cpu->priv) == X86_MODE_LONG;
    uint64_t lo = 0, hi = 0, base;
    uint32_t limit;
    uint16_t attr;
    uam_status st = x86_read_desc(cpu, sel, &lo, &hi, lm);
    if (st != UAM_OK)
        return st;
    x86_desc_unpack(lo, &base, &limit, &attr);
    unsigned type = attr & SEGA_TYPE;
    if ((attr & SEGA_S) || (type != 0x9 && type != 0x1))
        return GP(sel);
    if (!(attr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    if (lm)
        base |= hi << 32;
    s->tr = (x86_sysseg){ base, limit, sel, (uint16_t)(attr | 0x2) };   /* busy */
    return UAM_OK;
}

uint64_t x86_lar(uam_cpu *cpu, uint16_t sel, bool lsl)
{
    uint64_t lo = 0, hi = 0, base;
    uint32_t limit;
    uint16_t attr;
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    bool ok = (sel & ~3u) != 0 && x86_read_desc(cpu, sel, &lo, &hi, false) == UAM_OK;
    x86_st(cpu)->pend_valid = false;
    if (ok) {
        x86_desc_unpack(lo, &base, &limit, &attr);
        unsigned dpl = (attr & SEGA_DPL) >> 5;
        bool conforming = (attr & SEGA_S) && (attr & 0xC) == 0xC;
        if (!conforming && (dpl < cpl || dpl < rpl))
            ok = false;
        if (!(attr & SEGA_S)) {   /* system types LAR/LSL accept */
            unsigned t = attr & SEGA_TYPE;
            if (!(t == 1 || t == 2 || t == 3 || t == 9 || t == 0xB || (!lsl && (t == 0xC || t == 4 || t == 5))))
                ok = false;
        }
    }
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_ZF), 8, ok ? 1 : 0);
    if (!ok)
        return 0;
    return lsl ? limit : (lo >> 32) & 0x00F0FF00u;
}

/* ---- the current stack, from C ------------------------------------------- */

typedef struct stk { uint64_t sp, base, mask; } stk;

static stk cur_stack(uam_cpu *cpu)
{
    unsigned sw = x86_priv_stack(cpu->priv);
    stk k;
    k.mask = sw == 64 ? ~0ull : sw == 32 ? 0xFFFFFFFFull : 0xFFFFull;
    k.sp = x86_gpr(cpu, X86_RSP) & k.mask;
    k.base = code64(cpu) ? 0 : x86_seg_base(cpu, X86_SEG_SS);
    return k;
}

static uint64_t stk_lin(const stk *k)
{
    uint64_t lin = k->base + k->sp;
    return k->mask == ~0ull ? lin : (lin & 0xFFFFFFFFull);
}

static uam_status spush(uam_cpu *cpu, stk *k, unsigned bytes, uint64_t v)
{
    k->sp = (k->sp - bytes) & k->mask;
    return x86_lin_write(cpu, stk_lin(k), bytes, v, false);
}

static uam_status spop(uam_cpu *cpu, stk *k, unsigned bytes, uint64_t *v)
{
    uam_status st = x86_lin_read(cpu, stk_lin(k), bytes, v, false);
    k->sp = (k->sp + bytes) & k->mask;
    return st;
}

static void stk_commit(uam_cpu *cpu, const stk *k)
{
    uint64_t rsp = x86_gpr(cpu, X86_RSP);
    if (k->mask == 0xFFFFull) rsp = (rsp & ~0xFFFFull) | k->sp;
    else                      rsp = k->sp;
    x86_set_gpr(cpu, X86_RSP, rsp);
}

/* ---- code-segment targets ------------------------------------------------ */

typedef struct cdesc { uint64_t base; uint32_t limit; uint16_t attr; } cdesc;

/* Check a far JMP/CALL target (no gates). */
static uam_status check_code(uam_cpu *cpu, uint16_t sel, cdesc *d)
{
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    uint64_t lo = 0, hi = 0;
    if ((sel & ~3u) == 0)
        return GP(0);
    uam_status st = x86_read_desc(cpu, sel, &lo, &hi, false);
    if (st != UAM_OK)
        return st;
    x86_desc_unpack(lo, &d->base, &d->limit, &d->attr);
    unsigned dpl = (d->attr & SEGA_DPL) >> 5;
    if (!(d->attr & SEGA_S) || !(d->attr & 0x8))
        return GP(sel);   /* call gates and task gates are not supported */
    if (d->attr & 0x4) { if (dpl > cpl) return GP(sel); }
    else if (rpl > cpl || dpl != cpl) return GP(sel);
    if (!(d->attr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    return UAM_OK;
}

static uint64_t set_cs(uam_cpu *cpu, uint16_t sel, const cdesc *d, uint64_t off, unsigned osz)
{
    uint64_t base = d->base;
    bool lm = x86_priv_mode(cpu->priv) == X86_MODE_LONG;
    if (lm && (d->attr & SEGA_L))
        base = 0;
    x86_seg_set(cpu, X86_SEG_CS, sel, base, d->limit, d->attr);
    x86_update_priv(cpu);
    if (code64(cpu))
        return off;
    return (base + (off & (osz == 16 ? 0xFFFFull : 0xFFFFFFFFull))) & 0xFFFFFFFFull;
}

static uint64_t real_cs(uam_cpu *cpu, uint16_t sel, uint64_t off, unsigned osz)
{
    x86_seg_set_real(cpu, X86_SEG_CS, sel);
    return ((uint64_t)sel << 4) + (off & (osz == 16 ? 0xFFFFull : 0xFFFFFFFFull));
}

uam_status x86_far_jmp(uam_cpu *cpu, uint16_t sel, uint64_t off, uint64_t *pc)
{
    const unsigned osz = 32;   /* the lifter already cut the offset to the operand size */
    if (x86_priv_mode(cpu->priv) == X86_MODE_REAL) {
        *pc = real_cs(cpu, sel, off, osz);
        return UAM_OK;
    }
    cdesc d;
    uam_status st = check_code(cpu, sel, &d);
    if (st != UAM_OK)
        return st;
    *pc = set_cs(cpu, (uint16_t)((sel & ~3u) | x86_priv_cpl(cpu->priv)), &d, off, osz);
    return UAM_OK;
}

uam_status x86_far_call(uam_cpu *cpu, uint16_t sel, unsigned osz, uint64_t off,
                        uint64_t ret_rip, uint64_t *pc)
{
    unsigned b = osz / 8;
    stk k = cur_stack(cpu);
    uint16_t old_cs = x86_seg_sel(cpu, X86_SEG_CS);
    cdesc d = { 0, 0, 0 };
    uam_status st;
    bool real = x86_priv_mode(cpu->priv) == X86_MODE_REAL;
    if (!real && (st = check_code(cpu, sel, &d)) != UAM_OK)
        return st;
    if ((st = spush(cpu, &k, b, old_cs)) != UAM_OK || (st = spush(cpu, &k, b, ret_rip)) != UAM_OK)
        return st;
    stk_commit(cpu, &k);
    *pc = real ? real_cs(cpu, sel, off, osz)
               : set_cs(cpu, (uint16_t)((sel & ~3u) | x86_priv_cpl(cpu->priv)), &d, off, osz);
    return UAM_OK;
}

/* Check the code segment a RETF/IRET returns to. */
static uam_status check_return_cs(uam_cpu *cpu, uint16_t sel, cdesc *d)
{
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    uint64_t lo = 0, hi = 0;
    if ((sel & ~3u) == 0 || rpl < cpl)
        return GP(sel);
    uam_status st = x86_read_desc(cpu, sel, &lo, &hi, false);
    if (st != UAM_OK)
        return st;
    x86_desc_unpack(lo, &d->base, &d->limit, &d->attr);
    unsigned dpl = (d->attr & SEGA_DPL) >> 5;
    if (!(d->attr & SEGA_S) || !(d->attr & 0x8))
        return GP(sel);
    if ((d->attr & 0x4) ? dpl > rpl : dpl != rpl)
        return GP(sel);
    if (!(d->attr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    return UAM_OK;
}

uam_status x86_retf(uam_cpu *cpu, unsigned osz, unsigned imm16, uint64_t *pc)
{
    unsigned b = osz / 8;
    stk k = cur_stack(cpu);
    uint64_t ip = 0, cs = 0, nsp = 0, nss = 0;
    uam_status st;
    if ((st = spop(cpu, &k, b, &ip)) != UAM_OK || (st = spop(cpu, &k, b, &cs)) != UAM_OK)
        return st;
    k.sp = (k.sp + imm16) & k.mask;
    if (x86_priv_mode(cpu->priv) == X86_MODE_REAL) {
        stk_commit(cpu, &k);
        *pc = real_cs(cpu, (uint16_t)cs, ip, osz);
        return UAM_OK;
    }
    cdesc d;
    if ((st = check_return_cs(cpu, (uint16_t)cs, &d)) != UAM_OK)
        return st;
    bool outer = (cs & 3u) > x86_priv_cpl(cpu->priv);
    if (outer) {
        if ((st = spop(cpu, &k, b, &nsp)) != UAM_OK || (st = spop(cpu, &k, b, &nss)) != UAM_OK)
            return st;
    } else {
        stk_commit(cpu, &k);
    }
    *pc = set_cs(cpu, (uint16_t)cs, &d, ip, osz);
    if (outer) {
        if ((st = x86_load_seg_nocheck(cpu, X86_SEG_SS, (uint16_t)nss)) != UAM_OK)
            return st;
        x86_set_gpr(cpu, X86_RSP, (nsp + imm16) & (osz == 64 ? ~0ull : 0xFFFFFFFFull));
    }
    return UAM_OK;
}

uam_status x86_iret(uam_cpu *cpu, unsigned osz, uint64_t *pc)
{
    x86_state *s = x86_st(cpu);
    unsigned b = osz / 8;
    unsigned cpl = x86_priv_cpl(cpu->priv);
    bool was64 = code64(cpu);
    stk k = cur_stack(cpu);
    uint64_t ip = 0, cs = 0, fl = 0, nsp = 0, nss = 0;
    uam_status st;
    if ((st = spop(cpu, &k, b, &ip)) != UAM_OK || (st = spop(cpu, &k, b, &cs)) != UAM_OK ||
        (st = spop(cpu, &k, b, &fl)) != UAM_OK)
        return st;

    uint32_t cur = x86_get_rflags(cpu);
    uint32_t mask = osz == 16 ? 0xFFFFu : 0x3F7FD5u;   /* never VM through here */
    if (x86_priv_mode(cpu->priv) == X86_MODE_REAL) {
        stk_commit(cpu, &k);
        x86_set_rflags(cpu, (cur & ~mask) | ((uint32_t)fl & mask));
        *pc = real_cs(cpu, (uint16_t)cs, ip, osz);
        s->nmi_blocked = false;
        uam_cpu_request_exit(cpu);
        return UAM_OK;
    }
    if (fl & RFLAGS_VM || (cur & RFLAGS_NT && x86_priv_mode(cpu->priv) == X86_MODE_PROT))
        return GP(0);   /* virtual-8086 and task returns are not supported */

    cdesc d;
    if ((st = check_return_cs(cpu, (uint16_t)cs, &d)) != UAM_OK)
        return st;
    bool outer = (cs & 3u) > cpl;
    bool pop_ss = outer || was64;
    if (pop_ss && ((st = spop(cpu, &k, b, &nsp)) != UAM_OK || (st = spop(cpu, &k, b, &nss)) != UAM_OK))
        return st;

    if (cpl > 0) mask &= ~(uint32_t)RFLAGS_IOPL;
    if (cpl > ((cur & RFLAGS_IOPL) >> 12)) mask &= ~(uint32_t)RFLAGS_IF;
    x86_set_rflags(cpu, (cur & ~mask) | ((uint32_t)fl & mask));
    if (!pop_ss)
        stk_commit(cpu, &k);
    *pc = set_cs(cpu, (uint16_t)cs, &d, ip, osz);
    if (pop_ss) {
        if ((st = x86_load_seg_nocheck(cpu, X86_SEG_SS, (uint16_t)nss)) != UAM_OK)
            return st;
        x86_set_gpr(cpu, X86_RSP, nsp & (osz == 64 ? ~0ull : 0xFFFFFFFFull));
    }
    s->nmi_blocked = false;
    uam_cpu_request_exit(cpu);
    return UAM_OK;
}
