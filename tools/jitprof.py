#!/usr/bin/env python3
"""jitprof.py - where does a JIT run spend its host time? (docs/jit-perf.md)

Joins three things a run can produce:
  * perf samples          perf record -o perf.data uam-run ... --jit --jit-opmap ops.txt
  * the JIT's op map      --jit-opmap FILE (code offset -> IR instruction, region kind)
  * optionally, the IR    -t ir (stderr), to print the IR of the hottest blocks

and prints the share of samples in native code by region kind and by IR opcode, the
hottest blocks, and the hottest C functions (bridges, devices, the run loop).

Usage:
  jitprof.py --opmap ops.txt --perf perf.data [--ir ir.log] [--top 15] [--blocks 5]
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

# Pure ops the baseline JIT evaluates through a C call (jit_br_eval); kept as a category
# so a profile shows what inlining them would buy.
FLAG_OPS = {"carry", "scarry", "sborrow"}
ALU_OPS = {"add", "sub", "and", "or", "xor", "mul"}
CMP_OPS = {"eq", "ne", "ult", "ule", "slt", "sle"}
EXT_OPS = {"zext", "sext", "trunc"}
SHIFT_OPS = {"shl", "lshr", "ashr", "rotr"}
BIT_OPS = {"clz", "ctz", "popcnt", "bswap"}
MULDIV_OPS = {"mulhu", "mulhs", "udiv", "sdiv", "urem", "srem"}
CALL_OPS = {"helper", "sysrd", "syswr", "amo", "cas"}


def category(kind, text):
    """Region kind + op text ('put.8@64', 'ld.32', ...) -> a coarse category."""
    if kind != "op":
        if kind == "slow":
            return text.split(".")[0] + "-slow" if text else "slow"
        return kind
    mn = text.split(".")[0]
    if mn == "imark":
        return "imark-check"
    if mn == "put":
        return "put.8" if text.startswith("put.8") else "put"
    if mn in ("ld", "st"):
        return mn + "-fast"
    if mn in FLAG_OPS:
        return "carry/overflow"
    for name, group in (("alu", ALU_OPS), ("compare", CMP_OPS), ("extend", EXT_OPS),
                        ("shift", SHIFT_OPS), ("bit-op", BIT_OPS), ("mul/div", MULDIV_OPS),
                        ("call", CALL_OPS)):
        if mn in group:
            return name
    return mn


class Block:
    __slots__ = ("start", "size", "pc", "isa", "offs", "marks", "insns")

    def __init__(self, start, size, pc, isa):
        self.start, self.size, self.pc, self.isa = start, size, pc, isa
        self.offs, self.marks, self.insns = [], [], 0


def load_opmap(path):
    blocks = {}
    cur = None
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "B":
                cur = Block(int(p[1], 16), int(p[2], 16), int(p[3], 16), p[4])
                blocks[cur.start] = cur   # a recycled arena address: the newest wins
            elif p[0] == "M" and cur:
                kind, text = p[2], (p[4] if len(p) > 4 else "")
                cur.offs.append(int(p[1], 16))
                cur.marks.append((kind, text))
                if kind == "op" and text.startswith("imark"):
                    cur.insns += 1
    order = sorted(blocks)
    return order, blocks


def load_ir(path):
    """-t ir output -> {guest pc: text of its (last) block}."""
    ir, cur, lines = {}, None, []
    hdr = re.compile(r"^block 0x([0-9a-f]+):")
    with open(path, errors="replace") as f:
        for line in f:
            m = hdr.match(line)
            if m:
                if cur is not None:
                    ir[cur] = "".join(lines)
                cur, lines = int(m.group(1), 16), [line]
            elif cur is not None:
                if line.startswith(" ") or line.startswith("imark"):
                    lines.append(line)
                else:
                    ir[cur] = "".join(lines)
                    cur, lines = None, []
    if cur is not None:
        ir[cur] = "".join(lines)
    return ir


def samples(perf_data):
    out = subprocess.run(["perf", "script", "-i", perf_data, "-F", "ip,sym,dso"],
                         capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split(None, 1)
        if not p:
            continue
        try:
            ip = int(p[0], 16)
        except ValueError:
            continue
        rest = p[1] if len(p) > 1 else ""
        m = re.match(r"(.*?)\s*\(([^)]*)\)\s*$", rest)
        sym, dso = (m.group(1), m.group(2)) if m else (rest, "")
        yield ip, sym, dso


def pct(n, total):
    return 100.0 * n / total if total else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--opmap", required=True)
    ap.add_argument("--perf", required=True, help="perf.data of the same run")
    ap.add_argument("--ir", help="-t ir output of the same run")
    ap.add_argument("--top", type=int, default=15, help="rows per table")
    ap.add_argument("--blocks", type=int, default=5, help="hottest blocks to show the IR of")
    a = ap.parse_args()

    order, blocks = load_opmap(a.opmap)
    total = jit = 0
    by_cat, by_op, by_sym = collections.Counter(), collections.Counter(), collections.Counter()
    by_blk = collections.Counter()
    blk_cat = collections.defaultdict(collections.Counter)
    for ip, sym, dso in samples(a.perf):
        total += 1
        k = bisect.bisect_right(order, ip) - 1
        b = blocks[order[k]] if k >= 0 else None
        if b is None or ip >= b.start + b.size:
            name = sym if sym and sym != "[unknown]" else "?"
            by_sym["%s (%s)" % (name, dso.split("/")[-1])] += 1
            continue
        jit += 1
        off = ip - b.start
        m = bisect.bisect_right(b.offs, off) - 1
        kind, text = b.marks[m] if m >= 0 else ("prologue", "")
        cat = category(kind, text)
        by_cat[cat] += 1
        by_op[text.split("@")[0] if kind in ("op", "slow") else kind] += 1
        by_blk[b.start] += 1
        blk_cat[b.start][cat] += 1

    print("samples: %d   native code: %.1f%%   C and kernel: %.1f%%"
          % (total, pct(jit, total), pct(total - jit, total)))
    print("\nnative code by region (%% of all samples):")
    for cat, n in by_cat.most_common(a.top * 2):
        print("  %-16s %6.2f%%" % (cat, pct(n, total)))
    print("\nnative code by IR op (%% of all samples):")
    for op, n in by_op.most_common(a.top):
        print("  %-16s %6.2f%%" % (op, pct(n, total)))
    print("\nC / kernel functions (%% of all samples):")
    for s, n in by_sym.most_common(a.top):
        print("  %6.2f%%  %s" % (pct(n, total), s))
    print("\nhottest blocks:")
    ir = load_ir(a.ir) if a.ir else {}
    for i, (start, n) in enumerate(by_blk.most_common(a.top)):
        b = blocks[start]
        cats = ", ".join("%s %.0f%%" % (c, pct(v, n)) for c, v in blk_cat[start].most_common(4))
        print("  %6.2f%%  0x%x (%s, %d insns, %d code bytes): %s"
              % (pct(n, total), b.pc, b.isa, b.insns, b.size, cats))
    for start, n in by_blk.most_common(a.blocks):
        pc = blocks[start].pc
        if pc in ir:
            print("\n--- IR of 0x%x (%.2f%%) ---" % (pc, pct(n, total)))
            sys.stdout.write(ir[pc])


if __name__ == "__main__":
    main()
