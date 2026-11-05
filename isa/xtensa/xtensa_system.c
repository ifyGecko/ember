/*
 * xtensa_system.c - The Xtensa system architecture (XEA2): CPU models, special
 * registers, exceptions and interrupts, register windows, the CCOUNT/CCOMPARE timers,
 * Region Protection, the debugger view, snapshots and the two descriptors.
 *
 * The CPU models are the cores of the Espressif chips, from the configuration headers
 * Tensilica generated for them (core-isa.h, core-matmap.h, tie.h, as published in
 * Espressif's SDKs): the interrupt tables, levels, vectors and option sets below are
 * those headers' values.
 */
#include <stdlib.h>
#include <string.h>

#include "xtensa.h"
#include "xtensa_opc.h"

#define L1(t) { UAM_XT_INT_##t, 1 }
#define IRQ(t, l) { UAM_XT_INT_##t, l }

/* Region attributes: rights without caches (the cores have none). */
#define CA_RIGHTS_BASE { [0] = UAM_XT_CA_R | UAM_XT_CA_W, [1] = 7, [2] = 7, [3] = UAM_XT_CA_X, \
                         [4] = 7, [5] = 7, [14] = UAM_XT_CA_R | UAM_XT_CA_W }

const uam_xtensa_config uam_xtensa_lx106 = {
    .name = "lx106",
    .options = UAM_XT_OPT_DENSITY | UAM_XT_OPT_MUL16 | UAM_XT_OPT_MUL32 | UAM_XT_OPT_NSA
               | UAM_XT_OPT_LITBASE | UAM_XT_OPT_UNALIGNED_EXC | UAM_XT_OPT_REGION_PROT
               | UAM_XT_OPT_DEBUG | UAM_XT_OPT_PRID | UAM_XT_OPT_EXTERN_REGS,
    .nareg = 16,
    .ninterrupt = 15,
    .irq = { L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(TIMER),
             L1(SOFTWARE), L1(EDGE), L1(EDGE), L1(EDGE), L1(EDGE), L1(EDGE), L1(EDGE),
             IRQ(NMI, 3) },
    .nlevel = 2, .excm_level = 1, .debug_level = 2, .nmi = true,
    .nccompare = 1, .timer_int = { 6 },
    .nibreak = 1, .ndbreak = 1,
    .reset_vector = 0x40000080u,   /* the second static vector, which the ESP8266 selects */
    .vecbase = 0x40000000u,
    .vec = { [UAM_XT_VEC_LEVEL2] = 0x10, [UAM_XT_VEC_LEVEL3] = 0x20, [UAM_XT_VEC_KERNEL] = 0x30,
             [UAM_XT_VEC_USER] = 0x50, [UAM_XT_VEC_DOUBLE] = 0x70 },
    .ca_rights = CA_RIGHTS_BASE,   /* 6 is reserved on this core */
};

#define ESP_IRQS { L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(LEVEL), L1(LEVEL), \
    L1(TIMER), L1(SOFTWARE), L1(LEVEL), L1(LEVEL), L1(EDGE), IRQ(PROFILING, 3), L1(LEVEL), \
    L1(LEVEL), IRQ(NMI, 7), IRQ(TIMER, 3), IRQ(TIMER, 5), L1(LEVEL), L1(LEVEL), \
    IRQ(LEVEL, 2), IRQ(LEVEL, 2), IRQ(LEVEL, 2), IRQ(EDGE, 3), IRQ(LEVEL, 3), \
    IRQ(LEVEL, 4), IRQ(LEVEL, 4), IRQ(LEVEL, 5), IRQ(LEVEL, 3), IRQ(EDGE, 4), \
    IRQ(SOFTWARE, 3), IRQ(EDGE, 4), IRQ(LEVEL, 5) }

#define ESP_VECS { [UAM_XT_VEC_WO4] = 0x000, [UAM_XT_VEC_WU4] = 0x040, [UAM_XT_VEC_WO8] = 0x080, \
    [UAM_XT_VEC_WU8] = 0x0C0, [UAM_XT_VEC_WO12] = 0x100, [UAM_XT_VEC_WU12] = 0x140, \
    [UAM_XT_VEC_LEVEL2] = 0x180, [UAM_XT_VEC_LEVEL3] = 0x1C0, [UAM_XT_VEC_LEVEL4] = 0x200, \
    [UAM_XT_VEC_LEVEL5] = 0x240, [UAM_XT_VEC_LEVEL6] = 0x280, [UAM_XT_VEC_LEVEL7] = 0x2C0, \
    [UAM_XT_VEC_KERNEL] = 0x300, [UAM_XT_VEC_USER] = 0x340, [UAM_XT_VEC_DOUBLE] = 0x3C0 }

#define ESP_OPTIONS (UAM_XT_OPT_DENSITY | UAM_XT_OPT_WINDOWED | UAM_XT_OPT_LOOPS \
    | UAM_XT_OPT_BOOLEANS | UAM_XT_OPT_MUL16 | UAM_XT_OPT_MUL32 | UAM_XT_OPT_MUL32_HIGH \
    | UAM_XT_OPT_DIV32 | UAM_XT_OPT_NSA | UAM_XT_OPT_MINMAX | UAM_XT_OPT_SEXT \
    | UAM_XT_OPT_CLAMPS | UAM_XT_OPT_MAC16 | UAM_XT_OPT_FP | UAM_XT_OPT_FP_EXT \
    | UAM_XT_OPT_S32C1I | UAM_XT_OPT_SYNC | UAM_XT_OPT_THREADPTR | UAM_XT_OPT_REGION_PROT \
    | UAM_XT_OPT_COPROC | UAM_XT_OPT_DEBUG | UAM_XT_OPT_PRID | UAM_XT_OPT_EXTERN_REGS \
    | UAM_XT_OPT_S32NB | UAM_XT_OPT_MEMCTL)

const uam_xtensa_config uam_xtensa_lx6 = {
    .name = "lx6",
    .options = ESP_OPTIONS,
    .nareg = 64,
    .ninterrupt = 32,
    .irq = ESP_IRQS,
    .nlevel = 6, .excm_level = 3, .debug_level = 6, .nmi = true,
    .nccompare = 3, .timer_int = { 6, 15, 16 },
    .nibreak = 2, .ndbreak = 2, .nmisc = 4,
    .cp_mask = 0x01,
    .reset_vector = 0x40000400u,
    .vecbase = 0x40000000u,
    .vec = ESP_VECS,
    .ca_rights = { [0] = UAM_XT_CA_R | UAM_XT_CA_W, [1] = 7, [2] = 7, [3] = UAM_XT_CA_X,
                   [4] = 7, [5] = 7, [6] = 7, [14] = UAM_XT_CA_R | UAM_XT_CA_W },
    .prid = 0xCDCD,
    .configid0 = 0xC2BCFFFEu, .configid1 = 0x1CC5FE96u,
};

const uam_xtensa_config uam_xtensa_lx7 = {
    .name = "lx7",
    .options = ESP_OPTIONS | UAM_XT_OPT_SALT | UAM_XT_OPT_LONG_INSN,
    .nareg = 64,
    .ninterrupt = 32,
    .irq = ESP_IRQS,
    .nlevel = 6, .excm_level = 3, .debug_level = 6, .nmi = true,
    .nccompare = 3, .timer_int = { 6, 15, 16 },
    .nibreak = 2, .ndbreak = 2, .nmisc = 4,
    .cp_mask = 0x09,   /* the FPU and the AI extension (CP3) */
    .reset_vector = 0x40000400u,
    .vecbase = 0x40000000u,
    .vec = ESP_VECS,
    .ca_rights = { [0] = UAM_XT_CA_R | UAM_XT_CA_W, [1] = 7, [2] = 7, [3] = UAM_XT_CA_X,
                   [4] = 7, [5] = 7, [6] = 7 },
    .prid = 0xCDCD,
    .configid0 = 0xC2F0FFFEu, .configid1 = 0x23090F1Fu,
};

/* ------------------------------------------------------------------------ */
/* Model selection                                                           */
/* ------------------------------------------------------------------------ */

static bool cfg_valid(const uam_xtensa_config *c)
{
    bool win = c->options & UAM_XT_OPT_WINDOWED;
    if (win ? (c->nareg != 32 && c->nareg != 64) : c->nareg != 16)
        return false;
    if (c->ninterrupt > 32 || c->nlevel < 1 || c->nlevel > 6 || c->nccompare > 3
        || c->nibreak > 2 || c->ndbreak > 2 || c->nmisc > 4)
        return false;
    if (c->ninterrupt && (c->excm_level < 1 || c->excm_level > c->nlevel))
        return false;
    if ((c->options & UAM_XT_OPT_DEBUG) && (c->debug_level < 2 || c->debug_level > c->nlevel))
        return false;
    for (unsigned i = 0; i < c->ninterrupt; i++) {
        unsigned lv = c->irq[i].level;
        if (c->irq[i].type == UAM_XT_INT_NONE)
            continue;
        if (c->irq[i].type == UAM_XT_INT_NMI ? (!c->nmi || lv != c->nlevel + 1) : (lv < 1 || lv > c->nlevel))
            return false;
    }
    for (unsigned i = 0; i < c->nccompare; i++)
        if (c->timer_int[i] >= c->ninterrupt || c->irq[c->timer_int[i]].type != UAM_XT_INT_TIMER)
            return false;
    return true;
}

uam_status uam_xtensa_configure(uam_cpu *cpu, const uam_xtensa_config *cfg)
{
    if (!uam_xtensa_is(cpu->isa) || !cfg || !cfg_valid(cfg))
        return UAM_ERR_INVALID;
    xt_st(cpu)->cfg = *cfg;
    xt_st(cpu)->prid = cfg->prid;
    return UAM_OK;
}

const uam_xtensa_config *uam_xtensa_config_of(uam_cpu *cpu)
{
    return uam_xtensa_is(cpu->isa) ? &xt_st(cpu)->cfg : NULL;
}

void uam_xtensa_set_prid(uam_cpu *cpu, uint32_t prid)
{
    if (uam_xtensa_is(cpu->isa))
        xt_st(cpu)->prid = prid;
}

void uam_xtensa_set_eri(uam_cpu *cpu, uam_xtensa_eri_read_fn rd, uam_xtensa_eri_write_fn wr,
                        void *ctx)
{
    if (!uam_xtensa_is(cpu->isa))
        return;
    xt_state *s = xt_st(cpu);
    s->eri_rd = rd;
    s->eri_wr = wr;
    s->eri_ctx = ctx;
}

/* ------------------------------------------------------------------------ */
/* Register windows and the state lifted code reads                          */
/* ------------------------------------------------------------------------ */

static unsigned nwin(const xt_state *s) { return s->cfg.nareg / 4; }   /* WindowStart bits */

static bool ws_bit(const xt_state *s, uint32_t frame) { return (s->ws >> (frame % nwin(s))) & 1; }

static uint32_t cintlevel(const xt_state *s)
{
    uint32_t il = s->ps & PS_INTLEVEL;
    return (s->ps & PS_EXCM) && s->cfg.excm_level > il ? s->cfg.excm_level : il;
}

void xt_update_derived(uam_cpu *cpu)
{
    xt_state *s = xt_st(cpu);
    bool excm = s->ps & PS_EXCM;
    uint32_t wc = 0;
    if ((s->cfg.options & UAM_XT_OPT_WINDOWED) && (s->ps & PS_WOE) && !excm)
        for (unsigned i = 1; i <= 3; i++)
            if (ws_bit(s, s->wb + i))
                wc |= 1u << i;
    xt_set_reg(cpu, XT_WCHECK, wc);
    xt_set_reg(cpu, XT_LOOPEN, excm ? 0 : 1);
    bool loop = (s->cfg.options & UAM_XT_OPT_LOOPS) && !excm && xt_reg(cpu, XT_LCOUNT) != 0;
    xt_set_reg(cpu, XT_LENDA, loop ? xt_reg(cpu, XT_LEND) : 0xFFFFFFFFu);
}

/* Put the current window back into the physical file and load the one at new_wb. */
void xt_window_rotate(uam_cpu *cpu, uint32_t new_wb)
{
    xt_state *s = xt_st(cpu);
    if (!(s->cfg.options & UAM_XT_OPT_WINDOWED))
        return;
    unsigned n = s->cfg.nareg;
    for (unsigned i = 0; i < 16; i++)
        xt_set_reg(cpu, XT_AR((s->wb * 4 + i) % n), xt_reg(cpu, XT_A(i)));
    s->wb = new_wb % nwin(s);
    for (unsigned i = 0; i < 16; i++)
        xt_set_reg(cpu, XT_A(i), xt_reg(cpu, XT_AR((s->wb * 4 + i) % n)));
    xt_update_derived(cpu);
}

static uint32_t read_ps(uam_cpu *cpu)
{
    return (xt_st(cpu)->ps & ~PS_CALLINC) | (xt_reg(cpu, XT_CALLINC) & 3) << PS_CALLINC_SH;
}

static void write_ps(uam_cpu *cpu, uint32_t v)
{
    xt_state *s = xt_st(cpu);
    v &= PS_MASK;
    if (!(s->cfg.options & UAM_XT_OPT_WINDOWED))
        v &= ~(PS_OWB | PS_CALLINC | PS_WOE);
    if (!s->cfg.ninterrupt)
        v &= ~PS_INTLEVEL;
    s->ps = v & ~PS_CALLINC;
    xt_set_reg(cpu, XT_CALLINC, (v & PS_CALLINC) >> PS_CALLINC_SH);
    xt_update_derived(cpu);
    uam_cpu_request_exit(cpu);   /* the interrupt level, EXCM or a loop-back may change */
}

/* ------------------------------------------------------------------------ */
/* CCOUNT / CCOMPARE                                                         */
/* ------------------------------------------------------------------------ */

static uint32_t ccount(uam_cpu *cpu)
{
    return (uint32_t)uam_sched_now(cpu->sched) + xt_st(cpu)->ccount_bias;
}

static void timer_arm(uam_cpu *cpu, unsigned n)
{
    xt_state *s = xt_st(cpu);
    if (!cpu->sched)
        return;
    uint32_t delta = s->ccompare[n] - ccount(cpu);
    uint64_t when = uam_sched_now(cpu->sched) + (delta ? delta : UINT64_C(1) << 32);
    uam_timer_arm(cpu->sched, &s->timer[n], when);
}

static void timer_fire(void *ctx)
{
    struct xt_timer_ctx *t = ctx;
    xt_state *s = xt_st(t->cpu);
    s->intr |= 1u << s->cfg.timer_int[t->n];
    uam_cpu_request_exit(t->cpu);
    timer_arm(t->cpu, t->n);
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                                */
/* ------------------------------------------------------------------------ */

static uint32_t type_mask(const xt_state *s, unsigned type)
{
    uint32_t m = 0;
    for (unsigned i = 0; i < s->cfg.ninterrupt; i++)
        if (s->cfg.irq[i].type == type)
            m |= 1u << i;
    return m;
}

static uint32_t level_mask(const xt_state *s, unsigned level)
{
    uint32_t m = 0;
    for (unsigned i = 0; i < s->cfg.ninterrupt; i++)
        if (s->cfg.irq[i].level == level && s->cfg.irq[i].type != UAM_XT_INT_NMI)
            m |= 1u << i;
    return m;
}

/* INTERRUPT: level lines as they are, plus the latched requests. */
static uint32_t interrupt_reg(const xt_state *s)
{
    uint32_t lv = type_mask(s, UAM_XT_INT_LEVEL) | type_mask(s, UAM_XT_INT_PROFILING);
    return (s->lines & lv) | s->intr;
}

static uint32_t vector(const xt_state *s, unsigned v) { return s->vecbase + s->cfg.vec[v]; }

/* The highest level that may be taken now (0: none); NMI is nlevel + 1. */
static unsigned deliverable(const xt_state *s)
{
    if (s->nmi_pending)
        return s->cfg.nlevel + 1;
    uint32_t pend = interrupt_reg(s) & s->intenable;
    if (!pend)
        return 0;
    uint32_t cl = cintlevel(s);
    for (unsigned lv = s->cfg.nlevel; lv >= 1; lv--)
        if (cl < lv && (pend & level_mask(s, lv)))
            return lv;
    return 0;
}

static void general_exception(uam_cpu *cpu, uint64_t pc, uint32_t cause);

static bool xt_irq_take(uam_cpu *cpu)
{
    xt_state *s = xt_st(cpu);
    unsigned lv = deliverable(s);
    if (!lv)
        return false;
    s->taken_lines = lv == s->cfg.nlevel + 1 ? type_mask(s, UAM_XT_INT_NMI)
                                             : interrupt_reg(s) & s->intenable & level_mask(s, lv);
    cpu->resv.valid = false;
    if (lv == 1) {
        general_exception(cpu, cpu->pc, XT_EXC_LEVEL1);
        return true;
    }
    if (lv == s->cfg.nlevel + 1)
        s->nmi_pending = false;
    s->epc[lv] = (uint32_t)cpu->pc;
    s->eps[lv] = read_ps(cpu);
    cpu->pc = vector(s, UAM_XT_VEC_LEVEL2 + lv - 2);
    s->ps = (s->ps & ~PS_INTLEVEL) | lv | PS_EXCM;
    xt_update_derived(cpu);
    return true;
}

static bool xt_irq_wakeup(uam_cpu *cpu) { return deliverable(xt_st(cpu)) != 0; }

static void xt_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    xt_state *s = xt_st(cpu);
    if (line >= s->cfg.ninterrupt)
        return;
    uint32_t bit = 1u << line;
    bool was = s->lines & bit;
    s->lines = level ? s->lines | bit : s->lines & ~bit;
    switch (s->cfg.irq[line].type) {
    case UAM_XT_INT_EDGE: case UAM_XT_INT_WRITE_ERR:
        if (level && !was)
            s->intr |= bit;
        break;
    case UAM_XT_INT_NMI:
        if (level && !was)
            s->nmi_pending = true;
        break;
    default:
        break;
    }
    uam_cpu_request_exit(cpu);
}

static uint64_t xt_irq_lines(uam_cpu *cpu) { return xt_st(cpu)->taken_lines; }

/* ------------------------------------------------------------------------ */
/* Exceptions                                                                */
/* ------------------------------------------------------------------------ */

/* A general exception (ISA manual 4.4.1.10): the user, kernel or double vector. */
static void general_exception(uam_cpu *cpu, uint64_t pc, uint32_t cause)
{
    xt_state *s = xt_st(cpu);
    uint32_t target;
    if (s->ps & PS_EXCM) {
        s->depc = (uint32_t)pc;
        target = vector(s, UAM_XT_VEC_DOUBLE);
    } else {
        s->epc[1] = (uint32_t)pc;
        target = vector(s, (s->ps & PS_UM) ? UAM_XT_VEC_USER : UAM_XT_VEC_KERNEL);
    }
    s->exccause = cause;
    s->ps |= PS_EXCM;
    cpu->pc = target;
    cpu->resv.valid = false;
    xt_update_derived(cpu);
}

/* Window overflow: spill the nearest live frame (ISA manual 4.7.1.3). */
static void window_overflow(uam_cpu *cpu, uint64_t pc)
{
    xt_state *s = xt_st(cpu);
    unsigned n = ws_bit(s, s->wb + 1) ? 1 : ws_bit(s, s->wb + 2) ? 2 : 3;
    uint32_t m = s->wb + n;
    s->ps = (s->ps & ~PS_OWB) | (s->wb << PS_OWB_SH) | PS_EXCM;
    s->epc[1] = (uint32_t)pc;
    unsigned v = ws_bit(s, m + 1) ? UAM_XT_VEC_WO4 : ws_bit(s, m + 2) ? UAM_XT_VEC_WO8 : UAM_XT_VEC_WO12;
    cpu->pc = vector(s, v);
    xt_window_rotate(cpu, m);
}

/* Window underflow at RETW (ISA manual 4.7.1.4): WindowBase already moves to the caller. */
static void window_underflow(uam_cpu *cpu, uint64_t pc, unsigned n)
{
    xt_state *s = xt_st(cpu);
    uint32_t owb = s->wb;
    s->ps = (s->ps & ~PS_OWB) | (owb << PS_OWB_SH) | PS_EXCM;
    s->epc[1] = (uint32_t)pc;
    cpu->pc = vector(s, n == 1 ? UAM_XT_VEC_WU4 : n == 2 ? UAM_XT_VEC_WU8 : UAM_XT_VEC_WU12);
    xt_window_rotate(cpu, s->wb + nwin(s) - n);
}

/* BREAK and BREAK.N: a debug exception, or nothing at or above DEBUGLEVEL. */
static void debug_break(uam_cpu *cpu, uint64_t pc, uint32_t tval)
{
    xt_state *s = xt_st(cpu);
    unsigned dl = s->cfg.debug_level;
    if (cintlevel(s) >= dl) {
        cpu->pc = (pc + (tval >> 8)) & 0xFFFFFFFFu;
        return;
    }
    s->epc[dl] = (uint32_t)pc;
    s->eps[dl] = read_ps(cpu);
    s->debugcause = tval & 0x3F;
    cpu->pc = vector(s, UAM_XT_VEC_LEVEL2 + dl - 2);
    s->ps = (s->ps & ~PS_INTLEVEL) | dl | PS_EXCM;
    xt_update_derived(cpu);
}

static void raise(uam_cpu *cpu, uint64_t pc, uint32_t cause, bool va, uint32_t vaddr)
{
    xt_state *s = xt_st(cpu);
    if (cause == XT_TRAP_WOVF) {
        window_overflow(cpu, pc);
        return;
    }
    if (cause == XT_TRAP_WUNF) {
        window_underflow(cpu, pc, s->pend_n);
        return;
    }
    if (va)
        s->excvaddr = vaddr;
    general_exception(cpu, pc, cause);
}

static void xt_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    xt_state *s = xt_st(cpu);
    uint32_t va = (uint32_t)e->tval;
    switch (e->kind) {
    case UAM_EXC_TRAP:
        if (e->cause == XT_TRAP_BREAK)
            debug_break(cpu, e->pc, va);
        else
            raise(cpu, e->pc, (uint32_t)e->cause, false, 0);
        return;
    case UAM_EXC_FETCH_FAULT:      raise(cpu, e->pc, XT_EXC_IFETCH_ERR, true, va); return;
    case UAM_EXC_LOAD_FAULT:
    case UAM_EXC_STORE_FAULT:      raise(cpu, e->pc, XT_EXC_LS_ERR, true, va); return;
    case UAM_EXC_LOAD_MISALIGNED:
    case UAM_EXC_STORE_MISALIGNED: raise(cpu, e->pc, XT_EXC_ALIGN, true, va); return;
    case UAM_EXC_FETCH_XLATE:      raise(cpu, e->pc, XT_EXC_IFETCH_PROHIB, true, va); return;
    case UAM_EXC_LOAD_XLATE:       raise(cpu, e->pc, XT_EXC_LOAD_PROHIB, true, va); return;
    case UAM_EXC_STORE_XLATE:      raise(cpu, e->pc, XT_EXC_STORE_PROHIB, true, va); return;
    case UAM_EXC_SYSREG:
    case UAM_EXC_HELPER: {
        uint32_t c = s->pend_cause, v = s->pend_vaddr;
        s->pend_cause = XT_EXC_ILLEGAL;
        s->pend_vaddr = 0xFFFFFFFFu;
        raise(cpu, e->pc, c, v != 0xFFFFFFFFu, v);
        return;
    }
    }
}

/* ------------------------------------------------------------------------ */
/* Special registers                                                         */
/* ------------------------------------------------------------------------ */

static unsigned max_level(const uam_xtensa_config *c) { return c->nlevel + (c->nmi ? 1u : 0u); }

bool xt_sr_ok(const uam_xtensa_config *c, unsigned sr, bool write, bool xsr)
{
    uint32_t o = c->options;
    bool irq = c->ninterrupt > 0;
    switch (sr) {
    case 0: case 1: case 2: return o & UAM_XT_OPT_LOOPS;
    case 3: return true;
    case 4: return o & UAM_XT_OPT_BOOLEANS;
    case 5: return o & UAM_XT_OPT_LITBASE;
    case 12: return o & UAM_XT_OPT_S32C1I;
    case 16: case 17: case 32: case 33: case 34: case 35: return o & UAM_XT_OPT_MAC16;
    case 72: case 73: return o & UAM_XT_OPT_WINDOWED;
    case 96: return (o & UAM_XT_OPT_DEBUG) && c->nibreak;
    case 97: return o & UAM_XT_OPT_MEMCTL;
    case 99: return o & UAM_XT_OPT_S32C1I;
    case 104: return o & UAM_XT_OPT_DEBUG;
    case 128: case 129: return (o & UAM_XT_OPT_DEBUG) && sr - 128 < c->nibreak;
    case 144: case 145: case 160: case 161:
        return (o & UAM_XT_OPT_DEBUG) && (sr & 1) < c->ndbreak;
    case 176: case 208: return c->configid0 && !write;
    case 177: case 192: case 209: case 232: case 238: return true;
    case 224: return o & UAM_XT_OPT_COPROC;
    case 226: return irq && !xsr;                 /* INTERRUPT / INTSET */
    case 227: return irq && write && !xsr;        /* INTCLEAR */
    case 228: return irq;
    case 230: case 231: return true;              /* PS, VECBASE */
    case 233: return (o & UAM_XT_OPT_DEBUG) && !write;
    case 234: return true;                        /* CCOUNT */
    case 235: return (o & UAM_XT_OPT_PRID) && !write;
    case 236: case 237: return o & UAM_XT_OPT_DEBUG;
    case 240: case 241: case 242: return sr - 240 < c->nccompare;
    case 244: case 245: case 246: case 247: return sr - 244 < c->nmisc;
    default:
        if (sr >= 178 && sr <= 183)        /* EPC2-7 */
            return sr - 176 <= max_level(c);
        if (sr >= 194 && sr <= 199)        /* EPS2-7 */
            return sr - 192 <= max_level(c);
        if (sr >= 210 && sr <= 215)        /* EXCSAVE2-7 */
            return sr - 208 <= max_level(c);
        return false;
    }
}

static uam_status xt_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    xt_state *s = xt_st(cpu);
    uint32_t v = 0;
    switch (id) {
    case 72: v = s->wb; break;
    case 73: v = s->ws; break;
    case 96: v = s->ibreakenable; break;
    case 97: v = s->memctl; break;
    case 99: v = s->atomctl; break;
    case 104: v = s->ddr; break;
    case 128: case 129: v = s->ibreaka[id - 128]; break;
    case 144: case 145: v = s->dbreaka[id - 144]; break;
    case 160: case 161: v = s->dbreakc[id - 160]; break;
    case 176: v = s->cfg.configid0; break;
    case 208: v = s->cfg.configid1; break;
    case 192: v = s->depc; break;
    case 224: v = xt_reg(cpu, XT_CPENABLE); break;
    case 226: v = interrupt_reg(s); break;
    case 228: v = s->intenable; break;
    case 230: v = read_ps(cpu); break;
    case 231: v = s->vecbase; break;
    case 232: v = s->exccause; break;
    case 233: v = s->debugcause; break;
    case 234: v = ccount(cpu); break;
    case 235: v = s->prid; break;
    case 236: v = s->icount; break;
    case 237: v = s->icountlevel; break;
    case 238: v = s->excvaddr; break;
    case 240: case 241: case 242: v = s->ccompare[id - 240]; break;
    case 244: case 245: case 246: case 247: v = s->misc[id - 244]; break;
    case XT_SYS_RFE:
        s->ps &= ~PS_EXCM;
        xt_update_derived(cpu);
        uam_cpu_request_exit(cpu);
        v = s->epc[1];
        break;
    case XT_SYS_RFDE:
        v = s->depc;
        break;
    case XT_SYS_RFWO: case XT_SYS_RFWU:
        if (id == XT_SYS_RFWO)
            s->ws &= ~(1u << s->wb);
        else
            s->ws |= 1u << s->wb;
        s->ps &= ~PS_EXCM;
        xt_window_rotate(cpu, (s->ps & PS_OWB) >> PS_OWB_SH);
        uam_cpu_request_exit(cpu);
        v = s->epc[1];
        break;
    case XT_SYS_RET_PC: v = s->ret_pc; break;
    default:
        if (id >= 177 && id <= 183)
            v = s->epc[id - 176];
        else if (id >= 194 && id <= 199)
            v = s->eps[id - 192];
        else if (id >= 209 && id <= 215)
            v = s->excsave[id - 208];
        else if (id == 227)
            v = 0;
        else {
            s->pend_cause = XT_EXC_ILLEGAL;
            return UAM_ERR_INVALID;
        }
    }
    *val = v;
    return UAM_OK;
}

static void set_ccompare(uam_cpu *cpu, unsigned n, uint32_t v)
{
    xt_state *s = xt_st(cpu);
    s->ccompare[n] = v;
    s->intr &= ~(1u << s->cfg.timer_int[n]);
    timer_arm(cpu, n);
}

static uam_status xt_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    xt_state *s = xt_st(cpu);
    uint32_t v = (uint32_t)val;
    switch (id) {
    case 0: xt_set_reg(cpu, XT_LBEG, v); break;
    case 1: xt_set_reg(cpu, XT_LEND, v); xt_update_derived(cpu); uam_cpu_request_exit(cpu); break;
    case 2: xt_set_reg(cpu, XT_LCOUNT, v); xt_update_derived(cpu); uam_cpu_request_exit(cpu); break;
    case 72: xt_window_rotate(cpu, v); uam_cpu_request_exit(cpu); break;
    case 73: s->ws = v & ((1u << nwin(s)) - 1); xt_update_derived(cpu); break;
    case 96: s->ibreakenable = v & ((1u << s->cfg.nibreak) - 1); break;
    case 97: s->memctl = v; break;
    case 99: s->atomctl = v & 0x3F; break;
    case 104: s->ddr = v; break;
    case 128: case 129: s->ibreaka[id - 128] = v; break;
    case 144: case 145: s->dbreaka[id - 144] = v; break;
    case 160: case 161: s->dbreakc[id - 160] = v & 0xC000003Fu; break;
    case 192: s->depc = v; break;
    case 224: xt_set_reg(cpu, XT_CPENABLE, v & 0xFF); uam_cpu_request_exit(cpu); break;
    case 226: s->intr |= v & type_mask(s, UAM_XT_INT_SOFTWARE); uam_cpu_request_exit(cpu); break;
    case 227:
        s->intr &= ~(v & (type_mask(s, UAM_XT_INT_SOFTWARE) | type_mask(s, UAM_XT_INT_EDGE)
                          | type_mask(s, UAM_XT_INT_WRITE_ERR)));
        uam_cpu_request_exit(cpu);
        break;
    case 228:
        s->intenable = s->cfg.ninterrupt >= 32 ? v : v & ((1u << s->cfg.ninterrupt) - 1);
        uam_cpu_request_exit(cpu);
        break;
    case 230: write_ps(cpu, v); break;
    case 231: s->vecbase = v; break;
    case 232: s->exccause = v & 63; break;
    case 234:
        s->ccount_bias = v - (uint32_t)uam_sched_now(cpu->sched);
        for (unsigned i = 0; i < s->cfg.nccompare; i++)
            timer_arm(cpu, i);
        break;
    case 236: s->icount = v; break;
    case 237: s->icountlevel = v & 15; break;
    case 238: s->excvaddr = v; break;
    case 240: case 241: case 242: set_ccompare(cpu, id - 240, v); uam_cpu_request_exit(cpu); break;
    case 244: case 245: case 246: case 247: s->misc[id - 244] = v; break;
    case XT_SYS_STEP:
        cpu->priv = v ? cpu->priv | XT_PRIV_STEP : cpu->priv & ~XT_PRIV_STEP;
        uam_cpu_request_exit(cpu);
        break;
    case XT_SYS_RSIL: case XT_SYS_WAITI:
        s->ps = (s->ps & ~PS_INTLEVEL) | (v & PS_INTLEVEL);
        uam_cpu_request_exit(cpu);
        break;
    case XT_SYS_RFI:
        s->ret_pc = s->epc[v];
        write_ps(cpu, s->eps[v]);
        break;
    case XT_SYS_ROTW:
        xt_window_rotate(cpu, s->wb + v);
        uam_cpu_request_exit(cpu);
        break;
    default:
        if (id >= 177 && id <= 183)
            s->epc[id - 176] = v;
        else if (id >= 194 && id <= 199)
            s->eps[id - 192] = v & PS_MASK;
        else if (id >= 209 && id <= 215)
            s->excsave[id - 208] = v;
        else {
            s->pend_cause = XT_EXC_ILLEGAL;
            return UAM_ERR_INVALID;
        }
    }
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static uint64_t fail(xt_state *s, uam_status *st, uint32_t cause)
{
    s->pend_cause = cause;
    s->pend_vaddr = 0xFFFFFFFFu;
    *st = UAM_ERR_FAULT;
    return 0;
}

/* ENTRY as, imm12 (ISA manual 4.7.1.4): check the window of the callee's frame, set its
 * stack pointer and rotate. */
static uint64_t h_entry(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    xt_state *s = xt_st(cpu);
    unsigned as = a & 15;
    uint32_t imm = (uint32_t)(a >> 4) & 0xFFF;
    unsigned inc = xt_reg(cpu, XT_CALLINC) & 3;
    if (as > 3 || !(s->ps & PS_WOE) || (s->ps & PS_EXCM))
        return fail(s, st, XT_EXC_ILLEGAL);
    for (unsigned i = 1; i <= inc; i++)
        if (ws_bit(s, s->wb + i))
            return fail(s, st, XT_TRAP_WOVF);
    xt_set_reg(cpu, XT_A(inc * 4 + as), xt_reg(cpu, XT_A(as)) - (imm << 3));
    xt_window_rotate(cpu, s->wb + inc);
    s->ws |= 1u << s->wb;
    xt_update_derived(cpu);
    return 0;
}

/* RETW (ISA manual 4.7.1.4): the caller's frame is resident, or underflow. */
static uint64_t h_retw(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    xt_state *s = xt_st(cpu);
    uint32_t a0 = xt_reg(cpu, XT_A(0)), pc = (uint32_t)a;
    unsigned n = a0 >> 30;
    unsigned m = ws_bit(s, s->wb + nwin(s) - 1) ? 1 : ws_bit(s, s->wb + nwin(s) - 2) ? 2
               : ws_bit(s, s->wb + nwin(s) - 3) ? 3 : 0;
    if (n == 0 || (m && m != n) || !(s->ps & PS_WOE) || (s->ps & PS_EXCM))
        return fail(s, st, XT_EXC_ILLEGAL);
    uint32_t target = (pc & 0xC0000000u) | (a0 & 0x3FFFFFFFu);
    uint32_t caller = (s->wb + nwin(s) - n) % nwin(s);
    if (!ws_bit(s, caller)) {
        s->pend_n = n;
        return fail(s, st, XT_TRAP_WUNF);
    }
    s->ws &= ~(1u << s->wb);
    xt_window_rotate(cpu, caller);
    return target;
}

static uint64_t h_movsp(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    xt_state *s = xt_st(cpu);
    unsigned w = nwin(s);
    if (!ws_bit(s, s->wb + w - 1) && !ws_bit(s, s->wb + w - 2) && !ws_bit(s, s->wb + w - 3))
        return fail(s, st, XT_EXC_ALLOCA);
    return 0;
}

static uint64_t h_fpu(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)c;
    uam_status r = xt_fpu_exec(env, (unsigned)b, (uint32_t)a);
    if (r != UAM_OK)
        return fail(xt_st(env), st, XT_EXC_ILLEGAL);
    return 0;
}

/* Region Protection: eight 512 MiB regions, one attribute each (ISA manual 4.6.3). */
static uint64_t h_tlb(void *env, uint64_t a, uint64_t at, uint64_t as, uam_status *st)
{
    (void)st;
    uam_cpu *cpu = env;
    xt_state *s = xt_st(cpu);
    unsigned region = (uint32_t)as >> 29;
    uint32_t vpn = (uint32_t)as & 0xE0000000u;
    switch (a) {
    case XTO_WITLB: s->itlb[region] = (uint8_t)(at & 15); uam_cpu_xlate_changed(cpu); return 0;
    case XTO_WDTLB: s->dtlb[region] = (uint8_t)(at & 15); uam_cpu_xlate_changed(cpu); return 0;
    case XTO_RITLB1: return vpn | s->itlb[region];
    case XTO_RDTLB1: return vpn | s->dtlb[region];
    case XTO_PITLB: case XTO_PDTLB: return vpn | 1;
    default: return 0;   /* RITLB0, RDTLB0 */
    }
}

static uint64_t h_eri_rd(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    xt_state *s = xt_st(env);
    return s->eri_rd ? s->eri_rd(s->eri_ctx, (uint32_t)a) : 0;
}

static uint64_t h_eri_wr(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    xt_state *s = xt_st(env);
    if (s->eri_wr)
        s->eri_wr(s->eri_ctx, (uint32_t)a, (uint32_t)b);
    return 0;
}

#define RW (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE)
static const uam_helper_desc xt_helper_desc[XT_H__COUNT] = {
    [XT_H_ENTRY]  = { "xt_entry", 1, { 32 }, 0, RW | UAM_HELPER_MAY_FAULT, h_entry },
    [XT_H_RETW]   = { "xt_retw", 1, { 32 }, 32, RW | UAM_HELPER_MAY_FAULT, h_retw },
    [XT_H_MOVSP]  = { "xt_movsp", 0, { 0 }, 0, UAM_HELPER_READS_STATE | UAM_HELPER_MAY_FAULT, h_movsp },
    [XT_H_FPU]    = { "xt_fpu", 2, { 32, 32 }, 0, RW, h_fpu },
    [XT_H_TLB]    = { "xt_tlb", 3, { 32, 32, 32 }, 32, RW, h_tlb },
    [XT_H_ERI_RD] = { "xt_rer", 1, { 32 }, 32, RW, h_eri_rd },
    [XT_H_ERI_WR] = { "xt_wer", 2, { 32, 32 }, 0, RW, h_eri_wr },
};
const uam_helper_table xt_helpers = { xt_helper_desc, XT_H__COUNT };

/* ------------------------------------------------------------------------ */
/* Region Protection as address translation                                  */
/* ------------------------------------------------------------------------ */

static uam_status xt_translate(uam_cpu *cpu, uint64_t va, uam_xlate_kind kind, uam_xlate *out)
{
    xt_state *s = xt_st(cpu);
    out->paddr = va & 0xFFFFFFFFu;
    out->fault = 0;
    out->fixed = !(s->cfg.options & UAM_XT_OPT_REGION_PROT);
    if (out->fixed || kind == UAM_XLATE_DEBUG)
        return UAM_OK;
    unsigned region = (uint32_t)va >> 29;
    uint8_t need = kind == UAM_XLATE_FETCH ? UAM_XT_CA_X : kind == UAM_XLATE_LOAD ? UAM_XT_CA_R : UAM_XT_CA_W;
    uint8_t attr = kind == UAM_XLATE_FETCH ? s->itlb[region] : s->dtlb[region];
    return (s->cfg.ca_rights[attr] & need) ? UAM_OK : UAM_ERR_XLATE;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static uam_status xt_init_endian(uam_cpu *cpu, unsigned endian)
{
    xt_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    cpu->isa_state = s;
    s->cfg = uam_xtensa_lx6;
    s->prid = s->cfg.prid;
    s->endian = endian;
    for (unsigned i = 0; i < 3; i++) {
        s->tctx[i] = (struct xt_timer_ctx){ cpu, i };
        uam_timer_init(&s->timer[i], timer_fire, &s->tctx[i]);
    }
    return UAM_OK;
}

static uam_status xt_init_le(uam_cpu *cpu) { return xt_init_endian(cpu, UAM_ACC_LE); }
static uam_status xt_init_be(uam_cpu *cpu) { return xt_init_endian(cpu, UAM_ACC_BE); }

static void cancel_timers(uam_cpu *cpu)
{
    xt_state *s = xt_st(cpu);
    if (cpu->sched)
        for (unsigned i = 0; i < 3; i++)
            uam_timer_cancel(cpu->sched, &s->timer[i]);
}

static void xt_fini(uam_cpu *cpu)
{
    cancel_timers(cpu);
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

static void xt_reset(uam_cpu *cpu, uint64_t entry)
{
    xt_state *s = xt_st(cpu);
    cancel_timers(cpu);
    memset(cpu->regfile, 0, cpu->regfile_size);
    s->ps = PS_EXCM | (s->cfg.ninterrupt ? PS_INTLEVEL : 0);
    s->wb = 0;
    s->ws = 1;
    memset(s->epc, 0, sizeof(s->epc));
    memset(s->eps, 0, sizeof(s->eps));
    memset(s->excsave, 0, sizeof(s->excsave));
    s->depc = s->exccause = s->excvaddr = 0;
    s->vecbase = s->cfg.vecbase;
    s->intr = 0;
    s->intenable = 0;
    s->nmi_pending = false;
    s->ccount_bias = -(uint32_t)(cpu->sched ? uam_sched_now(cpu->sched) : 0);
    memset(s->ccompare, 0, sizeof(s->ccompare));
    s->atomctl = 0x28;
    s->memctl = 0;
    memset(s->misc, 0, sizeof(s->misc));
    s->debugcause = s->icount = s->icountlevel = s->ibreakenable = s->ddr = 0;
    memset(s->ibreaka, 0, sizeof(s->ibreaka));
    memset(s->dbreaka, 0, sizeof(s->dbreaka));
    memset(s->dbreakc, 0, sizeof(s->dbreakc));
    memset(s->itlb, 2, sizeof(s->itlb));
    memset(s->dtlb, 2, sizeof(s->dtlb));
    s->pend_cause = XT_EXC_ILLEGAL;
    s->pend_vaddr = 0xFFFFFFFFu;
    s->taken_lines = 0;
    cpu->priv = 0;
    cpu->pc = entry & 0xFFFFFFFFu;
    xt_update_derived(cpu);
    for (unsigned i = 0; i < s->cfg.nccompare; i++)
        timer_arm(cpu, i);
}

/* ------------------------------------------------------------------------ */
/* Debugger view, dump, snapshots                                            */
/* ------------------------------------------------------------------------ */

/* pc, a0-a15 (the current window), then SAR, PS, WINDOWBASE, WINDOWSTART, LBEG, LEND,
 * LCOUNT, BR, SCOMPARE1, ACCLO, ACCHI, M0-M3, THREADPTR, f0-f15, FCR, FSR. */
enum { G_PC, G_A0, G_SAR = G_A0 + 16, G_PS, G_WB, G_WS, G_LBEG, G_LEND, G_LCOUNT, G_BR,
       G_SCOMPARE1, G_ACCLO, G_ACCHI, G_M0, G_THREADPTR = G_M0 + 4, G_F0, G_FCR = G_F0 + 16,
       G_FSR, G_N };

static const char xt_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>xtensa</architecture><feature name=\"org.gnu.gdb.xtensa.core\">"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"0\"/>"
    "<reg name=\"a0\" bitsize=\"32\"/><reg name=\"a1\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"a2\" bitsize=\"32\"/><reg name=\"a3\" bitsize=\"32\"/><reg name=\"a4\" bitsize=\"32\"/>"
    "<reg name=\"a5\" bitsize=\"32\"/><reg name=\"a6\" bitsize=\"32\"/><reg name=\"a7\" bitsize=\"32\"/>"
    "<reg name=\"a8\" bitsize=\"32\"/><reg name=\"a9\" bitsize=\"32\"/><reg name=\"a10\" bitsize=\"32\"/>"
    "<reg name=\"a11\" bitsize=\"32\"/><reg name=\"a12\" bitsize=\"32\"/><reg name=\"a13\" bitsize=\"32\"/>"
    "<reg name=\"a14\" bitsize=\"32\"/><reg name=\"a15\" bitsize=\"32\"/>"
    "<reg name=\"sar\" bitsize=\"32\"/><reg name=\"ps\" bitsize=\"32\"/>"
    "<reg name=\"windowbase\" bitsize=\"32\"/><reg name=\"windowstart\" bitsize=\"32\"/>"
    "<reg name=\"lbeg\" bitsize=\"32\"/><reg name=\"lend\" bitsize=\"32\"/><reg name=\"lcount\" bitsize=\"32\"/>"
    "<reg name=\"br\" bitsize=\"32\"/><reg name=\"scompare1\" bitsize=\"32\"/>"
    "<reg name=\"acclo\" bitsize=\"32\"/><reg name=\"acchi\" bitsize=\"32\"/>"
    "<reg name=\"m0\" bitsize=\"32\"/><reg name=\"m1\" bitsize=\"32\"/><reg name=\"m2\" bitsize=\"32\"/>"
    "<reg name=\"m3\" bitsize=\"32\"/><reg name=\"threadptr\" bitsize=\"32\"/>"
    "<reg name=\"f0\" bitsize=\"32\"/><reg name=\"f1\" bitsize=\"32\"/><reg name=\"f2\" bitsize=\"32\"/>"
    "<reg name=\"f3\" bitsize=\"32\"/><reg name=\"f4\" bitsize=\"32\"/><reg name=\"f5\" bitsize=\"32\"/>"
    "<reg name=\"f6\" bitsize=\"32\"/><reg name=\"f7\" bitsize=\"32\"/><reg name=\"f8\" bitsize=\"32\"/>"
    "<reg name=\"f9\" bitsize=\"32\"/><reg name=\"f10\" bitsize=\"32\"/><reg name=\"f11\" bitsize=\"32\"/>"
    "<reg name=\"f12\" bitsize=\"32\"/><reg name=\"f13\" bitsize=\"32\"/><reg name=\"f14\" bitsize=\"32\"/>"
    "<reg name=\"f15\" bitsize=\"32\"/><reg name=\"fcr\" bitsize=\"32\"/><reg name=\"fsr\" bitsize=\"32\"/>"
    "</feature></target>";

static int gdb_off(unsigned n)
{
    if (n >= G_A0 && n < G_A0 + 16)
        return (int)XT_A(n - G_A0);
    if (n >= G_M0 && n < G_M0 + 4)
        return (int)XT_M(n - G_M0);
    if (n >= G_F0 && n < G_F0 + 16)
        return (int)XT_F(n - G_F0);
    switch (n) {
    case G_SAR: return XT_SAR;
    case G_LBEG: return XT_LBEG;
    case G_LEND: return XT_LEND;
    case G_LCOUNT: return XT_LCOUNT;
    case G_BR: return XT_BR;
    case G_SCOMPARE1: return XT_SCOMPARE1;
    case G_ACCLO: return XT_ACCLO;
    case G_ACCHI: return XT_ACCHI;
    case G_THREADPTR: return XT_THREADPTR;
    case G_FCR: return XT_FCR;
    case G_FSR: return XT_FSR;
    default: return -1;
    }
}

static bool xt_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    xt_state *s = xt_st(cpu);
    int off = gdb_off(n);
    if (off >= 0)
        *v = xt_reg(cpu, (uint32_t)off);
    else if (n == G_PC)
        *v = cpu->pc;
    else if (n == G_PS)
        *v = read_ps(cpu);
    else if (n == G_WB)
        *v = s->wb;
    else if (n == G_WS)
        *v = s->ws;
    else
        return false;
    return true;
}

static bool xt_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    xt_state *s = xt_st(cpu);
    int off = gdb_off(n);
    if (off >= 0) {
        xt_set_reg(cpu, (uint32_t)off, (uint32_t)v);
        if (n == G_LEND || n == G_LCOUNT)
            xt_update_derived(cpu);
    } else if (n == G_PC) {
        cpu->pc = v & 0xFFFFFFFFu;
    } else if (n == G_PS) {
        write_ps(cpu, (uint32_t)v);
    } else if (n == G_WB) {
        xt_window_rotate(cpu, (uint32_t)v);
    } else if (n == G_WS) {
        s->ws = (uint32_t)v & ((1u << nwin(s)) - 1);
        xt_update_derived(cpu);
    } else {
        return false;
    }
    uam_cpu_xlate_changed(cpu);   /* LEND is read when blocks are lifted */
    return true;
}

static const uam_gdb_arch xt_gdb = {
    .target_xml = xt_target_xml, .n_regs = G_N, .reg_bytes = 4,
    .read_reg = xt_gdb_read, .write_reg = xt_gdb_write,
};

static void xt_dump_state(uam_cpu *cpu, FILE *out)
{
    xt_state *s = xt_st(cpu);
    fprintf(out, "pc=%08x ps=%08x model=%s %s icount=%llu\n", (unsigned)cpu->pc, read_ps(cpu),
            s->cfg.name, s->endian == UAM_ACC_BE ? "big-endian" : "little-endian",
            (unsigned long long)cpu->icount);
    for (unsigned i = 0; i < 16; i++)
        fprintf(out, "a%-2u=%08x%s", i, xt_reg(cpu, XT_A(i)), i % 4 == 3 ? "\n" : "  ");
    fprintf(out, "sar=%02x windowbase=%u windowstart=%04x lbeg=%08x lend=%08x lcount=%08x\n",
            xt_reg(cpu, XT_SAR), s->wb, s->ws, xt_reg(cpu, XT_LBEG), xt_reg(cpu, XT_LEND),
            xt_reg(cpu, XT_LCOUNT));
    fprintf(out, "epc1=%08x exccause=%u excvaddr=%08x depc=%08x vecbase=%08x\n", s->epc[1],
            s->exccause, s->excvaddr, s->depc, s->vecbase);
    fprintf(out, "interrupt=%08x intenable=%08x ccount=%08x ccompare=%08x %08x %08x\n",
            interrupt_reg(s), s->intenable, cpu->sched ? ccount(cpu) : 0, s->ccompare[0],
            s->ccompare[1], s->ccompare[2]);
}

static void xt_save(uam_cpu *cpu, uam_state_writer *w)
{
    xt_state *s = xt_st(cpu);
    const uint32_t v[] = { s->ps, s->wb, s->ws, s->depc, s->exccause, s->excvaddr, s->vecbase,
                           s->intr, s->lines, s->intenable, s->ccount_bias, s->atomctl, s->memctl,
                           s->prid, s->debugcause, s->icount, s->icountlevel, s->ibreakenable,
                           s->ddr, s->ret_pc, s->pend_cause, s->pend_vaddr, s->pend_n };
    for (size_t i = 0; i < UAM_ARRAY_LEN(v); i++)
        uam_state_put_u32(w, v[i]);
    for (unsigned i = 0; i < 8; i++) {
        uam_state_put_u32(w, s->epc[i]);
        uam_state_put_u32(w, s->eps[i]);
        uam_state_put_u32(w, s->excsave[i]);
    }
    for (unsigned i = 0; i < 4; i++)
        uam_state_put_u32(w, s->misc[i]);
    for (unsigned i = 0; i < 2; i++) {
        uam_state_put_u32(w, s->ibreaka[i]);
        uam_state_put_u32(w, s->dbreaka[i]);
        uam_state_put_u32(w, s->dbreakc[i]);
    }
    uam_state_put_bytes(w, s->itlb, sizeof(s->itlb));
    uam_state_put_bytes(w, s->dtlb, sizeof(s->dtlb));
    uam_state_put_bool(w, s->nmi_pending);
    for (unsigned i = 0; i < 3; i++) {
        uam_state_put_u32(w, s->ccompare[i]);
        uam_state_put_timer(w, &s->timer[i]);
    }
}

static void xt_restore(uam_cpu *cpu, uam_state_reader *r)
{
    xt_state *s = xt_st(cpu);
    uint32_t *v[] = { &s->ps, &s->wb, &s->ws, &s->depc, &s->exccause, &s->excvaddr, &s->vecbase,
                      &s->intr, &s->lines, &s->intenable, &s->ccount_bias, &s->atomctl, &s->memctl,
                      &s->prid, &s->debugcause, &s->icount, &s->icountlevel, &s->ibreakenable,
                      &s->ddr, &s->ret_pc, &s->pend_cause, &s->pend_vaddr };
    for (size_t i = 0; i < UAM_ARRAY_LEN(v); i++)
        *v[i] = uam_state_get_u32(r);
    s->pend_n = uam_state_get_u32(r) & 3;
    for (unsigned i = 0; i < 8; i++) {
        s->epc[i] = uam_state_get_u32(r);
        s->eps[i] = uam_state_get_u32(r);
        s->excsave[i] = uam_state_get_u32(r);
    }
    for (unsigned i = 0; i < 4; i++)
        s->misc[i] = uam_state_get_u32(r);
    for (unsigned i = 0; i < 2; i++) {
        s->ibreaka[i] = uam_state_get_u32(r);
        s->dbreaka[i] = uam_state_get_u32(r);
        s->dbreakc[i] = uam_state_get_u32(r);
    }
    uam_state_get_bytes(r, s->itlb, sizeof(s->itlb));
    uam_state_get_bytes(r, s->dtlb, sizeof(s->dtlb));
    for (unsigned i = 0; i < 8; i++) {
        s->itlb[i] &= 15;
        s->dtlb[i] &= 15;
    }
    s->nmi_pending = uam_state_get_bool(r);
    for (unsigned i = 0; i < 3; i++) {
        s->ccompare[i] = uam_state_get_u32(r);
        uam_state_get_timer(r, &s->timer[i]);
    }
    if (nwin(s))
        s->wb %= nwin(s);
}

static const uam_regdesc xt_regs[] = {
    { "a0", XT_A(0), 32, UAM_REG_GPR }, { "a1", XT_A(1), 32, UAM_REG_GPR },
    { "a2", XT_A(2), 32, UAM_REG_GPR }, { "a3", XT_A(3), 32, UAM_REG_GPR },
    { "a4", XT_A(4), 32, UAM_REG_GPR }, { "a5", XT_A(5), 32, UAM_REG_GPR },
    { "a6", XT_A(6), 32, UAM_REG_GPR }, { "a7", XT_A(7), 32, UAM_REG_GPR },
    { "a8", XT_A(8), 32, UAM_REG_GPR }, { "a9", XT_A(9), 32, UAM_REG_GPR },
    { "a10", XT_A(10), 32, UAM_REG_GPR }, { "a11", XT_A(11), 32, UAM_REG_GPR },
    { "a12", XT_A(12), 32, UAM_REG_GPR }, { "a13", XT_A(13), 32, UAM_REG_GPR },
    { "a14", XT_A(14), 32, UAM_REG_GPR }, { "a15", XT_A(15), 32, UAM_REG_GPR },
    { "sar", XT_SAR, 32, UAM_REG_OTHER }, { "lbeg", XT_LBEG, 32, UAM_REG_OTHER },
    { "lend", XT_LEND, 32, UAM_REG_OTHER }, { "lcount", XT_LCOUNT, 32, UAM_REG_OTHER },
    { "lenda", XT_LENDA, 32, UAM_REG_SYS_SHADOW }, { "br", XT_BR, 32, UAM_REG_FLAG },
    { "scompare1", XT_SCOMPARE1, 32, UAM_REG_OTHER }, { "acclo", XT_ACCLO, 32, UAM_REG_OTHER },
    { "acchi", XT_ACCHI, 32, UAM_REG_OTHER }, { "m0", XT_M(0), 32, UAM_REG_OTHER },
    { "m1", XT_M(1), 32, UAM_REG_OTHER }, { "m2", XT_M(2), 32, UAM_REG_OTHER },
    { "m3", XT_M(3), 32, UAM_REG_OTHER }, { "threadptr", XT_THREADPTR, 32, UAM_REG_OTHER },
    { "litbase", XT_LITBASE, 32, UAM_REG_OTHER }, { "fcr", XT_FCR, 32, UAM_REG_OTHER },
    { "fsr", XT_FSR, 32, UAM_REG_OTHER },
    { "f0", XT_F(0), 32, UAM_REG_OTHER }, { "f1", XT_F(1), 32, UAM_REG_OTHER },
    { "f2", XT_F(2), 32, UAM_REG_OTHER }, { "f3", XT_F(3), 32, UAM_REG_OTHER },
    { "f4", XT_F(4), 32, UAM_REG_OTHER }, { "f5", XT_F(5), 32, UAM_REG_OTHER },
    { "f6", XT_F(6), 32, UAM_REG_OTHER }, { "f7", XT_F(7), 32, UAM_REG_OTHER },
    { "f8", XT_F(8), 32, UAM_REG_OTHER }, { "f9", XT_F(9), 32, UAM_REG_OTHER },
    { "f10", XT_F(10), 32, UAM_REG_OTHER }, { "f11", XT_F(11), 32, UAM_REG_OTHER },
    { "f12", XT_F(12), 32, UAM_REG_OTHER }, { "f13", XT_F(13), 32, UAM_REG_OTHER },
    { "f14", XT_F(14), 32, UAM_REG_OTHER }, { "f15", XT_F(15), 32, UAM_REG_OTHER },
    { "wcheck", XT_WCHECK, 32, UAM_REG_SYS_SHADOW }, { "callinc", XT_CALLINC, 32, UAM_REG_OTHER },
    { "cpenable", XT_CPENABLE, 32, UAM_REG_SYS_SHADOW }, { "loopen", XT_LOOPEN, 32, UAM_REG_SYS_SHADOW },
};

#define XT_DESC(sym, nm, endian, initfn)                                               \
    const uam_isa sym = {                                                            \
        .name = nm, .addr_width = 32, .default_endian = endian,                      \
        .regfile_size = XT_REGFILE_SIZE, .regs = xt_regs, .n_regs = UAM_ARRAY_LEN(xt_regs), \
        .helpers = { xt_helper_desc, XT_H__COUNT },                                  \
        .init = initfn, .fini = xt_fini, .reset = xt_reset, .lift = xt_lift,           \
        .take_exception = xt_take_exception, .irq_take = xt_irq_take,                \
        .irq_wakeup = xt_irq_wakeup, .irq_input = xt_irq_input,                      \
        .sysreg_read = xt_sysreg_read, .sysreg_write = xt_sysreg_write,              \
        .translate = xt_translate, .gdb = &xt_gdb, .dump_state = xt_dump_state,      \
        .save = xt_save, .restore = xt_restore, .irq_lines = xt_irq_lines,           \
    }

XT_DESC(uam_isa_xtensa, "xtensa", UAM_ACC_LE, xt_init_le);
XT_DESC(uam_isa_xtensaeb, "xtensaeb", UAM_ACC_BE, xt_init_be);
