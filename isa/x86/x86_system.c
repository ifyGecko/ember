/*
 * x86_system.c - x86-64 privileged architecture and the uam_isa descriptor:
 * reset, the operating-mode encoding in cpu->priv, exception and interrupt
 * delivery (real-mode IVT, protected- and long-mode IDT, #DF and triple
 * fault), NMI/SMI/INIT, the system-register handlers behind SYSRD/SYSWR, the
 * platform hooks and snapshots.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"
#include "x86.h"

/* ---- register descriptors ------------------------------------------------ */

#define G(n, i) { n, X86_OFF_GPR(i), 64, UAM_REG_GPR }
#define SB(n, s) { n, X86_OFF_SEG_BASE(s), 64, UAM_REG_OTHER }
#define SS(n, s) { n, X86_OFF_SEG_SEL(s), 16, UAM_REG_OTHER }
#define FL(n, f) { n, X86_OFF_FLAG(f), 8, UAM_REG_FLAG }
static const uam_regdesc x86_regs[] = {
    G("rax", 0), G("rcx", 1), G("rdx", 2),  G("rbx", 3),
    G("rsp", 4), G("rbp", 5), G("rsi", 6),  G("rdi", 7),
    G("r8", 8),  G("r9", 9),  G("r10", 10), G("r11", 11),
    G("r12", 12),G("r13", 13),G("r14", 14), G("r15", 15),
    SS("es", X86_SEG_ES), SS("cs", X86_SEG_CS), SS("ss", X86_SEG_SS),
    SS("ds", X86_SEG_DS), SS("fs", X86_SEG_FS), SS("gs", X86_SEG_GS),
    SB("es.base", X86_SEG_ES), SB("cs.base", X86_SEG_CS), SB("ss.base", X86_SEG_SS),
    SB("ds.base", X86_SEG_DS), SB("fs.base", X86_SEG_FS), SB("gs.base", X86_SEG_GS),
    FL("cf", X86_F_CF), FL("pf", X86_F_PF), FL("af", X86_F_AF), FL("zf", X86_F_ZF),
    FL("sf", X86_F_SF), FL("of", X86_F_OF), FL("df", X86_F_DF), FL("if", X86_F_IF),
};
#undef G
#undef SB
#undef SS
#undef FL

/* ---- RFLAGS and segment caches ------------------------------------------- */

static const struct { unsigned f, bit; } flag_bits[] = {
    { X86_F_CF, RFLAGS_CF }, { X86_F_AF, RFLAGS_AF }, { X86_F_ZF, RFLAGS_ZF },
    { X86_F_SF, RFLAGS_SF }, { X86_F_OF, RFLAGS_OF }, { X86_F_DF, RFLAGS_DF },
    { X86_F_IF, RFLAGS_IF },
};

uint32_t x86_get_rflags(uam_cpu *cpu)
{
    uint32_t v = RFLAGS_ALWAYS_1;
    for (size_t i = 0; i < UAM_ARRAY_LEN(flag_bits); i++)
        if (uam_cpu_reg_read(cpu, X86_OFF_FLAG(flag_bits[i].f), 8))
            v |= flag_bits[i].bit;
    /* PF holds the last result's low byte: set when it has even parity */
    if (!(__builtin_popcount((unsigned)uam_cpu_reg_read(cpu, X86_OFF_FLAG(X86_F_PF), 8)) & 1))
        v |= RFLAGS_PF;
    v |= (uint32_t)uam_cpu_reg_read(cpu, X86_OFF_RFLAGS_HI, 32) & RFLAGS_HI_MASK;
    return v;
}

void x86_set_rflags(uam_cpu *cpu, uint32_t v)
{
    for (size_t i = 0; i < UAM_ARRAY_LEN(flag_bits); i++)
        uam_cpu_reg_write(cpu, X86_OFF_FLAG(flag_bits[i].f), 8, (v & flag_bits[i].bit) ? 1 : 0);
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_PF), 8, (v & RFLAGS_PF) ? 0 : 1);
    uam_cpu_reg_write(cpu, X86_OFF_RFLAGS_HI, 32, v & RFLAGS_HI_MASK);
}

uint64_t x86_seg_base(uam_cpu *cpu, unsigned seg) { return uam_cpu_reg_read(cpu, X86_OFF_SEG_BASE(seg), 64); }
uint16_t x86_seg_sel(uam_cpu *cpu, unsigned seg)  { return (uint16_t)uam_cpu_reg_read(cpu, X86_OFF_SEG_SEL(seg), 16); }
uint16_t x86_seg_attr(uam_cpu *cpu, unsigned seg) { return (uint16_t)uam_cpu_reg_read(cpu, X86_OFF_SEG_ATTR(seg), 16); }

void x86_seg_set(uam_cpu *cpu, unsigned seg, uint16_t sel, uint64_t base, uint32_t limit, uint16_t attr)
{
    uam_cpu_reg_write(cpu, X86_OFF_SEG_SEL(seg), 16, sel);
    uam_cpu_reg_write(cpu, X86_OFF_SEG_BASE(seg), 64, base);
    uam_cpu_reg_write(cpu, X86_OFF_SEG_LIMIT(seg), 32, limit);
    uam_cpu_reg_write(cpu, X86_OFF_SEG_ATTR(seg), 16, attr);
}

/* Real-mode load: selector and base change, the hidden limit and attributes stay. */
void x86_seg_set_real(uam_cpu *cpu, unsigned seg, uint16_t sel)
{
    uam_cpu_reg_write(cpu, X86_OFF_SEG_SEL(seg), 16, sel);
    uam_cpu_reg_write(cpu, X86_OFF_SEG_BASE(seg), 64, (uint64_t)sel << 4);
}

/* ---- operating mode ------------------------------------------------------ */

void x86_update_priv(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    unsigned mode = X86_MODE_REAL, cpl = 0, code = X86_CODE16;
    uint16_t cs = x86_seg_attr(cpu, X86_SEG_CS);
    if (s->efer & EFER_LMA)
        mode = X86_MODE_LONG;
    else if (s->cr0 & CR0_PE)
        mode = (uam_cpu_reg_read(cpu, X86_OFF_RFLAGS_HI, 32) & RFLAGS_VM) ? X86_MODE_V86
                                                                          : X86_MODE_PROT;
    if (mode == X86_MODE_V86) {
        cpl = 3;                                  /* 16-bit code and stack, CPL 3 */
    } else if (mode != X86_MODE_REAL) {
        cpl = x86_seg_sel(cpu, X86_SEG_CS) & 3u;
        if (mode == X86_MODE_LONG && (cs & SEGA_L))
            code = X86_CODE64;
        else if (cs & SEGA_DB)
            code = X86_CODE32;
    }
    unsigned p = cpl | mode << X86_PRIV_MODE_SHIFT | code << X86_PRIV_CODE_SHIFT;
    if (s->in_smm)
        p |= X86_PRIV_SMM;
    if ((x86_seg_attr(cpu, X86_SEG_SS) & SEGA_DB) && mode != X86_MODE_V86)
        p |= X86_PRIV_STACK32;
    if (s->cr0 & CR0_PG)
        p |= X86_PRIV_PG;
    if ((s->irq_lines & (1u << UAM_X86_IRQ_A20M)) && !s->in_smm)
        p |= X86_PRIV_A20M;
    if (p != cpu->priv) {
        cpu->priv = p;
        uam_cpu_request_exit(cpu);
    }
}

uam_status x86_raise(uam_cpu *cpu, unsigned vec, bool has_err, uint32_t err)
{
    x86_state *s = x86_st(cpu);
    s->pend_valid = true;
    s->pend_vec = (uint8_t)vec;
    s->pend_has_err = has_err;
    s->pend_err = err;
    return UAM_ERR_FAULT;
}

/* ---- lifecycle ----------------------------------------------------------- */

static uam_status x86_init(uam_cpu *cpu)
{
    x86_state *s = calloc(1, sizeof(x86_state));
    if (!s)
        return UAM_ERR_NOMEM;
    cpu->isa_state = s;
    x86_cpuid_defaults(&s->cpuid);
    return UAM_OK;
}

static void x86_fini(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    if (s)
        free(s->msrs);
    free(s);
    cpu->isa_state = NULL;
}

/* Power-on / INIT state. INIT keeps the MSRs, the platform hooks and the
 * CPUID configuration (and, on hardware, caches and MTRRs). */
static void power_on_state(uam_cpu *cpu, uint64_t entry)
{
    x86_state *s = x86_st(cpu);
    x86_state keep = *s;
    memset(s, 0, sizeof(*s));
    s->msrs = keep.msrs; s->n_msrs = keep.n_msrs; s->cap_msrs = keep.cap_msrs;
    s->msr_policy = keep.msr_policy; s->msr_trace = keep.msr_trace;
    s->plat = keep.plat; s->cpuid = keep.cpuid;
    s->mtrr_def_type = keep.mtrr_def_type; s->pat = 0x0007040600070406ull;
    s->rng = 0x853c49e6748fea9bull;
    memset(cpu->regfile, 0, cpu->regfile_size);

    for (unsigned seg = 0; seg < X86_NSEG; seg++)
        x86_seg_set(cpu, seg, 0, 0, 0xFFFF, SEGA_P | SEGA_S | 0x3);   /* data RW, accessed */
    /* CS: selector F000 with the hidden base FFFF0000, so the first fetch is
     * at linear FFFFFFF0. A lower entry is a flat real-mode start for a raw
     * image in low memory. */
    if (entry >= 0xFFFF0000ull)
        x86_seg_set(cpu, X86_SEG_CS, 0xF000, 0xFFFF0000ull, 0xFFFF, SEGA_P | SEGA_S | 0xB);
    else
        x86_seg_set(cpu, X86_SEG_CS, 0, 0, 0xFFFF, SEGA_P | SEGA_S | 0xB);
    s->gdtr.limit = s->idtr.limit = 0xFFFF;
    s->ldtr.limit = s->tr.limit = 0xFFFF;
    s->ldtr.attr = SEGA_P | 0x2;
    s->tr.attr = SEGA_P | 0xB;
    s->cr0 = CR0_ET | CR0_NW | CR0_CD;
    s->dr[6] = 0xFFFF0FF0u;
    s->dr[7] = 0x400;
    s->smbase = 0x30000;
    s->apic_base = 0xFEE00000ull | (1u << 11) | (cpu->hartid == 0 ? (1u << 8) : 0);
    s->misc_enable = 1;
    s->xcr0 = 1;
    x86_set_rflags(cpu, RFLAGS_ALWAYS_1);
    x86_set_gpr(cpu, X86_RDX, s->cpuid.signature);
    uam_cpu_reg_write(cpu, X86_OFF_MXCSR, 32, 0x1F80);
    uam_cpu_reg_write(cpu, X86_OFF_FCW, 16, 0x37F);
    for (unsigned i = 0; i < X86_TLB_SIZE; i++)
        s->tlb[i].vpage = UINT64_MAX;

    cpu->priv = ~0u;
    x86_update_priv(cpu);
    cpu->pc = entry;
    uam_cpu_xlate_changed(cpu);
}

/* An application processor (hartid > 0) leaves reset, and INIT, waiting for
 * a startup IPI; the bootstrap processor starts at `entry`. */
static void enter_wait_sipi(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    s->wait_sipi = true;
    cpu->waiting = true;
}

static void x86_reset(uam_cpu *cpu, uint64_t entry)
{
    power_on_state(cpu, entry);
    if (cpu->hartid != 0)
        enter_wait_sipi(cpu);   /* power_on_state cleared any pending SIPI */
}

void uam_x86_sipi(void *ctx, uint8_t vector)
{
    uam_cpu *cpu = ctx;
    x86_state *s = x86_st(cpu);
    if (!s->wait_sipi && !s->init_pending)
        return;                  /* only a processor waiting for one (or about to) takes it */
    s->sipi_pending = true;
    s->sipi_vector = vector;
    uam_cpu_request_exit(cpu);
}

/* Multiprocessor switch: the platform's SMRAM view follows the processor. */
static void x86_switch_in(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    if (s->plat.smm_changed)
        s->plat.smm_changed(s->plat.ctx, s->in_smm);
}

/* ---- exception and interrupt delivery ------------------------------------ */

static bool vec_has_err(unsigned vec)
{
    return vec == X86_EXC_DF || (vec >= X86_EXC_TS && vec <= X86_EXC_PF) || vec == X86_EXC_AC;
}

static bool contributory(unsigned vec)
{
    return vec == X86_EXC_DE || (vec >= X86_EXC_TS && vec <= X86_EXC_GP);
}

/* Push onto the new stack (supervisor accesses). */
typedef struct frame { uint64_t sp; uint64_t ss_base; unsigned sw; } frame;

static uam_status fpush(uam_cpu *cpu, frame *f, unsigned bytes, uint64_t v)
{
    uint64_t m = (f->sw == 64) ? ~0ull : (f->sw == 32) ? 0xFFFFFFFFull : 0xFFFFull;
    uint64_t nsp = (f->sp - bytes) & m;
    uint64_t lin = f->ss_base + nsp;
    if (f->sw != 64)
        lin &= 0xFFFFFFFFull;
    uam_status st = x86_lin_write(cpu, lin, bytes, v, true);
    if (st == UAM_OK)
        f->sp = (f->sp & ~m) | nsp;
    return st;
}

static void set_sp(uam_cpu *cpu, const frame *f)
{
    if (f->sw == 16)
        uam_cpu_reg_write(cpu, X86_OFF_GPR(X86_RSP), 16, f->sp);
    else
        x86_set_gpr(cpu, X86_RSP, f->sp & (f->sw == 32 ? 0xFFFFFFFFull : ~0ull));
}

static void clear_entry_flags(uam_cpu *cpu, bool clear_if)
{
    uint32_t f = x86_get_rflags(cpu);
    f &= ~(uint32_t)(RFLAGS_TF | RFLAGS_NT | RFLAGS_RF | RFLAGS_VM | RFLAGS_AC);
    if (clear_if)
        f &= ~(uint32_t)RFLAGS_IF;
    x86_set_rflags(cpu, f);
}

static uam_status deliver_real(uam_cpu *cpu, unsigned vec, uint64_t ret)
{
    x86_state *s = x86_st(cpu);
    uint64_t ent = 0;
    if ((uint64_t)vec * 4 + 3 > s->idtr.limit)
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    uam_status st = x86_lin_read(cpu, s->idtr.base + vec * 4u, 4, &ent, true);
    if (st != UAM_OK)
        return st;
    frame f = { uam_cpu_reg_read(cpu, X86_OFF_GPR(X86_RSP), 16), x86_seg_base(cpu, X86_SEG_SS), 16 };
    uint16_t ip = (uint16_t)(ret - x86_seg_base(cpu, X86_SEG_CS));
    if ((st = fpush(cpu, &f, 2, x86_get_rflags(cpu) & 0xFFFF)) != UAM_OK ||
        (st = fpush(cpu, &f, 2, x86_seg_sel(cpu, X86_SEG_CS))) != UAM_OK ||
        (st = fpush(cpu, &f, 2, ip)) != UAM_OK)
        return st;
    set_sp(cpu, &f);
    clear_entry_flags(cpu, true);
    x86_seg_set_real(cpu, X86_SEG_CS, (uint16_t)(ent >> 16));
    cpu->pc = x86_seg_base(cpu, X86_SEG_CS) + (ent & 0xFFFF);
    return UAM_OK;
}

static uam_status deliver_prot(uam_cpu *cpu, unsigned vec, bool has_err, uint32_t err,
                               bool intn, uint64_t ret)
{
    x86_state *s = x86_st(cpu);
    bool lm = x86_priv_mode(cpu->priv) == X86_MODE_LONG;
    bool v86 = x86_priv_mode(cpu->priv) == X86_MODE_V86;
    /* INT n in virtual-8086 mode is IOPL-sensitive */
    if (v86 && intn && ((x86_get_rflags(cpu) & RFLAGS_IOPL) >> 12) < 3)
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    unsigned gsz = lm ? 16 : 8;
    uint32_t gerr = vec * 8u + 2u;   /* IDT, external bit clear */
    uint64_t lo = 0, hi = 0;
    uam_status st;

    if ((uint64_t)vec * gsz + gsz - 1 > s->idtr.limit)
        return x86_raise(cpu, X86_EXC_GP, true, gerr);
    if ((st = x86_lin_read(cpu, s->idtr.base + vec * gsz, 8, &lo, true)) != UAM_OK)
        return st;
    if (lm && (st = x86_lin_read(cpu, s->idtr.base + vec * gsz + 8, 8, &hi, true)) != UAM_OK)
        return st;

    unsigned type = (lo >> 40) & 0xF, dpl = (lo >> 45) & 3;
    bool trap_gate = (type & 1) != 0;
    bool gate32 = lm || (type & 8);
    bool task_gate = !lm && type == 0x5;
    if (!task_gate && (lm ? (type != 0xE && type != 0xF)
                          : (type != 0xE && type != 0xF && type != 0x6 && type != 0x7)))
        return x86_raise(cpu, X86_EXC_GP, true, gerr);
    unsigned cpl = x86_priv_cpl(cpu->priv);
    if (intn && dpl < cpl)
        return x86_raise(cpu, X86_EXC_GP, true, gerr);
    if (!(lo & (1ull << 47)))
        return x86_raise(cpu, X86_EXC_NP, true, gerr);
    if (task_gate) {   /* the handler is a task */
        uint64_t npc = 0;
        uam_status ts = x86_task_switch(cpu, (uint16_t)(lo >> 16), X86_TASK_INT, ret, has_err, err, &npc);
        if (ts == UAM_OK)
            cpu->pc = npc;
        return ts;
    }

    uint16_t sel = (uint16_t)(lo >> 16);
    uint64_t off = (lo & 0xFFFF) | ((lo >> 32) & 0xFFFF0000ull);
    if (lm)
        off |= hi << 32;
    else if (!gate32)
        off &= 0xFFFF;

    uint64_t cdlo = 0, cdhi = 0;
    if ((sel & ~3u) == 0)
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    if ((st = x86_read_desc(cpu, sel, &cdlo, &cdhi, false)) != UAM_OK)
        return st;
    uint64_t cbase; uint32_t climit; uint16_t cattr;
    x86_desc_unpack(cdlo, &cbase, &climit, &cattr);
    if (!(cattr & SEGA_S) || !(cattr & 0x8))
        return x86_raise(cpu, X86_EXC_GP, true, sel & ~3u);
    if (!(cattr & SEGA_P))
        return x86_raise(cpu, X86_EXC_NP, true, sel & ~3u);
    unsigned ncpl = (cattr & 0x4) ? cpl : ((cattr & SEGA_DPL) >> 5);   /* conforming keeps CPL */
    if (ncpl > cpl)
        return x86_raise(cpu, X86_EXC_GP, true, sel & ~3u);
    /* out of virtual-8086 mode: only to a non-conforming CPL 0 handler */
    if (v86 && ncpl != 0)
        return x86_raise(cpu, X86_EXC_GP, true, sel & ~3u);

    uint64_t old_ss = x86_seg_sel(cpu, X86_SEG_SS);
    uint64_t old_sp = x86_gpr(cpu, X86_RSP);
    uint32_t old_flags = x86_get_rflags(cpu);
    uint16_t old_cs = x86_seg_sel(cpu, X86_SEG_CS);
    uint64_t rip = ret - x86_seg_base(cpu, X86_SEG_CS);
    unsigned ist = lm ? (unsigned)((lo >> 32) & 7) : 0;
    bool switch_stack = ncpl < cpl || ist;
    frame f;

    if (switch_stack) {
        uint64_t nsp = 0;
        uint16_t nss = 0;
        if ((st = x86_tss_stack(cpu, lm, ncpl, ist, &nsp, &nss)) != UAM_OK)
            return st;
        if (lm) {
            f = (frame){ nsp & ~0xFull, 0, 64 };
        } else {
            uint64_t sdlo = 0, sdhi = 0, sb; uint32_t sl; uint16_t sa;
            if ((st = x86_read_desc(cpu, nss, &sdlo, &sdhi, false)) != UAM_OK)
                return st;
            x86_desc_unpack(sdlo, &sb, &sl, &sa);
            f = (frame){ nsp, sb, (sa & SEGA_DB) ? 32u : 16u };
        }
        unsigned b = lm ? 8 : gate32 ? 4 : 2;
        if (v86) {   /* the virtual-8086 data segments, which the handler finds null */
            static const unsigned vseg[] = { X86_SEG_GS, X86_SEG_FS, X86_SEG_DS, X86_SEG_ES };
            for (unsigned i = 0; i < UAM_ARRAY_LEN(vseg); i++)
                if ((st = fpush(cpu, &f, b, x86_seg_sel(cpu, vseg[i]))) != UAM_OK)
                    return st;
        }
        if ((st = fpush(cpu, &f, b, old_ss)) != UAM_OK ||
            (st = fpush(cpu, &f, b, old_sp)) != UAM_OK)
            return st;
        if (!lm) {
            /* commit SS now: the frame is written relative to it */
            if ((st = x86_load_seg_nocheck(cpu, X86_SEG_SS, nss)) != UAM_OK)
                return st;
        } else if (ncpl < cpl) {
            x86_seg_set(cpu, X86_SEG_SS, (uint16_t)ncpl, 0, 0xFFFFFFFFu,
                        SEGA_P | SEGA_S | 0x3 | (uint16_t)(ncpl << 5) | SEGA_DB | SEGA_G);
        }
    } else if (lm) {
        f = (frame){ old_sp & ~0xFull, 0, 64 };
        if ((st = fpush(cpu, &f, 8, old_ss)) != UAM_OK ||
            (st = fpush(cpu, &f, 8, old_sp)) != UAM_OK)
            return st;
    } else {
        f = (frame){ old_sp, x86_seg_base(cpu, X86_SEG_SS), x86_priv_stack(cpu->priv) };
    }

    unsigned b = lm ? 8 : gate32 ? 4 : 2;
    if ((st = fpush(cpu, &f, b, old_flags)) != UAM_OK ||
        (st = fpush(cpu, &f, b, old_cs)) != UAM_OK ||
        (st = fpush(cpu, &f, b, rip)) != UAM_OK)
        return st;
    if (has_err && (st = fpush(cpu, &f, b, err)) != UAM_OK)
        return st;

    set_sp(cpu, &f);
    if (lm && (cattr & SEGA_L))
        cbase = 0;
    x86_seg_set(cpu, X86_SEG_CS, (uint16_t)((sel & ~3u) | ncpl), cbase, climit, cattr);
    if (v86)
        for (unsigned i = 0; i < X86_NSEG; i++)
            if (i != X86_SEG_CS && i != X86_SEG_SS)
                x86_seg_set(cpu, i, 0, 0, 0, 0);
    clear_entry_flags(cpu, !trap_gate);
    x86_update_priv(cpu);
    cpu->pc = (x86_priv_code(cpu->priv) == X86_CODE64) ? off : ((cbase + off) & 0xFFFFFFFFull);
    return UAM_OK;
}

static uam_status deliver_once(uam_cpu *cpu, unsigned vec, bool has_err, uint32_t err,
                               bool intn, uint64_t ret)
{
    x86_state *s = x86_st(cpu);
    s->pend_valid = false;
    s->pend_in_task = false;
    if (x86_priv_mode(cpu->priv) == X86_MODE_REAL)
        return deliver_real(cpu, vec, ret);
    return deliver_prot(cpu, vec, has_err, err, intn, ret);
}

static void shutdown(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    s->shutdown = true;
    cpu->waiting = true;
    uam_cpu_request_exit(cpu);
    fprintf(stderr, "x86: triple fault at pc=%#llx: hart shut down\n", (unsigned long long)cpu->pc);
}

/*
 * Deliver an exception or interrupt. A fault while delivering it is
 * combined with it by the double-fault rules: contributory after
 * contributory, or anything but a benign exception after #PF, becomes #DF;
 * a fault while delivering #DF shuts the hart down.
 */
void x86_deliver(uam_cpu *cpu, unsigned vec, bool has_err, uint32_t err,
                 bool soft, uint64_t return_pc, bool intn)
{
    x86_state *s = x86_st(cpu);
    uint64_t fault_pc = cpu->pc;
    (void)soft;
    for (;;) {
        if (deliver_once(cpu, vec, has_err, err, intn, return_pc) == UAM_OK)
            return;
        unsigned v2 = s->pend_valid ? s->pend_vec : X86_EXC_GP;
        bool e2 = s->pend_valid ? s->pend_has_err : true;
        uint32_t c2 = s->pend_valid ? s->pend_err : 0;
        s->pend_valid = false;
        if (s->pend_in_task) {   /* the task gate's switch committed: the new task faults */
            s->pend_in_task = false;
            fault_pc = s->pend_pc;
        }
        if (vec == X86_EXC_DF) {
            shutdown(cpu);
            return;
        }
        bool df = (contributory(vec) && contributory(v2)) ||
                  (vec == X86_EXC_PF && (contributory(v2) || v2 == X86_EXC_PF));
        if (df) {
            vec = X86_EXC_DF; has_err = true; err = 0;
        } else {
            vec = v2; has_err = e2; err = c2;
        }
        intn = false;
        cpu->pc = return_pc = fault_pc;
    }
}

static void x86_take_exception(uam_cpu *cpu, const uam_exception *exc)
{
    x86_state *s = x86_st(cpu);
    unsigned vec = X86_EXC_GP;
    bool has_err = true, intn = false;
    uint32_t err = 0;
    /* the faulting instruction; an interrupt-shadowed one restarts with its parent */
    cpu->pc = (exc->flags & UAM_EXC_F_CONT) ? exc->parent_pc : exc->pc;
    uint64_t ret = cpu->pc;

    switch (exc->kind) {
    case UAM_EXC_TRAP: {
        uint64_t c = exc->cause;
        vec = (unsigned)(c & 0xFF);
        has_err = (c & X86_TRAP_ERR) != 0;
        err = (uint32_t)(c >> 32);
        if (c & X86_TRAP_SOFT) {
            ret = exc->tval;
            intn = (c & X86_TRAP_INTN) != 0;
        }
        break;
    }
    case UAM_EXC_FETCH_XLATE: case UAM_EXC_LOAD_XLATE: case UAM_EXC_STORE_XLATE:
        vec = X86_EXC_PF;
        err = (uint32_t)exc->cause;
        s->cr2 = exc->tval;
        break;
    case UAM_EXC_HELPER: case UAM_EXC_SYSREG:
        if (s->pend_valid) {
            vec = s->pend_vec; has_err = s->pend_has_err; err = s->pend_err;
            if (s->pend_in_task)   /* after a task switch committed: in the new task */
                cpu->pc = ret = s->pend_pc;
            s->pend_in_task = false;
        } else {
            vec = (exc->kind == UAM_EXC_SYSREG) ? X86_EXC_GP : X86_EXC_UD;
            has_err = vec == X86_EXC_GP;
        }
        s->pend_valid = false;
        break;
    case UAM_EXC_LOAD_MISALIGNED: case UAM_EXC_STORE_MISALIGNED:
        vec = X86_EXC_AC;
        break;
    default:   /* bus faults: nothing decodes the address */
        vec = X86_EXC_GP;
        break;
    }
    if (!vec_has_err(vec))
        has_err = false;
    x86_deliver(cpu, vec, has_err, err, ret != cpu->pc, ret, intn);
}

/* ---- interrupts ---------------------------------------------------------- */

static void x86_irq_input(uam_cpu *cpu, unsigned line, int level)
{
    x86_state *s = x86_st(cpu);
    uint32_t bit = 1u << line;
    bool rising = level && !(s->irq_lines & bit);
    if (level) s->irq_lines |= bit; else s->irq_lines &= ~bit;
    if (line == UAM_X86_IRQ_A20M) {   /* every physical address may change: re-key */
        x86_update_priv(cpu);
        uam_cpu_xlate_changed(cpu);
    }
    if (rising) {   /* NMI, SMI and INIT are edge-triggered */
        if (line == UAM_X86_IRQ_NMI) s->nmi_pending = true;
        if (line == UAM_X86_IRQ_SMI) {
            s->smi_pending = true;
            /* raised by the I/O instruction executing now: SMM sees which */
            if (s->io_icount == cpu->icount && s->io_cur.misc && !s->in_smm) {
                s->io_smi = s->io_cur;
                s->smi_io = true;
            }
        }
        if (line == UAM_X86_IRQ_INIT) {
            if (s->wait_sipi)
                s->sipi_pending = false;   /* INIT while waiting: back to waiting, now */
            else
                s->init_pending = true;
        }
    }
    uam_cpu_request_exit(cpu);
}

static bool intr_ready(uam_cpu *cpu)
{
    return (x86_st(cpu)->irq_lines & (1u << UAM_X86_IRQ_INTR)) &&
           uam_cpu_reg_read(cpu, X86_OFF_FLAG(X86_F_IF), 8);
}

static bool x86_irq_wakeup(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    if (s->wait_sipi)
        return s->sipi_pending || s->init_pending;
    if (s->shutdown)
        return s->init_pending;
    return (s->smi_pending && !s->in_smm) || (s->init_pending && !s->in_smm) ||
           (s->nmi_pending && !s->nmi_blocked) || intr_ready(cpu);
}

static bool x86_irq_take(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    if (s->wait_sipi) {          /* only INIT and SIPI are recognised */
        if (s->init_pending) {
            s->init_pending = false;
            enter_wait_sipi(cpu);
            return false;
        }
        if (!s->sipi_pending)
            return false;
        s->wait_sipi = s->sipi_pending = false;
        cpu->waiting = false;
        uint64_t base = (uint64_t)s->sipi_vector << 12;
        x86_seg_set(cpu, X86_SEG_CS, (uint16_t)(s->sipi_vector << 8), base, 0xFFFF,
                    SEGA_P | SEGA_S | 0xB);
        cpu->pc = base;
        uam_cpu_xlate_changed(cpu);
        return true;
    }
    if (s->smi_pending && !s->in_smm && !s->shutdown) {
        s->smi_pending = false;
        s->halted_in_smm = cpu->waiting;
        cpu->waiting = false;
        x86_smm_enter(cpu);
        return true;
    }
    if (s->init_pending && !s->in_smm) {
        s->init_pending = false;
        cpu->waiting = false;
        bool bsp = s->apic_base & (1u << 8);
        bool sipi = s->sipi_pending;            /* a SIPI that came after this INIT */
        uint8_t vec = s->sipi_vector;
        /* INIT keeps what only RESET clears: SMBASE (SMM relocation survives
         * an INIT-SIPI restart), the SMM range and PAT, the APIC base */
        uint64_t smbase = s->smbase, smrr_base = s->smrr_base, smrr_mask = s->smrr_mask;
        uint64_t pat = s->pat, apic_base = s->apic_base;
        power_on_state(cpu, 0xFFFFFFF0ull);
        s->smbase = smbase;
        s->smrr_base = smrr_base;
        s->smrr_mask = smrr_mask;
        s->pat = pat;
        s->apic_base = apic_base;
        if (!bsp) {
            s->apic_base &= ~(uint64_t)(1u << 8);
            s->sipi_pending = sipi;
            s->sipi_vector = vec;
            enter_wait_sipi(cpu);
            return false;
        }
        return true;
    }
    if (s->shutdown)
        return false;
    if (s->nmi_pending && !s->nmi_blocked) {
        s->nmi_pending = false;
        s->nmi_blocked = true;
        cpu->waiting = false;
        x86_deliver(cpu, X86_EXC_NMI, false, 0, false, cpu->pc, false);
        return true;
    }
    if (intr_ready(cpu) && s->plat.inta) {
        int vec = s->plat.inta(s->plat.ctx);
        if (vec < 0)
            return false;   /* withdrawn before the acknowledge */
        cpu->waiting = false;
        x86_deliver(cpu, (unsigned)vec & 0xFF, false, 0, false, cpu->pc, false);
        return true;
    }
    return false;
}

static uint64_t x86_irq_lines(uam_cpu *cpu) { return x86_st(cpu)->irq_lines; }

/* ---- system registers ---------------------------------------------------- */

static unsigned iopl(uam_cpu *cpu) { return (x86_get_rflags(cpu) & RFLAGS_IOPL) >> 12; }

static bool iopl_ok(uam_cpu *cpu)
{
    return x86_priv_mode(cpu->priv) == X86_MODE_REAL || x86_priv_cpl(cpu->priv) <= iopl(cpu);
}

static uam_status write_cr0(uam_cpu *cpu, uint64_t v)
{
    x86_state *s = x86_st(cpu);
    v |= CR0_ET;
    if ((v >> 32) || ((v & CR0_PG) && !(v & CR0_PE)) || ((v & CR0_NW) && !(v & CR0_CD)))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    bool pg_on = (v & CR0_PG) && !(s->cr0 & CR0_PG);
    if (pg_on && (s->efer & EFER_LME) && !(s->cr4 & CR4_PAE))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    if ((v ^ s->cr0) & (CR0_PG | CR0_PE | CR0_WP)) {
        x86_tlb_flush(cpu);
        uam_cpu_xlate_changed(cpu);
    }
    s->cr0 = v;
    if ((v & CR0_PG) && (s->efer & EFER_LME)) s->efer |= EFER_LMA;
    else                                      s->efer &= ~(uint64_t)EFER_LMA;
    x86_update_priv(cpu);
    return UAM_OK;
}

/* POPF: privilege rules for IOPL and IF; RF and VM are never set this way. */
static uam_status write_rflags_popf(uam_cpu *cpu, uint64_t v)
{
    uint32_t cur = x86_get_rflags(cpu);
    uint32_t mask = RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_TF |
                    RFLAGS_DF | RFLAGS_OF | RFLAGS_NT | RFLAGS_AC | RFLAGS_ID | RFLAGS_IF |
                    RFLAGS_IOPL;
    if (x86_priv_mode(cpu->priv) != X86_MODE_REAL) {
        unsigned cpl = x86_priv_cpl(cpu->priv);
        if (cpl > 0) mask &= ~(uint32_t)RFLAGS_IOPL;
        if (cpl > iopl(cpu)) mask &= ~(uint32_t)RFLAGS_IF;
    }
    if (x86_priv_mode(cpu->priv) == X86_MODE_V86 && iopl(cpu) < 3)
        return x86_raise(cpu, X86_EXC_GP, true, 0);   /* IOPL-sensitive in virtual-8086 mode */
    uint32_t nv = (cur & ~mask) | ((uint32_t)v & mask);
    nv = (nv & ~(uint32_t)(RFLAGS_RF | RFLAGS_VM)) | (cur & RFLAGS_VM);   /* VM stays */
    x86_set_rflags(cpu, nv);
    if ((nv & RFLAGS_IF) && !(cur & RFLAGS_IF))
        uam_cpu_request_exit(cpu);
    return UAM_OK;
}

static uint64_t next_rand(x86_state *s)
{
    uint64_t x = s->rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return s->rng = x;
}

uam_status x86_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val)
{
    x86_state *s = x86_st(cpu);
    switch (id) {
    case X86_SR_CR0: *val = s->cr0; return UAM_OK;
    case X86_SR_CR2: *val = s->cr2; return UAM_OK;
    case X86_SR_CR3: *val = s->cr3; return UAM_OK;
    case X86_SR_CR4: *val = s->cr4; return UAM_OK;
    case X86_SR_CR8: *val = s->cr8; return UAM_OK;
    case X86_SR_DR0: case X86_SR_DR1: case X86_SR_DR2: case X86_SR_DR3:
    case X86_SR_DR4: case X86_SR_DR5: case X86_SR_DR6: case X86_SR_DR7:
        *val = s->dr[id - X86_SR_DR0]; return UAM_OK;
    case X86_SR_RFLAGS:
        if (x86_priv_mode(cpu->priv) == X86_MODE_V86 && iopl(cpu) < 3)
            return x86_raise(cpu, X86_EXC_GP, true, 0);   /* PUSHF/POPF: IOPL-sensitive */
        *val = x86_get_rflags(cpu) & ~(uint32_t)(RFLAGS_RF | RFLAGS_VM);
        return UAM_OK;
    case X86_SR_GDTR_BASE:  *val = s->gdtr.base; return UAM_OK;
    case X86_SR_GDTR_LIMIT: *val = s->gdtr.limit; return UAM_OK;
    case X86_SR_IDTR_BASE:  *val = s->idtr.base; return UAM_OK;
    case X86_SR_IDTR_LIMIT: *val = s->idtr.limit; return UAM_OK;
    case X86_SR_LDTR: *val = s->ldtr.sel; return UAM_OK;
    case X86_SR_TR:   *val = s->tr.sel; return UAM_OK;
    case X86_SR_MSW:  *val = s->cr0 & 0xFFFF; return UAM_OK;
    case X86_SR_TSC:
        if ((s->cr4 & CR4_TSD) && x86_priv_cpl(cpu->priv) > 0)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        *val = x86_tsc_base(cpu) + s->tsc_offset;
        return UAM_OK;
    case X86_SR_TSC_AUX: *val = s->tsc_aux; return UAM_OK;
    case X86_SR_XCR0: *val = s->xcr0; return UAM_OK;
    case X86_SR_RDRAND: *val = next_rand(s); return UAM_OK;
    case X86_SR_CPL0_CHECK:
        *val = 0;
        return x86_priv_cpl(cpu->priv) == 0 ? UAM_OK : x86_raise(cpu, X86_EXC_GP, true, 0);
    }
    return x86_raise(cpu, X86_EXC_UD, false, 0);
}

uam_status x86_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val)
{
    x86_state *s = x86_st(cpu);
    switch (id) {
    case X86_SR_CR0: return write_cr0(cpu, val);
    case X86_SR_CR2: s->cr2 = val; return UAM_OK;
    case X86_SR_CR3:
        s->cr3 = val;
        x86_tlb_flush(cpu);
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    case X86_SR_CR4:
        if ((s->efer & EFER_LMA) && !(val & CR4_PAE))
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        if ((val ^ s->cr4) & (CR4_PAE | CR4_PSE | CR4_PGE | CR4_SMEP | CR4_SMAP | CR4_LA57)) {
            x86_tlb_flush(cpu);
            uam_cpu_xlate_changed(cpu);
        }
        s->cr4 = val;
        return UAM_OK;
    case X86_SR_CR8: s->cr8 = val & 0xF; return UAM_OK;
    case X86_SR_DR0: case X86_SR_DR1: case X86_SR_DR2: case X86_SR_DR3:
    case X86_SR_DR4: case X86_SR_DR5: case X86_SR_DR6: case X86_SR_DR7:
        s->dr[id - X86_SR_DR0] = val; return UAM_OK;
    case X86_SR_RFLAGS: return write_rflags_popf(cpu, val);
    case X86_SR_RFLAGS_INT:
        x86_set_rflags(cpu, (uint32_t)val);
        uam_cpu_request_exit(cpu);
        return UAM_OK;
    case X86_SR_STI: case X86_SR_CLI:
        if (!iopl_ok(cpu))
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_IF), 8, id == X86_SR_STI);
        if (id == X86_SR_STI)
            uam_cpu_request_exit(cpu);
        return UAM_OK;
    case X86_SR_GDTR_BASE:  s->gdtr.base = val; return UAM_OK;
    case X86_SR_GDTR_LIMIT: s->gdtr.limit = (uint32_t)val & 0xFFFF; return UAM_OK;
    case X86_SR_IDTR_BASE:  s->idtr.base = val; return UAM_OK;
    case X86_SR_IDTR_LIMIT: s->idtr.limit = (uint32_t)val & 0xFFFF; return UAM_OK;
    case X86_SR_MSW:   /* LMSW: PE can be set but not cleared */
        return write_cr0(cpu, (s->cr0 & ~0xEull) | (val & 0xF) | (s->cr0 & CR0_PE));
    case X86_SR_CLTS: s->cr0 &= ~(uint64_t)CR0_TS; return UAM_OK;
    case X86_SR_INVLPG:
        x86_tlb_flush(cpu);
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    case X86_SR_XCR0:
        if (!(val & 1) || (val & ~7ull) || ((val & 4) && !(val & 2)))
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        s->xcr0 = val;
        return UAM_OK;
    }
    return x86_raise(cpu, X86_EXC_UD, false, 0);
}

/* ---- configuration ------------------------------------------------------- */

void uam_x86_set_platform(uam_cpu *cpu, const uam_x86_platform *p)
{
    x86_st(cpu)->plat = *p;
}

void uam_x86_set_msr_policy(uam_cpu *cpu, uam_x86_msr_policy p, bool trace)
{
    x86_state *s = x86_st(cpu);
    s->msr_policy = p;
    s->msr_trace = trace;
}

void uam_x86_set_cpuid(uam_cpu *cpu, const uam_x86_cpuid_info *info)
{
    x86_state *s = x86_st(cpu);
    s->cpuid = *info;
    s->cpuid.vendor[12] = 0;
    s->cpuid.brand[48] = 0;
}

void uam_x86_get_cpuid(uam_cpu *cpu, uam_x86_cpuid_info *info)
{
    *info = x86_st(cpu)->cpuid;
}

/* ---- debug dump and snapshots -------------------------------------------- */

static void x86_dump_state(uam_cpu *cpu, FILE *out)
{
    static const char *gp[] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                "r8","r9","r10","r11","r12","r13","r14","r15" };
    static const char *sn[] = { "es", "cs", "ss", "ds", "fs", "gs" };
    x86_state *s = x86_st(cpu);
    for (unsigned i = 0; i < X86_NGPR; i++)
        fprintf(out, "%-3s=%016llx%s", gp[i], (unsigned long long)x86_gpr(cpu, i),
                (i % 4 == 3) ? "\n" : " ");
    fprintf(out, "pc=%016llx rip=%016llx rflags=%08x priv=%#x%s\n",
            (unsigned long long)cpu->pc, (unsigned long long)x86_rip(cpu),
            x86_get_rflags(cpu), cpu->priv, s->in_smm ? " SMM" : "");
    for (unsigned i = 0; i < X86_NSEG; i++)
        fprintf(out, "%s=%04x base=%016llx limit=%08x attr=%04x\n", sn[i], x86_seg_sel(cpu, i),
                (unsigned long long)x86_seg_base(cpu, i),
                (unsigned)uam_cpu_reg_read(cpu, X86_OFF_SEG_LIMIT(i), 32), x86_seg_attr(cpu, i));
    fprintf(out, "cr0=%08llx cr2=%016llx cr3=%016llx cr4=%08llx efer=%llx\n",
            (unsigned long long)s->cr0, (unsigned long long)s->cr2, (unsigned long long)s->cr3,
            (unsigned long long)s->cr4, (unsigned long long)s->efer);
    fprintf(out, "gdtr=%016llx/%04x idtr=%016llx/%04x tr=%04x ldtr=%04x\n",
            (unsigned long long)s->gdtr.base, s->gdtr.limit,
            (unsigned long long)s->idtr.base, s->idtr.limit, s->tr.sel, s->ldtr.sel);
}

static uint64_t *saved_u64(x86_state *s, size_t i)
{
    uint64_t *const f[] = {
        &s->cr0, &s->cr2, &s->cr3, &s->cr4, &s->cr8, &s->efer, &s->xcr0,
        &s->dr[0], &s->dr[1], &s->dr[2], &s->dr[3], &s->dr[6], &s->dr[7],
        &s->gdtr.base, &s->idtr.base, &s->ldtr.base, &s->tr.base, &s->smbase,
        &s->apic_base, &s->kernel_gs_base, &s->star, &s->lstar, &s->cstar, &s->fmask,
        &s->tsc_aux, &s->pat, &s->sysenter_cs, &s->sysenter_esp, &s->sysenter_eip,
        &s->misc_enable, &s->feature_control, &s->tsc_offset, &s->mtrr_def_type, &s->rng,
        &s->smrr_base, &s->smrr_mask, &s->io_icount,
        &s->io_cur.misc, &s->io_cur.rip, &s->io_cur.mem, &s->io_cur.rcx, &s->io_cur.rsi,
        &s->io_cur.rdi, &s->io_smi.misc, &s->io_smi.rip, &s->io_smi.mem, &s->io_smi.rcx,
        &s->io_smi.rsi, &s->io_smi.rdi, &s->io_smm.misc, &s->io_smm.rip, &s->io_smm.mem,
        &s->io_smm.rcx, &s->io_smm.rsi, &s->io_smm.rdi,
        &s->mm[0], &s->mm[1], &s->mm[2], &s->mm[3], &s->mm[4], &s->mm[5], &s->mm[6], &s->mm[7],
    };
    return i < UAM_ARRAY_LEN(f) ? f[i] : NULL;
}

static void x86_save(uam_cpu *cpu, uam_state_writer *w)
{
    x86_state *s = x86_st(cpu);
    uint64_t *p;
    for (size_t i = 0; (p = saved_u64(s, i)) != NULL; i++)
        uam_state_put_u64(w, *p);
    uam_state_put_u32(w, s->gdtr.limit); uam_state_put_u32(w, s->idtr.limit);
    uam_state_put_u32(w, s->ldtr.limit); uam_state_put_u32(w, s->tr.limit);
    uam_state_put_u16(w, s->ldtr.sel); uam_state_put_u16(w, s->ldtr.attr);
    uam_state_put_u16(w, s->tr.sel); uam_state_put_u16(w, s->tr.attr);
    uam_state_put_u32(w, s->irq_lines);
    uam_state_put_u8(w, s->sipi_vector);
    const bool b[] = { s->nmi_pending, s->smi_pending, s->init_pending, s->nmi_blocked,
                       s->in_smm, s->halted_in_smm, s->shutdown, s->wait_sipi, s->sipi_pending,
                       s->smi_io, s->smm_io, s->mmx };
    for (size_t i = 0; i < UAM_ARRAY_LEN(b); i++)
        uam_state_put_bool(w, b[i]);
    for (unsigned i = 0; i < 8; i++) {
        uint8_t fb[10];
        x86_f80_store(fb, s->fpr[i]);
        uam_state_put_bytes(w, fb, sizeof(fb));
    }
    uam_state_put_u32(w, (uint32_t)s->n_msrs);
    for (size_t i = 0; i < s->n_msrs; i++) {
        uam_state_put_u32(w, s->msrs[i].msr);
        uam_state_put_u64(w, s->msrs[i].val);
    }
}

static void x86_restore(uam_cpu *cpu, uam_state_reader *r)
{
    x86_state *s = x86_st(cpu);
    uint64_t *p;
    for (size_t i = 0; (p = saved_u64(s, i)) != NULL; i++)
        *p = uam_state_get_u64(r);
    s->gdtr.limit = uam_state_get_u32(r); s->idtr.limit = uam_state_get_u32(r);
    s->ldtr.limit = uam_state_get_u32(r); s->tr.limit = uam_state_get_u32(r);
    s->ldtr.sel = uam_state_get_u16(r); s->ldtr.attr = uam_state_get_u16(r);
    s->tr.sel = uam_state_get_u16(r); s->tr.attr = uam_state_get_u16(r);
    s->irq_lines = uam_state_get_u32(r);
    s->sipi_vector = uam_state_get_u8(r);
    bool *const b[] = { &s->nmi_pending, &s->smi_pending, &s->init_pending, &s->nmi_blocked,
                        &s->in_smm, &s->halted_in_smm, &s->shutdown, &s->wait_sipi,
                        &s->sipi_pending, &s->smi_io, &s->smm_io, &s->mmx };
    for (size_t i = 0; i < UAM_ARRAY_LEN(b); i++)
        *b[i] = uam_state_get_bool(r);
    for (unsigned i = 0; i < 8; i++) {
        uint8_t fb[10];
        uam_state_get_bytes(r, fb, sizeof(fb));
        s->fpr[i] = x86_f80_load(fb);
    }
    size_t n = uam_state_get_u32(r);
    s->n_msrs = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t m = uam_state_get_u32(r);
        uint64_t v = uam_state_get_u64(r);
        if (s->n_msrs == s->cap_msrs) {
            size_t cap = s->cap_msrs ? s->cap_msrs * 2 : 16;
            x86_msr_ent *e = realloc(s->msrs, cap * sizeof(*e));
            if (!e)
                break;
            s->msrs = e;
            s->cap_msrs = cap;
        }
        s->msrs[s->n_msrs++] = (x86_msr_ent){ m, v };
    }
    s->pend_valid = false;
    x86_tlb_flush(cpu);
    cpu->priv = ~0u;
    x86_update_priv(cpu);
}

/* ---- the descriptor ------------------------------------------------------ */

const uam_isa uam_isa_x86_64 = {
    .name           = "x86-64",
    .addr_width     = 64,
    .default_endian = UAM_ACC_LE,
    .regfile_size   = X86_REGFILE_SIZE,
    .regs           = x86_regs,
    .n_regs         = UAM_ARRAY_LEN(x86_regs),
    .helpers        = { x86_helpers, X86_H__COUNT },
    .init           = x86_init,
    .fini           = x86_fini,
    .reset          = x86_reset,
    .lift           = x86_lift,
    .take_exception = x86_take_exception,
    .irq_take       = x86_irq_take,
    .irq_wakeup     = x86_irq_wakeup,
    .irq_input      = x86_irq_input,
    .sysreg_read    = x86_sysreg_read,
    .sysreg_write   = x86_sysreg_write,
    .translate      = x86_translate,
    .gdb            = &x86_gdb,
    .dump_state     = x86_dump_state,
    .save           = x86_save,
    .restore        = x86_restore,
    .irq_lines      = x86_irq_lines,
    .switch_in      = x86_switch_in,
};
