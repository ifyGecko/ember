/*
 * x86_mmu.c - x86 paging (isa->translate) and linear-address memory access
 * from C (descriptor tables, interrupt frames, helpers).
 *
 * Supported: 32-bit paging (4 KiB, and 4 MiB with CR4.PSE), PAE paging
 * (4 KiB, 2 MiB) and 4-level IA-32e paging (4 KiB, 2 MiB, 1 GiB), with the
 * R/W, U/S and XD permission bits and CR0.WP. Accessed and dirty bits are
 * not written back (firmware does not depend on them). Walks are cached in a
 * small direct-mapped TLB that is invalidated by every xlate_gen bump.
 */
#include "uam/bus.h"
#include "x86.h"

enum { PERM_W = 1, PERM_U = 2, PERM_X = 4 };

#define PTE_P   (1ull << 0)
#define PTE_RW  (1ull << 1)
#define PTE_US  (1ull << 2)
#define PTE_PS  (1ull << 7)
#define PTE_XD  (1ull << 63)
#define ADDR_MASK 0x000FFFFFFFFFF000ull

void x86_tlb_flush(uam_cpu *cpu)
{
    x86_state *s = x86_st(cpu);
    for (unsigned i = 0; i < X86_TLB_SIZE; i++)
        s->tlb[i].vpage = UINT64_MAX;
}

static bool rd_phys(uam_cpu *cpu, uint64_t pa, unsigned size, uint64_t *v)
{
    return uam_bus_read(cpu->bus, pa, size, UAM_ACC_LE | UAM_ACC_PHYS | UAM_ACC_DEBUG, v) == UAM_OK;
}

/*
 * Walk the page tables for linear address `va`. On success returns true with
 * the page's physical base, size shift and accumulated permissions. On a
 * non-present entry or reserved-bit violation returns false with *fault set
 * to PFEC_P/PFEC_RSVD (the access-type bits are added by the caller).
 */
static bool walk(uam_cpu *cpu, uint64_t va, uint64_t *pbase, unsigned *shift, unsigned *perm,
                 uint32_t *fault)
{
    x86_state *s = x86_st(cpu);
    bool nxe = (s->efer & EFER_NXE) != 0;
    unsigned p = PERM_W | PERM_U | PERM_X;
    uint64_t e = 0;
    *fault = 0;

    if (s->efer & EFER_LMA) {   /* 4-level */
        uint64_t table = s->cr3 & ADDR_MASK;
        for (int level = 3; level >= 0; level--) {
            unsigned sh = 12 + 9 * (unsigned)level;
            uint64_t idx = (va >> sh) & 0x1FF;
            if (!rd_phys(cpu, table + idx * 8, 8, &e) || !(e & PTE_P))
                return false;
            if (!(e & PTE_RW)) p &= ~(unsigned)PERM_W;
            if (!(e & PTE_US)) p &= ~(unsigned)PERM_U;
            if (nxe && (e & PTE_XD)) p &= ~(unsigned)PERM_X;
            if (level > 0 && (e & PTE_PS)) {
                if (level == 3) { *fault = PFEC_P | PFEC_RSVD; return false; }
                *shift = sh;
                *pbase = e & ADDR_MASK & ~((1ull << sh) - 1);
                *perm = p;
                return true;
            }
            table = e & ADDR_MASK;
        }
        *shift = 12;
        *pbase = table;
        *perm = p;
        return true;
    }

    if (s->cr4 & CR4_PAE) {     /* PAE: PDPT -> PD -> PT */
        uint64_t pdpt = s->cr3 & 0xFFFFFFE0u;
        if (!rd_phys(cpu, pdpt + ((va >> 30) & 3) * 8, 8, &e) || !(e & PTE_P))
            return false;
        uint64_t table = e & ADDR_MASK;
        for (int level = 1; level >= 0; level--) {
            unsigned sh = 12 + 9 * (unsigned)level;
            uint64_t idx = (va >> sh) & 0x1FF;
            if (!rd_phys(cpu, table + idx * 8, 8, &e) || !(e & PTE_P))
                return false;
            if (!(e & PTE_RW)) p &= ~(unsigned)PERM_W;
            if (!(e & PTE_US)) p &= ~(unsigned)PERM_U;
            if (nxe && (e & PTE_XD)) p &= ~(unsigned)PERM_X;
            if (level == 1 && (e & PTE_PS)) {
                *shift = 21;
                *pbase = e & ADDR_MASK & ~0x1FFFFFull;
                *perm = p;
                return true;
            }
            table = e & ADDR_MASK;
        }
        *shift = 12;
        *pbase = table;
        *perm = p;
        return true;
    }

    /* 32-bit paging */
    uint64_t pd = s->cr3 & 0xFFFFF000u;
    if (!rd_phys(cpu, pd + ((va >> 22) & 0x3FF) * 4, 4, &e) || !(e & PTE_P))
        return false;
    if (!(e & PTE_RW)) p &= ~(unsigned)PERM_W;
    if (!(e & PTE_US)) p &= ~(unsigned)PERM_U;
    if ((e & PTE_PS) && (s->cr4 & CR4_PSE)) {
        *shift = 22;
        *pbase = (e & 0xFFC00000u) | ((e >> 13) & 0xFFull) << 32;   /* PSE-36 */
        *perm = p;
        return true;
    }
    uint64_t pt = e & 0xFFFFF000u;
    if (!rd_phys(cpu, pt + ((va >> 12) & 0x3FF) * 4, 4, &e) || !(e & PTE_P))
        return false;
    if (!(e & PTE_RW)) p &= ~(unsigned)PERM_W;
    if (!(e & PTE_US)) p &= ~(unsigned)PERM_U;
    *shift = 12;
    *pbase = e & 0xFFFFF000u;
    *perm = p;
    return true;
}

/* Translate with an explicit privilege: user = a CPL-3 access. */
static uam_status xlate(uam_cpu *cpu, uint64_t va, uam_xlate_kind kind, bool user,
                        uint64_t *pa, uint32_t *fault)
{
    x86_state *s = x86_st(cpu);
    uint64_t vpage = va >> 12;
    x86_tlb_ent *t = &s->tlb[vpage & (X86_TLB_SIZE - 1)];
    unsigned perm;
    uint64_t ppage;

    if (t->vpage == vpage && t->gen == cpu->xlate_gen) {
        perm = t->perm;
        ppage = t->ppage;
    } else {
        uint64_t base;
        unsigned shift;
        uint32_t f;
        if (!walk(cpu, va, &base, &shift, &perm, &f)) {
            *fault = f | (kind == UAM_XLATE_STORE ? PFEC_W : 0) | (user ? PFEC_U : 0) |
                     (kind == UAM_XLATE_FETCH && (s->efer & EFER_NXE) ? PFEC_I : 0);
            return UAM_ERR_XLATE;
        }
        ppage = base + (va & ((1ull << shift) - 1) & ~0xFFFull);
        if (kind != UAM_XLATE_DEBUG) {
            t->vpage = vpage;
            t->ppage = ppage;
            t->perm = (uint8_t)perm;
            t->gen = cpu->xlate_gen;
        }
    }

    if (kind != UAM_XLATE_DEBUG) {
        bool ok = true;
        if (user && !(perm & PERM_U))
            ok = false;
        if (kind == UAM_XLATE_STORE && !(perm & PERM_W) && (user || (s->cr0 & CR0_WP)))
            ok = false;
        if (kind == UAM_XLATE_FETCH && !(perm & PERM_X))
            ok = false;
        if (!ok) {
            *fault = PFEC_P | (kind == UAM_XLATE_STORE ? PFEC_W : 0) | (user ? PFEC_U : 0) |
                     (kind == UAM_XLATE_FETCH && (s->efer & EFER_NXE) ? PFEC_I : 0);
            return UAM_ERR_XLATE;
        }
    }
    *pa = ppage | (va & 0xFFF);
    return UAM_OK;
}

uam_status x86_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out)
{
    x86_state *s = x86_st(cpu);
    out->fault = 0;
    if (!(s->cr0 & CR0_PG)) {
        out->paddr = (s->efer & EFER_LMA) ? vaddr : (vaddr & 0xFFFFFFFFull);
        out->fixed = true;   /* paging changes re-key code through cpu->priv */
        return UAM_OK;
    }
    out->fixed = false;
    bool user = kind != UAM_XLATE_DEBUG && x86_priv_cpl(cpu->priv) == 3;
    return xlate(cpu, vaddr, kind, user, &out->paddr, &out->fault);
}

/* ---- linear-address access from C ---------------------------------------- */

static uam_status lin_access(uam_cpu *cpu, uint64_t lin, unsigned size, uint64_t *val,
                             bool write, bool sup)
{
    x86_state *s = x86_st(cpu);
    uint64_t v = write ? *val : 0;
    uint64_t r = 0;
    unsigned done = 0;
    while (done < size) {
        uint64_t va = lin + done;
        unsigned n = size - done;
        unsigned room = 4096u - (unsigned)(va & 0xFFF);
        if (n > room) n = room;
        uint64_t pa = va;
        if (s->cr0 & CR0_PG) {
            uint32_t fault = 0;
            bool user = !sup && x86_priv_cpl(cpu->priv) == 3;
            if (xlate(cpu, va, write ? UAM_XLATE_STORE : UAM_XLATE_LOAD, user, &pa, &fault) != UAM_OK) {
                s->cr2 = va;
                return x86_raise(cpu, X86_EXC_PF, true, fault);
            }
        } else if (!(s->efer & EFER_LMA)) {
            pa &= 0xFFFFFFFFull;
        }
        /* bytes [done, done + n) in one physical piece; n is a power of two or split bytewise */
        for (unsigned i = 0; i < n;) {
            unsigned chunk = (n - i >= 8 && !((pa + i) & 7)) ? 8 : (n - i >= 4 && !((pa + i) & 3)) ? 4
                           : (n - i >= 2 && !((pa + i) & 1)) ? 2 : 1;
            uint64_t part;
            if (write) {
                part = (v >> (8 * (done + i))) & (chunk == 8 ? ~0ull : ((1ull << (8 * chunk)) - 1));
                if (uam_bus_write(cpu->bus, pa + i, chunk, UAM_ACC_LE | UAM_ACC_PHYS, part) != UAM_OK)
                    return x86_raise(cpu, X86_EXC_GP, true, 0);
            } else {
                part = 0;
                if (uam_bus_read(cpu->bus, pa + i, chunk, UAM_ACC_LE | UAM_ACC_PHYS, &part) != UAM_OK)
                    return x86_raise(cpu, X86_EXC_GP, true, 0);
                r |= part << (8 * (done + i));
            }
            i += chunk;
        }
        done += n;
    }
    if (!write)
        *val = r;
    return UAM_OK;
}

uam_status x86_lin_read(uam_cpu *cpu, uint64_t lin, unsigned size, uint64_t *val, bool sup)
{
    return lin_access(cpu, lin, size, val, false, sup);
}

uam_status x86_lin_write(uam_cpu *cpu, uint64_t lin, unsigned size, uint64_t val, bool sup)
{
    return lin_access(cpu, lin, size, &val, true, sup);
}
