/*
 * rv_system.c - RISC-V privileged architecture (machine mode only) and the
 * uam_isa descriptor.
 *
 * Implemented: M-mode CSRs, synchronous exception entry, interrupt entry
 * (direct and vectored mtvec), MRET, the WFI wake condition, and the mip bits
 * driven by interrupt lines (MSIP, MTIP, MEIP). There is no S/U mode, so
 * mstatus.MPP always reads as M.
 */
#include <stdlib.h>

#include "rv.h"

/* ------------------------------------------------------------------------ */
/* Register descriptors                                                      */
/* ------------------------------------------------------------------------ */

#define R(n, i) { n, RV_REG_OFF(i), 32, UAM_REG_GPR }
static const uam_regdesc rv_regs[] = {
    R("zero", 0), R("ra", 1),  R("sp", 2),   R("gp", 3),   R("tp", 4),  R("t0", 5),  R("t1", 6),
    R("t2", 7),   R("s0", 8),  R("s1", 9),   R("a0", 10),  R("a1", 11), R("a2", 12), R("a3", 13),
    R("a4", 14),  R("a5", 15), R("a6", 16),  R("a7", 17),  R("s2", 18), R("s3", 19), R("s4", 20),
    R("s5", 21),  R("s6", 22), R("s7", 23),  R("s8", 24),  R("s9", 25), R("s10", 26), R("s11", 27),
    R("t3", 28),  R("t4", 29), R("t5", 30),  R("t6", 31),
};
#undef R

/* ------------------------------------------------------------------------ */
/* Trap entry                                                                */
/* ------------------------------------------------------------------------ */

static void enter_trap(uam_cpu *cpu, uint32_t cause, uint32_t tval, uint32_t epc, bool interrupt)
{
    rv_state *s = rv_st(cpu);
    uint32_t mie = (s->mstatus & MSTATUS_MIE) ? 1 : 0;
    s->mstatus = (s->mstatus & ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP))
               | (mie ? MSTATUS_MPIE : 0) | ((uint32_t)cpu->priv << 11);
    s->mepc = epc & ~1u;
    s->mcause = cause | (interrupt ? 0x80000000u : 0);
    s->mtval = tval;
    cpu->priv = RV_PRIV_M;
    cpu->resv.valid = false;

    uint32_t base = s->mtvec & ~3u;
    bool vectored = (s->mtvec & 3) == 1;
    cpu->pc = (interrupt && vectored) ? base + 4 * cause : base;
}

/* Raw bits of the instruction at pc, for mtval on illegal instruction. */
static uint32_t insn_bits(uam_cpu *cpu, uint64_t pc)
{
    uint64_t lo = 0, hi = 0;
    if (uam_cpu_peek(cpu, pc, 2, &lo) != UAM_OK)
        return 0;
    if ((lo & 3) != 3)
        return (uint32_t)lo;
    if (uam_cpu_peek(cpu, pc + 2, 2, &hi) != UAM_OK)
        return 0;
    return (uint32_t)(lo | hi << 16);
}

static void rv_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    uint32_t cause, tval = (uint32_t)e->tval;
    switch (e->kind) {
    case UAM_EXC_TRAP:
        cause = (uint32_t)e->cause;
        if (cause == RV_EXC_ECALL_U)
            cause += cpu->priv; /* 8 = U, 9 = S, 11 = M */
        break;
    case UAM_EXC_FETCH_FAULT:      cause = RV_EXC_INSN_FAULT; break;
    case UAM_EXC_LOAD_FAULT:       cause = RV_EXC_LOAD_FAULT; break;
    case UAM_EXC_STORE_FAULT:      cause = RV_EXC_STORE_FAULT; break;
    case UAM_EXC_LOAD_MISALIGNED:  cause = RV_EXC_LOAD_MISALIGNED; break;
    case UAM_EXC_STORE_MISALIGNED: cause = RV_EXC_STORE_MISALIGNED; break;
    /* No translation is modelled (M-mode only); if one is added these are
     * the page-fault causes. */
    case UAM_EXC_FETCH_XLATE:      cause = RV_EXC_INSN_PAGE_FAULT; break;
    case UAM_EXC_LOAD_XLATE:       cause = RV_EXC_LOAD_PAGE_FAULT; break;
    case UAM_EXC_STORE_XLATE:      cause = RV_EXC_STORE_PAGE_FAULT; break;
    case UAM_EXC_SYSREG:
    case UAM_EXC_HELPER:
    default:
        cause = RV_EXC_ILLEGAL;
        tval = insn_bits(cpu, e->pc);
        break;
    }
    enter_trap(cpu, cause, tval, (uint32_t)e->pc, false);
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                                */
/* ------------------------------------------------------------------------ */

static uint32_t mip_value(const rv_state *s)
{
    return s->lines & ((1u << UAM_RV_IRQ_MSI) | (1u << UAM_RV_IRQ_MTI) | (1u << UAM_RV_IRQ_MEI));
}

static bool rv_irq_take(uam_cpu *cpu)
{
    rv_state *s = rv_st(cpu);
    uint32_t pending = mip_value(s) & s->mie;
    if (!pending || !(s->mstatus & MSTATUS_MIE))
        return false;
    /* Standard priority: MEI > MSI > MTI. */
    static const unsigned order[] = { UAM_RV_IRQ_MEI, UAM_RV_IRQ_MSI, UAM_RV_IRQ_MTI };
    for (unsigned k = 0; k < UAM_ARRAY_LEN(order); k++) {
        if (pending & (1u << order[k])) {
            enter_trap(cpu, order[k], 0, (uint32_t)cpu->pc, true);
            return true;
        }
    }
    return false;
}

static bool rv_irq_wakeup(uam_cpu *cpu)
{
    rv_state *s = rv_st(cpu);
    return (mip_value(s) & s->mie) != 0;
}

static void rv_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    rv_state *s = rv_st(cpu);
    if (line >= 32)
        return;
    if (level)
        s->lines |= 1u << line;
    else
        s->lines &= ~(1u << line);
}

/* ------------------------------------------------------------------------ */
/* CSRs                                                                      */
/* ------------------------------------------------------------------------ */

#define MISA_RV32IMAC ((1u << 30) | (1u << ('I' - 'A')) | (1u << ('M' - 'A')) \
                       | (1u << ('A' - 'A')) | (1u << ('C' - 'A')))
#define MIE_MASK ((1u << UAM_RV_IRQ_MSI) | (1u << UAM_RV_IRQ_MTI) | (1u << UAM_RV_IRQ_MEI))

static uint64_t counter(uam_cpu *cpu)
{
    return cpu->icount + rv_st(cpu)->counter_bias;
}

static uam_status rv_sysreg_read(uam_cpu *cpu, uint32_t csr, uint64_t *val)
{
    rv_state *s = rv_st(cpu);
    if (((csr >> 8) & 3) > cpu->priv)
        return UAM_ERR_ILLEGAL;

    switch (csr) {
    case CSR_MSTATUS:    *val = (s->mstatus & ~MSTATUS_MPP) | (RV_PRIV_M << 11); break;
    case CSR_MISA:       *val = MISA_RV32IMAC; break;
    case CSR_MIE:        *val = s->mie; break;
    case CSR_MIP:        *val = mip_value(s); break;
    case CSR_MTVEC:      *val = s->mtvec; break;
    case CSR_MSCRATCH:   *val = s->mscratch; break;
    case CSR_MEPC:       *val = s->mepc; break;
    case CSR_MCAUSE:     *val = s->mcause; break;
    case CSR_MTVAL:      *val = s->mtval; break;
    case CSR_MCOUNTEREN: *val = s->mcounteren; break;
    case CSR_MCOUNTINHIBIT: *val = s->mcountinhibit; break;
    case CSR_MCYCLE: case CSR_MINSTRET: case CSR_CYCLE: case CSR_INSTRET:
        *val = (uint32_t)counter(cpu);
        break;
    case CSR_MCYCLEH: case CSR_MINSTRETH: case CSR_CYCLEH: case CSR_INSTRETH:
        *val = (uint32_t)(counter(cpu) >> 32);
        break;
    case CSR_MVENDORID: case CSR_MARCHID: case CSR_MIMPID: case CSR_MCONFIGPTR:
        *val = 0;
        break;
    case CSR_MHARTID:    *val = cpu->hartid; break;
    default:
        /* PMP is accepted as read-as-zero / write-ignored so that firmware
         * which configures it at boot keeps running. */
        if (csr >= CSR_PMPCFG0 && csr <= CSR_PMPADDR63) {
            *val = 0;
            break;
        }
        return UAM_ERR_ILLEGAL;
    }
    return UAM_OK;
}

static uam_status rv_sysreg_write(uam_cpu *cpu, uint32_t csr, uint64_t v64)
{
    rv_state *s = rv_st(cpu);
    uint32_t v = (uint32_t)v64;
    if (((csr >> 10) & 3) == 3)
        return UAM_ERR_ILLEGAL; /* read-only CSR space */
    if (((csr >> 8) & 3) > cpu->priv)
        return UAM_ERR_ILLEGAL;

    switch (csr) {
    case CSR_MSTATUS:
        s->mstatus = (v & (MSTATUS_MIE | MSTATUS_MPIE)) | (RV_PRIV_M << 11);
        uam_cpu_request_exit(cpu); /* MIE may have enabled a pending interrupt */
        break;
    case CSR_MISA:       break; /* WARL: fixed */
    case CSR_MIE:
        s->mie = v & MIE_MASK;
        uam_cpu_request_exit(cpu);
        break;
    case CSR_MIP:        break; /* all implemented bits are hardware-driven */
    case CSR_MTVEC:
        /* WARL: modes 0 (direct) and 1 (vectored); reserved modes become direct. */
        s->mtvec = (v & ~3u) | ((v & 3) == 1 ? 1 : 0);
        break;
    case CSR_MSCRATCH:   s->mscratch = v; break;
    case CSR_MEPC:       s->mepc = v & ~1u; break;
    case CSR_MCAUSE:     s->mcause = v; break;
    case CSR_MTVAL:      s->mtval = v; break;
    case CSR_MCOUNTEREN: s->mcounteren = v; break;
    case CSR_MCOUNTINHIBIT: s->mcountinhibit = v; break;
    case CSR_MCYCLE: case CSR_MINSTRET: {
        uint64_t c = counter(cpu);
        s->counter_bias = ((c & ~UINT64_C(0xFFFFFFFF)) | v) - cpu->icount;
        break;
    }
    case CSR_MCYCLEH: case CSR_MINSTRETH: {
        uint64_t c = counter(cpu);
        s->counter_bias = (((uint64_t)v << 32) | (c & 0xFFFFFFFFu)) - cpu->icount;
        break;
    }
    default:
        if (csr >= CSR_PMPCFG0 && csr <= CSR_PMPADDR63)
            break;
        return UAM_ERR_ILLEGAL;
    }
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* MRET: restore interrupt enable and privilege, return the new pc. */
static uint64_t helper_mret(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c;
    uam_cpu *cpu = env;
    rv_state *s = rv_st(cpu);
    if (cpu->priv != RV_PRIV_M) {
        *st = UAM_ERR_ILLEGAL;
        return 0;
    }
    uint32_t mpie = (s->mstatus & MSTATUS_MPIE) ? 1 : 0;
    cpu->priv = (s->mstatus & MSTATUS_MPP) >> 11;
    s->mstatus = (s->mstatus & ~(MSTATUS_MIE | MSTATUS_MPP)) | (mpie ? MSTATUS_MIE : 0)
               | MSTATUS_MPIE | (RV_PRIV_M << 11); /* MPP <- least privileged mode (M only) */
    cpu->resv.valid = false;
    return s->mepc;
}

const uam_helper_desc rv_helpers[RV_HELPER__COUNT] = {
    [RV_HELPER_MRET] = {
        .name = "rv_mret", .n_args = 0, .result_width = 32,
        .effect = UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE | UAM_HELPER_MAY_FAULT,
        .fn = helper_mret,
    },
};

/* ------------------------------------------------------------------------ */
/* Lifecycle and descriptor                                                  */
/* ------------------------------------------------------------------------ */

static uam_status rv_init(uam_cpu *cpu)
{
    cpu->isa_state = calloc(1, sizeof(rv_state));
    return cpu->isa_state ? UAM_OK : UAM_ERR_NOMEM;
}

static void rv_fini(uam_cpu *cpu)
{
    free(cpu->isa_state);
    cpu->isa_state = NULL;
}

static void rv_reset(uam_cpu *cpu, uint64_t entry)
{
    rv_state *s = rv_st(cpu);
    uint32_t lines = s->lines; /* line levels belong to the devices, not the hart */
    *s = (rv_state){ 0 };
    s->lines = lines;
    s->mstatus = RV_PRIV_M << 11;
    for (size_t i = 0; i < cpu->regfile_size; i++)
        cpu->regfile[i] = 0;
    cpu->priv = RV_PRIV_M;
    cpu->pc = entry;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

/* ---- debugger view (gdb: org.gnu.gdb.riscv.cpu, x0-x31 then pc) ------------ */

#define RV_GDB_REG(n, num) "<reg name=\"" n "\" bitsize=\"32\" regnum=\"" #num "\"/>"
static const char rv_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>riscv:rv32</architecture><feature name=\"org.gnu.gdb.riscv.cpu\">"
    RV_GDB_REG("zero", 0) RV_GDB_REG("ra", 1) RV_GDB_REG("sp", 2) RV_GDB_REG("gp", 3)
    RV_GDB_REG("tp", 4) RV_GDB_REG("t0", 5) RV_GDB_REG("t1", 6) RV_GDB_REG("t2", 7)
    RV_GDB_REG("fp", 8) RV_GDB_REG("s1", 9) RV_GDB_REG("a0", 10) RV_GDB_REG("a1", 11)
    RV_GDB_REG("a2", 12) RV_GDB_REG("a3", 13) RV_GDB_REG("a4", 14) RV_GDB_REG("a5", 15)
    RV_GDB_REG("a6", 16) RV_GDB_REG("a7", 17) RV_GDB_REG("s2", 18) RV_GDB_REG("s3", 19)
    RV_GDB_REG("s4", 20) RV_GDB_REG("s5", 21) RV_GDB_REG("s6", 22) RV_GDB_REG("s7", 23)
    RV_GDB_REG("s8", 24) RV_GDB_REG("s9", 25) RV_GDB_REG("s10", 26) RV_GDB_REG("s11", 27)
    RV_GDB_REG("t3", 28) RV_GDB_REG("t4", 29) RV_GDB_REG("t5", 30) RV_GDB_REG("t6", 31)
    RV_GDB_REG("pc", 32)
    "</feature></target>";
#undef RV_GDB_REG

static bool rv_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n < 32)
        *v = uam_cpu_reg_read(cpu, RV_REG_OFF(n), 32);
    else if (n == 32)
        *v = (uint32_t)cpu->pc;
    else
        return false;
    return true;
}

static bool rv_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n == 0)
        return true;
    if (n < 32)
        uam_cpu_reg_write(cpu, RV_REG_OFF(n), 32, v);
    else if (n == 32)
        cpu->pc = (uint32_t)v;
    else
        return false;
    return true;
}

static const uam_gdb_arch rv_gdb = {
    .target_xml = rv_target_xml, .n_regs = 33, .reg_bytes = 4,
    .read_reg = rv_gdb_read, .write_reg = rv_gdb_write,
};

static void rv_dump_state(uam_cpu *cpu, FILE *out)
{
    rv_state *s = rv_st(cpu);
    fprintf(out, "pc=%08llx priv=%u icount=%llu\n", (unsigned long long)cpu->pc, cpu->priv,
            (unsigned long long)cpu->icount);
    for (unsigned i = 0; i < 32; i++)
        fprintf(out, "%-4s=%08llx%s", rv_regs[i].name,
                (unsigned long long)uam_cpu_reg_read(cpu, RV_REG_OFF(i), 32), (i % 4 == 3) ? "\n" : "  ");
    fprintf(out, "mstatus=%08x mie=%08x mip=%08x mtvec=%08x\nmepc=%08x mcause=%08x mtval=%08x\n",
            s->mstatus, s->mie, mip_value(s), s->mtvec, s->mepc, s->mcause, s->mtval);
}

const uam_isa uam_isa_rv32 = {
    .name           = "rv32imac",
    .addr_width     = 32,
    .default_endian = UAM_ACC_LE,
    .regfile_size   = RV_REGFILE_SIZE,
    .regs           = rv_regs,
    .n_regs         = UAM_ARRAY_LEN(rv_regs),
    .helpers        = { rv_helpers, RV_HELPER__COUNT },
    .init           = rv_init,
    .fini           = rv_fini,
    .reset          = rv_reset,
    .lift           = rv_lift,
    .take_exception = rv_take_exception,
    .irq_take       = rv_irq_take,
    .irq_wakeup     = rv_irq_wakeup,
    .irq_input      = rv_irq_input,
    .sysreg_read    = rv_sysreg_read,
    .sysreg_write   = rv_sysreg_write,
    .gdb            = &rv_gdb,
    .dump_state     = rv_dump_state,
};
