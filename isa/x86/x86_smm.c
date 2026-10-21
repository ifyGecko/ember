/*
 * x86_smm.c - System-management mode: SMI entry and RSM.
 *
 * The save state uses the Intel 64-bit SMRAM save-state map at
 * SMBASE + 0xFC00 (SMM revision 0x30064: SMBASE relocation supported). The
 * hidden parts of the segment and descriptor-table registers, which the map
 * does not document, are kept in its reserved area at 0x7C00, as hardware
 * does. The platform's smm_changed hook is called before the save state is
 * written on entry and after it is read on RSM, so a chipset model can open
 * and close SMRAM for the processor.
 */
#include "uam/bus.h"
#include "x86.h"

#define SMM_REVID 0x00030064u

/* offsets from SMBASE + 0x8000 */
enum {
    SS_HIDDEN = 0x7C00,          /* 6 x { base u64, limit u32, attr u16, sel u16 } */
    SS_GDTR_LIMIT = 0x7C60, SS_IDTR_LIMIT = 0x7C64,
    SS_LDTR_BASE = 0x7C68, SS_LDTR_LIMIT = 0x7C70, SS_LDTR_ATTR = 0x7C74,
    SS_TR_BASE = 0x7C78, SS_TR_LIMIT = 0x7C80, SS_TR_ATTR = 0x7C84,
    SS_CR2 = 0x7C88, SS_XCR0 = 0x7C90, SS_NMI_BLOCKED = 0x7C98,
    SS_GDT_HI = 0x7DD0, SS_LDT_HI = 0x7DD4, SS_IDT_HI = 0x7DD8,
    SS_CR4 = 0x7E40,
    SS_GDT_LO = 0x7E8C, SS_IDT_LO = 0x7E94, SS_LDT_LO = 0x7E9C,
    SS_SMBASE = 0x7EF8, SS_REVID = 0x7EFC, SS_IORESTART = 0x7F00, SS_AUTOHALT = 0x7F02,
    SS_R15 = 0x7F1C,             /* R15..R8, then RAX RCX RDX RBX RSP RBP RSI RDI */
    SS_ES = 0x7FA8,              /* ES CS SS DS FS GS selectors, u32 each */
    SS_LDTR = 0x7FC0, SS_TR = 0x7FC4,
    SS_DR7 = 0x7FC8, SS_DR6 = 0x7FD0, SS_RIP = 0x7FD8, SS_EFER = 0x7FE0,
    SS_RFLAGS = 0x7FE8, SS_CR3 = 0x7FF0, SS_CR0 = 0x7FF8,
};

static uint64_t st_base(x86_state *s) { return s->smbase + 0x8000u; }

static void put(uam_cpu *cpu, unsigned off, unsigned size, uint64_t v)
{
    (void)uam_bus_write(cpu->bus, st_base(x86_st(cpu)) + off, size, UAM_ACC_LE | UAM_ACC_PHYS, v);
}

static uint64_t get(uam_cpu *cpu, unsigned off, unsigned size)
{
    uint64_t v = 0;
    (void)uam_bus_read(cpu->bus, st_base(x86_st(cpu)) + off, size, UAM_ACC_LE | UAM_ACC_PHYS, &v);
    return v;
}

/* GPR order in the map: R15..R8, RAX RCX RDX RBX RSP RBP RSI RDI */
static unsigned map_gpr(unsigned i) { return i < 8 ? 15 - i : i - 8; }

void x86_smm_enter(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    bool halted = s->halted_in_smm;   /* set by the caller: the SMI woke a HLT */
    s->in_smm = true;
    if (s->plat.smm_changed)
        s->plat.smm_changed(s->plat.ctx, true);

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
    uint64_t rip = get(cpu, SS_RIP, 8);
    uint64_t smbase = get(cpu, SS_SMBASE, 4);   /* the handler may relocate SMBASE */

    s->in_smm = false;
    if (s->plat.smm_changed)
        s->plat.smm_changed(s->plat.ctx, false);
    s->smbase = smbase;
    x86_tlb_flush(cpu);
    x86_update_priv(cpu);
    uam_cpu_xlate_changed(cpu);
    uint64_t pc = x86_seg_base(cpu, X86_SEG_CS) + rip;
    if (x86_priv_code(cpu->priv) != X86_CODE64)
        pc &= 0xFFFFFFFFull;
    *new_pc = pc;
    return UAM_OK;
}
