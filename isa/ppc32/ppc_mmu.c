/*
 * ppc_mmu.c - 32-bit PowerPC address translation (the OEA MMU).
 *
 * With MSR[IR] (fetch) or MSR[DR] (data) clear, effective addresses are real
 * addresses. Otherwise the block address translation registers are searched
 * first (IBATs for fetch, DBATs for data; a BAT is valid in supervisor state when
 * Vs is set and in problem state when Vp is set), then the segment register the
 * top four address bits select gives a 24-bit VSID and the page is looked up in
 * the hashed page table at SDR1: the primary PTE group, then the secondary one,
 * eight PTEs each. Protection comes from the BAT's PP bits, or from the segment's
 * Ks/Kp key with the PTE's PP bits; a fetch is refused from a no-execute (N)
 * segment, a guarded page or a direct-store segment. A successful page table
 * translation sets the PTE's referenced bit, and its changed bit on a store,
 * in memory.
 *
 * (Book E translation is in ppc_booke.c.)
 *
 * Like a hardware TLB, successful translations are kept (the software TLB below)
 * until tlbie, tlbia, or a change of the segment registers, SDR1, a BAT or the
 * MSR's translation and privilege bits; software that edits a PTE invalidates it
 * with tlbie, as the architecture requires. tlbie reaches every PowerPC hart on
 * the same bus, as the broadcast does on a real system bus.
 */
#include "uam/bus.h"

#include "ppc.h"

static inline unsigned tlb_set(uint32_t vpage) { return (vpage ^ (vpage >> 8)) & (PPC_TLB_SETS - 1); }

void ppc_tlb_flush(ppc_state *s)
{
    for (unsigned i = 0; i < PPC_TLB_SETS; i++)
        s->itlb[i].vpage = s->dtlb[i].vpage = PPC_TLB_NONE;
}

void ppc_tlb_flush_page(ppc_state *s, uint32_t ea)
{
    /* tlbie: the page's entries, and (like the hardware's congruence class) any entry
     * of the same set, in either TLB. */
    unsigned k = tlb_set(ea >> 12);
    s->itlb[k].vpage = s->dtlb[k].vpage = PPC_TLB_NONE;
}

/* Protection by PP and key: 0 none, 1 read only, 2 read/write. */
static unsigned access_of(unsigned pp, unsigned key)
{
    if (!key)
        return pp == 3 ? 1 : 2;
    return pp == 0 ? 0 : pp == 2 ? 2 : 1;
}

static bool pa_read32(uam_cpu *cpu, uint32_t pa, uint32_t *v)
{
    uint64_t x = 0;
    if (uam_bus_read(cpu->bus, pa, 4, UAM_ACC_BE | UAM_ACC_PHYS | UAM_ACC_DEBUG, &x) != UAM_OK)
        return false;
    *v = (uint32_t)x;
    return true;
}

/* A BAT hit: *pa set; returns false when no BAT of the kind covers ea. */
static bool bat_lookup(const ppc_state *s, uint32_t ea, bool fetch, bool pr, uint32_t *pa,
                       unsigned *acc)
{
    const uint32_t (*bat)[2] = fetch ? s->s.ibat : s->s.dbat;
    for (unsigned n = 0; n < s->cfg.bat_pairs; n++) {
        uint32_t u = bat[n][0], l = bat[n][1];
        if (!(u & (pr ? 1u : 2u)))
            continue;
        uint32_t bl = (u >> 2) & 0x7FF;
        uint32_t mask = ~(bl << 17) & 0xFFFE0000u;   /* EA bits compared with BEPI */
        if ((ea & mask) != (u & mask))
            continue;
        *pa = ((l & 0xFFFE0000u) & mask) | (ea & ~mask);
        *acc = access_of(l & 3, 1);   /* PP 00 none, x1 read only, 10 read/write */
        return true;
    }
    return false;
}

/*
 * Search the hashed page table. Returns 0 and the PTE's address and second word on a
 * hit, or the fault bits.
 */
static uint32_t pte_search(uam_cpu *cpu, const ppc_state *s, uint32_t vsid, uint32_t ea,
                           uint32_t *pte_pa, uint32_t *w1)
{
    uint32_t pidx = (ea >> 12) & 0xFFFF, api = pidx >> 10;
    uint32_t org = s->s.sdr1 & 0xFFFF0000u, hmask = s->s.sdr1 & 0x1FF;
    uint32_t hash = (vsid & 0x7FFFF) ^ pidx;
    for (unsigned h = 0; h < 2; h++, hash = ~hash & 0x7FFFF) {
        uint32_t pteg = (org & 0xFE000000u)
                      | ((((org >> 16) & 0x1FF) | ((hash >> 10) & hmask)) << 16)
                      | ((hash & 0x3FF) << 6);
        uint32_t want = 0x80000000u | (vsid << 7) | (h << 6) | api;
        for (unsigned k = 0; k < 8; k++) {
            uint32_t w0;
            if (!pa_read32(cpu, pteg + 8 * k, &w0) || w0 != want)
                continue;
            if (!pa_read32(cpu, pteg + 8 * k + 4, w1))
                continue;
            *pte_pa = pteg + 8 * k;
            return 0;
        }
    }
    return PPC_XF_NOPTE;
}

/* Translate for a fetch or data access; `debug` has no side effects (no R/C update,
 * no software TLB) and the supervisor's view of protection. */
static uam_status xlate(uam_cpu *cpu, uint32_t ea, bool fetch, bool store, bool debug,
                        uam_xlate *out)
{
    ppc_state *s = ppc_st(cpu);
    const uint32_t msr = s->s.msr;
    const bool pr = !debug && (msr & MSR_PR);

    /* The mapping depends on MSR, BATs, segments and SDR1, which all may change. */
    out->fixed = false;
    if (!(msr & (fetch ? MSR_IR : MSR_DR))) {
        out->paddr = ea;
        return UAM_OK;
    }

    ppc_tlb_entry *te = NULL;
    if (!debug) {
        te = &(fetch ? s->itlb : s->dtlb)[tlb_set(ea >> 12)];
        if (te->vpage == ea >> 12 && (!store || te->write_ok)) {
            out->paddr = (te->ppage << 12) | (ea & 0xFFF);
            return UAM_OK;
        }
    }

    uint32_t pa;
    unsigned acc;
    if (bat_lookup(s, ea, fetch, pr, &pa, &acc)) {
        if (acc == 0 || (store && acc != 2)) {
            out->fault = PPC_XF_PROT;
            return UAM_ERR_XLATE;
        }
        out->paddr = pa;
        if (te)
            *te = (ppc_tlb_entry){ .vpage = ea >> 12, .ppage = pa >> 12, .read_ok = true,
                                   .write_ok = acc == 2 };
        return UAM_OK;
    }

    uint32_t sr = s->s.sr[ea >> 28];
    if (sr & 0x80000000u) {          /* T: a direct-store segment */
        out->fault = fetch ? PPC_XF_NOEXEC : PPC_XF_DS;
        return UAM_ERR_XLATE;
    }
    if (fetch && (sr & 0x10000000u)) {   /* N: no-execute segment */
        out->fault = PPC_XF_NOEXEC;
        return UAM_ERR_XLATE;
    }
    unsigned key = pr ? (sr >> 29) & 1 : (sr >> 30) & 1;   /* Kp : Ks */
    uint32_t pte_pa, w1;
    uint32_t xf = pte_search(cpu, s, sr & 0xFFFFFF, ea, &pte_pa, &w1);
    if (xf) {
        out->fault = xf;
        return UAM_ERR_XLATE;
    }
    if (fetch && (w1 & 0x8)) {       /* G: no fetch from guarded storage */
        out->fault = PPC_XF_NOEXEC;
        return UAM_ERR_XLATE;
    }
    acc = access_of(w1 & 3, debug ? 0 : key);
    if (acc == 0 || (store && acc != 2)) {
        out->fault = PPC_XF_PROT;
        return UAM_ERR_XLATE;
    }
    out->paddr = (w1 & 0xFFFFF000u) | (ea & 0xFFF);
    if (debug)
        return UAM_OK;

    /* Referenced, and changed on a store. */
    uint32_t nw1 = w1 | 0x100 | (store ? 0x80 : 0);
    if (nw1 != w1)
        uam_bus_write(cpu->bus, pte_pa + 4, 4, UAM_ACC_BE | UAM_ACC_PHYS, nw1);
    *te = (ppc_tlb_entry){ .vpage = ea >> 12, .ppage = w1 >> 12, .read_ok = true,
                           .write_ok = acc == 2 && (nw1 & 0x80) };
    return UAM_OK;
}

uam_status ppc_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out)
{
    if (ppc_booke(ppc_st(cpu)))
        return ppc_booke_translate(cpu, (uint32_t)vaddr, kind == UAM_XLATE_FETCH,
                                   kind == UAM_XLATE_STORE, kind == UAM_XLATE_DEBUG, out);
    return xlate(cpu, (uint32_t)vaddr, kind == UAM_XLATE_FETCH, kind == UAM_XLATE_STORE,
                 kind == UAM_XLATE_DEBUG, out);
}

bool ppc_peek_insn(uam_cpu *cpu, uint32_t pc, uint32_t *insn)
{
    uam_xlate xl = { 0 };
    uam_status st = ppc_booke(ppc_st(cpu)) ? ppc_booke_translate(cpu, pc, true, false, true, &xl)
                                           : xlate(cpu, pc, true, false, true, &xl);
    return st == UAM_OK && pa_read32(cpu, (uint32_t)xl.paddr, insn);
}

/*
 * A data access on behalf of a helper (lswx, stswx): translated and performed like
 * the executor's, with the failure recorded for take_exception.
 */
uam_status ppc_mem_rw(uam_cpu *cpu, uint32_t ea, unsigned size, bool store, uint64_t *val)
{
    ppc_state *s = ppc_st(cpu);
    uam_xlate xl = { 0 };
    uam_status st = ppc_translate(cpu, ea, store ? UAM_XLATE_STORE : UAM_XLATE_LOAD, &xl);
    if (st != UAM_OK) {
        s->hfault = PPC_HF_DSI;
        s->hf_ea = ea;
        s->hf_dsisr = xl.fault | (store ? DSISR_STORE : 0);
        return st;
    }
    st = store ? uam_bus_write(cpu->bus, xl.paddr, size, UAM_ACC_BE, *val)
               : uam_bus_read(cpu->bus, xl.paddr, size, UAM_ACC_BE, val);
    if (st != UAM_OK) {
        s->hfault = PPC_HF_MCHECK;
        s->hf_ea = ea;
    }
    return st;
}
