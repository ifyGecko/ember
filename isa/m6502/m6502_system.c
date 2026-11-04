/*
 * m6502_system.c - The 6502 core's system side and the uam_isa descriptor.
 *
 * The CPU models, reset through the reset vector, P's I and D bits (the interrupt
 * poll's latency, the decimal flag translated code depends on), interrupt entry (the
 * wired-OR IRQ, the NMI edge, the set-overflow input), STP and JAM, decimal arithmetic,
 * the debugger view and snapshots.
 *
 * Not modelled: instruction timing (every instruction is one tick of virtual time), an
 * NMI arriving during BRK's interrupt sequence (which on NMOS takes it with BRK's B
 * bit), and the IRQ poll of a taken branch that crosses no page (the chip delays the
 * interrupt by one instruction there; here it is taken after the branch).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m6502.h"
#include "uam/bus.h"

/* ---- models -------------------------------------------------------------------- */

const uam_m6502_config uam_m6502_nmos = {
    .name = "m6502-nmos", .decimal = true, .magic = 0xEE,
};
const uam_m6502_config uam_m6502_2a03 = {
    .name = "m6502-2a03", .decimal = false, .magic = 0xEE,
};
const uam_m6502_config uam_m6502_w65c02 = {
    .name = "m6502-w65c02", .cmos = true, .decimal = true, .bitops = true, .wai_stp = true,
};
const uam_m6502_config uam_m6502_r65c02 = {
    .name = "m6502-r65c02", .cmos = true, .decimal = true, .bitops = true,
};

uam_status uam_m6502_configure(uam_cpu *cpu, const uam_m6502_config *cfg)
{
    if (!cpu || !uam_m6502_is(cpu->isa) || !cpu->isa_state || !cfg
        || (!cfg->cmos && (cfg->bitops || cfg->wai_stp)))
        return UAM_ERR_INVALID;
    m6502_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

const uam_m6502_config *uam_m6502_config_of(uam_cpu *cpu)
{
    return cpu && uam_m6502_is(cpu->isa) && cpu->isa_state ? &m6502_st(cpu)->cfg : NULL;
}

/* ---- registers ------------------------------------------------------------------ */

static const uam_regdesc m6502_regs[] = {
    { "a", M6502_A, 8, UAM_REG_GPR },   { "x", M6502_X, 8, UAM_REG_GPR },
    { "y", M6502_Y, 8, UAM_REG_GPR },   { "s", M6502_S, 8, UAM_REG_OTHER },
    { "c", M6502_C, 8, UAM_REG_FLAG },  { "z", M6502_Z, 8, UAM_REG_FLAG },
    { "v", M6502_V, 8, UAM_REG_FLAG },  { "n", M6502_N, 8, UAM_REG_FLAG },
    { "px", M6502_PX, 8, UAM_REG_FLAG },
};

/* P with bit 5 set and B as given. */
static uint8_t p_read(const uam_cpu *cpu, bool b)
{
    const uint8_t *r = cpu->regfile;
    return (uint8_t)((r[M6502_N] & 1) << 7 | (r[M6502_V] & 1) << 6 | UAM_M6502_P_U | (b ? UAM_M6502_P_B : 0)
                     | (r[M6502_PX] & PX_MASK) | (r[M6502_Z] & 1) << 1 | (r[M6502_C] & 1));
}

/* D, which translated code was made for. Changing it needs a new block. */
static void set_priv(uam_cpu *cpu)
{
    const m6502_state *s = m6502_st(cpu);
    const unsigned p = s->cfg.decimal && (cpu->regfile[M6502_PX] & UAM_M6502_P_D) ? M6502_PRIV_D : 0;
    if (p != cpu->priv) {
        cpu->priv = p;
        uam_cpu_request_exit(cpu);
    }
}

/* I and D. With `latency`, the next interrupt poll still sees the old I (the poll the
 * chip makes before the instruction's last cycle). */
static void px_write(uam_cpu *cpu, uint8_t v, bool latency)
{
    m6502_state *s = m6502_st(cpu);
    const bool was_i = cpu->regfile[M6502_PX] & UAM_M6502_P_I, now_i = v & UAM_M6502_P_I;
    cpu->regfile[M6502_PX] = v & PX_MASK;
    if (latency && was_i && !now_i)
        s->defer_irq = true;
    if (latency && !was_i && now_i)
        s->poll_i0 = true;
    set_priv(cpu);
    uam_cpu_request_exit(cpu);
}

static void p_write(uam_cpu *cpu, uint8_t v)
{
    uint8_t *r = cpu->regfile;
    r[M6502_C] = v & 1;
    r[M6502_Z] = v >> 1 & 1;
    r[M6502_V] = v >> 6 & 1;
    r[M6502_N] = v >> 7 & 1;
    px_write(cpu, v, false);
}

static uam_status m6502_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    if (id != M6502_SYS_P)
        return UAM_ERR_ILLEGAL;
    *val = p_read(cpu, true);
    return UAM_OK;
}

static uam_status m6502_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    m6502_state *s = m6502_st(cpu);
    switch (id) {
    case M6502_SYS_P:     px_write(cpu, (uint8_t)val, true); return UAM_OK;
    case M6502_SYS_P_NOW: px_write(cpu, (uint8_t)val, false); return UAM_OK;
    case M6502_SYS_STOP:
        if (val)
            uam_log(UAM_LOG_WARN, "m6502", "pc=0x%04llx: JAM: the core stops until reset",
                    (unsigned long long)cpu->pc);
        s->stopped = (uint8_t)(1 + (val != 0));
        uam_cpu_request_exit(cpu);
        return UAM_OK;
    default:
        return UAM_ERR_ILLEGAL;
    }
}

/* ---- interrupts ------------------------------------------------------------------- */

static void push(uam_cpu *cpu, uint8_t v)
{
    uint8_t *sp = &cpu->regfile[M6502_S];
    uam_bus_write(cpu->bus, 0x100u | *sp, 1, UAM_ACC_LE, v);
    (*sp)--;
}

static uint16_t vector(uam_cpu *cpu, uint32_t a, bool *ok)
{
    uint64_t lo = 0, hi = 0;
    const bool good = uam_bus_read(cpu->bus, a, 1, UAM_ACC_LE, &lo) == UAM_OK
                      && uam_bus_read(cpu->bus, a + 1, 1, UAM_ACC_LE, &hi) == UAM_OK;
    if (ok)
        *ok = good;
    return (uint16_t)(lo | hi << 8);
}

/* The interrupt sequence: push the pc and P (B clear), set I (CMOS: clear D), vector. */
static void enter(uam_cpu *cpu, uint32_t vec)
{
    m6502_state *s = m6502_st(cpu);
    const uint16_t pc = (uint16_t)cpu->pc;
    push(cpu, pc >> 8);
    push(cpu, pc & 0xFF);
    push(cpu, p_read(cpu, false));
    uint8_t px = cpu->regfile[M6502_PX] | UAM_M6502_P_I;
    if (s->cfg.cmos)
        px &= (uint8_t)~UAM_M6502_P_D;
    px_write(cpu, px, false);
    cpu->pc = vector(cpu, vec, NULL);
    cpu->waiting = false;
}

static bool m6502_irq_take(uam_cpu *cpu)
{
    m6502_state *s = m6502_st(cpu);
    const bool poll_i0 = s->poll_i0, defer = s->defer_irq;
    s->poll_i0 = s->defer_irq = false;
    if (s->stopped || s->halted)
        return false;
    if (s->nmi_pending) {
        s->nmi_pending = false;
        s->last_line = UAM_M6502_LINE_NMI;
        enter(cpu, UAM_M6502_VEC_NMI);
        return true;
    }
    if (!s->lines)
        return false;
    const bool masked = poll_i0 ? false : (cpu->regfile[M6502_PX] & UAM_M6502_P_I) != 0;
    if (masked)
        return false;
    if (defer) {   /* one more instruction first: stop the hart after it */
        uam_cpu_request_exit(cpu);
        return false;
    }
    s->last_line = (unsigned)__builtin_ctz(s->lines);
    enter(cpu, UAM_M6502_VEC_IRQ);
    return true;
}

/* WAI wakes on an interrupt request whether or not I masks it; STP and JAM never. */
static bool m6502_irq_wakeup(uam_cpu *cpu)
{
    const m6502_state *s = m6502_st(cpu);
    return !s->stopped && !s->halted && (s->nmi_pending || s->lines);
}

static void m6502_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    m6502_state *s = m6502_st(cpu);
    if (line == UAM_M6502_LINE_NMI) {
        if (level && !s->nmi_level)
            s->nmi_pending = true;
        s->nmi_level = level != 0;
    } else if (line == UAM_M6502_LINE_SO) {
        if (level)
            cpu->regfile[M6502_V] = 1;
    } else if (line < UAM_M6502_LINE_NMI) {
        if (level)
            s->lines |= UINT32_C(1) << line;
        else
            s->lines &= ~(UINT32_C(1) << line);
    }
}

static uint64_t m6502_irq_lines(uam_cpu *cpu)
{
    return UINT64_C(1) << m6502_st(cpu)->last_line;
}

/* ---- decimal arithmetic ------------------------------------------------------------ */

static unsigned pack(unsigned r, bool c, bool z, bool n, bool v)
{
    return (r & 0xFF) | (unsigned)c << M6502_DEC_C | (unsigned)z << M6502_DEC_Z | (unsigned)n << M6502_DEC_N
           | (unsigned)v << M6502_DEC_V;
}

/* ADC with D set. The accumulator: add the low digits, adjust a digit above 9 by 6
 * (carrying into the high digit), add the high digits, adjust by 0x60 from 0xA0 up; C
 * is a carry out of that. V and the NMOS N are those of the high-digit sum before its
 * adjustment, taken as signed; the NMOS Z is the binary sum's. The 65C02's N and Z are
 * the result's. Defined for every input, BCD or not. */
unsigned m6502_adc_dec(unsigned a, unsigned m, unsigned c, bool cmos)
{
    int al = (int)(a & 0x0F) + (int)(m & 0x0F) + (int)c;
    if (al >= 0x0A)
        al = ((al + 0x06) & 0x0F) + 0x10;
    int s = (int)(a & 0xF0) + (int)(m & 0xF0) + al;
    const int ss = (int)(int8_t)(a & 0xF0) + (int)(int8_t)(m & 0xF0) + al;
    const bool v = ss < -128 || ss > 127;
    const bool n7 = (ss >> 7) & 1;
    if (s >= 0xA0)
        s += 0x60;
    const unsigned r = (unsigned)s & 0xFF;
    if (cmos)
        return pack(r, s >= 0x100, r == 0, r >> 7, v);
    return pack(r, s >= 0x100, ((a + m + c) & 0xFF) == 0, n7, v);
}

/* SBC with D set. C, V and the NMOS N and Z are the binary subtraction's; the 65C02's
 * N and Z are the result's. The NMOS adjusts each digit that borrowed; the 65C02
 * adjusts the binary difference. */
unsigned m6502_sbc_dec(unsigned a, unsigned m, unsigned c, bool cmos)
{
    const int bin = (int)a - (int)m - (int)(1 - c);
    const unsigned b8 = (unsigned)bin & 0xFF;
    const bool cy = bin >= 0;
    const bool v = ((a ^ m) & (a ^ b8) & 0x80) != 0;
    int al = (int)(a & 0x0F) - (int)(m & 0x0F) + (int)c - 1;
    int s;
    if (cmos) {
        s = bin;
        if (s < 0)
            s -= 0x60;
        if (al < 0)
            s -= 0x06;
        const unsigned r = (unsigned)s & 0xFF;
        return pack(r, cy, r == 0, r >> 7, v);
    }
    if (al < 0)
        al = ((al - 0x06) & 0x0F) - 0x10;
    s = (int)(a & 0xF0) - (int)(m & 0xF0) + al;
    if (s < 0)
        s -= 0x60;
    return pack((unsigned)s, cy, b8 == 0, b8 >> 7, v);
}

/* ARR with D set (NMOS): t = A & #imm rotated right through C, then each digit
 * adjusted as by a decimal addition; N is the old C, Z and V are from the rotation. */
unsigned m6502_arr_dec(unsigned t, unsigned c)
{
    unsigned r = (t >> 1) | (c & 1) << 7;
    const bool n = c & 1, z = (r & 0xFF) == 0, v = ((r ^ t) & 0x40) != 0;
    if ((t & 0x0F) + (t & 0x01) > 5)
        r = (r & 0xF0) | ((r + 6) & 0x0F);
    bool cy = false;
    if ((t & 0xF0) + (t & 0x10) > 0x50) {
        r = (r + 0x60) & 0xFF;
        cy = true;
    }
    return pack(r, cy, z, n, v);
}

static uint64_t helper_adc_nmos(void *env, uint64_t a, uint64_t m, uint64_t c, uam_status *st)
{
    (void)env; (void)st;
    return m6502_adc_dec((unsigned)a, (unsigned)m, (unsigned)c & 1, false);
}
static uint64_t helper_adc_cmos(void *env, uint64_t a, uint64_t m, uint64_t c, uam_status *st)
{
    (void)env; (void)st;
    return m6502_adc_dec((unsigned)a, (unsigned)m, (unsigned)c & 1, true);
}
static uint64_t helper_sbc_nmos(void *env, uint64_t a, uint64_t m, uint64_t c, uam_status *st)
{
    (void)env; (void)st;
    return m6502_sbc_dec((unsigned)a, (unsigned)m, (unsigned)c & 1, false);
}
static uint64_t helper_sbc_cmos(void *env, uint64_t a, uint64_t m, uint64_t c, uam_status *st)
{
    (void)env; (void)st;
    return m6502_sbc_dec((unsigned)a, (unsigned)m, (unsigned)c & 1, true);
}
static uint64_t helper_arr_dec(void *env, uint64_t t, uint64_t c, uint64_t unused, uam_status *st)
{
    (void)env; (void)unused; (void)st;
    return m6502_arr_dec((unsigned)t, (unsigned)c & 1);
}

#define DEC3(n, f) { .name = n, .n_args = 3, .arg_width = { 8, 8, 8 }, .result_width = 16, \
                     .effect = UAM_HELPER_PURE, .fn = f }
const uam_helper_desc m6502_helpers[M6502_HELPER__COUNT] = {
    [M6502_HELPER_ADC_NMOS] = DEC3("m6502_adc_dec_nmos", helper_adc_nmos),
    [M6502_HELPER_ADC_CMOS] = DEC3("m6502_adc_dec_cmos", helper_adc_cmos),
    [M6502_HELPER_SBC_NMOS] = DEC3("m6502_sbc_dec_nmos", helper_sbc_nmos),
    [M6502_HELPER_SBC_CMOS] = DEC3("m6502_sbc_dec_cmos", helper_sbc_cmos),
    [M6502_HELPER_ARR_DEC]  = { .name = "m6502_arr_dec", .n_args = 2, .arg_width = { 8, 8 },
                                .result_width = 16, .effect = UAM_HELPER_PURE, .fn = helper_arr_dec },
};
#undef DEC3

/* ---- exceptions ------------------------------------------------------------------- */

/* A 6502 has no exceptions: an access nothing answers (with the board's unmapped policy
 * at "fault") halts the hart, with a message. */
static void m6502_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    m6502_state *s = m6502_st(cpu);
    uam_log(UAM_LOG_ERROR, "m6502", "pc=0x%04llx: %s at 0x%llx: core halted", (unsigned long long)e->pc,
            e->kind == UAM_EXC_FETCH_FAULT || e->kind == UAM_EXC_FETCH_XLATE ? "fetch fault"
            : e->kind == UAM_EXC_STORE_FAULT ? "store to nothing" : "load from nothing",
            (unsigned long long)e->tval);
    s->halted = true;
    cpu->pc = e->pc;
    cpu->waiting = true;
    uam_cpu_request_exit(cpu);
}

/* ---- lifecycle -------------------------------------------------------------------- */

static uam_status m6502_init(uam_cpu *cpu)
{
    m6502_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    s->cfg = uam_m6502_nmos;
    cpu->isa_state = s;
    return UAM_OK;
}

static void m6502_fini(uam_cpu *cpu)
{
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

/* Reset: I set, S = 0xFD, the pc from the reset vector (or `entry` if that cannot be
 * read); A, X, Y and the other flags 0 (the chip leaves them as they were). */
static void m6502_reset(uam_cpu *cpu, uint64_t entry)
{
    m6502_state *s = m6502_st(cpu);
    memset(cpu->regfile, 0, cpu->regfile_size);
    cpu->regfile[M6502_S] = 0xFD;
    cpu->regfile[M6502_PX] = UAM_M6502_P_I;
    s->nmi_pending = s->defer_irq = s->poll_i0 = false;
    s->stopped = 0;
    s->halted = false;
    s->last_line = 0;
    cpu->priv = 0;
    cpu->waiting = false;
    cpu->resv.valid = false;
    bool ok;
    const uint16_t v = vector(cpu, UAM_M6502_VEC_RESET, &ok);
    cpu->pc = ok ? v : entry & 0xFFFF;
    uam_cpu_xlate_changed(cpu);
}

/* ---- debugger view: A, X, Y, S, P, PC ----------------------------------------------- */

enum { G_A, G_X, G_Y, G_S, G_P, G_PC, G_N };
static const uint8_t m6502_gdb_sizes[G_N] = { 1, 1, 1, 1, 1, 2 };

#define G(n, num, bits) "<reg name=\"" n "\" bitsize=\"" #bits "\" regnum=\"" #num "\"/>"
static const char m6502_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><feature name=\"org.uam.m6502.core\">"
    G("a", 0, 8) G("x", 1, 8) G("y", 2, 8) G("sp", 3, 8) G("p", 4, 8) G("pc", 5, 16)
    "</feature></target>";
#undef G

static bool m6502_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    switch (n) {
    case G_A: case G_X: case G_Y: case G_S: *v = cpu->regfile[M6502_A + n]; break;
    case G_P:  *v = p_read(cpu, true); break;
    case G_PC: *v = cpu->pc & 0xFFFF; break;
    default:   return false;
    }
    return true;
}

static bool m6502_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    switch (n) {
    case G_A: case G_X: case G_Y: case G_S: cpu->regfile[M6502_A + n] = (uint8_t)v; break;
    case G_P:  p_write(cpu, (uint8_t)v); break;
    case G_PC: cpu->pc = v & 0xFFFF; break;
    default:   return false;
    }
    return true;
}

static const uam_gdb_arch m6502_gdb = {
    .target_xml = m6502_target_xml, .n_regs = G_N, .reg_bytes = 2, .reg_sizes = m6502_gdb_sizes,
    .read_reg = m6502_gdb_read, .write_reg = m6502_gdb_write,
};

static void m6502_dump_state(uam_cpu *cpu, FILE *out)
{
    const m6502_state *s = m6502_st(cpu);
    const uint8_t *r = cpu->regfile, p = p_read(cpu, false);
    char fl[9];
    static const char names[] = "NV-BDIZC";
    for (unsigned i = 0; i < 8; i++)
        fl[i] = (p >> (7 - i)) & 1 ? names[i] : (char)(names[i] | 0x20);
    fl[8] = 0;
    fprintf(out, "pc=%04llx a=%02x x=%02x y=%02x s=%02x p=%02x [%s] icount=%llu model=%s\n",
            (unsigned long long)cpu->pc, r[M6502_A], r[M6502_X], r[M6502_Y], r[M6502_S], p, fl,
            (unsigned long long)cpu->icount, s->cfg.name ? s->cfg.name : "?");
    fprintf(out, "irq lines=%08x nmi=%u%s%s%s\n", s->lines, s->nmi_level, s->nmi_pending ? " NMI-PENDING" : "",
            s->stopped == 1 ? " STOPPED" : s->stopped == 2 ? " JAMMED" : "", s->halted ? " HALTED" : "");
}

/* ---- snapshots -------------------------------------------------------------------- */

static void m6502_save(uam_cpu *cpu, uam_state_writer *w)
{
    const m6502_state *s = m6502_st(cpu);
    uam_state_put_u32(w, s->lines);
    const uint8_t b[] = { s->nmi_level, s->nmi_pending, s->defer_irq, s->poll_i0, s->stopped, s->halted };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_u32(w, s->last_line);
}

static void m6502_restore(uam_cpu *cpu, uam_state_reader *r)
{
    m6502_state *s = m6502_st(cpu);
    s->lines = uam_state_get_u32(r);
    uint8_t b[6];
    uam_state_get_bytes(r, b, sizeof(b));
    s->nmi_level = b[0] != 0;
    s->nmi_pending = b[1] != 0;
    s->defer_irq = b[2] != 0;
    s->poll_i0 = b[3] != 0;
    s->stopped = b[4] <= 2 ? b[4] : 2;
    s->halted = b[5] != 0;
    s->last_line = uam_state_get_u32(r) % 32u;
}

const uam_isa uam_isa_m6502 = {
    .name           = "m6502",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,   /* addresses are stored low byte first */
    .regfile_size   = M6502_REGFILE_SIZE,
    .regs           = m6502_regs,
    .n_regs         = UAM_ARRAY_LEN(m6502_regs),
    .helpers        = { m6502_helpers, M6502_HELPER__COUNT },
    .init           = m6502_init,
    .fini           = m6502_fini,
    .reset          = m6502_reset,
    .lift           = m6502_lift,
    .take_exception = m6502_take_exception,
    .irq_take       = m6502_irq_take,
    .irq_wakeup     = m6502_irq_wakeup,
    .irq_input      = m6502_irq_input,
    .sysreg_read    = m6502_sysreg_read,
    .sysreg_write   = m6502_sysreg_write,
    .gdb            = &m6502_gdb,
    .dump_state     = m6502_dump_state,
    .save           = m6502_save,
    .restore        = m6502_restore,
    .irq_lines      = m6502_irq_lines,
};
