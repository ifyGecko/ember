/*
 * x86_gdb.c - Debugger view of the x86-64 registers (gdb remote protocol).
 *
 * The general registers in gdb's amd64 order, then rip and eflags, all
 * 64 bits wide. rip is reported as the linear pc (CS.base + RIP), which is
 * what breakpoints are set on; the two are equal in 64-bit code.
 */
#include "x86.h"

#define GDB_X86_NREGS 18u   /* 16 GPRs, rip, eflags */

#define R(n, num, ty) "<reg name=\"" n "\" bitsize=\"64\" regnum=\"" #num "\"" ty "/>"
static const char x86_target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>i386:x86-64</architecture>"
    "<feature name=\"org.gnu.gdb.i386.core\">"
    R("rax", 0, "") R("rbx", 1, "") R("rcx", 2, "") R("rdx", 3, "")
    R("rsi", 4, "") R("rdi", 5, "") R("rbp", 6, " type=\"data_ptr\"") R("rsp", 7, " type=\"data_ptr\"")
    R("r8", 8, "") R("r9", 9, "") R("r10", 10, "") R("r11", 11, "")
    R("r12", 12, "") R("r13", 13, "") R("r14", 14, "") R("r15", 15, "")
    R("rip", 16, " type=\"code_ptr\"") R("eflags", 17, "")
    "</feature></target>";
#undef R

/* gdb order -> architectural register number */
static const uint8_t gdb_gpr[16] = { X86_RAX, X86_RBX, X86_RCX, X86_RDX, X86_RSI, X86_RDI,
                                     X86_RBP, X86_RSP, 8, 9, 10, 11, 12, 13, 14, 15 };

static bool x86_gdb_read(uam_cpu *cpu, unsigned n, uint64_t *v)
{
    if (n < 16)       *v = x86_gpr(cpu, gdb_gpr[n]);
    else if (n == 16) *v = cpu->pc;
    else if (n == 17) *v = x86_get_rflags(cpu);
    else return false;
    return true;
}

static bool x86_gdb_write(uam_cpu *cpu, unsigned n, uint64_t v)
{
    if (n < 16)       x86_set_gpr(cpu, gdb_gpr[n], v);
    else if (n == 16) cpu->pc = v;
    else if (n == 17) x86_set_rflags(cpu, (uint32_t)v);
    else return false;
    return true;
}

const uam_gdb_arch x86_gdb = {
    .target_xml = x86_target_xml,
    .n_regs = GDB_X86_NREGS,
    .reg_bytes = 8,
    .read_reg = x86_gdb_read,
    .write_reg = x86_gdb_write,
};
