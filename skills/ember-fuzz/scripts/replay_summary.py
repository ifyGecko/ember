#!/usr/bin/env python3
"""replay_summary.py - group the lines of an ember --fuzz-replay run into distinct findings.

Reads the runner's stdout (one "replay: PATH: VERDICT ..." line per input; guest console
output in front of a line, left there by a chatty firmware whose console is stdout, is
skipped) and prints, most severe first, one group per (verdict, location): crash-fault by
faulting pc and exception kind/cause (tval ignored: it varies with the input), crash-guard
by pc, access direction and size and guard name (the address, the offset into the guard and
the guard's range ignored: they vary with the input, and a mem-after zone moves with its
length), crash-oracle by pc (a crash pc or a debug trap) / reset / interrupt lines / halt
status, hang by stop reason, timeout and rejected as one group each. Each group lists its
count and its smallest input, the one to reproduce and debug first.

usage: replay_summary.py [build/replay.txt] [--all] [--examples N]
"""
import argparse
import os
import re
import sys
from collections import defaultdict

RE_LINE = re.compile(r"replay: (.*): (ok|crash-fault|crash-guard|crash-oracle|hang|timeout|rejected)"
                     r"(.*?) insns=(\d+) buckets=(\d+)$")
RANK = {"crash-fault": 0, "crash-guard": 1, "crash-oracle": 2, "hang": 3, "timeout": 4,
        "rejected": 5, "ok": 6}
# " (read 1 at 0x80000470: +0 into guard g_buf [0x80000470, 0x80000570))"
RE_GUARD = re.compile(r" \((read|write) (\d+) at 0x[0-9a-f]+: (?:\+\d+ into|\d+ before)"
                      r" guard (.*) \[0x[0-9a-f]+, 0x[0-9a-f]+\)\)")


def signature(verdict, rest):
    rest = rest.strip()
    rest = RE_GUARD.sub(r" (\1 \2, guard \3)", rest)
    if verdict == "crash-fault":
        rest = re.sub(r" tval 0x[0-9a-f]+", "", rest)
    elif verdict == "crash-oracle":
        rest = re.sub(r" \(at pc 0x[0-9a-f]+, handler", " (handler", rest)
    elif verdict == "rejected":
        rest = ""
    return rest


def size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return -1


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("replay", nargs="?", default="build/replay.txt")
    ap.add_argument("--all", action="store_true", help="also list ok inputs")
    ap.add_argument("--examples", type=int, default=3, help="inputs shown per group")
    a = ap.parse_args(argv)

    groups = defaultdict(list)
    total, other = 0, 0
    for ln in open(a.replay, errors="replace"):
        m = RE_LINE.search(ln.rstrip("\r\n"))
        if not m:
            other += "replay: " in ln
            continue
        path, verdict, rest, insns, buckets = m.groups()
        total += 1
        groups[(verdict, signature(verdict, rest))].append((size(path), path, int(insns), int(buckets)))

    if not total:
        print("no replay lines found in %s (run with --fuzz-replay, stdout)" % a.replay)
        return 1
    counts = defaultdict(int)
    for (v, _), items in groups.items():
        counts[v] += len(items)
    print("%d inputs: %s" % (total, ", ".join("%d %s" % (counts[v], v)
                                              for v in sorted(counts, key=RANK.get))))
    if other:
        print("(%d replay lines not understood)" % other)
    for (v, sig), items in sorted(groups.items(), key=lambda kv: (RANK[kv[0][0]], -len(kv[1]))):
        if v == "ok" and not a.all:
            continue
        items.sort()
        print("\n%-12s %s  x%d" % (v, sig or "-", len(items)))
        for sz, path, insns, buckets in items[:a.examples]:
            print("    %s  (%s bytes, %d insns, %d buckets)" % (path, sz if sz >= 0 else "?", insns, buckets))
        if len(items) > a.examples:
            print("    ... %d more" % (len(items) - a.examples))
    return 0


if __name__ == "__main__":
    sys.exit(main())
