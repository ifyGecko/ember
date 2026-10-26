/*
 * ppc_system.c - 32-bit PowerPC operating environment and the uam_isa descriptor.
 *
 * The MSR and the privilege it gives (cpu->priv carries PR, FP, SE and BE for the
 * lifter), the supervisor SPRs (SRR0/SRR1, DAR, DSISR, SPRG0-3, EAR, SDR1, PVR,
 * the BATs, the time base and the decrementer), the segment registers, the
 * exception model (vectors at 0 or, with MSR[IP], 0xFFF00000; SRR0/SRR1; the MSR
 * an interrupt leaves; rfi), the interrupt inputs (external, soft reset, machine
 * check) and the trace exception, the decrementer and time base driven by the
 * scheduler, the helpers the lifter calls, snapshots and the gdb view.
 *
 * Synchronous exceptions:
 *   0x200 machine check     a bus error on fetch or data access (checkstop if MSR[ME] = 0)
 *   0x300 DSI               data translation or protection fault (DAR, DSISR)
 *   0x400 ISI               fetch translation, no-execute or protection fault (SRR1)
 *   0x600 alignment         lwarx/stwcx./lmw/stmw on a misaligned address (DAR, DSISR)
 *   0x700 program           illegal or privileged instruction, trap, FP enabled (SRR1)
 *   0x800 FP unavailable    an FP instruction with MSR[FP] = 0
 *   0xC00 system call       sc (SRR0 = the next instruction)
 *   0xD00 trace             after an instruction with MSR[SE] (a branch with MSR[BE])
 * Asynchronous: 0x100 soft reset, 0x200 machine check pin, 0x500 external, 0x900
 * decrementer (the last two while MSR[EE] is set, external first).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/sched.h"

#include "ppc.h"

/* ------------------------------------------------------------------------ */
/* Register descriptors                                                      */
/* ------------------------------------------------------------------------ */

#define G(n, i) { n, PPC_GPR_OFF(i), 32, UAM_REG_GPR }
#define F(n, i) { n, PPC_FPR_OFF(i), 64, UAM_REG_OTHER }
static const uam_regdesc ppc_regs[] = {
    G("r0", 0),   G("r1", 1),   G("r2", 2),   G("r3", 3),   G("r4", 4),   G("r5", 5),
    G("r6", 6),   G("r7", 7),   G("r8", 8),   G("r9", 9),   G("r10", 10), G("r11", 11),
    G("r12", 12), G("r13", 13), G("r14", 14), G("r15", 15), G("r16", 16), G("r17", 17),
    G("r18", 18), G("r19", 19), G("r20", 20), G("r21", 21), G("r22", 22), G("r23", 23),
    G("r24", 24), G("r25", 25), G("r26", 26), G("r27", 27), G("r28", 28), G("r29", 29),
    G("r30", 30), G("r31", 31),
    { "lr", PPC_LR_OFF, 32, UAM_REG_OTHER }, { "ctr", PPC_CTR_OFF, 32, UAM_REG_OTHER },
    { "fpscr", PPC_FPSCR_OFF, 32, UAM_REG_OTHER },
    { "so", PPC_SO_OFF, 8, UAM_REG_FLAG }, { "ov", PPC_OV_OFF, 8, UAM_REG_FLAG },
    { "ca", PPC_CA_OFF, 8, UAM_REG_FLAG }, { "xer_bc", PPC_BC_OFF, 8, UAM_REG_OTHER },
    { "cr0", PPC_CR_OFF(0), 8, UAM_REG_FLAG }, { "cr1", PPC_CR_OFF(1), 8, UAM_REG_FLAG },
    { "cr2", PPC_CR_OFF(2), 8, UAM_REG_FLAG }, { "cr3", PPC_CR_OFF(3), 8, UAM_REG_FLAG },
    { "cr4", PPC_CR_OFF(4), 8, UAM_REG_FLAG }, { "cr5", PPC_CR_OFF(5), 8, UAM_REG_FLAG },
    { "cr6", PPC_CR_OFF(6), 8, UAM_REG_FLAG }, { "cr7", PPC_CR_OFF(7), 8, UAM_REG_FLAG },
    F("f0", 0),   F("f1", 1),   F("f2", 2),   F("f3", 3),   F("f4", 4),   F("f5", 5),
    F("f6", 6),   F("f7", 7),   F("f8", 8),   F("f9", 9),   F("f10", 10), F("f11", 11),
    F("f12", 12), F("f13", 13), F("f14", 14), F("f15", 15), F("f16", 16), F("f17", 17),
    F("f18", 18), F("f19", 19), F("f20", 20), F("f21", 21), F("f22", 22), F("f23", 23),
    F("f24", 24), F("f25", 25), F("f26", 26), F("f27", 27), F("f28", 28), F("f29", 29),
    F("f30", 30), F("f31", 31),
};
#undef G
#undef F

/* ------------------------------------------------------------------------ */
/* CPU models                                                                */
/* ------------------------------------------------------------------------ */

const uam_ppc_config uam_ppc_ref = {
    .name        = "ppc32-ref",
    .pvr         = 0x00FF0000u,   /* version 0x00FF: no implementation, revision 0 */
    .fpu         = true,
    .bat_pairs   = 4,
    .cache_block = 32,
    .reset_ip    = true,
    .tb_div      = 1,
};

static bool valid_config(const uam_ppc_config *c)
{
    return (c->bat_pairs == 4 || c->bat_pairs == 8) && c->cache_block >= 16
        && c->cache_block <= 128 && !(c->cache_block & (c->cache_block - 1)) && c->tb_div >= 1;
}

uam_status uam_ppc_configure(uam_cpu *cpu, const uam_ppc_config *cfg)
{
    if (!cpu || !cfg || !uam_ppc_is(cpu->isa) || !cpu->isa_state || !valid_config(cfg))
        return UAM_ERR_INVALID;
    ppc_st(cpu)->cfg = *cfg;
    return UAM_OK;
}

uam_status uam_ppc_set_spr_ops(uam_cpu *cpu, const uam_ppc_spr_ops *ops, void *ctx)
{
    if (!cpu || !uam_ppc_is(cpu->isa) || !cpu->isa_state)
        return UAM_ERR_INVALID;
    ppc_st(cpu)->spr_ops = ops;
    ppc_st(cpu)->spr_ctx = ctx;
    return UAM_OK;
}

/* Every PowerPC hart, for the tlbie broadcast (harts share a bus within a machine). */
static ppc_state *all_harts;

/* ------------------------------------------------------------------------ */
/* MSR                                                                       */
/* ------------------------------------------------------------------------ */

static unsigned priv_of(uint32_t msr)
{
    return ((msr & MSR_PR) ? PPC_PRIV_PR : 0) | ((msr & MSR_FP) ? PPC_PRIV_FP : 0)
         | ((msr & MSR_SE) ? PPC_PRIV_SE : 0) | ((msr & MSR_BE) ? PPC_PRIV_BE : 0);
}

static void dec_timer_update(uam_cpu *cpu);

/* Set the MSR. Writes to LE and ILE are ignored (the processor is big-endian), and
 * so is FP without an FPU. */
void ppc_msr_write(uam_cpu *cpu, uint32_t v)
{
    ppc_state *s = ppc_st(cpu);
    uint32_t mask = MSR_POW | MSR_EE | MSR_PR | MSR_ME | MSR_FE0 | MSR_SE | MSR_BE | MSR_FE1
                  | MSR_IP | MSR_IR | MSR_DR | MSR_RI | (s->cfg.fpu ? MSR_FP : 0);
    uint32_t old = s->s.msr;
    s->s.msr = v & mask;
    if ((old ^ s->s.msr) & (MSR_IR | MSR_DR | MSR_PR)) {
        ppc_tlb_flush(s);
        uam_cpu_xlate_changed(cpu);
    }
    cpu->priv = priv_of(s->s.msr);
    if ((old ^ s->s.msr) & MSR_EE)
        dec_timer_update(cpu);
    uam_cpu_request_exit(cpu);   /* EE may have enabled a pending interrupt */
}

/* ------------------------------------------------------------------------ */
/* Time base and decrementer                                                 */
/* ------------------------------------------------------------------------ */

static uint64_t ticks_now(uam_cpu *cpu)
{
    return uam_sched_now(cpu->sched) / ppc_st(cpu)->cfg.tb_div;
}

static uint64_t tb_now(uam_cpu *cpu) { return ticks_now(cpu) + ppc_st(cpu)->s.tb_bias; }

static uint32_t dec_now(uam_cpu *cpu)
{
    const ppc_state *s = ppc_st(cpu);
    return s->s.dec - (uint32_t)(ticks_now(cpu) - s->s.dec_ticks);
}

/*
 * The decrementer raises its exception when its most significant bit changes from 0
 * to 1, at tick dec_next and every 2^32 ticks after. The request is latched
 * (dec_pending) when time reaches it; a scheduler timer marks that moment only while
 * MSR[EE] lets the exception be taken, and otherwise the latch catches up whenever
 * EE is set again or DEC is written.
 */
static void dec_sync(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    if (!cpu->sched)
        return;
    uint64_t now = ticks_now(cpu);
    if (now >= s->s.dec_next) {
        s->s.dec_pending = true;
        s->s.dec_next += (((now - s->s.dec_next) >> 32) + 1) << 32;
    }
}

static void dec_timer_update(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    if (!cpu->sched)
        return;
    dec_sync(cpu);
    if ((s->s.msr & MSR_EE) && !s->s.checkstop)
        uam_timer_arm(cpu->sched, &s->dec_timer, s->s.dec_next * s->cfg.tb_div);
    else
        uam_timer_cancel(cpu->sched, &s->dec_timer);
}

static void dec_fire(void *ctx)
{
    uam_cpu *cpu = ctx;
    dec_timer_update(cpu);
    uam_cpu_request_exit(cpu);
}

static void dec_write(uam_cpu *cpu, uint32_t v)
{
    ppc_state *s = ppc_st(cpu);
    dec_sync(cpu);
    s->s.dec = v;
    s->s.dec_ticks = cpu->sched ? ticks_now(cpu) : 0;
    s->s.dec_next = s->s.dec_ticks + (uint64_t)v + 1;
    dec_timer_update(cpu);
}

/* ------------------------------------------------------------------------ */
/* Exceptions and interrupts                                                 */
/* ------------------------------------------------------------------------ */

enum {
    VEC_SRESET = 0x100, VEC_MCHECK = 0x200, VEC_DSI = 0x300, VEC_ISI = 0x400, VEC_EXT = 0x500,
    VEC_ALIGN = 0x600, VEC_PROGRAM = 0x700, VEC_FPU = 0x800, VEC_DEC = 0x900, VEC_SC = 0xC00,
    VEC_TRACE = 0xD00,
};

/* Enter the interrupt at `vec`: SRR0 = srr0, SRR1 = the MSR's saved bits | extra. */
static void enter(uam_cpu *cpu, uint32_t vec, uint32_t srr0, uint32_t extra)
{
    ppc_state *s = ppc_st(cpu);
    uint32_t msr = s->s.msr;
    s->s.srr0 = srr0;
    s->s.srr1 = (msr & MSR_SRR1_MASK) | extra;
    s->s.trace_pending = false;
    cpu->resv.valid = false;
    uint32_t keep = MSR_IP | (vec == VEC_MCHECK ? 0 : MSR_ME);
    cpu->pc = ((msr & MSR_IP) ? 0xFFF00000u : 0) | vec;
    ppc_msr_write(cpu, msr & keep);
}

/* A machine check: taken with MSR[ME] set, else the processor checkstops. */
static void machine_check(uam_cpu *cpu, uint32_t srr0)
{
    ppc_state *s = ppc_st(cpu);
    if (s->s.msr & MSR_ME) {
        enter(cpu, VEC_MCHECK, srr0, 0);
        return;
    }
    uam_log(UAM_LOG_ERROR, "ppc32", "cpu %u: checkstop (machine check with MSR[ME] = 0) at 0x%08x",
            cpu->hartid, srr0);
    s->s.checkstop = true;
    cpu->pc = srr0;              /* stopped at the instruction (or boundary) concerned */
    cpu->waiting = true;
    dec_timer_update(cpu);
}

/* DSISR of an alignment exception: the instruction's opcode and register fields. */
static uint32_t align_dsisr(uam_cpu *cpu, uint32_t pc)
{
    uint32_t i = 0;
    if (!ppc_peek_insn(cpu, pc, &i))
        return 0;
    uint32_t d = ((i >> 21) & 31) << 5 | ((i >> 16) & 31);
    if ((i >> 26) == 31)   /* X form: XO bits 29-30, 25, 21-24 */
        return d | ((i >> 1) & 3) << 15 | ((i >> 6) & 1) << 14 | ((i >> 7) & 0xF) << 10;
    return d | ((i >> 26) & 1) << 14 | ((i >> 27) & 0xF) << 10;   /* D form: bits 5, 1-4 */
}

static void data_exception(uam_cpu *cpu, uint32_t pc, uint32_t ea, uint32_t dsisr)
{
    ppc_state *s = ppc_st(cpu);
    s->s.dar = ea;
    s->s.dsisr = dsisr;
    enter(cpu, VEC_DSI, pc, 0);
}

static void alignment(uam_cpu *cpu, uint32_t pc, uint32_t ea)
{
    ppc_state *s = ppc_st(cpu);
    s->s.dar = ea;
    s->s.dsisr = align_dsisr(cpu, pc);
    enter(cpu, VEC_ALIGN, pc, 0);
}

static void ppc_take_exception(uam_cpu *cpu, const uam_exception *e)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t pc = (uint32_t)e->pc, va = (uint32_t)e->tval;
    switch (e->kind) {
    case UAM_EXC_TRAP:
        switch (e->cause) {
        case PPC_TRAP_SC:   enter(cpu, VEC_SC, pc + 4, 0); break;
        case PPC_TRAP_FPU:  enter(cpu, VEC_FPU, pc, 0); break;
        case PPC_TRAP_PRIV: enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_PRIV); break;
        case PPC_TRAP_TRAP: enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_TRAP); break;
        default:            enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_ILL); break;
        }
        break;
    case UAM_EXC_FETCH_XLATE:
        enter(cpu, VEC_ISI, pc, (uint32_t)e->cause & (SRR1_ISI_NOPTE | SRR1_ISI_NOEXEC | SRR1_ISI_PROT));
        break;
    case UAM_EXC_LOAD_XLATE:  data_exception(cpu, pc, va, (uint32_t)e->cause); break;
    case UAM_EXC_STORE_XLATE: data_exception(cpu, pc, va, (uint32_t)e->cause | DSISR_STORE); break;
    case UAM_EXC_LOAD_MISALIGNED:
    case UAM_EXC_STORE_MISALIGNED:
        alignment(cpu, pc, va);
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
        case PPC_HF_FP_ENABLED: enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_FP); break;
        case PPC_HF_PRIV:       enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_PRIV); break;
        case PPC_HF_DSI:        data_exception(cpu, pc, s->hf_ea, s->hf_dsisr); break;
        case PPC_HF_ALIGN:      alignment(cpu, pc, s->hf_ea); break;
        case PPC_HF_MCHECK:     machine_check(cpu, pc); break;
        default:                enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_ILL); break;
        }
        break;
    }
    default:   /* UAM_EXC_SYSREG: an SPR the processor does not have */
        enter(cpu, VEC_PROGRAM, pc, SRR1_PROG_ILL);
        break;
    }
}

static bool ext_pending(const ppc_state *s) { return s->intlines != 0; }

static bool ppc_irq_take(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t pc = (uint32_t)cpu->pc;
    if (s->s.checkstop)
        return false;
    if (s->s.sreset_pending) {
        s->s.sreset_pending = false;
        s->taken_lines = UINT64_C(1) << UAM_PPC_IRQ_SRESET;
        enter(cpu, VEC_SRESET, pc, 0);
        return true;
    }
    if (s->s.mcp_pending) {
        s->s.mcp_pending = false;
        s->taken_lines = UINT64_C(1) << UAM_PPC_IRQ_MCP;
        machine_check(cpu, pc);
        return !s->s.checkstop;
    }
    if (s->s.trace_pending) {
        s->taken_lines = 0;
        enter(cpu, VEC_TRACE, pc, 0);
        return true;
    }
    if (!(s->s.msr & MSR_EE))
        return false;
    if (ext_pending(s)) {
        s->taken_lines = s->intlines;
        enter(cpu, VEC_EXT, pc, 0);
        return true;
    }
    if (s->s.dec_pending) {
        s->s.dec_pending = false;   /* taking it cancels the request */
        s->taken_lines = 0;
        enter(cpu, VEC_DEC, pc, 0);
        return true;
    }
    return false;
}

static uint64_t ppc_irq_lines(uam_cpu *cpu) { return ppc_st(cpu)->taken_lines; }

/* A sleeping processor (MSR[POW]) wakes by taking an exception: soft reset and machine
 * check always, the external interrupt and the decrementer while MSR[EE] is set. */
static bool ppc_irq_wakeup(uam_cpu *cpu)
{
    const ppc_state *s = ppc_st(cpu);
    if (s->s.checkstop)
        return false;
    if (s->s.sreset_pending || s->s.mcp_pending)
        return true;
    return (s->s.msr & MSR_EE) && (ext_pending(s) || s->s.dec_pending);
}

static void ppc_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    ppc_state *s = ppc_st(cpu);
    if (line < UAM_PPC_IRQ_INT_LINES) {
        if (level)
            s->intlines |= (uint8_t)(1u << line);
        else
            s->intlines &= (uint8_t)~(1u << line);
    } else if (line == UAM_PPC_IRQ_SRESET) {
        if (level && !s->sreset_level)
            s->s.sreset_pending = true;
        s->sreset_level = level;
    } else if (line == UAM_PPC_IRQ_MCP) {
        if (level && !s->mcp_level)
            s->s.mcp_pending = true;
        s->mcp_level = level;
    }
}

/* ------------------------------------------------------------------------ */
/* Special purpose and segment registers                                     */
/* ------------------------------------------------------------------------ */

/* A BAT register by SPR number: &bat[n][0 upper / 1 lower], or NULL. */
static uint32_t *bat_reg(ppc_state *s, unsigned spr)
{
    unsigned k;
    if (spr >= SPR_IBAT0U && spr <= SPR_DBAT3L)
        k = spr - SPR_IBAT0U;
    else if (spr >= SPR_IBAT4U && spr <= SPR_DBAT7L && s->cfg.bat_pairs == 8)
        k = spr - SPR_IBAT4U;
    else
        return NULL;
    unsigned base = spr >= SPR_IBAT4U ? 4 : 0;
    uint32_t (*bat)[2] = (k & 8) ? s->s.dbat : s->s.ibat;
    return &bat[base + ((k & 7) >> 1)][k & 1];
}

/* An SPR the architecture does not define: the board's, else plain storage
 * (supervisor) or illegal (problem state). */
static uam_status impl_read(uam_cpu *cpu, unsigned spr, uint64_t *val)
{
    ppc_state *s = ppc_st(cpu);
    if (s->spr_ops && s->spr_ops->read) {
        uint32_t v = 0;
        uam_status st = s->spr_ops->read(s->spr_ctx, cpu, spr, &v);
        if (st != UAM_ERR_INVALID) {
            *val = v;
            return st == UAM_OK ? UAM_OK : UAM_ERR_ILLEGAL;
        }
    }
    if (s->s.msr & MSR_PR)
        return UAM_ERR_ILLEGAL;
    *val = s->impl_spr[spr & 1023];
    return UAM_OK;
}

static uam_status impl_write(uam_cpu *cpu, unsigned spr, uint32_t v)
{
    ppc_state *s = ppc_st(cpu);
    if (s->spr_ops && s->spr_ops->write) {
        uam_status st = s->spr_ops->write(s->spr_ctx, cpu, spr, v);
        if (st != UAM_ERR_INVALID)
            return st == UAM_OK ? UAM_OK : UAM_ERR_ILLEGAL;
    }
    if (s->s.msr & MSR_PR)
        return UAM_ERR_ILLEGAL;
    s->impl_spr[spr & 1023] = v;
    return UAM_OK;
}

static uam_status ppc_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    ppc_state *s = ppc_st(cpu);
    if (id == PPC_SR_MSR) {
        *val = s->s.msr;
        return UAM_OK;
    }
    if (id >= PPC_SR_SR0 && id < PPC_SR_SR0 + 16) {
        *val = s->s.sr[id - PPC_SR_SR0];
        return UAM_OK;
    }
    uint32_t *bat = bat_reg(s, id);
    if (bat) {
        *val = *bat;
        return UAM_OK;
    }
    switch (id) {
    case SPR_DSISR: *val = s->s.dsisr; break;
    case SPR_DAR:   *val = s->s.dar; break;
    case SPR_DEC:   *val = dec_now(cpu); break;
    case SPR_SDR1:  *val = s->s.sdr1; break;
    case SPR_SRR0:  *val = s->s.srr0; break;
    case SPR_SRR1:  *val = s->s.srr1; break;
    case SPR_SPRG0: case SPR_SPRG0 + 1: case SPR_SPRG0 + 2: case SPR_SPRG3:
        *val = s->s.sprg[id - SPR_SPRG0];
        break;
    case SPR_EAR:   *val = s->s.ear; break;
    case SPR_TBL_R: case SPR_TBL_W: *val = (uint32_t)tb_now(cpu); break;
    case SPR_TBU_R: case SPR_TBU_W: *val = (uint32_t)(tb_now(cpu) >> 32); break;
    case SPR_PVR:   *val = s->cfg.pvr; break;
    case SPR_PIR:   *val = cpu->hartid; break;
    default:        return impl_read(cpu, id, val);
    }
    return UAM_OK;
}

static uam_status ppc_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t v64)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t v = (uint32_t)v64;
    if (id == PPC_SR_MSR) {
        ppc_msr_write(cpu, v);
        return UAM_OK;
    }
    if (id >= PPC_SR_SR0 && id < PPC_SR_SR0 + 16) {
        s->s.sr[id - PPC_SR_SR0] = v;
        ppc_tlb_flush(s);
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    }
    uint32_t *bat = bat_reg(s, id);
    if (bat) {
        *bat = v;
        ppc_tlb_flush(s);
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    }
    switch (id) {
    case SPR_DSISR: s->s.dsisr = v; break;
    case SPR_DAR:   s->s.dar = v; break;
    case SPR_DEC:   dec_write(cpu, v); break;
    case SPR_SDR1:
        s->s.sdr1 = v;
        ppc_tlb_flush(s);
        uam_cpu_xlate_changed(cpu);
        break;
    case SPR_SRR0:  s->s.srr0 = v; break;
    case SPR_SRR1:  s->s.srr1 = v; break;
    case SPR_SPRG0: case SPR_SPRG0 + 1: case SPR_SPRG0 + 2: case SPR_SPRG3:
        s->s.sprg[id - SPR_SPRG0] = v;
        break;
    case SPR_EAR:   s->s.ear = v & 0x8000003Fu; break;
    case SPR_TBL_W: {
        uint64_t tb = (tb_now(cpu) & ~UINT64_C(0xFFFFFFFF)) | v;
        s->s.tb_bias = tb - ticks_now(cpu);
        break;
    }
    case SPR_TBU_W: {
        uint64_t tb = (tb_now(cpu) & UINT64_C(0xFFFFFFFF)) | ((uint64_t)v << 32);
        s->s.tb_bias = tb - ticks_now(cpu);
        break;
    }
    case SPR_TBL_R: case SPR_TBU_R:
        return UAM_ERR_ILLEGAL;      /* the user view of the time base is read only */
    case SPR_PVR: case SPR_PIR:
        break;                       /* read only */
    default:
        return impl_write(cpu, id, v);
    }
    return UAM_OK;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static uint64_t helper_rfi(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    ppc_msr_write(cpu, (s->s.msr & ~MSR_SRR1_MASK) | (s->s.srr1 & MSR_SRR1_MASK));
    return s->s.srr0 & ~3u;
}

static uint64_t helper_mfsrin(void *env, uint64_t rb, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    return ppc_st(env)->s.sr[(uint32_t)rb >> 28];
}

static uint64_t helper_mtsrin(void *env, uint64_t rs, uint64_t rb, uint64_t c, uam_status *st)
{
    (void)c; (void)st;
    uam_cpu *cpu = env;
    ppc_st(cpu)->s.sr[(uint32_t)rb >> 28] = (uint32_t)rs;
    ppc_tlb_flush(ppc_st(cpu));
    uam_cpu_xlate_changed(cpu);
    return 0;
}

/* tlbie / tlbia: on every hart of this bus. */
static void tlb_invalidate(uam_cpu *cpu, bool all, uint32_t ea)
{
    for (ppc_state *h = all_harts; h; h = h->next) {
        if (h->cpu->bus != cpu->bus)
            continue;
        if (all)
            ppc_tlb_flush(h);
        else
            ppc_tlb_flush_page(h, ea);
        uam_cpu_xlate_changed(h->cpu);
    }
}

static uint64_t helper_tlbie(void *env, uint64_t rb, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    tlb_invalidate(env, false, (uint32_t)rb);
    return 0;
}

static uint64_t helper_tlbia(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    tlb_invalidate(env, true, 0);
    return 0;
}

/* lswx: XER[BC] bytes from EA into rD, rD+1, ... (wrapping), zero-filling the last
 * register. Loads everything before writing a register, so a fault is precise. */
static uint64_t helper_lswx(void *env, uint64_t ea, uint64_t regs, uint64_t c, uam_status *st)
{
    (void)c;
    uam_cpu *cpu = env;
    unsigned n = uam_cpu_reg_read(cpu, PPC_BC_OFF, 8) & 0x7F, rd = ((uint32_t)regs >> 10) & 31;
    uint32_t w[32] = { 0 };
    for (unsigned k = 0; k < n; k++) {
        uint64_t byte = 0;
        uam_status r = ppc_mem_rw(cpu, (uint32_t)ea + k, 1, false, &byte);
        if (r != UAM_OK) {
            *st = r;
            return 0;
        }
        w[k / 4] |= (uint32_t)byte << (24 - 8 * (k & 3));
    }
    for (unsigned k = 0; k < (n + 3) / 4; k++)
        uam_cpu_reg_write(cpu, PPC_GPR_OFF((rd + k) & 31), 32, w[k]);
    return 0;
}

static uint64_t helper_stswx(void *env, uint64_t ea, uint64_t regs, uint64_t c, uam_status *st)
{
    (void)c;
    uam_cpu *cpu = env;
    unsigned n = uam_cpu_reg_read(cpu, PPC_BC_OFF, 8) & 0x7F, rs = ((uint32_t)regs >> 10) & 31;
    for (unsigned k = 0; k < n; k++) {
        uint64_t byte = (uam_cpu_reg_read(cpu, PPC_GPR_OFF((rs + k / 4) & 31), 32)
                         >> (24 - 8 * (k & 3))) & 0xFF;
        uam_status r = ppc_mem_rw(cpu, (uint32_t)ea + k, 1, true, &byte);
        if (r != UAM_OK) {
            *st = r;
            return 0;
        }
    }
    return 0;
}

static uint64_t helper_trace(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)a; (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    ppc_st(cpu)->s.trace_pending = true;
    uam_cpu_request_exit(cpu);
    return 0;
}

#define RW (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE)

const uam_helper_desc ppc_helpers[PPC_HELPER__COUNT] = {
    [PPC_HELPER_RFI]    = { .name = "ppc_rfi", .n_args = 0, .result_width = 32, .effect = RW,
                            .fn = helper_rfi },
    [PPC_HELPER_FP]     = { .name = "ppc_fp", .n_args = 1, .arg_width = { 32 }, .result_width = 0,
                            .effect = RW | UAM_HELPER_MAY_FAULT, .fn = ppc_helper_fp },
    [PPC_HELPER_SP2DP]  = { .name = "ppc_sp2dp", .n_args = 1, .arg_width = { 32 }, .result_width = 64,
                            .effect = UAM_HELPER_PURE, .fn = ppc_helper_sp2dp },
    [PPC_HELPER_DP2SP]  = { .name = "ppc_dp2sp", .n_args = 1, .arg_width = { 64 }, .result_width = 32,
                            .effect = UAM_HELPER_PURE, .fn = ppc_helper_dp2sp },
    [PPC_HELPER_MFSRIN] = { .name = "ppc_mfsrin", .n_args = 1, .arg_width = { 32 }, .result_width = 32,
                            .effect = UAM_HELPER_READS_STATE, .fn = helper_mfsrin },
    [PPC_HELPER_MTSRIN] = { .name = "ppc_mtsrin", .n_args = 2, .arg_width = { 32, 32 }, .result_width = 0,
                            .effect = RW, .fn = helper_mtsrin },
    [PPC_HELPER_TLBIE]  = { .name = "ppc_tlbie", .n_args = 1, .arg_width = { 32 }, .result_width = 0,
                            .effect = RW, .fn = helper_tlbie },
    [PPC_HELPER_TLBIA]  = { .name = "ppc_tlbia", .n_args = 0, .result_width = 0, .effect = RW,
                            .fn = helper_tlbia },
    [PPC_HELPER_LSWX]   = { .name = "ppc_lswx", .n_args = 2, .arg_width = { 32, 32 }, .result_width = 0,
                            .effect = RW | UAM_HELPER_MAY_FAULT, .fn = helper_lswx },
    [PPC_HELPER_STSWX]  = { .name = "ppc_stswx", .n_args = 2, .arg_width = { 32, 32 }, .result_width = 0,
                            .effect = RW | UAM_HELPER_MAY_FAULT, .fn = helper_stswx },
    [PPC_HELPER_TRACE]  = { .name = "ppc_trace", .n_args = 0, .result_width = 0, .effect = RW,
                            .fn = helper_trace },
};

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static uam_status ppc_init(uam_cpu *cpu)
{
    ppc_state *s = calloc(1, sizeof(ppc_state));
    if (!s)
        return UAM_ERR_NOMEM;
    cpu->isa_state = s;
    s->cfg = uam_ppc_ref;
    s->cpu = cpu;
    uam_timer_init(&s->dec_timer, dec_fire, cpu);
    ppc_tlb_flush(s);
    s->next = all_harts;
    all_harts = s;
    return UAM_OK;
}

static void ppc_fini(uam_cpu *cpu)
{
    ppc_state *s = ppc_st(cpu);
    if (cpu->sched)
        uam_timer_cancel(cpu->sched, &s->dec_timer);
    for (ppc_state **pp = &all_harts; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    free(s);
    cpu->isa_state = NULL;
}

static void ppc_reset(uam_cpu *cpu, uint64_t entry)
{
    ppc_state *s = ppc_st(cpu);
    if (cpu->sched)
        uam_timer_cancel(cpu->sched, &s->dec_timer);
    memset(&s->s, 0, sizeof(s->s));
    memset(s->impl_spr, 0, sizeof(s->impl_spr));
    s->hfault = PPC_HF_NONE;
    ppc_tlb_flush(s);
    for (size_t i = 0; i < cpu->regfile_size; i++)
        cpu->regfile[i] = 0;
    /* The time base starts at zero; the decrementer at -1, so that it raises
     * nothing until software writes it. */
    if (cpu->sched)
        s->s.tb_bias = (uint64_t)0 - ticks_now(cpu);
    s->s.msr = s->cfg.reset_ip ? MSR_IP : 0;
    s->s.dec_next = UINT64_MAX;     /* nothing to catch up on */
    dec_write(cpu, 0xFFFFFFFFu);
    cpu->priv = priv_of(s->s.msr);
    uam_cpu_xlate_changed(cpu);
    cpu->pc = entry;
    cpu->waiting = false;
    cpu->resv.valid = false;
}

static uint32_t cr_value(uam_cpu *cpu)
{
    uint32_t cr = 0;
    for (unsigned f = 0; f < 8; f++)
        cr |= (uint32_t)(uam_cpu_reg_read(cpu, PPC_CR_OFF(f), 8) & 0xF) << (28 - 4 * f);
    return cr;
}

static uint32_t xer_value(uam_cpu *cpu)
{
    return (uint32_t)(uam_cpu_reg_read(cpu, PPC_SO_OFF, 8) & 1) << 31
         | (uint32_t)(uam_cpu_reg_read(cpu, PPC_OV_OFF, 8) & 1) << 30
         | (uint32_t)(uam_cpu_reg_read(cpu, PPC_CA_OFF, 8) & 1) << 29
         | (uint32_t)(uam_cpu_reg_read(cpu, PPC_BC_OFF, 8) & 0x7F);
}

static void ppc_dump_state(uam_cpu *cpu, FILE *out)
{
    ppc_state *s = ppc_st(cpu);
    fprintf(out, "pc=%08llx icount=%llu msr=%08x %s\n", (unsigned long long)cpu->pc,
            (unsigned long long)cpu->icount, s->s.msr,
            (s->s.msr & MSR_PR) ? "problem" : "supervisor");
    for (unsigned i = 0; i < 32; i++)
        fprintf(out, "r%-2u=%08llx%s", i,
                (unsigned long long)uam_cpu_reg_read(cpu, PPC_GPR_OFF(i), 32),
                (i % 4 == 3) ? "\n" : "  ");
    fprintf(out, "cr=%08x lr=%08llx ctr=%08llx xer=%08x fpscr=%08llx\n", cr_value(cpu),
            (unsigned long long)uam_cpu_reg_read(cpu, PPC_LR_OFF, 32),
            (unsigned long long)uam_cpu_reg_read(cpu, PPC_CTR_OFF, 32), xer_value(cpu),
            (unsigned long long)uam_cpu_reg_read(cpu, PPC_FPSCR_OFF, 32));
    fprintf(out, "srr0=%08x srr1=%08x dar=%08x dsisr=%08x dec=%08x tb=%016llx sdr1=%08x\n",
            s->s.srr0, s->s.srr1, s->s.dar, s->s.dsisr, dec_now(cpu),
            (unsigned long long)tb_now(cpu), s->s.sdr1);
}

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                 */
/* ------------------------------------------------------------------------ */

static void ppc_save(uam_cpu *cpu, uam_state_writer *w)
{
    const ppc_state *s = ppc_st(cpu);
    const ppc_sys *y = &s->s;
    uam_state_put_u32(w, s->cfg.pvr);
    uam_state_put_u32(w, s->cfg.bat_pairs);
    uam_state_put_u8(w, s->intlines);
    uam_state_put_bool(w, s->sreset_level);
    uam_state_put_bool(w, s->mcp_level);
    const uint32_t regs[] = { y->msr, y->srr0, y->srr1, y->dar, y->dsisr, y->ear, y->sdr1,
                              y->sprg[0], y->sprg[1], y->sprg[2], y->sprg[3], y->dec };
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        uam_state_put_u32(w, regs[i]);
    for (unsigned n = 0; n < 8; n++)
        for (unsigned k = 0; k < 2; k++) {
            uam_state_put_u32(w, y->ibat[n][k]);
            uam_state_put_u32(w, y->dbat[n][k]);
        }
    for (unsigned n = 0; n < 16; n++)
        uam_state_put_u32(w, y->sr[n]);
    uam_state_put_u64(w, y->tb_bias);
    uam_state_put_u64(w, y->dec_ticks);
    uam_state_put_u64(w, y->dec_next);
    uam_state_put_bool(w, y->dec_pending);
    uam_state_put_bool(w, y->sreset_pending);
    uam_state_put_bool(w, y->mcp_pending);
    uam_state_put_bool(w, y->trace_pending);
    uam_state_put_bool(w, y->checkstop);
    for (unsigned n = 0; n < 1024; n++)
        uam_state_put_u32(w, s->impl_spr[n]);
    uam_state_put_timer(w, &s->dec_timer);
}

static void ppc_restore(uam_cpu *cpu, uam_state_reader *r)
{
    ppc_state *s = ppc_st(cpu);
    ppc_sys *y = &s->s;
    uam_state_expect_u32(r, s->cfg.pvr, "PowerPC PVR");
    uam_state_expect_u32(r, s->cfg.bat_pairs, "PowerPC BAT pairs");
    s->intlines = uam_state_get_u8(r);
    s->sreset_level = uam_state_get_bool(r);
    s->mcp_level = uam_state_get_bool(r);
    uint32_t *const regs[] = { &y->msr, &y->srr0, &y->srr1, &y->dar, &y->dsisr, &y->ear, &y->sdr1,
                               &y->sprg[0], &y->sprg[1], &y->sprg[2], &y->sprg[3], &y->dec };
    for (size_t i = 0; i < UAM_ARRAY_LEN(regs); i++)
        *regs[i] = uam_state_get_u32(r);
    for (unsigned n = 0; n < 8; n++)
        for (unsigned k = 0; k < 2; k++) {
            y->ibat[n][k] = uam_state_get_u32(r);
            y->dbat[n][k] = uam_state_get_u32(r);
        }
    for (unsigned n = 0; n < 16; n++)
        y->sr[n] = uam_state_get_u32(r);
    y->tb_bias = uam_state_get_u64(r);
    y->dec_ticks = uam_state_get_u64(r);
    y->dec_next = uam_state_get_u64(r);
    y->dec_pending = uam_state_get_bool(r);
    y->sreset_pending = uam_state_get_bool(r);
    y->mcp_pending = uam_state_get_bool(r);
    y->trace_pending = uam_state_get_bool(r);
    y->checkstop = uam_state_get_bool(r);
    for (unsigned n = 0; n < 1024; n++)
        s->impl_spr[n] = uam_state_get_u32(r);
    uam_state_get_timer(r, &s->dec_timer);
    s->hfault = PPC_HF_NONE;
    ppc_tlb_flush(s);
    cpu->priv = priv_of(y->msr);
}

/* ------------------------------------------------------------------------ */
/* Debugger view (gdb: powerpc:common, registers numbered as gdb's rs6000 port) */
/* ------------------------------------------------------------------------ */

/* r0-r31 (0-31), f0-f31 (32-63), pc msr cr lr ctr xer (64-69), fpscr (70). */
#define GDB_PPC_NREGS 71u

#define GDB_R(n) "<reg name=\"r" #n "\" bitsize=\"32\" type=\"uint32\" regnum=\"" #n "\"/>"
#define GDB_F(n, num) "<reg name=\"f" #n "\" bitsize=\"64\" type=\"ieee_double\" regnum=\"" #num "\"/>"
static const char ppc_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>powerpc:common</architecture>"
    "<feature name=\"org.gnu.gdb.power.core\">"
    GDB_R(0) GDB_R(1) GDB_R(2) GDB_R(3) GDB_R(4) GDB_R(5) GDB_R(6) GDB_R(7)
    GDB_R(8) GDB_R(9) GDB_R(10) GDB_R(11) GDB_R(12) GDB_R(13) GDB_R(14) GDB_R(15)
    GDB_R(16) GDB_R(17) GDB_R(18) GDB_R(19) GDB_R(20) GDB_R(21) GDB_R(22) GDB_R(23)
    GDB_R(24) GDB_R(25) GDB_R(26) GDB_R(27) GDB_R(28) GDB_R(29) GDB_R(30) GDB_R(31)
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"64\"/>"
    "<reg name=\"msr\" bitsize=\"32\" type=\"uint32\" regnum=\"65\"/>"
    "<reg name=\"cr\" bitsize=\"32\" type=\"uint32\" regnum=\"66\"/>"
    "<reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\" regnum=\"67\"/>"
    "<reg name=\"ctr\" bitsize=\"32\" type=\"uint32\" regnum=\"68\"/>"
    "<reg name=\"xer\" bitsize=\"32\" type=\"uint32\" regnum=\"69\"/>"
    "</feature><feature name=\"org.gnu.gdb.power.fpu\">"
    GDB_F(0, 32) GDB_F(1, 33) GDB_F(2, 34) GDB_F(3, 35) GDB_F(4, 36) GDB_F(5, 37)
    GDB_F(6, 38) GDB_F(7, 39) GDB_F(8, 40) GDB_F(9, 41) GDB_F(10, 42) GDB_F(11, 43)
    GDB_F(12, 44) GDB_F(13, 45) GDB_F(14, 46) GDB_F(15, 47) GDB_F(16, 48) GDB_F(17, 49)
    GDB_F(18, 50) GDB_F(19, 51) GDB_F(20, 52) GDB_F(21, 53) GDB_F(22, 54) GDB_F(23, 55)
    GDB_F(24, 56) GDB_F(25, 57) GDB_F(26, 58) GDB_F(27, 59) GDB_F(28, 60) GDB_F(29, 61)
    GDB_F(30, 62) GDB_F(31, 63)
    "<reg name=\"fpscr\" bitsize=\"32\" group=\"float\" regnum=\"70\"/>"
    "</feature></target>";
#undef GDB_R
#undef GDB_F

static const uint8_t ppc_gdb_sizes[GDB_PPC_NREGS] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    4, 4, 4, 4, 4, 4, 4,
};

static bool ppc_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n < 32)       *v = uam_cpu_reg_read(cpu, PPC_GPR_OFF(n), 32);
    else if (n < 64)  *v = uam_cpu_reg_read(cpu, PPC_FPR_OFF(n - 32), 64);
    else if (n == 64) *v = (uint32_t)cpu->pc;
    else if (n == 65) *v = ppc_st(cpu)->s.msr;
    else if (n == 66) *v = cr_value(cpu);
    else if (n == 67) *v = uam_cpu_reg_read(cpu, PPC_LR_OFF, 32);
    else if (n == 68) *v = uam_cpu_reg_read(cpu, PPC_CTR_OFF, 32);
    else if (n == 69) *v = xer_value(cpu);
    else if (n == 70) *v = uam_cpu_reg_read(cpu, PPC_FPSCR_OFF, 32);
    else return false;
    return true;
}

static bool ppc_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n < 32)       uam_cpu_reg_write(cpu, PPC_GPR_OFF(n), 32, v);
    else if (n < 64)  uam_cpu_reg_write(cpu, PPC_FPR_OFF(n - 32), 64, v);
    else if (n == 64) cpu->pc = (uint32_t)v & ~3u;
    else if (n == 65) ppc_msr_write(cpu, (uint32_t)v);
    else if (n == 66) {
        for (unsigned f = 0; f < 8; f++)
            uam_cpu_reg_write(cpu, PPC_CR_OFF(f), 8, (v >> (28 - 4 * f)) & 0xF);
    } else if (n == 67) uam_cpu_reg_write(cpu, PPC_LR_OFF, 32, v);
    else if (n == 68) uam_cpu_reg_write(cpu, PPC_CTR_OFF, 32, v);
    else if (n == 69) {
        uam_cpu_reg_write(cpu, PPC_SO_OFF, 8, (v >> 31) & 1);
        uam_cpu_reg_write(cpu, PPC_OV_OFF, 8, (v >> 30) & 1);
        uam_cpu_reg_write(cpu, PPC_CA_OFF, 8, (v >> 29) & 1);
        uam_cpu_reg_write(cpu, PPC_BC_OFF, 8, v & 0x7F);
    } else if (n == 70) uam_cpu_reg_write(cpu, PPC_FPSCR_OFF, 32, v);
    else return false;
    return true;
}

static const uam_gdb_arch ppc_gdb = {
    .target_xml = ppc_target_xml,
    .n_regs = GDB_PPC_NREGS,
    .reg_bytes = 4,
    .read_reg = ppc_gdb_read,
    .write_reg = ppc_gdb_write,
    .reg_sizes = ppc_gdb_sizes,
};

/* ------------------------------------------------------------------------ */
/* Descriptor                                                                */
/* ------------------------------------------------------------------------ */

const uam_isa uam_isa_ppc32 = {
    .name           = "ppc32",
    .addr_width     = 32,
    .default_endian = UAM_ACC_BE,
    .regfile_size   = PPC_REGFILE_SIZE,
    .regs           = ppc_regs,
    .n_regs         = UAM_ARRAY_LEN(ppc_regs),
    .helpers        = { ppc_helpers, PPC_HELPER__COUNT },
    .init           = ppc_init,
    .fini           = ppc_fini,
    .reset          = ppc_reset,
    .lift           = ppc_lift,
    .take_exception = ppc_take_exception,
    .irq_take       = ppc_irq_take,
    .irq_wakeup     = ppc_irq_wakeup,
    .irq_input      = ppc_irq_input,
    .sysreg_read    = ppc_sysreg_read,
    .sysreg_write   = ppc_sysreg_write,
    .translate      = ppc_translate,
    .gdb            = &ppc_gdb,
    .dump_state     = ppc_dump_state,
    .save           = ppc_save,
    .restore        = ppc_restore,
    .irq_lines      = ppc_irq_lines,
};
