#!/usr/bin/env python3
"""log_summary.py - condense an ember runner's stderr (run with --trace exc --stats and
catch-all windows / --unknown-mmio zero) into the next blocker.

Prints, in order:
  1. how the run ended (stop reason, pc, instruction count, exit-code meaning) and stats;
     an x86 shutdown (triple fault, machine check, RSM with an invalid saved state) and an
     8-bit core halted by an access nothing answers (AVR, PIC, 8051, 6502, Z80) or a 6502
     JAM are called out here
  2. the first N distinct events in time order: first access to each unmodelled offset
     (unimp windows; x86 I/O ports by port number and name) or address (unknown-mmio),
     exceptions, resets, errors - read these top-down, the first one is usually the blocker
  3. the hottest offsets of each catch-all window (printed by ember at exit): a hot read
     offset is a polling loop, i.e. a status bit the guest waits on
  4. unmapped addresses (unknown-mmio, or a PC's logged master abort) grouped by 64 KiB
     page: windows RECON missed, or garbage pointers from a wrong base
  5. exceptions grouped by kind/cause and by pc, plus the first one with context (a storm
     at one vector usually means the vectors are not installed yet: the FIRST one is real);
     MIPS ExcCodes, RISC-V mcause, x86 vectors (#UD, #GP, #PF, INT n) and ARM causes
     (UNDEF, SVC, aborts) by name; other ISAs' causes as numbers (their uam/isa/*.h)
  6. interrupt entries, resets, --log-pc hits, --mmio-rule stubs (their accesses need -v),
     --guard/--watch hits, scripted console input never sent, x86 MSRs no device answered
     (board option -o msr-trace=1), POST codes (-v -v), and the processors involved on a
     multiprocessor run (cpu=N)

usage: log_summary.py [build/emu.log] [--isa mips|riscv|arm|x86|other] [--first 20] [--top 15]
(default: from the project's run.sh ISA=; ppc, avr, avr32, pic, mcs51, m6502, z80 and xtensa
are "other"; pic32 is mips)
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

KINDS = ["TRAP", "FETCH_FAULT", "LOAD_FAULT", "STORE_FAULT", "LOAD_MISALIGNED",
         "STORE_MISALIGNED", "SYSREG", "HELPER", "FETCH_XLATE", "LOAD_XLATE", "STORE_XLATE"]
MIPS_EXC = {0: "Int", 1: "Mod", 2: "TLBL", 3: "TLBS", 4: "AdEL", 5: "AdES", 6: "IBE", 7: "DBE",
            8: "Sys", 9: "Bp (break: often a compiler divide-by-zero trap)", 10: "RI",
            11: "CpU", 12: "Ov", 13: "Tr"}
RV_EXC = {0: "insn misaligned", 1: "insn access fault", 2: "illegal instruction",
          3: "breakpoint", 4: "load misaligned", 5: "load access fault", 6: "store misaligned",
          7: "store access fault", 8: "ecall U", 11: "ecall M"}
ARM_EXC = {1: "UNDEF (undefined instruction: VFP/NEON? wrong ARM/Thumb state?)", 2: "SVC",
           3: "PABT (BKPT)", 4: "DABT"}
X86_VEC = {0: "#DE divide error", 1: "#DB", 2: "NMI", 3: "#BP int3", 4: "#OF", 5: "#BR",
           6: "#UD invalid opcode", 7: "#NM", 8: "#DF double fault", 10: "#TS", 11: "#NP",
           12: "#SS", 13: "#GP", 14: "#PF", 16: "#MF", 17: "#AC", 18: "#MC", 19: "#XM"}
X86_TRAP_ERR, X86_TRAP_SOFT, X86_TRAP_INTN = 1 << 8, 1 << 9, 1 << 10
# Well-known PC I/O ports, to name the "io" catch-all's offsets.
PORTS = [(0x00, 0x1F, "8237 DMA 1"), (0x20, 0x21, "8259 master"), (0x2E, 0x2F, "Super I/O config"),
         (0x40, 0x43, "8254 PIT"), (0x4E, 0x4F, "Super I/O / EC config"), (0x60, 0x60, "8042 data"),
         (0x61, 0x61, "port 61h (PIT gate, NMI status)"), (0x62, 0x62, "EC data"),
         (0x64, 0x64, "8042 cmd/status"), (0x66, 0x66, "EC cmd/status"),
         (0x70, 0x73, "RTC/CMOS"), (0x80, 0x80, "POST code"), (0x81, 0x8F, "DMA page"),
         (0x92, 0x92, "port 92h (fast A20, reset)"), (0xA0, 0xA1, "8259 slave"),
         (0xB2, 0xB3, "APM (software SMI)"), (0xC0, 0xDF, "8237 DMA 2"), (0xF0, 0xF1, "FPU"),
         (0x170, 0x177, "IDE secondary"), (0x1F0, 0x1F7, "IDE primary"),
         (0x278, 0x27A, "LPT2"), (0x2E8, 0x2EF, "COM4"), (0x2F8, 0x2FF, "COM2"),
         (0x376, 0x376, "IDE secondary ctl"), (0x378, 0x37A, "LPT1"), (0x3B0, 0x3DF, "VGA"),
         (0x3E8, 0x3EF, "COM3"), (0x3F0, 0x3F5, "floppy"), (0x3F6, 0x3F6, "IDE primary ctl"),
         (0x3F7, 0x3F7, "floppy DIR"), (0x3F8, 0x3FF, "COM1"), (0x402, 0x402, "debug console (edk2)"),
         (0x4D0, 0x4D1, "ELCR"), (0x510, 0x511, "fw_cfg (a virtual-machine firmware build: OVMF)"),
         (0x0CF8, 0x0CF8, "PCI config address"), (0x0CF9, 0x0CF9, "reset control"),
         (0x0CFC, 0x0CFF, "PCI config data")]
EXIT = {"instruction limit": "124: budget (-n) used up - still booting, or looping (see hot offsets)",
        "virtual time limit": "124: --max-ticks (or the 2^62-tick horizon) reached - idle time "
                              "counts too; compare idle ticks with instructions in the stats",
        "deadlock": "125: WAIT with no pending interrupt, no armed timer and no host input - "
                    "a timer or interrupt the guest waits for is not modelled/wired",
        "livelock": "128: WAIT while timers keep firing (--max-stall events, no instruction "
                    "retired) and no input source - a timer runs but its interrupt never reaches "
                    "the CPU input the guest enabled (unwired line, masked in the intc, wrong "
                    "Cause.IP / mip bit), or a device re-arms a timer the guest never asked for",
        "reset requested": "126: a device raised a reset line but the board set no reset handling",
        "fetch fault loop": "126: the exception handler itself is not fetchable - vectors not "
                            "mapped/installed, or a jump to garbage",
        "internal error": "126: ember internal error (lift/IR failure) - report it, do not work around it",
        "debug trap": "127: guest executed a debug trap (sdbbp/ebreak/bkpt)",
        "guarded memory access": "129: a data access touched a --guard range (the guard line "
                                 "names processor, pc, access and address) - before it happened",
        "stopped by a pc hook": "127: a --break address was reached",
        "stopped by the exception observer": "an exception observer (fuzz harness) stopped the run",
        "stopped by the interrupt observer": "an interrupt observer (fuzz harness) stopped the run",
        "stop requested": "the firmware (finisher, ACPI S5 power-off) or console quit ended the run "
                          "(exit = its status); 130 = SIGINT/SIGTERM",
        "halt": "the guest halted"}

CPU = r"(?: cpu=(\d+))?"
RE_UNIMP_FIRST = re.compile(r"\] unimp: (\S+)\+0x([0-9a-f]+) \(0x([0-9a-f]+)\): first (read|write) of (\d) bytes(?:: (0x[0-9a-f]+))?")
RE_UNIMP_SUM = re.compile(r"\] unimp: (\S+): (\d+) accesses to (\d+) unimplemented offsets")
RE_UNIMP_HOT = re.compile(r"\] unimp:\s+(\S+)\+0x([0-9a-f]+): (\d+) reads, (\d+) writes \(last (0x[0-9a-f]+)\)")
RE_UNK = re.compile(r"\] (unknown-mmio|master-abort): first (read|write) of (\d) bytes at 0x([0-9a-f]+)")
RE_EXC = re.compile(r"^exc: kind=(\d+) pc=0x([0-9a-f]+) cause=0x([0-9a-f]+) tval=0x([0-9a-f]+)" + CPU)
RE_IRQ = re.compile(r"^irq: entered handler at 0x([0-9a-f]+)" + CPU)
RE_RESET = re.compile(r"^reset: (\w+)")
RE_STOP = re.compile(r"^\S+: (.+?) \(status (-?\d+)\) at pc=0x([0-9a-f]+) after (\d+) instructions")
RE_STUB_FIRST = re.compile(r"\] mmio-stub: (\S+) \(([^)]*)\): first (read|write) of (\d) bytes at 0x([0-9a-f]+)")
RE_STUB_SUM = re.compile(r"\] mmio-stub: (\S+): (\d+) reads, (\d+) writes, (\d+) registers")
RE_LOGPC = re.compile(r"^\S+: pc 0x([0-9a-f]+) reached \(hit (\d+), icount (\d+)(?:, cpu (\d+))?\)")
RE_TRIPLE = re.compile(r"^x86: triple fault at pc=(0x[0-9a-f]+)")
RE_SHUTDOWN = re.compile(r"^x86: (.+) at pc=(0x[0-9a-f]+): hart shut down")
RE_HALT = re.compile(r"\] (\w+): pc=(0x[0-9a-f]+): (fetch fault|load from nothing|store to nothing) at (0x[0-9a-f]+): core halted")
RE_JAM = re.compile(r"\] m6502: pc=(0x[0-9a-f]+): JAM")
RE_MSR = re.compile(r"^x86: (rdmsr|wrmsr) (0x[0-9a-f]+) (?:->|<-) (0x[0-9a-f]+)")
RE_POST = re.compile(r"\] post: code (0x[0-9a-f]+)")
RE_GUARD = re.compile(r"^(?:\S+: )?guard (\S+) \((stop|fault|log)\): cpu (\d+) pc (0x[0-9a-f]+) (read|write) (\d+) at (0x[0-9a-f]+)")
RE_CONSOLE_LEFT = re.compile(r"\] chardev: console input: (\d+) byte\(s\) never sent, waiting for '(.*)' \(step")
PIO_BASE = 0x10000000000


def default_isa():
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        for ln in open(os.path.join(here, "..", "run.sh")):
            if ln.startswith("ISA="):
                v = ln.strip()[4:]
                return ("riscv" if v.startswith("rv") else "arm" if v.startswith("arm")
                        else "x86" if v.startswith("x86")
                        else "mips" if v.startswith("mips") or v == "pic32" else "other")
    except OSError:
        pass
    return "mips"


def port_name(port):
    for lo, hi, name in PORTS:
        if lo <= port <= hi:
            return name
    return ""


def exc_name(kind, cause, isa):
    k = KINDS[kind] if kind < len(KINDS) else "kind%d" % kind
    if isa == "x86":
        if kind == 0:
            vec = cause & 0xFF
            if cause & X86_TRAP_SOFT and vec not in (3, 4):
                return "TRAP INT 0x%x (software interrupt%s)" % (
                    vec, ": a BIOS service call" if vec in (0x10, 0x13, 0x15, 0x16, 0x1A) else "")
            name = X86_VEC.get(vec, "vector 0x%x" % vec)
            return "TRAP %s%s" % (name, " err=0x%x" % (cause >> 32) if cause & X86_TRAP_ERR else "")
        if k in ("SYSREG", "HELPER"):
            return k + " (#GP, #SS, #NP, #TS or #UD raised by a helper: privileged state, a bad " \
                       "selector or MSR; the vector is not in the trace)"
        if k.endswith("XLATE"):
            return "%s #PF err=0x%x" % (k, cause)
        if k in ("FETCH_FAULT", "LOAD_FAULT", "STORE_FAULT"):
            return k + " (delivered as #GP: nothing decodes the address)"
        return k
    if kind == 0:
        table = RV_EXC if isa == "riscv" else ARM_EXC if isa == "arm" else MIPS_EXC if isa == "mips" else {}
        return ("%s cause=%d %s" % (k, cause, table.get(cause, ""))).rstrip()
    if isa == "arm" and k.endswith("XLATE"):
        return "%s fsr=0x%x" % (k, cause)
    return k


def where(addr, window, isa):
    """An unimp window offset as text; the x86 I/O-port catch-all by port number."""
    if isa != "x86":
        return ""
    if window == "io" or addr >= PIO_BASE and addr < PIO_BASE + 0x10000:
        port = addr - PIO_BASE
        name = port_name(port)
        return "port 0x%x%s" % (port, " (%s)" % name if name else "")
    return ""


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", default="build/emu.log")
    ap.add_argument("--isa", choices=["mips", "riscv", "arm", "x86", "other"], default=None)
    ap.add_argument("--first", type=int, default=20)
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args(argv)
    isa = a.isa or default_isa()
    lines = open(a.log, errors="replace").read().splitlines()

    first, stop, stats = [], None, []
    hot = defaultdict(list)          # window -> [(count, off, reads, writes, last)]
    sums = {}
    unk_pages = defaultdict(set)
    exc_by_kind, exc_by_pc = Counter(), Counter()
    first_exc = None
    irqs, resets, logpc = Counter(), Counter(), {}
    stubs, abi, snap = {}, [], []
    cpus = Counter()
    triple = None
    halted = None
    smi_lost = None                  # first write to port 0xB2 that only a catch-all saw
    msrs = {}                        # (op, msr) -> [count, last value]
    posts = []
    guards = Counter()               # (guard, action, access) -> hits
    console_left = None

    def note(i, text):
        if len(first) < a.first:
            first.append((i + 1, text))

    for i, ln in enumerate(lines):
        m = RE_UNIMP_FIRST.search(ln)
        if m:
            w, off, addr, rw, sz, val = m.groups()
            w_ = where(int(addr, 16), w, isa)
            if rw == "write" and int(addr, 16) == PIO_BASE + 0xB2 and smi_lost is None:
                smi_lost = (i + 1, val)
            note(i, "unimp %-6s %s+0x%s (phys 0x%s) %sB%s%s" % (rw, w, off, addr, sz, " = " + val if val else "",
                                                             "  " + w_ if w_ else ""))
            continue
        m = RE_UNIMP_HOT.search(ln)
        if m:
            w, off, r, wr, last = m.groups()
            hot[w].append((int(r) + int(wr), off, int(r), int(wr), last))
            continue
        m = RE_UNIMP_SUM.search(ln)
        if m:
            sums[m.group(1)] = (int(m.group(2)), int(m.group(3)))
            continue
        m = RE_STUB_FIRST.search(ln)
        if m:
            name, how, rw, sz, addr = m.groups()
            note(i, "rule   %-6s 0x%s %sB (%s, %s)" % (rw, addr, sz, name, how))
            continue
        m = RE_STUB_SUM.search(ln)
        if m:
            stubs[m.group(1)] = tuple(int(x) for x in m.groups()[1:])
            continue
        m = RE_UNK.search(ln)
        if m:
            src, rw, sz, addr = m.groups()
            unk_pages[int(addr, 16) & ~0xFFFF].add(int(addr, 16))
            note(i, "unmapped %-5s 0x%s %sB (no window covers it%s)" % (
                rw, addr, sz, "; master abort: reads all ones" if src == "master-abort" else ""))
            continue
        m = RE_EXC.match(ln)
        if m:
            kind, pc, cause, tval = int(m.group(1)), m.group(2), int(m.group(3), 16), m.group(4)
            cpu = int(m.group(5) or 0)
            cpus[cpu] += 1
            name = exc_name(kind, cause, isa)
            exc_by_kind[name] += 1
            exc_by_pc[(pc, name)] += 1
            if first_exc is None:
                first_exc = i
            if exc_by_kind[name] == 1:
                note(i, "exception %s at pc 0x%s tval 0x%s%s" % (name, pc, tval, " cpu %d" % cpu if cpu else ""))
            continue
        m = RE_IRQ.match(ln)
        if m:
            irqs[m.group(1)] += 1
            continue
        m = RE_RESET.match(ln)
        if m:
            resets[m.group(1)] += 1
            note(i, "reset (%s) requested by a device" % m.group(1))
            continue
        m = RE_LOGPC.match(ln)
        if m:
            logpc[m.group(1)] = (int(m.group(2)), int(m.group(3)))
            continue
        m = RE_TRIPLE.match(ln)
        if m:
            triple = m.group(1)
            note(i, "x86 TRIPLE FAULT at pc %s: the processor shut down" % triple)
            continue
        m = RE_SHUTDOWN.match(ln)
        if m:
            # also a machine check (SMM code fetch outside SMRAM) or an RSM with an
            # invalid saved state: the processor stopped the same way
            note(i, "x86 SHUTDOWN at pc %s: %s" % (m.group(2), m.group(1)))
            continue
        m = RE_HALT.search(ln)
        if m:
            halted = m.groups()
            note(i, "%s CORE HALTED at pc %s: %s at bus address %s" % m.groups())
            continue
        m = RE_JAM.search(ln)
        if m:
            halted = ("m6502", m.group(1), "JAM opcode", None)
            note(i, "m6502 JAM at pc %s: the core stops until reset" % m.group(1))
            continue
        m = RE_MSR.match(ln)
        if m:
            op, msr, val = m.groups()
            e = msrs.setdefault((op, msr), [0, val])
            e[0] += 1
            e[1] = val
            if e[0] == 1:
                note(i, "%s %s %s %s (no device answers it: the MSR policy did)" %
                     (op, msr, "->" if op == "rdmsr" else "<-", val))
            continue
        m = RE_GUARD.match(ln)
        if m:
            name, act, cpu, pc, rw, sz, addr = m.groups()
            guards[(name, act, rw)] += 1
            if guards[(name, act, rw)] == 1:
                note(i, "guard %s (%s): %s %sB at %s, pc %s%s" % (
                    name, act, rw, sz, addr, pc, " cpu " + cpu if cpu != "0" else ""))
            continue
        m = RE_CONSOLE_LEFT.search(ln)
        if m:
            console_left = (int(m.group(1)), m.group(2))
            continue
        m = RE_POST.search(ln)
        if m:
            if not posts or posts[-1] != m.group(1):
                posts.append(m.group(1))
            continue
        m = RE_STOP.match(ln)
        if m:
            stop = m.groups()
            continue
        if ln.startswith("stats:"):
            stats.append(ln)
        elif ln.startswith("[error]"):
            if "] abi:" in ln:
                abi.append(ln)
            elif "] snapshot:" in ln:
                snap.append(ln)
            note(i, ln)

    print("== how the run ended")
    if stop:
        why, status, pc, n = stop
        print("   %s (status %s) at pc=0x%s after %s instructions" % (why, status, pc, n))
        for k, v in EXIT.items():
            if why.startswith(k):
                print("   -> exit " + v)
    elif abi:
        print("   setup failed (exit 2): the board was compiled against other ember headers than\n"
              "   libuam was built from. Rebuild both from the pinned checkout: scripts/build-ember.sh")
        for ln in abi[:6]:
            print("     " + ln)
    else:
        print("   no stop line: killed (timeout -k / SIGKILL), crashed, or a setup error (exit 2) -\n"
              "   check the shell's exit code. ember bounds virtual time itself (--max-stall ends a\n"
              "   stalled WAIT as a livelock, exit 128), so a run killed by timeout while idle was\n"
              "   usually waiting for console/network input: run with < /dev/null and no host --nic.")
    if triple:
        print("   -> x86 triple fault at pc %s: an exception while delivering a double fault (no or a\n"
              "      broken IDT/GDT, a stack in memory that is not there). The processor stopped, so the\n"
              "      run ended idle (125/128). The FIRST exception below started it." % triple)
    if halted and halted[3]:
        print("   -> %s core halted at pc %s: %s at bus address %s. This core has no bus error to\n"
              "      take, so the hart stopped and the run ended idle: give that address a device, RAM\n"
              "      or a catch-all window. No timer or interrupt is missing." % halted)
    elif halted:
        print("   -> 6502 JAM at pc %s: execution went astray (wrong base, bad jump) or the CPU model\n"
              "      is wrong. The run ended idle: no timer or interrupt is missing." % halted[1])
    if smi_lost:
        print("   -> software SMI lost (log line %d: port 0xB2 = %s reached a catch-all): no APM/SMI\n"
              "      device decodes 0xB2, so SMM never ran. Everything behind an SMI fails quietly\n"
              "      (SetVariable returns EFI_UNSUPPORTED, SMM drivers never initialize). Model the\n"
              "      chipset's APM_CNT + SMI_EN (examples/pc/ich9_lpc.c) or run with -o apm-smi=1." % smi_lost)
    for s in stats:
        print("   " + s)
    m = re.search(r"idle ticks=(\d+) virtual time=(\d+)", " ".join(stats))
    if m and stop and not triple and int(m.group(1)) > 1000 * max(1, int(stop[3])) \
            and not stop[0].startswith("livelock"):
        print("   -> idle time far above instructions: the hart mostly sat in WAIT while timers fired.\n"
              "      If the guest expected a tick, follow that timer's line to the CPU input it enabled.")
    for ln in snap[:4]:
        msg = ln.split("] snapshot: ", 1)[1]
        print("   snapshot: " + msg)
        if "has no save/restore" in msg or "is not saved by its" in msg:
            print("   -> every device needs save/restore or UAM_DEVICE_STATELESS, and saves the "
                  "timers it arms (device-anatomy.md)")
        elif "unread" in msg or "ends early" in msg or "saved array" in msg:
            print("   -> a device's save and restore disagree, or its layout changed: fix it, re-take the snapshot")
        elif "differs" in msg or "is of board" in msg or "holds state for" in msg:
            print("   -> load with the same board build, options, images, --ram, rules and ports")

    print("\n== first distinct events (log line: event)")
    for n, t in first:
        print("%7d: %s" % (n, t))

    print("\n== hottest unmodelled offsets per window (from ember's exit summary)")
    if not hot:
        print("   none (no catch-all windows, or the run was killed before devices were destroyed)")
    for w in sorted(hot, key=lambda w: -sum(x[0] for x in hot[w])):
        tot = sums.get(w)
        print("   %s%s" % (w, "  (%d accesses, %d offsets)" % tot if tot else ""))
        for c, off, r, wr, last in sorted(hot[w], reverse=True)[:a.top]:
            tag = "  <- polled?" if r > 100 and r > 10 * wr else ""
            w_ = where(PIO_BASE + int(off, 16), w, isa) if w == "io" else ""
            print("   %10d  +0x%-6s %d R / %d W, last write %s%s%s" % (c, off, r, wr, last, tag,
                                                                     "  " + w_ if w_ else ""))

    if unk_pages:
        print("\n== unmapped addresses by 64 KiB page (add a window, or suspect a wrong base)")
        for page, addrs in sorted(unk_pages.items(), key=lambda kv: -len(kv[1]))[:a.top]:
            offs = " ".join("+0x%x" % (x - page) for x in sorted(addrs)[:8])
            print("   0x%08x: %d addresses  %s%s" % (page, len(addrs), offs, " ..." if len(addrs) > 8 else ""))

    print("\n== exceptions by kind: %s" % (dict(exc_by_kind) if exc_by_kind else "none"))
    for (pc, name), c in exc_by_pc.most_common(8):
        print("%8d  pc 0x%s  %s" % (c, pc, name))
    if first_exc is not None:
        print("\n== first exception (line %d) with context:" % (first_exc + 1))
        for ln in lines[max(0, first_exc - 4):first_exc + 5]:
            print("    " + ln)

    if irqs:
        print("\n== interrupt entries: %d (handler pcs: %s)" % (sum(irqs.values()), dict(irqs.most_common(4))))
    if resets:
        print("== resets: %s" % dict(resets))
    if len(cpus) > 1 or (cpus and 0 not in cpus):
        print("== exceptions per processor (cpu: count): %s" % dict(sorted(cpus.items())))
    if stubs:
        print("== --mmio-rule stubs (name: reads, writes, registers): %s" % stubs)
    for pc, (hits, ic) in logpc.items():
        print("== --log-pc 0x%s: %d hits, last at icount %d" % (pc, hits, ic))
    if msrs:
        print("\n== MSRs answered by the MSR policy, not a device (op msr: count, last value)")
        for (op, msr), (c, val) in sorted(msrs.items(), key=lambda kv: int(kv[0][1], 16)):
            print("   %s %s: %d, last %s" % (op, msr, c, val))
    if guards:
        print("== --guard/--watch hits (guard, action, access: count): %s" %
              ", ".join("%s %s %s: %d" % (k + (c,)) for k, c in guards.most_common(8)))
    if console_left:
        print("== scripted console input: %d byte(s) never sent - the guest never printed '%s'\n"
              "   (a wrong prompt pattern, or the guest stopped before it; patterns match this run's\n"
              "   output only, not output from before a --load-snapshot)" % console_left)
    if posts:
        print("\n== POST codes in order (last is where the firmware got to): %s" % " ".join(posts[-24:]))
    print("\n%d lines total" % len(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
