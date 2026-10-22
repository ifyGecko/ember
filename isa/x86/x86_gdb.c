/*
 * x86_gdb.c - Debugger view of the x86-64 registers (gdb remote protocol).
 *
 * gdb's amd64 core register set, which gdb accepts only complete: the general
 * registers in its order, rip, eflags, the segment selectors, the x87 stack
 * (st<i> = ST(i)) and its control registers. rip is reported as the linear pc
 * (CS.base + RIP), which is what breakpoints are set on; the two are equal in
 * 64-bit code.
 */
#include "x86.h"

#define GDB_X86_NREGS 40u   /* gdb's amd64 core feature, which it requires whole */

/* 0-15 GPRs, 16 rip, 17 eflags, 18-23 cs ss ds es fs gs, 24-31 st0-st7,
 * 32-39 fctrl fstat ftag fiseg fioff foseg fooff fop */
#define R(n, bits, num, ty) "<reg name=\"" n "\" bitsize=\"" #bits "\" regnum=\"" #num "\"" ty "/>"
static const char x86_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>i386:x86-64</architecture>"
    "<feature name=\"org.gnu.gdb.i386.core\">"
    R("rax", 64, 0, " type=\"int64\"") R("rbx", 64, 1, " type=\"int64\"")
    R("rcx", 64, 2, " type=\"int64\"") R("rdx", 64, 3, " type=\"int64\"")
    R("rsi", 64, 4, " type=\"int64\"") R("rdi", 64, 5, " type=\"int64\"")
    R("rbp", 64, 6, " type=\"data_ptr\"") R("rsp", 64, 7, " type=\"data_ptr\"")
    R("r8", 64, 8, " type=\"int64\"") R("r9", 64, 9, " type=\"int64\"")
    R("r10", 64, 10, " type=\"int64\"") R("r11", 64, 11, " type=\"int64\"")
    R("r12", 64, 12, " type=\"int64\"") R("r13", 64, 13, " type=\"int64\"")
    R("r14", 64, 14, " type=\"int64\"") R("r15", 64, 15, " type=\"int64\"")
    R("rip", 64, 16, " type=\"code_ptr\"") R("eflags", 32, 17, " type=\"int32\"")
    R("cs", 32, 18, " type=\"int32\"") R("ss", 32, 19, " type=\"int32\"")
    R("ds", 32, 20, " type=\"int32\"") R("es", 32, 21, " type=\"int32\"")
    R("fs", 32, 22, " type=\"int32\"") R("gs", 32, 23, " type=\"int32\"")
    R("st0", 80, 24, " type=\"i387_ext\"") R("st1", 80, 25, " type=\"i387_ext\"")
    R("st2", 80, 26, " type=\"i387_ext\"") R("st3", 80, 27, " type=\"i387_ext\"")
    R("st4", 80, 28, " type=\"i387_ext\"") R("st5", 80, 29, " type=\"i387_ext\"")
    R("st6", 80, 30, " type=\"i387_ext\"") R("st7", 80, 31, " type=\"i387_ext\"")
    R("fctrl", 32, 32, " type=\"int\" group=\"float\"") R("fstat", 32, 33, " type=\"int\" group=\"float\"")
    R("ftag", 32, 34, " type=\"int\" group=\"float\"") R("fiseg", 32, 35, " type=\"int\" group=\"float\"")
    R("fioff", 32, 36, " type=\"int\" group=\"float\"") R("foseg", 32, 37, " type=\"int\" group=\"float\"")
    R("fooff", 32, 38, " type=\"int\" group=\"float\"") R("fop", 32, 39, " type=\"int\" group=\"float\"")
    "</feature></target>";
#undef R

static const uint8_t x86_reg_sizes[GDB_X86_NREGS] = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,   /* GPRs */
    8, 4,                                             /* rip, eflags */
    4, 4, 4, 4, 4, 4,                                 /* selectors */
    10, 10, 10, 10, 10, 10, 10, 10,                   /* st0-st7 */
    4, 4, 4, 4, 4, 4, 4, 4,                           /* x87 control */
};

/* gdb order -> architectural register number */
static const uint8_t gdb_gpr[16] = { X86_RAX, X86_RBX, X86_RCX, X86_RDX, X86_RSI, X86_RDI,
                                     X86_RBP, X86_RSP, 8, 9, 10, 11, 12, 13, 14, 15 };
static const uint8_t gdb_seg[6] = { X86_SEG_CS, X86_SEG_SS, X86_SEG_DS, X86_SEG_ES,
                                    X86_SEG_FS, X86_SEG_GS };

/* ST(i), the stack-relative register gdb calls st<i>. */
static long double *st_reg(uam_cpu *cpu, unsigned i)
{
    unsigned top = ((unsigned)uam_cpu_reg_read(cpu, X86_OFF_FSW, 16) >> 11) & 7;
    return &x86_st(cpu)->fpr[(top + i) & 7];
}

static bool x86_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    x86_state *s = x86_st(cpu);
    if (n < 16)       *v = x86_gpr(cpu, gdb_gpr[n]);
    else if (n == 16) *v = cpu->pc;
    else if (n == 17) *v = x86_get_rflags(cpu);
    else if (n < 24)  *v = x86_seg_sel(cpu, gdb_seg[n - 18]);
    else if (n == 32) *v = uam_cpu_reg_read(cpu, X86_OFF_FCW, 16);
    else if (n == 33) *v = uam_cpu_reg_read(cpu, X86_OFF_FSW, 16);
    else if (n == 34) *v = uam_cpu_reg_read(cpu, X86_OFF_FTW, 16);
    else if (n == 35) *v = 0;                       /* fiseg: not kept */
    else if (n == 36) *v = (uint32_t)s->fpu_ip;
    else if (n == 37) *v = 0;                       /* foseg */
    else if (n == 38) *v = (uint32_t)s->fpu_dp;
    else if (n == 39) *v = s->fpu_op;
    else return false;
    return true;
}

static bool x86_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n < 16)       x86_set_gpr(cpu, gdb_gpr[n], v);
    else if (n == 16) cpu->pc = v;
    else if (n == 17) x86_set_rflags(cpu, (uint32_t)v);
    else if (n == 32) uam_cpu_reg_write(cpu, X86_OFF_FCW, 16, v);
    else if (n == 33) uam_cpu_reg_write(cpu, X86_OFF_FSW, 16, v);
    else if (n == 34) uam_cpu_reg_write(cpu, X86_OFF_FTW, 16, v);
    else return false;   /* selectors (they need a descriptor load) and the pointers */
    return true;
}

static bool x86_gdb_read_wide(uam_cpu *cpu, unsigned n, uint8_t *buf)
{
    if (n < 24 || n >= 32)
        return false;
    x86_f80_store(buf, *st_reg(cpu, n - 24));
    return true;
}

static bool x86_gdb_write_wide(uam_cpu *cpu, unsigned n, const uint8_t *buf)
{
    if (n < 24 || n >= 32)
        return false;
    *st_reg(cpu, n - 24) = x86_f80_load(buf);
    return true;
}

const uam_gdb_arch x86_gdb = {
    .target_xml = x86_target_xml,
    .n_regs = GDB_X86_NREGS,
    .reg_bytes = 8,
    .read_reg = x86_gdb_read,
    .write_reg = x86_gdb_write,
    .reg_sizes = x86_reg_sizes,
    .read_wide = x86_gdb_read_wide,
    .write_wide = x86_gdb_write_wide,
};
