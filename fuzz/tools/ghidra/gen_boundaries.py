## -*- coding: utf-8 -*-
## Ghidra Jython 2.7 port of fuzz/tools/gen_boundaries.py
#@runtime Jython
##@category ember
##@keybinding
##@menupath
##@toolbar
"""
gen_boundaries.py -- write an ember boundary file (UAMB) from the program
open in Ghidra: the markers that say where a fuzz iteration begins and
ends. Jython 2.7; run as a Ghidra script.

The Ghidra counterpart of fuzz/tools/gen_boundaries.py and the sibling of
gen_sites.py here, whose pick_entry / pick_terminator it uses: each named
function's marker is the site gen_sites.py has for the function's first
basic block under the same --strategy, so for the same image and options it
writes the same file as the rizin tool wherever the two site lists agree on
that block. Pass --sites to check every marker against a site list. A
marker in ARM Thumb or MIPS16e/microMIPS code gets the alternate-state
flag. Offsets are from module 0's link base, also for a
position-independent ELF that Ghidra loaded at an image base of its own.

As with gen_sites.py, there is no positional <binary> (module 0 is the
open program); the first positional is OUTPUT.

Headless:
  analyzeHeadless <proj_dir> <proj_name> -process fw.elf \\
      -scriptPath fuzz/tools/ghidra \\
      -postScript gen_boundaries.py fw.bounds fuzz_iteration:begin \\
          --sites fw.sites
"""
from __future__ import print_function

import argparse
import os
import struct
import sys

from ghidra.program.model.block import BasicBlockModel


def _script_dir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        return str(getSourceFile().getParentFile().getAbsolutePath())


# This directory first, so its gen_sites / image_resolve (not the rizin
# tools') are the ones imported.
_HERE = _script_dir()
sys.dont_write_bytecode = True  # no $py.class files next to the scripts
if _HERE in sys.path:
    sys.path.remove(_HERE)
sys.path.insert(0, _HERE)

from image_resolve import NotFound, ProgramImage, add_symbols_arg, \
    load_symbols, resolve_to_offset, run_script
from gen_sites import (SITES_HEADER, SITES_MAGIC, SITES_RECORD, addr_of,
                       check_strategy, default_strategy, ensure_functions,
                       function_blocks, get_all_functions, get_bin_info,
                       monitor_or_dummy, pick_entry, pick_terminator,
                       site_flags, split_block, starts_in_delay_slot, uoff,
                       _func_bits, _text)

# Boundary file layout (fuzz/formats.h, docs/fuzz-formats.md).
BOUNDS_MAGIC = b"UAMB"
BOUNDS_VERSION = 1
BOUNDS_HEADER = struct.Struct("<4sIII")
BOUNDS_RECORD = struct.Struct("<QBBB5s")
assert BOUNDS_HEADER.size == 16 and BOUNDS_RECORD.size == 16

KIND_MAP = {"begin": 0, "end": 1}
DEPTH_MAP = {"any": 0, "zero": 1}


def get_function_addrs(program, image):
    """Return ({name: vaddr}, all_fns): every symbol-table name as a
    fallback dict, and the analysed functions as the list
    resolve_to_offset() expects. `image`: the program's ProgramImage. (The
    rizin tool's aflj + isj.)"""
    all_fns = [f for f in get_all_functions(program, image) if f["name"]]
    syms = {}
    for sym in program.getSymbolTable().getSymbolIterator(True):
        a = sym.getAddress()
        if a is None or not a.isMemoryAddress():
            continue
        name = _text(sym.getName())
        if name:
            syms.setdefault(name, uoff(a))
    for addr, name in image.symbols.items():
        syms.setdefault(name, addr)
    return syms, all_fns


def resolve(name, all_fns, syms):
    """Resolve `name` to an entry address: resolve_to_offset first (so this
    and gen_sites.py resolve names identically), then the symbol table for
    names not analysed as functions. Ambiguous = error."""
    try:
        off, _ = resolve_to_offset(name, all_fns)
        return off
    except NotFound:
        pass
    if name in syms:
        return syms[name]
    for prefix in ("dbg.", "sym.", "sym.imp.", "fcn."):
        if (prefix + name) in syms:
            return syms[prefix + name]
    raise ValueError("could not resolve '%s' to a function or symbol" % name)


def first_bb_site(program, bbm, monitor, fn_addr, strategy, is_mips, image):
    """Return (address, function bits) of the site gen_sites.py picks for
    the first basic block of the function at fn_addr, or None. `image`:
    the program's ProgramImage."""
    blocks = function_blocks(program, bbm, monitor, fn_addr, image)
    if not blocks:
        # A symbol-table name Ghidra did not make a function: make one.
        ensure_functions(program, [fn_addr])
        blocks = function_blocks(program, bbm, monitor, fn_addr, image)
    if not blocks:
        return None
    func = program.getFunctionManager().getFunctionAt(addr_of(program, fn_addr))
    # The entry block starts at fn_addr; else the lowest block at or above
    # it, and only then the lowest (function_blocks sorts by address).
    entry = None
    for b in blocks:
        if b[0]["offset"] == fn_addr:
            entry = b
            break
    if entry is None:
        forward = [b for b in blocks if b[0]["offset"] >= fn_addr]
        entry = forward[0] if forward else blocks[0]
    insns = split_block(entry, is_mips)[0]
    if strategy == "entry":
        pick = pick_entry(insns, is_mips and starts_in_delay_slot(insns))
    else:
        pick = pick_terminator(insns)
    if pick is None:
        return None
    return int(pick["offset"]), _func_bits(program, func)


def site_list_offsets(path):
    """{offset: flags} of the module-0 records of a site list."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except (OSError, IOError) as e:
        sys.exit("error: --sites %s: %s" % (path, e))
    if len(data) < SITES_HEADER.size or data[:4] != SITES_MAGIC:
        sys.exit("error: %s: not a site list" % path)
    count = struct.unpack_from("<I", data, 8)[0]
    if len(data) < SITES_HEADER.size + count * SITES_RECORD.size:
        sys.exit("error: %s: truncated site list" % path)
    out = {}
    for i in range(count):
        off, module, _len, flags, _r, _orig = SITES_RECORD.unpack_from(
            data, SITES_HEADER.size + i * SITES_RECORD.size)
        if module == 0:
            out[off] = flags
    return out


def parse_spec(spec):
    """Parse SYM[:kind[:depth_filter]]. (The rizin tool's parser, including
    its C++ '::' handling.)"""
    if "::" not in spec:
        parts = spec.split(":")
        if len(parts) > 3:
            raise ValueError("too many ':' fields in spec %r "
                             "(want SYM[:kind[:depth_filter]])" % spec)
        name = parts[0]
        kind = parts[1] if len(parts) > 1 else "begin"
        depth = parts[2] if len(parts) > 2 else "any"
        if not name:
            raise ValueError("empty symbol in spec %r" % spec)
        if kind not in KIND_MAP:
            raise ValueError("bad kind %r (want begin|end)" % kind)
        if depth not in DEPTH_MAP:
            raise ValueError("bad depth_filter %r (want any|zero)" % depth)
        return name, KIND_MAP[kind], DEPTH_MAP[depth]

    parts = spec.split(":")
    kind = "begin"
    depth = "any"
    if len(parts) >= 3 and parts[-1] in DEPTH_MAP and parts[-2] in KIND_MAP:
        depth = parts.pop()
        kind = parts.pop()
    elif len(parts) >= 2 and parts[-1] in KIND_MAP:
        kind = parts.pop()
    name = ":".join(parts)
    if not name:
        raise ValueError("empty symbol in spec %r" % spec)
    return name, KIND_MAP[kind], DEPTH_MAP[depth]


def check_records(records, force):
    """A BEGIN marker, one depth filter for every marker, and no depth-zero
    filter in a begin-only file. (The rizin tool's rules.)"""
    if not any(k == 0 for _, k, _, _ in records):
        sys.exit("error: no begin marker (a boundary file needs at least one)")
    if len(set(d for _, _, d, _ in records)) > 1:
        sys.exit("error: the markers have different depth filters; every "
                 "marker of a boundary file has the same one")
    if not any(k == 1 for _, k, _, _ in records) and \
            any(d for _, _, d, _ in records):
        msg = ("depth_filter=zero has no effect without END markers "
               "(begin-only file)")
        if not force:
            sys.exit("error: %s; drop ':zero', add an END marker, or pass "
                     "--force" % msg)
        print("warning: %s" % msg, file=sys.stderr)


def write_bounds(path, records):
    with open(path, "wb") as f:
        f.write(BOUNDS_HEADER.pack(BOUNDS_MAGIC, BOUNDS_VERSION, len(records), 0))
        for offset, kind, depth, flags in records:
            f.write(BOUNDS_RECORD.pack(offset, kind, depth, flags, b"\x00" * 5))


def main():
    program = getCurrentProgram()
    if program is None:
        sys.exit("error: no program open in Ghidra (import one, or run with "
                 "-process/-import under analyzeHeadless)")
    ap = argparse.ArgumentParser(
        prog="gen_boundaries.py",
        description="Write an ember boundary file (UAMB) for uam-run "
                    "--cov-boundary from the program open in Ghidra. "
                    "Format: docs/fuzz-formats.md.")
    ap.add_argument("output", help="the boundary file to write")
    ap.add_argument("specs", nargs="+", metavar="SYM[:kind[:depth_filter]]",
                    help="a marker: function name or address, begin|end, "
                         "any|zero (e.g. fuzz_iteration:begin)")
    ap.add_argument("--sites", metavar="FILE",
                    help="check that every marker is a site (module 0) of this "
                         "site list, with the same state flag; exit non-zero "
                         "if not")
    ap.add_argument("--strategy", choices=["auto", "entry", "terminator"],
                    default="auto",
                    help="marker site selection; the site list's --strategy "
                         "(auto = the arch's default)")
    ap.add_argument("--force", action="store_true",
                    help="write a begin-only file that uses depth_filter=zero "
                         "anyway (it has no effect without END markers)")
    ap.add_argument("--verbose", "-v", action="store_true")
    add_symbols_arg(ap)
    args = ap.parse_args([str(a) for a in getScriptArgs()])
    symbols = load_symbols(args.symbols) if args.symbols else {}

    monitor = monitor_or_dummy()
    bbm = BasicBlockModel(program)
    sites = site_list_offsets(args.sites) if args.sites else None

    image = ProgramImage(program, symbols)
    info = get_bin_info(program, image)
    baddr, arch = info["baddr"], info["arch"]
    strategy = (args.strategy if args.strategy != "auto"
                else default_strategy(arch))
    check_strategy(arch, strategy)
    ensure_functions(program, image.entries)
    syms, all_fns = get_function_addrs(program, image)
    if args.verbose:
        print("image base: 0x%x strategy=%s" % (baddr, strategy),
              file=sys.stderr)
        print("functions: %d" % len(all_fns), file=sys.stderr)

    records = []
    for spec in args.specs:
        try:
            name, kind, depth = parse_spec(spec)
            fn_addr = resolve(name, all_fns, syms)
        except ValueError as e:
            sys.exit("error: %s" % e)
        hit = first_bb_site(program, bbm, monitor, fn_addr, strategy,
                            arch == "mips", image)
        if hit is None:
            sys.exit("error: could not find a first-block %s site for %s @ 0x%x"
                     % (strategy, name, fn_addr))
        site, bits = hit
        offset = site - baddr
        flags = site_flags(arch, bits)
        if sites is not None and sites.get(offset) != flags:
            why = ("is not a site" if offset not in sites
                   else "has another state flag than its site")
            sys.exit("error: marker %s at off=0x%x %s in %s"
                     % (name, offset, why, args.sites))
        records.append((offset, kind, depth, flags))
        if args.verbose:
            print("  marker %s entry=0x%x site=0x%x off=0x%x kind=%d depth=%d "
                  "flags=%d" % (name, fn_addr, site, offset, kind, depth, flags),
                  file=sys.stderr)

    check_records(records, args.force)
    write_bounds(args.output, records)
    print("wrote %d marker(s) to %s" % (len(records), args.output),
          file=sys.stderr)


if __name__ == "__main__":
    run_script(main)
