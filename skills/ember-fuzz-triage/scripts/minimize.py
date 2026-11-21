#!/usr/bin/env python3
"""minimize.py INPUT -- FUZZ...     the smallest input that still stops the same way

FUZZ is the campaign's command line WITHOUT --fuzz-replay or --fuzz-net (as for
triage.py). Example:

    minimize.py crashes/crash_r1_s0_ab12 -- ./fuzz.sh
    minimize.py -o small.bin in/2-store -- build/uam-run -b rv32-ref \\
                   --fuzz-begin fuzz_iteration --fuzz-input uart0 fw.elf

It removes pieces of INPUT (halves first, then smaller and smaller, down to single bytes)
and keeps a removal when the replay still gives the same result: the same verdict at the
same pc, and for a crash-fault the same kind of exception. Instruction counts, coverage
and the faulting address may change (they depend on the bytes). It ends when removing any
one byte changes the result, and writes that input to OUT (default: INPUT.min). INPUT is
never changed.

Each round replays a directory of candidates in one run of FUZZ, so the firmware boots
once a round, not once a candidate. A firmware that boots slowly: put
--fuzz-snapshot FILE in FUZZ (a file of your own, not the campaign's) and no round boots.

A timeout or a hang costs a whole budget per candidate: lower --fuzz-iter-insns in FUZZ
first (the last one given wins), or expect to wait.

Exit status: 0 OUT was written (even if nothing could be removed), 2 the replay could not
run or INPUT gives no verdict worth minimizing (ok, rejected).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

RE_REPLAY = re.compile(r"^replay: (.*?): (ok|crash-fault|crash-guard|crash-oracle|hang|timeout|rejected)"
                       r"(.*?) insns=(\d+) buckets=(\d+)\s*$")


def signature(verdict, details):
    """What must stay the same: the verdict, its pc, and a fault's kind."""
    pc = re.search(r"pc=(0x[0-9a-f]+)", details)
    kind = re.search(r"\(([a-z-]+-(?:fault|misaligned|xlate)|trap|sysreg|reset)\b", details)
    return (verdict, pc.group(1) if pc else "", kind.group(1) if kind else "")


def replay(fuzz, path, timeout):
    """Replay a file or a directory; returns {file name: signature}."""
    try:
        p = subprocess.run(fuzz + ["--fuzz-replay", path], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        sys.exit("minimize: a replay took more than %d s (--timeout)" % timeout)
    except OSError as e:
        sys.exit("minimize: cannot run %s: %s" % (fuzz[0], e))
    out = {}
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        at = line.find("replay: ")          # console output may share the line
        m = RE_REPLAY.search(line[at:]) if at >= 0 else None
        if m:
            out[os.path.basename(m.group(1))] = signature(m.group(2), m.group(3))
    if not out:
        err = [l for l in p.stderr.decode("utf-8", "replace").splitlines() if l.strip()][-3:]
        sys.stderr.write("minimize: the replay printed no verdict (exit status %d). Its last "
                         "lines:\n  %s\n" % (p.returncode, "\n  ".join(err) or "(nothing)"))
        sys.exit(2)
    return out


def main():
    argv = sys.argv[1:]
    if "--" not in argv:
        sys.exit(__doc__ if argv[:1] in (["--help"], ["-h"]) else
                 "usage: minimize.py [options] INPUT -- FUZZ...   (minimize.py --help)")
    cut = argv.index("--")
    ap = argparse.ArgumentParser(usage="minimize.py [options] INPUT -- FUZZ...")
    ap.add_argument("input")
    ap.add_argument("-o", "--out", help="where to write the result (default: INPUT.min)")
    ap.add_argument("--batch", type=int, default=64, help="candidates per replay (64)")
    ap.add_argument("--max-replays", type=int, default=400, help="replay runs in all (400)")
    ap.add_argument("--timeout", type=int, default=600, help="seconds per replay run (600)")
    a = ap.parse_args(argv[:cut])
    fuzz = argv[cut + 1:]
    if not fuzz:
        sys.exit("minimize: nothing after --: give the FUZZ command")
    for bad in ("--fuzz-replay", "--fuzz-net"):
        if bad in fuzz:
            sys.exit("minimize: leave %s out of the FUZZ command; this script adds the replay" % bad)
    if not os.path.isfile(a.input):
        sys.exit("minimize: %s is not a file" % a.input)
    out = a.out or a.input + ".min"
    best = open(a.input, "rb").read()

    work = tempfile.mkdtemp(prefix="minimize-")
    runs = [0]

    def judge(cands):
        """Replay the candidates together; returns one signature (or None) per candidate."""
        d = os.path.join(work, "round")
        shutil.rmtree(d, ignore_errors=True)
        os.mkdir(d)
        for i, c in enumerate(cands):
            with open(os.path.join(d, "c%05d" % i), "wb") as f:
                f.write(c)
        runs[0] += 1
        got = replay(fuzz, d, a.timeout)
        return [got.get("c%05d" % i) for i in range(len(cands))]

    try:
        want = judge([best])[0]
        if want is None or want[0] in ("ok", "rejected"):
            sys.stderr.write("minimize: %s replays as %s: nothing to minimize\n"
                             % (a.input, want[0] if want else "no verdict"))
            return 2
        print("minimize: %d bytes: %s" % (len(best), " ".join(x for x in want if x)))

        chunk = max(len(best) // 2, 1)
        done = False                        # True: no single byte can go
        while len(best) > 0 and runs[0] < a.max_replays:
            spans = [(o, min(o + chunk, len(best))) for o in range(0, len(best), chunk)]
            kept = []                       # the spans whose removal keeps the result
            for at in range(0, len(spans), a.batch):
                if runs[0] >= a.max_replays:
                    break
                part = spans[at:at + a.batch]
                res = judge([best[:lo] + best[hi:] for lo, hi in part])
                kept += [s for s, r in zip(part, res) if r == want]
            if kept:
                # Removals that each work alone need not work together: try them all at once,
                # and fall back on the first.
                both = b"".join(best[lo:hi] for lo, hi in
                                zip([0] + [h for _, h in kept], [l for l, _ in kept] + [len(best)]))
                if len(kept) > 1 and runs[0] < a.max_replays and judge([both])[0] == want:
                    best = both
                else:
                    lo, hi = kept[0]
                    best = best[:lo] + best[hi:]
                print("minimize: %d bytes (pieces of %d)" % (len(best), chunk))
                chunk = max(min(chunk, len(best) // 2), 1)
            elif chunk > 1:
                chunk = max(chunk // 2, 1)
            else:
                done = True
                break
    finally:
        shutil.rmtree(work, ignore_errors=True)

    with open(out, "wb") as f:
        f.write(best)
    print("minimize: %s: %d bytes after %d replay runs; %s"
          % (out, len(best), runs[0],
             "removing any one byte changes the result" if done or not best else
             "stopped at --max-replays: it may shrink further"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
