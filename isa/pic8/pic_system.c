/*
 * pic_system.c - The 8-bit PIC core's system side and the uam_isa descriptor.
 *
 * The CPU models, reset (and the resets the core makes itself: RESET, stack overflow and
 * underflow with STVREN), the core's registers in the data space, the hardware stack and
 * its STKPTR/TOS view, interrupt entry (GIE/PEIE; PIC18 priorities with IPEN and the fast
 * register stack; the enhanced core's shadow registers), the address translation that
 * puts the data space on the bus (uam/isa/pic.h), the debugger view and snapshots.
 *
 * Not modelled: the watchdog (CLRWDT and SLEEP only set TO and PD), the priorities of
 * INTCON's own three sources on PIC18 (INTCON2's TMR0IP and RBIP: they are high priority
 * here), the vectored interrupt controllers of the newest parts, instruction timing (every
 * instruction is one tick of virtual time), and the enhanced core's STKOVF/STKUNF flags
 * without a reset (PCON is the board's).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pic.h"
#include "uam/bus.h"

/* ---- models -------------------------------------------------------------------- */

const uam_pic_config uam_pic_baseline_ref = {
    .name = "pic-baseline-ref", .core = UAM_PIC_BASELINE, .prog_words = 2048, .banks = 8,
};
const uam_pic_config uam_pic_mid_ref = {
    .name = "pic-mid-ref", .core = UAM_PIC_MID, .prog_words = 8192,
};
const uam_pic_config uam_pic_enhanced_ref = {
    .name = "pic-enhanced-ref", .core = UAM_PIC_ENHANCED, .features = UAM_PIC_F_STVREN, .prog_words = 32768,
};
const uam_pic_config uam_pic18_ref = {
    .name = "pic18-ref", .core = UAM_PIC18, .features = UAM_PIC_F_STVREN, .prog_words = 32768,
};

#define NO_CAUSE 0xFFu

uam_status uam_pic_configure(uam_cpu *cpu, const uam_pic_config *cfg)
{
    if (!cpu || !uam_pic_is(cpu->isa) || !cpu->isa_state || !cfg || cfg->core > UAM_PIC18 || cfg->prog_words < 2)
        return UAM_ERR_INVALID;
    const uint32_t max = cfg->core == UAM_PIC_BASELINE ? 4096 : cfg->core == UAM_PIC_MID ? 8192
                       : cfg->core == UAM_PIC_ENHANCED ? 32768 : (1u << 20);
    if (cfg->prog_words > max)
        return UAM_ERR_INVALID;
    if (cfg->core == UAM_PIC_BASELINE && cfg->banks != 1 && cfg->banks != 2 && cfg->banks != 4 && cfg->banks != 8)
        return UAM_ERR_INVALID;
    if (cfg->core != UAM_PIC18 && (cfg->features & UAM_PIC_F_XINST))
        return UAM_ERR_INVALID;
    pic_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

const uam_pic_config *uam_pic_config_of(uam_cpu *cpu)
{
    return cpu && uam_pic_is(cpu->isa) && cpu->isa_state ? &pic_st(cpu)->cfg : NULL;
}

void uam_pic_intcon_set(uam_cpu *cpu, uint8_t flags)
{
    if (!uam_pic_config_of(cpu))
        return;
    cpu->regfile[PIC_INTCON] |= flags & 7u;
    uam_cpu_request_exit(cpu);
}

void uam_pic_set_tblwt(uam_cpu *cpu, uam_pic_tblwt_fn fn, void *ctx)
{
    if (uam_pic_config_of(cpu)) {
        pic_st(cpu)->tblwt = fn;
        pic_st(cpu)->tblwt_ctx = ctx;
    }
}

void uam_pic_set_reset_hook(uam_cpu *cpu, uam_pic_reset_fn fn, void *ctx)
{
    if (uam_pic_config_of(cpu)) {
        pic_st(cpu)->reset_hook = fn;
        pic_st(cpu)->reset_ctx = ctx;
    }
}

uam_pic_reset_cause uam_pic_last_reset(uam_cpu *cpu)
{
    return uam_pic_config_of(cpu) ? (uam_pic_reset_cause)pic_st(cpu)->cause : UAM_PIC_RESET_POWER_ON;
}

/* ---- registers ------------------------------------------------------------------ */

static const uam_regdesc pic_regs[] = {
    { "w", PIC_W, 8, UAM_REG_GPR },
    { "c", PIC_C, 8, UAM_REG_FLAG }, { "dc", PIC_DC, 8, UAM_REG_FLAG }, { "z", PIC_Z, 8, UAM_REG_FLAG },
    { "ov", PIC_OV, 8, UAM_REG_FLAG }, { "n", PIC_N, 8, UAM_REG_FLAG },
    { "status", PIC_STATUS, 8, UAM_REG_OTHER }, { "bsr", PIC_BSR, 8, UAM_REG_OTHER },
    { "pclath", PIC_PCLATH, 8, UAM_REG_OTHER }, { "pclatu", PIC_PCLATU, 8, UAM_REG_OTHER },
    { "intcon", PIC_INTCON, 8, UAM_REG_OTHER }, { "rcon", PIC_RCON, 8, UAM_REG_OTHER },
    { "fsr0", PIC_FSR(0), 16, UAM_REG_GPR }, { "fsr1", PIC_FSR(1), 16, UAM_REG_GPR },
    { "fsr2", PIC_FSR(2), 16, UAM_REG_GPR }, { "prod", PIC_PROD, 16, UAM_REG_OTHER },
    { "tablat", PIC_TABLAT, 8, UAM_REG_OTHER }, { "tblptr", PIC_TBLPTR, 32, UAM_REG_OTHER },
    { "status_shad", PIC_SHAD + 0, 8, UAM_REG_OTHER }, { "wreg_shad", PIC_SHAD + 1, 8, UAM_REG_OTHER },
    { "bsr_shad", PIC_SHAD + 2, 8, UAM_REG_OTHER }, { "pclath_shad", PIC_SHAD + 3, 8, UAM_REG_OTHER },
    { "fsr0_shad", PIC_SHAD + 4, 16, UAM_REG_OTHER }, { "fsr1_shad", PIC_SHAD + 6, 16, UAM_REG_OTHER },
    { "ws", PIC_FAST + 0, 8, UAM_REG_OTHER }, { "statuss", PIC_FAST + 1, 8, UAM_REG_OTHER },
    { "bsrs", PIC_FAST + 2, 8, UAM_REG_OTHER },
};

uint8_t pic_status_wmask(uam_pic_core c)
{
    /* baseline PA2:PA0, mid-range IRP:RP1:RP0; TO and PD are read-only */
    return c == UAM_PIC_BASELINE || c == UAM_PIC_MID ? 0xE0 : 0x00;
}

static uint8_t status_read(uam_cpu *cpu)
{
    const uint8_t *r = cpu->regfile;
    uint8_t v = (uint8_t)(r[PIC_STATUS] | (r[PIC_C] & 1) | (r[PIC_DC] & 1) << 1 | (r[PIC_Z] & 1) << 2);
    if (pic_st(cpu)->cfg.core == UAM_PIC18)
        v |= (uint8_t)((r[PIC_OV] & 1) << 3 | (r[PIC_N] & 1) << 4);
    return v;
}

static void flags_write(uam_cpu *cpu, uint8_t v)
{
    uint8_t *r = cpu->regfile;
    r[PIC_C] = v & 1;
    r[PIC_DC] = (v >> 1) & 1;
    r[PIC_Z] = (v >> 2) & 1;
    if (pic_st(cpu)->cfg.core == UAM_PIC18) {
        r[PIC_OV] = (v >> 3) & 1;
        r[PIC_N] = (v >> 4) & 1;
    }
}

static void status_write(uam_cpu *cpu, uint8_t v)
{
    const uint8_t m = pic_status_wmask(pic_st(cpu)->cfg.core);
    cpu->regfile[PIC_STATUS] = (uint8_t)((cpu->regfile[PIC_STATUS] & ~m) | (v & m));
    flags_write(cpu, v);
}

/* ---- the core's registers in the data space -------------------------------------- */

pic_creg pic_core_reg(const uam_pic_config *c, uint32_t a)
{
    const pic_creg none = { CR_NONE, 0, 0 };
    switch (c->core) {
    case UAM_PIC_BASELINE:
        if (a >= 0x100)
            return none;
        switch (a & 0x1F) {
        case 0: return (pic_creg){ CR_INDF, 0, IND_PLAIN };
        case 2: return (pic_creg){ CR_PCL, 0, 0 };
        case 3: return (pic_creg){ CR_STATUS, 0, 0 };
        case 4: return (pic_creg){ CR_FSRL, 0, 0 };
        default: return none;
        }
    case UAM_PIC_MID:
        if (a >= 0x200)
            return none;
        switch (a & 0x7F) {
        case 0x0: return (pic_creg){ CR_INDF, 0, IND_PLAIN };
        case 0x2: return (pic_creg){ CR_PCL, 0, 0 };
        case 0x3: return (pic_creg){ CR_STATUS, 0, 0 };
        case 0x4: return (pic_creg){ CR_FSRL, 0, 0 };
        case 0xA: return (pic_creg){ CR_PCLATH, 0, 0 };
        case 0xB: return (pic_creg){ CR_INTCON, 0, 0 };
        default:  return none;
        }
    case UAM_PIC_ENHANCED:
        if (a >= 0x1000)
            return none;
        if (a >= 0xFE4 && a <= 0xFEB)
            return (pic_creg){ CR_SHAD, (uint8_t)(a - 0xFE4), 0 };
        switch (a) {
        case 0xFED: return (pic_creg){ CR_STKPTR, 0, 0 };
        case 0xFEE: return (pic_creg){ CR_TOSL, 0, 0 };
        case 0xFEF: return (pic_creg){ CR_TOSH, 0, 0 };
        default: break;
        }
        switch (a & 0x7F) {
        case 0x0: return (pic_creg){ CR_INDF, 0, IND_PLAIN };
        case 0x1: return (pic_creg){ CR_INDF, 1, IND_PLAIN };
        case 0x2: return (pic_creg){ CR_PCL, 0, 0 };
        case 0x3: return (pic_creg){ CR_STATUS, 0, 0 };
        case 0x4: return (pic_creg){ CR_FSRL, 0, 0 };
        case 0x5: return (pic_creg){ CR_FSRH, 0, 0 };
        case 0x6: return (pic_creg){ CR_FSRL, 1, 0 };
        case 0x7: return (pic_creg){ CR_FSRH, 1, 0 };
        case 0x8: return (pic_creg){ CR_BSR, 0, 0 };
        case 0x9: return (pic_creg){ CR_WREG, 0, 0 };
        case 0xA: return (pic_creg){ CR_PCLATH, 0, 0 };
        case 0xB: return (pic_creg){ CR_INTCON, 0, 0 };
        default:  return none;
        }
    default:
        if (a == 0xFD0)
            return (pic_creg){ CR_RCON, 0, 0 };
        if (a < 0xFD8 || a > 0xFFF || a == 0xFF0 || a == 0xFF1)
            return none;
        if (a <= 0xFEF) {
            /* three groups of eight from FSR2 (0xFD8, with STATUS) up to FSR0 (0xFE8, with WREG) */
            static const uint8_t fsr_of[] = { 2, 1, 0 };
            const unsigned g = (a - 0xFD8) / 8, o = (a - 0xFD8) % 8;
            const uint8_t n = fsr_of[g];
            switch (o) {
            case 0: return (pic_creg){ (uint8_t)(g == 0 ? CR_STATUS : g == 1 ? CR_BSR : CR_WREG), 0, 0 };
            case 1: return (pic_creg){ CR_FSRL, n, 0 };
            case 2: return (pic_creg){ CR_FSRH, n, 0 };
            case 3: return (pic_creg){ CR_INDF, n, IND_PLUSW };
            case 4: return (pic_creg){ CR_INDF, n, IND_PREINC };
            case 5: return (pic_creg){ CR_INDF, n, IND_POSTDEC };
            case 6: return (pic_creg){ CR_INDF, n, IND_POSTINC };
            default: return (pic_creg){ CR_INDF, n, IND_PLAIN };
            }
        }
        switch (a) {
        case 0xFF2: return (pic_creg){ CR_INTCON, 0, 0 };
        case 0xFF3: return (pic_creg){ CR_PRODL, 0, 0 };
        case 0xFF4: return (pic_creg){ CR_PRODH, 0, 0 };
        case 0xFF5: return (pic_creg){ CR_TABLAT, 0, 0 };
        case 0xFF6: return (pic_creg){ CR_TBLPTRL, 0, 0 };
        case 0xFF7: return (pic_creg){ CR_TBLPTRH, 0, 0 };
        case 0xFF8: return (pic_creg){ CR_TBLPTRU, 0, 0 };
        case 0xFF9: return (pic_creg){ CR_PCL, 0, 0 };
        case 0xFFA: return (pic_creg){ CR_PCLATH, 0, 0 };
        case 0xFFB: return (pic_creg){ CR_PCLATU, 0, 0 };
        case 0xFFC: return (pic_creg){ CR_STKPTR, 0, 0 };
        case 0xFFD: return (pic_creg){ CR_TOSL, 0, 0 };
        case 0xFFE: return (pic_creg){ CR_TOSH, 0, 0 };
        default:    return (pic_creg){ CR_TOSU, 0, 0 };
        }
    }
}

/* ---- the hardware stack ----------------------------------------------------------- */

static bool stvren(const pic_state *s) { return (s->cfg.features & UAM_PIC_F_STVREN) != 0; }

static void core_reset(uam_cpu *cpu, uam_pic_reset_cause cause);

/* Push pc; true if that reset the core. */
static bool stack_push(uam_cpu *cpu, uint32_t pc)
{
    pic_state *s = pic_st(cpu);
    switch (s->cfg.core) {
    case UAM_PIC_BASELINE:
        s->stack[1] = s->stack[0];
        s->stack[0] = pc;
        return false;
    case UAM_PIC_MID:
        s->stack[s->stkptr & 7] = pc;
        s->stkptr = (s->stkptr + 1) & 7;
        return false;
    case UAM_PIC_ENHANCED: {
        const uint8_t np = (s->stkptr + 1) & 0x1F;
        if (np == 0x10 && stvren(s)) {
            core_reset(cpu, UAM_PIC_RESET_STACK_OVERFLOW);
            return true;
        }
        s->stkptr = np;
        s->stack[np & 0xF] = pc;
        return false;
    }
    default: {
        unsigned sp = s->stkptr & 0x1F;
        if (sp == 31) {   /* already full: nothing is overwritten */
            s->stkptr |= 0x80;
            if (stvren(s)) {
                core_reset(cpu, UAM_PIC_RESET_STACK_OVERFLOW);
                return true;
            }
            return false;
        }
        sp++;
        s->stack[sp] = pc;
        s->stkptr = (uint8_t)((s->stkptr & 0xC0) | sp);
        if (sp == 31) {
            s->stkptr |= 0x80;
            if (stvren(s)) {
                core_reset(cpu, UAM_PIC_RESET_STACK_OVERFLOW);
                return true;
            }
        }
        return false;
    }
    }
}

/* Pop a pc; *reset set if that reset the core. */
static uint32_t stack_pop(uam_cpu *cpu, bool *reset)
{
    pic_state *s = pic_st(cpu);
    *reset = false;
    switch (s->cfg.core) {
    case UAM_PIC_BASELINE: {
        const uint32_t pc = s->stack[0];
        s->stack[0] = s->stack[1];
        return pc;
    }
    case UAM_PIC_MID:
        s->stkptr = (s->stkptr - 1) & 7;
        return s->stack[s->stkptr & 7];
    case UAM_PIC_ENHANCED: {
        if (s->stkptr == 0x1F && stvren(s)) {
            core_reset(cpu, UAM_PIC_RESET_STACK_UNDERFLOW);
            *reset = true;
            return 0;
        }
        const uint32_t pc = s->stack[s->stkptr & 0xF];
        s->stkptr = (s->stkptr - 1) & 0x1F;
        return pc;
    }
    default: {
        const unsigned sp = s->stkptr & 0x1F;
        if (sp == 0) {   /* underflow: pc 0, the pointer stays */
            s->stkptr |= 0x40;
            if (stvren(s)) {
                core_reset(cpu, UAM_PIC_RESET_STACK_UNDERFLOW);
                *reset = true;
            }
            return 0;
        }
        const uint32_t pc = s->stack[sp];
        s->stkptr = (uint8_t)((s->stkptr & 0xC0) | (sp - 1));
        return pc;
    }
    }
}

/* The top-of-stack entry (enhanced, PIC18), or NULL where it reads 0. */
static uint32_t *tos(pic_state *s)
{
    if (s->cfg.core == UAM_PIC_ENHANCED)
        return s->stkptr == 0x1F && stvren(s) ? NULL : &s->stack[s->stkptr & 0xF];
    return &s->stack[s->stkptr & 0x1F];
}

/* ---- the core's registers, by address (the run-time path and system registers) ------- */

static uint8_t core_read(uam_cpu *cpu, uint32_t a, bool via, uint32_t next_word);
static uint32_t core_write(uam_cpu *cpu, uint32_t a, uint8_t v, bool via);

/* A direct reference to an INDF-class register: the data address it reaches through its
 * FSR (applying the mode's FSR update), or false for none (enhanced unimplemented ranges;
 * *prog set for program memory). */
static bool ind_target(uam_cpu *cpu, pic_creg cr, uint32_t *addr, bool *prog)
{
    pic_state *s = pic_st(cpu);
    uint8_t *r = cpu->regfile;
    *prog = false;
    switch (s->cfg.core) {
    case UAM_PIC_BASELINE: {
        uint32_t a = r[PIC_FSR(0)] & ((s->cfg.banks * 32u - 1u) & 0xFF);
        *addr = (a & 0x1F) < 0x10 ? (a & 0x1F) : a;
        return true;
    }
    case UAM_PIC_MID:
        *addr = (uint32_t)(r[PIC_STATUS] & 0x80) << 1 | r[PIC_FSR(0)];
        return true;
    case UAM_PIC_ENHANCED: {
        const uint32_t f = (uint32_t)uam_cpu_reg_read(cpu, PIC_FSR(cr.n), 16);
        if (f < 0x1000) { *addr = f; return true; }
        if (f >= 0x2000 && f < 0x29B0) { *addr = ((f - 0x2000) / 80) * 128 + 0x20 + (f - 0x2000) % 80; return true; }
        if (f >= 0x8000) { *addr = (f & 0x7FFF) * 2; *prog = true; return true; }
        return false;
    }
    default: {
        const uint32_t f = (uint32_t)uam_cpu_reg_read(cpu, PIC_FSR(cr.n), 16) & 0xFFF;
        uint32_t a = f, nf = f;
        switch (cr.mode) {
        case IND_POSTINC: nf = f + 1; break;
        case IND_POSTDEC: nf = f - 1; break;
        case IND_PREINC:  a = nf = f + 1; break;
        case IND_PLUSW:   a = f + (uint32_t)(int32_t)(int8_t)r[PIC_W]; break;
        default: break;
        }
        uam_cpu_reg_write(cpu, PIC_FSR(cr.n), 16, nf & 0xFFF);
        *addr = a & 0xFFF;
        return true;
    }
    }
}

static uint8_t ind_read(uam_cpu *cpu, pic_creg cr, uint32_t next_word)
{
    uint32_t a;
    bool prog;
    if (!ind_target(cpu, cr, &a, &prog))
        return 0;
    uint64_t v = 0;
    if (prog)
        uam_bus_read(cpu->bus, a, 1, UAM_ACC_LE, &v);
    else if (pic_core_reg(&pic_st(cpu)->cfg, a).kind != CR_NONE)
        v = core_read(cpu, a, true, next_word);
    else
        uam_bus_read(cpu->bus, UAM_PIC_DATA_BASE + a, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}

static uint32_t ind_write(uam_cpu *cpu, pic_creg cr, uint8_t v)
{
    uint32_t a;
    bool prog;
    if (!ind_target(cpu, cr, &a, &prog) || prog)
        return 0;
    if (pic_core_reg(&pic_st(cpu)->cfg, a).kind != CR_NONE)
        return core_write(cpu, a, v, true);
    uam_bus_write(cpu->bus, UAM_PIC_DATA_BASE + a, 1, UAM_ACC_LE, v);
    return 0;
}

static uint8_t *plain_reg(uam_cpu *cpu, pic_creg cr, uint8_t *mask)
{
    const uam_pic_core c = pic_st(cpu)->cfg.core;
    uint8_t *r = cpu->regfile;
    *mask = 0xFF;
    switch (cr.kind) {
    case CR_FSRL:
        if (c == UAM_PIC_BASELINE)
            *mask = (uint8_t)(pic_st(cpu)->cfg.banks * 32u - 1u);
        return &r[PIC_FSR(cr.n)];
    case CR_FSRH:    if (c == UAM_PIC18) *mask = 0x0F; return &r[PIC_FSR(cr.n) + 1];
    case CR_BSR:     *mask = c == UAM_PIC18 ? 0x0F : 0x1F; return &r[PIC_BSR];
    case CR_WREG:    return &r[PIC_W];
    case CR_PCLATH:  *mask = c == UAM_PIC_MID ? 0x1F : c == UAM_PIC_ENHANCED ? 0x7F : 0xFF; return &r[PIC_PCLATH];
    case CR_PCLATU:  *mask = 0x1F; return &r[PIC_PCLATU];
    case CR_TBLPTRL: return &r[PIC_TBLPTR];
    case CR_TBLPTRH: return &r[PIC_TBLPTR + 1];
    case CR_TBLPTRU: *mask = 0x3F; return &r[PIC_TBLPTR + 2];
    case CR_TABLAT:  return &r[PIC_TABLAT];
    case CR_PRODL:   return &r[PIC_PROD];
    case CR_PRODH:   return &r[PIC_PROD + 1];
    case CR_SHAD:    if (cr.n == 2) *mask = 0x1F; return &r[PIC_SHAD + cr.n];
    default:         return NULL;
    }
}

/* The core register at data address a. via: reached through an FSR (an INDF reads 0).
 * next_word: the PC of the next instruction (a PCL read). */
static uint8_t core_read(uam_cpu *cpu, uint32_t a, bool via, uint32_t next_word)
{
    pic_state *s = pic_st(cpu);
    pic_creg cr = pic_core_reg(&s->cfg, a);
    uint8_t mask;
    uint8_t *p = plain_reg(cpu, cr, &mask);
    if (p) {
        uint8_t v = *p;
        if (s->cfg.core == UAM_PIC_BASELINE && cr.kind == CR_FSRL)
            v |= (uint8_t)~mask;
        return v;
    }
    switch (cr.kind) {
    case CR_INDF:   return via ? 0 : ind_read(cpu, cr, next_word);
    case CR_STATUS: return status_read(cpu);
    case CR_PCL:
        if (s->cfg.core == UAM_PIC18) {
            cpu->regfile[PIC_PCLATH] = (uint8_t)(next_word >> 8);
            cpu->regfile[PIC_PCLATU] = (uint8_t)((next_word >> 16) & 0x1F);
        }
        return (uint8_t)next_word;
    case CR_INTCON: return cpu->regfile[PIC_INTCON];
    case CR_RCON:   return cpu->regfile[PIC_RCON];
    case CR_STKPTR: return s->stkptr;
    case CR_TOSL: case CR_TOSH: case CR_TOSU: {
        const uint32_t *t = tos(s);
        uint32_t v = t ? *t : 0;
        if (s->cfg.core != UAM_PIC18)
            v >>= 1;   /* the word PC */
        return (uint8_t)(v >> (cr.kind == CR_TOSL ? 0 : cr.kind == CR_TOSH ? 8 : 16));
    }
    default: return 0;
    }
}

/* Write the core register at data address a; returns a jump (pc | 1) for PCL, else 0. */
static uint32_t core_write(uam_cpu *cpu, uint32_t a, uint8_t v, bool via)
{
    pic_state *s = pic_st(cpu);
    pic_creg cr = pic_core_reg(&s->cfg, a);
    uint8_t *r = cpu->regfile, mask;
    uint8_t *p = plain_reg(cpu, cr, &mask);
    if (p) {
        *p = v & mask;
        return 0;
    }
    const uint32_t pcmask = pic_pc_mask(&s->cfg);
    switch (cr.kind) {
    case CR_INDF:   return via ? 0 : ind_write(cpu, cr, v);
    case CR_STATUS: status_write(cpu, v); return 0;
    case CR_PCL: {
        uint32_t t;
        if (s->cfg.core == UAM_PIC18)
            t = (uint32_t)r[PIC_PCLATU] << 16 | (uint32_t)r[PIC_PCLATH] << 8 | v;
        else if (s->cfg.core == UAM_PIC_BASELINE)
            t = ((uint32_t)(r[PIC_STATUS] >> 5) << 9 | v) << 1;
        else
            t = ((uint32_t)r[PIC_PCLATH] << 8 | v) << 1;
        return (t & pcmask) | 1u;
    }
    case CR_INTCON:
        r[PIC_INTCON] = v;
        uam_cpu_request_exit(cpu);
        return 0;
    case CR_RCON:
        r[PIC_RCON] = (uint8_t)((r[PIC_RCON] & ~0xD3u) | (v & 0xD3u));
        uam_cpu_request_exit(cpu);
        return 0;
    case CR_STKPTR:
        if (s->cfg.core == UAM_PIC18)   /* STKFUL/STKUNF: only cleared */
            s->stkptr = (uint8_t)((s->stkptr & v & 0xC0) | (v & 0x1F));
        else
            s->stkptr = v & 0x1F;
        return 0;
    case CR_TOSL: case CR_TOSH: case CR_TOSU: {
        uint32_t *t = s->cfg.core == UAM_PIC_ENHANCED ? &s->stack[s->stkptr & 0xF] : tos(s);
        const unsigned sh = cr.kind == CR_TOSL ? 0 : cr.kind == CR_TOSH ? 8 : 16;
        uint32_t w = s->cfg.core == UAM_PIC18 ? *t : *t >> 1;
        w = (w & ~(0xFFu << sh)) | (uint32_t)v << sh;
        *t = s->cfg.core == UAM_PIC18 ? w & pcmask : (w << 1) & pcmask;
        return 0;
    }
    default: return 0;
    }
}

static uam_status pic_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    if (id >= 0x1000)
        return UAM_ERR_ILLEGAL;
    *val = core_read(cpu, id, true, (uint32_t)(cpu->pc >> (pic_st(cpu)->cfg.core == UAM_PIC18 ? 0 : 1)));
    return UAM_OK;
}

static uam_status pic_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    if (id >= 0x1000)
        return UAM_ERR_ILLEGAL;
    core_write(cpu, id, (uint8_t)val, true);
    return UAM_OK;
}

/* ---- resets ----------------------------------------------------------------------- */

static void reset_regs(uam_cpu *cpu, uam_pic_reset_cause cause)
{
    pic_state *s = pic_st(cpu);
    const uint8_t rcon = cpu->regfile[PIC_RCON];
    memset(cpu->regfile, 0, cpu->regfile_size);
    if (s->cfg.core == UAM_PIC18) {
        if (cause == UAM_PIC_RESET_POWER_ON)
            cpu->regfile[PIC_RCON] = PIC_RCON_RI | PIC_RCON_TO | PIC_RCON_PD;
        else
            cpu->regfile[PIC_RCON] = (uint8_t)(rcon & 0x5F & (cause == UAM_PIC_RESET_INSN ? ~PIC_RCON_RI : 0xFF));
    } else {
        cpu->regfile[PIC_STATUS] = PIC_ST_TO | PIC_ST_PD;
    }
    switch (s->cfg.core) {
    case UAM_PIC_ENHANCED: s->stkptr = 0x1F; break;
    case UAM_PIC18:        s->stkptr = cause == UAM_PIC_RESET_POWER_ON ? 0 : s->stkptr & 0xC0; break;
    default:               s->stkptr = 0; break;
    }
    if (cause == UAM_PIC_RESET_POWER_ON)
        memset(s->stack, 0, sizeof(s->stack));
    s->cause = (uint8_t)cause;
    s->halted = false;
    cpu->waiting = false;
}

/* A reset the core makes itself (from a helper: the lifter leaves for the reset vector). */
static void core_reset(uam_cpu *cpu, uam_pic_reset_cause cause)
{
    pic_state *s = pic_st(cpu);
    if (s->reset_hook) {   /* the board may reset the device: its reset keeps the cause */
        s->pending_cause = (uint8_t)cause;
        s->reset_hook(s->reset_ctx, cause);
    }
    reset_regs(cpu, cause);
    uam_cpu_request_exit(cpu);
}

/* ---- interrupts ------------------------------------------------------------------- */

/* INTCON's own sources: an enable (bits 5-3) and its flag (bits 2-0) both set. */
static bool intcon_pending(const uam_cpu *cpu)
{
    const uint8_t i = cpu->regfile[PIC_INTCON];
    return ((i >> 3) & i & 7u) != 0;
}

/* Which entry to make now: 0 none, 1 the (high-priority) vector, 2 PIC18's low one. */
static unsigned pick(uam_cpu *cpu, bool ignore_gie)
{
    pic_state *s = pic_st(cpu);
    if (s->halted || s->cfg.core == UAM_PIC_BASELINE)
        return 0;
    const uint8_t i = cpu->regfile[PIC_INTCON];
    const bool gie = ignore_gie || (i & PIC_INTCON_GIE);
    if (s->cfg.core == UAM_PIC18 && (cpu->regfile[PIC_RCON] & PIC_RCON_IPEN)) {
        if (gie && (intcon_pending(cpu) || (s->lines & 1)))
            return 1;
        if (gie && (ignore_gie || (i & PIC_INTCON_PEIE)) && (s->lines & 2))
            return 2;
        return 0;
    }
    const bool periph = (i & PIC_INTCON_PEIE) && (s->lines & 3);
    return gie && (intcon_pending(cpu) || periph) ? 1 : 0;
}

static bool pic_irq_take(uam_cpu *cpu)
{
    pic_state *s = pic_st(cpu);
    const unsigned k = pick(cpu, false);
    if (!k)
        return false;
    uint8_t *r = cpu->regfile;
    const bool ipen = s->cfg.core == UAM_PIC18 && (r[PIC_RCON] & PIC_RCON_IPEN);
    s->last_line = k == 2 ? 2 : (uint8_t)(s->lines & (ipen ? 1 : 3));
    r[PIC_INTCON] &= (uint8_t)~(k == 2 ? PIC_INTCON_PEIE : PIC_INTCON_GIE);
    if (s->cfg.core == UAM_PIC_ENHANCED) {   /* automatic context save */
        r[PIC_SHAD + 0] = (uint8_t)(status_read(cpu) & 0x07);
        r[PIC_SHAD + 1] = r[PIC_W];
        r[PIC_SHAD + 2] = r[PIC_BSR];
        r[PIC_SHAD + 3] = r[PIC_PCLATH];
        memcpy(&r[PIC_SHAD + 4], &r[PIC_FSR(0)], 4);
    } else if (s->cfg.core == UAM_PIC18) {   /* the fast register stack */
        r[PIC_FAST + 0] = r[PIC_W];
        r[PIC_FAST + 1] = status_read(cpu);
        r[PIC_FAST + 2] = r[PIC_BSR];
    }
    if (stack_push(cpu, (uint32_t)cpu->pc)) {
        cpu->pc = pic_reset_pc(&s->cfg);
        return true;
    }
    cpu->pc = s->cfg.core == UAM_PIC18 ? (k == 2 ? 0x18 : 0x08) : 4 * 2;
    return true;
}

static bool pic_irq_wakeup(uam_cpu *cpu)
{
    return pick(cpu, true) != 0;
}

static void pic_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    pic_state *s = pic_st(cpu);
    if (line > UAM_PIC_IRQ_LOW)
        return;
    if (level)
        s->lines |= (uint8_t)(1u << line);
    else
        s->lines &= (uint8_t)~(1u << line);
}

static uint64_t pic_irq_lines(uam_cpu *cpu)
{
    return pic_st(cpu)->last_line;
}

/* ---- helpers ---------------------------------------------------------------------- */

static uint64_t helper_core_rd(void *env, uint64_t a, uint64_t next_word, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    return core_read(env, (uint32_t)a & 0xFFFF, (a & PIC_VIA_FSR) != 0, (uint32_t)next_word);
}

static uint64_t helper_core_wr(void *env, uint64_t a, uint64_t v, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    return core_write(env, (uint32_t)a & 0xFFFF, (uint8_t)v, (a & PIC_VIA_FSR) != 0);
}

static uint64_t helper_push(void *env, uint64_t pc, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    return stack_push(env, (uint32_t)pc) ? PIC_RESET_MARK : 0;
}

static uint64_t helper_pop(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    bool reset;
    const uint32_t pc = stack_pop(env, &reset);
    return reset ? PIC_RESET_MARK : pc;
}

/* RETFIE's enable: GIE, or with PIC18 priorities GIEH if a high-priority handler ran
 * (GIEH clear), else GIEL. */
static uint64_t helper_retfie(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    uint8_t *r = cpu->regfile;
    if (pic_st(cpu)->cfg.core == UAM_PIC18 && (r[PIC_RCON] & PIC_RCON_IPEN) && (r[PIC_INTCON] & PIC_INTCON_GIE))
        r[PIC_INTCON] |= PIC_INTCON_PEIE;
    else
        r[PIC_INTCON] |= PIC_INTCON_GIE;
    uam_cpu_request_exit(cpu);
    return 0;
}

static uint64_t helper_reset(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    core_reset(env, UAM_PIC_RESET_INSN);
    return 0;
}

static uint64_t helper_tblwt(void *env, uint64_t a, uint64_t v, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    pic_state *s = pic_st(env);
    if (s->tblwt)
        s->tblwt(s->tblwt_ctx, (uint32_t)a, (uint8_t)v);
    return 0;
}

#define RW (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE)
const uam_helper_desc pic_helpers[PIC_HELPER__COUNT] = {
    [PIC_HELPER_CORE_RD] = { .name = "pic_core_rd", .n_args = 2, .arg_width = { 32, 32 },
                             .result_width = 8, .effect = RW, .fn = helper_core_rd },
    [PIC_HELPER_CORE_WR] = { .name = "pic_core_wr", .n_args = 2, .arg_width = { 32, 8 },
                             .result_width = 32, .effect = RW, .fn = helper_core_wr },
    [PIC_HELPER_PUSH]    = { .name = "pic_push", .n_args = 1, .arg_width = { 32 },
                             .result_width = 32, .effect = RW, .fn = helper_push },
    [PIC_HELPER_POP]     = { .name = "pic_pop", .n_args = 0, .result_width = 32, .effect = RW,
                             .fn = helper_pop },
    [PIC_HELPER_RETFIE]  = { .name = "pic_retfie", .n_args = 0, .result_width = 0, .effect = RW,
                             .fn = helper_retfie },
    [PIC_HELPER_RESET]   = { .name = "pic_reset", .n_args = 0, .result_width = 0, .effect = RW,
                             .fn = helper_reset },
    [PIC_HELPER_TBLWT]   = { .name = "pic_tblwt", .n_args = 2, .arg_width = { 32, 8 },
                             .result_width = 0, .effect = RW, .fn = helper_tblwt },
};
#undef RW

/* ---- exceptions, translation ------------------------------------------------------ */

/* A PIC has no exceptions: a fetch outside program memory or an access nothing answers
 * (with the board's unmapped policy at "fault") halts the hart, with a message. */
static void pic_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    pic_state *s = pic_st(cpu);
    uam_log(UAM_LOG_ERROR, "pic", "pc=0x%llx: %s at 0x%llx: core halted",
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
 * at UAM_PIC_DATA_BASE; a debugger or loader uses bus addresses as they are. */
static uam_status pic_translate(uam_cpu *cpu, uint64_t va, uam_xlate_kind kind, uam_xlate *out)
{
    (void)cpu;
    out->fault = 0;
    out->fixed = true;
    out->paddr = (kind == UAM_XLATE_LOAD || kind == UAM_XLATE_STORE) ? UAM_PIC_DATA_BASE + (va & 0xFFFFFF) : va;
    return UAM_OK;
}

/* ---- lifecycle -------------------------------------------------------------------- */

static uam_status pic_init(uam_cpu *cpu)
{
    pic_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    s->cfg = uam_pic18_ref;
    s->pending_cause = NO_CAUSE;
    cpu->isa_state = s;
    return UAM_OK;
}

static void pic_fini(uam_cpu *cpu)
{
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

static void pic_reset(uam_cpu *cpu, uint64_t entry)
{
    pic_state *s = pic_st(cpu);
    const uam_pic_reset_cause cause = s->pending_cause == NO_CAUSE ? UAM_PIC_RESET_POWER_ON
                                                                    : (uam_pic_reset_cause)s->pending_cause;
    s->pending_cause = NO_CAUSE;
    reset_regs(cpu, cause);
    s->last_line = 0;   /* lines are the board's */
    cpu->priv = 0;
    cpu->pc = entry;
    cpu->resv.valid = false;
}

/* ---- debugger view ---------------------------------------------------------------- */

#define GDB_STACK0 14u
#define GDB_NREGS  (GDB_STACK0 + PIC_STACK_MAX)

static const uint8_t pic_gdb_sizes[GDB_NREGS] = {
    1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 4, 1, 2, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
};

#define G(n, num, bits) "<reg name=\"" n "\" bitsize=\"" #bits "\" regnum=\"" #num "\"/>"
static const char pic_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><feature name=\"org.ember.pic.core\">"
    G("w", 0, 8) G("status", 1, 8) G("bsr", 2, 8) G("fsr0", 3, 16) G("fsr1", 4, 16) G("fsr2", 5, 16)
    G("pclath", 6, 8) G("pclatu", 7, 8) G("intcon", 8, 8) G("stkptr", 9, 8) G("tblptr", 10, 32)
    G("tablat", 11, 8) G("prod", 12, 16) G("pc", 13, 32)
    "</feature><feature name=\"org.ember.pic.stack\">"
    "<reg name=\"stack0\" bitsize=\"32\" regnum=\"14\"/>"
    "<reg name=\"stack1\" bitsize=\"32\"/><reg name=\"stack2\" bitsize=\"32\"/><reg name=\"stack3\" bitsize=\"32\"/>"
    "<reg name=\"stack4\" bitsize=\"32\"/><reg name=\"stack5\" bitsize=\"32\"/><reg name=\"stack6\" bitsize=\"32\"/>"
    "<reg name=\"stack7\" bitsize=\"32\"/><reg name=\"stack8\" bitsize=\"32\"/><reg name=\"stack9\" bitsize=\"32\"/>"
    "<reg name=\"stack10\" bitsize=\"32\"/><reg name=\"stack11\" bitsize=\"32\"/><reg name=\"stack12\" bitsize=\"32\"/>"
    "<reg name=\"stack13\" bitsize=\"32\"/><reg name=\"stack14\" bitsize=\"32\"/><reg name=\"stack15\" bitsize=\"32\"/>"
    "<reg name=\"stack16\" bitsize=\"32\"/><reg name=\"stack17\" bitsize=\"32\"/><reg name=\"stack18\" bitsize=\"32\"/>"
    "<reg name=\"stack19\" bitsize=\"32\"/><reg name=\"stack20\" bitsize=\"32\"/><reg name=\"stack21\" bitsize=\"32\"/>"
    "<reg name=\"stack22\" bitsize=\"32\"/><reg name=\"stack23\" bitsize=\"32\"/><reg name=\"stack24\" bitsize=\"32\"/>"
    "<reg name=\"stack25\" bitsize=\"32\"/><reg name=\"stack26\" bitsize=\"32\"/><reg name=\"stack27\" bitsize=\"32\"/>"
    "<reg name=\"stack28\" bitsize=\"32\"/><reg name=\"stack29\" bitsize=\"32\"/><reg name=\"stack30\" bitsize=\"32\"/>"
    "<reg name=\"stack31\" bitsize=\"32\"/>"
    "</feature></target>";
#undef G

static bool pic_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    pic_state *s = pic_st(cpu);
    switch (n) {
    case 0:  *v = cpu->regfile[PIC_W]; break;
    case 1:  *v = status_read(cpu); break;
    case 2:  *v = cpu->regfile[PIC_BSR]; break;
    case 3: case 4: case 5: *v = uam_cpu_reg_read(cpu, PIC_FSR(n - 3), 16); break;
    case 6:  *v = cpu->regfile[PIC_PCLATH]; break;
    case 7:  *v = cpu->regfile[PIC_PCLATU]; break;
    case 8:  *v = cpu->regfile[PIC_INTCON]; break;
    case 9:  *v = s->stkptr; break;
    case 10: *v = uam_cpu_reg_read(cpu, PIC_TBLPTR, 32); break;
    case 11: *v = cpu->regfile[PIC_TABLAT]; break;
    case 12: *v = uam_cpu_reg_read(cpu, PIC_PROD, 16); break;
    case 13: *v = (uint32_t)cpu->pc; break;
    default:
        if (n >= GDB_NREGS)
            return false;
        *v = s->stack[n - GDB_STACK0];
        break;
    }
    return true;
}

static bool pic_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    pic_state *s = pic_st(cpu);
    switch (n) {
    case 0:  cpu->regfile[PIC_W] = (uint8_t)v; break;
    case 1:  status_write(cpu, (uint8_t)v); break;
    case 2:  cpu->regfile[PIC_BSR] = (uint8_t)v; break;
    case 3: case 4: case 5: uam_cpu_reg_write(cpu, PIC_FSR(n - 3), 16, v); break;
    case 6:  cpu->regfile[PIC_PCLATH] = (uint8_t)v; break;
    case 7:  cpu->regfile[PIC_PCLATU] = (uint8_t)v; break;
    case 8:  cpu->regfile[PIC_INTCON] = (uint8_t)v; uam_cpu_request_exit(cpu); break;
    case 9:  s->stkptr = (uint8_t)v; break;
    case 10: uam_cpu_reg_write(cpu, PIC_TBLPTR, 32, v & 0x3FFFFF); break;
    case 11: cpu->regfile[PIC_TABLAT] = (uint8_t)v; break;
    case 12: uam_cpu_reg_write(cpu, PIC_PROD, 16, v); break;
    case 13: cpu->pc = (uint32_t)v & pic_pc_mask(&s->cfg); break;
    default:
        if (n >= GDB_NREGS)
            return false;
        s->stack[n - GDB_STACK0] = (uint32_t)v & pic_pc_mask(&s->cfg);
        break;
    }
    return true;
}

static const uam_gdb_arch pic_gdb = {
    .target_xml = pic_target_xml, .n_regs = GDB_NREGS, .reg_bytes = 4, .reg_sizes = pic_gdb_sizes,
    .read_reg = pic_gdb_read, .write_reg = pic_gdb_write,
};

static void pic_dump_state(uam_cpu *cpu, FILE *out)
{
    pic_state *s = pic_st(cpu);
    const uint8_t *r = cpu->regfile;
    const bool w14 = s->cfg.core != UAM_PIC18;
    fprintf(out, "pc=%06llx%s w=%02x status=%02x bsr=%02x pclath=%02x pclatu=%02x intcon=%02x icount=%llu model=%s\n",
            (unsigned long long)cpu->pc, w14 ? "" : " ", r[PIC_W], status_read(cpu), r[PIC_BSR],
            r[PIC_PCLATH], r[PIC_PCLATU], r[PIC_INTCON], (unsigned long long)cpu->icount,
            s->cfg.name ? s->cfg.name : "?");
    if (w14)
        fprintf(out, "word pc=%04llx ", (unsigned long long)cpu->pc >> 1);
    fprintf(out, "fsr0=%04llx fsr1=%04llx fsr2=%04llx prod=%04llx tblptr=%06llx tablat=%02x rcon=%02x\n",
            (unsigned long long)uam_cpu_reg_read(cpu, PIC_FSR(0), 16),
            (unsigned long long)uam_cpu_reg_read(cpu, PIC_FSR(1), 16),
            (unsigned long long)uam_cpu_reg_read(cpu, PIC_FSR(2), 16),
            (unsigned long long)uam_cpu_reg_read(cpu, PIC_PROD, 16),
            (unsigned long long)uam_cpu_reg_read(cpu, PIC_TBLPTR, 32), r[PIC_TABLAT], r[PIC_RCON]);
    const unsigned depth = pic_stack_depth(s->cfg.core);
    fprintf(out, "stkptr=%02x stack:", s->stkptr);
    for (unsigned i = 0; i < depth + (s->cfg.core == UAM_PIC18); i++)
        fprintf(out, " %06x", s->stack[i]);
    fprintf(out, " lines=%x%s\n", s->lines, s->halted ? " HALTED" : "");
}

/* ---- snapshots -------------------------------------------------------------------- */

static void pic_save(uam_cpu *cpu, uam_state_writer *w)
{
    const pic_state *s = pic_st(cpu);
    for (unsigned i = 0; i < PIC_STACK_MAX; i++)
        uam_state_put_u32(w, s->stack[i]);
    const uint8_t b[] = { s->stkptr, s->lines, s->last_line, s->cause, s->pending_cause };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_bool(w, s->halted);
}

static void pic_restore(uam_cpu *cpu, uam_state_reader *r)
{
    pic_state *s = pic_st(cpu);
    for (unsigned i = 0; i < PIC_STACK_MAX; i++)
        s->stack[i] = uam_state_get_u32(r);
    uint8_t b[5];
    uam_state_get_bytes(r, b, sizeof(b));
    s->stkptr = b[0]; s->lines = b[1]; s->last_line = b[2]; s->cause = b[3]; s->pending_cause = b[4];
    s->halted = uam_state_get_bool(r);
}

const uam_isa uam_isa_pic = {
    .name           = "pic",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,
    .regfile_size   = PIC_REGFILE_SIZE,
    .regs           = pic_regs,
    .n_regs         = UAM_ARRAY_LEN(pic_regs),
    .helpers        = { pic_helpers, PIC_HELPER__COUNT },
    .init           = pic_init,
    .fini           = pic_fini,
    .reset          = pic_reset,
    .lift           = pic_lift,
    .take_exception = pic_take_exception,
    .irq_take       = pic_irq_take,
    .irq_wakeup     = pic_irq_wakeup,
    .irq_input      = pic_irq_input,
    .sysreg_read    = pic_sysreg_read,
    .sysreg_write   = pic_sysreg_write,
    .translate      = pic_translate,
    .gdb            = &pic_gdb,
    .dump_state     = pic_dump_state,
    .save           = pic_save,
    .restore        = pic_restore,
    .irq_lines      = pic_irq_lines,
};
