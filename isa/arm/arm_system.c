/*
 * arm_system.c - ARMv7-A privileged architecture and the uam_isa descriptor.
 *
 * Implements the processor modes and register banking, the exception entry and
 * return sequences (vectors at 0, 0xffff0000 with SCTLR.V, or VBAR; SPSR with the
 * T and IT bits of the interrupted instruction), IRQ and FIQ, CP15 (identification,
 * SCTLR/ACTLR/CPACR, the translation registers, fault status and address registers,
 * cache and barrier operations as no-ops, TLB maintenance, address translation
 * operations, thread ID registers), CP14 debug registers as RAZ/WI, the generic
 * timer (CNTFRQ, CNTPCT/CNTVCT, the physical and virtual timers) and the
 * short-descriptor MMU:
 *
 *   TTBR0/TTBR1 split by TTBCR.N; first-level sections, supersections and
 *   page tables; second-level large and small pages; DACR domains (no access,
 *   client, manager); AP[2:0] permissions (with SCTLR.AFE, AP[0] is ignored);
 *   XN. There is no TLB: every translation walks the tables in memory, so stale
 *   entries can never be observed, and TLB maintenance only drops translated code.
 *
 * Unknown CP15 registers read as zero and ignore writes in privileged modes, which
 * keeps firmware running past implementation-defined registers. Not modelled:
 * the security, virtualization and LPAE extensions, VFP/NEON, alignment checking
 * of ordinary loads and stores (SCTLR.A), big-endian data (CPSR.E), and
 * unprivileged permission checks for LDRT/STRT (they use the current mode).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/sched.h"

#include "arm.h"

/* ------------------------------------------------------------------------ */
/* Register descriptors                                                      */
/* ------------------------------------------------------------------------ */

#define G(n, i) { n, ARM_REG_OFF(i), 32, UAM_REG_GPR }
static const uam_regdesc arm_regs[] = {
    G("r0", 0), G("r1", 1), G("r2", 2),   G("r3", 3),   G("r4", 4),   G("r5", 5),
    G("r6", 6), G("r7", 7), G("r8", 8),   G("r9", 9),   G("r10", 10), G("r11", 11),
    G("r12", 12), G("sp", 13), G("lr", 14),
    { "nf", ARM_N_OFF, 8, UAM_REG_FLAG }, { "zf", ARM_Z_OFF, 8, UAM_REG_FLAG },
    { "cf", ARM_C_OFF, 8, UAM_REG_FLAG }, { "vf", ARM_V_OFF, 8, UAM_REG_FLAG },
    { "qf", ARM_Q_OFF, 8, UAM_REG_FLAG }, { "ge", ARM_GE_OFF, 8, UAM_REG_FLAG },
};
#undef G

/* ------------------------------------------------------------------------ */
/* CPU models                                                                */
/* ------------------------------------------------------------------------ */

const uam_arm_config uam_arm_cortex_a7 = {
    .name     = "cortex-a7",
    .midr     = 0x410FC075u,     /* ARM, r0p5, Cortex-A7 */
    .ctr      = 0x84448003u,     /* 64-byte lines */
    .mpidr    = 0x80000000u,     /* uniprocessor-style affinity 0 */
    .id_pfr0  = 0x00001131u,     /* ARM, Thumb-2, ThumbEE-less, Jazelle trivial */
    .id_pfr1  = 0x00010001u,     /* programmers' model, generic timer */
    .id_mmfr0 = 0x00000003u,     /* VMSAv7 */
    .id_isar0 = 0x02101110u,     /* SDIV/UDIV in both instruction sets */
    .cntfrq   = 62500000u,
};

uam_status uam_arm_configure(uam_cpu *cpu, const uam_arm_config *cfg)
{
    if (!cpu || !cfg || cpu->isa != &uam_isa_arm || !cpu->isa_state)
        return UAM_ERR_INVALID;
    arm_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

uam_status uam_arm_set_timer_sink(uam_cpu *cpu, uam_arm_timer_sink fn, void *ctx)
{
    if (!cpu || cpu->isa != &uam_isa_arm || !cpu->isa_state)
        return UAM_ERR_INVALID;
    arm_st(cpu)->sink = fn;
    arm_st(cpu)->sink_ctx = ctx;
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Modes and banked registers                                                */
/* ------------------------------------------------------------------------ */

static int bank_of(unsigned mode)
{
    switch (mode) {
    case ARM_MODE_USR: case ARM_MODE_SYS: return ARM_BANK_USR;
    case ARM_MODE_FIQ: return ARM_BANK_FIQ;
    case ARM_MODE_IRQ: return ARM_BANK_IRQ;
    case ARM_MODE_SVC: return ARM_BANK_SVC;
    case ARM_MODE_ABT: return ARM_BANK_ABT;
    case ARM_MODE_UND: return ARM_BANK_UND;
    default: return -1;
    }
}

static unsigned cur_mode(const arm_state *a) { return a->s.cpsr & CPSR_M_MASK; }
static int cur_bank(const arm_state *a) { return bank_of(cur_mode(a)); }

static uint32_t reg_r(uam_cpu *cpu, unsigned r) { return (uint32_t)uam_cpu_reg_read(cpu, ARM_REG_OFF(r), 32); }
static void reg_w(uam_cpu *cpu, unsigned r, uint32_t v) { uam_cpu_reg_write(cpu, ARM_REG_OFF(r), 32, v); }

/* Keep cpu->priv's User bit current (it tags translated code and decides
 * translation permissions). The JIT data-TLB caches permissions of the old mode. */
static void priv_update(uam_cpu *cpu)
{
    unsigned user = cur_mode(arm_st(cpu)) == ARM_MODE_USR ? ARM_PRIV_USER : 0;
    if ((cpu->priv & ARM_PRIV_USER) != user) {
        cpu->priv = (cpu->priv & ~ARM_PRIV_USER) | user;
        uam_cpu_dtlb_flush(cpu);
    }
    uam_cpu_request_exit(cpu);
}

static void set_it(uam_cpu *cpu, uint8_t it)
{
    cpu->priv = (cpu->priv & ~(0xFFu << ARM_PRIV_IT_SHIFT)) | ((unsigned)it << ARM_PRIV_IT_SHIFT);
}

/* Switch to `mode`, swapping banked r8-r14 through the register file. */
static void switch_mode(uam_cpu *cpu, unsigned mode)
{
    arm_state *a = arm_st(cpu);
    int ob = cur_bank(a), nb = bank_of(mode);
    if (nb < 0)
        return;   /* reserved mode: ignored */
    if (ob != nb) {
        a->s.r13[ob] = reg_r(cpu, 13);
        a->s.r14[ob] = reg_r(cpu, 14);
        if (ob == ARM_BANK_FIQ || nb == ARM_BANK_FIQ) {
            uint32_t *save = ob == ARM_BANK_FIQ ? a->s.fiq_r8 : a->s.usr_r8;
            uint32_t *load = nb == ARM_BANK_FIQ ? a->s.fiq_r8 : a->s.usr_r8;
            for (unsigned i = 0; i < 5; i++) {
                save[i] = reg_r(cpu, 8 + i);
                reg_w(cpu, 8 + i, load[i]);
            }
        }
        reg_w(cpu, 13, a->s.r13[nb]);
        reg_w(cpu, 14, a->s.r14[nb]);
    }
    a->s.cpsr = (a->s.cpsr & ~CPSR_M_MASK) | mode;
    priv_update(cpu);
}

/* The CPSR as software sees it, for state T (pc bit 0) and IT state `it`. */
static uint32_t cpsr_full(uam_cpu *cpu, bool t, uint8_t it)
{
    const arm_state *a = arm_st(cpu);
    uint32_t v = a->s.cpsr & (CPSR_M_MASK | CPSR_A | CPSR_I | CPSR_F | CPSR_E);
    v |= (cpu->regfile[ARM_N_OFF] & 1) ? CPSR_N : 0;
    v |= (cpu->regfile[ARM_Z_OFF] & 1) ? CPSR_Z : 0;
    v |= (cpu->regfile[ARM_C_OFF] & 1) ? CPSR_C : 0;
    v |= (cpu->regfile[ARM_V_OFF] & 1) ? CPSR_V : 0;
    v |= (cpu->regfile[ARM_Q_OFF] & 1) ? CPSR_Q : 0;
    v |= (uint32_t)(cpu->regfile[ARM_GE_OFF] & 0xF) << CPSR_GE_SHIFT;
    v |= t ? CPSR_T : 0;
    v |= ((uint32_t)(it & 3) << CPSR_IT_LO_SHIFT) | ((uint32_t)(it >> 2) << CPSR_IT_HI_SHIFT);
    return v;
}

static uint8_t psr_it(uint32_t psr)
{
    return (uint8_t)(((psr >> CPSR_IT_LO_SHIFT) & 3) | (((psr >> CPSR_IT_HI_SHIFT) & 0x3F) << 2));
}

static void write_flags(uam_cpu *cpu, uint32_t psr)
{
    cpu->regfile[ARM_N_OFF] = (psr & CPSR_N) ? 1 : 0;
    cpu->regfile[ARM_Z_OFF] = (psr & CPSR_Z) ? 1 : 0;
    cpu->regfile[ARM_C_OFF] = (psr & CPSR_C) ? 1 : 0;
    cpu->regfile[ARM_V_OFF] = (psr & CPSR_V) ? 1 : 0;
    cpu->regfile[ARM_Q_OFF] = (psr & CPSR_Q) ? 1 : 0;
    cpu->regfile[ARM_GE_OFF] = (uint8_t)((psr >> CPSR_GE_SHIFT) & 0xF);
}

/* Write the whole CPSR (exception return, debugger): everything but T, which the
 * caller turns into pc bit 0. */
static void write_cpsr_full(uam_cpu *cpu, uint32_t psr)
{
    arm_state *a = arm_st(cpu);
    write_flags(cpu, psr);
    a->s.cpsr = (a->s.cpsr & ~(CPSR_A | CPSR_I | CPSR_F)) | (psr & (CPSR_A | CPSR_I | CPSR_F));
    if (bank_of(psr & CPSR_M_MASK) >= 0)
        switch_mode(cpu, psr & CPSR_M_MASK);
    set_it(cpu, psr_it(psr));
    priv_update(cpu);
}

/* ------------------------------------------------------------------------ */
/* Generic timer                                                             */
/* ------------------------------------------------------------------------ */

#define CNT_ENABLE  1u
#define CNT_IMASK   2u
#define CNT_ISTATUS 4u

static uint64_t cnt_phys(uam_cpu *cpu) { return uam_sched_now(cpu->sched); }
static uint64_t cnt_of(uam_cpu *cpu, unsigned t)
{
    return cnt_phys(cpu) - (t == UAM_ARM_TIMER_VIRT ? arm_st(cpu)->s.cntvoff : 0);
}

static bool timer_status(uam_cpu *cpu, unsigned t)
{
    const arm_state *a = arm_st(cpu);
    return (a->s.cnt_ctl[t] & CNT_ENABLE) && cnt_of(cpu, t) >= a->s.cnt_cval[t];
}

/* Re-evaluate timer t: report an output change, and arm the deadline. */
static void timer_update(uam_cpu *cpu, unsigned t)
{
    arm_state *a = arm_st(cpu);
    bool st = timer_status(cpu, t);
    bool out = st && !(a->s.cnt_ctl[t] & CNT_IMASK);
    if (out != a->s.timer_out[t]) {
        a->s.timer_out[t] = out;
        if (a->sink)
            a->sink(a->sink_ctx, t, out);
    }
    if (!cpu->sched)
        return;
    if ((a->s.cnt_ctl[t] & CNT_ENABLE) && !st) {
        uint64_t when = a->s.cnt_cval[t] + (t == UAM_ARM_TIMER_VIRT ? a->s.cntvoff : 0);
        if (when <= UAM_SCHED_TIME_MAX)
            uam_timer_arm(cpu->sched, &a->timer[t], when);
        else
            uam_timer_cancel(cpu->sched, &a->timer[t]);
    } else {
        uam_timer_cancel(cpu->sched, &a->timer[t]);
    }
}

static void timer_fire_phys(void *ctx) { timer_update(ctx, UAM_ARM_TIMER_PHYS); }
static void timer_fire_virt(void *ctx) { timer_update(ctx, UAM_ARM_TIMER_VIRT); }

/* ------------------------------------------------------------------------ */
/* Address translation                                                       */
/* ------------------------------------------------------------------------ */

#define ARM_FS_WALK_L1 0x0C   /* external abort on the first-level walk */
#define ARM_FS_WALK_L2 0x0E

static uam_status xfault(uam_xlate *out, unsigned fs, unsigned dom)
{
    out->fault = ARM_XF(fs, dom);
    return UAM_ERR_XLATE;
}

/* AP[2:0] check; true if the access is allowed. */
static bool ap_allows(unsigned ap, bool user, bool write)
{
    switch (ap) {
    case 1: return !user;
    case 2: return !user || !write;
    case 3: return true;
    case 5: return !user && !write;
    case 6: case 7: return !write;
    default: return false;   /* 0: no access; 4: reserved */
    }
}

static uam_status arm_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out)
{
    const arm_state *a = arm_st(cpu);
    uint32_t va = (uint32_t)vaddr;
    out->fixed = false;   /* the mapping changes with SCTLR.M and the tables */
    if (!(a->s.sctlr & SCTLR_M)) {
        out->paddr = va;
        return UAM_OK;
    }
    bool debug = kind == UAM_XLATE_DEBUG;
    bool user = !debug && (cpu->priv & ARM_PRIV_USER);
    bool write = kind == UAM_XLATE_STORE, fetch = kind == UAM_XLATE_FETCH;

    unsigned n = a->s.ttbcr & 7;
    uint32_t l1;
    if (n && (va >> (32 - n))) {
        if (a->s.ttbcr & (1u << 5))
            return xfault(out, ARM_FS_XLATE_SEC, 0);
        l1 = (a->s.ttbr1 & 0xFFFFC000u) | ((va >> 20) << 2);
    } else {
        if (a->s.ttbcr & (1u << 4))
            return xfault(out, ARM_FS_XLATE_SEC, 0);
        uint32_t mask = 0xFFFFFFFFu << (14 - n);   /* TTBR0 table: 16 KiB >> N */
        l1 = (a->s.ttbr0 & mask) | (((va >> 20) & (0xFFFu >> n)) << 2);
    }
    uint64_t d1 = 0;
    if (uam_cpu_peek(cpu, l1, 4, &d1) != UAM_OK)
        return xfault(out, ARM_FS_WALK_L1, 0);
    uint32_t desc = (uint32_t)d1;

    unsigned dom, ap, fs_dom, fs_perm;
    bool xn;
    uint32_t pa;
    switch (desc & 3) {
    case 0:
        return xfault(out, ARM_FS_XLATE_SEC, 0);
    case 1: {
        dom = (desc >> 5) & 0xF;
        uint32_t l2 = (desc & 0xFFFFFC00u) | (((va >> 12) & 0xFF) << 2);
        uint64_t d2 = 0;
        if (uam_cpu_peek(cpu, l2, 4, &d2) != UAM_OK)
            return xfault(out, ARM_FS_WALK_L2, dom);
        uint32_t pd = (uint32_t)d2;
        fs_dom = ARM_FS_DOM_PAGE;
        fs_perm = ARM_FS_PERM_PAGE;
        if ((pd & 3) == 0)
            return xfault(out, ARM_FS_XLATE_PAGE, dom);
        ap = ((pd >> 7) & 4) | ((pd >> 4) & 3);
        if (pd & 2) {                                   /* small page */
            pa = (pd & 0xFFFFF000u) | (va & 0xFFFu);
            xn = pd & 1;
        } else {                                        /* large page */
            pa = (pd & 0xFFFF0000u) | (va & 0xFFFFu);
            xn = (pd >> 15) & 1;
        }
        break;
    }
    default:
        fs_dom = ARM_FS_DOM_SEC;
        fs_perm = ARM_FS_PERM_SEC;
        ap = ((desc >> 13) & 4) | ((desc >> 10) & 3);
        xn = (desc >> 4) & 1;
        if (desc & (1u << 18)) {                        /* supersection */
            dom = 0;
            pa = (desc & 0xFF000000u) | (va & 0x00FFFFFFu);
        } else {
            dom = (desc >> 5) & 0xF;
            pa = (desc & 0xFFF00000u) | (va & 0x000FFFFFu);
        }
        break;
    }
    out->paddr = pa;
    if (debug)
        return UAM_OK;
    switch ((a->s.dacr >> (2 * dom)) & 3) {
    case 3:                                             /* manager: no checks */
        return UAM_OK;
    case 1:                                             /* client */
        break;
    default:
        return xfault(out, fs_dom, dom);
    }
    if (a->s.sctlr & SCTLR_AFE)
        ap |= 1;                                        /* AP[0] is the access flag */
    if (!ap_allows(ap, user, write) || (fetch && xn))
        return xfault(out, fs_perm, dom);
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Exceptions and interrupts                                                 */
/* ------------------------------------------------------------------------ */

static uint8_t it_advance(uint8_t it)
{
    if ((it & 7) == 0)
        return 0;
    return (uint8_t)((it & 0xE0) | ((it << 1) & 0x1F));
}

static bool peek16(uam_cpu *cpu, uint32_t va, uint32_t *hw)
{
    uam_xlate xl = { 0 };
    uint64_t v = 0;
    if (arm_translate(cpu, va, UAM_XLATE_DEBUG, &xl) != UAM_OK
        || uam_cpu_peek(cpu, xl.paddr, 2, &v) != UAM_OK)
        return false;
    *hw = (uint32_t)v;
    return true;
}

/*
 * The IT state of the instruction an exception reports. Inside an IT block the
 * instruction is a continuation of the IT instruction (parent_pc): replay the IT
 * state from there. Otherwise it is the state the block started with, which is
 * non-zero only in a block that resumed inside an IT block (one instruction long).
 */
static uint8_t exc_it(uam_cpu *cpu, const uam_exception *e)
{
    if (!(e->pc & 1))
        return 0;
    if (!(e->flags & UAM_EXC_F_CONT))
        return (uint8_t)ARM_PRIV_IT(cpu->priv);
    uint32_t pc = (uint32_t)e->parent_pc & ~1u, target = (uint32_t)e->pc & ~1u, hw;
    if (!peek16(cpu, pc, &hw) || (hw & 0xFF00u) != 0xBF00u || !(hw & 0xF))
        return 0;
    uint8_t it = (uint8_t)hw;
    pc += 2;
    for (unsigned k = 0; k < 4 && pc < target; k++) {
        if (!peek16(cpu, pc, &hw))
            return 0;
        pc += arm_t32_prefix(hw) ? 4 : 2;
        it = it_advance(it);
    }
    return it;
}

enum { VEC_RESET = 0x00, VEC_UNDEF = 0x04, VEC_SVC = 0x08, VEC_PABT = 0x0C, VEC_DABT = 0x10,
       VEC_IRQ = 0x18, VEC_FIQ = 0x1C };

/* Enter an exception: SPSR = CPSR (state t, IT it), LR = lr, new mode, vector. */
static void enter(uam_cpu *cpu, unsigned mode, unsigned vec, uint32_t lr, bool t, uint8_t it)
{
    arm_state *a = arm_st(cpu);
    uint32_t spsr = cpsr_full(cpu, t, it);
    switch_mode(cpu, mode);
    a->s.spsr[bank_of(mode)] = spsr;
    reg_w(cpu, ARM_LR, lr);
    a->s.cpsr |= CPSR_I;
    if (mode == ARM_MODE_FIQ)
        a->s.cpsr |= CPSR_F;
    if (mode != ARM_MODE_UND && mode != ARM_MODE_SVC)
        a->s.cpsr |= CPSR_A;
    set_it(cpu, 0);
    uint32_t base = (a->s.sctlr & SCTLR_V) ? 0xFFFF0000u : (a->s.vbar & ~0x1Fu);
    cpu->pc = (base + vec) | ((a->s.sctlr & SCTLR_TE) ? 1u : 0u);
    cpu->resv.valid = false;
    cpu->waiting = false;
    priv_update(cpu);
}

/* DFSR/IFSR value: FS[3:0] in 3:0, FS[4] in bit 10, domain in 7:4, WnR in 11. */
static uint32_t fsr(uint32_t fault, bool with_dom, bool wnr)
{
    uint32_t fs = fault & 0x1F, dom = (fault >> 8) & 0xF;
    uint32_t v = (fs & 0xF) | ((fs & 0x10) << 6);
    if (with_dom)
        v |= dom << 4;
    if (wnr)
        v |= 1u << 11;
    return v;
}

static void arm_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    arm_state *a = arm_st(cpu);
    bool t = e->pc & 1;
    uint32_t pc = (uint32_t)e->pc & ~1u, va = (uint32_t)e->tval;
    uint8_t it = exc_it(cpu, e);
    bool store = false;
    uint32_t fault = 0;

    switch (e->kind) {
    case UAM_EXC_TRAP:
        switch (e->cause) {
        case ARM_EXC_SVC:
            enter(cpu, ARM_MODE_SVC, VEC_SVC, pc + (t ? 2 : 4), t, it_advance(it));
            return;
        case ARM_EXC_PABT:   /* BKPT */
            a->s.ifsr = fsr(va, false, false);
            enter(cpu, ARM_MODE_ABT, VEC_PABT, pc + 4, t, it);
            return;
        default:
            enter(cpu, ARM_MODE_UND, VEC_UNDEF, pc + (t ? 2 : 4), t, it);
            return;
        }
    case UAM_EXC_FETCH_XLATE:
    case UAM_EXC_FETCH_FAULT:
        fault = e->kind == UAM_EXC_FETCH_XLATE ? (uint32_t)e->cause : ARM_FS_EXT;
        a->s.ifsr = fsr(fault, false, false);
        a->s.ifar = va;
        enter(cpu, ARM_MODE_ABT, VEC_PABT, pc + 4, t, it);
        return;
    case UAM_EXC_STORE_XLATE: store = true; /* fallthrough */
    case UAM_EXC_LOAD_XLATE:   fault = (uint32_t)e->cause; break;
    case UAM_EXC_STORE_FAULT: store = true; /* fallthrough */
    case UAM_EXC_LOAD_FAULT:   fault = ARM_FS_EXT; break;
    case UAM_EXC_STORE_MISALIGNED: store = true; /* fallthrough */
    case UAM_EXC_LOAD_MISALIGNED:  fault = ARM_FS_ALIGN; break;
    default:   /* UAM_EXC_SYSREG, UAM_EXC_HELPER: an inaccessible register or operation */
        enter(cpu, ARM_MODE_UND, VEC_UNDEF, pc + (t ? 2 : 4), t, it);
        return;
    }
    a->s.dfsr = fsr(fault, true, store);
    a->s.dfar = va;
    enter(cpu, ARM_MODE_ABT, VEC_DABT, pc + 8, t, it);
}

static bool arm_irq_take(uam_cpu *cpu)
{
    arm_state *a = arm_st(cpu);
    bool t = cpu->pc & 1;
    uint32_t lr = ((uint32_t)cpu->pc & ~1u) + 4;
    uint8_t it = (uint8_t)ARM_PRIV_IT(cpu->priv);
    if (a->fiq_line && !(a->s.cpsr & CPSR_F)) {
        a->last_irq = 1u << UAM_ARM_FIQ;
        enter(cpu, ARM_MODE_FIQ, VEC_FIQ, lr, t, it);
        return true;
    }
    if (a->irq_line && !(a->s.cpsr & CPSR_I)) {
        a->last_irq = 1u << UAM_ARM_IRQ;
        enter(cpu, ARM_MODE_IRQ, VEC_IRQ, lr, t, it);
        return true;
    }
    return false;
}

static uint64_t arm_irq_lines(uam_cpu *cpu) { return arm_st(cpu)->last_irq; }

static bool arm_irq_wakeup(uam_cpu *cpu)
{
    const arm_state *a = arm_st(cpu);
    return a->irq_line || a->fiq_line;   /* WFI wakes regardless of CPSR.I/F */
}

static void arm_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    arm_state *a = arm_st(cpu);
    if (line == UAM_ARM_IRQ)
        a->irq_line = level != 0;
    else if (line == UAM_ARM_FIQ)
        a->fiq_line = level != 0;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* Exception return: a = return address, b = PSR (c = 1) or use SPSR (c = 0). */
static uint64_t helper_eret(void *env, uint64_t pc64, uint64_t psr64, uint64_t from_mem, uam_status *st)
{
    (void)st;
    uam_cpu *cpu = env;
    arm_state *a = arm_st(cpu);
    uint32_t pc = (uint32_t)pc64;
    int b = cur_bank(a);
    if (b == ARM_BANK_USR) {
        /* No SPSR in User/System mode (UNPREDICTABLE): a plain interworking branch. */
        return (pc & 1) ? pc : pc & ~3u;
    }
    uint32_t psr = from_mem ? (uint32_t)psr64 : a->s.spsr[b];
    write_cpsr_full(cpu, psr);
    cpu->resv.valid = false;
    return (psr & CPSR_T) ? (pc | 1u) : (pc & ~3u);
}

static uint64_t helper_set_it(void *env, uint64_t it, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    set_it(cpu, (uint8_t)it);
    uam_cpu_request_exit(cpu);
    return 0;
}

static uint64_t helper_clrex(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    ((uam_cpu *)env)->resv.valid = false;
    return 0;
}

const uam_helper_desc arm_helpers[ARM_HELPER__COUNT] = {
    [ARM_HELPER_ERET] = {
        .name = "arm_eret", .n_args = 3, .arg_width = { 32, 32, 32 }, .result_width = 32,
        .effect = UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE, .fn = helper_eret,
    },
    [ARM_HELPER_SET_IT] = {
        .name = "arm_set_it", .n_args = 1, .arg_width = { 32 }, .result_width = 0,
        .effect = UAM_HELPER_WRITES_STATE, .fn = helper_set_it,
    },
    [ARM_HELPER_CLREX] = {
        .name = "arm_clrex", .n_args = 0, .result_width = 0,
        .effect = UAM_HELPER_WRITES_STATE, .fn = helper_clrex,
    },
};

/* ------------------------------------------------------------------------ */
/* System registers                                                          */
/* ------------------------------------------------------------------------ */

#define CP15(o1, n, m, o2) ARM_SR_CP(15, o1, n, m, o2)
#define CP15_64(o1, m)     ARM_SR_CP64(15, o1, m)

static uint32_t isr_value(const arm_state *a)
{
    return (a->irq_line ? 1u << 7 : 0) | (a->fiq_line ? 1u << 6 : 0);
}

/* Address translation operation (ATS1C*): the result in PAR. */
static void ats(uam_cpu *cpu, uint32_t va, bool user, bool write)
{
    arm_state *a = arm_st(cpu);
    unsigned saved = cpu->priv;
    cpu->priv = (cpu->priv & ~ARM_PRIV_USER) | (user ? ARM_PRIV_USER : 0);
    uam_xlate xl = { 0 };
    uam_status st = arm_translate(cpu, va, write ? UAM_XLATE_STORE : UAM_XLATE_LOAD, &xl);
    cpu->priv = saved;
    if (st == UAM_OK)
        a->s.par = ((uint32_t)xl.paddr & 0xFFFFF000u);
    else
        a->s.par = 1u | ((xl.fault & 0xF) << 1) | ((xl.fault & 0x10) << 2);
}

/* Registers User mode may access (others are UNDEFINED there). */
static bool user_ok(const arm_state *a, uint32_t id, bool write)
{
    switch (id) {
    case CP15(0, 13, 0, 2): return true;                 /* TPIDRURW */
    case CP15(0, 13, 0, 3): return !write;               /* TPIDRURO */
    case CP15(0, 7, 5, 4): case CP15(0, 7, 10, 4): case CP15(0, 7, 10, 5):
        return write;                                    /* CP15ISB, CP15DSB, CP15DMB */
    case CP15(0, 14, 0, 0): return !write && (a->s.cntkctl & 3);
    case CP15_64(0, 14):    return !write && (a->s.cntkctl & 1);
    case CP15_64(1, 14):    return !write && (a->s.cntkctl & 2);
    case CP15(0, 14, 2, 0): case CP15(0, 14, 2, 1): case CP15_64(2, 14):
        return (a->s.cntkctl >> 9) & 1;                  /* PL0PTEN */
    case CP15(0, 14, 3, 0): case CP15(0, 14, 3, 1): case CP15_64(3, 14):
        return (a->s.cntkctl >> 8) & 1;                  /* PL0VTEN */
    default: return false;
    }
}

static uam_status cp15_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    arm_state *a = arm_st(cpu);
    arm_sys *s = &a->s;
    switch (id) {
    case CP15(0, 0, 0, 0): *val = a->cfg.midr; break;
    case CP15(0, 0, 0, 1): *val = a->cfg.ctr; break;
    case CP15(0, 0, 0, 5): *val = a->cfg.mpidr; break;
    case CP15(0, 0, 1, 0): *val = a->cfg.id_pfr0; break;
    case CP15(0, 0, 1, 1): *val = a->cfg.id_pfr1; break;
    case CP15(0, 0, 1, 4): *val = a->cfg.id_mmfr0; break;
    case CP15(0, 0, 2, 0): *val = a->cfg.id_isar0; break;
    case CP15(1, 0, 0, 0): *val = 0x701FE00Au; break;   /* CCSIDR: 32 KiB, 4-way, 64 B */
    case CP15(1, 0, 0, 1): *val = 0x0A200023u; break;   /* CLIDR: L1 I+D, L2 unified */
    case CP15(2, 0, 0, 0): *val = s->csselr; break;
    case CP15(0, 1, 0, 0): *val = s->sctlr; break;
    case CP15(0, 1, 0, 1): *val = s->actlr; break;
    case CP15(0, 1, 0, 2): *val = s->cpacr; break;
    case CP15(0, 2, 0, 0): *val = s->ttbr0; break;
    case CP15(0, 2, 0, 1): *val = s->ttbr1; break;
    case CP15(0, 2, 0, 2): *val = s->ttbcr; break;
    case CP15(0, 3, 0, 0): *val = s->dacr; break;
    case CP15(0, 5, 0, 0): *val = s->dfsr; break;
    case CP15(0, 5, 0, 1): *val = s->ifsr; break;
    case CP15(0, 5, 1, 0): *val = s->adfsr; break;
    case CP15(0, 5, 1, 1): *val = s->aifsr; break;
    case CP15(0, 6, 0, 0): *val = s->dfar; break;
    case CP15(0, 6, 0, 2): *val = s->ifar; break;
    case CP15(0, 7, 4, 0): *val = s->par; break;
    case CP15(0, 10, 2, 0): *val = s->prrr; break;
    case CP15(0, 10, 2, 1): *val = s->nmrr; break;
    case CP15(0, 12, 0, 0): *val = s->vbar; break;
    case CP15(0, 12, 1, 0): *val = isr_value(a); break;
    case CP15(0, 13, 0, 1): *val = s->contextidr; break;
    case CP15(0, 13, 0, 2): *val = s->tpidrurw; break;
    case CP15(0, 13, 0, 3): *val = s->tpidruro; break;
    case CP15(0, 13, 0, 4): *val = s->tpidrprw; break;
    case CP15(0, 14, 0, 0): *val = s->cntfrq; break;
    case CP15(0, 14, 1, 0): *val = s->cntkctl; break;
    case CP15(0, 14, 2, 0): *val = (uint32_t)(s->cnt_cval[0] - cnt_of(cpu, 0)); break;
    case CP15(0, 14, 3, 0): *val = (uint32_t)(s->cnt_cval[1] - cnt_of(cpu, 1)); break;
    case CP15(0, 14, 2, 1):
    case CP15(0, 14, 3, 1): {
        unsigned t = id == CP15(0, 14, 3, 1);
        *val = (s->cnt_ctl[t] & (CNT_ENABLE | CNT_IMASK)) | (timer_status(cpu, t) ? CNT_ISTATUS : 0);
        break;
    }
    case CP15_64(0, 2):  *val = s->ttbr0; break;
    case CP15_64(1, 2):  *val = s->ttbr1; break;
    case CP15_64(0, 7):  *val = s->par; break;
    case CP15_64(0, 14): *val = cnt_phys(cpu); break;
    case CP15_64(1, 14): *val = cnt_of(cpu, 1); break;
    case CP15_64(2, 14): *val = s->cnt_cval[0]; break;
    case CP15_64(3, 14): *val = s->cnt_cval[1]; break;
    case CP15_64(4, 14): *val = s->cntvoff; break;
    default:             *val = 0; break;   /* RAZ */
    }
    return UAM_OK;
}

static uam_status cp15_write(uam_cpu *cpu, uint32_t id, uint64_t v64)
{
    arm_state *a = arm_st(cpu);
    arm_sys *s = &a->s;
    uint32_t v = (uint32_t)v64;
    switch (id) {
    case CP15(2, 0, 0, 0): s->csselr = v & 0xF; break;
    case CP15(0, 1, 0, 0): {
        uint32_t old = s->sctlr;
        s->sctlr = (v & ~0x00C00000u) | 0x00C50000u;    /* RES1 bits kept */
        s->sctlr &= ~(1u << 25);                        /* EE: no big-endian exceptions */
        if ((old ^ s->sctlr) & (SCTLR_M | SCTLR_AFE))
            uam_cpu_xlate_changed(cpu);
        uam_cpu_request_exit(cpu);
        break;
    }
    case CP15(0, 1, 0, 1): s->actlr = v; break;
    case CP15(0, 1, 0, 2): s->cpacr = v & ~0x00F00000u; break;   /* no cp10/cp11 */
    case CP15(0, 2, 0, 0): case CP15_64(0, 2): s->ttbr0 = v; uam_cpu_xlate_changed(cpu); break;
    case CP15(0, 2, 0, 1): case CP15_64(1, 2): s->ttbr1 = v; uam_cpu_xlate_changed(cpu); break;
    case CP15(0, 2, 0, 2): s->ttbcr = v & 0x37u; uam_cpu_xlate_changed(cpu); break;
    case CP15(0, 3, 0, 0): s->dacr = v; uam_cpu_xlate_changed(cpu); break;
    case CP15(0, 5, 0, 0): s->dfsr = v; break;
    case CP15(0, 5, 0, 1): s->ifsr = v; break;
    case CP15(0, 5, 1, 0): s->adfsr = v; break;
    case CP15(0, 5, 1, 1): s->aifsr = v; break;
    case CP15(0, 6, 0, 0): s->dfar = v; break;
    case CP15(0, 6, 0, 2): s->ifar = v; break;
    case CP15(0, 7, 4, 0): s->par = v; break;
    case CP15(0, 7, 8, 0): ats(cpu, v, false, false); break;     /* ATS1CPR */
    case CP15(0, 7, 8, 1): ats(cpu, v, false, true); break;      /* ATS1CPW */
    case CP15(0, 7, 8, 2): ats(cpu, v, true, false); break;      /* ATS1CUR */
    case CP15(0, 7, 8, 3): ats(cpu, v, true, true); break;       /* ATS1CUW */
    case CP15(0, 10, 2, 0): s->prrr = v; break;
    case CP15(0, 10, 2, 1): s->nmrr = v; break;
    case CP15(0, 12, 0, 0): s->vbar = v & ~0x1Fu; break;
    case CP15(0, 13, 0, 1): s->contextidr = v; break;
    case CP15(0, 13, 0, 2): s->tpidrurw = v; break;
    case CP15(0, 13, 0, 3): s->tpidruro = v; break;
    case CP15(0, 13, 0, 4): s->tpidrprw = v; break;
    case CP15(0, 14, 0, 0): s->cntfrq = v; break;
    case CP15(0, 14, 1, 0): s->cntkctl = v & 0x3FFu; break;
    case CP15(0, 14, 2, 0): case CP15(0, 14, 3, 0): {             /* TVAL */
        unsigned t = id == CP15(0, 14, 3, 0);
        s->cnt_cval[t] = cnt_of(cpu, t) + (uint64_t)(int64_t)(int32_t)v;
        timer_update(cpu, t);
        break;
    }
    case CP15(0, 14, 2, 1): case CP15(0, 14, 3, 1): {             /* CTL */
        unsigned t = id == CP15(0, 14, 3, 1);
        s->cnt_ctl[t] = v & (CNT_ENABLE | CNT_IMASK);
        timer_update(cpu, t);
        break;
    }
    case CP15_64(2, 14): s->cnt_cval[0] = v64; timer_update(cpu, 0); break;
    case CP15_64(3, 14): s->cnt_cval[1] = v64; timer_update(cpu, 1); break;
    case CP15_64(4, 14): s->cntvoff = v64; timer_update(cpu, 1); break;
    default:
        /* c7 cache and barrier operations, c8 TLB maintenance, c9 performance
         * monitors, implementation-defined registers: no state to keep. A TLB
         * operation drops translated code that went through the tables. */
        if (((id >> 8) & 0xF) == 8)
            uam_cpu_xlate_changed(cpu);
        break;
    }
    return UAM_OK;
}

static uint32_t usrreg_read(uam_cpu *cpu, unsigned r)
{
    arm_state *a = arm_st(cpu);
    int b = cur_bank(a);
    if (b == ARM_BANK_USR || (r < 13 && b != ARM_BANK_FIQ))
        return reg_r(cpu, r);
    if (r < 13)
        return a->s.usr_r8[r - 8];
    return r == 13 ? a->s.r13[ARM_BANK_USR] : a->s.r14[ARM_BANK_USR];
}

static void usrreg_write(uam_cpu *cpu, unsigned r, uint32_t v)
{
    arm_state *a = arm_st(cpu);
    int b = cur_bank(a);
    if (b == ARM_BANK_USR || (r < 13 && b != ARM_BANK_FIQ))
        reg_w(cpu, r, v);
    else if (r < 13)
        a->s.usr_r8[r - 8] = v;
    else if (r == 13)
        a->s.r13[ARM_BANK_USR] = v;
    else
        a->s.r14[ARM_BANK_USR] = v;
}

static uam_status arm_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    arm_state *a = arm_st(cpu);
    bool user = cur_mode(a) == ARM_MODE_USR;
    if (ARM_SR_IS_CP(id)) {
        if (user && !user_ok(a, id, false))
            return UAM_ERR_ILLEGAL;
        if ((id >> 16 & 0xF) == 14) {           /* CP14 debug: RAZ */
            *val = 0;
            return UAM_OK;
        }
        return cp15_read(cpu, id, val);
    }
    switch (id & 0xFFFF0000u) {
    case ARM_SR_CPSR:
        *val = a->s.cpsr & (CPSR_M_MASK | CPSR_A | CPSR_I | CPSR_F | CPSR_E);
        return UAM_OK;
    case ARM_SR_SPSR: {
        int b = cur_bank(a);
        *val = b == ARM_BANK_USR ? 0 : a->s.spsr[b];
        return UAM_OK;
    }
    case ARM_SR_USRREG:
        *val = usrreg_read(cpu, id & 0xF);
        return UAM_OK;
    case ARM_SR_BANKSP: {
        int b = bank_of(id & 0x1F);
        if (b < 0)
            return UAM_ERR_ILLEGAL;
        *val = b == cur_bank(a) ? reg_r(cpu, 13) : a->s.r13[b];
        return UAM_OK;
    }
    default:
        return UAM_ERR_ILLEGAL;
    }
}

static uint32_t field_mask(unsigned mask)
{
    uint32_t m = 0;
    for (unsigned k = 0; k < 4; k++)
        if (mask & (1u << k))
            m |= 0xFFu << (8 * k);
    return m;
}

static uam_status arm_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t v64)
{
    arm_state *a = arm_st(cpu);
    uint32_t v = (uint32_t)v64;
    bool user = cur_mode(a) == ARM_MODE_USR;
    if (ARM_SR_IS_CP(id)) {
        if (user && !user_ok(a, id, true))
            return UAM_ERR_ILLEGAL;
        if ((id >> 16 & 0xF) == 14)
            return UAM_OK;                      /* CP14 debug: WI */
        return cp15_write(cpu, id, v64);
    }
    switch (id & 0xFFFF0000u) {
    case ARM_SR_CPSR: {
        if (user)
            return UAM_OK;                      /* only the flags (written by the IR) */
        if (id & 2)
            a->s.cpsr = (a->s.cpsr & ~CPSR_A) | (v & CPSR_A);
        if (id & 1) {
            a->s.cpsr = (a->s.cpsr & ~(CPSR_I | CPSR_F)) | (v & (CPSR_I | CPSR_F));
            if (bank_of(v & CPSR_M_MASK) >= 0)
                switch_mode(cpu, v & CPSR_M_MASK);
        }
        priv_update(cpu);                       /* an unmasked interrupt may be pending */
        return UAM_OK;
    }
    case ARM_SR_SPSR: {
        int b = cur_bank(a);
        if (b != ARM_BANK_USR) {
            uint32_t m = field_mask(id & 0xF);
            a->s.spsr[b] = (a->s.spsr[b] & ~m) | (v & m);
        }
        return UAM_OK;
    }
    case ARM_SR_CPS: {
        if (user)
            return UAM_OK;
        unsigned imod = (v >> 6) & 3;
        uint32_t bits = ((v & 4) ? CPSR_A : 0) | ((v & 2) ? CPSR_I : 0) | ((v & 1) ? CPSR_F : 0);
        if (imod == 2)
            a->s.cpsr &= ~bits;
        else if (imod == 3)
            a->s.cpsr |= bits;
        if (v & (1u << 5))
            switch_mode(cpu, (v >> 8) & 0x1F);
        priv_update(cpu);
        return UAM_OK;
    }
    case ARM_SR_USRREG:
        usrreg_write(cpu, id & 0xF, v);
        return UAM_OK;
    case ARM_SR_BANKSP: {
        int b = bank_of(id & 0x1F);
        if (b < 0)
            return UAM_ERR_ILLEGAL;
        if (b == cur_bank(a))
            reg_w(cpu, 13, v);
        else
            a->s.r13[b] = v;
        return UAM_OK;
    }
    default:
        return UAM_ERR_ILLEGAL;
    }
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static uam_status arm_init(uam_cpu *cpu)
{
    arm_state *a = calloc(1, sizeof(arm_state));
    if (!a)
        return UAM_ERR_NOMEM;
    cpu->isa_state = a;
    a->cfg = uam_arm_cortex_a7;
    uam_timer_init(&a->timer[0], timer_fire_phys, cpu);
    uam_timer_init(&a->timer[1], timer_fire_virt, cpu);
    return UAM_OK;
}

static void arm_fini(uam_cpu *cpu)
{
    arm_state *a = arm_st(cpu);
    if (cpu->sched)
        for (unsigned t = 0; t < 2; t++)
            uam_timer_cancel(cpu->sched, &a->timer[t]);
    free(a);
    cpu->isa_state = NULL;
}

static void arm_reset(uam_cpu *cpu, uint64_t entry)
{
    arm_state *a = arm_st(cpu);
    if (cpu->sched)
        for (unsigned t = 0; t < 2; t++)
            uam_timer_cancel(cpu->sched, &a->timer[t]);
    for (unsigned t = 0; t < 2; t++)
        if (a->s.timer_out[t] && a->sink)
            a->sink(a->sink_ctx, t, 0);
    memset(&a->s, 0, sizeof(a->s));
    for (size_t i = 0; i < cpu->regfile_size; i++)
        cpu->regfile[i] = 0;
    a->s.cpsr = ARM_MODE_SVC | CPSR_A | CPSR_I | CPSR_F;
    a->s.sctlr = 0x00C50078u;
    a->s.cntfrq = a->cfg.cntfrq;
    cpu->priv = 0;
    priv_update(cpu);
    uam_cpu_xlate_changed(cpu);
    cpu->pc = entry;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

static const char *mode_name(unsigned m)
{
    switch (m) {
    case ARM_MODE_USR: return "usr";
    case ARM_MODE_FIQ: return "fiq";
    case ARM_MODE_IRQ: return "irq";
    case ARM_MODE_SVC: return "svc";
    case ARM_MODE_ABT: return "abt";
    case ARM_MODE_UND: return "und";
    case ARM_MODE_SYS: return "sys";
    default: return "?";
    }
}

static void arm_dump_state(uam_cpu *cpu, FILE *out)
{
    arm_state *a = arm_st(cpu);
    uint32_t cpsr = cpsr_full(cpu, cpu->pc & 1, (uint8_t)ARM_PRIV_IT(cpu->priv));
    fprintf(out, "pc=%08llx icount=%llu state=%s mode=%s cpsr=%08x\n",
            (unsigned long long)(cpu->pc & ~UINT64_C(1)), (unsigned long long)cpu->icount,
            (cpu->pc & 1) ? "thumb" : "arm", mode_name(cur_mode(a)), cpsr);
    for (unsigned i = 0; i < 15; i++)
        fprintf(out, "%-3s=%08x%s", arm_regs[i].name, reg_r(cpu, i), (i % 4 == 3) ? "\n" : "  ");
    fprintf(out, "\n");
    int b = cur_bank(a);
    fprintf(out, "spsr=%08x sctlr=%08x ttbr0=%08x ttbcr=%08x dacr=%08x vbar=%08x\n",
            b == ARM_BANK_USR ? 0 : a->s.spsr[b], a->s.sctlr, a->s.ttbr0, a->s.ttbcr, a->s.dacr,
            a->s.vbar);
    fprintf(out, "dfsr=%08x dfar=%08x ifsr=%08x ifar=%08x\n", a->s.dfsr, a->s.dfar, a->s.ifsr,
            a->s.ifar);
}

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                 */
/* ------------------------------------------------------------------------ */

#define SYS_U32(X) \
    X(cpsr) X(sctlr) X(actlr) X(cpacr) X(ttbr0) X(ttbr1) X(ttbcr) X(dacr) X(dfsr) X(ifsr) \
    X(dfar) X(ifar) X(adfsr) X(aifsr) X(vbar) X(contextidr) X(tpidrurw) X(tpidruro)        \
    X(tpidrprw) X(prrr) X(nmrr) X(par) X(csselr) X(cntfrq) X(cntkctl)

static void arm_save(uam_cpu *cpu, uam_state_writer *w)
{
    const arm_state *a = arm_st(cpu);
    const arm_sys *s = &a->s;
    uam_state_put_u32(w, a->cfg.midr);
    uam_state_put_bool(w, a->irq_line);
    uam_state_put_bool(w, a->fiq_line);
    uam_state_put_u64(w, a->last_irq);
#define PUT(f) uam_state_put_u32(w, s->f);
    SYS_U32(PUT)
#undef PUT
    for (unsigned b = 0; b < ARM_BANK__COUNT; b++) {
        uam_state_put_u32(w, s->spsr[b]);
        uam_state_put_u32(w, s->r13[b]);
        uam_state_put_u32(w, s->r14[b]);
    }
    for (unsigned i = 0; i < 5; i++) {
        uam_state_put_u32(w, s->usr_r8[i]);
        uam_state_put_u32(w, s->fiq_r8[i]);
    }
    for (unsigned t = 0; t < 2; t++) {
        uam_state_put_u32(w, s->cnt_ctl[t]);
        uam_state_put_u64(w, s->cnt_cval[t]);
        uam_state_put_bool(w, s->timer_out[t]);
        uam_state_put_timer(w, &a->timer[t]);
    }
    uam_state_put_u64(w, s->cntvoff);
}

static void arm_restore(uam_cpu *cpu, uam_state_reader *r)
{
    arm_state *a = arm_st(cpu);
    arm_sys *s = &a->s;
    uam_state_expect_u32(r, a->cfg.midr, "ARM MIDR");
    a->irq_line = uam_state_get_bool(r);
    a->fiq_line = uam_state_get_bool(r);
    a->last_irq = uam_state_get_u64(r);
#define GET(f) s->f = uam_state_get_u32(r);
    SYS_U32(GET)
#undef GET
    for (unsigned b = 0; b < ARM_BANK__COUNT; b++) {
        s->spsr[b] = uam_state_get_u32(r);
        s->r13[b] = uam_state_get_u32(r);
        s->r14[b] = uam_state_get_u32(r);
    }
    for (unsigned i = 0; i < 5; i++) {
        s->usr_r8[i] = uam_state_get_u32(r);
        s->fiq_r8[i] = uam_state_get_u32(r);
    }
    for (unsigned t = 0; t < 2; t++) {
        s->cnt_ctl[t] = uam_state_get_u32(r);
        s->cnt_cval[t] = uam_state_get_u64(r);
        s->timer_out[t] = uam_state_get_bool(r);
        uam_state_get_timer(r, &a->timer[t]);
    }
    s->cntvoff = uam_state_get_u64(r);
}

/* ------------------------------------------------------------------------ */
/* Debugger view (gdb: org.gnu.gdb.arm.core)                                 */
/* ------------------------------------------------------------------------ */

#define GDB_ARM_NREGS 17u   /* r0-r15, cpsr */

#define GDB_REG(n, num, ty) "<reg name=\"" n "\" bitsize=\"32\" regnum=\"" #num "\"" ty "/>"
static const char arm_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>arm</architecture>"
    "<feature name=\"org.gnu.gdb.arm.core\">"
    GDB_REG("r0", 0, "") GDB_REG("r1", 1, "") GDB_REG("r2", 2, "") GDB_REG("r3", 3, "")
    GDB_REG("r4", 4, "") GDB_REG("r5", 5, "") GDB_REG("r6", 6, "") GDB_REG("r7", 7, "")
    GDB_REG("r8", 8, "") GDB_REG("r9", 9, "") GDB_REG("r10", 10, "") GDB_REG("r11", 11, "")
    GDB_REG("r12", 12, "") GDB_REG("sp", 13, " type=\"data_ptr\"")
    GDB_REG("lr", 14, "") GDB_REG("pc", 15, " type=\"code_ptr\"") GDB_REG("cpsr", 16, "")
    "</feature></target>";
#undef GDB_REG

static bool arm_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n < 15)       *v = reg_r(cpu, n);
    else if (n == 15) *v = (uint32_t)cpu->pc & ~1u;
    else if (n == 16) *v = cpsr_full(cpu, cpu->pc & 1, (uint8_t)ARM_PRIV_IT(cpu->priv));
    else return false;
    return true;
}

static bool arm_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n < 15) {
        reg_w(cpu, n, (uint32_t)v);
    } else if (n == 15) {
        cpu->pc = ((uint32_t)v & ~1u) | (cpu->pc & 1);
    } else if (n == 16) {
        write_cpsr_full(cpu, (uint32_t)v);
        cpu->pc = (cpu->pc & ~UINT64_C(1)) | (((uint32_t)v & CPSR_T) ? 1 : 0);
    } else {
        return false;
    }
    return true;
}

static const uam_gdb_arch arm_gdb = {
    .target_xml = arm_target_xml,
    .n_regs = GDB_ARM_NREGS,
    .reg_bytes = 4,
    .read_reg = arm_gdb_read,
    .write_reg = arm_gdb_write,
    .pc_state_bits = 1,   /* Thumb */
};

/* ------------------------------------------------------------------------ */
/* Descriptor                                                                */
/* ------------------------------------------------------------------------ */

const uam_isa uam_isa_arm = {
    .name           = "armv7",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,
    .regfile_size   = ARM_REGFILE_SIZE,
    .regs           = arm_regs,
    .n_regs         = UAM_ARRAY_LEN(arm_regs),
    .helpers        = { arm_helpers, ARM_HELPER__COUNT },
    .init           = arm_init,
    .fini           = arm_fini,
    .reset          = arm_reset,
    .lift           = arm_lift_block,
    .take_exception = arm_take_exception,
    .irq_take       = arm_irq_take,
    .irq_wakeup     = arm_irq_wakeup,
    .irq_input      = arm_irq_input,
    .sysreg_read    = arm_sysreg_read,
    .sysreg_write   = arm_sysreg_write,
    .translate      = arm_translate,
    .gdb            = &arm_gdb,
    .dump_state     = arm_dump_state,
    .save           = arm_save,
    .restore        = arm_restore,
    .irq_lines      = arm_irq_lines,
};
