/*
 * z80_system.c - The Z80 core's system side and the uam_isa descriptor.
 *
 * The CPU models, reset, the interrupt flip-flops and mode behind SYSRD/SYSWR, interrupt
 * and NMI entry (with the board's acknowledge hook for IM 0 and IM 2), the debugger view
 * and snapshots.
 *
 * Not modelled: instruction timing (every instruction is one tick of virtual time), R
 * counting while HALT waits, IM 0 with an instruction other than RST p or CALL nn on the
 * data bus, and the NMOS core losing IFF2 in LD A,I/R when an interrupt is accepted
 * during it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80.h"
#include "uam/bus.h"

/* ---- models -------------------------------------------------------------------- */

const uam_z80_config uam_z80_nmos_ref = { .name = "z80-nmos-ref", .out_c_zero = 0x00 };
const uam_z80_config uam_z80_cmos_ref = { .name = "z80-cmos-ref", .out_c_zero = 0xFF };

uam_status uam_z80_configure(uam_cpu *cpu, const uam_z80_config *cfg)
{
    if (!cpu || !uam_z80_is(cpu->isa) || !cpu->isa_state || !cfg)
        return UAM_ERR_INVALID;
    z80_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

const uam_z80_config *uam_z80_config_of(uam_cpu *cpu)
{
    return cpu && uam_z80_is(cpu->isa) && cpu->isa_state ? &z80_st(cpu)->cfg : NULL;
}

void uam_z80_set_io_decode(uam_cpu *cpu, uint16_t mask)
{
    if (uam_z80_config_of(cpu))
        z80_st(cpu)->io_mask = mask;
}

void uam_z80_set_inta(uam_cpu *cpu, uam_z80_inta_fn fn, void *ctx)
{
    if (uam_z80_config_of(cpu)) {
        z80_st(cpu)->inta = fn;
        z80_st(cpu)->inta_ctx = ctx;
    }
}

void uam_z80_set_reti(uam_cpu *cpu, uam_z80_reti_fn fn, void *ctx)
{
    if (uam_z80_config_of(cpu)) {
        z80_st(cpu)->reti = fn;
        z80_st(cpu)->reti_ctx = ctx;
    }
}

/* ---- registers ------------------------------------------------------------------ */

static const uam_regdesc z80_regs[] = {
    { "af", Z80_AF, 16, UAM_REG_GPR },   { "bc", Z80_BC, 16, UAM_REG_GPR },
    { "de", Z80_DE, 16, UAM_REG_GPR },   { "hl", Z80_HL, 16, UAM_REG_GPR },
    { "ix", Z80_IX, 16, UAM_REG_GPR },   { "iy", Z80_IY, 16, UAM_REG_GPR },
    { "sp", Z80_SP, 16, UAM_REG_OTHER }, { "af'", Z80_AF2, 16, UAM_REG_GPR },
    { "bc'", Z80_BC2, 16, UAM_REG_GPR }, { "de'", Z80_DE2, 16, UAM_REG_GPR },
    { "hl'", Z80_HL2, 16, UAM_REG_GPR }, { "i", Z80_I, 8, UAM_REG_OTHER },
    { "rcnt", Z80_RCNT, 8, UAM_REG_OTHER }, { "r7", Z80_R7, 8, UAM_REG_OTHER },
    { "qv", Z80_QV, 8, UAM_REG_FLAG },   { "wz", Z80_WZ, 16, UAM_REG_OTHER },
};

static uint8_t r_read(const uam_cpu *cpu)
{
    return (uint8_t)((cpu->regfile[Z80_R7] & 0x80) | (cpu->regfile[Z80_RCNT] & 0x7F));
}

static void r_write(uam_cpu *cpu, uint8_t v)
{
    cpu->regfile[Z80_RCNT] = v;
    cpu->regfile[Z80_R7] = v & 0x80;
}

/* ---- the flip-flops behind SYSRD/SYSWR -------------------------------------------- */

static uam_status z80_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    const z80_state *s = z80_st(cpu);
    switch (id) {
    case Z80_SYS_IFF: *val = (uint64_t)s->iff1 | (uint64_t)s->iff2 << 1; return UAM_OK;
    case Z80_SYS_IM:  *val = s->im; return UAM_OK;
    default:          return UAM_ERR_ILLEGAL;
    }
}

static uam_status z80_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    z80_state *s = z80_st(cpu);
    switch (id) {
    case Z80_SYS_IFF:
        s->iff1 = val & 1;
        s->iff2 = (val >> 1) & 1;
        if (s->iff1) {           /* EI: not before the next instruction has run */
            s->ei_shadow = true;
            uam_cpu_request_exit(cpu);
        }
        return UAM_OK;
    case Z80_SYS_IM:
        s->im = (uint8_t)(val & 3);
        return UAM_OK;
    case Z80_SYS_RETN:
        s->iff1 = s->iff2;
        if (val && s->reti)
            s->reti(s->reti_ctx);
        uam_cpu_request_exit(cpu);
        return UAM_OK;
    case Z80_SYS_SHADOW:
        s->ei_shadow = false;
        return UAM_OK;
    default:
        return UAM_ERR_ILLEGAL;
    }
}

/* ---- interrupts ------------------------------------------------------------------- */

static uint8_t bus_byte(uam_cpu *cpu, uint16_t a)
{
    uint64_t v = 0xFF;
    uam_bus_read(cpu->bus, a, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}

static void push_pc(uam_cpu *cpu, uint16_t pc)
{
    uint16_t sp = (uint16_t)uam_cpu_reg_read(cpu, Z80_SP, 16);
    sp = (uint16_t)(sp - 1);
    uam_bus_write(cpu->bus, sp, 1, UAM_ACC_LE, pc >> 8);
    sp = (uint16_t)(sp - 1);
    uam_bus_write(cpu->bus, sp, 1, UAM_ACC_LE, pc & 0xFF);
    uam_cpu_reg_write(cpu, Z80_SP, 16, sp);
}

static uint8_t inta(z80_state *s, unsigned n) { return s->inta ? s->inta(s->inta_ctx, n) : 0xFF; }

/* Enter a call to `target` from an interrupt: one opcode fetch for R, Q cleared. */
static void enter(uam_cpu *cpu, uint16_t target)
{
    push_pc(cpu, (uint16_t)cpu->pc);
    cpu->regfile[Z80_RCNT]++;
    cpu->regfile[Z80_QV] = 0;
    uam_cpu_reg_write(cpu, Z80_WZ, 16, target);
    cpu->pc = target;
    cpu->waiting = false;
}

static bool z80_irq_take(uam_cpu *cpu)
{
    z80_state *s = z80_st(cpu);
    if (s->halted)
        return false;
    if (s->nmi_pending) {
        s->nmi_pending = false;
        s->iff1 = false;
        s->last_line = UAM_Z80_LINE_NMI;
        enter(cpu, 0x0066);
        return true;
    }
    if (s->ei_shadow) {
        /* The block ended at EI: let one instruction run, then look again. */
        s->ei_shadow = false;
        if (s->int_line && s->iff1)
            uam_cpu_request_exit(cpu);
        return false;
    }
    if (!s->int_line || !s->iff1)
        return false;
    s->iff1 = s->iff2 = false;
    s->last_line = UAM_Z80_LINE_INT;
    switch (s->im) {
    case 2: {
        const uint16_t v = (uint16_t)(cpu->regfile[Z80_I] << 8 | inta(s, 0));
        enter(cpu, (uint16_t)(bus_byte(cpu, v) | bus_byte(cpu, (uint16_t)(v + 1)) << 8));
        break;
    }
    case 1:
        (void)inta(s, 0);   /* the acknowledge cycle runs in every mode */
        enter(cpu, 0x0038);
        break;
    default: {
        const uint8_t op = inta(s, 0);
        if ((op & 0xC7) == 0xC7) {
            enter(cpu, op & 0x38);
        } else if (op == 0xCD) {
            const uint8_t lo = inta(s, 1);
            enter(cpu, (uint16_t)(lo | inta(s, 2) << 8));
        } else {
            uam_log(UAM_LOG_WARN, "z80", "pc=0x%04llx: IM 0 with 0x%02x on the data bus: not executed",
                    (unsigned long long)cpu->pc, op);
            cpu->waiting = false;
        }
        break;
    }
    }
    return true;
}

static bool z80_irq_wakeup(uam_cpu *cpu)
{
    const z80_state *s = z80_st(cpu);
    return !s->halted && (s->nmi_pending || (s->int_line && s->iff1));
}

static void z80_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    z80_state *s = z80_st(cpu);
    if (line == UAM_Z80_LINE_INT) {
        s->int_line = level != 0;
    } else if (line == UAM_Z80_LINE_NMI) {
        if (level && !s->nmi_line)
            s->nmi_pending = true;
        s->nmi_line = level != 0;
    }
}

static uint64_t z80_irq_lines(uam_cpu *cpu)
{
    return UINT64_C(1) << z80_st(cpu)->last_line;
}

/* ---- exceptions, translation ------------------------------------------------------ */

/* A Z80 has no exceptions: an access nothing answers (with the board's unmapped policy at
 * "fault") halts the hart, with a message. */
static void z80_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    z80_state *s = z80_st(cpu);
    uam_log(UAM_LOG_ERROR, "z80", "pc=0x%04llx: %s at 0x%llx: core halted",
            (unsigned long long)e->pc,
            e->kind == UAM_EXC_FETCH_FAULT || e->kind == UAM_EXC_FETCH_XLATE ? "fetch fault"
            : e->kind == UAM_EXC_STORE_FAULT ? "store to nothing" : "load from nothing",
            (unsigned long long)e->tval);
    s->halted = true;
    cpu->pc = e->pc;
    cpu->waiting = true;
    uam_cpu_request_exit(cpu);
}

/* ---- lifecycle -------------------------------------------------------------------- */

static uam_status z80_init(uam_cpu *cpu)
{
    z80_state *s = calloc(1, sizeof(*s));
    if (!s)
        return UAM_ERR_NOMEM;
    s->cfg = uam_z80_nmos_ref;
    s->io_mask = 0xFFFF;
    cpu->isa_state = s;
    return UAM_OK;
}

static void z80_fini(uam_cpu *cpu)
{
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

/* Reset: PC 0, IFF1 = IFF2 = 0, IM 0, I = R = 0; AF and SP all ones, as most cores leave
 * them (the other registers are left as they were; a fresh hart has them zeroed). */
static void z80_reset(uam_cpu *cpu, uint64_t entry)
{
    z80_state *s = z80_st(cpu);
    uam_cpu_reg_write(cpu, Z80_AF, 16, 0xFFFF);
    uam_cpu_reg_write(cpu, Z80_SP, 16, 0xFFFF);
    cpu->regfile[Z80_I] = 0;
    r_write(cpu, 0);
    cpu->regfile[Z80_QV] = 0;
    uam_cpu_reg_write(cpu, Z80_WZ, 16, 0);
    s->iff1 = s->iff2 = false;
    s->im = 0;
    s->nmi_pending = false;
    s->ei_shadow = false;
    s->halted = false;
    s->last_line = 0;
    cpu->priv = 0;
    cpu->pc = entry & 0xFFFF;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

/* ---- debugger view: AF BC DE HL SP PC IX IY AF' BC' DE' HL' IR (GNU gdb's z80 order) --- */

enum { G_AF, G_BC, G_DE, G_HL, G_SP, G_PC, G_IX, G_IY, G_AF2, G_BC2, G_DE2, G_HL2, G_IR, G_N };
static const uint32_t gdb_off[G_N] = { Z80_AF, Z80_BC, Z80_DE, Z80_HL, Z80_SP, 0, Z80_IX, Z80_IY,
                                       Z80_AF2, Z80_BC2, Z80_DE2, Z80_HL2, 0 };

#define G(n, num) "<reg name=\"" n "\" bitsize=\"16\" regnum=\"" #num "\"/>"
static const char z80_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><feature name=\"org.uam.z80.core\">"
    G("af", 0) G("bc", 1) G("de", 2) G("hl", 3) G("sp", 4) G("pc", 5) G("ix", 6) G("iy", 7)
    G("af'", 8) G("bc'", 9) G("de'", 10) G("hl'", 11) G("ir", 12)
    "</feature></target>";
#undef G

static bool z80_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n >= G_N)
        return false;
    if (n == G_PC)
        *v = cpu->pc & 0xFFFF;
    else if (n == G_IR)
        *v = (uint64_t)cpu->regfile[Z80_I] << 8 | r_read(cpu);
    else
        *v = uam_cpu_reg_read(cpu, gdb_off[n], 16);
    return true;
}

static bool z80_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n >= G_N)
        return false;
    if (n == G_PC) {
        cpu->pc = v & 0xFFFF;
    } else if (n == G_IR) {
        cpu->regfile[Z80_I] = (uint8_t)(v >> 8);
        r_write(cpu, (uint8_t)v);
    } else {
        uam_cpu_reg_write(cpu, gdb_off[n], 16, v);
    }
    return true;
}

static const uam_gdb_arch z80_gdb = {
    .target_xml = z80_target_xml, .n_regs = G_N, .reg_bytes = 2,
    .read_reg = z80_gdb_read, .write_reg = z80_gdb_write,
};

static void z80_dump_state(uam_cpu *cpu, FILE *out)
{
    const z80_state *s = z80_st(cpu);
    const uint8_t *r = cpu->regfile;
#define W(o) ((unsigned)uam_cpu_reg_read(cpu, (o), 16))
    fprintf(out, "pc=%04llx af=%04x bc=%04x de=%04x hl=%04x ix=%04x iy=%04x sp=%04x icount=%llu model=%s\n",
            (unsigned long long)cpu->pc, W(Z80_AF), W(Z80_BC), W(Z80_DE), W(Z80_HL), W(Z80_IX), W(Z80_IY),
            W(Z80_SP), (unsigned long long)cpu->icount, s->cfg.name ? s->cfg.name : "?");
    fprintf(out, "af'=%04x bc'=%04x de'=%04x hl'=%04x i=%02x r=%02x wz=%04x q=%u iff1=%u iff2=%u im=%u int=%u nmi=%u%s\n",
            W(Z80_AF2), W(Z80_BC2), W(Z80_DE2), W(Z80_HL2), r[Z80_I], r_read(cpu), W(Z80_WZ), r[Z80_QV],
            s->iff1, s->iff2, s->im, s->int_line, s->nmi_pending, s->halted ? " HALTED" : "");
#undef W
}

/* ---- snapshots -------------------------------------------------------------------- */

static void z80_save(uam_cpu *cpu, uam_state_writer *w)
{
    const z80_state *s = z80_st(cpu);
    const uint8_t b[] = { s->iff1, s->iff2, s->im, s->int_line, s->nmi_line, s->nmi_pending,
                          s->ei_shadow, s->halted };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_u32(w, s->io_mask);
    uam_state_put_u32(w, s->last_line);
}

static void z80_restore(uam_cpu *cpu, uam_state_reader *r)
{
    z80_state *s = z80_st(cpu);
    uint8_t b[8];
    uam_state_get_bytes(r, b, sizeof(b));
    s->iff1 = b[0] != 0; s->iff2 = b[1] != 0; s->im = b[2] & 3; s->int_line = b[3] != 0;
    s->nmi_line = b[4] != 0; s->nmi_pending = b[5] != 0; s->ei_shadow = b[6] != 0; s->halted = b[7] != 0;
    s->io_mask = (uint16_t)uam_state_get_u32(r);
    s->last_line = uam_state_get_u32(r) & 1;
}

const uam_isa uam_isa_z80 = {
    .name           = "z80",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,
    .regfile_size   = Z80_REGFILE_SIZE,
    .regs           = z80_regs,
    .n_regs         = UAM_ARRAY_LEN(z80_regs),
    .init           = z80_init,
    .fini           = z80_fini,
    .reset          = z80_reset,
    .lift           = z80_lift,
    .take_exception = z80_take_exception,
    .irq_take       = z80_irq_take,
    .irq_wakeup     = z80_irq_wakeup,
    .irq_input      = z80_irq_input,
    .sysreg_read    = z80_sysreg_read,
    .sysreg_write   = z80_sysreg_write,
    .gdb            = &z80_gdb,
    .dump_state     = z80_dump_state,
    .save           = z80_save,
    .restore        = z80_restore,
    .irq_lines      = z80_irq_lines,
};
