/*
 * x86_helpers.c - The x86 helper table and the system helpers behind it:
 * segment loads, far transfers, CPUID, MSRs, 128-bit division, FXSAVE,
 * SYSCALL/SYSRET, SWAPGS and RSM. SSE helpers are in x86_sse_ops.c, the x87 in x86_x87.c.
 *
 * A helper that faults records the exception with x86_raise() and reports
 * failure through *st; take_exception then delivers it precisely.
 */
#include "x86.h"

uint64_t x86_h_sse(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);

#define CPU uam_cpu *cpu = env
#define CHECK(expr) do { uam_status s_ = (expr); if (s_ != UAM_OK) { *st = s_; return 0; } } while (0)

static uint64_t h_load_seg(void *env, uint64_t seg, uint64_t sel, uint64_t c, uam_status *st)
{
    CPU; (void)c;
    CHECK(x86_load_seg(cpu, (unsigned)seg, (uint16_t)sel));
    return 0;
}

static uint64_t h_far_jmp(void *env, uint64_t sel, uint64_t off, uint64_t next, uam_status *st)
{
    CPU;
    uint64_t pc = 0;
    CHECK(x86_far_jmp(cpu, (uint16_t)sel, off, next, &pc));
    return pc;
}

static uint64_t h_far_call(void *env, uint64_t a, uint64_t off, uint64_t ret, uam_status *st)
{
    CPU;
    uint64_t pc = 0;
    CHECK(x86_far_call(cpu, (uint16_t)a, (unsigned)(a >> 16) & 0x7F, off, ret, &pc));
    return pc;
}

static uint64_t h_retf(void *env, uint64_t osz, uint64_t imm, uint64_t c, uam_status *st)
{
    CPU; (void)c;
    uint64_t pc = 0;
    CHECK(x86_retf(cpu, (unsigned)osz, (unsigned)imm, &pc));
    return pc;
}

static uint64_t h_iret(void *env, uint64_t osz, uint64_t next, uint64_t c, uam_status *st)
{
    CPU; (void)c;
    uint64_t pc = 0;
    CHECK(x86_iret(cpu, (unsigned)osz, next, &pc));
    return pc;
}

static uint64_t h_cpuid(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c; (void)st;
    uint32_t r[4];
    x86_cpuid(cpu, (uint32_t)x86_gpr(cpu, X86_RAX), (uint32_t)x86_gpr(cpu, X86_RCX), r);
    x86_set_gpr(cpu, X86_RAX, r[0]);
    x86_set_gpr(cpu, X86_RBX, r[1]);
    x86_set_gpr(cpu, X86_RCX, r[2]);
    x86_set_gpr(cpu, X86_RDX, r[3]);
    return 0;
}

static uint64_t h_rdmsr(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c;
    uint64_t v = 0;
    CHECK(x86_rdmsr(cpu, (uint32_t)x86_gpr(cpu, X86_RCX), &v));
    x86_set_gpr(cpu, X86_RAX, v & 0xFFFFFFFFu);
    x86_set_gpr(cpu, X86_RDX, v >> 32);
    return 0;
}

static uint64_t h_wrmsr(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c;
    uint64_t v = (x86_gpr(cpu, X86_RDX) << 32) | (x86_gpr(cpu, X86_RAX) & 0xFFFFFFFFu);
    CHECK(x86_wrmsr(cpu, (uint32_t)x86_gpr(cpu, X86_RCX), v));
    return 0;
}

static uint64_t h_lldt(void *env, uint64_t sel, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    CHECK(x86_lldt(cpu, (uint16_t)sel));
    return 0;
}

static uint64_t h_ltr(void *env, uint64_t sel, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    CHECK(x86_ltr(cpu, (uint16_t)sel));
    return 0;
}

static uint64_t h_rsm(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c;
    uint64_t pc = 0;
    CHECK(x86_smm_rsm(cpu, &pc));
    return pc;
}

static uint64_t h_divq(void *env, uint64_t d, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    unsigned __int128 n = ((unsigned __int128)x86_gpr(cpu, X86_RDX) << 64) | x86_gpr(cpu, X86_RAX);
    if (d == 0 || x86_gpr(cpu, X86_RDX) >= d) {
        *st = x86_raise(cpu, X86_EXC_DE, false, 0);
        return 0;
    }
    x86_set_gpr(cpu, X86_RAX, (uint64_t)(n / d));
    x86_set_gpr(cpu, X86_RDX, (uint64_t)(n % d));
    return 0;
}

static uint64_t h_idivq(void *env, uint64_t d, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    __int128 n = (__int128)(((unsigned __int128)x86_gpr(cpu, X86_RDX) << 64) | x86_gpr(cpu, X86_RAX));
    int64_t sd = (int64_t)d;
    if (sd == 0) {
        *st = x86_raise(cpu, X86_EXC_DE, false, 0);
        return 0;
    }
    __int128 q, r;
    if (sd == -1) {   /* n / -1 can overflow __int128 itself */
        if (n < -(__int128)INT64_MAX || n > (__int128)INT64_MAX + 1) {
            *st = x86_raise(cpu, X86_EXC_DE, false, 0);
            return 0;
        }
        q = -n;
        r = 0;
    } else {
        q = n / sd;
        r = n % sd;
    }
    if (q > INT64_MAX || q < INT64_MIN) {
        *st = x86_raise(cpu, X86_EXC_DE, false, 0);
        return 0;
    }
    x86_set_gpr(cpu, X86_RAX, (uint64_t)(int64_t)q);
    x86_set_gpr(cpu, X86_RDX, (uint64_t)(int64_t)r);
    return 0;
}

/* FXSAVE / FXRSTOR: the legacy region (x87, MXCSR, XMM0-15). */
static uint64_t h_fxsave(void *env, uint64_t lin, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    if (lin & 15) { *st = x86_raise(cpu, X86_EXC_GP, true, 0); return 0; }
    uint64_t w0 = uam_cpu_reg_read(cpu, X86_OFF_FCW, 16) | uam_cpu_reg_read(cpu, X86_OFF_FSW, 16) << 16 |
                  uam_cpu_reg_read(cpu, X86_OFF_FTW, 8) << 32;
    uint64_t w3 = uam_cpu_reg_read(cpu, X86_OFF_MXCSR, 32) | 0xFFBFull << 32;
    CHECK(x86_lin_write(cpu, lin, 8, w0, false));
    CHECK(x86_lin_write(cpu, lin + 8, 8, 0, false));
    CHECK(x86_lin_write(cpu, lin + 16, 8, 0, false));
    CHECK(x86_lin_write(cpu, lin + 24, 8, w3, false));
    unsigned top = (unsigned)(uam_cpu_reg_read(cpu, X86_OFF_FSW, 16) >> 11) & 7;
    for (unsigned i = 0; i < 8; i++) {   /* ST0-7, 16 bytes apart */
        uint8_t fb[10];
        uint64_t lo = 0;
        x86_f80_store(fb, x86_st(cpu)->fpr[(top + i) & 7]);
        for (unsigned k = 0; k < 8; k++) lo |= (uint64_t)fb[k] << (8 * k);
        uint64_t hi8 = fb[8] | (uint64_t)fb[9] << 8;
        if (x86_st(cpu)->mmx) {   /* MMi in slot i: significand, exponent all ones */
            lo = x86_st(cpu)->mm[i];
            hi8 = 0xFFFF;
        }
        CHECK(x86_lin_write(cpu, lin + 32 + i * 16, 8, lo, false));
        CHECK(x86_lin_write(cpu, lin + 40 + i * 16, 8, hi8, false));
    }
    for (unsigned i = 0; i < 32; i++)   /* XMM0-15 */
        CHECK(x86_lin_write(cpu, lin + 160 + i * 8, 8,
                            uam_cpu_reg_read(cpu, X86_OFF_XMM(i / 2) + (i & 1) * 8, 64), false));
    return 0;
}

static uint64_t h_fxrstor(void *env, uint64_t lin, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    if (lin & 15) { *st = x86_raise(cpu, X86_EXC_GP, true, 0); return 0; }
    uint64_t w0 = 0, w3 = 0, x[32], f[16];
    CHECK(x86_lin_read(cpu, lin, 8, &w0, false));
    CHECK(x86_lin_read(cpu, lin + 24, 8, &w3, false));
    if ((uint32_t)w3 & ~0xFFFFu) { *st = x86_raise(cpu, X86_EXC_GP, true, 0); return 0; }
    for (unsigned i = 0; i < 32; i++)
        CHECK(x86_lin_read(cpu, lin + 160 + i * 8, 8, &x[i], false));
    for (unsigned i = 0; i < 16; i++)
        CHECK(x86_lin_read(cpu, lin + 32 + i * 8, 8, &f[i], false));
    unsigned top = (unsigned)(w0 >> 27) & 7;   /* FSW bits 11-13 */
    for (unsigned i = 0; i < 8; i++) {
        uint8_t fb[10];
        for (unsigned k = 0; k < 8; k++) fb[k] = (uint8_t)(f[2 * i] >> (8 * k));
        fb[8] = (uint8_t)f[2 * i + 1];
        fb[9] = (uint8_t)(f[2 * i + 1] >> 8);
        x86_st(cpu)->fpr[(top + i) & 7] = x86_f80_load(fb);
        x86_st(cpu)->mm[i] = f[2 * i];   /* the significand is MMi */
    }
    uam_cpu_reg_write(cpu, X86_OFF_FCW, 16, w0 & 0xFFFF);
    uam_cpu_reg_write(cpu, X86_OFF_FSW, 16, (w0 >> 16) & 0xFFFF);
    uam_cpu_reg_write(cpu, X86_OFF_FTW, 8, (w0 >> 32) & 0xFF);
    uam_cpu_reg_write(cpu, X86_OFF_MXCSR, 32, w3 & 0xFFFF);
    for (unsigned i = 0; i < 32; i++)
        uam_cpu_reg_write(cpu, X86_OFF_XMM(i / 2) + (i & 1) * 8, 64, x[i]);
    return 0;
}

static uint64_t h_lar(void *env, uint64_t sel, uint64_t lsl, uint64_t c, uam_status *st)
{
    CPU; (void)c; (void)st;
    return x86_lar(cpu, (uint16_t)sel, lsl != 0);
}

/* ---- SYSCALL / SYSRET / SWAPGS (64-bit mode) ----------------------------- */

static const uint16_t FLAT_CODE64 = SEGA_P | SEGA_S | 0xB | SEGA_L | SEGA_G;
static const uint16_t FLAT_DATA = SEGA_P | SEGA_S | 0x3 | SEGA_DB | SEGA_G;

static uint64_t h_syscall(void *env, uint64_t next, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    x86_state *s = x86_st(cpu);
    if (!(s->efer & EFER_SCE)) { *st = x86_raise(cpu, X86_EXC_UD, false, 0); return 0; }
    uint16_t sel = (uint16_t)(s->star >> 32);
    x86_set_gpr(cpu, X86_RCX, next);
    x86_set_gpr(cpu, X86_R11, x86_get_rflags(cpu));
    x86_set_rflags(cpu, x86_get_rflags(cpu) & ~((uint32_t)s->fmask | RFLAGS_RF));
    x86_seg_set(cpu, X86_SEG_CS, sel & ~3u, 0, 0xFFFFFFFFu, FLAT_CODE64);
    x86_seg_set(cpu, X86_SEG_SS, (uint16_t)((sel + 8) & ~3u), 0, 0xFFFFFFFFu, FLAT_DATA);
    x86_update_priv(cpu);
    return s->lstar;
}

static uint64_t h_sysret(void *env, uint64_t osz, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)b; (void)c;
    x86_state *s = x86_st(cpu);
    if (!(s->efer & EFER_SCE)) { *st = x86_raise(cpu, X86_EXC_UD, false, 0); return 0; }
    uint16_t sel = (uint16_t)(s->star >> 48);
    uint64_t rip = x86_gpr(cpu, X86_RCX);
    const uint16_t dpl3 = 3u << 5;
    if (osz == 64) {
        x86_seg_set(cpu, X86_SEG_CS, (uint16_t)((sel + 16) | 3), 0, 0xFFFFFFFFu, FLAT_CODE64 | dpl3);
    } else {
        rip &= 0xFFFFFFFFu;
        x86_seg_set(cpu, X86_SEG_CS, (uint16_t)(sel | 3), 0, 0xFFFFFFFFu,
                    SEGA_P | SEGA_S | 0xB | SEGA_DB | SEGA_G | dpl3);
    }
    x86_seg_set(cpu, X86_SEG_SS, (uint16_t)((sel + 8) | 3), 0, 0xFFFFFFFFu, FLAT_DATA | dpl3);
    x86_set_rflags(cpu, ((uint32_t)x86_gpr(cpu, X86_R11) & 0x3C7FD7u) | RFLAGS_ALWAYS_1);
    x86_update_priv(cpu);
    uam_cpu_request_exit(cpu);
    return rip;
}

static uint64_t h_swapgs(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    CPU; (void)a; (void)b; (void)c; (void)st;
    x86_state *s = x86_st(cpu);
    uint64_t gs = x86_seg_base(cpu, X86_SEG_GS);
    uam_cpu_reg_write(cpu, X86_OFF_SEG_BASE(X86_SEG_GS), 64, s->kernel_gs_base);
    s->kernel_gs_base = gs;
    return 0;
}

/* An I/O instruction is about to access its port: what an SMI it raises
 * reports (SMRAM IOMisc and friends), and the registers a restart needs. */
static uint64_t h_io(void *env, uint64_t misc, uint64_t rip, uint64_t mem, uam_status *st)
{
    uam_cpu *cpu = env;
    x86_state *s = x86_st(cpu);
    /* CPL > IOPL, or virtual-8086 mode: the TSS I/O permission bitmap decides */
    if ((*st = x86_io_permitted(cpu, (unsigned)(misc >> 16) & 0xFFFF, (unsigned)(misc >> 1) & 7))
        != UAM_OK)
        return 0;
    s->io_cur = (x86_io){ .misc = misc, .rip = rip, .mem = mem, .rcx = x86_gpr(cpu, X86_RCX),
                          .rsi = x86_gpr(cpu, X86_RSI), .rdi = x86_gpr(cpu, X86_RDI) };
    s->io_icount = cpu->icount;
    return 0;
}

/* IN/OUT: record the SMM I/O state and access the port, in one call. The
 * IOMisc word gives the port, length and direction (odd types are IN). */
static uint64_t h_io_port(void *env, uint64_t misc, uint64_t rip, uint64_t val, uam_status *st)
{
    uam_cpu *cpu = env;
    h_io(env, misc, rip, 0, st);
    if (*st != UAM_OK)
        return 0;
    uint64_t addr = UAM_X86_PIO_BASE + (misc >> 16 & 0xFFFF);
    unsigned size = (unsigned)(misc >> 1) & 7;
    uam_status r;
    if ((misc >> 4) & 1) {
        uint64_t v = 0;
        r = uam_bus_read(cpu->bus, addr, size, UAM_ACC_LE | UAM_ACC_PHYS, &v);
        val = v;
    } else {
        r = uam_bus_write(cpu->bus, addr, size, UAM_ACC_LE | UAM_ACC_PHYS, val);
        val = 0;
    }
    if (r != UAM_OK)
        *st = x86_raise(cpu, X86_EXC_GP, true, 0);
    return val;
}

/* ---- the table ----------------------------------------------------------- */

#define RW (UAM_HELPER_READS_STATE | UAM_HELPER_WRITES_STATE)
#define RWF (RW | UAM_HELPER_MAY_FAULT)
#define H(id, nm, n, res, eff, f) [id] = { nm, n, { 64, 64, 64 }, res, eff, f }

const uam_helper_desc x86_helpers[X86_H__COUNT] = {
    H(X86_H_LOAD_SEG, "x86_load_seg", 2, 0,  RWF, h_load_seg),
    H(X86_H_FAR_JMP,  "x86_far_jmp",  3, 64, RWF, h_far_jmp),
    H(X86_H_FAR_CALL, "x86_far_call", 3, 64, RWF, h_far_call),
    H(X86_H_RETF,     "x86_retf",     2, 64, RWF, h_retf),
    H(X86_H_IRET,     "x86_iret",     2, 64, RWF, h_iret),
    H(X86_H_CPUID,    "x86_cpuid",    0, 0,  RW,  h_cpuid),
    H(X86_H_RDMSR,    "x86_rdmsr",    0, 0,  RWF, h_rdmsr),
    H(X86_H_WRMSR,    "x86_wrmsr",    0, 0,  RWF, h_wrmsr),
    H(X86_H_LLDT,     "x86_lldt",     1, 0,  RWF, h_lldt),
    H(X86_H_LTR,      "x86_ltr",      1, 0,  RWF, h_ltr),
    H(X86_H_RSM,      "x86_rsm",      0, 64, RWF, h_rsm),
    H(X86_H_DIVQ,     "x86_divq",     1, 0,  RWF, h_divq),
    H(X86_H_IDIVQ,    "x86_idivq",    1, 0,  RWF, h_idivq),
    H(X86_H_FXSAVE,   "x86_fxsave",   1, 0,  RWF, h_fxsave),
    H(X86_H_FXRSTOR,  "x86_fxrstor",  1, 0,  RWF, h_fxrstor),
    H(X86_H_LAR,      "x86_lar",      2, 64, RWF, h_lar),
    H(X86_H_SYSCALL,  "x86_syscall",  1, 64, RWF, h_syscall),
    H(X86_H_SYSRET,   "x86_sysret",   1, 64, RWF, h_sysret),
    H(X86_H_SWAPGS,   "x86_swapgs",   0, 0,  RW,  h_swapgs),
    H(X86_H_SSE,      "x86_sse",      2, 0,  RWF, x86_h_sse),
    H(X86_H_X87,      "x86_x87",      2, 0,  RWF, x86_h_x87),
    H(X86_H_IO,       "x86_io",       3, 0,  RWF, h_io),
    H(X86_H_IO_PORT,  "x86_io_port",  3, 64, RWF, h_io_port),
};
