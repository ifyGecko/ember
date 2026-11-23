#!/usr/bin/env python3
"""fuzz_frontier.py -- the coverage frontier of an ember fuzz corpus.

ember's block coverage (docs/fuzzing.md) buckets a hashed pc, so a coverage map cannot
be turned back into addresses: it says how much of the code ran, not which branch has
only ever gone one way. This tool answers that second question, which is what a helper
(a person, or an LLM guiding the queue through fuzz_brain.py's import route) needs to
write an input that reaches new code.

It replays a corpus through `uam-run -t blocks` to learn which blocks ran, reads the
firmware's control-flow graph with rizin (as fuzz/tools/gen_sites.py does), and reports
every **frontier branch**: a conditional branch whose own block ran but whose taken or
fall-through target never did. For each it prints the branch instruction and the compare
before it, the uncovered target (named), and the smallest corpus input that reaches the
branch -- the input to mutate, and the value to aim the branch at.

It changes nothing and runs beside a live campaign: it only replays the queue read-only.

Needs rizin and python3's rzpipe (pip install rzpipe), like gen_sites.py. Without them it
prints why and exits 2.

Usage:
  # replay the corpus itself (the trailing command is the uam-run replay command WITHOUT
  # the firmware positional and WITHOUT -t/--fuzz-replay: the tool adds those):
  python3 fuzz/tools/fuzz_frontier.py FIRMWARE --corpus run/queue [--range SYM]... \\
      [--module NAME:PATH]... [--all-functions] [--top N] [--json] -- \\
      build/uam-run -b rv32-ref --fuzz-begin fuzz_iteration --fuzz-input uart0

  # or parse a block trace you already captured (no replay, no trailing command):
  build/uam-run -b rv32-ref ... -t blocks --trace-file t.txt --fuzz-replay run/queue FW
  python3 fuzz/tools/fuzz_frontier.py FIRMWARE --corpus run/queue --trace t.txt --range parse

--range SYM | LO-HI | LO+LEN (repeatable) restricts both the CFG and the block trace to
those pcs (an ELF symbol's name resolves to its [addr, addr+size); hex addresses are
absolute). Without it the whole image is analysed, which on a large firmware is slow and
noisy: give the function(s) under test. --module NAME:PATH adds a module's ELF for a
multi-module image (its base comes from --base NAME=ADDR, default 0). --json prints the
records as JSON (a list), for a program to read.

Exit: 0 if it ran (frontier branches or none), 1 on a usage or replay error, 2 when rizin
or rzpipe is missing.
"""
import argparse
import bisect
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True


def die(msg, code=1):
    print("frontier: " + msg, file=sys.stderr)
    sys.exit(code)


def elf_symbols(path):
    """{name: (addr, size)} from readelf -sW (STT_FUNC and others); empty if unreadable."""
    out = {}
    try:
        txt = subprocess.run(["readelf", "-sW", path], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return out
    for line in txt.splitlines():
        f = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(f) >= 8 and f[0].endswith(":") and re.fullmatch(r"[0-9a-fA-F]+", f[1]):
            try:
                addr, size = int(f[1], 16), int(f[2], 0)
            except ValueError:
                continue
            name = f[7]
            if name and name not in out:
                out[name] = (addr, size)
    return out


def parse_ranges(specs, syms):
    """Turn --range specs into [(lo, hi)] absolute half-open intervals."""
    ranges = []
    for s in specs:
        m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)-(0x[0-9a-fA-F]+|\d+)", s)
        if m:
            ranges.append((int(m.group(1), 0), int(m.group(2), 0)))
            continue
        m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)\+(0x[0-9a-fA-F]+|\d+)", s)
        if m:
            lo = int(m.group(1), 0)
            ranges.append((lo, lo + int(m.group(2), 0)))
            continue
        # An exact symbol, else every clone the compiler made of it (GCC's
        # name.constprop.N, name.part.N, ...): --range parse covers parse.constprop.0.
        matches = ([s] if s in syms else
                   [n for n in syms if n.startswith(s + ".")])
        if matches:
            for n in matches:
                lo, size = syms[n]
                ranges.append((lo, lo + max(size, 1)))
            continue
        die("--range %r is not SYM, LO-HI or LO+LEN (and no symbol of that name)" % s)
    return ranges


def in_ranges(pc, ranges):
    return not ranges or any(lo <= pc < hi for lo, hi in ranges)


def corpus_inputs(corpus):
    """The corpus files in the order ember replays them (name order; dotfiles and .info
    sidecars skipped), as [(name, size)]."""
    out = []
    for n in sorted(os.listdir(corpus)):
        if n.startswith(".") or n.endswith(".info"):
            continue
        p = os.path.join(corpus, n)
        if os.path.isfile(p):
            out.append((n, os.path.getsize(p)))
    return out


def run_replay(cmd, corpus, firmware, ranges, trace_path):
    """Run the replay command with block tracing to trace_path. cmd is the uam-run command
    without the firmware or -t/--fuzz-replay."""
    full = list(cmd) + ["-t", "blocks", "--trace-file", trace_path,
                        "--fuzz-replay", corpus]
    for lo, hi in ranges:
        full += ["--trace-range", "0x%x:0x%x" % (lo, hi)]
    full += [firmware]
    try:
        r = subprocess.run(full, capture_output=True, text=True)
    except OSError as e:
        die("could not run the replay command (%s): %s" % (full[0], e))
    if not os.path.exists(trace_path):
        die("the replay wrote no trace file; its output was:\n" + (r.stderr or r.stdout))
    return full


def parse_trace(trace_path, ninputs):
    """Parse a `-t blocks` trace: return (covered_pcs:set, per_input:list[set]). A block
    line is `blk 0xPC ...`; `trace: input N` starts input N (1-based). Blocks before the
    first input marker (the boot) go to input 0's own set and the covered set."""
    covered = set()
    per_input = [set() for _ in range(ninputs + 1)]   # index 0: the boot
    cur = 0
    blk = re.compile(r"^blk\s+(0x[0-9a-fA-F]+)")
    inp = re.compile(r"^trace: input (\d+)")
    with open(trace_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = inp.match(line)
            if m:
                cur = int(m.group(1))
                if cur >= len(per_input):
                    per_input.extend(set() for _ in range(cur - len(per_input) + 1))
                continue
            m = blk.match(line)
            if m:
                pc = int(m.group(1), 16)
                covered.add(pc)
                per_input[cur].add(pc)
    return covered, per_input


class Block:
    __slots__ = ("addr", "size", "jump", "fail", "func", "tail")

    def __init__(self, addr, size, jump, fail, func, tail):
        self.addr, self.size, self.jump, self.fail = addr, size, jump, fail
        self.func, self.tail = func, tail

    @property
    def end(self):
        return self.addr + self.size


def load_cfg(binary, modules, bases, ranges, all_functions):
    """[(Block)] for the binary (and modules), from rizin. Each block carries its
    successors (jump/fail), its function name and the disassembly of its last few
    instructions. Only blocks that overlap --range are kept."""
    try:
        import rzpipe
    except ImportError:
        die("rzpipe is not installed (pip install rzpipe); cannot read the CFG", 2)
    if not shutil.which("rizin"):
        die("rizin is not on PATH; cannot read the CFG", 2)
    blocks = []
    for name, path, base in [("", binary, 0)] + [(n, p, bases.get(n, 0))
                                                 for n, p in modules]:
        rz = rzpipe.open(path, flags=["-2"])
        try:
            if base:
                rz.cmd("omba 0x%x" % base)        # map at the module's base
            rz.cmd("aaa")
            funcs = rz.cmdj("aflj") or []
            if not funcs:
                continue
            want = []
            for fn in funcs:
                off = fn.get("offset", 0)
                if not all_functions and fn.get("name", "").startswith("fcn."):
                    continue                      # analysis-only function: opt in
                if ranges and not any(lo < off + fn.get("size", 0) and off < hi
                                      for lo, hi in ranges):
                    continue
                want.append(fn)
            for fn in want:
                fname = fn.get("name", "fcn")
                bbs = rz.cmdj("afbj @ 0x%x" % fn["offset"]) or []
                for b in bbs:
                    addr, size = b.get("addr"), b.get("size", 0)
                    if addr is None or not size:
                        continue
                    if ranges and not in_ranges(addr, ranges):
                        continue
                    jump = b.get("jump")
                    fail = b.get("fail")
                    ops = rz.cmdj("pdbj @ 0x%x" % addr) or []
                    tail = [(o.get("offset"), o.get("disasm", "")) for o in ops[-3:]]
                    blocks.append(Block(addr, size, jump, fail, fname, tail))
        finally:
            rz.quit()
    return blocks


def symbolize(pc, syms_by_addr):
    """name+offset for a pc, from the nearest ELF symbol at or below it."""
    i = bisect.bisect_right(syms_by_addr, (pc, chr(0x10ffff))) - 1
    if i >= 0:
        saddr, sname = syms_by_addr[i]
        return "%s+0x%x" % (sname, pc - saddr) if pc != saddr else sname
    return "0x%x" % pc


def main():
    ap = argparse.ArgumentParser(description="report the coverage frontier of an ember "
                                 "fuzz corpus (uncovered branch targets).")
    ap.add_argument("firmware", help="the firmware ELF (module 0)")
    ap.add_argument("--corpus", required=True, help="directory of inputs (e.g. run/queue)")
    ap.add_argument("--trace", help="parse this block trace instead of replaying the corpus")
    ap.add_argument("--range", action="append", default=[], dest="ranges",
                    metavar="SYM|LO-HI|LO+LEN", help="restrict to these pcs (repeatable)")
    ap.add_argument("--module", action="append", default=[], metavar="NAME:PATH",
                    help="an extra module's ELF (repeatable)")
    ap.add_argument("--base", action="append", default=[], metavar="NAME=ADDR",
                    help="a module's load base (default 0; repeatable)")
    ap.add_argument("--all-functions", action="store_true",
                    help="include functions rizin found only by analysis (fcn.*)")
    ap.add_argument("--top", type=int, default=40,
                    help="show at most N frontier branches (default 40; 0 = all)")
    ap.add_argument("--json", action="store_true", help="print the records as JSON")
    ap.epilog = ("After the options, give -- then the uam-run replay command (no firmware, "
                 "no -t/--fuzz-replay). Omit it with --trace.")
    # argparse.REMAINDER is greedy and would swallow our own options, so split on -- by hand.
    argv = sys.argv[1:]
    if "--" in argv:
        i = argv.index("--")
        pre, cmd = argv[:i], argv[i + 1:]
    else:
        pre, cmd = argv, []
    args = ap.parse_args(pre)

    if not os.path.isdir(args.corpus):
        die("--corpus %r is not a directory" % args.corpus)
    if not args.trace and not cmd:
        die("give either --trace FILE or, after --, the uam-run replay command")

    syms = elf_symbols(args.firmware)
    ranges = parse_ranges(args.ranges, syms)
    modules = []
    for m in args.module:
        name, _, path = m.partition(":")
        if not path:
            die("--module must be NAME:PATH, got %r" % m)
        modules.append((name, path))
    bases = {}
    for b in args.base:
        name, _, addr = b.partition("=")
        bases[name] = int(addr, 0)

    inputs = corpus_inputs(args.corpus)
    if not inputs:
        die("the corpus is empty (no inputs under %r)" % args.corpus)

    tmp = None
    try:
        if args.trace:
            trace_path = args.trace
        else:
            tmp = tempfile.NamedTemporaryFile(prefix="frontier-", suffix=".trace",
                                              delete=False)
            tmp.close()
            trace_path = tmp.name
            run_replay(cmd, args.corpus, args.firmware, ranges, trace_path)
        covered, per_input = parse_trace(trace_path, len(inputs))
    finally:
        if tmp:
            try:
                os.unlink(tmp.name)
            except OSError:
                pass

    if not covered:
        die("the trace shows no blocks (wrong --fuzz-begin, or nothing ran); "
            "check the replay command")

    blocks = load_cfg(args.firmware, modules, bases, ranges, args.all_functions)
    if not blocks:
        die("rizin found no functions in range; widen --range or pass --all-functions")

    cov_sorted = sorted(covered)
    covered_starts = set()

    def block_covered(b):
        i = bisect.bisect_left(cov_sorted, b.addr)
        return i < len(cov_sorted) and cov_sorted[i] < b.end

    for b in blocks:
        if block_covered(b):
            covered_starts.add(b.addr)

    # input index -> covered pc set; input 0 is the boot, inputs 1..N are the corpus.
    def reaching(b):
        """corpus inputs (name, size) whose run entered block b, smallest first."""
        hit = []
        for idx, (name, size) in enumerate(inputs, 1):
            s = per_input[idx] if idx < len(per_input) else set()
            if any(b.addr <= pc < b.end for pc in s):
                hit.append((size, name))
        hit.sort()
        return hit

    syms_by_addr = sorted((a, n) for n, (a, _sz) in syms.items())

    records = []
    for b in blocks:
        if b.addr not in covered_starts:
            continue
        # Only conditional branches (a block with both a taken target and a fall-through)
        # are frontier candidates: for those, an uncovered side is a branch to aim at. A
        # block with one successor is unconditional flow; an uncovered target there is code
        # never reached at all, not a branch whose condition we can try to flip.
        if b.fail is None or b.jump is None:
            continue
        succs = [(b.fail, "fall-through"), (b.jump, "taken")]
        frontier = [(t, kind) for t, kind in succs if t not in covered_starts]
        if not frontier:
            continue
        hit = reaching(b)
        for target, kind in frontier:
            records.append({
                "branch": b.addr,
                "branch_sym": symbolize(b.addr, syms_by_addr),
                "func": b.func,
                "target": target,
                "target_sym": symbolize(target, syms_by_addr),
                "edge": kind,
                "disasm": [d for _a, d in b.tail],
                "reached_by": len(hit),
                "smallest_input": hit[0][1] if hit else None,
                "smallest_size": hit[0][0] if hit else None,
            })

    # Most useful first: branches some input already reaches (so a mutation can flip them),
    # the smallest input first.
    records.sort(key=lambda r: (r["reached_by"] == 0, r["smallest_size"] if r["reached_by"]
                                else 1 << 62, r["branch"]))
    shown = records if not args.top else records[:args.top]

    if args.json:
        print(json.dumps(shown, indent=2))
        return

    cov_blocks = sum(1 for b in blocks if b.addr in covered_starts)
    print("frontier: %d/%d CFG blocks covered, %d frontier branch target(s)%s"
          % (cov_blocks, len(blocks), len(records),
             "" if len(shown) == len(records) else " (showing %d)" % len(shown)))
    for r in shown:
        reach = ("reached by %d input(s); smallest %s (%d B)"
                 % (r["reached_by"], r["smallest_input"], r["smallest_size"])
                 if r["reached_by"] else "NOT reached by any corpus input")
        print("\n  %s (0x%x) in %s -- %s target uncovered:"
              % (r["branch_sym"], r["branch"], r["func"], r["edge"]))
        print("    -> %s (0x%x)" % (r["target_sym"], r["target"]))
        for d in r["disasm"]:
            print("       %s" % d)
        print("    %s" % reach)
    if not records:
        print("\n(no frontier branches: every conditional in range has gone both ways)")


if __name__ == "__main__":
    main()
