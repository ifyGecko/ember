/*
 * mcs51_system.c - The 8051 core's system side and the uam_isa descriptor.
 *
 * The CPU models, reset, the core SFRs that are not plain register-file bytes (PSW's
 * bank bits, the interrupt registers, PCON, the data-pointer select), the interrupt
 * system (priority levels, the in-progress levels RETI ends, SFR page saving), code
 * banking through isa->translate, the debugger view and snapshots.
 *
 * Not modelled: the one instruction the 8051 runs after RETI before the next interrupt
 * (it runs none here; after a write to IE or IP it does run one), power-down mode
 * (PCON.PD idles like PCON.IDL, until an interrupt), instruction timing (every
 * instruction is one tick of virtual time) and the SFR page stack's own registers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcs51.h"
#include "uam/bus.h"

/* ---- models -------------------------------------------------------------------- */

const uam_mcs51_config uam_mcs51_8051_ref = {
    .name = "mcs51-8051-ref", .iram_size = 128, .n_sources = 5, .xpage = UAM_MCS51_SFR_P2,
};
const uam_mcs51_config uam_mcs51_8052_ref = {
    .name = "mcs51-8052-ref", .iram_size = 256, .n_sources = 6, .iph = 0xB7, .dps = 0xA2,
    .xpage = UAM_MCS51_SFR_P2,
};
const uam_mcs51_config uam_mcs51_cip51_ref = {
    .name = "mcs51-cip51-ref", .iram_size = 256, .n_sources = 23,
    .eie = { 0xE6, 0xE7 }, .eip = { 0xF6, 0xF7 }, .xpage = 0xAA, .sfrpage = 0x84,
    .bank_from = 0x8000,
};

static bool sfr_ok(uint8_t a) { return a == 0 || a >= 0x80; }

uam_status uam_mcs51_configure(uam_cpu *cpu, const uam_mcs51_config *cfg)
{
    if (!cpu || !uam_mcs51_is(cpu->isa) || !cpu->isa_state || !cfg
        || (cfg->iram_size != 128 && cfg->iram_size != 256)
        || cfg->n_sources == 0 || cfg->n_sources > UAM_MCS51_SOURCES
        || !sfr_ok(cfg->iph) || !sfr_ok(cfg->dps) || !sfr_ok(cfg->dpl1) || !sfr_ok(cfg->xpage)
        || !sfr_ok(cfg->sfrpage) || (cfg->dpl1 && !cfg->dps)
        || (cfg->bank_from && cfg->bank_from % (0x10000u - cfg->bank_from)))
        return UAM_ERR_INVALID;
    for (unsigned k = 0; k < 3; k++)
        if (!sfr_ok(cfg->eie[k]) || !sfr_ok(cfg->eip[k]) || !sfr_ok(cfg->eiph[k]))
            return UAM_ERR_INVALID;
    mcs51_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

const uam_mcs51_config *uam_mcs51_config_of(uam_cpu *cpu)
{
    return cpu && uam_mcs51_is(cpu->isa) && cpu->isa_state ? &mcs51_st(cpu)->cfg : NULL;
}

void uam_mcs51_set_irq_ack(uam_cpu *cpu, uam_mcs51_irq_ack_fn fn, void *ctx)
{
    if (uam_mcs51_config_of(cpu)) {
        mcs51_st(cpu)->ack = fn;
        mcs51_st(cpu)->ack_ctx = ctx;
    }
}

void uam_mcs51_set_code_bank(uam_cpu *cpu, unsigned fetch_bank, unsigned movc_bank)
{
    if (!uam_mcs51_config_of(cpu))
        return;
    mcs51_state *s = mcs51_st(cpu);
    if (s->fetch_bank != fetch_bank || s->movc_bank != movc_bank) {
        s->fetch_bank = (uint16_t)fetch_bank;
        s->movc_bank = (uint16_t)movc_bank;
        uam_cpu_xlate_changed(cpu);
        uam_cpu_request_exit(cpu);
    }
}

void uam_mcs51_set_sfr_autopage(uam_cpu *cpu, bool on)
{
    if (uam_mcs51_config_of(cpu))
        mcs51_st(cpu)->autopage = on;
}

uam_status uam_mcs51_set_irq_sfrpage(uam_cpu *cpu, unsigned source, uint8_t page)
{
    if (!uam_mcs51_config_of(cpu) || source >= UAM_MCS51_SOURCES)
        return UAM_ERR_INVALID;
    mcs51_st(cpu)->irq_page[source] = page;
    return UAM_OK;
}

uint8_t uam_mcs51_iram_read(uam_cpu *cpu, unsigned addr)
{
    return uam_mcs51_config_of(cpu) && addr < mcs51_st(cpu)->cfg.iram_size ? cpu->regfile[MCS51_IRAM(addr)] : 0;
}

void uam_mcs51_iram_write(uam_cpu *cpu, unsigned addr, uint8_t v)
{
    if (uam_mcs51_config_of(cpu) && addr < mcs51_st(cpu)->cfg.iram_size)
        cpu->regfile[MCS51_IRAM(addr)] = v;
}

/* ---- registers ------------------------------------------------------------------ */

static const uam_regdesc mcs51_regs[] = {
    { "a", MCS51_A, 8, UAM_REG_GPR },     { "b", MCS51_B, 8, UAM_REG_GPR },
    { "cy", MCS51_CY, 8, UAM_REG_FLAG },  { "ac", MCS51_AC, 8, UAM_REG_FLAG },
    { "ov", MCS51_OV, 8, UAM_REG_FLAG },  { "pswx", MCS51_PSWX, 8, UAM_REG_FLAG },
    { "sp", MCS51_SP, 8, UAM_REG_OTHER }, { "dptr", MCS51_DPTR(0), 16, UAM_REG_GPR },
    { "dptr1", MCS51_DPTR(1), 16, UAM_REG_GPR }, { "sfrpage", MCS51_SFRPAGE, 8, UAM_REG_OTHER },
};

static uint8_t psw_read(uam_cpu *cpu)
{
    const uint8_t *r = cpu->regfile;
    return (uint8_t)((r[MCS51_CY] & 1) << 7 | (r[MCS51_AC] & 1) << 6 | (r[MCS51_PSWX] & PSWX_MASK)
                     | (r[MCS51_OV] & 1) << 2 | (__builtin_popcount(r[MCS51_A]) & 1));
}

/* The bank and data pointer translated code was made for. Writing them changes it. */
static void set_priv(uam_cpu *cpu)
{
    const mcs51_state *s = mcs51_st(cpu);
    const unsigned p = MCS51_PRIV((cpu->regfile[MCS51_PSWX] >> 3) & 3, s->cfg.dps ? s->dps & 1 : 0);
    if (p != cpu->priv) {
        cpu->priv = p;
        uam_cpu_request_exit(cpu);
    }
}

static void psw_write(uam_cpu *cpu, uint8_t v)
{
    uint8_t *r = cpu->regfile;
    r[MCS51_CY] = v >> 7 & 1;
    r[MCS51_AC] = v >> 6 & 1;
    r[MCS51_OV] = v >> 2 & 1;
    r[MCS51_PSWX] = v & PSWX_MASK;
    set_priv(cpu);
}

/* ---- the core SFRs behind SYSRD/SYSWR -------------------------------------------- */

static int ext_index(const uint8_t regs[3], uint32_t a)
{
    for (int k = 0; k < 3; k++)
        if (regs[k] && regs[k] == a)
            return k;
    return -1;
}

bool mcs51_sys_sfr(const uam_mcs51_config *c, uint32_t a)
{
    if (a == UAM_MCS51_SFR_IE || a == UAM_MCS51_SFR_IP || a == UAM_MCS51_SFR_PCON
        || a == UAM_MCS51_SFR_PSW)
        return true;
    if ((c->iph && a == c->iph) || (c->dps && a == c->dps))
        return true;
    return ext_index(c->eie, a) >= 0 || ext_index(c->eip, a) >= 0 || ext_index(c->eiph, a) >= 0;
}

static uint8_t sys_read(uam_cpu *cpu, uint32_t a)
{
    mcs51_state *s = mcs51_st(cpu);
    const uam_mcs51_config *c = &s->cfg;
    int k;
    switch (a) {
    case UAM_MCS51_SFR_IE:   return s->ie;
    case UAM_MCS51_SFR_IP:   return s->ip;
    case UAM_MCS51_SFR_PCON: return s->pcon;
    case UAM_MCS51_SFR_PSW:  return psw_read(cpu);
    default: break;
    }
    if (c->iph && a == c->iph)
        return s->iph;
    if (c->dps && a == c->dps)
        return s->dps;
    if ((k = ext_index(c->eie, a)) >= 0)
        return s->eie[k];
    if ((k = ext_index(c->eip, a)) >= 0)
        return s->eip[k];
    if ((k = ext_index(c->eiph, a)) >= 0)
        return s->eiph[k];
    return 0;
}

static void sys_write(uam_cpu *cpu, uint32_t a, uint8_t v)
{
    mcs51_state *s = mcs51_st(cpu);
    const uam_mcs51_config *c = &s->cfg;
    int k;
    switch (a) {
    case UAM_MCS51_SFR_PSW:
        cpu->regfile[MCS51_PSWX] = v & PSWX_MASK;   /* the lifter PUTs CY, AC and OV */
        set_priv(cpu);
        return;
    case UAM_MCS51_SFR_IE:   s->ie = v; uam_cpu_request_exit(cpu); return;
    case UAM_MCS51_SFR_IP:   s->ip = v; uam_cpu_request_exit(cpu); return;
    case UAM_MCS51_SFR_PCON:
        s->pcon = v & 0xFC;               /* IDL and PD read back 0 once the core wakes */
        if (v & 3)
            cpu->waiting = true;
        uam_cpu_request_exit(cpu);
        return;
    default: break;
    }
    if (c->iph && a == c->iph) {
        s->iph = v;
    } else if (c->dps && a == c->dps) {
        s->dps = v & 1;
        set_priv(cpu);
        return;
    } else if ((k = ext_index(c->eie, a)) >= 0) {
        s->eie[k] = v;
    } else if ((k = ext_index(c->eip, a)) >= 0) {
        s->eip[k] = v;
    } else if ((k = ext_index(c->eiph, a)) >= 0) {
        s->eiph[k] = v;
    }
    uam_cpu_request_exit(cpu);
}

static uam_status mcs51_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    if (id < 0x80 || id > 0xFF || !mcs51_sys_sfr(&mcs51_st(cpu)->cfg, id))
        return UAM_ERR_ILLEGAL;
    *val = sys_read(cpu, id);
    return UAM_OK;
}

static uam_status mcs51_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    if (id < 0x80 || id > 0xFF || !mcs51_sys_sfr(&mcs51_st(cpu)->cfg, id))
        return UAM_ERR_ILLEGAL;
    sys_write(cpu, id, (uint8_t)val);
    return UAM_OK;
}

/* ---- interrupts ------------------------------------------------------------------- */

/* Source n's bit in IE/IP (n < 7) or in the k-th extended register. */
static bool src_bit(uint8_t base, const uint8_t ext[3], const uint8_t ext_addr[3], unsigned n)
{
    if (n < 7)
        return (base >> n) & 1;
    const unsigned k = (n - 7) / 8;
    return k < 3 && ext_addr[k] && ((ext[k] >> ((n - 7) % 8)) & 1);
}

static unsigned src_level(const mcs51_state *s, unsigned n)
{
    const uam_mcs51_config *c = &s->cfg;
    unsigned l = src_bit(s->ip, s->eip, c->eip, n);
    if (c->iph)
        l |= (unsigned)src_bit(s->iph, s->eiph, c->eiph, n) << 1;
    return l;
}

/* The source to take now (or, ignoring EA and the levels in progress, the one that
 * would wake an idle core), or -1. */
static int pick(const mcs51_state *s, bool wake)
{
    const uam_mcs51_config *c = &s->cfg;
    uint32_t pend = s->lines;
    if (c->n_sources < 32)
        pend &= (UINT32_C(1) << c->n_sources) - 1;
    if (!pend || (!wake && !(s->ie & 0x80)))
        return -1;
    const unsigned running = s->in_progress ? 32u - (unsigned)__builtin_clz(s->in_progress) : 0;  /* highest + 1 */
    int best = -1;
    unsigned best_level = 0;
    for (unsigned n = 0; n < c->n_sources; n++) {
        if (!((pend >> n) & 1) || !src_bit(s->ie, s->eie, c->eie, n))
            continue;
        const unsigned l = src_level(s, n);
        if (wake)
            return (int)n;
        if (l + 1 <= running)
            continue;   /* that level, or a higher one, is being serviced */
        if (best < 0 || l > best_level) {
            best = (int)n;
            best_level = l;
        }
    }
    return best;
}

static void iram_push(uam_cpu *cpu, uint8_t v)
{
    const mcs51_state *s = mcs51_st(cpu);
    const uint8_t sp = (uint8_t)(cpu->regfile[MCS51_SP] + 1);
    cpu->regfile[MCS51_SP] = sp;
    if (sp < s->cfg.iram_size)
        cpu->regfile[MCS51_IRAM(sp)] = v;
}

static bool mcs51_irq_take(uam_cpu *cpu)
{
    mcs51_state *s = mcs51_st(cpu);
    if (s->halted)
        return false;
    const int n = pick(s, false);
    if (n < 0)
        return false;
    const uint16_t pc = (uint16_t)cpu->pc;
    iram_push(cpu, pc & 0xFF);
    iram_push(cpu, pc >> 8);
    s->in_progress |= (uint8_t)(1u << src_level(s, (unsigned)n));
    if (s->cfg.sfrpage && s->autopage) {
        if (s->page_depth < MCS51_PAGE_STACK)
            s->page_stack[s->page_depth++] = cpu->regfile[MCS51_SFRPAGE];
        cpu->regfile[MCS51_SFRPAGE] = s->irq_page[n];
    }
    cpu->pc = 0x0003u + 8u * (unsigned)n;
    cpu->waiting = false;
    s->last_source = (unsigned)n;
    if (s->ack)
        s->ack(s->ack_ctx, (unsigned)n);
    return true;
}

static bool mcs51_irq_wakeup(uam_cpu *cpu)
{
    const mcs51_state *s = mcs51_st(cpu);
    return !s->halted && pick(s, true) >= 0;
}

static void mcs51_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    mcs51_state *s = mcs51_st(cpu);
    if (line >= UAM_MCS51_SOURCES)
        return;
    if (level)
        s->lines |= UINT32_C(1) << line;
    else
        s->lines &= ~(UINT32_C(1) << line);
}

static uint64_t mcs51_irq_lines(uam_cpu *cpu)
{
    return UINT64_C(1) << mcs51_st(cpu)->last_source;
}

/* ---- helpers ---------------------------------------------------------------------- */

static uint64_t helper_iram_rd(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    return a < mcs51_st(cpu)->cfg.iram_size ? cpu->regfile[MCS51_IRAM(a)] : 0;
}

static uint64_t helper_iram_wr(void *env, uint64_t a, uint64_t v, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    uam_cpu *cpu = env;
    if (a < mcs51_st(cpu)->cfg.iram_size)
        cpu->regfile[MCS51_IRAM(a)] = (uint8_t)v;
    return 0;
}

/* RETI's effect besides the return: end the highest level in progress and restore the
 * SFR page the interrupt saved. */
static uint64_t helper_reti(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    mcs51_state *s = mcs51_st(cpu);
    if (s->in_progress)
        s->in_progress &= (uint8_t)~(1u << (31 - __builtin_clz(s->in_progress)));
    if (s->cfg.sfrpage && s->autopage && s->page_depth)
        cpu->regfile[MCS51_SFRPAGE] = s->page_stack[--s->page_depth];
    uam_cpu_request_exit(cpu);
    return 0;
}

#define RW (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE)
const uam_helper_desc mcs51_helpers[MCS51_HELPER__COUNT] = {
    [MCS51_HELPER_IRAM_RD] = { .name = "mcs51_iram_rd", .n_args = 1, .arg_width = { 32 },
                               .result_width = 8, .effect = UAM_HELPER_READS_STATE, .fn = helper_iram_rd },
    [MCS51_HELPER_IRAM_WR] = { .name = "mcs51_iram_wr", .n_args = 2, .arg_width = { 32, 8 },
                               .result_width = 0, .effect = RW, .fn = helper_iram_wr },
    [MCS51_HELPER_RETI]    = { .name = "mcs51_reti", .n_args = 0, .result_width = 0, .effect = RW,
                               .fn = helper_reti },
};
#undef RW

/* ---- exceptions, translation ------------------------------------------------------ */

/* An 8051 has no exceptions: a fetch outside program memory or an access nothing answers
 * (with the board's unmapped policy at "fault") halts the hart, with a message. */
static void mcs51_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    mcs51_state *s = mcs51_st(cpu);
    uam_log(UAM_LOG_ERROR, "mcs51", "pc=0x%04llx: %s at 0x%llx: core halted",
            (unsigned long long)e->pc,
            e->kind == UAM_EXC_FETCH_FAULT || e->kind == UAM_EXC_FETCH_XLATE ? "fetch fault"
            : e->kind == UAM_EXC_STORE_FAULT ? "store to nothing" : "load from nothing",
            (unsigned long long)e->tval);
    s->halted = true;
    cpu->pc = e->pc;
    cpu->waiting = true;
    uam_cpu_request_exit(cpu);
}

/* Bus addresses are the spaces' addresses (uam/isa/mcs51.h); only code banking maps:
 * the window from bank_from up shows the bank instruction fetch or MOVC selects. */
static uam_status mcs51_translate(uam_cpu *cpu, uint64_t va, uam_xlate_kind kind, uam_xlate *out)
{
    const mcs51_state *s = mcs51_st(cpu);
    const uint32_t from = s->cfg.bank_from;
    out->fault = 0;
    out->fixed = true;
    out->paddr = va;
    if (from && kind != UAM_XLATE_DEBUG && va < 0x10000 && va >= from) {
        const unsigned bank = kind == UAM_XLATE_FETCH ? s->fetch_bank : s->movc_bank;
        out->paddr = (uint64_t)bank * (0x10000u - from) + (va - from);
        out->fixed = false;
    }
    return UAM_OK;
}

/* ---- lifecycle -------------------------------------------------------------------- */

static uam_status mcs51_init(uam_cpu *cpu)
{
    mcs51_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    s->cfg = uam_mcs51_8051_ref;
    cpu->isa_state = s;
    return UAM_OK;
}

static void mcs51_fini(uam_cpu *cpu)
{
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

/* Reset: SP = 7, everything else the core owns 0 (the internal RAM is left as it was,
 * as on the chip; a fresh hart has it zeroed). */
static void mcs51_reset(uam_cpu *cpu, uint64_t entry)
{
    mcs51_state *s = mcs51_st(cpu);
    memset(cpu->regfile + MCS51_A, 0, cpu->regfile_size - MCS51_A);
    cpu->regfile[MCS51_SP] = 0x07;
    s->ie = s->ip = s->iph = s->pcon = s->dps = 0;
    memset(s->eie, 0, sizeof(s->eie));
    memset(s->eip, 0, sizeof(s->eip));
    memset(s->eiph, 0, sizeof(s->eiph));
    s->in_progress = 0;
    s->page_depth = 0;
    s->autopage = false;
    const uint32_t from = s->cfg.bank_from;
    s->fetch_bank = s->movc_bank = from ? (uint16_t)(from / (0x10000u - from)) : 0;
    s->last_source = 0;
    s->halted = false;
    cpu->priv = 0;
    cpu->pc = entry & 0xFFFF;
    cpu->waiting = false;
    cpu->resv.valid = false;
    uam_cpu_xlate_changed(cpu);
}

/* ---- debugger view: R0-R7 of the current bank, A, B, PSW, SP, DPTR, PC ---------------- */

enum { G_R0 = 0, G_A = 8, G_B, G_PSW, G_SP, G_DPTR, G_PC, G_N };
static const uint8_t mcs51_gdb_sizes[G_N] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2 };

#define G(n, num, bits) "<reg name=\"" n "\" bitsize=\"" #bits "\" regnum=\"" #num "\"/>"
static const char mcs51_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><feature name=\"org.uam.mcs51.core\">"
    G("r0", 0, 8) G("r1", 1, 8) G("r2", 2, 8) G("r3", 3, 8) G("r4", 4, 8) G("r5", 5, 8)
    G("r6", 6, 8) G("r7", 7, 8) G("a", 8, 8) G("b", 9, 8) G("psw", 10, 8) G("sp", 11, 8)
    G("dptr", 12, 16) G("pc", 13, 16)
    "</feature></target>";
#undef G

static uint32_t bank_base(uam_cpu *cpu) { return ((cpu->regfile[MCS51_PSWX] >> 3) & 3) * 8u; }
static uint32_t dptr_off(uam_cpu *cpu)
{
    const mcs51_state *s = mcs51_st(cpu);
    return MCS51_DPTR(s->cfg.dps && !s->cfg.dpl1 ? s->dps & 1 : 0);
}

static bool mcs51_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    switch (n) {
    case G_A:    *v = cpu->regfile[MCS51_A]; break;
    case G_B:    *v = cpu->regfile[MCS51_B]; break;
    case G_PSW:  *v = psw_read(cpu); break;
    case G_SP:   *v = cpu->regfile[MCS51_SP]; break;
    case G_DPTR: *v = uam_cpu_reg_read(cpu, dptr_off(cpu), 16); break;
    case G_PC:   *v = cpu->pc & 0xFFFF; break;
    default:
        if (n >= G_N)
            return false;
        *v = cpu->regfile[MCS51_IRAM(bank_base(cpu) + n)];
    }
    return true;
}

static bool mcs51_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    switch (n) {
    case G_A:    cpu->regfile[MCS51_A] = (uint8_t)v; break;
    case G_B:    cpu->regfile[MCS51_B] = (uint8_t)v; break;
    case G_PSW:  psw_write(cpu, (uint8_t)v); break;
    case G_SP:   cpu->regfile[MCS51_SP] = (uint8_t)v; break;
    case G_DPTR: uam_cpu_reg_write(cpu, dptr_off(cpu), 16, v); break;
    case G_PC:   cpu->pc = v & 0xFFFF; break;
    default:
        if (n >= G_N)
            return false;
        cpu->regfile[MCS51_IRAM(bank_base(cpu) + n)] = (uint8_t)v;
    }
    return true;
}

static const uam_gdb_arch mcs51_gdb = {
    .target_xml = mcs51_target_xml, .n_regs = G_N, .reg_bytes = 2, .reg_sizes = mcs51_gdb_sizes,
    .read_reg = mcs51_gdb_read, .write_reg = mcs51_gdb_write,
};

static void mcs51_dump_state(uam_cpu *cpu, FILE *out)
{
    mcs51_state *s = mcs51_st(cpu);
    const uint8_t *r = cpu->regfile;
    fprintf(out, "pc=%04llx a=%02x b=%02x psw=%02x sp=%02x dptr=%04llx dptr1=%04llx icount=%llu model=%s\n",
            (unsigned long long)cpu->pc, r[MCS51_A], r[MCS51_B], psw_read(cpu), r[MCS51_SP],
            (unsigned long long)uam_cpu_reg_read(cpu, MCS51_DPTR(0), 16),
            (unsigned long long)uam_cpu_reg_read(cpu, MCS51_DPTR(1), 16), (unsigned long long)cpu->icount,
            s->cfg.name ? s->cfg.name : "?");
    const uint32_t bb = bank_base(cpu);
    for (unsigned i = 0; i < 8; i++)
        fprintf(out, "r%u=%02x%s", i, r[MCS51_IRAM(bb + i)], i == 7 ? "" : " ");
    fprintf(out, " (bank %u)\nie=%02x ip=%02x iph=%02x pcon=%02x dps=%u sfrpage=%02x in_progress=%x pending=%08x%s\n",
            bb / 8, s->ie, s->ip, s->iph, s->pcon, s->dps, r[MCS51_SFRPAGE], s->in_progress, s->lines,
            s->halted ? " HALTED" : "");
}

/* ---- snapshots -------------------------------------------------------------------- */

static void mcs51_save(uam_cpu *cpu, uam_state_writer *w)
{
    const mcs51_state *s = mcs51_st(cpu);
    uam_state_put_u32(w, s->lines);
    const uint8_t b[] = { s->ie, s->ip, s->iph, s->pcon, s->dps, s->in_progress, s->page_depth, s->autopage };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_bytes(w, s->eie, sizeof(s->eie));
    uam_state_put_bytes(w, s->eip, sizeof(s->eip));
    uam_state_put_bytes(w, s->eiph, sizeof(s->eiph));
    uam_state_put_bytes(w, s->page_stack, sizeof(s->page_stack));
    uam_state_put_bytes(w, s->irq_page, sizeof(s->irq_page));
    uam_state_put_u32(w, s->fetch_bank);
    uam_state_put_u32(w, s->movc_bank);
    uam_state_put_u32(w, s->last_source);
    uam_state_put_bool(w, s->halted);
}

static void mcs51_restore(uam_cpu *cpu, uam_state_reader *r)
{
    mcs51_state *s = mcs51_st(cpu);
    s->lines = uam_state_get_u32(r);
    uint8_t b[8];
    uam_state_get_bytes(r, b, sizeof(b));
    s->ie = b[0]; s->ip = b[1]; s->iph = b[2]; s->pcon = b[3]; s->dps = b[4];
    s->in_progress = b[5]; s->page_depth = b[6] <= MCS51_PAGE_STACK ? b[6] : MCS51_PAGE_STACK;
    s->autopage = b[7] != 0;
    uam_state_get_bytes(r, s->eie, sizeof(s->eie));
    uam_state_get_bytes(r, s->eip, sizeof(s->eip));
    uam_state_get_bytes(r, s->eiph, sizeof(s->eiph));
    uam_state_get_bytes(r, s->page_stack, sizeof(s->page_stack));
    uam_state_get_bytes(r, s->irq_page, sizeof(s->irq_page));
    s->fetch_bank = (uint16_t)uam_state_get_u32(r);
    s->movc_bank = (uint16_t)uam_state_get_u32(r);
    s->last_source = uam_state_get_u32(r);
    s->halted = uam_state_get_bool(r);
}

const uam_isa uam_isa_mcs51 = {
    .name           = "mcs51",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,   /* the stack holds the pc low byte first; SDCC is little-endian */
    .regfile_size   = MCS51_REGFILE_SIZE,
    .regs           = mcs51_regs,
    .n_regs         = UAM_ARRAY_LEN(mcs51_regs),
    .helpers        = { mcs51_helpers, MCS51_HELPER__COUNT },
    .init           = mcs51_init,
    .fini           = mcs51_fini,
    .reset          = mcs51_reset,
    .lift           = mcs51_lift,
    .take_exception = mcs51_take_exception,
    .irq_take       = mcs51_irq_take,
    .irq_wakeup     = mcs51_irq_wakeup,
    .irq_input      = mcs51_irq_input,
    .sysreg_read    = mcs51_sysreg_read,
    .sysreg_write   = mcs51_sysreg_write,
    .translate      = mcs51_translate,
    .gdb            = &mcs51_gdb,
    .dump_state     = mcs51_dump_state,
    .save           = mcs51_save,
    .restore        = mcs51_restore,
    .irq_lines      = mcs51_irq_lines,
};
