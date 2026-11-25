#!/usr/bin/env python3
"""fuzz_frontier.py -- the coverage frontier of an ember fuzz corpus.

ember's block coverage (docs/fuzzing.md) buckets a hashed pc, so a coverage map cannot
be turned back into addresses: it says how much of the code ran, not which branch has
only ever gone one way. This tool answers that second question, which is what a helper
(a person, or an LLM guiding the queue through fuzz_brain.py's import route) needs to
write an input that reaches new code.

It replays a corpus through `uam-run -t blocks` to learn which blocks ran, reads the
firmware's control-flow graph, and reports every **frontier branch**: a conditional branch
(or a switch's jump table) whose own block ran but one of whose targets never did. For each
it prints the last instructions of the branch's block (usually the compare and the constant
it tests), the uncovered target (named; for a switch, the case value), and the smallest
corpus input that reaches the branch -- the input to mutate.

ember's blocks are not the disassembler's: an ember block runs on past a point another
branch jumps to (a loop head, the join after an `if`), so its trace line names only where it
began. A CFG block is counted as run when a traced pc lies inside it, or when a block that
ran flows into it with no branch of its own.

It changes nothing and runs beside a live campaign: it copies the corpus first and replays
the copy, so entries the campaign adds meanwhile cannot shift which input is which.

## The firmware image

The image is any ember firmware, given the same way as to ember and the other fuzz tools:

  an ELF           just the path (FIRMWARE). The analyser's own loader reads it.
  a raw image      --raw --base ADDR --arch A --bits N --endian big|little [--cpu C], the
                   same flags gen_sites.py takes. The code runs at --base. --symbols FILE
                   names its functions (nm / "ADDR NAME" / a Ghidra CSV), as gen_sites.py.
  extra modules    --module NAME:PATH (another ELF) and --base NAME=ADDR (its load base).

So a bare-metal dump loads directly: no turning it into an ELF first.

## Where the CFG comes from (--cfg-from, default auto)

  rizin            the `rizin` program over its own pipe (no rzpipe module needed).
  ghidra           headless Ghidra importing the image (ExportCfg.java). Reads raw images,
                   ARM BE8, AVR32, 8-bit PIC and MIPS16e/microMIPS that rizin cannot.
  ghidra-project   a program already analysed in a Ghidra project (--ghidra-project
                   PATH.gpr [--ghidra-program NAME]); the ember-dev-recon project, or your
                   own. No re-analysis.
  a CFG file       --cfg FILE: a JSON CFG (fuzz/tools/frontier_cfg.py's format) that
                   anything wrote -- this tool's --save-cfg, ExportCfg.java from the Ghidra
                   GUI, or your own exporter. The image flags are then not needed.
  auto (default)   a CFG file if --cfg is given, else rizin if it can read the image, else
                   headless Ghidra if one is found. --cfg-from names one explicitly.

No loader option is out of reach: --rizin-arg / --rizin-cmd and --ghidra-arg pass straight
through to that tool, and --cfg takes a graph made any other way.

Usage:
  # replay the corpus (the trailing command is the uam-run replay command WITHOUT the
  # firmware positional and WITHOUT -t/--fuzz-replay: the tool adds those):
  python3 fuzz/tools/fuzz_frontier.py FIRMWARE --corpus run/queue [--range SYM]... -- \\
      build/uam-run -b rv32-ref --fuzz-begin fuzz_iteration --fuzz-input uart0

  # a raw image, CFG from headless Ghidra:
  python3 fuzz/tools/fuzz_frontier.py fw.bin --raw --base 0x8000000 --arch arm --bits 32 \\
      --endian little --cfg-from ghidra --corpus run/queue --range 0x8000100-0x8000400 -- \\
      build/uam-run -b my-ref --raw-base 0x8000000 --fuzz-begin 0x8000101 --fuzz-input uart0

  # or parse a block trace you already captured (no replay, no trailing command):
  build/uam-run -b rv32-ref ... -t blocks --trace-file t.txt --fuzz-replay run/queue FW
  python3 fuzz/tools/fuzz_frontier.py FIRMWARE --corpus run/queue --trace t.txt --range parse

--range SYM | LO-HI | LO+LEN (repeatable) restricts both the CFG and the block trace to
those pcs (an ELF symbol's name resolves to its [addr, addr+size), with an ARM Thumb
symbol's bit 0 cleared; hex addresses are absolute). Without it the whole image is analysed,
which on a large firmware is slow and noisy: give the function(s) under test. --json prints
the records as JSON (a list), for a program to read. With --trace, the corpus must be the
one the trace was made from, unchanged: inputs are matched to the trace's `trace: input N`
sections in ember's replay order (every regular file whose name does not start with '.', by
name).

Exit: 0 if it ran (frontier branches or none), 1 on a usage or replay error, 2 when no CFG
source works (no disassembler for the machine, or none installed).
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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frontier_cfg as fc  # noqa: E402

Block = fc.Block


def die(msg, code=1):
    print("frontier: " + msg, file=sys.stderr)
    sys.exit(code)


def elf_symbols(path):
    """{name: (addr, size)} of an ELF's function and object symbols (bit 0 of an ARM/MIPS
    function cleared); empty for a raw image or an unreadable file."""
    return fc.elf_symbols(path)[0]


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


def build_image(path, raw, base, arch, bits, endian, cpu, processor, cspec, map_base=0,
                extra_syms=None):
    """An fc.ImageSpec for a module, with its ELF symbols (addresses at the module's load
    base) as analysis seeds and names. For a raw image, --symbols supplies the names."""
    syms = {} if raw else {a + map_base: n for n, (a, _s) in elf_symbols(path).items()}
    if extra_syms:
        for a, n in extra_syms.items():
            syms.setdefault(a + map_base, n)
    # Seed analysis only at real function entries (an ELF's STT_FUNC + entry point, or a raw
    # image's --symbols addresses), never at data symbols.
    if raw:
        entries = sorted(syms)
    else:
        try:
            import image_resolve
            with open(path, "rb") as f:
                found = image_resolve.elf_code_entries(f.read())
            entries = sorted((a + map_base) for a in (found[1] if found else ()))
        except (OSError, ImportError):
            entries = []
    return fc.ImageSpec(path, raw=raw, base=base, arch=arch, bits=bits, endian=endian,
                        cpu=cpu, processor=processor, cspec=cspec,
                        entries=entries, symbols=syms)


def load_cfg(args, firmware, modules, bases, ranges):
    """The CFG of the firmware (and modules), from the source --cfg-from chooses. Each
    block carries its successors, its function name and the disassembly of its last few
    instructions. Only blocks that start inside --range are kept."""
    if args.cfg:
        cfg = fc.load_file(args.cfg)
        cfg.restrict(ranges)
        return cfg

    raw_syms = load_symbols_file(args.symbols) if (args.raw and args.symbols) else None
    specs = [(build_image(firmware, args.raw, args.base_addr, args.arch, args.bits,
                          args.endian, args.cpu, args.processor, args.cspec,
                          extra_syms=raw_syms), 0)]
    for name, path in modules:
        mb = bases.get(name, 0)
        specs.append((build_image(path, False, None, None, None, None, None,
                                  args.processor, args.cspec, map_base=mb), mb))

    order = {"rizin": ["rizin"], "ghidra": ["ghidra"],
             "ghidra-project": ["ghidra-project"],
             "auto": (["ghidra-project"] if args.ghidra_project else ["rizin", "ghidra"])}
    sources = order[args.cfg_from]
    if args.cfg_from == "ghidra-project" and not args.ghidra_project:
        die("--cfg-from ghidra-project needs --ghidra-project PATH.gpr", 1)

    tried = []
    for src in sources:
        try:
            cfg = fc.Cfg()
            for spec, mb in specs:
                if src == "rizin":
                    sub = fc.rizin_cfg(spec, ranges=([(lo - mb, hi - mb) for lo, hi in ranges]
                                                     if mb else ranges),
                                       all_functions=args.all_functions, map_base=mb,
                                       extra_args=args.rizin_arg, extra_cmds=args.rizin_cmd)
                    if mb:
                        sub.slide(mb)
                elif src == "ghidra":
                    sub = fc.ghidra_cfg(spec, ranges=ranges, home=args.ghidra_home,
                                        extra_args=args.ghidra_arg, map_base=mb or None)
                else:  # ghidra-project
                    sub = fc.ghidra_project_cfg(args.ghidra_project, args.ghidra_program,
                                                ranges=ranges, home=args.ghidra_home,
                                                extra_args=args.ghidra_arg)
                cfg.merge(sub)
            cfg.restrict(ranges)
            if args.save_cfg:
                fc.save_file(cfg, args.save_cfg)
                print("frontier: wrote the CFG to %s" % args.save_cfg, file=sys.stderr)
            return cfg
        except fc.Unusable as e:
            tried.append("%s: %s" % (src, e))
    code = 2 if args.cfg_from == "auto" else 1
    die("could not read the firmware's CFG:\n  " + "\n  ".join(tried)
        + "\n(choose a source with --cfg-from, or give a CFG file with --cfg)", code)


def covered_blocks(blocks, pcs):
    """The starts of the blocks that ran, given the traced pcs (sorted). A block ran when a
    traced pc lies inside it, or when a block that ran flows into it with no branch of its
    own (one successor: no fall-through/taken pair, no jump table). An ember block runs on
    past a point another branch jumps to (a loop head, the join after an `if`), so its trace
    line names only where it began, and the CFG blocks it ran through after that have no
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
    ap.add_argument("firmware", help="the firmware image (module 0): an ELF, or a raw "
                    "image with --raw")
    ap.add_argument("--corpus", required=True, help="directory of inputs (e.g. run/queue)")
    ap.add_argument("--trace", help="parse this block trace instead of replaying the corpus")
    ap.add_argument("--range", action="append", default=[], dest="ranges",
                    metavar="SYM|LO-HI|LO+LEN", help="restrict to these pcs (repeatable)")
    ap.add_argument("--module", action="append", default=[], metavar="NAME:PATH",
                    help="an extra module's ELF (repeatable)")
    ap.add_argument("--base", action="append", default=[], metavar="ADDR|NAME=ADDR",
                    help="the raw image's load address (ADDR), or an extra module's load "
                         "base (NAME=ADDR; default 0). Repeatable")
    ap.add_argument("--all-functions", action="store_true",
                    help="include functions the analyser found only by itself (not at a "
                         "symbol)")
    ap.add_argument("--top", type=int, default=40,
                    help="show at most N frontier branches (default 40; 0 = all)")
    ap.add_argument("--json", action="store_true", help="print the records as JSON")

    g = ap.add_argument_group("raw image (module 0 without an ELF header)")
    g.add_argument("--raw", action="store_true",
                   help="the firmware is a raw image (needs --base, --arch, --bits, --endian)")
    g.add_argument("--arch", metavar="ARCH",
                   help="arch name: mips, arm, riscv, ppc, x86, avr, xtensa, 8051, z80, "
                        "6502, pic, ...")
    g.add_argument("--bits", type=int, choices=[8, 16, 32, 64])
    g.add_argument("--endian", choices=["big", "little"])
    g.add_argument("--cpu", metavar="CPU", help="rizin asm.cpu for an arch with variants")
    g.add_argument("--symbols", metavar="FILE",
                   help="name the raw image's functions (nm / 'ADDR NAME' / Ghidra CSV)")

    c = ap.add_argument_group("where the CFG comes from")
    c.add_argument("--cfg-from", choices=["auto", "rizin", "ghidra", "ghidra-project"],
                   default="auto", help="the CFG source (default: auto -- rizin, else "
                   "headless Ghidra)")
    c.add_argument("--cfg", metavar="FILE",
                   help="read the CFG from a JSON file (frontier_cfg.py's format) instead "
                        "of running a disassembler")
    c.add_argument("--save-cfg", metavar="FILE",
                   help="also write the CFG this run built to FILE (reuse it with --cfg)")
    c.add_argument("--ghidra-project", metavar="PATH",
                   help="for --cfg-from ghidra-project: the .gpr (or dir/name) of an "
                        "analysed project (ember-dev-recon's, or your own)")
    c.add_argument("--ghidra-program", metavar="NAME",
                   help="which program of the project (default: the only one)")
    c.add_argument("--ghidra-home", metavar="DIR", help="the Ghidra install to use")
    c.add_argument("--processor", metavar="LANGID",
                   help="Ghidra language id for a raw image (e.g. ARM:LE:32:Cortex), when "
                        "the default from --arch is not right")
    c.add_argument("--cspec", metavar="ID", help="Ghidra compiler-spec id, if needed")
    c.add_argument("--rizin-arg", action="append", default=[], metavar="ARG",
                   help="pass ARG on rizin's command line (repeatable)")
    c.add_argument("--rizin-cmd", action="append", default=[], metavar="CMD",
                   help="run CMD in rizin before analysis, e.g. 'ahb 16 @ 0x8000' "
                        "(repeatable)")
    c.add_argument("--ghidra-arg", action="append", default=[], metavar="ARG",
                   help="pass ARG on analyzeHeadless's command line (repeatable)")

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
    bases = {}
    args.base_addr = None
    for b in args.base:
        if "=" in b:
            name, _, addr = b.partition("=")
            bases[name] = int(addr, 0)
        elif args.base_addr is None:
            args.base_addr = int(b, 0)
        else:
            die("--base ADDR (the raw image's load address) was given twice")
    if args.raw:
        missing = [n for n, v in (("--base", args.base_addr), ("--arch", args.arch),
                                  ("--bits", args.bits), ("--endian", args.endian))
                   if v is None]
        if missing and not args.cfg:
            die("--raw needs " + ", ".join(missing) + " (or give a ready CFG with --cfg)")

    syms = elf_symbols(args.firmware)
    if args.symbols:
        for a, n in load_symbols_file(args.symbols).items():
            syms.setdefault(n, (a, 1))
    ranges = parse_ranges(args.ranges, syms)
    modules = []
    for m in args.module:
        name, _, path = m.partition(":")
        if not path:
            die("--module must be NAME:PATH, got %r" % m)
        modules.append((name, path))
        for n, (a, s) in elf_symbols(path).items():
            syms.setdefault(n, (a, s))
    # a module's symbols need its load base for --range to find them by name
    for name, path in modules:
        mb = bases.get(name, 0)
        if mb:
            for n, (a, s) in elf_symbols(path).items():
                syms.setdefault(n, (a + mb, s))

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

    cfg = load_cfg(args, args.firmware, modules, bases, ranges)
    blocks = cfg.blocks
    if not blocks:
        die("the CFG has no block in range; widen --range or pass --all-functions", 1)
    # An untyped instruction naming an address in range is a branch the analyser did not
    # recognise (rizin 0.9's PowerPC analysis leaves beq/bne untyped): the blocks run on
    # through it, and it never shows as a frontier target.
    spans = ranges or [(min(b.addr for b in blocks), max(b.end for b in blocks))]
    missed = [(off, dis) for off, dis in cfg.untyped
              if off is not None and any(lo <= int(h, 16) < hi
                                         for h in re.findall(r"0x[0-9a-fA-F]+", dis)
                                         for lo, hi in spans)]
    if missed:
        print("frontier: the analyser left %d branch(es) in range untyped (%s at "
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

    # Symbol table for naming: ELF symbols, plus the CFG's own function names where the ELF
    # gave none (a raw image, or a function only the analyser found).
    named = dict((a, n) for n, (a, _sz) in syms.items())
    for n, a, _s in cfg.functions:
        named.setdefault(a, n)
    syms_by_addr = sorted(named.items())

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


def load_symbols_file(path):
    """{addr: name} from an nm / 'ADDR NAME' / Ghidra-CSV file (for --symbols on a raw
    image). Reuses the generators' parser."""
    try:
        import image_resolve
        return image_resolve.load_symbols(path)
    except SystemExit:
        raise
    except Exception as e:
        die("--symbols %s: %s" % (path, e))


if __name__ == "__main__":
    main()
