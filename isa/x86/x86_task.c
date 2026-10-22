/*
 * x86_task.c - Hardware task switching, the TSS (inner stacks, the I/O
 * permission bitmap) and the segment registers of virtual-8086 mode.
 *
 * A task switch (JMP or CALL to a TSS or task gate, an interrupt or exception
 * through a task gate, IRET with NT set) follows the protected-mode sequence:
 * check the new TSS descriptor and read the new task's state, save the
 * current state into the old TSS, update the busy bits and the back link, then
 * commit - TR, CR0.TS, CR3, EFLAGS, the general registers, LDTR and the
 * segment registers. A fault while loading the new task's segments is raised
 * in the new task (x86_state.pend_in_task). 32-bit and 16-bit TSSes both work;
 * in IA-32e mode there are no task switches (#GP).
 */
#include "x86.h"

/* 32-bit TSS */
enum { T32_LINK = 0x00, T32_CR3 = 0x1C, T32_EIP = 0x20, T32_EFLAGS = 0x24, T32_GPR = 0x28,
       T32_SEG = 0x48, T32_LDT = 0x60, T32_IOMAP = 0x66, T32_MIN_LIMIT = 0x67 };
/* 16-bit TSS */
enum { T16_LINK = 0x00, T16_IP = 0x0E, T16_FLAGS = 0x10, T16_GPR = 0x12, T16_SEG = 0x22,
       T16_LDT = 0x2A, T16_MIN_LIMIT = 0x2B };

#define TSS_BUSY 0x2u   /* type bit 1 */

static bool tss32(uint16_t attr) { return (attr & 0x8) != 0; }

static uam_status rd(uam_cpu *cpu, uint64_t lin, unsigned n, uint64_t *v)
{
    return x86_lin_read(cpu, lin, n, v, true);
}

static uam_status wr(uam_cpu *cpu, uint64_t lin, unsigned n, uint64_t v)
{
    return x86_lin_write(cpu, lin, n, v, true);
}

/* ---- inner stacks --------------------------------------------------------- */

uam_status x86_tss_stack(uam_cpu *cpu, bool lm, unsigned cpl, unsigned ist, uint64_t *sp,
                         uint16_t *ss)
{
    x86_state *s = x86_st(cpu);
    uint64_t v = 0;
    uam_status st;
    if (lm) {
        uint32_t off = ist ? 0x24u + (ist - 1) * 8u : 4u + cpl * 8u;
        if (off + 7 > s->tr.limit)
            return x86_raise(cpu, X86_EXC_TS, true, s->tr.sel & ~3u);
        st = rd(cpu, s->tr.base + off, 8, &v);
        *sp = v;
        *ss = 0;
        return st;
    }
    bool t32 = tss32(s->tr.attr);
    uint32_t w = t32 ? 4u : 2u;                     /* ESPn/SSn, or SPn/SSn */
    uint32_t off = t32 ? 4u + cpl * 8u : 2u + cpl * 4u;
    if (off + w + 1 > s->tr.limit)
        return x86_raise(cpu, X86_EXC_TS, true, s->tr.sel & ~3u);
    if ((st = rd(cpu, s->tr.base + off, w, &v)) != UAM_OK)
        return st;
    *sp = v;
    if ((st = rd(cpu, s->tr.base + off + w, 2, &v)) != UAM_OK)
        return st;
    *ss = (uint16_t)v;
    return UAM_OK;
}

/* ---- the I/O permission bitmap -------------------------------------------- */

uam_status x86_io_permitted(uam_cpu *cpu, unsigned port, unsigned size)
{
    x86_state *s = x86_st(cpu);
    enum x86_mode mode = x86_priv_mode(cpu->priv);
    if (mode == X86_MODE_REAL)
        return UAM_OK;
    unsigned iopl = (x86_get_rflags(cpu) & RFLAGS_IOPL) >> 12;
    if (mode != X86_MODE_V86 && x86_priv_cpl(cpu->priv) <= iopl)
        return UAM_OK;
    /* the bitmap: a 32-bit or 64-bit TSS only; every bit of the access clear */
    uint64_t base = 0, bits = 0;
    if (!(s->tr.attr & SEGA_P) || !tss32(s->tr.attr) || s->tr.limit < T32_MIN_LIMIT)
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    if (rd(cpu, s->tr.base + T32_IOMAP, 2, &base) != UAM_OK)
        return UAM_ERR_FAULT;
    uint64_t at = base + port / 8;
    if (at + 1 > s->tr.limit)
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    if (rd(cpu, s->tr.base + at, 2, &bits) != UAM_OK)
        return UAM_ERR_FAULT;
    if ((bits >> (port & 7)) & ((1u << size) - 1))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    return UAM_OK;
}

/* ---- virtual-8086 segments ------------------------------------------------- */

void x86_v86_segs(uam_cpu *cpu, const uint16_t sel[X86_NSEG])
{
    const uint16_t attr = SEGA_P | SEGA_S | SEGA_DPL | 0x3;   /* data, read/write, DPL 3 */
    for (unsigned i = 0; i < X86_NSEG; i++)
        x86_seg_set(cpu, i, sel[i], (uint64_t)sel[i] << 4, 0xFFFF, attr);
}

/* ---- task switch ------------------------------------------------------------ */

/* A task's state as a TSS holds it. */
typedef struct tstate {
    uint64_t cr3;
    uint32_t eip, eflags;
    uint32_t gpr[8];
    uint16_t seg[X86_NSEG];
    uint16_t ldt;
} tstate;

static uam_status read_tss(uam_cpu *cpu, uint64_t base, bool t32, tstate *t)
{
    uint64_t v = 0;
    uam_status st;
    *t = (tstate){ 0 };
#define R(off, n, dst) do { if ((st = rd(cpu, base + (off), (n), &v)) != UAM_OK) return st; \
                            (dst) = v; } while (0)
    if (t32) {
        R(T32_CR3, 4, t->cr3);
        R(T32_EIP, 4, t->eip);
        R(T32_EFLAGS, 4, t->eflags);
        for (unsigned i = 0; i < 8; i++)
            R(T32_GPR + 4 * i, 4, t->gpr[i]);
        for (unsigned i = 0; i < X86_NSEG; i++)
            R(T32_SEG + 4 * i, 2, t->seg[i]);
        R(T32_LDT, 2, t->ldt);
    } else {
        R(T16_IP, 2, t->eip);
        R(T16_FLAGS, 2, t->eflags);
        for (unsigned i = 0; i < 8; i++)
            R(T16_GPR + 2 * i, 2, t->gpr[i]);
        for (unsigned i = 0; i < 4; i++)               /* ES CS SS DS */
            R(T16_SEG + 2 * i, 2, t->seg[i]);
        R(T16_LDT, 2, t->ldt);
    }
#undef R
    return UAM_OK;
}

static uam_status write_tss(uam_cpu *cpu, uint64_t base, bool t32, const tstate *t)
{
    uam_status st;
#define W(off, n, val) do { if ((st = wr(cpu, base + (off), (n), (val))) != UAM_OK) return st; } while (0)
    if (t32) {
        W(T32_EIP, 4, t->eip);
        W(T32_EFLAGS, 4, t->eflags);
        for (unsigned i = 0; i < 8; i++)
            W(T32_GPR + 4 * i, 4, t->gpr[i]);
        for (unsigned i = 0; i < X86_NSEG; i++)
            W(T32_SEG + 4 * i, 2, t->seg[i]);
    } else {
        W(T16_IP, 2, t->eip & 0xFFFF);
        W(T16_FLAGS, 2, t->eflags & 0xFFFF);
        for (unsigned i = 0; i < 8; i++)
            W(T16_GPR + 2 * i, 2, t->gpr[i] & 0xFFFF);
        for (unsigned i = 0; i < 4; i++)
            W(T16_SEG + 2 * i, 2, t->seg[i]);
    }
#undef W
    return UAM_OK;
}

/* Set or clear the busy bit of the TSS descriptor `sel` in the GDT. */
static uam_status set_busy(uam_cpu *cpu, uint16_t sel, bool busy)
{
    x86_state *s = x86_st(cpu);
    uint64_t lin = s->gdtr.base + (sel & ~7u) + 5, b = 0;   /* the type/access byte */
    uam_status st = rd(cpu, lin, 1, &b);
    if (st != UAM_OK)
        return st;
    b = busy ? (b | TSS_BUSY) : (b & ~(uint64_t)TSS_BUSY);
    return wr(cpu, lin, 1, b);
}

/* A fault while loading the new task: #TS (or #NP / #SS) in its context. */
static uam_status task_fault(uam_cpu *cpu, unsigned vec, uint16_t sel, uint64_t pc)
{
    x86_state *s = x86_st(cpu);
    uam_status st = x86_raise(cpu, vec, true, sel & ~3u);
    s->pend_in_task = true;
    s->pend_pc = pc;
    return st;
}

/* Load one segment register of the new task (not virtual-8086), with the
 * checks a task switch applies. */
static uam_status load_task_seg(uam_cpu *cpu, unsigned seg, uint16_t sel, unsigned cpl, uint64_t pc)
{
    unsigned rpl = sel & 3u;
    if ((sel & ~3u) == 0) {
        if (seg == X86_SEG_CS || seg == X86_SEG_SS)
            return task_fault(cpu, X86_EXC_TS, sel, pc);
        x86_seg_set(cpu, seg, sel, 0, 0, 0);
        return UAM_OK;
    }
    uint64_t lo = 0, hi = 0, base;
    uint32_t limit;
    uint16_t attr;
    if (x86_read_desc(cpu, sel, &lo, &hi, false) != UAM_OK)
        return task_fault(cpu, X86_EXC_TS, sel, pc);
    x86_desc_unpack(lo, &base, &limit, &attr);
    unsigned dpl = (attr & SEGA_DPL) >> 5;
    bool code = (attr & 0x8) != 0, conforming = code && (attr & 0x4);
    bool ok = (attr & SEGA_S) != 0;
    if (seg == X86_SEG_CS)
        ok = ok && code && (conforming ? dpl <= rpl : dpl == rpl);
    else if (seg == X86_SEG_SS)
        ok = ok && !code && (attr & 0x2) && dpl == cpl && rpl == cpl;
    else
        ok = ok && (!code || (attr & 0x2)) && (conforming || (dpl >= cpl && dpl >= rpl));
    if (!ok)
        return task_fault(cpu, X86_EXC_TS, sel, pc);
    if (!(attr & SEGA_P))
        return task_fault(cpu, seg == X86_SEG_SS ? X86_EXC_SS : X86_EXC_NP, sel, pc);
    x86_seg_set(cpu, seg, sel, base, limit, attr);
    return UAM_OK;
}

uam_status x86_task_switch(uam_cpu *cpu, uint16_t sel, enum x86_task_reason why, uint64_t ret,
                           bool has_err, uint32_t err, uint64_t *pc)
{
    x86_state *s = x86_st(cpu);
    uam_status st;
    if (x86_priv_mode(cpu->priv) == X86_MODE_LONG)
        return x86_raise(cpu, X86_EXC_GP, true, sel & ~3u);

    /* the new TSS descriptor: in the GDT, a TSS, available (busy for IRET) */
    uint64_t lo = 0, hi = 0, nbase;
    uint32_t nlimit;
    uint16_t nattr;
    if (sel & 4)
        return x86_raise(cpu, why == X86_TASK_IRET ? X86_EXC_TS : X86_EXC_GP, true, sel & ~3u);
    if ((st = x86_read_desc(cpu, sel, &lo, &hi, false)) != UAM_OK)
        return st;
    x86_desc_unpack(lo, &nbase, &nlimit, &nattr);
    unsigned type = nattr & SEGA_TYPE;
    bool ntype_ok = !(nattr & SEGA_S) && (type == 0x1 || type == 0x3 || type == 0x9 || type == 0xB);
    bool nbusy = (type & TSS_BUSY) != 0;
    if (!ntype_ok || (why == X86_TASK_IRET) != nbusy)
        return x86_raise(cpu, why == X86_TASK_IRET ? X86_EXC_TS : X86_EXC_GP, true, sel & ~3u);
    if (!(nattr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    bool n32 = tss32(nattr);
    if (nlimit < (n32 ? (uint32_t)T32_MIN_LIMIT : (uint32_t)T16_MIN_LIMIT))
        return x86_raise(cpu, X86_EXC_TS, true, sel & ~3u);
    bool o32 = tss32(s->tr.attr);
    if (!(s->tr.attr & SEGA_P) || s->tr.limit < (o32 ? (uint32_t)T32_MIN_LIMIT : (uint32_t)T16_MIN_LIMIT))
        return x86_raise(cpu, X86_EXC_TS, true, s->tr.sel & ~3u);

    /* the new task's state, read before anything changes */
    tstate nt;
    if ((st = read_tss(cpu, nbase, n32, &nt)) != UAM_OK)
        return st;

    /* save the current task */
    tstate ot;
    ot.eip = (uint32_t)(ret - x86_seg_base(cpu, X86_SEG_CS));
    ot.eflags = x86_get_rflags(cpu);
    if (why == X86_TASK_IRET)
        ot.eflags &= ~(uint32_t)RFLAGS_NT;
    for (unsigned i = 0; i < 8; i++)
        ot.gpr[i] = (uint32_t)x86_gpr(cpu, i);
    for (unsigned i = 0; i < X86_NSEG; i++)
        ot.seg[i] = x86_seg_sel(cpu, i);
    if ((st = write_tss(cpu, s->tr.base, o32, &ot)) != UAM_OK)
        return st;

    /* busy bits and the back link */
    uint16_t old_tr = s->tr.sel;
    if (why == X86_TASK_JMP || why == X86_TASK_IRET)
        if ((st = set_busy(cpu, old_tr, false)) != UAM_OK)
            return st;
    if (why == X86_TASK_CALL || why == X86_TASK_INT) {
        if ((st = wr(cpu, nbase + (n32 ? T32_LINK : T16_LINK), 2, old_tr)) != UAM_OK)
            return st;
        nt.eflags |= RFLAGS_NT;
    }
    if (why != X86_TASK_IRET && (st = set_busy(cpu, sel, true)) != UAM_OK)
        return st;

    /* commit: from here on, faults belong to the new task */
    s->tr = (x86_sysseg){ nbase, nlimit, sel, (uint16_t)(nattr | TSS_BUSY) };
    s->cr0 |= CR0_TS;
    if (n32 && (s->cr0 & CR0_PG) && nt.cr3 != s->cr3) {
        s->cr3 = nt.cr3;
        x86_tlb_flush(cpu);
        uam_cpu_xlate_changed(cpu);
    }
    uint32_t fl = n32 ? nt.eflags : (x86_get_rflags(cpu) & 0xFFFF0000u & ~(uint32_t)RFLAGS_VM) | (nt.eflags & 0xFFFF);
    x86_set_rflags(cpu, fl | RFLAGS_ALWAYS_1);
    for (unsigned i = 0; i < 8; i++) {
        if (n32)
            x86_set_gpr(cpu, i, nt.gpr[i]);
        else
            uam_cpu_reg_write(cpu, X86_OFF_GPR(i), 16, nt.gpr[i]);
    }

    uint64_t npc;
    if (fl & RFLAGS_VM) {
        /* into virtual-8086 mode: every segment loads as in real mode */
        x86_v86_segs(cpu, nt.seg);
        npc = ((uint64_t)nt.seg[X86_SEG_CS] << 4) + (nt.eip & 0xFFFF);
    } else {
        /* the new pc a fault in the new task is reported at; then the checks */
        uint64_t cslo = 0, cshi = 0, csbase = 0;
        uint32_t cslim;
        uint16_t csattr;
        if ((nt.seg[X86_SEG_CS] & ~3u) && x86_read_desc(cpu, nt.seg[X86_SEG_CS], &cslo, &cshi, false) == UAM_OK)
            x86_desc_unpack(cslo, &csbase, &cslim, &csattr);
        npc = (csbase + (n32 ? nt.eip : nt.eip & 0xFFFF)) & 0xFFFFFFFFull;
        x86_seg_set(cpu, X86_SEG_CS, nt.seg[X86_SEG_CS], csbase, 0xFFFF, SEGA_P | SEGA_S | 0xB);
        cpu->pc = npc;
        /* LDTR first: the other selectors may name LDT descriptors */
        if ((nt.ldt & ~3u) == 0) {
            s->ldtr = (x86_sysseg){ 0, 0, nt.ldt, 0 };
        } else {
            uint64_t llo = 0, lhi = 0, lbase;
            uint32_t llim;
            uint16_t lattr;
            if ((nt.ldt & 4) || x86_read_desc(cpu, nt.ldt, &llo, &lhi, false) != UAM_OK)
                return task_fault(cpu, X86_EXC_TS, nt.ldt, npc);
            x86_desc_unpack(llo, &lbase, &llim, &lattr);
            if ((lattr & SEGA_S) || (lattr & SEGA_TYPE) != 0x2)
                return task_fault(cpu, X86_EXC_TS, nt.ldt, npc);
            if (!(lattr & SEGA_P))
                return task_fault(cpu, X86_EXC_NP, nt.ldt, npc);
            s->ldtr = (x86_sysseg){ lbase, llim, nt.ldt, lattr };
        }
        unsigned cpl = nt.seg[X86_SEG_CS] & 3u;
        static const unsigned order[] = { X86_SEG_CS, X86_SEG_SS, X86_SEG_DS, X86_SEG_ES,
                                          X86_SEG_FS, X86_SEG_GS };
        for (unsigned k = 0; k < UAM_ARRAY_LEN(order); k++)
            if ((st = load_task_seg(cpu, order[k], nt.seg[order[k]], cpl, npc)) != UAM_OK) {
                x86_update_priv(cpu);
                return st;
            }
    }
    x86_update_priv(cpu);
    cpu->pc = npc;

    /* an exception's error code goes on the new task's stack */
    if (has_err) {
        unsigned b = n32 ? 4 : 2;
        uint64_t mask = x86_priv_stack(cpu->priv) == 32 ? 0xFFFFFFFFull : 0xFFFFull;
        uint64_t sp = (x86_gpr(cpu, X86_RSP) - b) & mask;
        uint64_t lin = (x86_seg_base(cpu, X86_SEG_SS) + sp) & 0xFFFFFFFFull;
        if (wr(cpu, lin, b, err) != UAM_OK) {
            s->pend_in_task = true;
            s->pend_pc = npc;
            return UAM_ERR_FAULT;
        }
        if (mask == 0xFFFFull)
            uam_cpu_reg_write(cpu, X86_OFF_GPR(X86_RSP), 16, sp);
        else
            x86_set_gpr(cpu, X86_RSP, sp);
    }
    s->nmi_blocked = s->nmi_blocked && why != X86_TASK_IRET;
    uam_cpu_request_exit(cpu);
    *pc = npc;
    return UAM_OK;
}
