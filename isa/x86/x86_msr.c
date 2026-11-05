/*
 * x86_msr.c - CPUID and model-specific registers.
 *
 * CPUID describes exactly what this module implements: x86-64 with SSE2,
 * CMPXCHG16B, POPCNT, RDRAND, NX, 1 GiB pages, RDTSCP, PAT and MTRRs (with
 * the SMM range registers). SMRAM protection MSRs: AMD's HWCR, SMM_ADDR and
 * SMM_MASK with the AMD64 map; Intel's MSR_SMM_MCA_CAP and
 * MSR_SMM_FEATURE_CONTROL with UAM_X86_FEAT_SMM_CODE_CHK.
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
    memcpy(info->vendor, "UAMx86RefCPU", 13);   /* no vendor: boards set a real one */
    info->signature = 0x00000600u;              /* family 6, model 0, stepping 0 */
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

static void cpuid_module(uam_cpu *cpu, uint32_t leaf, uint32_t sub, uint32_t r[4])
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
        r[2] = (1u << 13) | (1u << 23) | (1u << 30) |         /* CX16 POPCNT RDRAND */
               ((c->features & UAM_X86_FEAT_X2APIC) ? (1u << 21) : 0) |
               ((c->features & UAM_X86_FEAT_TSC_DEADLINE) ? (1u << 24) : 0);
        /* FPU DE PSE TSC MSR PAE MCE CX8 APIC MTRR PGE CMOV PAT PSE-36 CLFSH MMX FXSR SSE SSE2 [HTT] */
        r[3] = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
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

/* The module's answer, then the platform's changes (uam_x86_platform.cpuid). */
void x86_cpuid(uam_cpu *cpu, uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    const x86_state *s = x86_st(cpu);
    cpuid_module(cpu, leaf, sub, r);
    if (s->plat.cpuid)
        s->plat.cpuid(s->plat.ctx, leaf, sub, r);
}

/* The CR4 bits this processor has: those of the features CPUID reports (the
 * board's hook included), as hardware reserves the bits of features it lacks.
 * MOV to CR4 raises #GP(0) on any other bit; RSM shuts down on one. */
uint64_t x86_cr4_valid(uam_cpu *cpu)
{
    uint32_t r0[4], r1[4], r7[4] = { 0 }, r71[4] = { 0 };
    x86_cpuid(cpu, 0, 0, r0);
    x86_cpuid(cpu, 1, 0, r1);
    if (r0[0] >= 7) {
        x86_cpuid(cpu, 7, 0, r7);
        if (r7[0] >= 1)
            x86_cpuid(cpu, 7, 1, r71);
    }
    const uint32_t ecx1 = r1[2], edx1 = r1[3], ebx7 = r7[1], ecx7 = r7[2], edx7 = r7[3], eax71 = r71[0];
    static const struct { uint8_t cr4, reg, bit; } map[] = {
        /* reg: 0 leaf 1 EDX, 1 leaf 1 ECX, 2 leaf 7 EBX, 3 leaf 7 ECX, 4 leaf 7 EDX, 5 leaf 7.1 EAX */
        { 0, 0, 1 }, { 1, 0, 1 },   /* VME, PVI: VME */
        { 2, 0, 4 },                /* TSD: TSC */
        { 3, 0, 2 },                /* DE */
        { 4, 0, 3 },                /* PSE */
        { 5, 0, 6 },                /* PAE */
        { 6, 0, 7 },                /* MCE */
        { 7, 0, 13 },               /* PGE */
        { 9, 0, 24 },               /* OSFXSR: FXSR */
        { 10, 0, 25 },              /* OSXMMEXCPT: SSE */
        { 11, 3, 2 },               /* UMIP */
        { 12, 3, 16 },              /* LA57 */
        { 13, 1, 5 },               /* VMXE: VMX */
        { 14, 1, 6 },               /* SMXE: SMX */
        { 16, 2, 0 },               /* FSGSBASE */
        { 17, 1, 17 },              /* PCIDE: PCID */
        { 18, 1, 26 },              /* OSXSAVE: XSAVE */
        { 19, 3, 23 },              /* KL */
        { 20, 2, 7 },               /* SMEP */
        { 21, 2, 20 },              /* SMAP */
        { 22, 3, 3 },               /* PKE: PKU */
        { 23, 3, 7 }, { 23, 4, 20 },/* CET: CET_SS or CET_IBT */
        { 24, 3, 31 },              /* PKS */
        { 25, 4, 5 },               /* UINTR */
        { 28, 5, 26 },              /* LAM_SUP: LAM */
        { 32, 5, 17 },              /* FRED */
    };
    const uint32_t regs[] = { edx1, ecx1, ebx7, ecx7, edx7, eax71 };
    uint64_t v = CR4_PCE;           /* RDPMC at CPL > 0: every processor has it */
    for (size_t i = 0; i < UAM_ARRAY_LEN(map); i++)
        if (regs[map[i].reg] & (1u << map[i].bit))
            v |= 1ull << map[i].cr4;
    return v;
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

/* x2APIC registers exist only in x2APIC mode; IA32_TSC_DEADLINE only with
 * the feature. Both are then the platform's (its local APIC model). */
static bool apic_msr_absent(const x86_state *s, uint32_t msr)
{
    if (msr >= 0x800 && msr <= 0x8FF)
        return (s->apic_base & 0xC00) != 0xC00;
    if (msr == 0x6E0)
        return !(s->cpuid.features & UAM_X86_FEAT_TSC_DEADLINE);
    return false;
}

uint64_t uam_x86_read_tsc(void *cpu)
{
    uam_cpu *c = cpu;
    return x86_tsc_base(c) + x86_st(c)->tsc_offset;
}

uam_status x86_rdmsr(uam_cpu *cpu, uint32_t msr, uint64_t *val)
{
    x86_state *s = x86_st(cpu);
    if (apic_msr_absent(s, msr))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    switch (msr) {
    case MSR_TSC:             *val = x86_tsc_base(cpu) + s->tsc_offset; return UAM_OK;
    case MSR_PLATFORM_ID:     *val = 0; return UAM_OK;
    case MSR_APIC_BASE:       *val = s->apic_base; return UAM_OK;
    case MSR_FEATURE_CONTROL: *val = s->feature_control; return UAM_OK;
    case MSR_TSC_ADJUST:      *val = s->tsc_offset; return UAM_OK;
    case MSR_BIOS_SIGN_ID:    *val = 0; return UAM_OK;
    case MSR_SMBASE:          /* Intel: read-only, only in SMM */
        if (s->smm_map == UAM_X86_SMM_MAP_AMD64) break;
        if (!s->in_smm)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        *val = s->smbase; return UAM_OK;
    case MSR_AMD_SMM_BASE:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        *val = s->smbase; return UAM_OK;
    case MSR_MTRRCAP:         /* the board may describe its own; else 8 variable, fixed, WC (+ SMRR on Intel) */
        if (s->plat.rdmsr && s->plat.rdmsr(s->plat.ctx, msr, val))
            return UAM_OK;
        *val = s->smm_map == UAM_X86_SMM_MAP_AMD64 ? 0x508 : 0xD08;
        return UAM_OK;
    case MSR_SMRR_PHYSBASE: case MSR_SMRR_PHYSMASK:   /* Intel; accessible only in SMM */
        if (s->smm_map == UAM_X86_SMM_MAP_AMD64) break;
        if (!s->in_smm)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        *val = msr == MSR_SMRR_PHYSBASE ? s->smrr_base : s->smrr_mask;
        return UAM_OK;
    case MSR_SMM_MCA_CAP:   /* the enhanced-SMM MSRs: each with its feature, SMM only */
        if (!(s->cpuid.features & (UAM_X86_FEAT_SMM_CODE_CHK | UAM_X86_FEAT_SMM_BLOCKED))) break;
        if (!s->in_smm)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        *val = ((s->cpuid.features & UAM_X86_FEAT_SMM_CODE_CHK) ? SMM_MCA_CODE_CHK : 0) |
               ((s->cpuid.features & UAM_X86_FEAT_SMM_BLOCKED) ? SMM_MCA_LONG_FLOW : 0);
        return UAM_OK;
    case MSR_SMM_FEATURE_CONTROL:
        if (!(s->cpuid.features & UAM_X86_FEAT_SMM_CODE_CHK)) break;
        if (!s->in_smm)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        *val = s->smm_fc;
        return UAM_OK;
    case MSR_SMM_DELAYED: case MSR_SMM_BLOCKED:
        if (!(s->cpuid.features & UAM_X86_FEAT_SMM_BLOCKED)) break;
        if (!s->in_smm)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        /* no processor here is ever in a long flow that delays an SMI */
        *val = msr == MSR_SMM_BLOCKED ? x86_smm_blocked(cpu) : 0;
        return UAM_OK;
    case MSR_AMD_HWCR:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        *val = s->hwcr; return UAM_OK;
    case MSR_AMD_SMM_ADDR:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        *val = s->smm_addr; return UAM_OK;
    case MSR_AMD_SMM_MASK:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        *val = s->smm_mask; return UAM_OK;
    case MSR_SYSENTER_CS:     *val = s->sysenter_cs; return UAM_OK;
    case MSR_SYSENTER_ESP:    *val = s->sysenter_esp; return UAM_OK;
    case MSR_SYSENTER_EIP:    *val = s->sysenter_eip; return UAM_OK;
    case MSR_MCG_CAP:         *val = 0; return UAM_OK;   /* no banks */
    case MSR_MCG_STATUS:      *val = s->mcg_status; return UAM_OK;
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
    if (apic_msr_absent(s, msr))
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    switch (msr) {
    case MSR_TSC:             s->tsc_offset = val - x86_tsc_base(cpu); return UAM_OK;
    case MSR_APIC_BASE: {
        /* BSP flag (8) is read-only; EXTD (10) needs x2APIC support and EN;
         * leaving x2APIC mode only to disabled (SDM 10.12.5) */
        uint64_t keep = s->apic_base & (1u << 8);
        unsigned pab = s->cpuid.phys_addr_bits ? s->cpuid.phys_addr_bits : 36;
        uint64_t rw = (((1ull << pab) - 1) & ~0xFFFull) | (1u << 11) |
                      ((s->cpuid.features & UAM_X86_FEAT_X2APIC) ? (1u << 10) : 0);
        bool extd = val & (1u << 10), en = val & (1u << 11);
        bool was_x2 = (s->apic_base & 0xC00) == 0xC00;
        if ((val & ~(rw | (1u << 8))) || (extd && !en) || (was_x2 && en && !extd))
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        s->apic_base = keep | (val & rw);
        if (s->plat.wrmsr)
            (void)s->plat.wrmsr(s->plat.ctx, msr, s->apic_base);   /* the APIC moves */
        return UAM_OK;
    }
    case MSR_FEATURE_CONTROL:
        if (s->feature_control & 1)
            return x86_raise(cpu, X86_EXC_GP, true, 0);   /* locked */
        s->feature_control = val; return UAM_OK;
    case MSR_TSC_ADJUST:      s->tsc_offset = val; return UAM_OK;
    case MSR_BIOS_SIGN_ID: case MSR_PLATFORM_ID: return UAM_OK;
    case MSR_SMBASE:          /* Intel: read-only (RSM takes SMBASE from the save state) */
        if (s->smm_map == UAM_X86_SMM_MAP_AMD64) break;
        return x86_raise(cpu, X86_EXC_GP, true, 0);
    case MSR_AMD_SMM_BASE:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        if (!(s->hwcr & HWCR_SMMLOCK))   /* HWCR.SMMLOCK: read-only, writes ignored */
            s->smbase = val & 0xFFFFFFFFu;
        return UAM_OK;
    case MSR_AMD_HWCR:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        s->hwcr = val | (s->hwcr & HWCR_SMMLOCK);   /* SMMLOCK: set once, until RESET */
        return UAM_OK;
    case MSR_AMD_SMM_ADDR: case MSR_AMD_SMM_MASK:
        if (s->smm_map != UAM_X86_SMM_MAP_AMD64) break;
        if (s->hwcr & HWCR_SMMLOCK)
            return UAM_OK;
        if (msr == MSR_AMD_SMM_ADDR) s->smm_addr = val & SMM_TSEG_FIELD;
        else                         s->smm_mask = val & SMMMASK_DEFINED;
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    case MSR_SMM_MCA_CAP:
        if (!(s->cpuid.features & (UAM_X86_FEAT_SMM_CODE_CHK | UAM_X86_FEAT_SMM_BLOCKED))) break;
        return x86_raise(cpu, X86_EXC_GP, true, 0);   /* read-only */
    case MSR_SMM_DELAYED: case MSR_SMM_BLOCKED:
        if (!(s->cpuid.features & UAM_X86_FEAT_SMM_BLOCKED)) break;
        return x86_raise(cpu, X86_EXC_GP, true, 0);   /* read-only */
    case MSR_SMM_FEATURE_CONTROL:
        if (!(s->cpuid.features & UAM_X86_FEAT_SMM_CODE_CHK)) break;
        if (!s->in_smm || (s->smm_fc & SMM_FC_LOCK) || (val & ~(uint64_t)(SMM_FC_LOCK | SMM_FC_CODE_CHK_EN)))
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        s->smm_fc = val;
        x86_update_priv(cpu);
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    case MSR_MTRRCAP:         return x86_raise(cpu, X86_EXC_GP, true, 0);
    case MSR_SMRR_PHYSBASE: case MSR_SMRR_PHYSMASK:   /* Intel; accessible only in SMM */
        if (s->smm_map == UAM_X86_SMM_MAP_AMD64) break;
        if (!s->in_smm)
            return x86_raise(cpu, X86_EXC_GP, true, 0);
        if (msr == MSR_SMRR_PHYSBASE) s->smrr_base = val & 0x000FFFFFFFFFF0FFull;
        else                          s->smrr_mask = val & 0x000FFFFFFFFFF800ull;
        uam_cpu_xlate_changed(cpu);
        return UAM_OK;
    case MSR_SYSENTER_CS:     s->sysenter_cs = val; return UAM_OK;
    case MSR_SYSENTER_ESP:    s->sysenter_esp = val; return UAM_OK;
    case MSR_SYSENTER_EIP:    s->sysenter_eip = val; return UAM_OK;
    case MSR_MCG_STATUS:      /* the handler clears MCIP */
        s->mcg_status = val & (MCG_RIPV | MCG_EIPV | MCG_MCIP);
        return UAM_OK;
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
