/*
 * x86_smm.c - System-management mode: SMI entry and RSM.
 *
 * The save state uses the map the board selected (uam_x86_set_smm_map), as
 * the processor vendor documents it; both report SMM revision 0x30064
 * (SMBASE relocation and I/O instruction restart supported):
 *
 * - Intel 64 (default): SMBASE + 0xFC00..0xFFFF. The hidden parts of the
 *   segment and descriptor-table registers, which the map does not document,
 *   are kept in its reserved area at 0x7C00, as hardware does.
 * - AMD64: SMBASE + 0xFE00..0xFFFF, which documents every segment's hidden
 *   part itself. CR2 and XCR0 have no place in it and are left as they are
 *   (SMM entry does not change them); whether NMIs were blocked is kept in
 *   the NMI-mask byte at 0xFECA.
 *
 * Auto-HALT restart and I/O instruction restart are supported: an SMI
 * raised by an IN/OUT/INS/OUTS port access (the chipset traps the access)
 * describes it (Intel: IOMisc, IOMemAddr and the I/O RIP; AMD: the SMM I/O
 * trap dword and the I/O RIP/RCX/RSI/RDI); if the handler leaves FFh in the
 * I/O restart field, RSM re-executes the instruction - for a string
 * instruction, the iteration that was trapped (rCX, rSI and rDI as before
 * it). The platform's smm_changed hook is called before the save state is
 * written on entry and after it is read on RSM, so a chipset model can open
 * and close SMRAM for the processor.
 */
#include "uam/bus.h"
#include "x86.h"

#define SMM_REVID 0x00030064u

/* Intel 64 map: offsets from SMBASE + 0x8000 */
enum {
    SS_HIDDEN = 0x7C00,          /* 6 x { base u64, limit u32, attr u16, sel u16 } */
    SS_GDTR_LIMIT = 0x7C60, SS_IDTR_LIMIT = 0x7C64,
    SS_LDTR_BASE = 0x7C68, SS_LDTR_LIMIT = 0x7C70, SS_LDTR_ATTR = 0x7C74,
    SS_TR_BASE = 0x7C78, SS_TR_LIMIT = 0x7C80, SS_TR_ATTR = 0x7C84,
    SS_CR2 = 0x7C88, SS_XCR0 = 0x7C90, SS_NMI_BLOCKED = 0x7C98,
    SS_GDT_HI = 0x7DD0, SS_LDT_HI = 0x7DD4, SS_IDT_HI = 0x7DD8, SS_IO_RIP = 0x7DE8,
    SS_CR4 = 0x7E40,
    SS_GDT_LO = 0x7E8C, SS_IDT_LO = 0x7E94, SS_LDT_LO = 0x7E9C,
    SS_SMBASE = 0x7EF8, SS_REVID = 0x7EFC, SS_IORESTART = 0x7F00, SS_AUTOHALT = 0x7F02,
    SS_R15 = 0x7F1C,             /* R15..R8, then RAX RCX RDX RBX RSP RBP RSI RDI */
    SS_IO_MEM = 0x7F9C, SS_IO_MISC = 0x7FA4,
    SS_ES = 0x7FA8,              /* ES CS SS DS FS GS selectors, u32 each */
    SS_LDTR = 0x7FC0, SS_TR = 0x7FC4,
    SS_DR7 = 0x7FC8, SS_DR6 = 0x7FD0, SS_RIP = 0x7FD8, SS_EFER = 0x7FE0,
    SS_RFLAGS = 0x7FE8, SS_CR3 = 0x7FF0, SS_CR0 = 0x7FF8,
};

/* AMD64 map: offsets from SMBASE, as AMD documents them */
enum {
    AS_SEG = 0xFE00,             /* ES CS SS DS FS GS: { sel u16, attr u16, limit u32, base u64 } */
    AS_GDTR_LIMIT = 0xFE64, AS_GDTR_BASE = 0xFE68,
    AS_LDTR = 0xFE70,            /* { sel, attr, limit, base } like a segment */
    AS_IDTR_LIMIT = 0xFE84, AS_IDTR_BASE = 0xFE88,
    AS_TR = 0xFE90,
    AS_IO_RIP = 0xFEA0, AS_IO_RCX = 0xFEA8, AS_IO_RSI = 0xFEB0, AS_IO_RDI = 0xFEB8,
    AS_IO_TRAP = 0xFEC0, AS_IORESTART = 0xFEC8, AS_AUTOHALT = 0xFEC9, AS_NMI_MASK = 0xFECA,
    AS_EFER = 0xFED0, AS_SVM_GUEST = 0xFED8,   /* SVM guest, VMCB, virtual interrupt: 0 */
    AS_REVID = 0xFEFC, AS_SMBASE = 0xFF00,
    AS_SSP = 0xFF18,             /* SSP, then SVM guest PAT, host EFER/CR4/CR3/CR0: 0 */
    AS_CR4 = 0xFF48, AS_CR3 = 0xFF50, AS_CR0 = 0xFF58, AS_DR7 = 0xFF60, AS_DR6 = 0xFF68,
    AS_RFLAGS = 0xFF70, AS_RIP = 0xFF78,
    AS_R15 = 0xFF80,             /* R15..R8, then RDI RSI RBP RSP RBX RDX RCX RAX */
};

/* SMM I/O trap dword (AMD): port << 16, the access width, string/REP, IN, valid */
enum { AS_IO_IN = 1u << 0, AS_IO_VALID = 1u << 1, AS_IO_STR = 1u << 2, AS_IO_REP = 1u << 3,
       AS_IO_SZ8 = 1u << 4, AS_IO_SZ16 = 1u << 5, AS_IO_SZ32 = 1u << 6 };

static void put_at(uam_cpu *cpu, unsigned off, unsigned size, uint64_t v)
{
    (void)uam_bus_write(cpu->bus, x86_st(cpu)->smbase + off, size, UAM_ACC_LE | UAM_ACC_PHYS, v);
}

static uint64_t get_at(uam_cpu *cpu, unsigned off, unsigned size)
{
    uint64_t v = 0;
    (void)uam_bus_read(cpu->bus, x86_st(cpu)->smbase + off, size, UAM_ACC_LE | UAM_ACC_PHYS, &v);
    return v;
}

/* Intel offsets are from SMBASE + 0x8000 */
static void put(uam_cpu *cpu, unsigned off, unsigned size, uint64_t v) { put_at(cpu, 0x8000u + off, size, v); }
static uint64_t get(uam_cpu *cpu, unsigned off, unsigned size) { return get_at(cpu, 0x8000u + off, size); }

/* What RSM takes from the save state besides the registers it restores. */
typedef struct rsm_info {
    uint64_t rip, smbase;
    bool     io_restart, auto_halt;
} rsm_info;

/* ---- Intel 64 map --------------------------------------------------------- */

/* GPR order in the map: R15..R8, RAX RCX RDX RBX RSP RBP RSI RDI */
static unsigned map_gpr(unsigned i) { return i < 8 ? 15 - i : i - 8; }

static void intel_save(uam_cpu *cpu, bool halted)
{
    x86_state *s = x86_st(cpu);
    for (unsigned i = 0; i < X86_NSEG; i++) {
        unsigned o = SS_HIDDEN + i * 16;
        put(cpu, o, 8, x86_seg_base(cpu, i));
        put(cpu, o + 8, 4, uam_cpu_reg_read(cpu, X86_OFF_SEG_LIMIT(i), 32));
        put(cpu, o + 12, 2, x86_seg_attr(cpu, i));
        put(cpu, o + 14, 2, x86_seg_sel(cpu, i));
        put(cpu, SS_ES + i * 4, 4, x86_seg_sel(cpu, i));
    }
    put(cpu, SS_GDTR_LIMIT, 4, s->gdtr.limit);
    put(cpu, SS_IDTR_LIMIT, 4, s->idtr.limit);
    put(cpu, SS_LDTR_BASE, 8, s->ldtr.base);
    put(cpu, SS_LDTR_LIMIT, 4, s->ldtr.limit);
    put(cpu, SS_LDTR_ATTR, 2, s->ldtr.attr);
    put(cpu, SS_TR_BASE, 8, s->tr.base);
    put(cpu, SS_TR_LIMIT, 4, s->tr.limit);
    put(cpu, SS_TR_ATTR, 2, s->tr.attr);
    put(cpu, SS_CR2, 8, s->cr2);
    put(cpu, SS_XCR0, 8, s->xcr0);
    put(cpu, SS_NMI_BLOCKED, 1, s->nmi_blocked);
    put(cpu, SS_GDT_HI, 4, s->gdtr.base >> 32);
    put(cpu, SS_GDT_LO, 4, s->gdtr.base & 0xFFFFFFFFu);
    put(cpu, SS_IDT_HI, 4, s->idtr.base >> 32);
    put(cpu, SS_IDT_LO, 4, s->idtr.base & 0xFFFFFFFFu);
    put(cpu, SS_LDT_HI, 4, s->ldtr.base >> 32);
    put(cpu, SS_LDT_LO, 4, s->ldtr.base & 0xFFFFFFFFu);
    put(cpu, SS_CR4, 4, s->cr4);
    put(cpu, SS_SMBASE, 4, s->smbase);
    put(cpu, SS_REVID, 4, SMM_REVID);
    put(cpu, SS_IORESTART, 2, 0);
    put(cpu, SS_IO_MISC, 4, s->smm_io ? s->io_smm.misc : 0);
    put(cpu, SS_IO_MEM, 8, s->smm_io ? s->io_smm.mem : 0);
    put(cpu, SS_IO_RIP, 8, s->smm_io ? s->io_smm.rip : 0);
    put(cpu, SS_AUTOHALT, 2, halted ? 1 : 0);
    for (unsigned i = 0; i < 16; i++)
        put(cpu, SS_R15 + i * 8, 8, x86_gpr(cpu, map_gpr(i)));
    put(cpu, SS_LDTR, 4, s->ldtr.sel);
    put(cpu, SS_TR, 4, s->tr.sel);
    put(cpu, SS_DR7, 8, s->dr[7]);
    put(cpu, SS_DR6, 8, s->dr[6]);
    put(cpu, SS_RIP, 8, x86_rip(cpu));
    put(cpu, SS_EFER, 8, s->efer);
    put(cpu, SS_RFLAGS, 8, x86_get_rflags(cpu));
    put(cpu, SS_CR3, 8, s->cr3);
    put(cpu, SS_CR0, 8, s->cr0);
}

static rsm_info intel_load(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    for (unsigned i = 0; i < X86_NSEG; i++) {
        unsigned o = SS_HIDDEN + i * 16;
        x86_seg_set(cpu, i, (uint16_t)get(cpu, o + 14, 2), get(cpu, o, 8),
                    (uint32_t)get(cpu, o + 8, 4), (uint16_t)get(cpu, o + 12, 2));
    }
    s->gdtr.limit = (uint32_t)get(cpu, SS_GDTR_LIMIT, 4);
    s->idtr.limit = (uint32_t)get(cpu, SS_IDTR_LIMIT, 4);
    s->gdtr.base = get(cpu, SS_GDT_LO, 4) | get(cpu, SS_GDT_HI, 4) << 32;
    s->idtr.base = get(cpu, SS_IDT_LO, 4) | get(cpu, SS_IDT_HI, 4) << 32;
    s->ldtr.base = get(cpu, SS_LDTR_BASE, 8);
    s->ldtr.limit = (uint32_t)get(cpu, SS_LDTR_LIMIT, 4);
    s->ldtr.attr = (uint16_t)get(cpu, SS_LDTR_ATTR, 2);
    s->ldtr.sel = (uint16_t)get(cpu, SS_LDTR, 4);
    s->tr.base = get(cpu, SS_TR_BASE, 8);
    s->tr.limit = (uint32_t)get(cpu, SS_TR_LIMIT, 4);
    s->tr.attr = (uint16_t)get(cpu, SS_TR_ATTR, 2);
    s->tr.sel = (uint16_t)get(cpu, SS_TR, 4);
    s->cr2 = get(cpu, SS_CR2, 8);
    s->xcr0 = get(cpu, SS_XCR0, 8);
    s->nmi_blocked = get(cpu, SS_NMI_BLOCKED, 1) != 0;
    for (unsigned i = 0; i < 16; i++)
        x86_set_gpr(cpu, map_gpr(i), get(cpu, SS_R15 + i * 8, 8));
    s->dr[7] = get(cpu, SS_DR7, 8);
    s->dr[6] = get(cpu, SS_DR6, 8);
    s->cr0 = get(cpu, SS_CR0, 8);
    s->cr3 = get(cpu, SS_CR3, 8);
    s->cr4 = get(cpu, SS_CR4, 4);
    s->efer = get(cpu, SS_EFER, 8);
    x86_set_rflags(cpu, (uint32_t)get(cpu, SS_RFLAGS, 8));
    return (rsm_info){
        .rip = get(cpu, SS_RIP, 8),
        .smbase = get(cpu, SS_SMBASE, 4),
        .io_restart = (get(cpu, SS_IORESTART, 2) & 0xFF) == 0xFF,
        .auto_halt = (get(cpu, SS_AUTOHALT, 2) & 1) != 0,
    };
}

/* ---- AMD64 map ------------------------------------------------------------ */

/* GPR order in the map: R15..R8, then RDI RSI RBP RSP RBX RDX RCX RAX */
static unsigned amd_gpr(unsigned i) { return 15 - i; }

/* Segment attributes in the map: descriptor bits 47:40 in 7:0 and bits 55:52
 * (AVL, L, D/B, G) in 11:8; the core keeps the latter in 15:12. */
static uint16_t amd_attr(uint16_t a)   { return (uint16_t)((a & 0xFFu) | ((a >> 4) & 0xF00u)); }
static uint16_t core_attr(uint16_t a)  { return (uint16_t)((a & 0xFFu) | ((a & 0xF00u) << 4)); }

static void amd_put_seg(uam_cpu *cpu, unsigned o, uint16_t sel, uint16_t attr, uint32_t limit,
                        uint64_t base)
{
    put_at(cpu, o, 2, sel);
    put_at(cpu, o + 2, 2, amd_attr(attr));
    put_at(cpu, o + 4, 4, limit);
    put_at(cpu, o + 8, 8, base);
}

/* The SMM I/O trap dword for an I/O instruction the core recorded (IOMisc). */
static uint32_t amd_io_trap(uint64_t misc)
{
    unsigned type = (unsigned)(misc >> 4) & 0xF, len = (unsigned)(misc >> 1) & 7;
    uint32_t v = (uint32_t)(misc >> 16 & 0xFFFF) << 16 | AS_IO_VALID;
    v |= len == 1 ? AS_IO_SZ8 : len == 2 ? AS_IO_SZ16 : AS_IO_SZ32;
    if (type & 1)
        v |= AS_IO_IN;
    if (type & X86_IO_OUTS)          /* INS/OUTS, with or without REP: types 2, 3, 6, 7 */
        v |= AS_IO_STR;
    if (type & X86_IO_REP)
        v |= AS_IO_REP;
    return v;
}

static void amd_save(uam_cpu *cpu, bool halted)
{
    x86_state *s = x86_st(cpu);
    for (unsigned i = 0; i < X86_NSEG; i++)
        amd_put_seg(cpu, AS_SEG + i * 16, x86_seg_sel(cpu, i), x86_seg_attr(cpu, i),
                    (uint32_t)uam_cpu_reg_read(cpu, X86_OFF_SEG_LIMIT(i), 32), x86_seg_base(cpu, i));
    put_at(cpu, AS_GDTR_LIMIT - 4, 4, 0);
    put_at(cpu, AS_GDTR_LIMIT, 4, s->gdtr.limit & 0xFFFFu);
    put_at(cpu, AS_GDTR_BASE, 8, s->gdtr.base);
    amd_put_seg(cpu, AS_LDTR, s->ldtr.sel, s->ldtr.attr, s->ldtr.limit, s->ldtr.base);
    put_at(cpu, AS_IDTR_LIMIT - 4, 4, 0);
    put_at(cpu, AS_IDTR_LIMIT, 4, s->idtr.limit & 0xFFFFu);
    put_at(cpu, AS_IDTR_BASE, 8, s->idtr.base);
    amd_put_seg(cpu, AS_TR, s->tr.sel, s->tr.attr, s->tr.limit, s->tr.base);
    put_at(cpu, AS_IO_RIP, 8, s->smm_io ? s->io_smm.rip : 0);
    put_at(cpu, AS_IO_RCX, 8, s->smm_io ? s->io_smm.rcx : 0);
    put_at(cpu, AS_IO_RSI, 8, s->smm_io ? s->io_smm.rsi : 0);
    put_at(cpu, AS_IO_RDI, 8, s->smm_io ? s->io_smm.rdi : 0);
    put_at(cpu, AS_IO_TRAP, 4, s->smm_io ? amd_io_trap(s->io_smm.misc) : 0);
    put_at(cpu, AS_IORESTART, 1, 0);
    put_at(cpu, AS_AUTOHALT, 1, halted ? 1 : 0);
    put_at(cpu, AS_NMI_MASK, 1, s->nmi_blocked);
    put_at(cpu, AS_EFER, 8, s->efer);
    for (unsigned o = AS_SVM_GUEST; o < AS_SVM_GUEST + 24; o += 8)
        put_at(cpu, o, 8, 0);
    put_at(cpu, AS_REVID, 4, SMM_REVID);
    put_at(cpu, AS_SMBASE, 4, s->smbase);
    for (unsigned o = AS_SSP; o < AS_CR4; o += 8)
        put_at(cpu, o, 8, 0);
    put_at(cpu, AS_CR4, 8, s->cr4);
    put_at(cpu, AS_CR3, 8, s->cr3);
    put_at(cpu, AS_CR0, 8, s->cr0);
    put_at(cpu, AS_DR7, 8, s->dr[7]);
    put_at(cpu, AS_DR6, 8, s->dr[6]);
    put_at(cpu, AS_RFLAGS, 8, x86_get_rflags(cpu));
    put_at(cpu, AS_RIP, 8, x86_rip(cpu));
    for (unsigned i = 0; i < 16; i++)
        put_at(cpu, AS_R15 + i * 8, 8, x86_gpr(cpu, amd_gpr(i)));
}

static rsm_info amd_load(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    for (unsigned i = 0; i < X86_NSEG; i++) {
        unsigned o = AS_SEG + i * 16;
        x86_seg_set(cpu, i, (uint16_t)get_at(cpu, o, 2), get_at(cpu, o + 8, 8),
                    (uint32_t)get_at(cpu, o + 4, 4), core_attr((uint16_t)get_at(cpu, o + 2, 2)));
    }
    s->gdtr.limit = (uint32_t)get_at(cpu, AS_GDTR_LIMIT, 2);
    s->gdtr.base = get_at(cpu, AS_GDTR_BASE, 8);
    s->idtr.limit = (uint32_t)get_at(cpu, AS_IDTR_LIMIT, 2);
    s->idtr.base = get_at(cpu, AS_IDTR_BASE, 8);
    s->ldtr.sel = (uint16_t)get_at(cpu, AS_LDTR, 2);
    s->ldtr.attr = core_attr((uint16_t)get_at(cpu, AS_LDTR + 2, 2));
    s->ldtr.limit = (uint32_t)get_at(cpu, AS_LDTR + 4, 4);
    s->ldtr.base = get_at(cpu, AS_LDTR + 8, 8);
    s->tr.sel = (uint16_t)get_at(cpu, AS_TR, 2);
    s->tr.attr = core_attr((uint16_t)get_at(cpu, AS_TR + 2, 2));
    s->tr.limit = (uint32_t)get_at(cpu, AS_TR + 4, 4);
    s->tr.base = get_at(cpu, AS_TR + 8, 8);
    s->nmi_blocked = (get_at(cpu, AS_NMI_MASK, 1) & 1) != 0;
    s->efer = get_at(cpu, AS_EFER, 8);
    s->cr4 = get_at(cpu, AS_CR4, 8);
    s->cr3 = get_at(cpu, AS_CR3, 8);
    s->cr0 = get_at(cpu, AS_CR0, 8);
    s->dr[7] = get_at(cpu, AS_DR7, 8);
    s->dr[6] = get_at(cpu, AS_DR6, 8);
    x86_set_rflags(cpu, (uint32_t)get_at(cpu, AS_RFLAGS, 8));
    for (unsigned i = 0; i < 16; i++)
        x86_set_gpr(cpu, amd_gpr(i), get_at(cpu, AS_R15 + i * 8, 8));
    return (rsm_info){
        .rip = get_at(cpu, AS_RIP, 8),
        .smbase = get_at(cpu, AS_SMBASE, 4),
        .io_restart = get_at(cpu, AS_IORESTART, 1) == 0xFF,
        .auto_halt = (get_at(cpu, AS_AUTOHALT, 1) & 1) != 0,
    };
}

/* ---- entry and RSM -------------------------------------------------------- */

void x86_smm_enter(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    bool halted = s->halted_in_smm;   /* set by the caller: the SMI woke a HLT */
    s->in_smm = true;
    if (s->plat.smm_changed)
        s->plat.smm_changed(s->plat.ctx, true);

    s->smm_io = s->smi_io;           /* the I/O instruction behind this SMI, if any */
    s->io_smm = s->io_smi;
    s->smi_io = false;
    if (s->smm_map == UAM_X86_SMM_MAP_AMD64)
        amd_save(cpu, halted);
    else
        intel_save(cpu, halted);

    /* SMM entry state: real-address mode with 4 GiB segment limits */
    const uint16_t data = SEGA_P | SEGA_S | 0x3, code = SEGA_P | SEGA_S | 0xB;
    for (unsigned i = 0; i < X86_NSEG; i++)
        x86_seg_set(cpu, i, 0, 0, 0xFFFFFFFFu, data | SEGA_G);
    x86_seg_set(cpu, X86_SEG_CS, (uint16_t)(s->smbase >> 4), s->smbase, 0xFFFFFFFFu, code | SEGA_G);
    x86_set_rflags(cpu, RFLAGS_ALWAYS_1);
    s->cr0 &= ~(uint64_t)(CR0_PE | CR0_EM | CR0_TS | CR0_PG);
    s->cr4 = 0;
    s->efer = 0;
    s->dr[7] = 0x400;
    s->nmi_blocked = true;
    s->halted_in_smm = false;
    x86_tlb_flush(cpu);
    x86_update_priv(cpu);
    uam_cpu_xlate_changed(cpu);
    cpu->pc = s->smbase + 0x8000u;
}

uam_status x86_smm_rsm(uam_cpu *cpu, uint64_t *new_pc)
{
    x86_state *s = x86_st(cpu);
    rsm_info ri = s->smm_map == UAM_X86_SMM_MAP_AMD64 ? amd_load(cpu) : intel_load(cpu);
    uint64_t rip = ri.rip;
    /* I/O instruction restart: back to the trapped I/O instruction (the
     * iteration it was, for a string instruction) */
    if (s->smm_io && ri.io_restart) {
        rip = s->io_smm.rip;
        if ((s->io_smm.misc >> 4) & X86_IO_OUTS) {          /* INS/OUTS: types 2, 3, 6, 7 */
            x86_set_gpr(cpu, X86_RCX, s->io_smm.rcx);
            x86_set_gpr(cpu, X86_RSI, s->io_smm.rsi);
            x86_set_gpr(cpu, X86_RDI, s->io_smm.rdi);
        }
    } else if (ri.auto_halt) {
        /* Auto-HALT restart: the SMI interrupted HLT and the handler left bit 0
         * set, so execution resumes at the HLT (one byte back) and halts again. */
        rip -= 1;
    }
    s->smm_io = false;

    s->in_smm = false;
    if (s->plat.smm_changed)
        s->plat.smm_changed(s->plat.ctx, false);
    s->smbase = ri.smbase;   /* the handler may relocate SMBASE */
    x86_tlb_flush(cpu);
    x86_update_priv(cpu);
    uam_cpu_xlate_changed(cpu);
    uint64_t pc = x86_seg_base(cpu, X86_SEG_CS) + rip;
    if (x86_priv_code(cpu->priv) != X86_CODE64)
        pc &= 0xFFFFFFFFull;
    *new_pc = pc;
    return UAM_OK;
}
