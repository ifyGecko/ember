#!/usr/bin/env python3
"""triage.py INPUT -- FUZZ...     the first pass on one fuzz input, as a report

FUZZ is the campaign's command line WITHOUT --fuzz-replay or --fuzz-net: the runner, the
board and image, and the fuzz options (--fuzz-begin, --fuzz-input, --fuzz-crash-pc, ...).
In an ember-dev-build project that is ./fuzz.sh. Example:

    triage.py crashes/crash_r1_s0_ab12 -- ./fuzz.sh
    triage.py in/2-store -- build/uam-run -b rv32-ref --fuzz-begin fuzz_iteration \\
                               --fuzz-input uart0 fw.elf

It replays INPUT a few times and writes OUT/report.md (default OUT: triage-<input name>):

  1. twice on the interpreter: the verdict must be the same both times;
  2. once on the JIT (--jit): the verdict must be the same again;
  3. once with --history, --backtrace, the exception trace and the register dump on.

The report holds the verdict, what it means, the exceptions the input took, the last
blocks executed, the call chain and the registers at the stop, followed by the sections a
person has to fill in (references/report.md). The raw output of every run is kept in OUT.
Each replay may take up to --timeout seconds (600): lower it for a quick look.

Nothing here is specific to a board or an architecture. Exit status: 0 a report was
written, 1 the input did not reproduce the same way twice (written anyway, with a
warning), 2 the replay could not run at all.
"""
import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys

RE_REPLAY = re.compile(r"^replay: (.*?): (ok|crash-fault|crash-guard|crash-oracle|hang|timeout|rejected)"
                       r"(.*?) insns=(\d+) buckets=(\d+)\s*$")

MEANING = {
    "ok": "The input ran to the end of the iteration and nothing was detected. If it was "
          "reported as a crash, the options differ from the campaign's.",
    "crash-fault": "The processor took an exception the campaign counts as a crash. pc is the "
                   "faulting instruction; for a memory fault, tval is the address accessed.",
    "crash-guard": "An access to guarded memory was stopped before it happened. pc is the "
                   "instruction that tried; the details name the access and the guard.",
    "crash-oracle": "The firmware reached an address marked fatal, reset, halted with an error "
                    "or raised a watched interrupt. pc is the panic site, not the cause: the "
                    "caller in the call chain is.",
    "hang": "The firmware waits and nothing can wake it.",
    "timeout": "The input used up its instruction budget (--fuzz-iter-insns): a loop, or slow.",
    "rejected": "The input could not be delivered; nothing ran.",
}

NEXT = {
    "crash-fault": [
        "Look up tval in `RUN --map`: nothing there, RAM, or a device?",
        "Find the register that held it (the faulting instruction's base register) in the "
        "registers above, then trace it back: SKILL.md step 8.",
        "A fetch fault or a pc that is not code: the last history entry is the jump; try "
        "`--guard-returns`.",
    ],
    "crash-guard": [
        "The pc is at or just below the bug. Which length or index did the instruction use?",
        "Read the details with references/verdicts.md, \"crash-guard\".",
    ],
    "crash-oracle": [
        "The call chain's second frame is the check that failed.",
        "If an exception is listed above, triage that one: its pc is the real crash site.",
        "A reset: is it the watchdog? Compare insns with a clean input's.",
    ],
    "hang": [
        "The history and call chain end in the wait. What is it waiting for?",
        "`-t mmio-summary` and `-t irq`: a status register that never changes, an "
        "interrupt that is pending but masked.",
    ],
    "timeout": [
        "The history above shows the loop if there is one (the same blocks repeating).",
        "Disassemble that block and log its registers each time round: "
        "`--log-pc ADDR,REG,...` (references/verdicts.md, \"timeout\").",
    ],
}


def run(cmd, timeout, log_path):
    """Run cmd; returns (exit status or None on a timeout, stdout, stderr)."""
    try:
        p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc, out, err = None, e.stdout or b"", e.stderr or b""
    except OSError as e:
        sys.exit("triage: cannot run %s: %s" % (cmd[0], e))
    out = out.decode("utf-8", "replace")
    err = err.decode("utf-8", "replace")
    with open(log_path, "w") as f:
        f.write("$ %s\n# exit status: %s\n\n--- stdout ---\n%s\n--- stderr ---\n%s"
                % (" ".join(shlex.quote(c) for c in cmd), "timed out" if rc is None else rc,
                   out, err))
    return rc, out, err


def replay_line(out):
    """The replay: line of the output (the last one), parsed; or None."""
    found = None
    for line in out.splitlines():
        m = RE_REPLAY.search(line[line.find("replay: "):] if "replay: " in line else line)
        if m:
            found = m
    return found


def verdict_text(m):
    return "%s%s insns=%s buckets=%s" % (m.group(2), m.group(3), m.group(4), m.group(5))


def section(text, start):
    """The lines of the diagnostic run that belong to the input (after `trace: input`)."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("trace: input"):
            return lines[i + 1:]
    return lines if not start else lines


def hexdump(data, limit=256):
    rows = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        rows.append("%08x  %-47s  |%s|" % (off, " ".join("%02x" % b for b in chunk),
                                           "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    if len(data) > limit:
        rows.append("... (%d bytes in all)" % len(data))
    return "\n".join(rows)


def main():
    argv = sys.argv[1:]
    if "--" not in argv:
        sys.exit(__doc__.strip().split("\n\n")[0] + "\n\ntriage: put -- before the FUZZ command "
                 "(triage.py --help)" if argv[:1] != ["--help"] and argv[:1] != ["-h"] else __doc__)
    cut = argv.index("--")
    ap = argparse.ArgumentParser(usage="triage.py [options] INPUT -- FUZZ...",
                                 description="The first pass on one fuzz input, as a report.")
    ap.add_argument("input")
    ap.add_argument("--out", help="directory for the report (default: triage-<input name>)")
    ap.add_argument("--history", type=int, default=32, help="events of history to keep (32)")
    ap.add_argument("--timeout", type=int, default=600, help="seconds per replay (600)")
    ap.add_argument("--no-jit", action="store_true", help="skip the JIT comparison")
    a = ap.parse_args(argv[:cut])
    fuzz = argv[cut + 1:]
    if not fuzz:
        sys.exit("triage: nothing after --: give the FUZZ command")
    for bad in ("--fuzz-replay", "--fuzz-net"):
        if bad in fuzz:
            sys.exit("triage: leave %s out of the FUZZ command; this script adds the replay" % bad)
    if not os.path.isfile(a.input):
        sys.exit("triage: %s is not a file (one input at a time)" % a.input)

    out = a.out or "triage-" + os.path.basename(a.input.rstrip("/"))
    os.makedirs(out, exist_ok=True)
    data = open(a.input, "rb").read()
    base = fuzz + ["--fuzz-replay", a.input]
    notes, repeatable = [], True

    rc1, o1, e1 = run(base, a.timeout, os.path.join(out, "replay-1.log"))
    m1 = replay_line(o1)
    if not m1:
        why = [line for line in e1.splitlines() if line.strip()][-3:]
        sys.stderr.write("triage: the replay printed no verdict (exit status %s). Its last lines:\n  %s\n"
                         "triage: the full output is in %s/replay-1.log\n"
                         % ("timed out" if rc1 is None else rc1, "\n  ".join(why) or "(nothing)", out))
        return 2
    rc2, o2, _ = run(base, a.timeout, os.path.join(out, "replay-2.log"))
    m2 = replay_line(o2)
    if not m2 or verdict_text(m2) != verdict_text(m1):
        repeatable = False
        notes.append("**Not repeatable**: a second identical replay gave `%s`. Something outside "
                     "the guest feeds the run (a console on a terminal, a host network port). "
                     "Find it before trusting anything below."
                     % (verdict_text(m2) if m2 else "no verdict"))
    if a.no_jit:
        jit = "not checked (--no-jit)"
    else:
        _, oj, _ = run(base + ["--jit"], a.timeout, os.path.join(out, "replay-jit.log"))
        mj = replay_line(oj)
        if mj and verdict_text(mj) == verdict_text(m1):
            jit = "the same verdict"
        else:
            jit = "DIFFERENT: `%s`" % (verdict_text(mj) if mj else "no verdict")
            notes.append("**The JIT disagrees with the interpreter.** That is an ember bug, not "
                         "a firmware bug: report it with this input and both lines.")

    cmds = [base, base] + ([] if a.no_jit else [base + ["--jit"]])
    diag = base + ["--history", str(a.history), "--backtrace", "-t", "exc", "-d"]
    rcd, od, ed = run(diag, a.timeout, os.path.join(out, "replay-diag.log"))
    if not replay_line(od):
        # An option the setup does not take (a snapshot saved without --backtrace, say).
        diag = base + ["--history", str(a.history), "-t", "exc", "-d"]
        rcd, od, ed = run(diag, a.timeout, os.path.join(out, "replay-diag.log"))
        notes.append("The replay did not run with `--backtrace` (see replay-diag.log), so there "
                     "is no call chain below.")
    cmds.append(diag)
    lines = section(ed, True)
    excs = [l for l in lines if l.startswith("exc: kind=") or l.startswith("irq: entered")
            or l.startswith("reset:")]
    hist = [l for l in lines if l.startswith("history:")]
    back = [l for l in lines if l.startswith("backtrace:")]
    regs, grab = [], False
    for l in ed.splitlines():
        if re.match(r"^(cpu\d+: )?pc=[0-9a-f]+ ", l):
            grab = True
        elif grab and (l.startswith("uam-run:") or l.startswith("exc:") or l.startswith("irq:")
                       or not l.strip()):
            grab = False
        if grab:
            regs.append(l)

    verdict = m1.group(2)
    with open(os.path.join(out, "report.md"), "w") as f:
        w = f.write
        w("# Triage of %s\n\n" % os.path.basename(a.input))
        w("Started by triage.py. The facts below were collected automatically; the sections "
          "marked TODO are yours (references/report.md).\n\n")
        for n in notes:
            w("> %s\n\n" % n)
        w("## Reproduce\n\n")
        w("- Input: `%s`, %d bytes, sha256 `%s`\n\n```\n%s\n```\n\n"
          % (a.input, len(data), hashlib.sha256(data).hexdigest(), hexdump(data)))
        w("- Command:\n\n```\n%s\n```\n\n" % " ".join(shlex.quote(c) for c in base))
        w("- Result:\n\n```\n%s\n```\n\n" % m1.group(0).strip())
        w("- Repeatable: %s\n- On the JIT: %s\n- Minimal: TODO\n\n"
          % ("yes (two identical replays)" if repeatable else "NO", jit))
        w("## What\n\n**%s**: %s\n\n" % (verdict, MEANING[verdict]))
        pc = re.search(r"pc=(0x[0-9a-f]+)", m1.group(3))
        w("## Where\n\n- pc: %s\n- Instructions from the begin pc: %s\n"
          % (pc.group(1) if pc else "(none in the verdict)", m1.group(4)))
        w("- Function, instruction, source line: TODO (`objdump -d`, `addr2line -f -e`, or Ghidra)\n\n")
        if excs:
            w("Exceptions, interrupts and resets during the input, in order (the first "
              "exception is usually the real crash site):\n\n```\n%s\n```\n\n" % "\n".join(excs))
        w("## How execution got there\n\n")
        w("Call chain at the stop, innermost first:\n\n```\n%s\n```\n\n"
          % ("\n".join(back) if back else "(none: no --backtrace output)"))
        w("Up to %d events before the stop, oldest first (`blk` = a block of instructions "
          "starting at that address):\n\n```\n%s\n```\n\n"
          % (a.history, "\n".join(hist) if hist else "(none)"))
        if hist:
            addrs = [l.split()[2] for l in hist if " blk " in l]
            tail = addrs[-8:]
            if len(tail) >= 4 and len(set(tail)) <= 2:
                w("The last blocks repeat (%s): a loop.\n\n" % ", ".join(sorted(set(tail))))
        w("Registers at the stop:\n\n```\n%s\n```\n\n" % ("\n".join(regs) if regs else "(none)"))
        w("## Next steps for this verdict\n\n")
        for step in NEXT.get(verdict, ["See references/verdicts.md."]):
            w("- %s\n" % step)
        w("\n## Why\n\nTODO: the chain from input bytes to the bad value, each step with the "
          "output that shows it (SKILL.md step 8).\n\n")
        w("## Classification\n\nTODO: firmware bug / emulation gap / harness artefact / ember "
          "bug, and the evidence (SKILL.md step 9).\n\n")
        w("## Impact\n\nTODO: what the input controls, the consequence, who can send it.\n\n")
        w("## Commands used\n\n```\n%s\n```\n"
          % "\n".join(" ".join(shlex.quote(c) for c in cmd) for cmd in cmds))

    print("triage: %s: %s" % (a.input, verdict_text(m1)))
    for n in notes:
        print("triage: " + re.sub(r"[*`]", "", n))
    print("triage: report in %s/report.md (raw output beside it)" % out)
    return 0 if repeatable else 1


if __name__ == "__main__":
    sys.exit(main())
