/*
 * avr_system.c - The 8-bit AVR core's system side and the uam_isa descriptor.
 *
 * The CPU models, reset, the address translation that puts the data space on the bus
 * (uam/isa/avr.h), interrupt entry for every core family (SREG.I on AVRe/AVRe+/AVRrc;
 * the level-based interrupt controllers of AVRxm (PMIC) and AVRxt (CPUINT), which are
 * part of the core because RETI ends their levels), the core's own I/O registers, SPM
 * and DES, the debugger view and snapshots.
 *
 * Not modelled: the one instruction an AVRe core runs after RETI before the next
 * interrupt (it runs none here; after SEI it does run one), configuration change
 * protection (CCP accepts any value and protects nothing), the watchdog, fuses and
 * instruction timing (every instruction is one tick of virtual time).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avr.h"
#include "uam/bus.h"

/* ---- models -------------------------------------------------------------------- */

#define F_E (UAM_AVR_F_MUL | UAM_AVR_F_JMP | UAM_AVR_F_SPM | UAM_AVR_F_BREAK)
const uam_avr_config uam_avr_e_ref = {
    .name = "avr-e-ref", .core = UAM_AVR_E, .features = F_E,
    .flash_size = 32u << 10, .sp_reset = 0x08FF, .vector_size = 4,
};
const uam_avr_config uam_avr_eplus_ref = {
    .name = "avr-eplus-ref", .core = UAM_AVR_EPLUS, .features = F_E | UAM_AVR_F_ELPM,
    .flash_size = 256u << 10, .sp_reset = 0x21FF, .vector_size = 4,
};
const uam_avr_config uam_avr_xm_ref = {
    .name = "avr-xm-ref", .core = UAM_AVR_XM, .features = F_E | UAM_AVR_F_ELPM,
    .flash_size = 136u << 10, .sp_reset = 0x3FFF, .vector_size = 4, .boot_base = 0x20000,
};
const uam_avr_config uam_avr_xt_ref = {
    .name = "avr-xt-ref", .core = UAM_AVR_XT, .features = F_E,
    .flash_size = 48u << 10, .sp_reset = 0x3FFF, .vector_size = 4,
};
const uam_avr_config uam_avr_rc_ref = {
    .name = "avr-rc-ref", .core = UAM_AVR_RC, .features = UAM_AVR_F_BREAK,
    .flash_size = 4u << 10, .sp_reset = 0x013F, .vector_size = 2,
};
#undef F_E

uam_status uam_avr_configure(uam_cpu *cpu, const uam_avr_config *cfg)
{
    if (!cpu || !uam_avr_is(cpu->isa) || !cpu->isa_state || !cfg || cfg->core > UAM_AVR_RC
        || (cfg->vector_size != 2 && cfg->vector_size != 4) || cfg->flash_size < 2
        || cfg->flash_size > (8u << 20))
        return UAM_ERR_INVALID;
    avr_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

const uam_avr_config *uam_avr_config_of(uam_cpu *cpu)
{
    return cpu && uam_avr_is(cpu->isa) && cpu->isa_state ? &avr_st(cpu)->cfg : NULL;
}

uam_status uam_avr_set_irq_level(uam_cpu *cpu, unsigned vector, unsigned level)
{
    if (!uam_avr_config_of(cpu) || vector == 0 || vector >= AVR_VECTORS || level > UAM_AVR_LEVEL_NMI)
        return UAM_ERR_INVALID;
    avr_st(cpu)->level[vector] = (uint8_t)level;
    return UAM_OK;
}

void uam_avr_set_irq_ack(uam_cpu *cpu, uam_avr_irq_ack_fn fn, void *ctx)
{
    if (uam_avr_config_of(cpu)) {
        avr_st(cpu)->ack = fn;
        avr_st(cpu)->ack_ctx = ctx;
    }
}

void uam_avr_set_spm(uam_cpu *cpu, uam_avr_spm_fn fn, void *ctx)
{
    if (uam_avr_config_of(cpu)) {
        avr_st(cpu)->spm = fn;
        avr_st(cpu)->spm_ctx = ctx;
    }
}

/* ---- registers ------------------------------------------------------------------ */

#define R(i) { "r" #i, AVR_R(i), 8, UAM_REG_GPR }
#define F(n, b) { n, AVR_FLAG(b), 8, UAM_REG_FLAG }
static const uam_regdesc avr_regs[] = {
    R(0), R(1), R(2), R(3), R(4), R(5), R(6), R(7), R(8), R(9), R(10), R(11), R(12), R(13),
    R(14), R(15), R(16), R(17), R(18), R(19), R(20), R(21), R(22), R(23), R(24), R(25), R(26),
    R(27), R(28), R(29), R(30), R(31),
    F("c", AVR_SREG_C), F("z", AVR_SREG_Z), F("n", AVR_SREG_N), F("v", AVR_SREG_V),
    F("s", AVR_SREG_S), F("h", AVR_SREG_H), F("t", AVR_SREG_T), F("i", AVR_SREG_I),
    { "sp", AVR_SP, 16, UAM_REG_OTHER },
    { "rampd", AVR_RAMPD, 8, UAM_REG_OTHER }, { "rampx", AVR_RAMPX, 8, UAM_REG_OTHER },
    { "rampy", AVR_RAMPY, 8, UAM_REG_OTHER }, { "rampz", AVR_RAMPZ, 8, UAM_REG_OTHER },
    { "eind", AVR_EIND, 8, UAM_REG_OTHER },
};
#undef R
#undef F

static uint8_t sreg_read(uam_cpu *cpu)
{
    uint8_t v = 0;
    for (unsigned b = 0; b < 8; b++)
        v |= (uint8_t)((cpu->regfile[AVR_FLAG(b)] & 1) << b);
    return v;
}

static void sreg_write(uam_cpu *cpu, uint8_t v)
{
    for (unsigned b = 0; b < 8; b++)
        cpu->regfile[AVR_FLAG(b)] = (v >> b) & 1;
    if (v & 0x80)
        uam_cpu_request_exit(cpu);   /* an interrupt may be pending */
}

/* ---- the core's I/O registers ----------------------------------------------------- */

bool avr_core_addr(const uam_avr_config *c, uint32_t a)
{
    const uint32_t io = a - avr_io_offset(c->core);
    switch (c->core) {
    case UAM_AVR_E:     return a < 0x20 || (io >= AVR_IO_SPL && io <= AVR_IO_SREG);
    case UAM_AVR_EPLUS: return a < 0x20 || (io >= AVR_IO_RAMPZ && io <= AVR_IO_SREG);
    case UAM_AVR_XM:    return a == AVR_IO_CCP || (a >= AVR_IO_RAMPD && a <= AVR_IO_SREG)
                               || (a >= AVR_PMIC_STATUS && a <= AVR_PMIC_CTRL);
    case UAM_AVR_XT:    return a == AVR_IO_CCP || (a >= AVR_IO_SPL && a <= AVR_IO_SREG)
                               || (a >= AVR_CPUINT_CTRLA && a <= AVR_CPUINT_LVL1VEC);
    default:            return a >= AVR_IO_EIND && a <= AVR_IO_SREG;   /* RC: CCP at 0x3C */
    }
}

static uint8_t core_read(uam_cpu *cpu, uint32_t a)
{
    avr_state *s = avr_st(cpu);
    const uam_avr_core core = s->cfg.core;
    if (avr_regs_mapped(core) && a < 0x20)
        return cpu->regfile[AVR_R(a)];
    switch (a - avr_io_offset(core)) {
    case AVR_IO_SREG:  return sreg_read(cpu);
    case AVR_IO_SPL:   return cpu->regfile[AVR_SP];
    case AVR_IO_SPH:   return cpu->regfile[AVR_SP + 1];
    case AVR_IO_EIND:  return core == UAM_AVR_RC ? s->ccp : cpu->regfile[AVR_EIND];
    case AVR_IO_RAMPZ: return cpu->regfile[AVR_RAMPZ];
    case AVR_IO_RAMPY: return cpu->regfile[AVR_RAMPY];
    case AVR_IO_RAMPX: return cpu->regfile[AVR_RAMPX];
    case AVR_IO_RAMPD: return cpu->regfile[AVR_RAMPD];
    case AVR_IO_CCP:   return s->ccp;
    default: break;
    }
    if (core == UAM_AVR_XM) {
        switch (a) {
        case AVR_PMIC_STATUS: return s->status;
        case AVR_PMIC_INTPRI: return s->pri;
        case AVR_PMIC_CTRL:   return s->ctrl;
        default: break;
        }
    } else if (core == UAM_AVR_XT) {
        switch (a) {
        case AVR_CPUINT_CTRLA:   return s->ctrl;
        case AVR_CPUINT_STATUS:  return s->status;
        case AVR_CPUINT_LVL0PRI: return s->pri;
        case AVR_CPUINT_LVL1VEC: return s->lvl1vec;
        default: break;
        }
    }
    return 0;
}

static void core_write(uam_cpu *cpu, uint32_t a, uint8_t v)
{
    avr_state *s = avr_st(cpu);
    const uam_avr_core core = s->cfg.core;
    if (avr_regs_mapped(core) && a < 0x20) {
        cpu->regfile[AVR_R(a)] = v;
        return;
    }
    switch (a - avr_io_offset(core)) {
    case AVR_IO_SREG:  sreg_write(cpu, v); return;
    case AVR_IO_SPL:   cpu->regfile[AVR_SP] = v; return;
    case AVR_IO_SPH:   cpu->regfile[AVR_SP + 1] = v; return;
    case AVR_IO_EIND:
        if (core == UAM_AVR_RC) s->ccp = v; else cpu->regfile[AVR_EIND] = v;
        return;
    case AVR_IO_RAMPZ: cpu->regfile[AVR_RAMPZ] = v; return;
    case AVR_IO_RAMPY: cpu->regfile[AVR_RAMPY] = v; return;
    case AVR_IO_RAMPX: cpu->regfile[AVR_RAMPX] = v; return;
    case AVR_IO_RAMPD: cpu->regfile[AVR_RAMPD] = v; return;
    case AVR_IO_CCP:   s->ccp = v; return;
    default: break;
    }
    if (core == UAM_AVR_XM) {
        switch (a) {
        case AVR_PMIC_INTPRI: s->pri = v; return;
        case AVR_PMIC_CTRL:   s->ctrl = v & 0xC7; uam_cpu_request_exit(cpu); return;
        default: return;   /* STATUS is read-only */
        }
    } else if (core == UAM_AVR_XT) {
        switch (a) {
        case AVR_CPUINT_CTRLA:   s->ctrl = v & 0x61; uam_cpu_request_exit(cpu); return;
        case AVR_CPUINT_LVL0PRI: s->pri = v; return;
        case AVR_CPUINT_LVL1VEC: s->lvl1vec = v; uam_cpu_request_exit(cpu); return;
        default: return;
        }
    }
}

static uam_status avr_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    if (id == AVR_SYS_SET_I)
        *val = cpu->regfile[AVR_FLAG(AVR_SREG_I)] & 1;
    else if (id < 0x10000)
        *val = core_read(cpu, id);
    else
        return UAM_ERR_ILLEGAL;
    return UAM_OK;
}

static uam_status avr_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    if (id == AVR_SYS_SET_I) {
        cpu->regfile[AVR_FLAG(AVR_SREG_I)] = val & 1;
        if (val & 1)
            uam_cpu_request_exit(cpu);
    } else if (id < 0x10000) {
        core_write(cpu, id, (uint8_t)val);
    } else {
        return UAM_ERR_ILLEGAL;
    }
    return UAM_OK;
}

/* ---- interrupts ------------------------------------------------------------------- */

/* Interrupt-controller status bits: level n is bit n (AVRxm LO 0, MED 1, HI 2; AVRxt
 * LVL0 0, LVL1 1), NMI bit 7. */
static unsigned status_bit(unsigned level) { return level == UAM_AVR_LEVEL_NMI ? 0x80u : 1u << level; }

/* The level of vector v on a level-based core. AVRxm: 1..3 low/medium/high, kept as
 * 0..2 for the status bits; AVRxt: 0, or 1 for LVL1VEC. */
static unsigned vec_level(const avr_state *s, unsigned v)
{
    if (s->level[v] == UAM_AVR_LEVEL_NMI)
        return UAM_AVR_LEVEL_NMI;
    if (s->cfg.core == UAM_AVR_XM)
        return s->level[v] ? s->level[v] - 1u : 0u;
    return (s->lvl1vec && v == s->lvl1vec) || s->level[v] == 1 ? 1u : 0u;
}

/* May level `l` start now (enabled, and nothing at or above it running)? */
static bool level_ok(const avr_state *s, const uam_cpu *cpu, unsigned l, bool ignore_i)
{
    if (l == UAM_AVR_LEVEL_NMI)
        return !(s->status & 0x80);
    if (!ignore_i && !(cpu->regfile[AVR_FLAG(AVR_SREG_I)] & 1))
        return false;
    if (s->cfg.core == UAM_AVR_XM && !(s->ctrl & (1u << l)))
        return false;
    return (s->status & ~((1u << l) - 1u)) == 0;   /* no level >= l running (NMI is 0x80) */
}

/* The vector to take now, or 0. Levels in priority order; within the lowest level,
 * round robin after `pri` when enabled (AVRxm RREN, AVRxt LVL0RR; AVRxt's LVL0PRI also
 * sets the order without it). */
static unsigned pick(const avr_state *s, const uam_cpu *cpu, bool ignore_i)
{
    const uint64_t pend = s->lines & ~UINT64_C(1);
    if (!pend)
        return 0;
    if (!avr_levels(s->cfg.core)) {
        if (!ignore_i && !(cpu->regfile[AVR_FLAG(AVR_SREG_I)] & 1))
            return 0;
        return (unsigned)__builtin_ctzll(pend);
    }
    static const unsigned order[] = { UAM_AVR_LEVEL_NMI, 2, 1, 0 };
    const bool rr = s->cfg.core == UAM_AVR_XM ? (s->ctrl & 0x80) != 0 : (s->ctrl & 1) != 0;
    for (unsigned k = 0; k < UAM_ARRAY_LEN(order); k++) {
        const unsigned l = order[k];
        if (!level_ok(s, cpu, l, ignore_i))
            continue;
        const unsigned start = l == 0 && (rr || s->cfg.core == UAM_AVR_XT) ? s->pri + 1u : 1u;
        for (unsigned i = 0; i < AVR_VECTORS - 1; i++) {
            unsigned v = 1 + (start - 1 + i) % (AVR_VECTORS - 1);
            if ((pend >> v) & 1 && vec_level(s, v) == l)
                return v;
        }
    }
    return 0;
}

/* Push the pc as a return address (most significant byte at the lowest address). */
static void push_pc(uam_cpu *cpu, uint32_t pc)
{
    avr_state *s = avr_st(cpu);
    const uint32_t w = pc >> 1;
    const unsigned n = avr_pc22(&s->cfg) ? 3 : 2;
    uint16_t sp = (uint16_t)uam_cpu_reg_read(cpu, AVR_SP, 16);
    for (unsigned k = 0; k < n; k++, sp--)
        uam_bus_write(cpu->bus, UAM_AVR_DATA_BASE + sp, 1, UAM_ACC_LE, (w >> (8 * k)) & 0xFF);
    uam_cpu_reg_write(cpu, AVR_SP, 16, sp);
}

static bool avr_irq_take(uam_cpu *cpu)
{
    avr_state *s = avr_st(cpu);
    if (s->halted)
        return false;
    const unsigned v = pick(s, cpu, false);
    if (!v)
        return false;
    unsigned slot = v;
    uint32_t base = 0;
    if (avr_levels(s->cfg.core)) {
        const unsigned l = vec_level(s, v);
        s->status |= (uint8_t)status_bit(l);
        if (l == 0 && (s->cfg.core == UAM_AVR_XT || (s->ctrl & 0x80)))
            s->pri = (uint8_t)v;                       /* round robin: v is now lowest */
        if (s->ctrl & 0x40)
            base = s->cfg.boot_base;                   /* IVSEL */
        if (s->cfg.core == UAM_AVR_XT && (s->ctrl & 0x20))   /* compact vector table */
            slot = l == UAM_AVR_LEVEL_NMI ? 1 : l == 1 ? 2 : 3;
    } else {
        cpu->regfile[AVR_FLAG(AVR_SREG_I)] = 0;
    }
    push_pc(cpu, (uint32_t)cpu->pc);
    cpu->pc = base + slot * s->cfg.vector_size;
    s->last_vector = v;
    if (s->ack)
        s->ack(s->ack_ctx, v);
    return true;
}

static bool avr_irq_wakeup(uam_cpu *cpu)
{
    avr_state *s = avr_st(cpu);
    return !s->halted && pick(s, cpu, true) != 0;
}

static void avr_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    avr_state *s = avr_st(cpu);
    if (line == 0 || line >= AVR_VECTORS)
        return;
    if (level)
        s->lines |= UINT64_C(1) << line;
    else
        s->lines &= ~(UINT64_C(1) << line);
}

static uint64_t avr_irq_lines(uam_cpu *cpu)
{
    return UINT64_C(1) << avr_st(cpu)->last_vector;
}

/* ---- helpers ---------------------------------------------------------------------- */

static uint64_t helper_core_rd(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    return core_read(env, (uint32_t)a);
}

static uint64_t helper_core_wr(void *env, uint64_t a, uint64_t v, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    core_write(env, (uint32_t)a, (uint8_t)v);
    return 0;
}

/* RETI's effect besides the return: AVRe sets SREG.I; AVRxm/AVRxt end the highest
 * level running. */
static uint64_t helper_reti(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    avr_state *s = avr_st(cpu);
    if (avr_levels(s->cfg.core)) {
        for (int bitn = 7; bitn >= 0; bitn--)
            if (s->status & (1u << bitn)) {
                s->status &= (uint8_t)~(1u << bitn);
                break;
            }
    } else {
        cpu->regfile[AVR_FLAG(AVR_SREG_I)] = 1;
    }
    uam_cpu_request_exit(cpu);
    return 0;
}

static uint64_t helper_spm(void *env, uint64_t z, uint64_t word, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    avr_state *s = avr_st(env);
    if (s->spm)
        s->spm(s->spm_ctx, (uint32_t)z, (uint16_t)word);
    return 0;
}

static uint64_t helper_des(void *env, uint64_t k, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    avr_des_round(cpu->regfile, (unsigned)k, cpu->regfile[AVR_FLAG(AVR_SREG_H)] & 1);
    return 0;
}

#define RW (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE)
const uam_helper_desc avr_helpers[AVR_HELPER__COUNT] = {
    [AVR_HELPER_CORE_RD] = { .name = "avr_core_rd", .n_args = 1, .arg_width = { 32 },
                             .result_width = 8, .effect = UAM_HELPER_READS_STATE, .fn = helper_core_rd },
    [AVR_HELPER_CORE_WR] = { .name = "avr_core_wr", .n_args = 2, .arg_width = { 32, 8 },
                             .result_width = 0, .effect = RW, .fn = helper_core_wr },
    [AVR_HELPER_RETI]    = { .name = "avr_reti", .n_args = 0, .result_width = 0, .effect = RW,
                             .fn = helper_reti },
    [AVR_HELPER_SPM]     = { .name = "avr_spm", .n_args = 2, .arg_width = { 32, 32 },
                             .result_width = 0, .effect = RW, .fn = helper_spm },
    [AVR_HELPER_DES]     = { .name = "avr_des", .n_args = 1, .arg_width = { 8 },
                             .result_width = 0, .effect = RW, .fn = helper_des },
};
#undef RW

/* ---- exceptions, translation ------------------------------------------------------ */

/* An AVR has no exceptions: a fetch outside program memory or an access nothing answers
 * (with the board's unmapped policy at "fault") halts the hart, with a message. */
static void avr_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    avr_state *s = avr_st(cpu);
    uam_log(UAM_LOG_ERROR, "avr", "pc=0x%llx: %s at 0x%llx: core halted",
            (unsigned long long)e->pc,
            e->kind == UAM_EXC_FETCH_FAULT || e->kind == UAM_EXC_FETCH_XLATE ? "fetch fault"
            : e->kind == UAM_EXC_STORE_FAULT ? "store to nothing" : "load from nothing",
            (unsigned long long)e->tval);
    s->halted = true;
    cpu->pc = e->pc;
    cpu->waiting = true;
    uam_cpu_request_exit(cpu);
}

/* Fetches address program memory and loads/stores the data space, which is on the bus
 * at UAM_AVR_DATA_BASE; a debugger or loader uses bus addresses as they are. */
static uam_status avr_translate(uam_cpu *cpu, uint64_t va, uam_xlate_kind kind, uam_xlate *out)
{
    (void)cpu;
    out->fault = 0;
    out->fixed = true;
    out->paddr = (kind == UAM_XLATE_LOAD || kind == UAM_XLATE_STORE) ? UAM_AVR_DATA_BASE + (va & 0xFFFFFF) : va;
    return UAM_OK;
}

/* ---- lifecycle -------------------------------------------------------------------- */

static uam_status avr_init(uam_cpu *cpu)
{
    avr_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    s->cfg = uam_avr_eplus_ref;
    cpu->isa_state = s;
    return UAM_OK;
}

static void avr_fini(uam_cpu *cpu)
{
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

static void avr_reset(uam_cpu *cpu, uint64_t entry)
{
    avr_state *s = avr_st(cpu);
    memset(cpu->regfile, 0, cpu->regfile_size);
    uam_cpu_reg_write(cpu, AVR_SP, 16, s->cfg.sp_reset);
    s->status = s->pri = s->ctrl = s->lvl1vec = s->ccp = 0;   /* lines and levels are the board's */
    s->last_vector = 0;
    s->halted = false;
    cpu->priv = 0;
    cpu->pc = entry;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

/* ---- debugger view (avr-gdb: r0-r31, SREG, SP, PC as a byte address) ---------------- */

static const uint8_t avr_gdb_sizes[35] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 2, 4,
};

#define G(n, num, bits) "<reg name=\"" n "\" bitsize=\"" #bits "\" regnum=\"" #num "\"/>"
static const char avr_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>avr</architecture><feature name=\"org.gnu.gdb.avr.cpu\">"
    G("r0", 0, 8) G("r1", 1, 8) G("r2", 2, 8) G("r3", 3, 8) G("r4", 4, 8) G("r5", 5, 8)
    G("r6", 6, 8) G("r7", 7, 8) G("r8", 8, 8) G("r9", 9, 8) G("r10", 10, 8) G("r11", 11, 8)
    G("r12", 12, 8) G("r13", 13, 8) G("r14", 14, 8) G("r15", 15, 8) G("r16", 16, 8)
    G("r17", 17, 8) G("r18", 18, 8) G("r19", 19, 8) G("r20", 20, 8) G("r21", 21, 8)
    G("r22", 22, 8) G("r23", 23, 8) G("r24", 24, 8) G("r25", 25, 8) G("r26", 26, 8)
    G("r27", 27, 8) G("r28", 28, 8) G("r29", 29, 8) G("r30", 30, 8) G("r31", 31, 8)
    G("sreg", 32, 8) G("sp", 33, 16) G("pc", 34, 32)
    "</feature></target>";
#undef G

static bool avr_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n < 32)       *v = cpu->regfile[AVR_R(n)];
    else if (n == 32) *v = sreg_read(cpu);
    else if (n == 33) *v = uam_cpu_reg_read(cpu, AVR_SP, 16);
    else if (n == 34) *v = (uint32_t)cpu->pc;
    else return false;
    return true;
}

static bool avr_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n < 32)       cpu->regfile[AVR_R(n)] = (uint8_t)v;
    else if (n == 32) sreg_write(cpu, (uint8_t)v);
    else if (n == 33) uam_cpu_reg_write(cpu, AVR_SP, 16, v);
    else if (n == 34) cpu->pc = (uint32_t)v & ~UINT64_C(1);
    else return false;
    return true;
}

static const uam_gdb_arch avr_gdb = {
    .target_xml = avr_target_xml, .n_regs = 35, .reg_bytes = 4, .reg_sizes = avr_gdb_sizes,
    .read_reg = avr_gdb_read, .write_reg = avr_gdb_write,
};

static void avr_dump_state(uam_cpu *cpu, FILE *out)
{
    avr_state *s = avr_st(cpu);
    static const char flags[] = "CZNVSHTI";
    char f[9];
    for (unsigned b = 0; b < 8; b++)
        f[7 - b] = (cpu->regfile[AVR_FLAG(b)] & 1) ? flags[b] : '-';
    f[8] = 0;
    fprintf(out, "pc=%06llx (word %06llx) sreg=%s sp=%04llx icount=%llu model=%s\n",
            (unsigned long long)cpu->pc, (unsigned long long)cpu->pc >> 1, f,
            (unsigned long long)uam_cpu_reg_read(cpu, AVR_SP, 16), (unsigned long long)cpu->icount,
            s->cfg.name ? s->cfg.name : "?");
    for (unsigned i = 0; i < 32; i++)
        fprintf(out, "r%-2u=%02x%s", i, cpu->regfile[AVR_R(i)], (i % 8 == 7) ? "\n" : " ");
    fprintf(out, "rampd=%02x rampx=%02x rampy=%02x rampz=%02x eind=%02x", cpu->regfile[AVR_RAMPD],
            cpu->regfile[AVR_RAMPX], cpu->regfile[AVR_RAMPY], cpu->regfile[AVR_RAMPZ], cpu->regfile[AVR_EIND]);
    if (avr_levels(s->cfg.core))
        fprintf(out, " intc status=%02x pri=%02x ctrl=%02x", s->status, s->pri, s->ctrl);
    fprintf(out, " pending=%016llx%s\n", (unsigned long long)s->lines, s->halted ? " HALTED" : "");
}

/* ---- snapshots -------------------------------------------------------------------- */

static void avr_save(uam_cpu *cpu, uam_state_writer *w)
{
    const avr_state *s = avr_st(cpu);
    uam_state_put_u64(w, s->lines);
    uam_state_put_bytes(w, s->level, sizeof(s->level));
    const uint8_t b[] = { s->status, s->pri, s->ctrl, s->lvl1vec, s->ccp };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_u32(w, s->last_vector);
    uam_state_put_bool(w, s->halted);
}

static void avr_restore(uam_cpu *cpu, uam_state_reader *r)
{
    avr_state *s = avr_st(cpu);
    s->lines = uam_state_get_u64(r);
    uam_state_get_bytes(r, s->level, sizeof(s->level));
    uint8_t b[5];
    uam_state_get_bytes(r, b, sizeof(b));
    s->status = b[0]; s->pri = b[1]; s->ctrl = b[2]; s->lvl1vec = b[3]; s->ccp = b[4];
    s->last_vector = uam_state_get_u32(r);
    s->halted = uam_state_get_bool(r);
}

const uam_isa uam_isa_avr = {
    .name           = "avr",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,
    .regfile_size   = AVR_REGFILE_SIZE,
    .regs           = avr_regs,
    .n_regs         = UAM_ARRAY_LEN(avr_regs),
    .helpers        = { avr_helpers, AVR_HELPER__COUNT },
    .init           = avr_init,
    .fini           = avr_fini,
    .reset          = avr_reset,
    .lift           = avr_lift,
    .take_exception = avr_take_exception,
    .irq_take       = avr_irq_take,
    .irq_wakeup     = avr_irq_wakeup,
    .irq_input      = avr_irq_input,
    .sysreg_read    = avr_sysreg_read,
    .sysreg_write   = avr_sysreg_write,
    .translate      = avr_translate,
    .gdb            = &avr_gdb,
    .dump_state     = avr_dump_state,
    .save           = avr_save,
    .restore        = avr_restore,
    .irq_lines      = avr_irq_lines,
};
