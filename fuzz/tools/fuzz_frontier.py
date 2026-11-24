#!/usr/bin/env python3
"""fuzz_frontier.py -- the coverage frontier of an ember fuzz corpus.

ember's block coverage (docs/fuzzing.md) buckets a hashed pc, so a coverage map cannot
be turned back into addresses: it says how much of the code ran, not which branch has
only ever gone one way. This tool answers that second question, which is what a helper
(a person, or an LLM guiding the queue through fuzz_brain.py's import route) needs to
write an input that reaches new code.

It replays a corpus through `uam-run -t blocks` to learn which blocks ran, reads the
firmware's control-flow graph with rizin (as fuzz/tools/gen_sites.py does), and reports
every **frontier branch**: a conditional branch (or a switch's jump table) whose own block
ran but one of whose targets never did. For each it prints the last instructions of the
branch's block (usually the compare and the constant it tests), the uncovered target
(named; for a switch, the case value), and the smallest corpus input that reaches the
branch -- the input to mutate.

ember's blocks are not rizin's: an ember block runs on past a point another branch jumps to
(a loop head, the join after an `if`), so its trace line names only where it began. A rizin
block is counted as run when a traced pc lies inside it, or when a block that ran flows
into it with no branch of its own.

It changes nothing and runs beside a live campaign: it copies the corpus first and replays
the copy, so entries the campaign adds meanwhile cannot shift which input is which.

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
those pcs (an ELF symbol's name resolves to its [addr, addr+size), with an ARM Thumb
symbol's bit 0 cleared; hex addresses are absolute). Without it the whole image is analysed, which on a large firmware is slow and
noisy: give the function(s) under test. --module NAME:PATH adds a module's ELF for a
multi-module image (its base comes from --base NAME=ADDR, default 0). --json prints the
records as JSON (a list), for a program to read. With --trace, the corpus must be the one
the trace was made from, unchanged: inputs are matched to the trace's `trace: input N`
sections in ember's replay order (every regular file whose name does not start with '.',
by name).

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
    """{name: (addr, size)} from readelf -sW (STT_FUNC and others); empty if unreadable.
    On ARM a Thumb function's symbol has bit 0 set; it is cleared here, so the address is
    the function's first instruction, as in rizin's CFG."""
    out = {}
    try:
        txt = subprocess.run([os.environ.get("READELF", "readelf"), "-sW", path], capture_output=True, text=True,
                             check=True).stdout
        hdr = subprocess.run([os.environ.get("READELF", "readelf"), "-hW", path], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return out
    arm = re.search(r"Machine:\s+ARM\b", hdr) is not None
    for line in txt.splitlines():
        f = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(f) >= 8 and f[0].endswith(":") and re.fullmatch(r"[0-9a-fA-F]+", f[1]):
            try:
                addr, size = int(f[1], 16), int(f[2], 0)
            except ValueError:
                continue
            name = f[7]
            if arm and f[3] == "FUNC":
                addr &= ~1
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
    """The corpus files in the order ember replays a directory (every regular file whose
    name does not start with '.', by name), as [(name, size)]. A board may declare
    sidecars that ember skips; a replay (not --trace) learns the real list from ember."""
    out = []
    for n in sorted(os.listdir(corpus)):
        if n.startswith("."):
            continue
        p = os.path.join(corpus, n)
        if os.path.isfile(p):
            out.append((n, os.path.getsize(p)))
    return out


def snapshot_corpus(corpus, dest):
    """Copy the corpus's inputs into dest, so a live campaign adding entries cannot change
    what is replayed. The brain's .info sidecars are not inputs and are left out."""
    for n in sorted(os.listdir(corpus)):
        if n.startswith(".") or n.endswith(".info"):
            continue
        p = os.path.join(corpus, n)
        try:
            if os.path.isfile(p):
                shutil.copyfile(p, os.path.join(dest, n))
        except OSError:
            pass                       # removed meanwhile (a trimmed entry): skip it


def run_replay(cmd, corpus, firmware, ranges, trace_path):
    """Run the replay command with block tracing to trace_path. cmd is the uam-run command
    without the firmware or -t/--fuzz-replay. Returns [(name, size)] of the inputs in the
    order ember ran them (input N of the trace is element N-1), from its `replay:` lines."""
    full = list(cmd) + ["-t", "blocks", "--trace-file", trace_path,
                        "--fuzz-replay", corpus]
    for lo, hi in ranges:
        full += ["--trace-range", "0x%x:0x%x" % (lo, hi)]
    full += [firmware]
    try:
        r = subprocess.run(full, capture_output=True, text=True, errors="replace")
    except OSError as e:
        die("could not run the replay command (%s): %s" % (full[0], e))
    if not os.path.exists(trace_path):
        die("the replay wrote no trace file (exit %d); its output was:\n%s"
            % (r.returncode, r.stderr or r.stdout))
    prefix = "replay: " + corpus.rstrip("/") + "/"
    ran = []
    for line in r.stdout.splitlines():
        if line.startswith(prefix):
            n = line[len(prefix):].split(": ", 1)[0]
            ran.append((n, os.path.getsize(os.path.join(corpus, n))))
    if not ran:
        die("the replay ran no input (exit %d); its output was:\n%s"
            % (r.returncode, r.stderr or r.stdout))
    return ran


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


UNTYPED = []     # (addr, disasm) of instructions in range rizin's analysis gave no type


class Block:
    __slots__ = ("addr", "size", "jump", "fail", "cases", "func", "tail")

    def __init__(self, addr, size, jump, fail, cases, func, tail):
        self.addr, self.size, self.jump, self.fail = addr, size, jump, fail
        self.cases = cases             # [(value, target)] of a switch's jump table
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
            # A rizin built without the arch's disassembler plugin (Homebrew's has no
            # RISC-V) loads the ELF but keeps another asm.arch, and decodes garbage.
            bin_arch = ((rz.cmdj("ij") or {}).get("bin") or {}).get("arch")
            asm_arch = rz.cmd("e asm.arch").strip()
            if bin_arch and asm_arch != bin_arch:
                die("%s: this rizin has no disassembler for %r (built without its "
                    "plugin; `rz-asm -L` lists those it has)" % (path, bin_arch), 2)
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
                    sw = b.get("switch_op") or {}
                    cases = [(c.get("value"), c["jump"]) for c in sw.get("cases") or []
                             if c.get("jump") is not None]
                    ops = rz.cmdj("pdbj @ 0x%x" % addr) or []
                    UNTYPED.extend((o.get("offset"), o.get("disasm", "")) for o in ops
                                   if o.get("type") == "null")
                    tail = [(o.get("offset"), o.get("disasm", "")) for o in ops[-3:]]
                    blocks.append(Block(addr, size, jump, fail, cases, fname, tail))
        finally:
            rz.quit()
    return blocks


def covered_blocks(blocks, pcs):
    """The starts of the blocks that ran, given the traced pcs (sorted). A block ran when a
    traced pc lies inside it, or when a block that ran flows into it with no branch of its
    own (one successor: no fall-through/taken pair, no jump table). An ember block runs on
    past a point another branch jumps to (a loop head, the join after an `if`), so its trace
    line names only where it began, and the rizin blocks it ran through after that have no
    line of their own."""
    by_addr = {}
    for b in blocks:
        by_addr.setdefault(b.addr, b)
    got = set()
    for b in blocks:
        i = bisect.bisect_left(pcs, b.addr)
        if i < len(pcs) and pcs[i] < b.end:
            got.add(b.addr)
    work = list(got)
    while work:
        b = by_addr[work.pop()]
        if b.fail is None and not b.cases and b.jump is not None:
            t = b.jump
            if t in by_addr and t not in got:
                got.add(t)
                work.append(t)
    return got


def frontier_records(blocks, covered_starts, reaching, syms_by_addr):
    """The frontier records: for each block that ran and ends in a conditional branch or a
    switch, one per target that never ran. reaching(b) gives the [(size, name)] of the
    inputs that ran b, smallest first."""
    records = []
    for b in blocks:
        if b.addr not in covered_starts:
            continue
        # Frontier candidates: a conditional branch (a block with both a taken target and a
        # fall-through) and a switch's jump table. An uncovered target there is a branch to
        # aim at. A block with one successor is unconditional flow (counted as run above).
        if b.cases:
            succs = [(t, "case %s" % v, v) for v, t in b.cases]
        elif b.fail is not None and b.jump is not None:
            succs = [(b.fail, "fall-through", None), (b.jump, "taken", None)]
        else:
            continue
        seen = set()
        frontier = []
        for t, kind, v in succs:
            if t not in covered_starts and t not in seen:
                seen.add(t)
                frontier.append((t, kind, v))
        if not frontier:
            continue
        hit = reaching(b)
        for target, kind, value in frontier:
            rec = {
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
            }
            if value is not None:
                rec["case_value"] = value
            records.append(rec)

    return records


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

    tmpdir = tempfile.mkdtemp(prefix="frontier-")
    try:
        if args.trace:
            trace_path = args.trace
            inputs = corpus_inputs(args.corpus)
        else:
            snap = os.path.join(tmpdir, "corpus")
            os.mkdir(snap)
            snapshot_corpus(args.corpus, snap)
            if not os.listdir(snap):
                die("the corpus is empty (no inputs under %r)" % args.corpus)
            trace_path = os.path.join(tmpdir, "blocks.trace")
            inputs = run_replay(cmd, snap, args.firmware, ranges, trace_path)
        if not inputs:
            die("the corpus is empty (no inputs under %r)" % args.corpus)
        covered, per_input = parse_trace(trace_path, len(inputs))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if not covered:
        die("the trace shows no blocks (wrong --fuzz-begin, --range, or nothing ran); "
            "check the replay command")

    blocks = load_cfg(args.firmware, modules, bases, ranges, args.all_functions)
    if not blocks:
        die("rizin found no functions in range; widen --range or pass --all-functions")
    # An untyped instruction naming an address in range is a branch rizin did not
    # recognise (rizin 0.9's PowerPC analysis leaves beq/bne untyped): the blocks run on
    # through it, and it never shows as a frontier target.
    spans = ranges or [(min(b.addr for b in blocks), max(b.end for b in blocks))]
    missed = [(off, dis) for off, dis in UNTYPED
              if any(lo <= int(h, 16) < hi for h in re.findall(r"0x[0-9a-fA-F]+", dis)
                     for lo, hi in spans)]
    if missed:
        print("frontier: rizin's analysis left %d branch(es) in range untyped (%s at "
              "0x%x first): its CFG misses them, so the report may be incomplete"
              % (len(missed), missed[0][1], missed[0][0]), file=sys.stderr)

    covered_starts = covered_blocks(blocks, sorted(covered))
    # input index -> blocks it ran; 1..N are the corpus inputs, as the trace numbers them.
    reached = [covered_blocks(blocks, sorted(per_input[i])) if i < len(per_input) else set()
               for i in range(len(inputs) + 1)]

    def reaching(b):
        """corpus inputs (size, name) whose run entered block b, smallest first."""
        hit = [(size, name) for idx, (name, size) in enumerate(inputs, 1)
               if b.addr in reached[idx]]
        hit.sort()
        return hit

    syms_by_addr = sorted((a, n) for n, (a, _sz) in syms.items())

    records = frontier_records(blocks, covered_starts, reaching, syms_by_addr)

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
    if not records and not cov_blocks:
        print("\n(no CFG block in range ran: check --range and the replay command)")
    elif not records:
        print("\n(no frontier branches: every conditional and switch in range has gone "
              "every way)")


if __name__ == "__main__":
    main()
