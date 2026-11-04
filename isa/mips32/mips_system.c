/*
 * mips_system.c - MIPS32 privileged architecture and the uam_isa descriptors.
 *
 * Implements CP0 (Status/Cause/EPC/BadVAddr/Count/Compare/Config/TLB
 * registers/...), the exception and interrupt entry sequence (including the
 * TLB refill vector and branch-delay-slot reporting through Cause.BD), ERET,
 * the CP0 Count/Compare timer (driven by the scheduler), the eight-level
 * interrupt model, and address translation:
 *
 *   kuseg 0x00000000-0x7fffffff  mapped through the TLB (unmapped while ERL=1)
 *   kseg0 0x80000000-0x9fffffff  unmapped, kernel only
 *   kseg1 0xa0000000-0xbfffffff  unmapped, kernel only
 *   sseg  0xc0000000-0xdfffffff  mapped, supervisor or kernel
 *   kseg3 0xe0000000-0xffffffff  mapped, kernel only
 *
 * The TLB is an R4K-style joint TLB of even/odd page pairs with ASIDs and
 * variable page sizes. Entries reset to zero, so with an
 * uninitialised TLB an access to 0x0000-0x1fff hits entry 0 and raises a
 * TLB-invalid exception, while other mapped addresses raise a TLB refill.
 *
 * With Config.MT = 3 (fixed mapping translation, no TLB) kuseg maps to
 * physical va + 0x40000000 (unmapped while ERL=1), kseg0/kseg1 as above and
 * kseg2/kseg3 to themselves.
 *
 * Interrupts run in one of three modes: compatibility (Cause.IP masked by
 * Status.IM, one interrupt vector), vectored (Config3.VInt with IntCtl.VS != 0:
 * one vector per IP line, the highest pending line wins) and external (Config3.VEIC:
 * the controller's request, taken when its priority level exceeds Status.IPL,
 * at the vector it names). With shadow register sets (SRSCtl.HSS > 0) an
 * exception or interrupt taken with EXL = 0 and BEV = 0 switches to the set
 * SRSCtl.ESS, SRSMap or the controller names, and ERET switches back.
 *
 * The CPU identity (PRId, Config*, TLB size, writable Status bits) comes from
 * a uam_mips_config chosen by the board (default: uam_mips_ref).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/sched.h"

#include "mips.h"

/* ------------------------------------------------------------------------ */
/* Register descriptors                                                      */
/* ------------------------------------------------------------------------ */

#define G(n, i) { n, MIPS_REG_OFF(i), 32, UAM_REG_GPR }
static const uam_regdesc mips_regs[] = {
    G("zero", 0), G("at", 1),  G("v0", 2),  G("v1", 3),  G("a0", 4),  G("a1", 5),
    G("a2", 6),   G("a3", 7),  G("t0", 8),  G("t1", 9),  G("t2", 10), G("t3", 11),
    G("t4", 12),  G("t5", 13), G("t6", 14), G("t7", 15), G("s0", 16), G("s1", 17),
    G("s2", 18),  G("s3", 19), G("s4", 20), G("s5", 21), G("s6", 22), G("s7", 23),
    G("t8", 24),  G("t9", 25), G("k0", 26), G("k1", 27), G("gp", 28), G("sp", 29),
    G("fp", 30),  G("ra", 31),
    { "hi", MIPS_HI_OFF, 32, UAM_REG_OTHER }, { "lo", MIPS_LO_OFF, 32, UAM_REG_OTHER },
};
#undef G

/* ------------------------------------------------------------------------ */
/* CPU models                                                                */
/* ------------------------------------------------------------------------ */

const uam_mips_config uam_mips_ref = {
    .name        = "mips32r2-ref",
    .prid        = 0x00FF0000u,  /* company 0xFF (unassigned), processor 0, rev 0 */
    /* M=1, AR=1 (release 2), MT=1 (standard TLB), K0=2 (uncached) */
    .config0     = (1u << 31) | (1u << 10) | (1u << 7) | 0x2u,
    /* M=1, IL=3 IA=1 (I$ 32B lines, 2-way), DL=3 DA=1, EP=1 (EJTAG). No FPU,
     * no MIPS16, no watch registers. MMUSize is filled in from tlb_entries. */
    .config1     = (1u << 31) | (3u << 19) | (1u << 16) | (3u << 10) | (1u << 7) | (1u << 1),
    .config2     = 1u << 31,     /* M: Config3 present */
    .config3     = 0,
    /* CU0, RE, BEV, TS, SR, NMI, IM[7:0], KSU[1] (no supervisor mode), ERL, EXL, IE */
    .status_rw   = 0x1278FF17u,
    .tlb_entries = 16,
};

/* MIPS32r2 + MIPS16e, fixed mapping, no caches, one shadow set, EIC (PIC32MX class). */
const uam_mips_config uam_mips_m16_ref = {
    .name        = "mips32r2-mips16e-ref",
    .prid        = 0x00FF0100u,
    .config0     = (1u << 31) | (1u << 10) | (3u << 7) | 0x2u,     /* M, AR=1, MT=3 (FMT), K0 */
    .config1     = (1u << 31) | CFG1_CA | (1u << 1),                /* M, CA (MIPS16e), EP */
    .config2     = 1u << 31,
    .config3     = CFG3_VINT | CFG3_VEIC,
    .status_rw   = 0x1278FF17u,
    .tlb_entries = 0,
    .shadow_sets = 1,
};

/* microMIPS32 only, fixed mapping, MCU ASE, one shadow set, EIC (PIC32MM class). */
const uam_mips_config uam_mips_umips_ref = {
    .name        = "micromips32-ref",
    .prid        = 0x00FF0200u,
    .config0     = (1u << 31) | (1u << 10) | (3u << 7) | 0x2u,
    .config1     = (1u << 31) | (1u << 1),
    .config2     = 1u << 31,
    .config3     = CFG3_VINT | CFG3_VEIC | (1u << CFG3_ISA_SHIFT) | CFG3_ISAONEXC | CFG3_MCU,
    .status_rw   = 0x1278FF17u,
    .tlb_entries = 0,
    .shadow_sets = 1,
};

/* MIPS32r2 + microMIPS (MIPS32 at reset), 16-entry TLB, caches, the FPU with 64-bit
 * registers, DSP ASE rev 2, MCU ASE, seven shadow sets, EIC (PIC32MZ EF class). */
const uam_mips_config uam_mips_mz_ref = {
    .name        = "mips32r2-mz-ref",
    .prid        = 0x00FF0300u,
    .config0     = (1u << 31) | (1u << 10) | (1u << 7) | 0x2u,
    .config1     = (1u << 31) | (3u << 19) | (1u << 16) | (3u << 10) | (1u << 7) | (1u << 1) | CFG1_FP,
    .config2     = 1u << 31,
    .config3     = CFG3_VINT | CFG3_VEIC | CFG3_DSPP | CFG3_DSP2P | (2u << CFG3_ISA_SHIFT) | CFG3_MCU,
    .status_rw   = 0x1278FF17u | ST_CU1 | ST_FR | ST_MX,
    .tlb_entries = 16,
    .shadow_sets = 7,
    /* F64, L, W, D, S; processor id 0x01 */
    .fir         = (1u << 22) | (1u << 21) | (1u << 20) | (1u << 17) | (1u << 16) | (0x01u << 8),
};

static bool is_mips(const uam_cpu *cpu)
{
    return cpu->isa == &uam_isa_mips32be || cpu->isa == &uam_isa_mips32le;
}

uam_status uam_mips_configure(uam_cpu *cpu, const uam_mips_config *cfg)
{
    if (!cpu || !cfg || !is_mips(cpu) || !cpu->isa_state)
        return UAM_ERR_INVALID;
    bool tlb = CFG0_MT(cfg->config0) == 1;
    if (tlb ? cfg->tlb_entries < 1 || cfg->tlb_entries > MIPS_TLB_MAX : cfg->tlb_entries != 0)
        return UAM_ERR_INVALID;
    if (CFG0_MT(cfg->config0) != 1 && CFG0_MT(cfg->config0) != 3)
        return UAM_ERR_INVALID;     /* a TLB or fixed mapping */
    if (cfg->shadow_sets >= MIPS_SRS_MAX)
        return UAM_ERR_INVALID;
    if ((cfg->config1 & CFG1_CA) && (cfg->config3 & CFG3_ISA_MASK))
        return UAM_ERR_INVALID;     /* MIPS16e and microMIPS exclude each other */
    if ((cfg->config3 & CFG3_DSP2P) && !(cfg->config3 & CFG3_DSPP))
        return UAM_ERR_INVALID;
    mips_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Operating mode                                                            */
/* ------------------------------------------------------------------------ */

static unsigned effective_mode(const mips_state *s)
{
    if (s->c.status & (ST_EXL | ST_ERL))
        return MIPS_KERNEL;
    switch ((s->c.status & ST_KSU_MASK) >> ST_KSU_SHIFT) {
    case 0:  return MIPS_KERNEL;
    case 1:  return MIPS_SUPERVISOR;
    default: return MIPS_USER;
    }
}

/* Call after any change to Status: keeps cpu->priv (which tags translated
 * code) current, and reports ERL changes, which remap kuseg. */
static void mode_update(uam_cpu *cpu)
{
    mips_state *s = mips_st(cpu);
    unsigned p = effective_mode(s);
    if (s->c.status & ST_CU1) p |= MIPS_PRIV_CU1;
    if (s->c.status & ST_FR)  p |= MIPS_PRIV_FR;
    if (s->c.status & ST_MX)  p |= MIPS_PRIV_MX;
    cpu->priv = p;
    bool erl = s->c.status & ST_ERL;
    if (erl != s->erl_seen) {
        s->erl_seen = erl;
        uam_cpu_xlate_changed(cpu);
    }
}

/* CP0 and privileged instructions are usable in kernel mode or with CU0. */
static bool cp0_usable(uam_cpu *cpu)
{
    return MIPS_PRIV_MODE(cpu->priv) == MIPS_KERNEL || (mips_st(cpu)->c.status & ST_CU0);
}

/* ------------------------------------------------------------------------ */
/* Count/Compare timer                                                       */
/* ------------------------------------------------------------------------ */

/* CP0 Count runs at half the scheduler tick rate (the usual MIPS ratio). */
uint32_t mips_count_now(uam_cpu *cpu)
{
    mips_state *s = mips_st(cpu);
    return (uint32_t)((uam_sched_now(cpu->sched) >> 1) + s->c.count_bias);
}

static void timer_fire(void *ctx)
{
    uam_cpu *cpu = ctx;
    mips_state *s = mips_st(cpu);
    s->c.timer_ip = true;
    uam_signal_set(&s->timer_line, 1);
    uam_cpu_request_exit(cpu);
}

/* Arm the scheduler timer for the next Count==Compare crossing. */
static void timer_update(uam_cpu *cpu)
{
    mips_state *s = mips_st(cpu);
    uint32_t count = mips_count_now(cpu);
    /* Ticks until Count reaches Compare, accounting for the /2 ratio and wrap. */
    uint32_t delta_count = s->c.compare - count; /* unsigned wrap gives time to next match */
    uint64_t when = uam_sched_now(cpu->sched) + (uint64_t)delta_count * 2 + 1;
    uam_timer_arm(cpu->sched, &s->timer, when);
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                                */
/* ------------------------------------------------------------------------ */

static uint32_t cause_ip(const mips_state *s)
{
    uint32_t ip = (s->c.cause >> CAUSE_IP_SHIFT) & 0x3;   /* software IP1:0 */
    ip |= s->hwlines & 0xFC;                              /* hardware IP7:2 */
    if (s->c.timer_ip)
        ip |= 1u << 7;
    return ip & 0xFF;
}

static uint32_t cause_read(const mips_state *s)
{
    if (mips_has_eic(s)) {
        /* IP7:2 are the controller's RIPL; TI shows the timer, which goes to it. */
        uint32_t c = s->c.cause & ~(CAUSE_IP_MASK | CAUSE_TI);
        c |= (s->c.cause & (0x3u << CAUSE_IP_SHIFT)) | ((s->eic.ripl & 0x3F) << CAUSE_RIPL_SHIFT);
        return c | (s->c.timer_ip ? CAUSE_TI : 0);
    }
    return (s->c.cause & ~CAUSE_IP_MASK) | (cause_ip(s) << CAUSE_IP_SHIFT);
}

/* Vectored interrupts are in effect: VInt, IntCtl.VS != 0, Cause.IV, BEV clear. */
static bool vectored(const mips_state *s)
{
    return (s->cfg.config3 & CFG3_VINT) && (s->c.intctl & 0x3E0u) && (s->c.cause & CAUSE_IV)
        && !(s->c.status & ST_BEV);
}

static bool int_pending(const mips_state *s)
{
    if (mips_has_eic(s))
        return s->eic.ripl > ((s->c.status & ST_IPL_MASK) >> ST_IPL_SHIFT);
    uint32_t im = (s->c.status & ST_IM_MASK) >> ST_IM_SHIFT;
    return (cause_ip(s) & im) != 0;
}

static bool ints_enabled(const mips_state *s)
{
    return (s->c.status & ST_IE) && !(s->c.status & (ST_EXL | ST_ERL));
}

uam_signal *uam_mips_timer_line(uam_cpu *cpu)
{
    return cpu && is_mips(cpu) && cpu->isa_state ? &mips_st(cpu)->timer_line : NULL;
}

uam_signal *uam_mips_swint_line(uam_cpu *cpu, unsigned n)
{
    return cpu && is_mips(cpu) && cpu->isa_state && n < 2 ? &mips_st(cpu)->swint_line[n] : NULL;
}

void uam_mips_eic_input(void *ctx, unsigned ripl, unsigned vector, uint32_t offset,
                        unsigned shadow_set)
{
    uam_cpu *cpu = ctx;
    mips_state *s = mips_st(cpu);
    s->eic = (mips_eic){ .ripl = ripl & 0x3F, .vector = vector, .offset = offset,
                         .ss = shadow_set & 0xF };
    uam_cpu_request_exit(cpu);
}

/* ------------------------------------------------------------------------ */
/* TLB and address translation                                               */
/* ------------------------------------------------------------------------ */

#define ENTRYHI_ASID   0x000000FFu
#define ENTRYHI_VPN2   0xFFFFE000u
#define ENTRYLO_G      0x1u
#define ENTRYLO_V      0x2u
#define ENTRYLO_D      0x4u
#define ENTRYLO_MASK   0x03FFFFFFu  /* PFN[25:6] (32-bit physical) C D V G */
#define PAGEMASK_MASK  0x1FFFE000u
#define CONTEXT_PTE    0xFF800000u  /* PTEBase; BadVPN2 is bits 22:4 */

/* Index of the entry matching va under the current ASID, or -1. */
static int tlb_match(const mips_state *s, uint32_t va)
{
    uint8_t asid = (uint8_t)(s->c.entryhi & ENTRYHI_ASID);
    for (unsigned i = 0; i < s->cfg.tlb_entries; i++) {
        const mips_tlb_entry *e = &s->c.tlb[i];
        uint32_t m = ~(e->mask | 0x1FFFu);  /* VPN2 compare mask */
        if ((e->g || e->asid == asid) && ((va ^ e->vpn2) & m) == 0)
            return (int)i;
    }
    return -1;
}

static uam_status tlb_translate(const mips_state *s, uint32_t va, uam_xlate_kind kind, uam_xlate *out)
{
    int i = tlb_match(s, va);
    if (i < 0) {
        out->fault = MIPS_XF_REFILL;
        return UAM_ERR_XLATE;
    }
    const mips_tlb_entry *e = &s->c.tlb[i];
    uint32_t page_mask = (e->mask >> 1) | 0xFFFu;        /* offset bits within one page */
    uint32_t lo = e->lo[(va & (page_mask + 1)) ? 1 : 0]; /* odd or even page */
    if (!(lo & ENTRYLO_V)) {
        out->fault = MIPS_XF_INVALID;
        return UAM_ERR_XLATE;
    }
    if (kind == UAM_XLATE_STORE && !(lo & ENTRYLO_D)) {
        out->fault = MIPS_XF_MOD;
        return UAM_ERR_XLATE;
    }
    uint64_t pfn_base = (uint64_t)((lo >> 6) & 0xFFFFFu) << 12;
    out->paddr = (pfn_base & ~(uint64_t)page_mask) | (va & page_mask);
    out->fixed = false;
    return UAM_OK;
}

static uam_status mips_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out)
{
    const mips_state *s = mips_st(cpu);
    uint32_t va = (uint32_t)vaddr;
    unsigned mode = kind == UAM_XLATE_DEBUG ? MIPS_KERNEL : MIPS_PRIV_MODE(cpu->priv);
    bool fmt = CFG0_MT(s->cfg.config0) == 3;

    out->fixed = true;
    /* Instructions are word-aligned, or halfword-aligned with a compressed ISA
     * (whose blocks fetch at pc & ~1; the lifter checks MIPS32 alignment). */
    if (kind == UAM_XLATE_FETCH && (va & (mips_compressed(s) ? 1u : 3u))) {
        out->fault = MIPS_XF_ADDR;
        return UAM_ERR_XLATE;
    }
    if (va < 0x80000000u) {                       /* kuseg */
        if (s->c.status & ST_ERL) {
            /* Unmapped while ERL=1 (reset and error handlers). Not fixed:
             * the mapping changes with ERL, see mode_update(). */
            out->paddr = va;
            out->fixed = false;
            return UAM_OK;
        }
        if (fmt) {
            out->paddr = (uint32_t)(va + 0x40000000u);
            out->fixed = false;                   /* also changes with ERL */
            return UAM_OK;
        }
        return tlb_translate(s, va, kind, out);
    }
    if (va >= 0xC0000000u && va < 0xE0000000u) { /* sseg / kseg2 */
        if (mode == MIPS_USER) {
            out->fault = MIPS_XF_ADDR;
            return UAM_ERR_XLATE;
        }
        if (fmt) {
            out->paddr = va;
            return UAM_OK;
        }
        return tlb_translate(s, va, kind, out);
    }
    if (mode != MIPS_KERNEL) {                    /* kseg0, kseg1, kseg3 */
        out->fault = MIPS_XF_ADDR;
        return UAM_ERR_XLATE;
    }
    if (va >= 0xE0000000u) {                      /* kseg3 */
        if (fmt) {
            out->paddr = va;
            return UAM_OK;
        }
        return tlb_translate(s, va, kind, out);
    }
    out->paddr = va & 0x1FFFFFFFu;                /* kseg0 / kseg1 */
    return UAM_OK;
}

/* Random: deterministic stand-in for the free-running counter, always in
 * [Wired, entries-1]. */
static uint32_t tlb_random(uam_cpu *cpu)
{
    const mips_state *s = mips_st(cpu);
    uint32_t n = s->cfg.tlb_entries, wired = s->c.wired < n ? s->c.wired : 0;
    if (!n)
        return 0;
    return n - 1 - (uint32_t)(cpu->icount % (n - wired));
}

static void tlb_write(uam_cpu *cpu, uint32_t idx)
{
    mips_state *s = mips_st(cpu);
    mips_tlb_entry *e = &s->c.tlb[idx % s->cfg.tlb_entries];
    e->mask = s->c.pagemask & PAGEMASK_MASK;
    e->vpn2 = s->c.entryhi & ENTRYHI_VPN2 & ~e->mask;
    e->asid = (uint8_t)(s->c.entryhi & ENTRYHI_ASID);
    e->g = (s->c.entrylo0 & s->c.entrylo1 & ENTRYLO_G) != 0;
    e->lo[0] = s->c.entrylo0 & ENTRYLO_MASK & ~ENTRYLO_G;
    e->lo[1] = s->c.entrylo1 & ENTRYLO_MASK & ~ENTRYLO_G;
    uam_cpu_xlate_changed(cpu);
}

/* TLB instruction helpers: privileged, no operands. */
static uint64_t helper_tlbr(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    mips_state *s = mips_st(cpu);
    const mips_tlb_entry *e = &s->c.tlb[(s->c.index & 0x3F) % s->cfg.tlb_entries];
    uint32_t g = e->g ? ENTRYLO_G : 0;
    uint32_t old_asid = s->c.entryhi & ENTRYHI_ASID;
    s->c.entryhi = e->vpn2 | e->asid;
    s->c.pagemask = e->mask;
    s->c.entrylo0 = e->lo[0] | g;
    s->c.entrylo1 = e->lo[1] | g;
    if (e->asid != old_asid)
        uam_cpu_xlate_changed(cpu);
    return 0;
}

static uint64_t helper_tlbwi(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    tlb_write(cpu, mips_st(cpu)->c.index & 0x3F);
    return 0;
}

static uint64_t helper_tlbwr(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    tlb_write(cpu, tlb_random(cpu));
    return 0;
}

static uint64_t helper_tlbp(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    mips_state *s = mips_st(cpu);
    int i = tlb_match(s, s->c.entryhi & ENTRYHI_VPN2);
    if (i >= 0)
        s->c.index = (uint32_t)i;
    else
        s->c.index |= 0x80000000u;  /* P: probe failed (the index bits are unpredictable;
                                     * they are left unchanged) */
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Exception entry                                                           */
/* ------------------------------------------------------------------------ */

enum vec { VEC_GENERAL, VEC_REFILL, VEC_INTERRUPT };

static uint32_t exc_base(const mips_state *s)
{
    return (s->c.status & ST_BEV) ? 0xBFC00200u : (s->c.ebase & 0xFFFFF000u);
}

static uint32_t exc_vector(const mips_state *s, enum vec v)
{
    uint32_t base = exc_base(s);
    switch (v) {
    case VEC_REFILL:    return base + ((s->c.status & ST_EXL) ? 0x180 : 0x000);
    case VEC_INTERRUPT: return base + ((s->c.cause & CAUSE_IV) ? 0x200 : 0x180);
    default:            return base + 0x180;
    }
}

/* The GPR set `css` becomes current: the register file holds the current set. */
static void srs_switch(uam_cpu *cpu, unsigned css)
{
    mips_state *s = mips_st(cpu);
    unsigned old = s->c.srsctl & 0xFu;
    if (css == old)
        return;
    memcpy(s->c.srs[old], cpu->regfile, 32 * 4);
    memcpy(cpu->regfile, s->c.srs[css], 32 * 4);
    s->c.srsctl = (s->c.srsctl & ~0xFu) | css;
}

/*
 * The common entry sequence. EPC and Cause.BD are only written when EXL was
 * clear; an exception in a delay slot (cont) reports the branch in EPC. With
 * shadow sets the set `ss` (SRSCtl.ESS for exceptions) becomes current, the
 * old one recorded in PSS. Handlers run in microMIPS when Config3.ISAOnExc says so.
 */
static void enter_exception_at(uam_cpu *cpu, uint32_t exccode, uint32_t pc, bool cont,
                               uint32_t parent_pc, uint32_t vector, unsigned ss)
{
    mips_state *s = mips_st(cpu);
    if (!(s->c.status & ST_EXL)) {
        s->c.epc = cont ? parent_pc : pc;
        s->c.cause = cont ? (s->c.cause | CAUSE_BD) : (s->c.cause & ~CAUSE_BD);
        if (s->cfg.shadow_sets && !(s->c.status & ST_BEV)) {
            unsigned css = s->c.srsctl & 0xFu;
            s->c.srsctl = (s->c.srsctl & ~(0xFu << 6)) | (css << 6);
            srs_switch(cpu, ss <= s->cfg.shadow_sets ? ss : 0);
        }
    }
    s->c.cause = (s->c.cause & ~(CAUSE_EXC_MASK | CAUSE_CE_MASK))
               | ((exccode << CAUSE_EXC_SHIFT) & CAUSE_EXC_MASK);
    s->c.status |= ST_EXL;
    cpu->resv.valid = false;
    cpu->pc = vector | (mips_has_umips(s) && (s->c.config3 & CFG3_ISAONEXC) ? 1u : 0u);
    mode_update(cpu);
}

static unsigned srs_ess(const mips_state *s) { return (s->c.srsctl >> 12) & 0xFu; }

static void enter_exception(uam_cpu *cpu, uint32_t exccode, uint32_t pc, bool cont,
                            uint32_t parent_pc, enum vec v)
{
    mips_state *s = mips_st(cpu);
    uint32_t vector = exc_vector(s, v);  /* depends on the old EXL */
    enter_exception_at(cpu, exccode, pc, cont, parent_pc, vector, srs_ess(s));
}

/* BadVAddr, Context.BadVPN2 and EntryHi.VPN2 for TLB exceptions. */
static void tlb_fault_regs(mips_state *s, uint32_t va)
{
    s->c.badvaddr = va;
    s->c.context = (s->c.context & CONTEXT_PTE) | ((va >> 9) & 0x007FFFF0u);
    s->c.entryhi = (va & ENTRYHI_VPN2) | (s->c.entryhi & ENTRYHI_ASID);
}

/* Map a translation fault sub-cause to ExcCode/vector, setting fault registers. */
static uint32_t xlate_exception(mips_state *s, uint32_t xf, bool store, uint32_t va, enum vec *v)
{
    *v = VEC_GENERAL;
    switch (xf) {
    case MIPS_XF_REFILL:
        tlb_fault_regs(s, va);
        *v = VEC_REFILL;
        return store ? EXC_TLBS : EXC_TLBL;
    case MIPS_XF_INVALID:
        tlb_fault_regs(s, va);
        return store ? EXC_TLBS : EXC_TLBL;
    case MIPS_XF_MOD:
        tlb_fault_regs(s, va);
        return EXC_MOD;
    case MIPS_XF_ADDR:
    default:
        s->c.badvaddr = va;
        return store ? EXC_ADES : EXC_ADEL;
    }
}

static void mips_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    mips_state *s = mips_st(cpu);
    uint32_t va = (uint32_t)e->tval, code, ce = 0;
    enum vec v = VEC_GENERAL;
    switch (e->kind) {
    case UAM_EXC_TRAP:
        code = (uint32_t)e->cause;  /* the lifter passes the ExcCode */
        if (code == MIPS_TRAP_SLOT_FETCH) {
            /* Delay slot not fetchable: find out why, precisely. */
            uam_xlate xl = { 0 };
            if (mips_translate(cpu, va, UAM_XLATE_FETCH, &xl) == UAM_ERR_XLATE)
                code = xlate_exception(s, xl.fault, false, va, &v);
            else
                code = EXC_IBE;
        } else if (code == MIPS_TRAP_FETCH_ADDR) {
            code = EXC_ADEL;
            s->c.badvaddr = va;
        } else if (code == EXC_CPU) {
            ce = va & 3;            /* tval carries the coprocessor number */
        }
        break;
    case UAM_EXC_FETCH_XLATE:      code = xlate_exception(s, (uint32_t)e->cause, false, va, &v); break;
    case UAM_EXC_LOAD_XLATE:       code = xlate_exception(s, (uint32_t)e->cause, false, va, &v); break;
    case UAM_EXC_STORE_XLATE:      code = xlate_exception(s, (uint32_t)e->cause, true, va, &v); break;
    case UAM_EXC_FETCH_FAULT:      code = EXC_IBE; break;
    case UAM_EXC_LOAD_FAULT:       code = EXC_DBE; break;
    case UAM_EXC_STORE_FAULT:      code = EXC_DBE; break;
    case UAM_EXC_LOAD_MISALIGNED:  code = EXC_ADEL; s->c.badvaddr = va; break;
    case UAM_EXC_STORE_MISALIGNED: code = EXC_ADES; s->c.badvaddr = va; break;
    case UAM_EXC_HELPER: {
        unsigned hf = s->hfault;
        s->hfault = MIPS_HF_NONE;
        code = hf == MIPS_HF_FPE ? EXC_FPE : hf == MIPS_HF_RI ? EXC_RI : EXC_CPU;
        break;                     /* EXC_CPU: a privileged helper (ERET, TLB ops, */
    }                              /* RDPGPR) outside kernel mode without CU0 */
    case UAM_EXC_SYSREG:           /* CP0 access outside kernel mode without CU0 */
        code = EXC_CPU;
        break;
    default:
        code = EXC_RI;
        break;
    }
    enter_exception(cpu, code, (uint32_t)e->pc, e->flags & UAM_EXC_F_CONT,
                    (uint32_t)e->parent_pc, v);
    s->c.cause |= ce << CAUSE_CE_SHIFT;
}

/* The vector number and shadow set of the interrupt to take (vectored mode): the
 * highest pending, unmasked IP line. */
static unsigned vi_vector(const mips_state *s)
{
    uint32_t p = cause_ip(s) & ((s->c.status & ST_IM_MASK) >> ST_IM_SHIFT);
    unsigned n = 7;
    while (n && !(p & (1u << n)))
        n--;
    return n;
}

static bool mips_irq_take(uam_cpu *cpu)
{
    mips_state *s = mips_st(cpu);
    if (!ints_enabled(s) || !int_pending(s))
        return false;
    /* Interrupts are only taken between blocks, never inside a branch/delay
     * slot pair (UAM_IMARK_CONT), so EPC is simply the next pc. */
    uint32_t pc = (uint32_t)cpu->pc;
    uint32_t spacing = ((s->c.intctl >> 5) & 0x1Fu) * 32u;
    bool iv = (s->c.cause & CAUSE_IV) && !(s->c.status & ST_BEV);
    if (mips_has_eic(s)) {
        uint32_t vector = !iv ? exc_vector(s, VEC_INTERRUPT)
                        : s->eic.offset != UAM_MIPS_EIC_NO_OFFSET ? exc_base(s) + s->eic.offset
                        : exc_base(s) + 0x200 + s->eic.vector * spacing;
        enter_exception_at(cpu, EXC_INT, pc, false, 0, vector, s->eic.ss);
    } else if (vectored(s)) {
        unsigned vn = vi_vector(s);
        enter_exception_at(cpu, EXC_INT, pc, false, 0, exc_base(s) + 0x200 + vn * spacing,
                           (s->c.srsmap >> (4 * vn)) & 0xFu);
    } else {
        enter_exception(cpu, EXC_INT, pc, false, 0, VEC_INTERRUPT);
    }
    return true;
}

/* MIPS has no fixed priority among interrupt lines (the handler reads Cause.IP): report
 * every pending, enabled line (IP0..IP7, bit i = line i; IP7 includes the CP0 timer).
 * With an external controller: bit `vector` (63 for higher vectors) of a request
 * above Status.IPL. */
static uint64_t mips_irq_lines(uam_cpu *cpu)
{
    const mips_state *s = mips_st(cpu);
    if (mips_has_eic(s))
        return int_pending(s) ? UINT64_C(1) << (s->eic.vector < 63 ? s->eic.vector : 63) : 0;
    return cause_ip(s) & ((s->c.status & ST_IM_MASK) >> ST_IM_SHIFT);
}

static bool mips_irq_wakeup(uam_cpu *cpu)
{
    /* WAIT wakes on any pending+enabled line even if Status.IE is clear. */
    return int_pending(mips_st(cpu));
}

static void mips_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    mips_state *s = mips_st(cpu);
    if (line < 2 || line > 7)
        return; /* IP0/IP1 are software-only (written via Cause) */
    if (level)
        s->hwlines |= 1u << line;
    else
        s->hwlines &= ~(1u << line);
}

/* ------------------------------------------------------------------------ */
/* CP0 registers                                                             */
/* ------------------------------------------------------------------------ */

static uint32_t config1_value(const mips_state *s)
{
    uint32_t mmu = s->cfg.tlb_entries ? s->cfg.tlb_entries - 1 : 0;
    return (s->cfg.config1 & ~(0x3Fu << 25)) | (mmu << 25);
}

static bool has_tlb(const mips_state *s) { return CFG0_MT(s->cfg.config0) == 1; }

/* The CP0 registers of the TLB, absent with fixed mapping. */
static bool tlb_reg(uint32_t id)
{
    switch (id) {
    case CP0_INDEX: case CP0_RANDOM: case CP0_ENTRYLO0: case CP0_ENTRYLO1: case CP0_CONTEXT:
    case CP0_PAGEMASK: case CP0_WIRED: case CP0_ENTRYHI:
        return true;
    default:
        return false;
    }
}

static uint32_t srsctl_read(const mips_state *s)
{
    uint32_t v = s->c.srsctl & ((0xFu << 12) | (0xFu << 6) | 0xFu);
    v |= (uint32_t)s->cfg.shadow_sets << 26;
    if (mips_has_eic(s))
        v |= (s->eic.ss & 0xFu) << 18;
    return v;
}

static uint32_t intctl_read(const mips_state *s)
{
    uint32_t v = s->c.intctl;
    if ((s->cfg.config3 & (CFG3_VINT | CFG3_VEIC)) && !mips_has_eic(s))
        v |= 7u << 29;      /* IPTI: the timer is on IP7 */
    return v;
}

static uam_status mips_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    mips_state *s = mips_st(cpu);
    if (!cp0_usable(cpu))
        return UAM_ERR_ILLEGAL;
    if (!has_tlb(s) && tlb_reg(id)) {
        *val = 0;
        return UAM_OK;
    }
    switch (id) {
    case CP0_INDEX:    *val = s->c.index; break;
    case CP0_RANDOM:   *val = tlb_random(cpu); break;
    case CP0_ENTRYLO0: *val = s->c.entrylo0; break;
    case CP0_ENTRYLO1: *val = s->c.entrylo1; break;
    case CP0_CONTEXT:  *val = s->c.context; break;
    case CP0_PAGEMASK: *val = s->c.pagemask; break;
    case CP0_WIRED:    *val = s->c.wired; break;
    case CP0_ENTRYHI:  *val = s->c.entryhi; break;
    case CP0_STATUS:   *val = s->c.status; break;
    case CP0_CAUSE:    *val = cause_read(s); break;
    case CP0_EPC:      *val = s->c.epc; break;
    case CP0_ERROREPC: *val = s->c.errorepc; break;
    case CP0_BADVADDR: *val = s->c.badvaddr; break;
    case CP0_COUNT:    *val = mips_count_now(cpu); break;
    case CP0_COMPARE:  *val = s->c.compare; break;
    case CP0_EBASE:    /* bit 31 reads 1; CPUNum (bits 9..0) is the processor number */
        *val = s->c.ebase | (1u << 31) | (cpu->hartid & 0x3FFu);
        break;
    case CP0_INTCTL:   *val = intctl_read(s); break;
    case CP0_SRSCTL:   *val = srsctl_read(s); break;
    case CP0_SRSMAP:   *val = s->c.srsmap; break;
    case CP0_PRID:     *val = s->cfg.prid; break;
    case CP0_CONFIG:   *val = s->c.config; break;
    case CP0_CONFIG1:  *val = config1_value(s); break;
    case CP0_CONFIG2:  *val = s->cfg.config2; break;
    case CP0_CONFIG3:  *val = s->c.config3; break;
    default:           *val = 0; break; /* unimplemented CP0 regs read as zero */
    }
    return UAM_OK;
}

static uam_status mips_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t v64)
{
    mips_state *s = mips_st(cpu);
    uint32_t v = (uint32_t)v64;
    uint32_t n = s->cfg.tlb_entries;
    if (!cp0_usable(cpu))
        return UAM_ERR_ILLEGAL;
    if (!has_tlb(s) && tlb_reg(id))
        return UAM_OK;
    switch (id) {
    case CP0_INDEX: {
        uint32_t m = 1;
        while (m < n)
            m <<= 1;
        s->c.index = (s->c.index & 0x80000000u) | (v & (m - 1)); /* P is read-only */
        break;
    }
    case CP0_ENTRYLO0: s->c.entrylo0 = v & ENTRYLO_MASK; break;
    case CP0_ENTRYLO1: s->c.entrylo1 = v & ENTRYLO_MASK; break;
    case CP0_CONTEXT:  s->c.context = (s->c.context & ~CONTEXT_PTE) | (v & CONTEXT_PTE); break;
    case CP0_PAGEMASK: s->c.pagemask = v & PAGEMASK_MASK; break;
    case CP0_WIRED:    s->c.wired = v % n; break;
    case CP0_ENTRYHI: {
        uint32_t old_asid = s->c.entryhi & ENTRYHI_ASID;
        s->c.entryhi = v & (ENTRYHI_VPN2 | ENTRYHI_ASID);
        if ((v & ENTRYHI_ASID) != old_asid)
            uam_cpu_xlate_changed(cpu); /* the address space switched */
        break;
    }
    case CP0_STATUS:
        s->c.status = (s->c.status & ~s->cfg.status_rw) | (v & s->cfg.status_rw);
        mode_update(cpu);
        uam_cpu_request_exit(cpu); /* IE/IM may have enabled a pending interrupt (EI too) */
        break;
    case CP0_CAUSE:
        /* Only IV and software interrupt bits IP1:0 are writable. */
        s->c.cause = (s->c.cause & ~(CAUSE_IV | (0x3u << CAUSE_IP_SHIFT)))
                   | (v & (CAUSE_IV | (0x3u << CAUSE_IP_SHIFT)));
        uam_signal_set(&s->swint_line[0], (s->c.cause >> CAUSE_IP_SHIFT) & 1);
        uam_signal_set(&s->swint_line[1], (s->c.cause >> (CAUSE_IP_SHIFT + 1)) & 1);
        uam_cpu_request_exit(cpu); /* a software interrupt may now be pending */
        break;
    case CP0_EPC:      s->c.epc = v; break;
    case CP0_ERROREPC: s->c.errorepc = v; break;
    case CP0_COUNT:
        s->c.count_bias = v - (uam_sched_now(cpu->sched) >> 1);
        timer_update(cpu);
        break;
    case CP0_COMPARE:
        s->c.compare = v;
        s->c.timer_ip = false; /* writing Compare clears the timer interrupt */
        uam_signal_set(&s->timer_line, 0);
        timer_update(cpu);
        break;
    case CP0_EBASE:    s->c.ebase = (s->c.ebase & 0xC00FF000u) | (v & 0x3FFFF000u); break;
    case CP0_INTCTL:   s->c.intctl = v & 0x000003E0u; break;
    case CP0_SRSCTL:
        if (s->cfg.shadow_sets) {
            uint32_t ess = (v >> 12) & 0xFu, pss = (v >> 6) & 0xFu;
            if (ess <= s->cfg.shadow_sets && pss <= s->cfg.shadow_sets)
                s->c.srsctl = (s->c.srsctl & 0xFu) | (ess << 12) | (pss << 6);
        }
        break;
    case CP0_SRSMAP:
        if (s->cfg.shadow_sets && (s->cfg.config3 & CFG3_VINT))
            s->c.srsmap = v;
        break;
    case CP0_CONFIG3:
        if (mips_umips(s) >= 2)
            s->c.config3 = (s->c.config3 & ~CFG3_ISAONEXC) | (v & CFG3_ISAONEXC);
        break;
    case CP0_CONFIG:   s->c.config = (s->c.config & ~0x7u) | (v & 0x7u); break; /* K0 */
    default:
        break; /* read-only and unimplemented registers ignore writes */
    }
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* ERET helper                                                               */
/* ------------------------------------------------------------------------ */

static uint64_t helper_eret(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    mips_state *s = mips_st(cpu);
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    uint32_t pc;
    if (s->c.status & ST_ERL) {
        s->c.status &= ~ST_ERL;
        pc = s->c.errorepc;
    } else {
        s->c.status &= ~ST_EXL;
        pc = s->c.epc;
        if (s->cfg.shadow_sets && !(s->c.status & ST_BEV))
            srs_switch(cpu, (s->c.srsctl >> 6) & 0xFu);   /* back to the previous set */
    }
    cpu->resv.valid = false;
    mode_update(cpu);
    return pc;   /* bit 0 is the ISA mode to return to */
}

/* RDPGPR / WRPGPR: a register of the previous shadow set (SRSCtl.PSS). */
static uint64_t helper_rdpgpr(void *env, uint64_t rt, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    const mips_state *s = mips_st(cpu);
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    unsigned pss = (s->c.srsctl >> 6) & 0xFu, r = (unsigned)rt & 31;
    if (!r)
        return 0;
    return pss == (s->c.srsctl & 0xFu) ? uam_cpu_reg_read(cpu, MIPS_REG_OFF(r), 32)
                                       : s->c.srs[pss][r];
}

static uint64_t helper_wrpgpr(void *env, uint64_t rd, uint64_t v, uint64_t c, uam_status *st)
{
    (void)c;
    uam_cpu *cpu = env;
    mips_state *s = mips_st(cpu);
    if (!cp0_usable(cpu)) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    unsigned pss = (s->c.srsctl >> 6) & 0xFu, r = (unsigned)rd & 31;
    if (!r)
        return 0;
    if (pss == (s->c.srsctl & 0xFu))
        uam_cpu_reg_write(cpu, MIPS_REG_OFF(r), 32, (uint32_t)v);
    else
        s->c.srs[pss][r] = (uint32_t)v;
    return 0;
}

#define ALL_EFFECTS (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE | UAM_HELPER_MAY_FAULT)

#define PRIV_HELPER(nm, f) { .name = nm, .n_args = 0, .result_width = 0, .effect = ALL_EFFECTS, .fn = f }

const uam_helper_desc mips_helpers[MIPS_HELPER__COUNT] = {
    [MIPS_HELPER_ERET] = {
        .name = "mips_eret", .n_args = 0, .result_width = 32,
        .effect = ALL_EFFECTS,
        .fn = helper_eret,
    },
    [MIPS_HELPER_TLBR]  = PRIV_HELPER("mips_tlbr", helper_tlbr),
    [MIPS_HELPER_TLBWI] = PRIV_HELPER("mips_tlbwi", helper_tlbwi),
    [MIPS_HELPER_TLBWR] = PRIV_HELPER("mips_tlbwr", helper_tlbwr),
    [MIPS_HELPER_TLBP]  = PRIV_HELPER("mips_tlbp", helper_tlbp),
    [MIPS_HELPER_RDPGPR] = {
        .name = "mips_rdpgpr", .n_args = 1, .arg_width = { 32 }, .result_width = 32,
        .effect = UAM_HELPER_READS_STATE | UAM_HELPER_MAY_FAULT, .fn = helper_rdpgpr,
    },
    [MIPS_HELPER_WRPGPR] = {
        .name = "mips_wrpgpr", .n_args = 2, .arg_width = { 32, 32 }, .result_width = 0,
        .effect = ALL_EFFECTS, .fn = helper_wrpgpr,
    },
    [MIPS_HELPER_IRET] = {   /* without interrupt chaining or auto-prologue, IRET is ERET */
        .name = "mips_iret", .n_args = 0, .result_width = 32,
        .effect = ALL_EFFECTS, .fn = helper_eret,
    },
    [MIPS_HELPER_FPU] = {
        .name = "mips_fpu", .n_args = 1, .arg_width = { 32 }, .result_width = 0,
        .effect = ALL_EFFECTS, .fn = mips_fpu_op,
    },
    [MIPS_HELPER_CFC1] = {
        .name = "mips_cfc1", .n_args = 1, .arg_width = { 32 }, .result_width = 32,
        .effect = UAM_HELPER_READS_STATE, .fn = mips_cfc1,
    },
    [MIPS_HELPER_CTC1] = {
        .name = "mips_ctc1", .n_args = 2, .arg_width = { 32, 32 }, .result_width = 0,
        .effect = ALL_EFFECTS, .fn = mips_ctc1,
    },
    [MIPS_HELPER_DSP] = {
        .name = "mips_dsp", .n_args = 1, .arg_width = { 32 }, .result_width = 0,
        .effect = UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE, .fn = mips_dsp_op,
    },
};

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static uam_status mips_init(uam_cpu *cpu)
{
    mips_state *s = calloc(1, sizeof(mips_state));
    if (!s)
        return UAM_ERR_NOMEM;
    cpu->isa_state = s;
    s->cfg = uam_mips_ref;
    uam_timer_init(&s->timer, timer_fire, cpu);
    uam_signal_init(&s->timer_line);
    uam_signal_init(&s->swint_line[0]);
    uam_signal_init(&s->swint_line[1]);
    return UAM_OK;
}

static void mips_fini(uam_cpu *cpu)
{
    mips_state *s = mips_st(cpu);
    if (cpu->sched)
        uam_timer_cancel(cpu->sched, &s->timer);
    free(s);
    cpu->isa_state = NULL;
}

static void mips_reset(uam_cpu *cpu, uint64_t entry)
{
    mips_state *s = mips_st(cpu);
    /* The timer stays initialised and owned by this hart; it is only taken
     * off the scheduler's list (never re-initialised while linked). */
    if (cpu->sched)
        uam_timer_cancel(cpu->sched, &s->timer);
    memset(&s->c, 0, sizeof(s->c));
    s->endian = (uint8_t)cpu->isa->default_endian;
    /* Reset state: ERL and BEV set, interrupts off (MIPS cold reset). */
    s->c.status = ST_BEV | ST_ERL;
    s->c.ebase = 0x80000000u;
    s->c.config = (s->cfg.config0 & ~(1u << 15)) | ((s->endian & UAM_ACC_BE) ? (1u << 15) : 0);
    s->c.config3 = s->cfg.config3;
    s->c.index = 0;
    s->eic = (mips_eic){ .offset = UAM_MIPS_EIC_NO_OFFSET };
    s->hfault = MIPS_HF_NONE;
    uam_signal_set(&s->timer_line, 0);
    uam_signal_set(&s->swint_line[0], 0);
    uam_signal_set(&s->swint_line[1], 0);
    for (size_t i = 0; i < cpu->regfile_size; i++)
        cpu->regfile[i] = 0;
    s->erl_seen = !(s->c.status & ST_ERL); /* force mode_update to report the ERL state */
    mode_update(cpu);
    /* A processor whose reset ISA is microMIPS (Config3.ISA 1 or 3) starts there. */
    if (mips_umips(s) == 1 || mips_umips(s) == 3)
        entry |= 1;
    cpu->pc = entry;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

static void mips_dump_state(uam_cpu *cpu, FILE *out)
{
    mips_state *s = mips_st(cpu);
    unsigned mode = MIPS_PRIV_MODE(cpu->priv);
    fprintf(out, "pc=%08llx icount=%llu endian=%s mode=%s%s\n", (unsigned long long)cpu->pc,
            (unsigned long long)cpu->icount, (s->endian & UAM_ACC_BE) ? "big" : "little",
            mode == MIPS_KERNEL ? "kernel" : mode == MIPS_USER ? "user" : "supervisor",
            !(cpu->pc & 1) ? "" : mips_has_m16(s) ? " isa=mips16e" : " isa=micromips");
    for (unsigned i = 0; i < 32; i++)
        fprintf(out, "%-3s=%08llx%s", mips_regs[i].name,
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_REG_OFF(i), 32),
                (i % 4 == 3) ? "\n" : "  ");
    fprintf(out, "hi=%08llx lo=%08llx\n",
            (unsigned long long)uam_cpu_reg_read(cpu, MIPS_HI_OFF, 32),
            (unsigned long long)uam_cpu_reg_read(cpu, MIPS_LO_OFF, 32));
    fprintf(out, "status=%08x cause=%08x epc=%08x badvaddr=%08x count=%08x compare=%08x\n",
            s->c.status, cause_read(s), s->c.epc, s->c.badvaddr, mips_count_now(cpu),
            s->c.compare);
    fprintf(out, "entryhi=%08x index=%08x wired=%u\n", s->c.entryhi, s->c.index, s->c.wired);
    if (s->cfg.shadow_sets || mips_has_eic(s))
        fprintf(out, "srsctl=%08x intctl=%08x ripl=%u vector=%u\n", srsctl_read(s),
                intctl_read(s), s->eic.ripl, s->eic.vector);
    if (mips_has_fpu(s)) {
        fprintf(out, "fcsr=%08llx\n", (unsigned long long)uam_cpu_reg_read(cpu, MIPS_FCSR_OFF, 32));
        for (unsigned i = 0; i < 32; i++)
            fprintf(out, "f%-2u=%016llx%s", i,
                    (unsigned long long)uam_cpu_reg_read(cpu, MIPS_FPR_OFF(i), 64),
                    (i % 4 == 3) ? "\n" : "  ");
    }
    if (mips_has_dsp(s))
        fprintf(out, "dspcontrol=%08llx ac1=%08llx:%08llx ac2=%08llx:%08llx ac3=%08llx:%08llx\n",
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_DSPCTL_OFF, 32),
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_ACHI_OFF(1), 32),
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_ACLO_OFF(1), 32),
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_ACHI_OFF(2), 32),
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_ACLO_OFF(2), 32),
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_ACHI_OFF(3), 32),
                (unsigned long long)uam_cpu_reg_read(cpu, MIPS_ACLO_OFF(3), 32));
}

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                 */
/* ------------------------------------------------------------------------ */

static void mips_save(uam_cpu *cpu, uam_state_writer *w)
{
    const mips_state *s = mips_st(cpu);
    const mips_cp0 *c = &s->c;
    uam_state_put_u32(w, s->cfg.prid);
    uam_state_put_u32(w, s->cfg.tlb_entries);
    uam_state_put_u32(w, s->hwlines);
    uam_state_put_u8(w, s->endian);
    uam_state_put_bool(w, s->erl_seen);
    const uint32_t regs[] = { c->status, c->cause, c->epc, c->errorepc, c->badvaddr, c->ebase,
                              c->compare, c->config, c->intctl, c->index, c->entrylo0,
                              c->entrylo1, c->context, c->pagemask, c->wired, c->entryhi };
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        uam_state_put_u32(w, regs[i]);
    uam_state_put_u64(w, c->count_bias);
    uam_state_put_bool(w, c->timer_ip);
    for (unsigned i = 0; i < MIPS_TLB_MAX; i++) {
        const mips_tlb_entry *e = &c->tlb[i];
        uam_state_put_u32(w, e->vpn2);
        uam_state_put_u32(w, e->mask);
        uam_state_put_u8(w, e->asid);
        uam_state_put_bool(w, e->g);
        uam_state_put_u32(w, e->lo[0]);
        uam_state_put_u32(w, e->lo[1]);
    }
    uam_state_put_timer(w, &s->timer);
    uam_state_put_u32(w, c->srsctl);
    uam_state_put_u32(w, c->srsmap);
    uam_state_put_u32(w, c->config3);
    for (unsigned k = 0; k <= s->cfg.shadow_sets; k++)
        for (unsigned i = 0; i < 32; i++)
            uam_state_put_u32(w, c->srs[k][i]);
    uam_state_put_u32(w, s->eic.ripl);
    uam_state_put_u32(w, s->eic.vector);
    uam_state_put_u32(w, s->eic.offset);
    uam_state_put_u32(w, s->eic.ss);
}

static void mips_restore(uam_cpu *cpu, uam_state_reader *r)
{
    mips_state *s = mips_st(cpu);
    mips_cp0 *c = &s->c;
    uam_state_expect_u32(r, s->cfg.prid, "MIPS PRId");
    uam_state_expect_u32(r, s->cfg.tlb_entries, "MIPS TLB size");
    s->hwlines = uam_state_get_u32(r);
    s->endian = uam_state_get_u8(r);
    s->erl_seen = uam_state_get_bool(r);
    uint32_t *const regs[] = { &c->status, &c->cause, &c->epc, &c->errorepc, &c->badvaddr,
                               &c->ebase, &c->compare, &c->config, &c->intctl, &c->index,
                               &c->entrylo0, &c->entrylo1, &c->context, &c->pagemask, &c->wired,
                               &c->entryhi };
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        *regs[i] = uam_state_get_u32(r);
    c->count_bias = uam_state_get_u64(r);
    c->timer_ip = uam_state_get_bool(r);
    for (unsigned i = 0; i < MIPS_TLB_MAX; i++) {
        mips_tlb_entry *e = &c->tlb[i];
        e->vpn2 = uam_state_get_u32(r);
        e->mask = uam_state_get_u32(r);
        e->asid = uam_state_get_u8(r);
        e->g = uam_state_get_bool(r);
        e->lo[0] = uam_state_get_u32(r);
        e->lo[1] = uam_state_get_u32(r);
    }
    uam_state_get_timer(r, &s->timer);
    c->srsctl = uam_state_get_u32(r);
    c->srsmap = uam_state_get_u32(r);
    c->config3 = uam_state_get_u32(r);
    for (unsigned k = 0; k <= s->cfg.shadow_sets; k++)
        for (unsigned i = 0; i < 32; i++)
            c->srs[k][i] = uam_state_get_u32(r);
    s->eic.ripl = uam_state_get_u32(r);
    s->eic.vector = uam_state_get_u32(r);
    s->eic.offset = uam_state_get_u32(r);
    s->eic.ss = uam_state_get_u32(r);
    /* The outputs' levels follow the state; their sinks restore their own. */
    s->timer_line.level = c->timer_ip;
    s->swint_line[0].level = (c->cause >> CAUSE_IP_SHIFT) & 1;
    s->swint_line[1].level = (c->cause >> (CAUSE_IP_SHIFT + 1)) & 1;
}

/* ------------------------------------------------------------------------ */
/* Debugger view (gdb: mips:isa32r2 register numbering)                      */
/* ------------------------------------------------------------------------ */

/* gdb's MIPS layout: r0-r31, sr, lo, hi, bad, cause, pc, f0-f31, fsr, fir.
 * gdb requires the FPU feature; with no FPU modelled those read as zero. */
#define GDB_MIPS_NREGS 72u

#define GDB_REG(n, num, grp) "<reg name=\"" n "\" bitsize=\"32\" regnum=\"" #num "\"" grp "/>"
static const char mips_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>mips:isa32r2</architecture>"
    "<feature name=\"org.gnu.gdb.mips.cpu\">"
    GDB_REG("r0", 0, "") GDB_REG("r1", 1, "") GDB_REG("r2", 2, "") GDB_REG("r3", 3, "")
    GDB_REG("r4", 4, "") GDB_REG("r5", 5, "") GDB_REG("r6", 6, "") GDB_REG("r7", 7, "")
    GDB_REG("r8", 8, "") GDB_REG("r9", 9, "") GDB_REG("r10", 10, "") GDB_REG("r11", 11, "")
    GDB_REG("r12", 12, "") GDB_REG("r13", 13, "") GDB_REG("r14", 14, "") GDB_REG("r15", 15, "")
    GDB_REG("r16", 16, "") GDB_REG("r17", 17, "") GDB_REG("r18", 18, "") GDB_REG("r19", 19, "")
    GDB_REG("r20", 20, "") GDB_REG("r21", 21, "") GDB_REG("r22", 22, "") GDB_REG("r23", 23, "")
    GDB_REG("r24", 24, "") GDB_REG("r25", 25, "") GDB_REG("r26", 26, "") GDB_REG("r27", 27, "")
    GDB_REG("r28", 28, "") GDB_REG("r29", 29, "") GDB_REG("r30", 30, "") GDB_REG("r31", 31, "")
    GDB_REG("lo", 33, "") GDB_REG("hi", 34, "") GDB_REG("pc", 37, "")
    "</feature><feature name=\"org.gnu.gdb.mips.cp0\">"
    GDB_REG("status", 32, "") GDB_REG("badvaddr", 35, "") GDB_REG("cause", 36, "")
    "</feature><feature name=\"org.gnu.gdb.mips.fpu\">"
    GDB_REG("f0", 38, " type=\"ieee_single\"") GDB_REG("f1", 39, " type=\"ieee_single\"")
    GDB_REG("f2", 40, " type=\"ieee_single\"") GDB_REG("f3", 41, " type=\"ieee_single\"")
    GDB_REG("f4", 42, " type=\"ieee_single\"") GDB_REG("f5", 43, " type=\"ieee_single\"")
    GDB_REG("f6", 44, " type=\"ieee_single\"") GDB_REG("f7", 45, " type=\"ieee_single\"")
    GDB_REG("f8", 46, " type=\"ieee_single\"") GDB_REG("f9", 47, " type=\"ieee_single\"")
    GDB_REG("f10", 48, " type=\"ieee_single\"") GDB_REG("f11", 49, " type=\"ieee_single\"")
    GDB_REG("f12", 50, " type=\"ieee_single\"") GDB_REG("f13", 51, " type=\"ieee_single\"")
    GDB_REG("f14", 52, " type=\"ieee_single\"") GDB_REG("f15", 53, " type=\"ieee_single\"")
    GDB_REG("f16", 54, " type=\"ieee_single\"") GDB_REG("f17", 55, " type=\"ieee_single\"")
    GDB_REG("f18", 56, " type=\"ieee_single\"") GDB_REG("f19", 57, " type=\"ieee_single\"")
    GDB_REG("f20", 58, " type=\"ieee_single\"") GDB_REG("f21", 59, " type=\"ieee_single\"")
    GDB_REG("f22", 60, " type=\"ieee_single\"") GDB_REG("f23", 61, " type=\"ieee_single\"")
    GDB_REG("f24", 62, " type=\"ieee_single\"") GDB_REG("f25", 63, " type=\"ieee_single\"")
    GDB_REG("f26", 64, " type=\"ieee_single\"") GDB_REG("f27", 65, " type=\"ieee_single\"")
    GDB_REG("f28", 66, " type=\"ieee_single\"") GDB_REG("f29", 67, " type=\"ieee_single\"")
    GDB_REG("f30", 68, " type=\"ieee_single\"") GDB_REG("f31", 69, " type=\"ieee_single\"")
    GDB_REG("fcsr", 70, " group=\"float\"") GDB_REG("fir", 71, " group=\"float\"")
    "</feature></target>";
#undef GDB_REG

static bool mips_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    mips_state *s = mips_st(cpu);
    if (n < 32)       *v = uam_cpu_reg_read(cpu, MIPS_REG_OFF(n), 32);
    else if (n == 32) *v = s->c.status;
    else if (n == 33) *v = uam_cpu_reg_read(cpu, MIPS_LO_OFF, 32);
    else if (n == 34) *v = uam_cpu_reg_read(cpu, MIPS_HI_OFF, 32);
    else if (n == 35) *v = s->c.badvaddr;
    else if (n == 36) *v = cause_read(s);
    else if (n == 37) *v = (uint32_t)cpu->pc;
    else if (n < GDB_MIPS_NREGS && !mips_has_fpu(s)) *v = 0;   /* no FPU */
    else if (n < 70) {
        unsigned f = n - 38;
        *v = (cpu->priv & MIPS_PRIV_FR) ? uam_cpu_reg_read(cpu, MIPS_FPR_OFF(f), 32)
                                        : uam_cpu_reg_read(cpu, MIPS_FPR_OFF(f & ~1u) + 4 * (f & 1), 32);
    }
    else if (n == 70) *v = uam_cpu_reg_read(cpu, MIPS_FCSR_OFF, 32);
    else if (n == 71) *v = s->cfg.fir;
    else return false;
    return true;
}

static bool mips_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    mips_state *s = mips_st(cpu);
    if (n >= 38 && n < 70 && mips_has_fpu(s)) {
        unsigned f = n - 38;
        uam_cpu_reg_write(cpu, (cpu->priv & MIPS_PRIV_FR) ? MIPS_FPR_OFF(f)
                                                          : MIPS_FPR_OFF(f & ~1u) + 4 * (f & 1), 32, v);
        return true;
    }
    if (n == 70 && mips_has_fpu(s)) {
        uam_cpu_reg_write(cpu, MIPS_FCSR_OFF, 32, v);
        return true;
    }
    if (n == 0 || (n >= 38 && n < GDB_MIPS_NREGS) || n == 35 || n == 36)
        return true;                         /* read-only / not modelled: ignored */
    if (n < 32)       uam_cpu_reg_write(cpu, MIPS_REG_OFF(n), 32, v);
    else if (n == 33) uam_cpu_reg_write(cpu, MIPS_LO_OFF, 32, v);
    else if (n == 34) uam_cpu_reg_write(cpu, MIPS_HI_OFF, 32, v);
    else if (n == 37) cpu->pc = (uint32_t)v;
    else if (n == 32) {
        s->c.status = (uint32_t)v;
        mode_update(cpu);
    } else
        return false;
    return true;
}

static const uam_gdb_arch mips_gdb = {
    .target_xml = mips_target_xml,
    .n_regs = GDB_MIPS_NREGS,
    .reg_bytes = 4,
    .pc_state_bits = 1,   /* the ISA mode (MIPS16e / microMIPS) */
    .read_reg = mips_gdb_read,
    .write_reg = mips_gdb_write,
};

/* ------------------------------------------------------------------------ */
/* Descriptors                                                               */
/* ------------------------------------------------------------------------ */

#define MIPS_COMMON                                   \
    .addr_width     = 32,                             \
    .regfile_size   = MIPS_REGFILE_SIZE,              \
    .regs           = mips_regs,                      \
    .n_regs         = UAM_ARRAY_LEN(mips_regs),       \
    .helpers        = { mips_helpers, MIPS_HELPER__COUNT }, \
    .init           = mips_init,                      \
    .fini           = mips_fini,                      \
    .reset          = mips_reset,                     \
    .lift           = mips_lift,                      \
    .take_exception = mips_take_exception,            \
    .irq_take       = mips_irq_take,                  \
    .irq_wakeup     = mips_irq_wakeup,                \
    .irq_input      = mips_irq_input,                 \
    .sysreg_read    = mips_sysreg_read,               \
    .sysreg_write   = mips_sysreg_write,              \
    .translate      = mips_translate,                 \
    .gdb            = &mips_gdb,                      \
    .dump_state     = mips_dump_state,                \
    .save           = mips_save,                      \
    .restore        = mips_restore,                   \
    .irq_lines      = mips_irq_lines

const uam_isa uam_isa_mips32be = {
    .name = "mips32be", .default_endian = UAM_ACC_BE, MIPS_COMMON,
};
const uam_isa uam_isa_mips32le = {
    .name = "mips32le", .default_endian = UAM_ACC_LE, MIPS_COMMON,
};
