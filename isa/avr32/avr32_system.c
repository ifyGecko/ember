/*
 * avr32_system.c - AVR32 privileged architecture (AVR32 Architecture Manual,
 * chapters 2, 5, 6 and 8) and the uam_isa descriptor.
 *
 * Implemented: the status register and the eight execution modes with the
 * shadowed SP (and on AVR32B the optional INT3 shadow of R8-R12 and LR), the
 * event system of both microarchitectures (AVR32A saves SR and PC, and for
 * interrupts R8-R12 and LR, on the system stack; AVR32B uses the per-context
 * RSR/RAR registers), autovectored interrupts at four levels plus NMI, scall /
 * rets / rete / retd, the breakpoint instruction's debug entry, the system
 * registers, COUNT/COMPARE (COUNT runs at the scheduler tick rate and is cleared
 * on a match; the match is an output line for the interrupt controller), the
 * MPU of AVR32A (8 regions of 16 subregions) and the segmented memory map and
 * MMU of AVR32B (ITLB and DTLB, 1 KB to 1 MB pages, private or shared virtual
 * memory, tlbr / tlbw / tlbs).
 *
 * Not modelled: caches (the cache instruction does nothing), the TLB Accessed
 * bits (TLBARLO/HI keep what tlbw and software leave there: translation has no
 * side effects), performance counters (storage only), OCD and debug registers,
 * the Java state and the secure state.
 */
#include <stdlib.h>
#include <string.h>

#include "avr32.h"
#include "uam/bus.h"

/* ------------------------------------------------------------------------ */
/* Register descriptors                                                      */
/* ------------------------------------------------------------------------ */

#define R(n, i) { n, A32_REG_OFF(i), 32, UAM_REG_GPR }
#define FL(n, o) { n, o, 8, UAM_REG_FLAG }
static const uam_regdesc avr32_regs[] = {
    R("r0", 0),  R("r1", 1),   R("r2", 2),   R("r3", 3),   R("r4", 4),   R("r5", 5),
    R("r6", 6),  R("r7", 7),   R("r8", 8),   R("r9", 9),   R("r10", 10), R("r11", 11),
    R("r12", 12), R("sp", 13), R("lr", 14),
    FL("c", A32_F_C), FL("z", A32_F_Z), FL("n", A32_F_N), FL("v", A32_F_V), FL("q", A32_F_Q),
    FL("l", A32_F_L), FL("t", A32_F_T),
};
#undef R
#undef FL

static bool is_b(const avr32_state *s) { return s->cfg.arch == UAM_AVR32_B; }

/* ------------------------------------------------------------------------ */
/* SR and register banking                                                   */
/* ------------------------------------------------------------------------ */

static const uint32_t flag_off[7] = { A32_F_C, A32_F_Z, A32_F_N, A32_F_V, A32_F_Q, A32_F_L, A32_F_T };
static const uint32_t flag_bit[7] = { SR_C, SR_Z, SR_N, SR_V, SR_Q, SR_L, SR_T };

uint32_t avr32_sr_read(uam_cpu *cpu)
{
    uint32_t sr = a32_st(cpu)->sr_hi;
    for (unsigned i = 0; i < 7; i++)
        if (cpu->regfile[flag_off[i]])
            sr |= flag_bit[i];
    return sr;
}

static uint32_t reg(uam_cpu *cpu, unsigned r) { return (uint32_t)uam_cpu_reg_read(cpu, A32_REG_OFF(r), 32); }
static void set_reg(uam_cpu *cpu, unsigned r, uint32_t v) { uam_cpu_reg_write(cpu, A32_REG_OFF(r), 32, v); }

static void swap_reg(uam_cpu *cpu, unsigned r, uint32_t *saved)
{
    uint32_t cur = reg(cpu, r);
    set_reg(cpu, r, *saved);
    *saved = cur;
}

/* The register file follows a mode change: SP_APP / SP_SYS, and INT3's bank. */
static void switch_bank(uam_cpu *cpu, unsigned from, unsigned to)
{
    avr32_state *s = a32_st(cpu);
    if (to == A32_MODE_APP && from != A32_MODE_APP) {
        s->sp_sys = reg(cpu, A32_SP);
        set_reg(cpu, A32_SP, s->sp_app);
    } else if (from == A32_MODE_APP && to != A32_MODE_APP) {
        s->sp_app = reg(cpu, A32_SP);
        set_reg(cpu, A32_SP, s->sp_sys);
    }
    if (is_b(s) && s->cfg.int3_shadow && (from == A32_MODE_INT3) != (to == A32_MODE_INT3)) {
        static const unsigned banked[6] = { 8, 9, 10, 11, 12, A32_LR };
        for (unsigned i = 0; i < 6; i++)
            swap_reg(cpu, banked[i], &s->shadow[i]);
    }
}

/* Write the whole SR: flags, mode (with the register banks), masks. */
void avr32_sr_write(uam_cpu *cpu, uint32_t sr)
{
    avr32_state *s = a32_st(cpu);
    for (unsigned i = 0; i < 7; i++)
        cpu->regfile[flag_off[i]] = (sr & flag_bit[i]) ? 1 : 0;
    unsigned from = a32_mode(s);
    uint32_t old = s->sr_hi;
    s->sr_hi = sr & SR_HI_MASK;
    unsigned to = a32_mode(s);
    if (from != to)
        switch_bank(cpu, from, to);
    cpu->priv = to == A32_MODE_APP ? A32_PRIV_APP : A32_PRIV_SYS;
    if (old != s->sr_hi)
        uam_cpu_request_exit(cpu);   /* masks, mode: an interrupt may now be taken */
}

/* ------------------------------------------------------------------------ */
/* Memory on behalf of the system code (event stacking, helpers)             */
/* ------------------------------------------------------------------------ */

static uam_status avr32_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out);

uam_status avr32_mem_rw(uam_cpu *cpu, uint32_t va, unsigned size, bool store, uint64_t *val)
{
    uam_xlate xl = { 0 };
    if (avr32_translate(cpu, va, UAM_XLATE_DEBUG, &xl) != UAM_OK)
        xl.paddr = va;
    return store ? uam_bus_write(cpu->bus, xl.paddr, size, UAM_ACC_BE, *val)
                 : uam_bus_read(cpu->bus, xl.paddr, size, UAM_ACC_BE, val);
}

/* The system stack: SP_SYS, in the register file in a privileged mode. */
static uint32_t sp_sys(uam_cpu *cpu)
{
    avr32_state *s = a32_st(cpu);
    return a32_mode(s) == A32_MODE_APP ? s->sp_sys : reg(cpu, A32_SP);
}
static void set_sp_sys(uam_cpu *cpu, uint32_t v)
{
    avr32_state *s = a32_st(cpu);
    if (a32_mode(s) == A32_MODE_APP)
        s->sp_sys = v;
    else
        set_reg(cpu, A32_SP, v);
}
static void push_sys(uam_cpu *cpu, uint32_t v)
{
    uint32_t sp = sp_sys(cpu) - 4;
    uint64_t x = v;
    (void)avr32_mem_rw(cpu, sp, 4, true, &x);
    set_sp_sys(cpu, sp);
}
static uint32_t pop_sys(uam_cpu *cpu)
{
    uint32_t sp = sp_sys(cpu);
    uint64_t x = 0;
    (void)avr32_mem_rw(cpu, sp, 4, false, &x);
    set_sp_sys(cpu, sp + 4);
    return (uint32_t)x;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

/* Enter `mode` for an event, saving SR and ret_pc (AVR32A: on the system stack, with
 * R8-R12 and LR first for an interrupt; AVR32B: in RSR/RAR of the new mode), setting
 * `masks`, and continuing at EVBA + offset. */
static void enter_event(uam_cpu *cpu, unsigned mode, uint32_t masks, uint32_t offset, uint32_t ret_pc,
                        bool ecr)
{
    avr32_state *s = a32_st(cpu);
    uint32_t sr = avr32_sr_read(cpu);
    bool irq = mode >= A32_MODE_INT0 && mode <= A32_MODE_INT3;
    if (is_b(s)) {
        s->rsr[mode] = sr;
        s->rar[mode] = ret_pc;
    } else {
        if (irq) {
            static const unsigned saved[6] = { 8, 9, 10, 11, 12, A32_LR };
            for (unsigned i = 0; i < 6; i++)
                push_sys(cpu, reg(cpu, saved[i]));
        }
        push_sys(cpu, ret_pc);
        push_sys(cpu, sr);
    }
    if (ecr)
        s->ecr = (offset >> 2) & 0x7F;
    uint32_t nsr = (sr & ~(SR_M | SR_R | SR_J)) | (uint32_t)mode << SR_M_SHIFT | masks;
    avr32_sr_write(cpu, nsr);
    cpu->resv.valid = false;
    cpu->pc = (s->evba + offset) & ~1u;
}

static void enter_exception(uam_cpu *cpu, uint32_t offset, uint32_t pc)
{
    avr32_state *s = a32_st(cpu);
    /* an exception while exceptions are masked is unrecoverable */
    if ((s->sr_hi & SR_EM) && offset != EV_UNRECOVERABLE && offset != EV_TLB_MULTI)
        offset = EV_UNRECOVERABLE;
    enter_event(cpu, A32_MODE_EX, SR_EM | SR_GM, offset, pc, true);
}

/* The MMU registers after a translation fault at va (I: the instruction side). */
static void mmu_fault(avr32_state *s, uint32_t va, bool ins)
{
    s->tlbear = va;
    s->tlbehi = (va & 0xFFFFFC00u) | (1u << 9) | (ins ? 1u << 8 : 0) | (s->tlbehi & 0xFFu);
}

static void do_scall(uam_cpu *cpu, uint32_t pc)
{
    avr32_state *s = a32_st(cpu);
    unsigned mode = a32_mode(s);
    uint32_t ret = pc + 2;
    if (mode == A32_MODE_APP || mode == A32_MODE_SUP) {
        uint32_t sr = avr32_sr_read(cpu);
        if (is_b(s)) {
            s->rar[A32_MODE_SUP] = ret;
            s->rsr[A32_MODE_SUP] = sr;
        } else {
            push_sys(cpu, ret);
            push_sys(cpu, sr);
        }
        avr32_sr_write(cpu, (sr & ~SR_M) | (uint32_t)A32_MODE_SUP << SR_M_SHIFT);
    } else {
        set_reg(cpu, A32_LR, ret);
    }
    cpu->pc = s->evba + EV_SCALL;
}

static void do_breakpoint(uam_cpu *cpu, uint32_t pc)
{
    avr32_state *s = a32_st(cpu);
    if (s->sr_hi & SR_DM) {           /* debug state masked: a no-op */
        cpu->pc = pc + 2;
        return;
    }
    uint32_t sr = avr32_sr_read(cpu);
    s->rsr_dbg = sr;
    s->rar_dbg = pc;
    avr32_sr_write(cpu, (sr & ~SR_M) | (uint32_t)A32_MODE_EX << SR_M_SHIFT | SR_D | SR_DM | SR_EM | SR_GM);
    cpu->pc = s->evba + EV_BREAKPOINT;
}

static void avr32_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    avr32_state *s = a32_st(cpu);
    uint32_t pc = (uint32_t)e->pc, va = (uint32_t)e->tval, off;
    switch (e->kind) {
    case UAM_EXC_TRAP:
        if (e->cause == A32_TRAP_SCALL) {
            do_scall(cpu, pc);
            return;
        }
        if (e->cause == A32_TRAP_BREAKPOINT) {
            do_breakpoint(cpu, pc);
            return;
        }
        off = (uint32_t)e->cause;
        if (off == EV_INSN_ADDR)
            mmu_fault(s, pc, true);
        break;
    case UAM_EXC_FETCH_FAULT:      off = EV_BUS_INSN; s->bear = va; break;
    case UAM_EXC_LOAD_FAULT:
    case UAM_EXC_STORE_FAULT:      off = EV_BUS_DATA; s->bear = va; break;
    case UAM_EXC_LOAD_MISALIGNED:  off = EV_DATA_ADDR_R; mmu_fault(s, va, false); break;
    case UAM_EXC_STORE_MISALIGNED: off = EV_DATA_ADDR_W; mmu_fault(s, va, false); break;
    case UAM_EXC_FETCH_XLATE:
        off = e->cause == A32_XF_MISS ? EV_ITLB_MISS : e->cause == A32_XF_MULTI ? EV_TLB_MULTI : EV_ITLB_PROT;
        mmu_fault(s, va, true);
        break;
    case UAM_EXC_LOAD_XLATE:
        off = e->cause == A32_XF_MISS ? EV_DTLB_MISS_R : e->cause == A32_XF_MULTI ? EV_TLB_MULTI : EV_DTLB_PROT_R;
        mmu_fault(s, va, false);
        break;
    case UAM_EXC_STORE_XLATE:
        off = e->cause == A32_XF_MISS ? EV_DTLB_MISS_W : e->cause == A32_XF_MULTI ? EV_TLB_MULTI
            : e->cause == A32_XF_MOD ? EV_DTLB_MOD : EV_DTLB_PROT_W;
        mmu_fault(s, va, false);
        break;
    case UAM_EXC_SYSREG:
    case UAM_EXC_HELPER:
    default:
        off = EV_PRIV;
        break;
    }
    enter_exception(cpu, off, pc);
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                                */
/* ------------------------------------------------------------------------ */

static bool nmi_pending(const avr32_state *s)
{
    return (s->lines & (1u << UAM_AVR32_IRQ_NMI)) && a32_mode(s) != A32_MODE_NMI;
}

/* The highest interrupt level pending and not masked, or -1 (`global`: obey GM). */
static int int_level(const avr32_state *s, bool global)
{
    if (global && (s->sr_hi & SR_GM))
        return -1;
    for (int l = 3; l >= 0; l--)
        if ((s->lines & (1u << l)) && !(s->sr_hi & (SR_I0M << l)))
            return l;
    return -1;
}

static bool avr32_irq_take(uam_cpu *cpu)
{
    avr32_state *s = a32_st(cpu);
    if (nmi_pending(s)) {
        enter_event(cpu, A32_MODE_NMI, SR_EM | SR_GM, EV_NMI, (uint32_t)cpu->pc, true);
        return true;
    }
    int l = int_level(s, true);
    if (l < 0)
        return false;
    uint32_t vec = s->autovector ? s->autovector(s->autovector_ctx, (unsigned)l) & 0x3FFF : 0;
    /* INTn masks its own level and every lower one */
    uint32_t masks = 0;
    for (int k = 0; k <= l; k++)
        masks |= SR_I0M << k;
    enter_event(cpu, A32_MODE_INT0 + (unsigned)l, masks, vec, (uint32_t)cpu->pc, true);
    return true;
}

static uint64_t avr32_irq_lines(uam_cpu *cpu)
{
    avr32_state *s = a32_st(cpu);
    unsigned mode = a32_mode(s);
    if (mode == A32_MODE_NMI)
        return 1u << UAM_AVR32_IRQ_NMI;
    if (mode >= A32_MODE_INT0 && mode <= A32_MODE_INT3)
        return 1u << (mode - A32_MODE_INT0);
    return 0;
}

static bool avr32_irq_wakeup(uam_cpu *cpu)
{
    avr32_state *s = a32_st(cpu);
    return nmi_pending(s) || int_level(s, false) >= 0;
}

static void avr32_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    avr32_state *s = a32_st(cpu);
    if (line > UAM_AVR32_IRQ_NMI)
        return;
    if (level)
        s->lines |= 1u << line;
    else
        s->lines &= ~(1u << line);
}

/* ------------------------------------------------------------------------ */
/* COUNT / COMPARE                                                           */
/* ------------------------------------------------------------------------ */

static uint32_t count_now(uam_cpu *cpu)
{
    return (uint32_t)(uam_sched_now(cpu->sched) + a32_st(cpu)->count_bias);
}

static void timer_update(uam_cpu *cpu)
{
    avr32_state *s = a32_st(cpu);
    if (!cpu->sched)
        return;
    if (!s->compare) {
        uam_timer_cancel(cpu->sched, &s->timer);
        return;
    }
    uint64_t delta = (uint32_t)(s->compare - count_now(cpu));
    if (!delta)
        delta = UINT64_C(1) << 32;   /* COUNT is at COMPARE now: the next match is a wrap away */
    uam_timer_arm(cpu->sched, &s->timer, uam_sched_now(cpu->sched) + delta);
}

static void timer_fire(void *ctx)
{
    uam_cpu *cpu = ctx;
    avr32_state *s = a32_st(cpu);
    /* COUNT == COMPARE: request the interrupt and clear COUNT */
    s->count_bias = (uint64_t)0 - uam_sched_now(cpu->sched);
    uam_signal_set(&s->compare_line, 1);
    timer_update(cpu);
}

/* ------------------------------------------------------------------------ */
/* Address translation                                                       */
/* ------------------------------------------------------------------------ */

/* MPU access permission codes (Table 6-3): bit 0 read, 1 write, 2 execute, for
 * the privileged (low nibble) and unprivileged (high nibble) modes. */
static const uint8_t mpu_ap[16] = {
    0x01, 0x05, 0x03, 0x07, 0x11, 0x55, 0x33, 0x77, 0x13, 0x53, 0x00,
};

static uam_status mpu_translate(const avr32_state *s, uint32_t va, uam_xlate_kind kind, bool app,
                                uam_xlate *out)
{
    out->paddr = va;
    if (kind == UAM_XLATE_DEBUG || !(s->mpucr & 1))
        return UAM_OK;
    int hit = -1;
    for (unsigned n = 0; n < s->cfg.mmu_entries && n < A32_MPU_REGIONS; n++) {
        uint32_t ar = s->mpuar[n];
        if (!(ar & 1))
            continue;
        unsigned sz = (ar >> 1) & 31;
        if (sz < 11)
            continue;   /* UNDEFINED sizes: not considered */
        uint64_t size = UINT64_C(1) << (sz + 1);
        uint64_t base = ar & 0xFFFFF000u & ~(size - 1);
        if (va >= base && va < base + size) {
            if (hit >= 0) {
                out->fault = A32_XF_MULTI;
                return UAM_ERR_XLATE;
            }
            hit = (int)n;
        }
    }
    if (hit < 0) {
        out->fault = A32_XF_MISS;
        return UAM_ERR_XLATE;
    }
    uint32_t ar = s->mpuar[hit];
    uint64_t size = UINT64_C(1) << (((ar >> 1) & 31) + 1);
    unsigned sub = (unsigned)(((va - (ar & 0xFFFFF000u & ~(uint32_t)(size - 1))) / (size / 16)) & 15);
    uint32_t apr = (s->mpupsr[hit] >> sub) & 1 ? s->mpuaprb : s->mpuapra;
    uint8_t ap = mpu_ap[(apr >> (4 * hit)) & 15];
    unsigned need = kind == UAM_XLATE_STORE ? 2 : kind == UAM_XLATE_FETCH ? 4 : 1;
    if (!((app ? ap >> 4 : ap) & need)) {
        out->fault = A32_XF_PROT;
        return UAM_ERR_XLATE;
    }
    return UAM_OK;
}

static const uint32_t page_mask[4] = { 0x3FFu, 0xFFFu, 0xFFFFu, 0xFFFFFu };

/* The entry of `tlb` that maps va, or -1; -2 for several. */
static int tlb_match(const avr32_state *s, const avr32_tlb_entry *tlb, uint32_t va)
{
    bool shared = s->mmucr & (1u << 1);   /* M: shared virtual memory */
    int hit = -1;
    for (unsigned i = 0; i < s->cfg.mmu_entries; i++) {
        const avr32_tlb_entry *e = &tlb[i];
        if (!(e->hi & (1u << 9)))
            continue;
        uint32_t m = page_mask[(e->lo >> 2) & 3];
        if (((e->hi ^ va) & ~m & 0xFFFFFC00u) != 0)
            continue;
        if (!shared && !(e->lo & (1u << 8)) && (e->hi & 0xFF) != (s->tlbehi & 0xFF))
            continue;
        if (hit >= 0)
            return -2;
        hit = (int)i;
    }
    return hit;
}

static uam_status mmu_paged(const avr32_state *s, uint32_t va, uam_xlate_kind kind, bool app, uam_xlate *out)
{
    bool ins = kind == UAM_XLATE_FETCH;
    int i = tlb_match(s, ins ? s->itlb : s->dtlb, va);
    if (i == -2) {
        out->fault = A32_XF_MULTI;
        return UAM_ERR_XLATE;
    }
    if (i < 0) {
        if (kind == UAM_XLATE_DEBUG && (i = tlb_match(s, s->itlb, va)) >= 0)
            goto found;   /* a debugger looks in both */
        out->fault = A32_XF_MISS;
        return UAM_ERR_XLATE;
    }
found:;
    const avr32_tlb_entry *e = kind == UAM_XLATE_DEBUG && tlb_match(s, s->dtlb, va) < 0 ? &s->itlb[i]
                             : ins ? &s->itlb[i] : &s->dtlb[i];
    unsigned ap = (e->lo >> 4) & 7;
    if (kind != UAM_XLATE_DEBUG) {
        if ((app && !(ap & 4)) || (kind == UAM_XLATE_STORE && !(ap & 2)) || (ins && !(ap & 1))) {
            out->fault = A32_XF_PROT;
            return UAM_ERR_XLATE;
        }
        if (kind == UAM_XLATE_STORE && !(e->lo & (1u << 1))) {
            out->fault = A32_XF_MOD;
            return UAM_ERR_XLATE;
        }
    }
    uint32_t m = page_mask[(e->lo >> 2) & 3];
    out->paddr = (e->lo & ~m & 0xFFFFFC00u) | (va & m);
    return UAM_OK;
}

static uam_status avr32_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out)
{
    const avr32_state *s = a32_st(cpu);
    uint32_t va = (uint32_t)vaddr;
    bool app = cpu->priv == A32_PRIV_APP && kind != UAM_XLATE_DEBUG;
    out->paddr = va;
    out->fault = 0;
    out->fixed = false;
    if (!is_b(s)) {
        if (!s->cfg.mmu_entries) {
            out->fixed = true;
            return UAM_OK;
        }
        return mpu_translate(s, va, kind, app, out);
    }
    bool paging = s->cfg.mmu_entries && (s->mmucr & 1);
    if (!(s->mmucr & (1u << 4)))            /* S = 0: unsegmented */
        return paging ? mmu_paged(s, va, kind, app, out) : UAM_OK;
    if (va >= 0x80000000u) {
        if (app) {
            out->fault = A32_XF_PROT;
            return UAM_ERR_XLATE;
        }
        if (va >= 0xE0000000u)              /* P4: untranslated */
            return UAM_OK;
        if (va < 0xC0000000u || !paging) {  /* P1, P2, and P3 without paging */
            out->paddr = va & 0x1FFFFFFFu;
            return UAM_OK;
        }
        return mmu_paged(s, va, kind, app, out);
    }
    return paging ? mmu_paged(s, va, kind, app, out) : UAM_OK;   /* P0 / U0 */
}

/* ------------------------------------------------------------------------ */
/* System registers                                                          */
/* ------------------------------------------------------------------------ */

static uint32_t config0(const avr32_state *s)
{
    uint32_t c = s->cfg.config0 & 0xFFFF0000u;          /* processor ID and revision */
    c |= (uint32_t)s->cfg.arch << 13 | 2u << 10;        /* AT, AR: revision 2 */
    uint32_t mmut = !s->cfg.mmu_entries ? 0 : is_b(s) ? 1 : 3;
    c |= mmut << 7;
    if (s->cfg.simd)
        c |= 1u << 2;
    c |= 1u << 1 | 1u << 0;                             /* D (DSP), R (RMW) */
    return c;
}

static uint32_t config1(const avr32_state *s)
{
    uint32_t c = 0;   /* the cache fields: no caches */
    if (s->cfg.mmu_entries) {
        uint32_t n = is_b(s) ? s->cfg.mmu_entries - 1 : s->cfg.mmu_entries;
        c |= (is_b(s) ? n << 26 : 0) | n << 20;
    }
    return c;
}

static uint32_t tlbar(const avr32_state *s, bool hi)
{
    const avr32_tlb_entry *t = (s->tlbehi & (1u << 8)) ? s->itlb : s->dtlb;
    uint32_t v = 0;
    for (unsigned k = 0; k < 32; k++) {
        unsigned i = hi ? 32 + k : k;
        if (i < s->cfg.mmu_entries && t[i].a)
            v |= 1u << (31 - k);   /* reversed: entry 0 is bit 31 */
    }
    return v;
}

static uam_status avr32_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    avr32_state *s = a32_st(cpu);
    uint32_t r = id & 0xFFFF;   /* bit 16: the internal reads acall and csrfcz make */
    uint32_t v = 0;
    if (r >= SYS_RSR_SUP && r <= SYS_RSR_SUP + 6)
        v = is_b(s) ? s->rsr[r - SYS_RSR_SUP + 1] : 0;
    else if (r >= SYS_RAR_SUP && r <= SYS_RAR_SUP + 6)
        v = is_b(s) ? s->rar[r - SYS_RAR_SUP + 1] : 0;
    else if (r >= SYS_JECR && r <= SYS_JBCR)
        v = s->java[r - SYS_JECR];
    else if (r >= SYS_MPUAR0 && r < SYS_MPUAR0 + A32_MPU_REGIONS)
        v = s->mpuar[r - SYS_MPUAR0];
    else if (r >= SYS_MPUPSR0 && r < SYS_MPUPSR0 + A32_MPU_REGIONS)
        v = s->mpupsr[r - SYS_MPUPSR0];
    else switch (r) {
    case SYS_SR:      v = avr32_sr_read(cpu); break;
    case SYS_EVBA:    v = s->evba; break;
    case SYS_ACBA:    v = s->acba; break;
    case SYS_CPUCR:   v = s->cpucr; break;
    case SYS_ECR:     v = s->ecr; break;
    case SYS_RSR_DBG: v = s->rsr_dbg; break;
    case SYS_RAR_DBG: v = s->rar_dbg; break;
    case SYS_CONFIG0: v = config0(s); break;
    case SYS_CONFIG1: v = config1(s); break;
    case SYS_COUNT:   v = count_now(cpu); break;
    case SYS_COMPARE: v = s->compare; break;
    case SYS_TLBEHI:  v = s->tlbehi; break;
    case SYS_TLBELO:  v = s->tlbelo; break;
    case SYS_PTBR:    v = s->ptbr; break;
    case SYS_TLBEAR:  v = s->tlbear; break;
    case SYS_MMUCR:   v = s->mmucr; break;
    case SYS_TLBARLO: v = tlbar(s, false); break;
    case SYS_TLBARHI: v = tlbar(s, true); break;
    case SYS_PCCNT:   v = s->pccnt; break;
    case SYS_PCNT0:   v = s->pcnt[0]; break;
    case SYS_PCNT1:   v = s->pcnt[1]; break;
    case SYS_PCCR:    v = s->pccr; break;
    case SYS_BEAR:    v = s->bear; break;
    case SYS_MPUCRA:  v = s->mpucra; break;
    case SYS_MPUCRB:  v = s->mpucrb; break;
    case SYS_MPUBRA:  v = s->mpubra; break;
    case SYS_MPUBRB:  v = s->mpubrb; break;
    case SYS_MPUAPRA: v = s->mpuapra; break;
    case SYS_MPUAPRB: v = s->mpuaprb; break;
    case SYS_MPUCR:   v = s->mpucr; break;
    default:          v = 0; break;   /* reserved and implementation-defined: read as zero */
    }
    *val = v;
    return UAM_OK;
}

static uam_status avr32_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val64)
{
    avr32_state *s = a32_st(cpu);
    uint32_t v = (uint32_t)val64, r = id & 0xFFFF;
    if (r >= SYS_RSR_SUP && r <= SYS_RSR_SUP + 6) {
        if (is_b(s))
            s->rsr[r - SYS_RSR_SUP + 1] = v;
        return UAM_OK;
    }
    if (r >= SYS_RAR_SUP && r <= SYS_RAR_SUP + 6) {
        if (is_b(s))
            s->rar[r - SYS_RAR_SUP + 1] = v;
        return UAM_OK;
    }
    if (r >= SYS_JECR && r <= SYS_JBCR) {
        s->java[r - SYS_JECR] = v;
        return UAM_OK;
    }
    if (r >= SYS_MPUAR0 && r < SYS_MPUAR0 + A32_MPU_REGIONS) {
        s->mpuar[r - SYS_MPUAR0] = v & 0xFFFFF03Fu;
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    }
    if (r >= SYS_MPUPSR0 && r < SYS_MPUPSR0 + A32_MPU_REGIONS) {
        s->mpupsr[r - SYS_MPUPSR0] = v & 0xFFFF;
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    }
    switch (r) {
    case SYS_SR:      avr32_sr_write(cpu, v); break;
    case SYS_EVBA:    s->evba = v & ~3u; break;
    case SYS_ACBA:    s->acba = v & ~3u; break;
    case SYS_CPUCR:   s->cpucr = v; break;
    case SYS_ECR:     s->ecr = v; break;
    case SYS_RSR_DBG: s->rsr_dbg = v; break;
    case SYS_RAR_DBG: s->rar_dbg = v; break;
    case SYS_COUNT:
        s->count_bias = (uint64_t)v - uam_sched_now(cpu->sched);
        timer_update(cpu);
        break;
    case SYS_COMPARE:
        s->compare = v;
        uam_signal_set(&s->compare_line, 0);   /* writing COMPARE clears the request */
        timer_update(cpu);
        break;
    case SYS_TLBEHI:
        if ((v ^ s->tlbehi) & 0xFF)
            uam_cpu_xlate_changed(cpu);         /* a new ASID */
        s->tlbehi = v;
        break;
    case SYS_TLBELO:  s->tlbelo = v; break;
    case SYS_PTBR:    s->ptbr = v; break;
    case SYS_TLBEAR:  s->tlbear = v; break;
    case SYS_MMUCR:
        if (v & (1u << 2)) {                    /* I: invalidate every entry */
            for (unsigned i = 0; i < A32_TLB_MAX; i++)
                s->itlb[i].hi = s->dtlb[i].hi = 0;
            v &= ~(1u << 2);
        }
        s->mmucr = v & 0xFFFFFF1Fu;
        uam_cpu_xlate_changed(cpu);
        break;
    case SYS_TLBARLO: case SYS_TLBARHI: {
        avr32_tlb_entry *t = (s->tlbehi & (1u << 8)) ? s->itlb : s->dtlb;
        for (unsigned k = 0; k < 32; k++) {
            unsigned i = r == SYS_TLBARHI ? 32 + k : k;
            if (i < A32_TLB_MAX)
                t[i].a = (v >> (31 - k)) & 1;
        }
        break;
    }
    case SYS_PCCNT:   s->pccnt = v; break;
    case SYS_PCNT0:   s->pcnt[0] = v; break;
    case SYS_PCNT1:   s->pcnt[1] = v; break;
    case SYS_PCCR:    s->pccr = v; break;
    case SYS_MPUCRA:  s->mpucra = v & 0xFF; break;
    case SYS_MPUCRB:  s->mpucrb = v & 0xFF; break;
    case SYS_MPUBRA:  s->mpubra = v & 0xFF; break;
    case SYS_MPUBRB:  s->mpubrb = v & 0xFF; break;
    case SYS_MPUAPRA: s->mpuapra = v; uam_cpu_xlate_changed(cpu); break;
    case SYS_MPUAPRB: s->mpuaprb = v; uam_cpu_xlate_changed(cpu); break;
    case SYS_MPUCR:   s->mpucr = v & 1; uam_cpu_xlate_changed(cpu); break;
    default:          break;   /* read-only (CONFIG0/1, BEAR) and reserved: ignored */
    }
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

#define CPU uam_cpu *cpu = env; avr32_state *s = a32_st(cpu)

static uint64_t h_rete(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c;
    unsigned mode = a32_mode(s);
    if (mode == A32_MODE_APP || mode == A32_MODE_SUP) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    uint32_t sr, pc;
    if (is_b(s)) {
        sr = s->rsr[mode];
        pc = s->rar[mode];
    } else {
        sr = pop_sys(cpu);
        pc = pop_sys(cpu);
        if (mode >= A32_MODE_INT0 && mode <= A32_MODE_INT3) {
            static const unsigned saved[6] = { A32_LR, 12, 11, 10, 9, 8 };
            for (unsigned i = 0; i < 6; i++)
                set_reg(cpu, saved[i], pop_sys(cpu));
        }
    }
    avr32_sr_write(cpu, sr & ~SR_L);
    cpu->resv.valid = false;
    return pc;
}

static uint64_t h_rets(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c;
    unsigned mode = a32_mode(s);
    if (mode == A32_MODE_APP) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    if (mode != A32_MODE_SUP)
        return reg(cpu, A32_LR);
    uint32_t sr, pc;
    if (is_b(s)) {
        sr = s->rsr[A32_MODE_SUP];
        pc = s->rar[A32_MODE_SUP];
    } else {
        sr = pop_sys(cpu);
        pc = pop_sys(cpu);
    }
    avr32_sr_write(cpu, sr);
    return pc;
}

static uint64_t h_retd(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c;
    if (!(s->sr_hi & SR_D)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    avr32_sr_write(cpu, s->rsr_dbg);
    return s->rar_dbg;
}

/* csrf / ssrf on SR[31:16] */
static uint64_t h_setsr(void *env, uint64_t bp, uint64_t v, uint64_t c, uam_status *st)
{
    CPU; (void)c; (void)st; (void)s;
    uint32_t sr = avr32_sr_read(cpu);
    sr = v ? sr | (1u << bp) : sr & ~(1u << bp);
    avr32_sr_write(cpu, sr);
    return 0;
}

static uint64_t h_tlb(void *env, uint64_t op, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c; (void)st;
    if (!is_b(s) || !s->cfg.mmu_entries)
        return 0;
    bool ins = s->tlbehi & (1u << 8);
    avr32_tlb_entry *t = ins ? s->itlb : s->dtlb;
    unsigned ptr = (s->mmucr >> (ins ? 26 : 14)) & 63;
    if (op == 0) {                         /* tlbr */
        if (ptr < s->cfg.mmu_entries) {
            s->tlbehi = (t[ptr].hi & ~(1u << 8)) | (ins ? 1u << 8 : 0);
            s->tlbelo = t[ptr].lo;
        }
    } else if (op == 1) {                  /* tlbw */
        if (ptr < s->cfg.mmu_entries) {
            t[ptr].hi = s->tlbehi & ~(1u << 8);
            t[ptr].lo = s->tlbelo;
            t[ptr].a = true;
        }
        uam_cpu_xlate_changed(cpu);
    } else {                               /* tlbs */
        int i = tlb_match(s, t, s->tlbehi);
        s->mmucr |= 1u << 3;               /* N: not found */
        if (i >= 0) {
            s->mmucr &= ~(1u << 3);
            s->mmucr = (s->mmucr & ~(63u << (ins ? 26 : 14))) | (uint32_t)i << (ins ? 26 : 14);
        }
    }
    return 0;
}

static uint64_t h_sleep(void *env, uint64_t op8, uint64_t b, uint64_t c, uam_status *st)
{
    uam_cpu *cpu = env;
    (void)b; (void)c; (void)st;
    if (op8 & 0x80)
        avr32_sr_write(cpu, avr32_sr_read(cpu) & ~SR_GM);
    return 0;
}

/* Where the application context's register r is while in the current mode. */
static uint32_t *user_slot(uam_cpu *cpu, unsigned r, uint32_t *tmp)
{
    avr32_state *s = a32_st(cpu);
    unsigned mode = a32_mode(s);
    if (mode != A32_MODE_APP) {
        if (r == A32_SP)
            return &s->sp_app;
        if (is_b(s) && s->cfg.int3_shadow && mode == A32_MODE_INT3 && ((r >= 8 && r <= 12) || r == A32_LR))
            return &s->shadow[r == A32_LR ? 5 : r - 8];
    }
    *tmp = reg(cpu, r);
    return NULL;
}

static uint64_t h_userreg_rd(void *env, uint64_t r, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c; (void)st; (void)s;
    uint32_t tmp;
    uint32_t *p = user_slot(cpu, (unsigned)r & 15, &tmp);
    return p ? *p : tmp;
}

static uint64_t h_userreg_wr(void *env, uint64_t r, uint64_t v, uint64_t c, uam_status *st)
{
    CPU; (void)c; (void)st; (void)s;
    uint32_t tmp;
    uint32_t *p = user_slot(cpu, (unsigned)r & 15, &tmp);
    if (p)
        *p = (uint32_t)v;
    else
        set_reg(cpu, (unsigned)r & 15, (uint32_t)v);
    return 0;
}

static uint64_t h_dsp(void *env, uint64_t op, uint64_t x, uint64_t y, uam_status *st)
{
    (void)st;
    return avr32_dsp(a32_st((uam_cpu *)env), (unsigned)op, (uint32_t)x, (uint32_t)y);
}

static uint64_t h_simd(void *env, uint64_t insn, uint64_t x, uint64_t y, uam_status *st)
{
    (void)st;
    return avr32_simd(a32_st((uam_cpu *)env), (uint32_t)insn, (uint32_t)x, (uint32_t)y);
}

#define STATE_HELPER(nm, f, nargs, rw)                                                    \
    { .name = nm, .n_args = nargs, .arg_width = { 32, 32, 32 }, .result_width = rw,        \
      .effect = UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE | UAM_HELPER_MAY_FAULT, .fn = f }

const uam_helper_desc avr32_helpers[A32_H__COUNT] = {
    [A32_H_RETE]       = STATE_HELPER("avr32_rete", h_rete, 0, 32),
    [A32_H_RETS]       = STATE_HELPER("avr32_rets", h_rets, 0, 32),
    [A32_H_RETD]       = STATE_HELPER("avr32_retd", h_retd, 0, 32),
    [A32_H_SETSR]      = STATE_HELPER("avr32_setsr", h_setsr, 2, 0),
    [A32_H_TLB]        = STATE_HELPER("avr32_tlb", h_tlb, 1, 0),
    [A32_H_SLEEP]      = STATE_HELPER("avr32_sleep", h_sleep, 1, 0),
    [A32_H_USERREG_RD] = { .name = "avr32_userreg_rd", .n_args = 1, .arg_width = { 32 }, .result_width = 32,
                           .effect = UAM_HELPER_READS_STATE, .fn = h_userreg_rd },
    [A32_H_USERREG_WR] = { .name = "avr32_userreg_wr", .n_args = 2, .arg_width = { 32, 32 }, .result_width = 0,
                           .effect = UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE, .fn = h_userreg_wr },
    [A32_H_DSP]        = { .name = "avr32_dsp", .n_args = 3, .arg_width = { 32, 32, 32 }, .result_width = 64,
                           .effect = UAM_HELPER_PURE, .fn = h_dsp },
    [A32_H_SIMD]       = { .name = "avr32_simd", .n_args = 3, .arg_width = { 32, 32, 32 }, .result_width = 64,
                           .effect = UAM_HELPER_PURE, .fn = h_simd },
};

/* ------------------------------------------------------------------------ */
/* CPU models                                                                */
/* ------------------------------------------------------------------------ */

const uam_avr32_config uam_avr32a_ref = {
    .name = "avr32a-ref", .arch = UAM_AVR32_A, .config0 = 0x00000000u, .config1 = 0,
    .mmu_entries = 8, .int3_shadow = false, .simd = false,
};

const uam_avr32_config uam_avr32b_ref = {
    .name = "avr32b-ref", .arch = UAM_AVR32_B, .config0 = 0x00000000u, .config1 = 0,
    .mmu_entries = 32, .int3_shadow = true, .simd = true,
};

bool uam_avr32_is(const uam_isa *isa) { return isa == &uam_isa_avr32; }

uam_status uam_avr32_configure(uam_cpu *cpu, const uam_avr32_config *cfg)
{
    if (!cpu || !uam_avr32_is(cpu->isa) || !cfg || cfg->arch > UAM_AVR32_B)
        return UAM_ERR_INVALID;
    if (cfg->arch == UAM_AVR32_A ? (cfg->mmu_entries != 0 && cfg->mmu_entries != A32_MPU_REGIONS)
                                 : cfg->mmu_entries > A32_TLB_MAX)
        return UAM_ERR_INVALID;
    if (cfg->simd && cfg->arch != UAM_AVR32_B)
        return UAM_ERR_INVALID;
    a32_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

void uam_avr32_set_autovector(uam_cpu *cpu, uam_avr32_autovector_fn fn, void *ctx)
{
    if (!cpu || !uam_avr32_is(cpu->isa))
        return;
    a32_st(cpu)->autovector = fn;
    a32_st(cpu)->autovector_ctx = ctx;
}

uam_signal *uam_avr32_compare_line(uam_cpu *cpu)
{
    return cpu && uam_avr32_is(cpu->isa) ? &a32_st(cpu)->compare_line : NULL;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static uam_status avr32_init(uam_cpu *cpu)
{
    avr32_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    cpu->isa_state = s;
    s->cfg = uam_avr32a_ref;
    uam_timer_init(&s->timer, timer_fire, cpu);
    uam_signal_init(&s->compare_line);
    return UAM_OK;
}

static void avr32_fini(uam_cpu *cpu)
{
    avr32_state *s = a32_st(cpu);
    if (cpu->sched)
        uam_timer_cancel(cpu->sched, &s->timer);
    free(s);
    cpu->isa_state = NULL;
}

static void avr32_reset(uam_cpu *cpu, uint64_t entry)
{
    avr32_state *s = a32_st(cpu);
    if (cpu->sched)
        uam_timer_cancel(cpu->sched, &s->timer);
    /* what survives: the model, the board's wiring, the input line levels */
    uam_avr32_config cfg = s->cfg;
    uam_timer timer = s->timer;
    uam_signal line = s->compare_line;
    uam_avr32_autovector_fn av = s->autovector;
    void *avctx = s->autovector_ctx;
    uint32_t lines = s->lines;
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    s->timer = timer;
    s->compare_line = line;
    s->autovector = av;
    s->autovector_ctx = avctx;
    s->lines = lines;
    uam_signal_set(&s->compare_line, 0);
    for (size_t i = 0; i < cpu->regfile_size; i++)
        cpu->regfile[i] = 0;
    s->sr_hi = SR_GM | SR_EM | (uint32_t)A32_MODE_SUP << SR_M_SHIFT;
    s->mmucr = is_b(s) ? 1u << 4 : 0;   /* segmentation on, paging off */
    s->count_bias = cpu->sched ? (uint64_t)0 - uam_sched_now(cpu->sched) : 0;
    cpu->priv = A32_PRIV_SYS;
    cpu->pc = entry;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

/* ------------------------------------------------------------------------ */
/* Debugger view: r0-r12, sp, lr, pc, sr                                     */
/* ------------------------------------------------------------------------ */

#define GDB_REG(n, num) "<reg name=\"" n "\" bitsize=\"32\" regnum=\"" #num "\"/>"
static const char avr32_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>avr32</architecture><feature name=\"org.gnu.gdb.avr32.core\">"
    GDB_REG("r0", 0) GDB_REG("r1", 1) GDB_REG("r2", 2) GDB_REG("r3", 3) GDB_REG("r4", 4)
    GDB_REG("r5", 5) GDB_REG("r6", 6) GDB_REG("r7", 7) GDB_REG("r8", 8) GDB_REG("r9", 9)
    GDB_REG("r10", 10) GDB_REG("r11", 11) GDB_REG("r12", 12) GDB_REG("sp", 13) GDB_REG("lr", 14)
    GDB_REG("pc", 15) GDB_REG("sr", 16)
    "</feature></target>";
#undef GDB_REG

static bool avr32_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n < 15)
        *v = reg(cpu, n);
    else if (n == 15)
        *v = (uint32_t)cpu->pc;
    else if (n == 16)
        *v = avr32_sr_read(cpu);
    else
        return false;
    return true;
}

static bool avr32_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n < 15)
        set_reg(cpu, n, (uint32_t)v);
    else if (n == 15)
        cpu->pc = (uint32_t)v & ~1u;
    else if (n == 16)
        avr32_sr_write(cpu, (uint32_t)v);
    else
        return false;
    return true;
}

static const uam_gdb_arch avr32_gdb = {
    .target_xml = avr32_target_xml, .n_regs = 17, .reg_bytes = 4,
    .read_reg = avr32_gdb_read, .write_reg = avr32_gdb_write,
};

static void avr32_dump_state(uam_cpu *cpu, FILE *out)
{
    avr32_state *s = a32_st(cpu);
    static const char *const modes[8] = { "app", "sup", "int0", "int1", "int2", "int3", "ex", "nmi" };
    fprintf(out, "pc=%08llx icount=%llu arch=%s mode=%s\n", (unsigned long long)cpu->pc,
            (unsigned long long)cpu->icount, is_b(s) ? "AVR32B" : "AVR32A", modes[a32_mode(s)]);
    for (unsigned i = 0; i < 15; i++)
        fprintf(out, "%-3s=%08x%s", avr32_regs[i].name, reg(cpu, i), (i % 5 == 4) ? "\n" : "  ");
    fprintf(out, "sr=%08x evba=%08x acba=%08x ecr=%08x count=%08x compare=%08x\n",
            avr32_sr_read(cpu), s->evba, s->acba, s->ecr, count_now(cpu), s->compare);
    fprintf(out, "sp_app=%08x sp_sys=%08x (the current one is sp)\n",
            a32_mode(s) == A32_MODE_APP ? reg(cpu, A32_SP) : s->sp_app,
            a32_mode(s) == A32_MODE_APP ? s->sp_sys : reg(cpu, A32_SP));
    if (is_b(s))
        fprintf(out, "mmucr=%08x tlbehi=%08x tlbelo=%08x tlbear=%08x\n", s->mmucr, s->tlbehi, s->tlbelo,
                s->tlbear);
    else if (s->cfg.mmu_entries)
        fprintf(out, "mpucr=%08x mpuapra=%08x mpuaprb=%08x\n", s->mpucr, s->mpuapra, s->mpuaprb);
}

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                 */
/* ------------------------------------------------------------------------ */

static void put_words(uam_state_writer *w, const uint32_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        uam_state_put_u32(w, v[i]);
}
static void get_words(uam_state_reader *r, uint32_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        v[i] = uam_state_get_u32(r);
}

static void avr32_save(uam_cpu *cpu, uam_state_writer *w)
{
    const avr32_state *s = a32_st(cpu);
    uam_state_put_u32(w, s->cfg.arch);
    uam_state_put_u32(w, s->cfg.mmu_entries);
    const uint32_t regs[] = { s->lines, s->sr_hi, s->sp_app, s->sp_sys, s->rsr_dbg, s->rar_dbg, s->evba,
                              s->acba, s->cpucr, s->ecr, s->bear, s->pccnt, s->pcnt[0], s->pcnt[1], s->pccr,
                              s->compare, s->tlbehi, s->tlbelo, s->ptbr, s->tlbear, s->mmucr, s->mpucra,
                              s->mpucrb, s->mpubra, s->mpubrb, s->mpuapra, s->mpuaprb, s->mpucr };
    put_words(w, regs, UAM_ARRAY_LEN(regs));
    put_words(w, s->shadow, 6);
    put_words(w, s->rsr, 8);
    put_words(w, s->rar, 8);
    put_words(w, s->java, 12);
    put_words(w, s->mpuar, A32_MPU_REGIONS);
    put_words(w, s->mpupsr, A32_MPU_REGIONS);
    uam_state_put_u64(w, s->count_bias);
    for (unsigned i = 0; i < A32_TLB_MAX; i++) {
        uam_state_put_u32(w, s->itlb[i].hi);
        uam_state_put_u32(w, s->itlb[i].lo);
        uam_state_put_bool(w, s->itlb[i].a);
        uam_state_put_u32(w, s->dtlb[i].hi);
        uam_state_put_u32(w, s->dtlb[i].lo);
        uam_state_put_bool(w, s->dtlb[i].a);
    }
    uam_state_put_signal(w, &s->compare_line);
    uam_state_put_timer(w, &s->timer);
}

static void avr32_restore(uam_cpu *cpu, uam_state_reader *r)
{
    avr32_state *s = a32_st(cpu);
    uam_state_expect_u32(r, s->cfg.arch, "AVR32 architecture");
    uam_state_expect_u32(r, s->cfg.mmu_entries, "AVR32 MMU/MPU size");
    if (!uam_state_ok(r))
        return;
    uint32_t *const regs[] = { &s->lines, &s->sr_hi, &s->sp_app, &s->sp_sys, &s->rsr_dbg, &s->rar_dbg,
                               &s->evba, &s->acba, &s->cpucr, &s->ecr, &s->bear, &s->pccnt, &s->pcnt[0],
                               &s->pcnt[1], &s->pccr, &s->compare, &s->tlbehi, &s->tlbelo, &s->ptbr,
                               &s->tlbear, &s->mmucr, &s->mpucra, &s->mpucrb, &s->mpubra, &s->mpubrb,
                               &s->mpuapra, &s->mpuaprb, &s->mpucr };
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        *regs[i] = uam_state_get_u32(r);
    get_words(r, s->shadow, 6);
    get_words(r, s->rsr, 8);
    get_words(r, s->rar, 8);
    get_words(r, s->java, 12);
    get_words(r, s->mpuar, A32_MPU_REGIONS);
    get_words(r, s->mpupsr, A32_MPU_REGIONS);
    s->count_bias = uam_state_get_u64(r);
    for (unsigned i = 0; i < A32_TLB_MAX; i++) {
        s->itlb[i].hi = uam_state_get_u32(r);
        s->itlb[i].lo = uam_state_get_u32(r);
        s->itlb[i].a = uam_state_get_bool(r);
        s->dtlb[i].hi = uam_state_get_u32(r);
        s->dtlb[i].lo = uam_state_get_u32(r);
        s->dtlb[i].a = uam_state_get_bool(r);
    }
    uam_state_get_signal(r, &s->compare_line);
    uam_state_get_timer(r, &s->timer);
    cpu->priv = a32_mode(s) == A32_MODE_APP ? A32_PRIV_APP : A32_PRIV_SYS;
}

const uam_isa uam_isa_avr32 = {
    .name           = "avr32",
    .addr_width     = 32,
    .default_endian = UAM_ACC_BE,
    .regfile_size   = A32_REGFILE_SIZE,
    .regs           = avr32_regs,
    .n_regs         = UAM_ARRAY_LEN(avr32_regs),
    .helpers        = { avr32_helpers, A32_H__COUNT },
    .init           = avr32_init,
    .fini           = avr32_fini,
    .reset          = avr32_reset,
    .lift           = avr32_lift,
    .take_exception = avr32_take_exception,
    .irq_take       = avr32_irq_take,
    .irq_wakeup     = avr32_irq_wakeup,
    .irq_input      = avr32_irq_input,
    .sysreg_read    = avr32_sysreg_read,
    .sysreg_write   = avr32_sysreg_write,
    .translate      = avr32_translate,
    .gdb            = &avr32_gdb,
    .dump_state     = avr32_dump_state,
    .save           = avr32_save,
    .restore        = avr32_restore,
    .irq_lines      = avr32_irq_lines,
};
