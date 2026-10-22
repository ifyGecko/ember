/*
 * x86_seg.c - Segment descriptors: GDT/LDT lookup, segment-register loads,
 * LLDT/LTR, LAR/LSL, and the far transfers (JMP/CALL far, RETF, IRET).
 *
 * Far JMP/CALL also go through call gates (16-, 32- and 64-bit, with the
 * inner-stack switch and parameter copy) and to tasks (a TSS or a task gate:
 * x86_task.c); IRET returns from a nested task (NT), into virtual-8086 mode,
 * and within it. Not modelled: segment limit checks on ordinary accesses, and
 * the accessed bit of code and data descriptors.
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
    if (x86_priv_realseg(cpu->priv)) {
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
    /* the descriptor becomes busy, in the GDT too (a task switch back checks it);
     * a GDT in ROM ignores the write, as on hardware, but a page fault is one */
    uint64_t at = s->gdtr.base + (sel & ~7u) + 5;
    if ((st = x86_lin_write(cpu, at, 1, ((lo >> 40) & 0xFF) | 0x2, true)) != UAM_OK) {
        if (s->pend_vec == X86_EXC_PF)
            return st;
        s->pend_valid = false;
    }
    s->tr = (x86_sysseg){ base, limit, sel, (uint16_t)(attr | 0x2) };
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

/* Push onto a stack being built for an inner privilege level (supervisor). */
static uam_status kpush(uam_cpu *cpu, stk *k, unsigned bytes, uint64_t v)
{
    k->sp = (k->sp - bytes) & k->mask;
    return x86_lin_write(cpu, stk_lin(k), bytes, v, true);
}

void x86_null_outer_segs(uam_cpu *cpu)
{
    unsigned cpl = x86_priv_cpl(cpu->priv);
    static const unsigned segs[] = { X86_SEG_ES, X86_SEG_DS, X86_SEG_FS, X86_SEG_GS };
    for (unsigned i = 0; i < UAM_ARRAY_LEN(segs); i++) {
        uint16_t a = x86_seg_attr(cpu, segs[i]);
        bool conforming_code = (a & 0x8) && (a & 0x4);
        if ((a & SEGA_S) && !conforming_code && ((a & SEGA_DPL) >> 5) < cpl)
            x86_seg_set(cpu, segs[i], 0, 0, 0, 0);
    }
}

/* ---- code-segment targets ------------------------------------------------ */

typedef struct cdesc { uint64_t base; uint32_t limit; uint16_t attr; } cdesc;

/* A code segment reached directly (not through a gate): checked for the CPL. */
static uam_status check_direct_code(uam_cpu *cpu, uint16_t sel, uint64_t lo, cdesc *d)
{
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    x86_desc_unpack(lo, &d->base, &d->limit, &d->attr);
    unsigned dpl = (d->attr & SEGA_DPL) >> 5;
    if (!(d->attr & 0x8))
        return GP(sel);
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

/* The descriptor a far JMP/CALL selector names: 16 bytes for a system
 * descriptor in IA-32e mode. */
static uam_status far_desc(uam_cpu *cpu, uint16_t sel, uint64_t *lo, uint64_t *hi)
{
    *lo = *hi = 0;
    if ((sel & ~3u) == 0)
        return GP(0);
    uam_status st = x86_read_desc(cpu, sel, lo, hi, false);
    if (st == UAM_OK && !((*lo >> 44) & 1) && x86_priv_mode(cpu->priv) == X86_MODE_LONG)
        st = x86_read_desc(cpu, sel, lo, hi, true);
    return st;
}

/* JMP/CALL to a TSS or a task gate. */
static uam_status far_task(uam_cpu *cpu, uint16_t sel, uint64_t lo, bool call, uint64_t ret_lin,
                           uint64_t *pc)
{
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    unsigned type = (lo >> 40) & 0xF, dpl = (lo >> 45) & 3;
    if (x86_priv_mode(cpu->priv) == X86_MODE_LONG || dpl < cpl || dpl < rpl)
        return GP(sel);
    if (!((lo >> 47) & 1))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    uint16_t tss = type == 0x5 ? (uint16_t)(lo >> 16) : sel;
    return x86_task_switch(cpu, tss, call ? X86_TASK_CALL : X86_TASK_JMP, ret_lin, false, 0, pc);
}

/* JMP/CALL through a call gate. ret_rip is the return offset (CALL). */
static uam_status far_gate(uam_cpu *cpu, uint16_t sel, uint64_t lo, uint64_t hi, bool call,
                           uint64_t ret_rip, uint64_t *pc)
{
    x86_state *s = x86_st(cpu);
    bool lm = x86_priv_mode(cpu->priv) == X86_MODE_LONG;
    unsigned cpl = x86_priv_cpl(cpu->priv), rpl = sel & 3u;
    unsigned type = (lo >> 40) & 0xF, gdpl = (lo >> 45) & 3;
    if (gdpl < cpl || gdpl < rpl)
        return GP(sel);
    if (!((lo >> 47) & 1))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    unsigned gsz = lm ? 64 : type == 0xC ? 32 : 16;     /* the frame's slot size */
    uint16_t csel = (uint16_t)(lo >> 16);
    uint64_t goff = (lo & 0xFFFF) | ((lo >> 32) & 0xFFFF0000ull);
    if (lm) {
        if (((hi >> 40) & 0x1F) != 0)                 /* the upper half's type must be 0 */
            return GP(sel);
        goff |= hi << 32;
    } else if (gsz == 16) {
        goff &= 0xFFFF;
    }
    unsigned pcount = lm ? 0 : (unsigned)(lo >> 32) & 0x1F;

    uint64_t clo = 0, chi = 0;
    uam_status st;
    if ((csel & ~3u) == 0)
        return GP(0);
    if ((st = x86_read_desc(cpu, csel, &clo, &chi, false)) != UAM_OK)
        return st;
    cdesc d;
    x86_desc_unpack(clo, &d.base, &d.limit, &d.attr);
    unsigned cdpl = (d.attr & SEGA_DPL) >> 5;
    bool conforming = (d.attr & 0x4) != 0;
    if (!(d.attr & SEGA_S) || !(d.attr & 0x8) || cdpl > cpl || (lm && !(d.attr & SEGA_L)))
        return GP(csel);
    if (!call && !conforming && cdpl != cpl)
        return GP(csel);
    if (!(d.attr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, csel & ~3u);

    if (!call) {
        *pc = set_cs(cpu, (uint16_t)((csel & ~3u) | cpl), &d, goff, gsz);
        return UAM_OK;
    }
    unsigned b = gsz / 8;
    uint16_t old_cs = x86_seg_sel(cpu, X86_SEG_CS);
    if (conforming || cdpl == cpl) {                  /* same privilege */
        stk k = cur_stack(cpu);
        if ((st = spush(cpu, &k, b, old_cs)) != UAM_OK || (st = spush(cpu, &k, b, ret_rip)) != UAM_OK)
            return st;
        stk_commit(cpu, &k);
        *pc = set_cs(cpu, (uint16_t)((csel & ~3u) | cpl), &d, goff, gsz);
        return UAM_OK;
    }

    /* to an inner level: the new stack from the TSS, then the old SS:ESP, the
     * parameters copied from the old stack, the return address */
    uint64_t nsp = 0;
    uint16_t nss = 0;
    if ((st = x86_tss_stack(cpu, lm, cdpl, 0, &nsp, &nss)) != UAM_OK)
        return st;
    cdesc sd = { 0, 0xFFFFFFFFu, SEGA_P | SEGA_S | 0x3 | SEGA_DB | SEGA_G };
    stk k;
    if (lm) {
        nss = (uint16_t)cdpl;
        sd.attr |= (uint16_t)(cdpl << 5);
        k = (stk){ nsp & ~0xFull, 0, ~0ull };
    } else {
        uint64_t slo = 0, shi = 0;
        if ((nss & ~3u) == 0 || (st = x86_read_desc(cpu, nss, &slo, &shi, false)) != UAM_OK)
            return x86_raise(cpu, X86_EXC_TS, true, nss & ~3u);
        x86_desc_unpack(slo, &sd.base, &sd.limit, &sd.attr);
        if ((nss & 3u) != cdpl || !(sd.attr & SEGA_S) || (sd.attr & 0x8) || !(sd.attr & 0x2) ||
            ((sd.attr & SEGA_DPL) >> 5) != cdpl)
            return x86_raise(cpu, X86_EXC_TS, true, nss & ~3u);
        if (!(sd.attr & SEGA_P))
            return x86_raise(cpu, X86_EXC_SS, true, nss & ~3u);
        k = (stk){ nsp, sd.base, (sd.attr & SEGA_DB) ? 0xFFFFFFFFull : 0xFFFFull };
        k.sp &= k.mask;
    }
    stk old = cur_stack(cpu);
    uint64_t old_ss = x86_seg_sel(cpu, X86_SEG_SS), old_sp = x86_gpr(cpu, X86_RSP);
    if ((st = kpush(cpu, &k, b, old_ss)) != UAM_OK || (st = kpush(cpu, &k, b, old_sp)) != UAM_OK)
        return st;
    for (unsigned i = pcount; i-- > 0;) {
        uint64_t v = 0;
        stk at = old;
        at.sp = (old.sp + (uint64_t)i * b) & old.mask;
        if ((st = x86_lin_read(cpu, stk_lin(&at), b, &v, false)) != UAM_OK ||
            (st = kpush(cpu, &k, b, v)) != UAM_OK)
            return st;
    }
    if ((st = kpush(cpu, &k, b, old_cs)) != UAM_OK || (st = kpush(cpu, &k, b, ret_rip)) != UAM_OK)
        return st;
    x86_seg_set(cpu, X86_SEG_SS, nss, sd.base, sd.limit, sd.attr);
    *pc = set_cs(cpu, (uint16_t)((csel & ~3u) | cdpl), &d, goff, gsz);   /* CPL = DPL */
    stk_commit(cpu, &k);
    (void)s;
    return UAM_OK;
}

/* Far JMP (call = false) or CALL in protected and long mode. */
static uam_status far_transfer(uam_cpu *cpu, uint16_t sel, bool call, unsigned osz, uint64_t off,
                               uint64_t ret_rip, uint64_t ret_lin, uint64_t *pc)
{
    uint64_t lo, hi;
    uam_status st = far_desc(cpu, sel, &lo, &hi);
    if (st != UAM_OK)
        return st;
    if (!((lo >> 44) & 1)) {                         /* a system descriptor */
        unsigned type = (lo >> 40) & 0xF;
        bool lm = x86_priv_mode(cpu->priv) == X86_MODE_LONG;
        if (type == 0xC || (type == 0x4 && !lm))
            return far_gate(cpu, sel, lo, hi, call, ret_rip, pc);
        if (type == 0x1 || type == 0x9 || type == 0x5)
            return far_task(cpu, sel, lo, call, ret_lin, pc);
        return GP(sel);
    }
    cdesc d;
    if ((st = check_direct_code(cpu, sel, lo, &d)) != UAM_OK)
        return st;
    if (call) {
        unsigned b = osz / 8;
        stk k = cur_stack(cpu);
        uint16_t old_cs = x86_seg_sel(cpu, X86_SEG_CS);
        if ((st = spush(cpu, &k, b, old_cs)) != UAM_OK || (st = spush(cpu, &k, b, ret_rip)) != UAM_OK)
            return st;
        stk_commit(cpu, &k);
    }
    *pc = set_cs(cpu, (uint16_t)((sel & ~3u) | x86_priv_cpl(cpu->priv)), &d, off, osz);
    return UAM_OK;
}

uam_status x86_far_jmp(uam_cpu *cpu, uint16_t sel, uint64_t off, uint64_t next_lin, uint64_t *pc)
{
    const unsigned osz = 32;   /* the lifter already cut the offset to the operand size */
    if (x86_priv_realseg(cpu->priv)) {
        *pc = real_cs(cpu, sel, off, osz);
        return UAM_OK;
    }
    return far_transfer(cpu, sel, false, osz, off, 0, next_lin, pc);
}

uam_status x86_far_call(uam_cpu *cpu, uint16_t sel, unsigned osz, uint64_t off,
                        uint64_t ret_rip, uint64_t *pc)
{
    if (x86_priv_realseg(cpu->priv)) {
        unsigned b = osz / 8;
        stk k = cur_stack(cpu);
        uam_status st;
        if ((st = spush(cpu, &k, b, x86_seg_sel(cpu, X86_SEG_CS))) != UAM_OK ||
            (st = spush(cpu, &k, b, ret_rip)) != UAM_OK)
            return st;
        stk_commit(cpu, &k);
        *pc = real_cs(cpu, sel, off, osz);
        return UAM_OK;
    }
    uint64_t ret_lin = x86_seg_base(cpu, X86_SEG_CS) + ret_rip;
    if (!code64(cpu))
        ret_lin &= 0xFFFFFFFFull;
    return far_transfer(cpu, sel, true, osz, off, ret_rip, ret_lin, pc);
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
    if (x86_priv_realseg(cpu->priv)) {
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
        uint64_t m = osz == 64 ? ~0ull : x86_priv_stack(cpu->priv) == 16 ? 0xFFFFull : 0xFFFFFFFFull;
        x86_set_gpr(cpu, X86_RSP, (nsp + imm16) & m);
        x86_null_outer_segs(cpu);
    }
    return UAM_OK;
}

/* IRET within virtual-8086 mode (IOPL 3): as in real mode, but IOPL and VM stay. */
static uam_status iret_v86(uam_cpu *cpu, unsigned osz, uint64_t *pc)
{
    uint32_t cur = x86_get_rflags(cpu);
    if (((cur & RFLAGS_IOPL) >> 12) < 3)
        return GP(0);
    unsigned b = osz / 8;
    stk k = cur_stack(cpu);
    uint64_t ip = 0, cs = 0, fl = 0;
    uam_status st;
    if ((st = spop(cpu, &k, b, &ip)) != UAM_OK || (st = spop(cpu, &k, b, &cs)) != UAM_OK ||
        (st = spop(cpu, &k, b, &fl)) != UAM_OK)
        return st;
    uint32_t mask = (osz == 16 ? 0xFFFFu : 0x3F7FD5u) & ~(uint32_t)(RFLAGS_IOPL | RFLAGS_VM |
                                                                   RFLAGS_VIF | RFLAGS_VIP);
    stk_commit(cpu, &k);
    x86_set_rflags(cpu, (cur & ~mask) | ((uint32_t)fl & mask));
    *pc = real_cs(cpu, (uint16_t)cs, ip, 16);
    x86_st(cpu)->nmi_blocked = false;
    uam_cpu_request_exit(cpu);
    return UAM_OK;
}

uam_status x86_iret(uam_cpu *cpu, unsigned osz, uint64_t next_lin, uint64_t *pc)
{
    x86_state *s = x86_st(cpu);
    unsigned b = osz / 8;
    unsigned cpl = x86_priv_cpl(cpu->priv);
    bool was64 = code64(cpu);
    enum x86_mode mode = x86_priv_mode(cpu->priv);
    if (mode == X86_MODE_V86)
        return iret_v86(cpu, osz, pc);
    uint32_t cur = x86_get_rflags(cpu);
    if (mode == X86_MODE_PROT && (cur & RFLAGS_NT)) {
        /* return from a nested task: to the TSS this one links back to */
        uint64_t link = 0;
        uam_status st = x86_lin_read(cpu, s->tr.base, 2, &link, true);
        if (st != UAM_OK)
            return st;
        return x86_task_switch(cpu, (uint16_t)link, X86_TASK_IRET, next_lin, false, 0, pc);
    }

    stk k = cur_stack(cpu);
    uint64_t ip = 0, cs = 0, fl = 0, nsp = 0, nss = 0;
    uam_status st;
    if ((st = spop(cpu, &k, b, &ip)) != UAM_OK || (st = spop(cpu, &k, b, &cs)) != UAM_OK ||
        (st = spop(cpu, &k, b, &fl)) != UAM_OK)
        return st;

    uint32_t mask = osz == 16 ? 0xFFFFu : 0x3F7FD5u;
    if (mode == X86_MODE_REAL) {
        stk_commit(cpu, &k);
        x86_set_rflags(cpu, (cur & ~mask) | ((uint32_t)fl & mask & ~(uint32_t)RFLAGS_VM));
        *pc = real_cs(cpu, (uint16_t)cs, ip, osz);
        s->nmi_blocked = false;
        uam_cpu_request_exit(cpu);
        return UAM_OK;
    }
    if ((fl & RFLAGS_VM) && cpl == 0 && osz == 32 && mode == X86_MODE_PROT) {
        /* into virtual-8086 mode: ESP, SS, ES, DS, FS, GS follow on the stack */
        uint64_t v[6];
        for (unsigned i = 0; i < 6; i++)
            if ((st = spop(cpu, &k, 4, &v[i])) != UAM_OK)
                return st;
        const uint16_t sel[X86_NSEG] = { [X86_SEG_ES] = (uint16_t)v[2], [X86_SEG_CS] = (uint16_t)cs,
                                         [X86_SEG_SS] = (uint16_t)v[1], [X86_SEG_DS] = (uint16_t)v[3],
                                         [X86_SEG_FS] = (uint16_t)v[4], [X86_SEG_GS] = (uint16_t)v[5] };
        x86_set_rflags(cpu, ((uint32_t)fl & 0x3F7FD5u) | RFLAGS_ALWAYS_1);
        x86_v86_segs(cpu, sel);
        x86_set_gpr(cpu, X86_RSP, v[0] & 0xFFFFFFFFull);
        x86_update_priv(cpu);
        *pc = ((uint64_t)cs << 4) + (ip & 0xFFFF);
        s->nmi_blocked = false;
        uam_cpu_request_exit(cpu);
        return UAM_OK;
    }
    mask &= ~(uint32_t)RFLAGS_VM;   /* VM changes only on the path above */

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
    if (outer)
        x86_null_outer_segs(cpu);
    s->nmi_blocked = false;
    uam_cpu_request_exit(cpu);
    return UAM_OK;
}
