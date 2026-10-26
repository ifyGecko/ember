/*
 * ppc_booke.c - The Book E operating environment of the 32-bit PowerPC module
 * (uam_ppc_config.model = UAM_PPC_MODEL_BOOKE; Power ISA Book III-E).
 *
 * MSR: WE (wait), CE (critical interrupts), EE, PR, FP, ME, FE0/FE1, DE (debug
 * interrupts), IS/DS (the address space fetches and data accesses translate in).
 *
 * Interrupts go to IVPR[0:15] | IVORn[16:27]. Four classes save the pc and MSR in
 * their own pair: base (SRR0/1, rfi), critical (CSRR0/1, rfci: critical input,
 * watchdog), machine check (MCSRR0/1 and rfmci, or the critical pair, as configured)
 * and debug (DSRR0/1 and rfdi, or the critical pair). Entering one clears MSR[WE EE
 * PR FP FE0 FE1 IS DS], and CE and DE for the critical and debug classes, and ME as
 * well for a machine check. ESR reports program (PIL, PPR, PTR, FP), alignment, DSI
 * and DTLB details (ST: a store), DEAR the data address.
 *
 *   IVOR0 critical input   IVOR4 external        IVOR8  system call     IVOR12 watchdog
 *   IVOR1 machine check    IVOR5 alignment       IVOR9  AP unavailable  IVOR13 data TLB error
 *   IVOR2 data storage     IVOR6 program         IVOR10 decrementer     IVOR14 instruction TLB error
 *   IVOR3 instr. storage   IVOR7 FP unavailable  IVOR11 fixed interval  IVOR15 debug
 *
 * Timers (TCR, TSR): the decrementer counts down to 0 and sets TSR[DIS] when it gets
 * there, then stops or (TCR[ARE]) starts again from DECAR; the fixed-interval timer
 * sets TSR[FIS] when the time-base bit TCR[FP] selects changes from 0 to 1; the
 * watchdog, at each such change of its bit (TCR[WP]), sets TSR[ENW], then TSR[WIS],
 * then resets the processor as TCR[WRC] says. Their interrupts are levels, cleared
 * by writing 1 to the TSR bits. A timer only has a scheduler event while its
 * interrupt (or the watchdog's reset) could happen; otherwise TSR catches up when it
 * is read or the enables change.
 *
 * MMU: TLB arrays (TLB0 set-associative, TLB1 fully associative, geometry and page
 * sizes from the CPU model) searched for the effective address with the address
 * space and the PIDs; permissions UX SX UW SW UR SR. No match is a TLB error
 * interrupt, which loads MAS0-MAS2, MAS3, MAS6, MAS7 with the defaults in MAS4 as
 * the architected MMU does; a permission failure is a storage interrupt. tlbwe,
 * tlbre and tlbsx move entries through the MAS registers (or a board's own
 * interface, uam_ppc_set_tlb_ops), tlbivax invalidates on every hart of the bus,
 * MMUCSR0 flash-invalidates. Entries with IPROT survive both. Not modelled: the E
 * (little-endian page) attribute, debug address compares (IAC/DAC: plain SPRs).
 */
#include <string.h>

#include "uam/bus.h"

#include "ppc.h"

/* TCR and TSR fields. */
#define TCR_WP_SHIFT   30
#define TCR_WRC_SHIFT  28
#define TCR_WIE        (1u << 27)
#define TCR_DIE        (1u << 26)
#define TCR_FP_SHIFT   24
#define TCR_FIE        (1u << 23)
#define TCR_ARE        (1u << 22)
#define TCR_WPEXT_SHIFT 17
#define TCR_FPEXT_SHIFT 13
#define TSR_ENW        (1u << 31)
#define TSR_WIS        (1u << 30)
#define TSR_WRS_SHIFT  28
#define TSR_DIS        (1u << 27)
#define TSR_FIS        (1u << 26)

/* ESR. */
#define ESR_PIL 0x08000000u
#define ESR_PPR 0x04000000u
#define ESR_PTR 0x02000000u
#define ESR_FP  0x01000000u
#define ESR_ST  0x00800000u

/* DBCR0 and DBSR. */
#define DBCR0_IDM  0x40000000u
#define DBCR0_ICMP 0x08000000u
#define DBCR0_BRT  0x04000000u
#define DBCR0_TRAP 0x01000000u
#define DBSR_ICMP  0x08000000u
#define DBSR_BRT   0x04000000u
#define DBSR_TRAP  0x01000000u

enum {
    IV_CI = 0, IV_MC, IV_DSI, IV_ISI, IV_EXT, IV_ALIGN, IV_PROG, IV_FPU, IV_SC, IV_APU,
    IV_DEC, IV_FIT, IV_WDT, IV_DTLB, IV_ITLB, IV_DEBUG,
};

#define BOOKE_MSR_MASK (MSR_WE | MSR_CE | MSR_EE | MSR_PR | MSR_ME | MSR_FE0 | MSR_DE | MSR_FE1 \
                        | MSR_IS | MSR_DS)

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

bool ppc_booke_config_ok(const uam_ppc_config *c)
{
    const uam_ppc_booke_config *b = &c->booke;
    for (unsigned a = 0; a < 2; a++) {
        const uam_ppc_tlb_geom *g = &b->tlb[a];
        if (!g->entries)
            continue;
        if (g->entries > PPC_BOOKE_TLB_MAX || !g->ways || g->entries % g->ways)
            return false;
        unsigned sets = g->entries / g->ways;
        if (sets & (sets - 1))
            return false;
        if (!g->page_sizes || (g->page_sizes & ~(((UINT64_C(1) << 33) - 1) & ~UINT64_C(0xFFF))))
            return false;
    }
    if (!b->tlb[0].entries && !b->tlb[1].entries)
        return false;
    for (unsigned k = 0; k < 4; k++)
        if (b->fit_bit[k] > 63 || b->wdt_bit[k] > 63)
            return false;
    return b->pids == 1 || b->pids == 3;
}

static const uam_ppc_booke_config *bcfg(const ppc_state *s) { return &s->cfg.booke; }

/* ------------------------------------------------------------------------ */
/* MSR                                                                       */
/* ------------------------------------------------------------------------ */

/* Debug events lift as the trace mechanism: instruction complete and branch taken. */
unsigned ppc_booke_priv(const ppc_state *s)
{
    uint32_t msr = s->s.msr, d = s->bk.dbcr0;
    bool dbg = (msr & MSR_DE) && (d & DBCR0_IDM);
    return ((msr & MSR_PR) ? PPC_PRIV_PR : 0) | ((msr & MSR_FP) ? PPC_PRIV_FP : 0)
         | ((dbg && (d & DBCR0_ICMP)) ? PPC_PRIV_SE : 0) | ((dbg && (d & DBCR0_BRT)) ? PPC_PRIV_BE : 0);
}

static void timers_update(uam_cpu *cpu);

void ppc_booke_msr_write(uam_cpu *cpu, uint32_t v)
{
    ppc_state *s = ppc_st(cpu);
    uint32_t old = s->s.msr;
    s->s.msr = v & (BOOKE_MSR_MASK | (s->cfg.fpu ? MSR_FP : 0));
    if ((old ^ s->s.msr) & (MSR_PR | MSR_IS | MSR_DS)) {
        ppc_tlb_flush(s);
        uam_cpu_xlate_changed(cpu);
    }
    cpu->priv = ppc_booke_priv(s);
    if ((old ^ s->s.msr) & (MSR_EE | MSR_CE))
        timers_update(cpu);
    uam_cpu_request_exit(cpu);   /* EE, CE or DE may have enabled a pending interrupt */
}

/* ------------------------------------------------------------------------ */
/* Timers                                                                    */
/* ------------------------------------------------------------------------ */

/* The time-base bits (0 = least significant) the FIT and the watchdog watch. */
static unsigned fit_bit(const ppc_state *s)
{
    uint32_t t = s->bk.tcr, sel = (t >> TCR_FP_SHIFT) & 3;
    if (bcfg(s)->period_ext)
        return 63 - ((((t >> TCR_FPEXT_SHIFT) & 0xF) << 2) | sel);
    return bcfg(s)->fit_bit[sel];
}

static unsigned wdt_bit(const ppc_state *s)
{
    uint32_t t = s->bk.tcr, sel = (t >> TCR_WP_SHIFT) & 3;
    if (bcfg(s)->period_ext)
        return 63 - ((((t >> TCR_WPEXT_SHIFT) & 0xF) << 2) | sel);
    return bcfg(s)->wdt_bit[sel];
}

/* How often time-base bit b has changed from 0 to 1 by the value x. */
static uint64_t rises(uint64_t x, unsigned b) { return ((x >> b) + 1) >> 1; }

/* The watchdog's third time-out with TCR[WRC] set: report it and reset. */
static void watchdog_reset(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    unsigned wrc = (s->bk.tcr >> TCR_WRC_SHIFT) & 3;
    s->bk.tsr = (s->bk.tsr & ~(3u << TSR_WRS_SHIFT)) | (wrc << TSR_WRS_SHIFT);
    s->wrs_latched = (uint8_t)wrc;
    if (s->reset_sink)
        s->reset_sink(s->reset_ctx, cpu, wrc);
}

/* Bring TSR and the decrementer up to the present. */
static void timers_sync(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    ppc_booke_regs *b = &s->bk;
    if (!cpu->sched)
        return;
    uint64_t now = ppc_ticks_now(cpu), tb = now + s->s.tb_bias;

    if (s->s.dec) {
        uint64_t zero = s->s.dec_ticks + s->s.dec;
        if (now >= zero) {
            b->tsr |= TSR_DIS;
            if ((b->tcr & TCR_ARE) && b->decar) {
                s->s.dec_ticks = zero + (now - zero) / b->decar * b->decar;
                s->s.dec = b->decar;
            } else {
                s->s.dec = 0;
            }
        }
    }

    unsigned fb = fit_bit(s);
    if (tb > b->fit_tb && rises(tb, fb) != rises(b->fit_tb, fb))
        b->tsr |= TSR_FIS;
    b->fit_tb = tb;

    unsigned wb = wdt_bit(s);
    uint64_t n = tb > b->wdt_tb ? rises(tb, wb) - rises(b->wdt_tb, wb) : 0;
    b->wdt_tb = tb;
    for (; n; n--) {
        if (!(b->tsr & TSR_ENW)) {
            b->tsr |= TSR_ENW;
        } else if (!(b->tsr & TSR_WIS)) {
            b->tsr |= TSR_WIS;
        } else {
            if ((b->tcr >> TCR_WRC_SHIFT) & 3)
                watchdog_reset(cpu);
            break;
        }
    }
}

/* Arm `t` for the next rise of time-base bit `bit`, if there is one in range. */
static void arm_rise(uam_cpu *cpu, uam_timer *t, unsigned bit)
{
    ppc_state *s = ppc_st(cpu);
    uint64_t tb = ppc_tb_now(cpu), n = rises(tb, bit);
    if (bit >= 62 || n >= (UINT64_C(1) << (62 - bit))) {
        uam_timer_cancel(cpu->sched, t);
        return;
    }
    uint64_t next = (UINT64_C(1) << bit) + (n << (bit + 1));   /* the time-base value */
    uint64_t ticks = next - s->s.tb_bias;
    if (ticks > (UINT64_C(1) << 62) / s->cfg.tb_div) {
        uam_timer_cancel(cpu->sched, t);
        return;
    }
    uam_timer_arm(cpu->sched, t, ticks * s->cfg.tb_div);
}

/* Give each timer a scheduler event while it could interrupt (or reset). */
static void timers_arm(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    const ppc_booke_regs *b = &s->bk;
    if (!cpu->sched)
        return;
    bool ee = (s->s.msr & MSR_EE) && !s->s.checkstop, ce = (s->s.msr & MSR_CE) && !s->s.checkstop;
    if (s->s.dec && (b->tcr & TCR_DIE) && ee)
        uam_timer_arm(cpu->sched, &s->dec_timer, (s->s.dec_ticks + s->s.dec) * s->cfg.tb_div);
    else
        uam_timer_cancel(cpu->sched, &s->dec_timer);
    if ((b->tcr & TCR_FIE) && ee)
        arm_rise(cpu, &s->fit_timer, fit_bit(s));
    else
        uam_timer_cancel(cpu->sched, &s->fit_timer);
    if ((((b->tcr & TCR_WIE) && ce) || ((b->tcr >> TCR_WRC_SHIFT) & 3)) && !s->s.checkstop)
        arm_rise(cpu, &s->wdt_timer, wdt_bit(s));
    else
        uam_timer_cancel(cpu->sched, &s->wdt_timer);
}

static void timers_update(uam_cpu *cpu)
{
    timers_sync(cpu);
    timers_arm(cpu);
}

void ppc_booke_timer_fire(void *ctx)
{
    uam_cpu *cpu = ctx;
    timers_update(cpu);
    uam_cpu_request_exit(cpu);
}

static uint32_t dec_value(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    timers_sync(cpu);
    if (!s->s.dec || !cpu->sched)
        return s->s.dec;
    return s->s.dec - (uint32_t)(ppc_ticks_now(cpu) - s->s.dec_ticks);
}

/* ------------------------------------------------------------------------ */
/* TLB                                                                       */
/* ------------------------------------------------------------------------ */

static uint32_t page_mask(const uam_ppc_tlbe *e)
{
    return e->size_log2 >= 32 ? 0 : ~((UINT32_C(1) << e->size_log2) - 1);
}

static bool tlbe_match(const uam_ppc_tlbe *e, uint32_t ea, unsigned ts, const uint32_t *pids,
                       unsigned np)
{
    if (!e->valid || e->ts != ts || ((ea ^ e->epn) & page_mask(e)))
        return false;
    if (!e->tid)
        return true;
    for (unsigned k = 0; k < np; k++)
        if (e->tid == (pids[k] & 0x3FFF))
            return true;
    return false;
}

/* The first entry of array a that may hold ea (its set), and how many to look at. */
static unsigned set_first(const ppc_state *s, unsigned a, uint32_t ea, unsigned *n)
{
    const uam_ppc_tlb_geom *g = &bcfg(s)->tlb[a];
    *n = g->ways;
    if (g->ways >= g->entries)
        return 0;
    unsigned sets = g->entries / g->ways;
    return ((ea >> 12) & (sets - 1)) * g->ways;
}

static bool lookup(const ppc_state *s, uint32_t ea, unsigned ts, const uint32_t *pids, unsigned np,
                   unsigned *arr, unsigned *idx)
{
    for (unsigned a = 0; a < 2; a++) {
        if (!bcfg(s)->tlb[a].entries)
            continue;
        unsigned n, first = set_first(s, a, ea, &n);
        for (unsigned k = first; k < first + n; k++) {
            if (tlbe_match(&s->tlb[a][k], ea, ts, pids, np)) {
                *arr = a;
                *idx = k;
                return true;
            }
        }
    }
    return false;
}

uam_status ppc_booke_translate(uam_cpu *cpu, uint32_t ea, bool fetch, bool store, bool debug,
                               uam_xlate *out)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t msr = s->s.msr;
    const bool pr = !debug && (msr & MSR_PR);
    const unsigned ts = (msr & (fetch ? MSR_IS : MSR_DS)) ? 1 : 0;

    out->fixed = false;
    ppc_tlb_entry *te = NULL;
    if (!debug) {
        te = &(fetch ? s->itlb : s->dtlb)[(ea >> 12 ^ ea >> 20) & (PPC_TLB_SETS - 1)];
        if (te->vpage == ea >> 12 && (store ? te->write_ok : te->read_ok)) {
            out->paddr = ((uint64_t)te->ppage << 12) | (ea & 0xFFF);
            return UAM_OK;
        }
    }
    unsigned a, k;
    if (!lookup(s, ea, ts, s->bk.pid, bcfg(s)->pids, &a, &k)) {
        out->fault = PPC_XF_TLBMISS;
        return UAM_ERR_XLATE;
    }
    const uam_ppc_tlbe *e = &s->tlb[a][k];
    unsigned need = fetch ? (pr ? UAM_PPC_TLB_UX : UAM_PPC_TLB_SX)
                  : store ? (pr ? UAM_PPC_TLB_UW : UAM_PPC_TLB_SW)
                          : (pr ? UAM_PPC_TLB_UR : UAM_PPC_TLB_SR);
    if (!debug && !(e->perm & need)) {
        out->fault = PPC_XF_PROT;
        return UAM_ERR_XLATE;
    }
    out->paddr = (e->rpn & ~(uint64_t)(e->size_log2 >= 32 ? 0 : (UINT64_C(1) << e->size_log2) - 1))
               | (ea & ~page_mask(e));
    if (te)
        *te = (ppc_tlb_entry){
            .vpage = ea >> 12, .ppage = (uint32_t)(out->paddr >> 12),
            .read_ok = fetch || (e->perm & (pr ? UAM_PPC_TLB_UR : UAM_PPC_TLB_SR)),
            .write_ok = !fetch && (e->perm & (pr ? UAM_PPC_TLB_UW : UAM_PPC_TLB_SW)),
        };
    return UAM_OK;
}

/* Translations changed on this hart. */
static void tlb_changed(uam_cpu *cpu)
{
    ppc_tlb_flush(ppc_st(cpu));
    uam_cpu_xlate_changed(cpu);
}

/* The smallest size array a supports that is at least log2, else its largest. */
static unsigned fit_size(const ppc_state *s, unsigned a, unsigned log2)
{
    uint64_t sizes = bcfg(s)->tlb[a].page_sizes;
    if (sizes & (UINT64_C(1) << log2))
        return log2;
    for (unsigned n = log2; n <= 32; n++)
        if (sizes & (UINT64_C(1) << n))
            return n;
    for (unsigned n = 32; n >= 12; n--)
        if (sizes & (UINT64_C(1) << n))
            return n;
    return 12;
}

static void tlbe_set(uam_cpu *cpu, unsigned a, unsigned k, const uam_ppc_tlbe *in)
{
    ppc_state *s = ppc_st(cpu);
    uam_ppc_tlbe e = *in;
    e.size_log2 = (uint8_t)fit_size(s, a, e.size_log2 < 12 ? 12 : e.size_log2);
    e.epn &= page_mask(&e);
    e.rpn &= ~(e.size_log2 >= 32 ? 0 : (UINT64_C(1) << e.size_log2) - 1) & UINT64_C(0xFFFFFFFFF);
    e.tid &= 0x3FFF;
    e.ts &= 1;
    e.iprot = e.iprot && bcfg(s)->tlb[a].iprot;
    s->tlb[a][k] = e;
    tlb_changed(cpu);
}

/* ---- the MAS interface ---------------------------------------------------- */

#define MAS1_V       0x80000000u
#define MAS1_IPROT   0x40000000u
#define MAS1_TS      0x00001000u

static unsigned tsize_shift(const ppc_state *s) { return bcfg(s)->tsize_pow2 ? 7 : 8; }
static unsigned tsize_mask(const ppc_state *s) { return bcfg(s)->tsize_pow2 ? 0x1F : 0xF; }
static unsigned tsize_to_log2(const ppc_state *s, unsigned t)
{
    return bcfg(s)->tsize_pow2 ? 10 + t : 10 + 2 * t;
}
static unsigned log2_to_tsize(const ppc_state *s, unsigned l)
{
    return bcfg(s)->tsize_pow2 ? l - 10 : (l - 10) / 2;
}

/* Entry index from MAS0[ESEL] (the way, in a set-associative array) and an EPN. */
static bool mas_index(const ppc_state *s, unsigned a, unsigned esel, uint32_t epn, unsigned *k)
{
    const uam_ppc_tlb_geom *g = &bcfg(s)->tlb[a];
    if (a > 1 || !g->entries)
        return false;
    unsigned n, first = set_first(s, a, epn, &n);
    *k = first + esel % n;
    return true;
}

static void mas_load(ppc_state *s, unsigned a, unsigned k)
{
    const uam_ppc_tlbe *e = &s->tlb[a][k];
    const uam_ppc_tlb_geom *g = &bcfg(s)->tlb[a];
    uint32_t *m = s->bk.mas;
    unsigned way = g->ways >= g->entries ? k : k % g->ways;
    m[0] = (a << 28) | (way << 16) | (m[0] & 0xFFF);
    m[1] = (e->valid ? MAS1_V : 0) | (e->iprot ? MAS1_IPROT : 0) | (e->tid << 16)
         | (e->ts ? MAS1_TS : 0) | (log2_to_tsize(s, e->size_log2) << tsize_shift(s));
    m[2] = e->epn | (uint32_t)((e->user >> 4) & 3) << 5 | e->wimge;
    m[3] = (uint32_t)(e->rpn & 0xFFFFF000u) | (uint32_t)(e->user & 0xF) << 6 | e->perm;
    m[7] = (uint32_t)(e->rpn >> 32);
}

static void mas_entry(const ppc_state *s, uam_ppc_tlbe *e)
{
    const uint32_t *m = s->bk.mas;
    *e = (uam_ppc_tlbe){
        .valid = m[1] & MAS1_V, .iprot = m[1] & MAS1_IPROT, .ts = (m[1] & MAS1_TS) ? 1 : 0,
        .tid = (m[1] >> 16) & 0x3FFF,
        .size_log2 = (uint8_t)tsize_to_log2(s, (m[1] >> tsize_shift(s)) & tsize_mask(s)),
        .epn = m[2] & 0xFFFFF000u, .wimge = m[2] & 0x1F,
        .rpn = ((uint64_t)(m[7] & 0xF) << 32) | (m[3] & 0xFFFFF000u),
        .perm = m[3] & 0x3F, .user = (uint8_t)(((m[3] >> 6) & 0xF) | ((m[2] >> 5) & 3) << 4),
    };
}

/* After a TLB error or a tlbsx that found nothing: the MAS4 defaults. */
static void mas_defaults(ppc_state *s, uint32_t ea, unsigned ts, uint32_t tid)
{
    uint32_t *m = s->bk.mas, m4 = m[4];
    unsigned a = (m4 >> 28) & 3;
    if (a > 1 || !bcfg(s)->tlb[a].entries)
        a = bcfg(s)->tlb[0].entries ? 0 : 1;
    unsigned ways = bcfg(s)->tlb[a].ways, nv = s->bk.victim[a] % ways;
    m[0] = (a << 28) | (nv << 16) | ((nv + 1) % ways);
    m[1] = MAS1_V | (tid & 0x3FFF) << 16 | (ts ? MAS1_TS : 0)
         | (((m4 >> tsize_shift(s)) & tsize_mask(s)) << tsize_shift(s));
    m[2] = (ea & 0xFFFFF000u) | (m4 & 0x7F);
    m[3] = 0;
    m[7] = 0;
}

/* A TLB error interrupt: MAS0-3 and MAS7 from MAS4, MAS6 for a following tlbsx. */
static void mas_miss(ppc_state *s, uint32_t ea, bool fetch)
{
    unsigned ts = (s->s.msr & (fetch ? MSR_IS : MSR_DS)) ? 1 : 0;
    unsigned tidsel = (s->bk.mas[4] >> 16) & 3;
    uint32_t tid = tidsel < bcfg(s)->pids ? s->bk.pid[tidsel] : 0;
    mas_defaults(s, ea, ts, tid);
    s->bk.mas[6] = (s->bk.pid[0] & 0x3FFF) << 16 | ts;
}

static uam_status board_tlb(uam_cpu *cpu, int op, uint32_t insn, uint32_t ea)
{
    ppc_state *s = ppc_st(cpu);
    const uam_ppc_tlb_ops *o = s->tlb_ops;
    if (!o)
        return UAM_ERR_INVALID;
    uam_status st = UAM_ERR_INVALID;
    if (op == 0 && o->tlbwe) st = o->tlbwe(s->tlb_ctx, cpu, insn);
    if (op == 1 && o->tlbre) st = o->tlbre(s->tlb_ctx, cpu, insn);
    if (op == 2 && o->tlbsx) st = o->tlbsx(s->tlb_ctx, cpu, insn, ea);
    if (st == UAM_OK)
        tlb_changed(cpu);
    return st;
}

static uint64_t board_illegal(ppc_state *s, uam_status *st)
{
    s->hfault = PPC_HF_ILLEGAL;
    *st = UAM_ERR_ILLEGAL;
    return 0;
}

uint64_t ppc_helper_tlbwe(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    uam_status r = board_tlb(cpu, 0, (uint32_t)insn, 0);
    if (r != UAM_ERR_INVALID)
        return r == UAM_OK ? 0 : board_illegal(s, st);
    unsigned a = (s->bk.mas[0] >> 28) & 3, esel = (s->bk.mas[0] >> 16) & 0xFFF, k;
    if (!mas_index(s, a, esel, s->bk.mas[2], &k))
        return 0;   /* no such array: nothing written */
    uam_ppc_tlbe e;
    mas_entry(s, &e);
    tlbe_set(cpu, a, k, &e);
    unsigned ways = bcfg(s)->tlb[a].ways;
    if (esel % ways == s->bk.victim[a] % ways)
        s->bk.victim[a] = (s->bk.victim[a] + 1) % ways;
    return 0;
}

uint64_t ppc_helper_tlbre(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    uam_status r = board_tlb(cpu, 1, (uint32_t)insn, 0);
    if (r != UAM_ERR_INVALID)
        return r == UAM_OK ? 0 : board_illegal(s, st);
    unsigned a = (s->bk.mas[0] >> 28) & 3, esel = (s->bk.mas[0] >> 16) & 0xFFF, k;
    if (!mas_index(s, a, esel, s->bk.mas[2], &k)) {
        s->bk.mas[1] = 0;
        return 0;
    }
    mas_load(s, a, k);
    return 0;
}

uint64_t ppc_helper_tlbsx(void *env, uint64_t insn, uint64_t ea, uint64_t c, uam_status *st)
{
    (void)c;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    uam_status r = board_tlb(cpu, 2, (uint32_t)insn, (uint32_t)ea);
    if (r != UAM_ERR_INVALID)
        return r == UAM_OK ? 0 : board_illegal(s, st);
    uint32_t spid = (s->bk.mas[6] >> 16) & 0x3FFF;
    unsigned sas = s->bk.mas[6] & 1, a, k;
    bool hit = lookup(s, (uint32_t)ea, sas, &spid, 1, &a, &k);
    if (hit)
        mas_load(s, a, k);
    else
        mas_defaults(s, (uint32_t)ea, sas, spid);
    if (!hit)
        s->bk.mas[1] &= ~MAS1_V;
    if (insn & 1) {   /* tlbsx.: CR0[EQ] on a hit */
        unsigned so = uam_cpu_reg_read(cpu, PPC_SO_OFF, 8) & 1;
        uam_cpu_reg_write(cpu, PPC_CR_OFF(0), 8, (hit ? CR_EQ : 0) | so);
    }
    return 0;
}

/* Invalidate, on every hart of this bus, the unprotected entries of array a that map
 * ea (all of them with `all`). */
static void invalidate(uam_cpu *cpu, unsigned a, bool all, uint32_t ea, bool broadcast)
{
    for (ppc_state *h = ppc_harts(); h; h = h->next) {
        if (broadcast ? h->cpu->bus != cpu->bus : h->cpu != cpu)
            continue;
        if (!ppc_booke(h) || a > 1)
            continue;
        const uam_ppc_tlb_geom *g = &h->cfg.booke.tlb[a];
        for (unsigned k = 0; k < g->entries; k++) {
            uam_ppc_tlbe *e = &h->tlb[a][k];
            if (e->valid && !e->iprot && (all || !((ea ^ e->epn) & page_mask(e))))
                e->valid = false;
        }
        tlb_changed(h->cpu);
    }
}

uint64_t ppc_helper_tlbivax(void *env, uint64_t ea, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    uint32_t v = (uint32_t)ea;
    invalidate(env, (v >> 1) & 3, v & 8, v & 0xFFFFF000u, true);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Device control registers                                                  */
/* ------------------------------------------------------------------------ */

uint64_t ppc_helper_mfdcr(void *env, uint64_t dcrn, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    uint32_t v = 0;
    if (!s->dcr_ops || !s->dcr_ops->read
        || s->dcr_ops->read(s->dcr_ctx, cpu, (unsigned)dcrn & 0x3FF, &v) != UAM_OK)
        return board_illegal(s, st);
    return v;
}

uint64_t ppc_helper_mtdcr(void *env, uint64_t dcrn, uint64_t v, uint64_t c, uam_status *st)
{
    (void)c;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    if (!s->dcr_ops || !s->dcr_ops->write
        || s->dcr_ops->write(s->dcr_ctx, cpu, (unsigned)dcrn & 0x3FF, (uint32_t)v) != UAM_OK)
        return board_illegal(s, st);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                                */
/* ------------------------------------------------------------------------ */

static void enter(uam_cpu *cpu, unsigned iv, uint32_t srr0)
{
    ppc_state *s = ppc_st(cpu);
    ppc_booke_regs *b = &s->bk;
    uint32_t msr = s->s.msr, keep;
    bool crit_pair = iv == IV_CI || iv == IV_WDT
                  || (iv == IV_MC && !bcfg(s)->mcheck_mcsrr) || (iv == IV_DEBUG && !bcfg(s)->debug_dsrr);
    if (iv == IV_MC && bcfg(s)->mcheck_mcsrr) {
        b->mcsrr0 = srr0;
        b->mcsrr1 = msr;
    } else if (iv == IV_DEBUG && bcfg(s)->debug_dsrr) {
        b->dsrr0 = srr0;
        b->dsrr1 = msr;
    } else if (crit_pair) {
        b->csrr0 = srr0;
        b->csrr1 = msr;
    } else {
        s->s.srr0 = srr0;
        s->s.srr1 = msr;
    }
    if (iv == IV_MC)
        keep = 0;
    else if (iv == IV_CI || iv == IV_WDT || iv == IV_DEBUG)
        keep = MSR_ME;
    else
        keep = MSR_ME | MSR_CE | MSR_DE;
    s->s.trace_pending = false;
    cpu->resv.valid = false;
    cpu->pc = (b->ivpr & 0xFFFF0000u) | (b->ivor[iv] & 0xFFF0u);
    ppc_booke_msr_write(cpu, msr & keep);
}

static void program(uam_cpu *cpu, uint32_t pc, uint32_t esr)
{
    ppc_st(cpu)->bk.esr = esr;
    enter(cpu, IV_PROG, pc);
}

static void machine_check(uam_cpu *cpu, uint32_t pc)
{
    if (ppc_st(cpu)->s.msr & MSR_ME)
        enter(cpu, IV_MC, pc);
    else
        ppc_checkstop(cpu, pc);
}

/* A data access fault: TLB error (MAS loaded) or data storage interrupt. */
static void data_fault(uam_cpu *cpu, uint32_t pc, uint32_t ea, uint32_t xf, bool store)
{
    ppc_state *s = ppc_st(cpu);
    s->bk.dear = ea;
    s->bk.esr = store ? ESR_ST : 0;
    if (xf & PPC_XF_TLBMISS) {
        mas_miss(s, ea, false);
        enter(cpu, IV_DTLB, pc);
    } else {
        enter(cpu, IV_DSI, pc);
    }
}

/* tw/twi with debug trap events enabled: a debug interrupt instead of a program one. */
static bool trap_debug(const ppc_state *s)
{
    return (s->s.msr & MSR_DE) && (s->bk.dbcr0 & DBCR0_IDM) && (s->bk.dbcr0 & DBCR0_TRAP);
}

void ppc_booke_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t pc = (uint32_t)e->pc, va = (uint32_t)e->tval;
    bool store = e->kind == UAM_EXC_STORE_XLATE || e->kind == UAM_EXC_STORE_MISALIGNED;
    switch (e->kind) {
    case UAM_EXC_TRAP:
        switch (e->cause) {
        case PPC_TRAP_SC:   enter(cpu, IV_SC, pc + 4); break;
        case PPC_TRAP_FPU:  enter(cpu, IV_FPU, pc); break;
        case PPC_TRAP_PRIV: program(cpu, pc, ESR_PPR); break;
        case PPC_TRAP_TRAP:
            if (trap_debug(s)) {
                s->bk.dbsr |= DBSR_TRAP;
                enter(cpu, IV_DEBUG, pc);
            } else {
                program(cpu, pc, ESR_PTR);
            }
            break;
        default:            program(cpu, pc, ESR_PIL); break;
        }
        break;
    case UAM_EXC_FETCH_XLATE:
        if (e->cause & PPC_XF_TLBMISS) {
            mas_miss(s, pc, true);
            enter(cpu, IV_ITLB, pc);
        } else {
            s->bk.esr = 0;
            enter(cpu, IV_ISI, pc);
        }
        break;
    case UAM_EXC_LOAD_XLATE:
    case UAM_EXC_STORE_XLATE:
        data_fault(cpu, pc, va, (uint32_t)e->cause, store);
        break;
    case UAM_EXC_LOAD_MISALIGNED:
    case UAM_EXC_STORE_MISALIGNED:
        s->bk.dear = va;
        s->bk.esr = store ? ESR_ST : 0;
        enter(cpu, IV_ALIGN, pc);
        break;
    case UAM_EXC_FETCH_FAULT:
    case UAM_EXC_LOAD_FAULT:
    case UAM_EXC_STORE_FAULT:
        machine_check(cpu, pc);
        break;
    case UAM_EXC_HELPER: {
        unsigned hf = s->hfault;
        s->hfault = PPC_HF_NONE;
        switch (hf) {
        case PPC_HF_FP_ENABLED: program(cpu, pc, ESR_FP); break;
        case PPC_HF_PRIV:       program(cpu, pc, ESR_PPR); break;
        case PPC_HF_DSI:
            data_fault(cpu, pc, s->hf_ea, s->hf_dsisr, s->hf_dsisr & DSISR_STORE);
            break;
        case PPC_HF_ALIGN:
            s->bk.dear = s->hf_ea;
            s->bk.esr = 0;
            enter(cpu, IV_ALIGN, pc);
            break;
        case PPC_HF_MCHECK:     machine_check(cpu, pc); break;
        default:                program(cpu, pc, ESR_PIL); break;
        }
        break;
    }
    default:   /* UAM_EXC_SYSREG: an SPR the processor does not have */
        program(cpu, pc, ESR_PIL);
        break;
    }
}

static bool wdt_int(const ppc_state *s) { return (s->bk.tsr & TSR_WIS) && (s->bk.tcr & TCR_WIE); }
static bool fit_int(const ppc_state *s) { return (s->bk.tsr & TSR_FIS) && (s->bk.tcr & TCR_FIE); }
static bool dec_int(const ppc_state *s) { return (s->bk.tsr & TSR_DIS) && (s->bk.tcr & TCR_DIE); }

bool ppc_booke_irq_take(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t pc = (uint32_t)cpu->pc, msr = s->s.msr;
    if (s->s.checkstop)
        return false;
    timers_sync(cpu);
    if (s->s.mcp_pending) {
        s->s.mcp_pending = false;
        s->taken_lines = UINT64_C(1) << UAM_PPC_IRQ_MCP;
        machine_check(cpu, pc);
        return !s->s.checkstop;
    }
    if (s->s.trace_pending && (msr & MSR_DE)) {
        s->bk.dbsr |= (s->bk.dbcr0 & DBCR0_ICMP) ? DBSR_ICMP : DBSR_BRT;
        s->taken_lines = 0;
        enter(cpu, IV_DEBUG, pc);
        return true;
    }
    s->s.trace_pending = false;
    if (msr & MSR_CE) {
        if (s->cint_level) {
            s->taken_lines = UINT64_C(1) << UAM_PPC_IRQ_CINT;
            enter(cpu, IV_CI, pc);
            return true;
        }
        if (wdt_int(s)) {
            s->taken_lines = 0;
            enter(cpu, IV_WDT, pc);
            return true;
        }
    }
    if (msr & MSR_EE) {
        unsigned iv = s->intlines ? IV_EXT : fit_int(s) ? IV_FIT : dec_int(s) ? IV_DEC : 0;
        if (iv) {
            s->taken_lines = iv == IV_EXT ? s->intlines : 0;
            enter(cpu, iv, pc);
            return true;
        }
    }
    return false;
}

/* Waiting (MSR[WE]) ends by taking an interrupt. */
bool ppc_booke_irq_wakeup(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    if (s->s.checkstop)
        return false;
    timers_sync(cpu);
    uint32_t msr = s->s.msr;
    return s->s.mcp_pending
        || ((msr & MSR_CE) && (s->cint_level || wdt_int(s)))
        || ((msr & MSR_EE) && (s->intlines || fit_int(s) || dec_int(s)));
}

/* ------------------------------------------------------------------------ */
/* Return from interrupt                                                     */
/* ------------------------------------------------------------------------ */

static uint64_t rf(uam_cpu *cpu, uint32_t xsrr0, uint32_t xsrr1)
{
    ppc_booke_msr_write(cpu, xsrr1);
    return xsrr0 & ~3u;
}

uint64_t ppc_helper_rfci(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    ppc_booke_regs *k = &ppc_st(env)->bk;
    return rf(env, k->csrr0, k->csrr1);
}

uint64_t ppc_helper_rfmci(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    ppc_booke_regs *k = &ppc_st(env)->bk;
    return rf(env, k->mcsrr0, k->mcsrr1);
}

uint64_t ppc_helper_rfdi(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    ppc_booke_regs *k = &ppc_st(env)->bk;
    return rf(env, k->dsrr0, k->dsrr1);
}

/* ------------------------------------------------------------------------ */
/* Special purpose registers                                                 */
/* ------------------------------------------------------------------------ */

static uint32_t tlbcfg(const ppc_state *s, unsigned a)
{
    const uam_ppc_tlb_geom *g = &bcfg(s)->tlb[a];
    if (!g->entries)
        return 0;
    unsigned lo = 32, hi = 12;
    for (unsigned n = 12; n <= 32; n++)
        if (g->page_sizes & (UINT64_C(1) << n)) {
            if (n < lo) lo = n;
            hi = n;
        }
    uint32_t assoc = g->ways >= g->entries ? g->entries : g->ways;
    return (assoc & 0xFF) << 24 | (log2_to_tsize(s, lo) & 0xF) << 20 | (log2_to_tsize(s, hi) & 0xF) << 16
         | (g->iprot ? 0x8000u : 0) | (g->page_sizes & (g->page_sizes - 1) ? 0x4000u : 0)
         | (g->entries & 0xFFF);
}

static uint32_t mmucfg(const ppc_state *s)
{
    unsigned ntlbs = bcfg(s)->tlb[1].entries ? 1 : 0;
    /* RASIZE 36, NPIDS, PIDSIZE 14 bits - 1, NTLBS - 1, MAVN */
    return 36u << 17 | (uint32_t)(bcfg(s)->pids & 0xF) << 11 | 13u << 6 | ntlbs << 2
         | (bcfg(s)->tsize_pow2 ? 1 : 0);
}

/* UAM_ERR_INVALID: not a Book E register (the caller handles it). */
uam_status ppc_booke_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    ppc_state *s = ppc_st(cpu);
    ppc_booke_regs *b = &s->bk;
    const uam_ppc_booke_config *c = bcfg(s);
    if (id >= SPR_IVOR0 && id <= SPR_IVOR15) {
        *val = b->ivor[id - SPR_IVOR0];
        return UAM_OK;
    }
    if (id >= SPR_MAS0 && id <= SPR_MAS6 && id != SPR_MAS0 + 5) {
        *val = b->mas[id - SPR_MAS0];
        return UAM_OK;
    }
    if (id >= SPR_SPRG4_R && id <= SPR_SPRG7_R) {
        *val = b->sprg47[id - SPR_SPRG4_R];
        return UAM_OK;
    }
    if (id >= SPR_SPRG4_W && id <= SPR_SPRG7_W) {
        *val = b->sprg47[id - SPR_SPRG4_W];
        return UAM_OK;
    }
    switch (id) {
    case SPR_CSRR0:   *val = b->csrr0; break;
    case SPR_CSRR1:   *val = b->csrr1; break;
    case SPR_DEAR:    *val = b->dear; break;
    case SPR_ESR:     *val = b->esr; break;
    case SPR_IVPR:    *val = b->ivpr; break;
    case SPR_PID0:    *val = b->pid[0]; break;
    case SPR_PID1:    if (c->pids < 3) return UAM_ERR_INVALID; *val = b->pid[1]; break;
    case SPR_PID2:    if (c->pids < 3) return UAM_ERR_INVALID; *val = b->pid[2]; break;
    case SPR_MCSRR0:  if (!c->mcheck_mcsrr) return UAM_ERR_INVALID; *val = b->mcsrr0; break;
    case SPR_MCSRR1:  if (!c->mcheck_mcsrr) return UAM_ERR_INVALID; *val = b->mcsrr1; break;
    case SPR_MCSR:    *val = b->mcsr; break;
    case SPR_MCAR:    *val = b->mcar; break;
    case SPR_DSRR0:   if (!c->debug_dsrr) return UAM_ERR_INVALID; *val = b->dsrr0; break;
    case SPR_DSRR1:   if (!c->debug_dsrr) return UAM_ERR_INVALID; *val = b->dsrr1; break;
    case SPR_DEC:     *val = dec_value(cpu); break;
    case SPR_DECAR:   *val = b->decar; break;
    case SPR_TCR:     *val = b->tcr; break;
    case SPR_TSR:     timers_sync(cpu); *val = b->tsr; break;
    case SPR_DBCR0:   *val = b->dbcr0; break;
    case SPR_DBSR:    *val = b->dbsr; break;
    case SPR_USPRG0:  *val = b->usprg0; break;
    case SPR_PIR_BOOKE: *val = cpu->hartid; break;
    case SPR_MAS7:    *val = b->mas[7]; break;
    case SPR_MMUCSR0: *val = 0; break;
    case SPR_MMUCFG:  *val = mmucfg(s); break;
    case SPR_TLB0CFG: *val = tlbcfg(s, 0); break;
    case SPR_TLB1CFG: *val = tlbcfg(s, 1); break;
    case SPR_TBL_W: case SPR_TBU_W: case SPR_TBL_R: case SPR_TBU_R:
        *val = id == SPR_TBL_W || id == SPR_TBL_R ? (uint32_t)ppc_tb_now(cpu)
                                                  : (uint32_t)(ppc_tb_now(cpu) >> 32);
        break;
    default:
        return UAM_ERR_INVALID;
    }
    return UAM_OK;
}

uam_status ppc_booke_sysreg_write(uam_cpu *cpu, uint32_t id, uint32_t v)
{
    ppc_state *s = ppc_st(cpu);
    ppc_booke_regs *b = &s->bk;
    const uam_ppc_booke_config *c = bcfg(s);
    if (id >= SPR_IVOR0 && id <= SPR_IVOR15) {
        b->ivor[id - SPR_IVOR0] = v & 0xFFF0u;
        return UAM_OK;
    }
    if (id >= SPR_MAS0 && id <= SPR_MAS6 && id != SPR_MAS0 + 5) {
        b->mas[id - SPR_MAS0] = v;
        return UAM_OK;
    }
    if (id >= SPR_SPRG4_W && id <= SPR_SPRG7_W) {
        b->sprg47[id - SPR_SPRG4_W] = v;
        return UAM_OK;
    }
    if (id >= SPR_SPRG4_R && id <= SPR_SPRG7_R)
        return UAM_ERR_ILLEGAL;   /* the user view is read only */
    switch (id) {
    case SPR_CSRR0:   b->csrr0 = v; break;
    case SPR_CSRR1:   b->csrr1 = v; break;
    case SPR_DEAR:    b->dear = v; break;
    case SPR_ESR:     b->esr = v; break;
    case SPR_IVPR:    b->ivpr = v & 0xFFFF0000u; break;
    case SPR_PID0: case SPR_PID1: case SPR_PID2: {
        unsigned n = id == SPR_PID0 ? 0 : id == SPR_PID1 ? 1 : 2;
        if (n >= c->pids)
            return UAM_ERR_INVALID;
        b->pid[n] = v & 0x3FFF;
        tlb_changed(cpu);
        break;
    }
    case SPR_MCSRR0:  if (!c->mcheck_mcsrr) return UAM_ERR_INVALID; b->mcsrr0 = v; break;
    case SPR_MCSRR1:  if (!c->mcheck_mcsrr) return UAM_ERR_INVALID; b->mcsrr1 = v; break;
    case SPR_MCSR:    b->mcsr &= ~v; break;            /* write 1 to clear */
    case SPR_MCAR:    b->mcar = v; break;
    case SPR_DSRR0:   if (!c->debug_dsrr) return UAM_ERR_INVALID; b->dsrr0 = v; break;
    case SPR_DSRR1:   if (!c->debug_dsrr) return UAM_ERR_INVALID; b->dsrr1 = v; break;
    case SPR_DEC:
        timers_sync(cpu);
        s->s.dec = v;
        s->s.dec_ticks = cpu->sched ? ppc_ticks_now(cpu) : 0;
        timers_arm(cpu);
        break;
    case SPR_DECAR:
        timers_sync(cpu);
        b->decar = v;
        timers_arm(cpu);
        break;
    case SPR_TCR: {
        timers_sync(cpu);
        uint32_t wrc = (b->tcr >> TCR_WRC_SHIFT) & 3;   /* once set, until reset */
        b->tcr = v;
        if (wrc)
            b->tcr = (b->tcr & ~(3u << TCR_WRC_SHIFT)) | wrc << TCR_WRC_SHIFT;
        timers_arm(cpu);
        break;
    }
    case SPR_TSR:
        timers_sync(cpu);
        b->tsr &= ~(v & (TSR_ENW | TSR_WIS | TSR_DIS | TSR_FIS));
        timers_arm(cpu);
        break;
    case SPR_TBL_W: case SPR_TBU_W: {
        timers_sync(cpu);
        uint64_t tb = ppc_tb_now(cpu);
        tb = id == SPR_TBL_W ? (tb & ~UINT64_C(0xFFFFFFFF)) | v
                             : (tb & UINT64_C(0xFFFFFFFF)) | (uint64_t)v << 32;
        ppc_tb_set(cpu, tb);
        b->fit_tb = b->wdt_tb = tb;
        timers_arm(cpu);
        break;
    }
    case SPR_TBL_R: case SPR_TBU_R:
        return UAM_ERR_ILLEGAL;
    case SPR_DBCR0:
        b->dbcr0 = v;
        cpu->priv = ppc_booke_priv(s);
        uam_cpu_request_exit(cpu);
        break;
    case SPR_DBSR:    b->dbsr &= ~v; break;            /* write 1 to clear */
    case SPR_USPRG0:  b->usprg0 = v; break;
    case SPR_PIR_BOOKE: break;                        /* read only */
    case SPR_MAS7:    b->mas[7] = v; break;
    case SPR_MMUCSR0: /* flash invalidation: bit 29 TLB0, bit 30 TLB1 */
        if (v & 0x4)
            invalidate(cpu, 0, true, 0, false);
        if (v & 0x2)
            invalidate(cpu, 1, true, 0, false);
        break;
    case SPR_MMUCFG: case SPR_TLB0CFG: case SPR_TLB1CFG:
        break;                                        /* read only */
    default:
        return UAM_ERR_INVALID;
    }
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Reset, snapshots, dump                                                    */
/* ------------------------------------------------------------------------ */

void ppc_booke_reset(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    if (cpu->sched) {
        uam_timer_cancel(cpu->sched, &s->fit_timer);
        uam_timer_cancel(cpu->sched, &s->wdt_timer);
    }
    memset(&s->bk, 0, sizeof(s->bk));
    memset(s->tlb, 0, sizeof(s->tlb));
    s->bk.tsr = (uint32_t)s->wrs_latched << TSR_WRS_SHIFT;
    const uam_ppc_tlbe *boot = &bcfg(s)->boot;
    if (boot->valid) {
        unsigned a = bcfg(s)->tlb[1].entries ? 1 : 0, k = 0;
        if (a == 0)
            mas_index(s, 0, 0, boot->epn, &k);
        tlbe_set(cpu, a, k, boot);
    }
    s->s.dec = 0;   /* stopped */
    s->s.dec_ticks = 0;
    if (cpu->sched)
        s->bk.fit_tb = s->bk.wdt_tb = ppc_tb_now(cpu);
}

uam_status uam_ppc_set_dcr_ops(uam_cpu *cpu, const uam_ppc_dcr_ops *ops, void *ctx)
{
    if (!cpu || !uam_ppc_is(cpu->isa) || !cpu->isa_state)
        return UAM_ERR_INVALID;
    ppc_st(cpu)->dcr_ops = ops;
    ppc_st(cpu)->dcr_ctx = ctx;
    return UAM_OK;
}

uam_status uam_ppc_set_tlb_ops(uam_cpu *cpu, const uam_ppc_tlb_ops *ops, void *ctx)
{
    if (!cpu || !uam_ppc_is(cpu->isa) || !cpu->isa_state)
        return UAM_ERR_INVALID;
    ppc_st(cpu)->tlb_ops = ops;
    ppc_st(cpu)->tlb_ctx = ctx;
    return UAM_OK;
}

uam_status uam_ppc_set_reset_sink(uam_cpu *cpu, uam_ppc_reset_sink fn, void *ctx)
{
    if (!cpu || !uam_ppc_is(cpu->isa) || !cpu->isa_state)
        return UAM_ERR_INVALID;
    ppc_st(cpu)->reset_sink = fn;
    ppc_st(cpu)->reset_ctx = ctx;
    return UAM_OK;
}

static bool tlb_slot(uam_cpu *cpu, unsigned a, unsigned k)
{
    if (!cpu || !uam_ppc_is(cpu->isa) || !cpu->isa_state || a > 1)
        return false;
    const ppc_state *s = ppc_st(cpu);
    return ppc_booke(s) && k < bcfg(s)->tlb[a].entries;
}

uam_status uam_ppc_tlb_read(uam_cpu *cpu, unsigned array, unsigned index, uam_ppc_tlbe *out)
{
    if (!tlb_slot(cpu, array, index))
        return UAM_ERR_INVALID;
    *out = ppc_st(cpu)->tlb[array][index];
    return UAM_OK;
}

uam_status uam_ppc_tlb_write(uam_cpu *cpu, unsigned array, unsigned index, const uam_ppc_tlbe *e)
{
    if (!tlb_slot(cpu, array, index) || !e)
        return UAM_ERR_INVALID;
    tlbe_set(cpu, array, index, e);
    return UAM_OK;
}

uam_status uam_ppc_tlb_find(uam_cpu *cpu, uint32_t ea, unsigned ts, uint32_t pid,
                            unsigned *array, unsigned *index)
{
    if (!tlb_slot(cpu, 0, 0) && !tlb_slot(cpu, 1, 0))
        return UAM_ERR_INVALID;
    return lookup(ppc_st(cpu), ea, ts & 1, &pid, 1, array, index) ? UAM_OK : UAM_ERR_XLATE;
}

uint32_t uam_ppc_gpr(uam_cpu *cpu, unsigned n) { return (uint32_t)uam_cpu_reg_read(cpu, PPC_GPR_OFF(n & 31), 32); }
void uam_ppc_set_gpr(uam_cpu *cpu, unsigned n, uint32_t v) { uam_cpu_reg_write(cpu, PPC_GPR_OFF(n & 31), 32, v); }
void uam_ppc_set_cr_field(uam_cpu *cpu, unsigned field, unsigned v)
{
    uam_cpu_reg_write(cpu, PPC_CR_OFF(field & 7), 8, v & 0xF);
}

static void put_tlbe(uam_state_writer *w, const uam_ppc_tlbe *e)
{
    uam_state_put_bool(w, e->valid);
    uam_state_put_bool(w, e->iprot);
    uam_state_put_u8(w, e->ts);
    uam_state_put_u8(w, e->size_log2);
    uam_state_put_u32(w, e->tid);
    uam_state_put_u32(w, e->epn);
    uam_state_put_u64(w, e->rpn);
    uam_state_put_u8(w, e->wimge);
    uam_state_put_u8(w, e->perm);
    uam_state_put_u8(w, e->user);
}

static void get_tlbe(uam_state_reader *r, uam_ppc_tlbe *e)
{
    e->valid = uam_state_get_bool(r);
    e->iprot = uam_state_get_bool(r);
    e->ts = uam_state_get_u8(r);
    e->size_log2 = uam_state_get_u8(r);
    e->tid = uam_state_get_u32(r);
    e->epn = uam_state_get_u32(r);
    e->rpn = uam_state_get_u64(r);
    e->wimge = uam_state_get_u8(r);
    e->perm = uam_state_get_u8(r);
    e->user = uam_state_get_u8(r);
}

#define BK_REGS(b) { &(b)->csrr0, &(b)->csrr1, &(b)->mcsrr0, &(b)->mcsrr1, &(b)->dsrr0, &(b)->dsrr1, \
    &(b)->dear, &(b)->esr, &(b)->ivpr, &(b)->mcsr, &(b)->mcar, &(b)->pid[0], &(b)->pid[1],        \
    &(b)->pid[2], &(b)->tcr, &(b)->tsr, &(b)->decar, &(b)->dbcr0, &(b)->dbsr, &(b)->sprg47[0],     \
    &(b)->sprg47[1], &(b)->sprg47[2], &(b)->sprg47[3], &(b)->usprg0, &(b)->victim[0], &(b)->victim[1] }

void ppc_booke_save(uam_cpu *cpu, uam_state_writer *w)
{
    ppc_state *s = ppc_st(cpu);
    ppc_booke_regs *b = &s->bk;
    uint32_t *const regs[] = BK_REGS(b);
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        uam_state_put_u32(w, *regs[i]);
    for (unsigned i = 0; i < 16; i++)
        uam_state_put_u32(w, b->ivor[i]);
    for (unsigned i = 0; i < 8; i++)
        uam_state_put_u32(w, b->mas[i]);
    uam_state_put_u64(w, b->fit_tb);
    uam_state_put_u64(w, b->wdt_tb);
    uam_state_put_bool(w, s->cint_level);
    uam_state_put_u8(w, s->wrs_latched);
    for (unsigned a = 0; a < 2; a++) {
        unsigned n = ppc_booke(s) ? bcfg(s)->tlb[a].entries : 0;
        uam_state_put_u32(w, n);
        for (unsigned k = 0; k < n; k++)
            put_tlbe(w, &s->tlb[a][k]);
    }
    uam_state_put_timer(w, &s->fit_timer);
    uam_state_put_timer(w, &s->wdt_timer);
}

void ppc_booke_restore(uam_cpu *cpu, uam_state_reader *r)
{
    ppc_state *s = ppc_st(cpu);
    ppc_booke_regs *b = &s->bk;
    uint32_t *const regs[] = BK_REGS(b);
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        *regs[i] = uam_state_get_u32(r);
    for (unsigned i = 0; i < 16; i++)
        b->ivor[i] = uam_state_get_u32(r);
    for (unsigned i = 0; i < 8; i++)
        b->mas[i] = uam_state_get_u32(r);
    b->fit_tb = uam_state_get_u64(r);
    b->wdt_tb = uam_state_get_u64(r);
    s->cint_level = uam_state_get_bool(r);
    s->wrs_latched = uam_state_get_u8(r);
    for (unsigned a = 0; a < 2; a++) {
        unsigned n = ppc_booke(s) ? bcfg(s)->tlb[a].entries : 0;
        uam_state_expect_u32(r, n, "PowerPC TLB array size");
        for (unsigned k = 0; k < n; k++)
            get_tlbe(r, &s->tlb[a][k]);
    }
    uam_state_get_timer(r, &s->fit_timer);
    uam_state_get_timer(r, &s->wdt_timer);
}

void ppc_booke_dump(uam_cpu *cpu, FILE *out)
{
    ppc_state *s = ppc_st(cpu);
    const ppc_booke_regs *b = &s->bk;
    fprintf(out, "srr0=%08x srr1=%08x csrr0=%08x csrr1=%08x dear=%08x esr=%08x ivpr=%08x pid=%x\n",
            s->s.srr0, s->s.srr1, b->csrr0, b->csrr1, b->dear, b->esr, b->ivpr, b->pid[0]);
    fprintf(out, "tcr=%08x tsr=%08x dec=%08x tb=%016llx mas0=%08x mas1=%08x mas2=%08x mas3=%08x\n",
            b->tcr, b->tsr, s->s.dec, (unsigned long long)ppc_tb_now(cpu), b->mas[0], b->mas[1],
            b->mas[2], b->mas[3]);
}
