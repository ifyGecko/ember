/*
 * x86_msr.c - CPUID and model-specific registers.
 *
 * CPUID describes exactly what this module implements: x86-64 with SSE2,
 * CMPXCHG16B, POPCNT, RDRAND, NX, 1 GiB pages, RDTSCP, PAT and MTRRs.
 * Vendor, signature, brand, topology and physical-address width come from
 * the board (uam_x86_set_cpuid). MSRs the core does not implement go first
 * to the platform hook, then to the configured policy.
 */
#include <stdlib.h>
#include <string.h>

#include "x86.h"

void x86_cpuid_defaults(uam_x86_cpuid_info *info)
{
    memset(info, 0, sizeof(*info));
    memcpy(info->vendor, "GenuineIntel", 13);
    info->signature = 0x000506E3u;   /* family 6 */
    strcpy(info->brand, "UAM x86-64 reference processor");
    info->logical_cpus = 1;
    info->phys_addr_bits = 39;
    info->initial_apic_id = 0;
}

static uint32_t str4(const char *p)
{
    return (uint32_t)(uint8_t)p[0] | (uint32_t)(uint8_t)p[1] << 8 |
           (uint32_t)(uint8_t)p[2] << 16 | (uint32_t)(uint8_t)p[3] << 24;
}

static unsigned log2_ceil(unsigned n)
{
    unsigned l = 0;
    while ((1u << l) < n)
        l++;
    return l;
}

void x86_cpuid(uam_cpu *cpu, uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    const uam_x86_cpuid_info *c = &x86_st(cpu)->cpuid;
    unsigned n = c->logical_cpus ? c->logical_cpus : 1;
    r[0] = r[1] = r[2] = r[3] = 0;
    switch (leaf) {
    case 0:
        r[0] = 0xB;
        r[1] = str4(c->vendor); r[3] = str4(c->vendor + 4); r[2] = str4(c->vendor + 8);
        return;
    case 1:
        r[0] = c->signature;
        r[1] = (8u << 8) | ((n & 0xFF) << 16) | (c->initial_apic_id << 24);
        r[2] = (1u << 13) | (1u << 23) | (1u << 30);          /* CX16 POPCNT RDRAND */
        r[3] = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) |
               (1u << 9) | (1u << 12) | (1u << 13) | (1u << 15) | (1u << 16) | (1u << 17) |
               (1u << 19) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26) |
               (n > 1 ? (1u << 28) : 0);
        return;
    case 0xB: {
        unsigned shift = log2_ceil(n);
        r[3] = c->initial_apic_id;
        if (sub == 0) { r[0] = 0; r[1] = 1; r[2] = sub | (1u << 8); }
        else if (sub == 1) { r[0] = shift; r[1] = n; r[2] = sub | (2u << 8); }
        else r[2] = sub;
        return;
    }
    case 0x80000000:
        r[0] = 0x80000008;
        return;
    case 0x80000001:
        r[2] = 1u << 0;                                          /* LAHF/SAHF in 64-bit mode */
        r[3] = (1u << 11) | (1u << 20) | (1u << 26) | (1u << 27) | (1u << 29);
        return;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        const char *b = c->brand + (leaf - 0x80000002) * 16;
        for (unsigned i = 0; i < 4; i++)
            r[i] = str4(b + i * 4);
        return;
    }
    case 0x80000008:
        r[0] = (c->phys_addr_bits ? c->phys_addr_bits : 39) | (48u << 8);
        return;
    }
}

/* ---- the MSR store ------------------------------------------------------- */

static x86_msr_ent *msr_find(x86_state *s, uint32_t msr)
{
    for (size_t i = 0; i < s->n_msrs; i++)
        if (s->msrs[i].msr == msr)
            return &s->msrs[i];
    return NULL;
}

static bool msr_store(x86_state *s, uint32_t msr, uint64_t val)
{
    x86_msr_ent *e = msr_find(s, msr);
    if (!e) {
        if (s->n_msrs == s->cap_msrs) {
            size_t cap = s->cap_msrs ? s->cap_msrs * 2 : 32;
            x86_msr_ent *m = realloc(s->msrs, cap * sizeof(*m));
            if (!m)
                return false;
            s->msrs = m;
            s->cap_msrs = cap;
        }
        e = &s->msrs[s->n_msrs++];
        e->msr = msr;
    }
    e->val = val;
    return true;
}

/* The MTRRs are plain storage: memory types do not change emulation. */
static bool is_mtrr(uint32_t msr)
{
    return (msr >= 0x200 && msr <= 0x21F) || msr == 0x250 || msr == 0x258 || msr == 0x259 ||
           (msr >= 0x268 && msr <= 0x26F);
}

uam_status x86_rdmsr(uam_cpu *cpu, uint32_t msr, uint64_t *val)
{
    x86_state *s = x86_st(cpu);
    switch (msr) {
    case MSR_TSC:             *val = cpu->icount + s->tsc_offset; return UAM_OK;
    case MSR_PLATFORM_ID:     *val = 0; return UAM_OK;
    case MSR_APIC_BASE:       *val = s->apic_base; return UAM_OK;
    case MSR_FEATURE_CONTROL: *val = s->feature_control; return UAM_OK;
    case MSR_TSC_ADJUST:      *val = s->tsc_offset; return UAM_OK;
    case MSR_BIOS_SIGN_ID:    *val = 0; return UAM_OK;
    case MSR_SMBASE:
        if (!s->in_smm) break;
        *val = s->smbase; return UAM_OK;
    case MSR_MTRRCAP:         *val = 0x508; return UAM_OK;   /* 8 variable, fixed, WC */
    case MSR_SYSENTER_CS:     *val = s->sysenter_cs; return UAM_OK;
    case MSR_SYSENTER_ESP:    *val = s->sysenter_esp; return UAM_OK;
    case MSR_SYSENTER_EIP:    *val = s->sysenter_eip; return UAM_OK;
    case MSR_MCG_CAP: case MSR_MCG_STATUS: *val = 0; return UAM_OK;
    case MSR_MISC_ENABLE:     *val = s->misc_enable; return UAM_OK;
    case MSR_PAT:             *val = s->pat; return UAM_OK;
    case MSR_MTRR_DEF_TYPE:   *val = s->mtrr_def_type; return UAM_OK;
    case MSR_EFER:            *val = s->efer; return UAM_OK;
    case MSR_STAR:            *val = s->star; return UAM_OK;
    case MSR_LSTAR:           *val = s->lstar; return UAM_OK;
    case MSR_CSTAR:           *val = s->cstar; return UAM_OK;
    case MSR_FMASK:           *val = s->fmask; return UAM_OK;
    case MSR_FS_BASE:         *val = x86_seg_base(cpu, X86_SEG_FS); return UAM_OK;
    case MSR_GS_BASE:         *val = x86_seg_base(cpu, X86_SEG_GS); return UAM_OK;
    case MSR_KERNEL_GS_BASE:  *val = s->kernel_gs_base; return UAM_OK;
    case MSR_TSC_AUX:         *val = s->tsc_aux; return UAM_OK;
    }
    if (s->plat.rdmsr && s->plat.rdmsr(s->plat.ctx, msr, val))
        return UAM_OK;
    x86_msr_ent *e = msr_find(s, msr);
    if (e || is_mtrr(msr) || s->msr_policy == UAM_X86_MSR_STORE) {
        *val = e ? e->val : 0;
        if (s->msr_trace && !is_mtrr(msr))
            fprintf(stderr, "x86: rdmsr %#x -> %#llx\n", msr, (unsigned long long)*val);
        return UAM_OK;
    }
    return x86_raise(cpu, X86_EXC_GP, true, 0);
}

static uam_status write_efer(uam_cpu *cpu, uint64_t v)
{
    x86_state *s = x86_st(cpu);
    if (v & ~(uint64_t)(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    if (((v ^ s->efer) & EFER_LME) && (s->cr0 & CR0_PG))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    v = (v & ~(uint64_t)EFER_LMA) | (s->efer & EFER_LMA);
    if ((v ^ s->efer) & EFER_NXE) {
        x86_tlb_flush(cpu);
        uam_cpu_xlate_changed(cpu);
    }
    s->efer = v;
    return UAM_OK;
}

uam_status x86_wrmsr(uam_cpu *cpu, uint32_t msr, uint64_t val)
{
    x86_state *s = x86_st(cpu);
    switch (msr) {
    case MSR_TSC:             s->tsc_offset = val - cpu->icount; return UAM_OK;
    case MSR_APIC_BASE:
        s->apic_base = val;
        if (s->plat.wrmsr)
            (void)s->plat.wrmsr(s->plat.ctx, msr, val);   /* the APIC moves */
        return UAM_OK;
    case MSR_FEATURE_CONTROL:
        if (s->feature_control & 1)
            return x86_raise(cpu, X86_EXC_GP, true, 0);   /* locked */
        s->feature_control = val; return UAM_OK;
    case MSR_TSC_ADJUST:      s->tsc_offset = val; return UAM_OK;
    case MSR_BIOS_SIGN_ID: case MSR_PLATFORM_ID: return UAM_OK;
    case MSR_SMBASE:
        if (!s->in_smm) break;
        s->smbase = val & 0xFFFFFFFFu; return UAM_OK;
    case MSR_MTRRCAP:         return x86_raise(cpu, X86_EXC_GP, true, 0);
    case MSR_SYSENTER_CS:     s->sysenter_cs = val; return UAM_OK;
    case MSR_SYSENTER_ESP:    s->sysenter_esp = val; return UAM_OK;
    case MSR_SYSENTER_EIP:    s->sysenter_eip = val; return UAM_OK;
    case MSR_MCG_STATUS:      return UAM_OK;
    case MSR_MISC_ENABLE:     s->misc_enable = val; return UAM_OK;
    case MSR_PAT:             s->pat = val; return UAM_OK;
    case MSR_MTRR_DEF_TYPE:   s->mtrr_def_type = val; return UAM_OK;
    case MSR_EFER:            return write_efer(cpu, val);
    case MSR_STAR:            s->star = val; return UAM_OK;
    case MSR_LSTAR:           s->lstar = val; return UAM_OK;
    case MSR_CSTAR:           s->cstar = val; return UAM_OK;
    case MSR_FMASK:           s->fmask = val; return UAM_OK;
    case MSR_FS_BASE:         uam_cpu_reg_write(cpu, X86_OFF_SEG_BASE(X86_SEG_FS), 64, val); return UAM_OK;
    case MSR_GS_BASE:         uam_cpu_reg_write(cpu, X86_OFF_SEG_BASE(X86_SEG_GS), 64, val); return UAM_OK;
    case MSR_KERNEL_GS_BASE:  s->kernel_gs_base = val; return UAM_OK;
    case MSR_TSC_AUX:         s->tsc_aux = val & 0xFFFFFFFFu; return UAM_OK;
    }
    if (s->plat.wrmsr && s->plat.wrmsr(s->plat.ctx, msr, val))
        return UAM_OK;
    if (is_mtrr(msr) || msr_find(s, msr) || s->msr_policy == UAM_X86_MSR_STORE) {
        if (s->msr_trace && !is_mtrr(msr))
            fprintf(stderr, "x86: wrmsr %#x <- %#llx\n", msr, (unsigned long long)val);
        return msr_store(s, msr, val) ? UAM_OK : x86_raise(cpu, X86_EXC_GP, true, 0);
    }
    return x86_raise(cpu, X86_EXC_GP, true, 0);
}
