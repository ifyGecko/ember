# gdb_trace_calls.py - log entry arguments and return values of guest functions, without
# stopping the guest. Use it to find WHICH call fails ("returned 0xffffffff") or WHAT a
# hang loops on (the same call repeating with the same arguments).
#
# usage with ember's gdb stub (the runner waits for gdb before executing anything; the
# stub sends a target description, so gdb picks the architecture itself, but a big-endian
# MIPS, ARM BE8 or PowerPC guest still needs `set endian big`):
#   ./run.sh --gdb 1234 > build/console-gdb.txt 2> build/emu-gdb.log < /dev/null &
#   gdb-multiarch -q -batch -ex 'set endian big' -ex 'target remote :1234' \
#       -ex 'source scripts/gdb_trace_calls.py' \
#       -ex 'trace 0x80001234 my_func' -ex 'trace 0x80005678 other' -ex continue > build/gdb.txt 2>&1
# Redirect to a file: if the batch session is killed by a timeout, piped output is lost.
# Breakpoints are ember PC hooks: precise, but each one discards translated code once.
#
# Calling conventions: MIPS o32 (a0-a3, ra, v0), RISC-V (a0-a3, ra, a0), PowerPC SysV/EABI
# (r3-r6, lr, r3), ARM AAPCS (r0-r3, lr, r0; a Thumb function's address is given WITHOUT bit 0 - gdb addresses never carry
# it, ember's stub breaks in both states), x86-64 (return address on the stack, result in
# rax): Microsoft x64 (rcx rdx r8 r9) - what UEFI code uses, the default - or System V
# (rdi rsi rdx rcx: coreboot, Linux) with `trace-abi sysv`. ember's x86 stub always
# describes the amd64 register set, so gdb cannot tell 32-bit code (SEC, an IA32 PEI
# phase, coreboot's romstage) from 64-bit: `trace-abi i386` for those (cdecl: arguments
# and return address on the stack, result in eax). Real-mode (16-bit) code is not handled.
# On a multiprocessor machine every processor is a gdb thread; the trace names the one
# that made the call.
import gdb

REGS = {  # (argument registers, return-address register or None = on the stack, result)
    "mips": (["a0", "a1", "a2", "a3"], "ra", "v0"),
    "riscv": (["a0", "a1", "a2", "a3"], "ra", "a0"),
    "arm": (["r0", "r1", "r2", "r3"], "lr", "r0"),
    "powerpc": (["r3", "r4", "r5", "r6"], "lr", "r3"),
}
X86_64 = {"ms": ["rcx", "rdx", "r8", "r9"], "sysv": ["rdi", "rsi", "rdx", "rcx"], "i386": None}
_abi_x64 = ["ms"]


def _arch():
    return gdb.selected_inferior().architecture().name().lower()


def _bits():
    return 8 * int(gdb.parse_and_eval("sizeof(void *)"))


def _reg(n):
    # 32-bit MIPS targets report registers sign-extended to 64 bits; mask to pointer width.
    return int(gdb.parse_and_eval("$" + n)) & ((1 << _bits()) - 1)


def _mem(addr, size):
    t = gdb.lookup_type("unsigned long long" if size == 8 else "unsigned int")
    return int(gdb.parse_and_eval("*(%s *)0x%x" % (t, addr))) & ((1 << (8 * size)) - 1)


def _call_site():
    """(argument values, return address) at a function's first instruction."""
    a = _arch()
    if "i386" in a and (_abi_x64[0] == "i386" or not ("x86-64" in a or "x86_64" in a)):
        sp = _reg("rsp" if "64" in a else "esp") & 0xFFFFFFFF
        return [_mem(sp + 4 * (i + 1), 4) for i in range(4)], _mem(sp, 4)
    if "x86-64" in a or "x86_64" in a:
        sp = _reg("rsp")
        return [_reg(r) for r in X86_64[_abi_x64[0]]], _mem(sp, 8)
    for key, (args, ra, _) in REGS.items():
        if key in a:
            return [_reg(r) for r in args], _reg(ra) & ~1 if key == "arm" else _reg(ra)
    raise gdb.GdbError("architecture %s: no calling convention here (mips, riscv, arm, powerpc, x86)" % a)


def _result():
    a = _arch()
    if "x86-64" in a or "x86_64" in a:
        return _reg("rax") & (0xFFFFFFFF if _abi_x64[0] == "i386" else ~0)
    if "i386" in a:
        return _reg("eax")
    for key, (_, _, ret) in REGS.items():
        if key in a:
            return _reg(ret)
    raise gdb.GdbError("architecture %s is not an ember ISA" % a)


def _thread():
    t = gdb.selected_thread()
    return " [cpu %d]" % (t.num - 1) if t is not None and len(gdb.selected_inferior().threads()) > 1 else ""


class _RetBP(gdb.Breakpoint):
    def __init__(self, addr, name):
        super().__init__("*0x%x" % addr, internal=True, temporary=True)
        self.name = name

    def stop(self):
        print("  <- %s returned 0x%x%s" % (self.name, _result(), _thread()), flush=True)
        return False


class _CallBP(gdb.Breakpoint):
    def __init__(self, addr, name):
        super().__init__("*0x%x" % addr, internal=True)
        self.name = name

    def stop(self):
        args, ra = _call_site()
        print("-> %s(%s) ret=0x%x%s" % (self.name, ", ".join("0x%x" % v for v in args), ra, _thread()),
              flush=True)
        _RetBP(ra, self.name)
        return False


class Trace(gdb.Command):
    """trace ADDR [NAME] - log calls to ADDR and their return values."""

    def __init__(self):
        super().__init__("trace", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        parts = arg.split()
        if not parts:
            raise gdb.GdbError("usage: trace ADDR [NAME]")
        _CallBP(int(parts[0], 0), parts[1] if len(parts) > 1 else parts[0])


class TraceAbi(gdb.Command):
    """trace-abi ms|sysv|i386 - the x86 calling convention (default ms: 64-bit UEFI)."""

    def __init__(self):
        super().__init__("trace-abi", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if arg.strip() not in X86_64:
            raise gdb.GdbError("usage: trace-abi ms|sysv|i386")
        _abi_x64[0] = arg.strip()


Trace()
TraceAbi()
