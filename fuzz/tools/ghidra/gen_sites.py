## -*- coding: utf-8 -*-
## Ghidra Jython 2.7 port of fuzz/tools/gen_sites.py
#@runtime Jython
##@category ember
##@keybinding
##@menupath
##@toolbar
"""
gen_sites.py -- write an ember site list (UAMS) from the program open in
Ghidra: one coverage site per basic block. Jython 2.7; run as a Ghidra
script.

The Ghidra counterpart of fuzz/tools/gen_sites.py (the reference; its
docstring documents the strategies and --root/--cutoff/--max-depth). For
the same image and options it writes the same file, byte for byte: the
site selection, the filters, the slice, the overlap check and the packing
are the same code. Only the data source differs: the rizin queries are
replaced by Ghidra's Program API on the already-analysed program.

  rizin                         Ghidra
  ----------------------------  --------------------------------------------
  rzpipe.open + aaa             currentProgram (import + auto-analysis)
  ij / bin info                 Language: processor, size, endianness, base
  aflj (functions)              FunctionManager.getFunctions(True)
  afbj @ fn (basic blocks)      BasicBlockModel.getCodeBlocksContaining(body)
  pdj N @ addr (instructions)   Listing.getInstructions(block, True)
  op type (call/jmp/ret/ind.)   Instruction.getFlowType()
  afij @@F callrefs (CALL)      Function.getCalledFunctions(monitor)
  function bits 16 (Thumb)      the TMode / ISA_MODE context register

Interface differences from the rizin tool:
  - No positional <binary>: module 0 is the program open in Ghidra. The
    first positional is the OUTPUT path.
  - No --raw/--base/--arch/--bits/--endian/--cpu: a raw image's processor,
    base and byte order are chosen when it is imported into Ghidra.
    --symbols is kept.
  - --module NAME:PROJECTPATH opens another module from the SAME Ghidra
    project (imported and analysed there); PROJECTPATH is the program's
    path inside the project (e.g. "/app.elf").
  - rizin's name prefixes don't exist in Ghidra: import stubs are thunk
    functions, and the CRT/init exclusion is the same names undecorated.
    Which functions get sites is decided by the image, as in the rizin tool:
    the ELF's function symbols and entry point (read from the original file
    bytes Ghidra keeps) and --symbols addresses.

Headless:
  analyzeHeadless <proj_dir> <proj_name> -import fw.elf \\
      -scriptPath fuzz/tools/ghidra -postScript gen_sites.py fw.sites
"""
from __future__ import print_function

import argparse
import binascii
import os
import re
import struct
import sys

from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.block import BasicBlockModel
from ghidra.util.task import TaskMonitor


def _script_dir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        return str(getSourceFile().getParentFile().getAbsolutePath())


# This directory first, so its image_resolve (not the rizin tools' one)
# is the one imported.
_HERE = _script_dir()
sys.dont_write_bytecode = True  # no $py.class files next to the scripts
if _HERE in sys.path:
    sys.path.remove(_HERE)
sys.path.insert(0, _HERE)

from image_resolve import (NotFound, add_symbols_arg, apply_symbol_names,
                           load_symbols, program_entries, resolve_to_offset)


# Functions that get no sites by default, besides import thunks
# (Function.isThunk()): the rizin tool's CRT/init prefixes, undecorated.
# --exclude-pat is ORed with this; --no-default-exclude drops both.
DEFAULT_EXCLUDE = r"^(_init|_fini|deregister|register|frame_dummy|__libc_csu)"

# Site list layout (fuzz/formats.h, docs/fuzz-formats.md).
SITES_MAGIC = b"UAMS"
SITES_VERSION = 1
SITES_HEADER = struct.Struct("<4sIIBBHII")
assert SITES_HEADER.size == 24
SITES_RECORD = struct.Struct("<QBBBB4s")
assert SITES_RECORD.size == 16
SITES_FLAG_BASES = 1 << 0
SITE_ALT_STATE = 1 << 0
SITES_ORIG_MAX = 4
SITES_MAX_MODULES = 255
SITES_NAMES_MAX = 4096

# Ghidra processor name -> the rizin arch name the shared logic expects.
ARCH_MAP = {
    "x86": "x86",
    "ARM": "arm",
    "AARCH64": "arm",
    "MIPS": "mips",
    "PowerPC": "ppc",
    "RISCV": "riscv",
    "AVR8": "avr",
    "Xtensa": "xtensa",
    "8051": "8051",
    "Z80": "z80",
    "6502": "6502",
}


# ----------------------------------------------------------------------
# Ghidra address / data helpers.
# ----------------------------------------------------------------------

def _text(x):
    """Coerce a Java String (what the Ghidra API returns) to a Python
    unicode string, so re.search, .encode() and dict comparisons behave."""
    if x is None:
        return u""
    if isinstance(x, unicode):
        return x
    try:
        return unicode(x)
    except Exception:
        return x


def uoff(address):
    """Unsigned offset of a Ghidra Address (getOffset() is a signed Java
    long; high addresses would come back negative)."""
    return address.getOffset() & 0xFFFFFFFFFFFFFFFF


def addr_of(program, off):
    return program.getAddressFactory().getDefaultAddressSpace().getAddress(
        off & 0xFFFFFFFFFFFFFFFF)


# ----------------------------------------------------------------------
# Program facts (the ij / aflj equivalents).
# ----------------------------------------------------------------------

def get_bin_info(program):
    lang = program.getLanguage()
    proc = _text(lang.getProcessor().toString())
    arch = ARCH_MAP.get(proc, proc.lower())
    bits = int(lang.getLanguageDescription().getSize())
    endian = "BE" if lang.isBigEndian() else "LE"
    return {"arch": arch, "bits": bits, "endian": endian,
            "baddr": uoff(program.getImageBase())}


# The context register that says a function is in the ISA's alternate
# state: ARM Thumb, MIPS16e/microMIPS.
_STATE_REGISTERS = {"ARM": "TMode", "MIPS": "ISA_MODE"}


def _func_bits(program, func):
    """rizin's per-function `bits` (16 = Thumb), which decides the site's
    state flag: 16 where the ARM TMode / MIPS ISA_MODE context register is
    set at the entry, else the address size."""
    lang = program.getLanguage()
    size = int(lang.getLanguageDescription().getSize())
    reg = _STATE_REGISTERS.get(_text(lang.getProcessor().toString()))
    ctx = program.getProgramContext()
    state = ctx.getRegister(reg) if reg else None
    if state is None:
        return size
    val = ctx.getValue(state, func.getEntryPoint(), False)
    return 16 if val is not None and int(val) == 1 else size


def ensure_functions(program, entries):
    """Make every address in `entries` a function where Ghidra has none
    (rizin's `af @ ADDR`)."""
    fm = program.getFunctionManager()
    listing = program.getListing()
    tx = program.startTransaction("ember function entries")
    try:
        for off in sorted(entries):
            a = addr_of(program, off)
            if fm.getFunctionAt(a) is not None:
                continue
            if listing.getInstructionAt(a) is None:
                DisassembleCommand(a, None, True).applyTo(program)
            CreateFunctionCmd(a).applyTo(program)
    finally:
        program.endTransaction(tx, True)


def get_all_functions(program, symbols):
    """Unfiltered function list, shaped like rizin's aflj entries, with
    --symbols names applied. Caller filters via regex."""
    fm = program.getFunctionManager()
    out = []
    for func in fm.getFunctions(True):
        if func.isExternal():
            continue
        entry = uoff(func.getEntryPoint())
        body = func.getBody()
        if body is None or body.isEmpty():
            maxbound = entry
        else:
            maxbound = uoff(body.getMaxAddress()) + 1
        out.append({
            "offset":   entry,
            "name":     _text(func.getName()),
            "bits":     _func_bits(program, func),
            "size":     0,            # maxbound carries the extent for bounds
            "maxbound": maxbound,
            "is_thunk": bool(func.isThunk()),
        })
    return apply_symbol_names(out, symbols)


def site_flags(arch, fn_bits):
    """A site's record flags: ARM Thumb and MIPS16e/microMIPS code is the
    alternate state."""
    return SITE_ALT_STATE if arch in ("arm", "mips") and fn_bits == 16 else 0


def filter_functions(all_fns, include_re, exclude_re, drop_thunks):
    out = []
    for f in all_fns:
        name = f.get("name", "")
        if drop_thunks and f.get("is_thunk"):
            continue
        if include_re and not include_re.search(name):
            continue
        if exclude_re and exclude_re.search(name):
            continue
        out.append({
            "offset":   int(f["offset"]),
            "name":     name,
            "bits":     int(f.get("bits", 0)),
            "size":     int(f.get("size") or 0),
            "maxbound": int(f.get("maxbound") or 0),
            "is_thunk": bool(f.get("is_thunk")),
        })
    return out


def report_include_matches(fns, pattern, module_name, verbose):
    """Make --include-pat's effect visible: list what it matched under -v,
    and warn when a bare name matched under two spellings."""
    if verbose:
        print("--include-pat %r matched %d function(s) in '%s':"
              % (pattern, len(fns), module_name), file=sys.stderr)
        for f in fns:
            print("  0x%x %s" % (f["offset"], f["name"]), file=sys.stderr)
    by_base = {}
    for f in fns:
        base = re.sub(r"^(sym|dbg|fcn|loc)\.", "", f["name"])
        by_base.setdefault(base, []).append(f["name"])
    for base, names in sorted(by_base.items()):
        if len(names) > 1:
            print("warning: --include-pat %r matched %r under several names "
                  "(%s); sites at the same address are deduplicated"
                  % (pattern, base, ", ".join(names)), file=sys.stderr)


# ----------------------------------------------------------------------
# Instruction classification + per-block site picking (the rizin tool's
# helpers; they consume plain dicts).
# ----------------------------------------------------------------------

def _insn_dict(program, ins):
    """One instruction as the pickers expect: offset, size, raw bytes, a
    flow class ("call"/"jmp"/"ret"/"") and the lowercased mnemonic."""
    ft = ins.getFlowType()
    if ft.isCall():
        cls = "call"
    elif ft.isJump():
        cls = "jmp"
    elif ft.isTerminal():
        cls = "ret"
    else:
        cls = ""
    jb = ins.getBytes()
    raw = "".join(chr(jb[i] & 0xFF) for i in range(len(jb)))
    return {
        "offset":   uoff(ins.getAddress()),
        "size":     int(ins.getLength()),
        "bytes":    raw,
        "type":     cls,
        "disasm":   _text(ins.getMnemonicString()).lower(),
    }


def op_class(insn_type):
    """The flow class: the adapter already stores "call"/"jmp"/"ret"/""."""
    t = insn_type or ""
    return t if t in ("call", "jmp", "ret") else None


def _insn_mnemonic(insn):
    disasm = (insn.get("disasm") or "").strip().lower()
    return disasm.split(None, 1)[0] if disasm else ""


def _is_code(insn):
    """Ghidra's Listing only hands back decoded instructions (never data),
    so every instruction with bytes is code."""
    return int(insn.get("size", 0)) > 0


def _it_continuations(insns):
    """Offsets of the instructions an ARM Thumb IT block makes conditional:
    the 1-4 after an `it`/`itt`/`ite`/... (one per letter after the "i").
    ember lifts those as continuations of the IT (UAM_IMARK_CONT), like a
    MIPS delay slot, and a coverage site on a continuation is an internal
    error ("translation failed") that stops the run. The IT itself is a
    fine site. Detected from the mnemonic, so a no-op on every other arch;
    an IT block split across two blocks (rare) is not tracked."""
    cond = set()
    for i, insn in enumerate(insns):
        mnem = _insn_mnemonic(insn)
        if mnem.startswith("it") and set(mnem[2:]) <= set(("t", "e")):
            for j in range(i + 1, min(i + len(mnem), len(insns))):
                cond.add(int(insns[j].get("offset", -1)))
    return cond


def pick_terminator(insns):
    """The site of a block under the terminator strategy, or None: the
    first call before the block's last instruction, else the last code
    instruction; never an IT-block continuation (_it_continuations).
    (The rizin tool's helper.)"""
    cont = _it_continuations(insns)

    def eligible(insn):
        return _is_code(insn) and int(insn.get("offset", -1)) not in cont

    for insn in insns[:-1]:
        if op_class(insn.get("type")) == "call" and eligible(insn):
            return insn
    for insn in reversed(insns):
        if eligible(insn):
            return insn
    return None


def pick_entry(insns, in_delay_slot):
    """The site of a block under the entry strategy, or None: its first
    instruction, its second if the block starts in a MIPS delay slot;
    never an IT-block continuation. (The rizin tool's helper.)"""
    cont = _it_continuations(insns)
    idx = 1 if in_delay_slot else 0
    while idx < len(insns):
        insn = insns[idx]
        if _is_code(insn) and int(insn.get("offset", -1)) not in cont:
            return insn
        idx += 1
    return None


def is_branch_at(program, off):
    """MIPS delay-slot test: is the instruction at `off` (a block's start
    - 4) a branch or call? (rizin decoded `pdj 1 @ addr-4`.)"""
    ins = program.getListing().getInstructionAt(addr_of(program, off))
    if ins is None:
        return False
    ft = ins.getFlowType()
    return bool(ft.isCall() or ft.isJump() or ft.isTerminal())


def block_instructions(program, block):
    listing = program.getListing()
    out = []
    it = listing.getInstructions(block, True)
    while it.hasNext():
        out.append(_insn_dict(program, it.next()))
    return out


def function_blocks(program, bbm, monitor, fn_addr):
    """The basic blocks of the function at fn_addr, or [] if none."""
    func = program.getFunctionManager().getFunctionAt(addr_of(program, fn_addr))
    if func is None:
        return []
    out = []
    bit = bbm.getCodeBlocksContaining(func.getBody(), monitor)
    while bit.hasNext():
        out.append(bit.next())
    return out


def split_block(insns, is_mips):
    """Cut a block's instructions after every jump or return (on MIPS after
    its delay slot), so a block never runs on past a conditional transfer.
    (The rizin tool's helper; Ghidra's blocks already end there, rizin's
    do not always.)"""
    out, cur, cut = [], [], None
    for i, insn in enumerate(insns):
        cur.append(insn)
        if cut is None and op_class(insn.get("type")) in ("jmp", "ret"):
            cut = i + 1 if is_mips else i
        if cut is not None and i >= cut:
            out.append(cur)
            cur, cut = [], None
    if cur:
        out.append(cur)
    return out


def pick_sites(program, bbm, monitor, fn, strategy, arch):
    """(address, size, bytes) of the site of each of fn's blocks."""
    is_mips = arch == "mips"
    sites = []
    for block in function_blocks(program, bbm, monitor, fn["offset"]):
        for part in split_block(block_instructions(program, block), is_mips):
            start = int(part[0]["offset"])
            if strategy == "entry":
                pick = pick_entry(part, is_mips and is_branch_at(program,
                                                                 start - 4))
            else:
                pick = pick_terminator(part)
            if pick is not None:
                sites.append((int(pick["offset"]), int(pick["size"]),
                              pick["bytes"]))
    return sites


def default_strategy(arch):
    return "entry" if arch == "mips" else "terminator"


def check_strategy(arch, strategy):
    if arch == "mips" and strategy == "terminator":
        sys.exit("error: --strategy terminator puts sites in branch delay "
                 "slots on MIPS; use entry")


# ----------------------------------------------------------------------
# Selective coverage: call-graph build + BFS (Ghidra-native edges).
# ----------------------------------------------------------------------

def count_indirect_transfers(program, offsets):
    """offset -> (indirect calls, indirect jumps) for each function in
    `offsets` that has any."""
    fm = program.getFunctionManager()
    listing = program.getListing()
    out = {}
    for off in offsets:
        func = fm.getFunctionAt(addr_of(program, off))
        if func is None:
            continue
        calls = jumps = 0
        it = listing.getInstructions(func.getBody(), True)
        while it.hasNext():
            ft = it.next().getFlowType()
            if ft.isComputed() and (ft.isCall() or ft.isJump()):
                if ft.isCall():
                    calls += 1
                else:
                    jumps += 1
        if calls or jumps:
            out[off] = (calls, jumps)
    return out


def report_indirect_transfers(indirect):
    calls = sum(c for c, _ in indirect.values())
    jumps = sum(j for _, j in indirect.values())
    if not calls and not jumps:
        return
    print("note: the slice has %d indirect call(s) and %d indirect jump(s) "
          "in %d function(s). Their targets are not followed: code reached "
          "only through function pointers, vtables or handler tables gets "
          "NO sites unless you add it with --root (see --print-reachable)."
          % (calls, jumps, len(indirect)), file=sys.stderr)


def build_call_graph(program, monitor, all_fns):
    """Outgoing-CALL adjacency: offset -> set(offset), restricted to entries
    in all_fns, from Ghidra's call references (Function.getCalledFunctions).
    An import thunk makes no call, so the BFS stops there."""
    fn_set = set(int(f["offset"]) for f in all_fns)
    fm = program.getFunctionManager()
    adj = {}
    for off in fn_set:
        func = fm.getFunctionAt(addr_of(program, off))
        callees = set()
        if func is not None:
            for callee in func.getCalledFunctions(monitor):
                d = uoff(callee.getEntryPoint())
                if d in fn_set:
                    callees.add(d)
        adj[off] = callees
    return adj


def bfs_reachable(roots, adj, cutoffs, max_depth=None):
    """BFS from `roots` over `adj`, treating each offset in `cutoffs` as a
    hard boundary. (The rizin tool's helper.)"""
    out = set()
    seen = set()
    queue = []
    for r in roots:
        if r in cutoffs:
            continue
        seen.add(r)
        queue.append((r, 0))
    while queue:
        fn, depth = queue.pop(0)
        if fn in cutoffs:
            continue
        out.add(fn)
        if max_depth is not None and depth >= max_depth:
            continue
        for callee in adj.get(fn, ()):
            if callee in seen or callee in cutoffs:
                continue
            seen.add(callee)
            queue.append((callee, depth + 1))
    return out


def validate_no_overlap(records, module_name, fail):
    """Drop records whose [offset, offset+len) byte ranges overlap (one of
    the two is not a real instruction). (The rizin tool's helper.)"""
    ordered = sorted(records, key=lambda r: (r[0], r[1]))
    dropped = set()
    prev = None
    for rec in ordered:
        off, size = rec[0], rec[1]
        if prev is not None:
            poff, psize = prev[0], prev[1]
            if off < poff + psize:
                msg = ("site 0x%x (len %d) overlaps 0x%x-0x%x"
                       % (off, size, poff, poff + psize))
                if fail:
                    sys.exit("error: %s: %s" % (module_name, msg))
                print("warning: %s: %s; dropping 0x%x"
                      % (module_name, msg, off), file=sys.stderr)
                dropped.add(id(rec))
                continue
        prev = rec
    if not dropped:
        return records
    return [r for r in records if id(r) not in dropped]


def parse_slice_specs(tokens, module_specs, flag):
    """Parse --root / --cutoff tokens into specs (MOD!TOKEN pins a module,
    an unqualified address means module 0). (The rizin tool's helper.)"""
    specs = []
    for tok in tokens:
        mod = None
        body = tok
        if "!" in tok:
            mname, body = tok.split("!", 1)
            found = False
            for i, (name, _prog) in enumerate(module_specs):
                if mname == name:
                    mod = i
                    found = True
                    break
            if not found:
                names = ", ".join(n for n, _ in module_specs)
                sys.exit("error: %s '%s': no module '%s' (modules: %s)"
                         % (flag, tok, mname, names))
            if not body:
                sys.exit("error: %s '%s': empty name after '!'" % (flag, tok))
        b = body.strip()
        is_addr = b.startswith(("0x", "0X")) or b.isdigit()
        specs.append({"tok": body, "mod": mod, "addr": is_addr, "orig": tok})
    return specs


def resolve_slice_specs(specs, module_idx, all_fns, found, stubs, flag, verbose):
    """Resolve the specs applying to `module_idx` to entry offsets. (The
    rizin tool's helper; its sym.imp. import-stub case is a thunk check.)"""
    out = set()
    for i, sp in enumerate(specs):
        if sp["mod"] is not None and sp["mod"] != module_idx:
            continue
        if sp["mod"] is None and sp["addr"] and module_idx != 0:
            continue
        try:
            off, fn = resolve_to_offset(sp["tok"], all_fns)
        except NotFound as e:
            if sp["mod"] is not None or sp["addr"]:
                sys.exit("error: %s '%s': %s" % (flag, sp["orig"], e))
            continue
        except ValueError as e:          # ambiguous within this module
            sys.exit("error: %s '%s': %s" % (flag, sp["orig"], e))
        if sp["mod"] is None and fn.get("is_thunk"):
            stubs.setdefault(i, (module_idx, fn["name"]))
            continue
        out.add(off)
        found.setdefault(i, []).append(module_idx)
        if verbose:
            print("%s: module[%d] '%s' -> 0x%x (%s)"
                  % (flag[2:], module_idx, sp["orig"], off, fn["name"]),
                  file=sys.stderr)
    return out


# ----------------------------------------------------------------------
# Per-module record collection + output.
# ----------------------------------------------------------------------

def collect_module_records(args, program, monitor, module_idx, module_name,
                           slicing, symbols=None):
    """Walk one module's functions and return its site records, each
    (offset, len, module_idx, orig_bytes, flags). Only functions starting at
    a program_entries() address get sites, unless --all-functions. (The
    rizin tool's logic, data from Ghidra.)"""
    inc = re.compile(args.include_pat) if args.include_pat else None
    exc_pats = []
    if not args.no_default_exclude:
        exc_pats.append(DEFAULT_EXCLUDE)
    if args.exclude_pat:
        exc_pats.append(args.exclude_pat)
    exc = re.compile("|".join("(?:%s)" % p for p in exc_pats)) if exc_pats \
        else None
    drop_thunks = not args.no_default_exclude

    bbm = BasicBlockModel(program)
    info = get_bin_info(program)
    baddr = info["baddr"]
    arch = info["arch"]
    strategy = args.strategy if args.strategy != "auto" \
        else default_strategy(arch)
    check_strategy(arch, strategy)

    if args.verbose:
        print("module[%d] '%s': arch=%s bits=%d endian=%s baddr=0x%x "
              "strategy=%s" % (module_idx, module_name, arch, info["bits"],
                               info["endian"], baddr, strategy), file=sys.stderr)

    entries = program_entries(program, symbols)
    ensure_functions(program, entries)
    all_fns_raw = get_all_functions(program, symbols)
    fns = filter_functions(all_fns_raw, inc, exc, drop_thunks)
    if inc:
        report_include_matches(fns, args.include_pat, module_name, args.verbose)

    if args.root:
        all_fns = filter_functions(all_fns_raw, None, None, False)
        roots = resolve_slice_specs(slicing["roots"], module_idx, all_fns,
                                    slicing["found_roots"],
                                    slicing["stub_roots"], "--root",
                                    args.verbose)
        cutoffs = resolve_slice_specs(slicing["cutoffs"], module_idx, all_fns,
                                      slicing["found_cutoffs"],
                                      slicing["stub_cutoffs"], "--cutoff",
                                      args.verbose)
        if not roots:
            print("warning: module[%d] '%s': no --root resolves here, so it "
                  "gets no sites (add --root '%s!FUNC')"
                  % (module_idx, module_name, module_name), file=sys.stderr)
            if args.print_reachable:
                print("module[%d] '%s': no roots"
                      % (module_idx, module_name), file=sys.stderr)
            return []

        bad_roots = roots & cutoffs
        if bad_roots:
            bad_hex = ", ".join("0x%x" % x for x in sorted(bad_roots))
            print("warning: root(s) %s are also cutoffs; slice will exclude "
                  "them and BFS will not expand from them" % bad_hex,
                  file=sys.stderr)

        print("building call graph over %d functions..." % len(all_fns),
              file=sys.stderr)
        adj = build_call_graph(program, monitor, all_fns)
        reachable_offsets = bfs_reachable(roots, adj, cutoffs, args.max_depth)
        depth_note = (", max-depth=%d" % args.max_depth
                      if args.max_depth is not None else "")
        print("module[%d] '%s': reachable: %d function(s) from %d root(s), "
              "%d cutoff(s)%s" % (module_idx, module_name,
                                  len(reachable_offsets), len(roots),
                                  len(cutoffs), depth_note), file=sys.stderr)

        indirect = count_indirect_transfers(program, reachable_offsets)
        report_indirect_transfers(indirect)

        if args.print_reachable:
            by_off = dict((int(f["offset"]), f) for f in all_fns)
            for off in sorted(reachable_offsets):
                f = by_off.get(off, {"name": "?", "bits": 0})
                calls, jumps = indirect.get(off, (0, 0))
                note = (" indirect: %d call(s), %d jump(s)" % (calls, jumps)
                        if calls or jumps else "")
                print("  0x%x %s bits=%d%s"
                      % (off, f["name"], f.get("bits", 0), note),
                      file=sys.stderr)
            return []

        before = len(fns)
        fns = [f for f in fns if int(f["offset"]) in reachable_offsets]
        if args.verbose:
            print("regex AND reachable: %d -> %d functions"
                  % (before, len(fns)), file=sys.stderr)

    if args.verbose:
        print("functions with sites: %d" % len(fns), file=sys.stderr)
        for fn in fns:
            print("  0x%x %s bits=%d" % (fn["offset"], fn["name"], fn["bits"]),
                  file=sys.stderr)

    seen = set()
    records = []
    unnamed = 0
    for fn in fns:
        # Functions found only by analysis (no function symbol: FUN_xxxx,
        # or a code label): least reliable block/code-data info, and which
        # of them are found differs between Ghidra and rizin.
        if not args.all_functions and fn["offset"] not in entries:
            unnamed += 1
            if args.verbose:
                print("  skip fn %s (no function symbol; use --all-functions)"
                      % fn["name"], file=sys.stderr)
            continue
        flags = site_flags(arch, fn["bits"])
        for addr, size, orig in pick_sites(program, bbm, monitor, fn,
                                           strategy, arch):
            if addr in seen:
                continue
            if size < 1 or len(orig) != size:
                continue
            size = min(size, SITES_ORIG_MAX)
            orig = orig[:size]
            offset = addr - baddr
            records.append((offset, size, module_idx, orig, flags))
            seen.add(addr)
            if args.verbose:
                print("  site 0x%x off=0x%x len=%d module=%d bytes=%s "
                      "flags=%d (%s)" % (addr, offset, size, module_idx,
                                         binascii.hexlify(orig), flags,
                                         fn["name"]), file=sys.stderr)

    if unnamed and args.verbose:
        print("  skipped %d function(s) without a function symbol (use "
              "--all-functions to give them sites)" % unnamed,
              file=sys.stderr)

    return validate_no_overlap(records, module_name, args.fail_on_overlap)


def write_sites(output_path, module_names, records, module_bases):
    """Pack the records, the module name table and the base table into a
    site list (fuzz/formats.h). Records are in (module, offset) order, so
    the site ids do not depend on the analyser's iteration order."""
    name_blob = b"".join(n.encode("utf-8") + b"\x00" for n in module_names)
    if len(name_blob) > SITES_NAMES_MAX:
        sys.exit("error: module names take %d bytes; a site list holds at "
                 "most %d" % (len(name_blob), SITES_NAMES_MAX))
    assert len(module_bases) == len(module_names)
    records = sorted(records, key=lambda r: (r[2], r[0]))
    modules_offset = SITES_HEADER.size + SITES_RECORD.size * len(records)
    modules_size = len(name_blob)
    pad = (-(modules_offset + modules_size)) % 8    # bases at align8(end)
    bases_blob = b"\x00" * pad + b"".join(struct.pack("<Q", b)
                                          for b in module_bases)

    with open(output_path, "wb") as f:
        f.write(SITES_HEADER.pack(SITES_MAGIC, SITES_VERSION, len(records),
                                  SITES_FLAG_BASES, len(module_names), 0,
                                  modules_offset, modules_size))
        for offset, size, module_idx, orig, flags in records:
            padded = orig + b"\x00" * (SITES_ORIG_MAX - len(orig))
            f.write(SITES_RECORD.pack(offset, module_idx, size, flags, 0,
                                      padded))
        f.write(name_blob)
        f.write(bases_blob)


# ----------------------------------------------------------------------
# Opening additional modules from the current Ghidra project.
# ----------------------------------------------------------------------

def open_project_program(proj_path):
    """Open an additional module by its path inside the current Ghidra
    project (it must already be imported and analysed). Returns (program,
    release) where release() frees it. sys.exit with guidance on failure."""
    try:
        project = getState().getProject()
    except Exception:
        project = None
    if project is None:
        sys.exit("error: --module needs a Ghidra project (run via "
                 "analyzeHeadless so additional modules can be opened)")
    data = project.getProjectData()
    df = data.getFile(proj_path)
    if df is None and not proj_path.startswith("/"):
        df = data.getFile("/" + proj_path)
    if df is None:
        sys.exit("error: --module: no program '%s' in the project; import and "
                 "analyse it first, and pass its project path (e.g. "
                 "'/app.elf')" % proj_path)
    import java.lang
    consumer = java.lang.Object()
    try:
        prog = df.getDomainObject(consumer, False, False, monitor_or_dummy())
    except Exception as e:
        sys.exit("error: --module: could not open '%s': %s" % (proj_path, e))

    def release():
        try:
            prog.release(consumer)
        except Exception:
            pass
    return prog, release


def monitor_or_dummy():
    try:
        return monitor
    except NameError:
        return TaskMonitor.DUMMY


def module_name(program):
    """Module 0's name in the list: the imported file's basename, as the
    rizin tool names its positional binary."""
    try:
        path = program.getExecutablePath()
    except Exception:
        path = None
    if path:
        base = os.path.basename(_text(path))
        if base:
            return base
    return _text(program.getName())


def run(args, primary_program, monitor):
    # Module 0 is the program open in Ghidra.
    module_specs = [(module_name(primary_program), primary_program)]
    releases = []
    for spec in args.module:
        if ":" not in spec:
            sys.exit("error: --module requires NAME:PROJECTPATH form, got '%s'"
                     % spec)
        name, path = spec.split(":", 1)
        if not name or not path:
            sys.exit("error: --module '%s' has empty NAME or PATH" % spec)
        prog, release = open_project_program(path)
        releases.append(release)
        module_specs.append((name, prog))

    if len(module_specs) > SITES_MAX_MODULES:
        sys.exit("error: too many modules (%d); a site list holds at most %d"
                 % (len(module_specs), SITES_MAX_MODULES))

    slicing = {
        "roots":   parse_slice_specs(args.root, module_specs, "--root"),
        "cutoffs": parse_slice_specs(args.cutoff, module_specs, "--cutoff"),
        "found_roots": {}, "found_cutoffs": {},
        "stub_roots": {}, "stub_cutoffs": {},
    }

    all_records = []
    module_bases = []
    try:
        for module_idx, (name, prog) in enumerate(module_specs):
            if args.verbose:
                print("collecting module[%d] '%s'" % (module_idx, name),
                      file=sys.stderr)
            symbols = args.symbols_map if module_idx == 0 else None
            module_bases.append(get_bin_info(prog)["baddr"])
            all_records.extend(collect_module_records(
                args, prog, monitor, module_idx, name, slicing, symbols))
    finally:
        for release in releases:
            release()

    for key, flag in (("roots", "--root"), ("cutoffs", "--cutoff")):
        found = slicing["found_" + key]
        for i, sp in enumerate(slicing[key]):
            mods = found.get(i, [])
            if not mods:
                stub = slicing["stub_" + key].get(i)
                why = (" (module[%d] has only an import thunk %s; add the "
                       "module that defines it with --module)"
                       % (stub[0], stub[1]) if stub else "")
                sys.exit("error: %s '%s': could not resolve it in any module%s"
                         % (flag, sp["orig"], why))
            if len(mods) > 1:
                where = ", ".join("module[%d] '%s'" % (m, module_specs[m][0])
                                  for m in mods)
                print("note: %s '%s' resolved in %s (qualify it as "
                      "MODULE!NAME to pick one)" % (flag, sp["orig"], where),
                      file=sys.stderr)

    if args.print_reachable:
        print("(--print-reachable: exiting before writing the site list)",
              file=sys.stderr)
        return

    if not all_records and not args.allow_empty:
        hint = ""
        if args.include_pat:
            hint = (" Check --include-pat against Ghidra's function names "
                    "(run with -v to see what matched).")
        sys.exit("error: no sites selected; not writing %s.%s Pass "
                 "--allow-empty to write an empty site list anyway."
                 % (args.output, hint))

    write_sites(args.output, [n for n, _ in module_specs], all_records,
                module_bases)
    print("wrote %d sites across %d module(s) to %s"
          % (len(all_records), len(module_specs), args.output),
          file=sys.stderr)


def build_arg_parser():
    ap = argparse.ArgumentParser(
        prog="gen_sites.py",
        description="Write an ember site list (UAMS) for uam-run --cov-sites "
                    "from the program open in Ghidra: one coverage site per "
                    "basic block. Format: docs/fuzz-formats.md.")
    ap.add_argument("output", help="the site list to write")
    ap.add_argument("--module", action="append", default=[],
                    metavar="NAME:PROJECTPATH",
                    help="add another module already imported into this Ghidra "
                         "project: NAME is its name in the list (uam-run "
                         "--cov-module NAME=BASE refers to it), PROJECTPATH the "
                         "program's path inside the project (e.g. '/app.elf'). "
                         "Repeatable; each gets the next module index, from 1.")
    ap.add_argument("--include-pat", default=None,
                    help="only functions whose name matches this regex get sites")
    ap.add_argument("--exclude-pat", default=None,
                    help="also exclude functions whose name matches this "
                         "regex; added to the built-in exclusions")
    ap.add_argument("--no-default-exclude", action="store_true",
                    help="do not apply the built-in exclusions (import "
                         "thunks + CRT/init stubs)")
    ap.add_argument("--all-functions", action="store_true",
                    help="give every function Ghidra found sites, also those "
                         "that start at no function symbol (FUN_*, code "
                         "labels); by default only an ELF's STT_FUNC symbols, "
                         "its entry point and --symbols addresses get sites")
    ap.add_argument("--fail-on-overlap", action="store_true",
                    help="treat overlapping sites as a hard error")
    ap.add_argument("--strategy", choices=["auto", "entry", "terminator"],
                    default="auto",
                    help="which instruction of a block is its site (auto = "
                         "the arch's default: entry on MIPS, terminator "
                         "elsewhere)")
    ap.add_argument("--root", action="append", default=[],
                    metavar="SYM_OR_ADDR",
                    help="give sites only to the call-graph slice reachable "
                         "from this function/address. Repeatable. MODULE!NAME "
                         "pins a module.")
    ap.add_argument("--cutoff", action="append", default=[],
                    metavar="SYM_OR_ADDR",
                    help="stop call-graph traversal at this function (itself "
                         "excluded). Repeatable.")
    ap.add_argument("--max-depth", type=int, default=None, metavar="N",
                    help="bound the BFS to N edges below each root")
    ap.add_argument("--print-reachable", action="store_true",
                    help="resolve the slice, print it, and exit without "
                         "writing the site list")
    ap.add_argument("--allow-empty", action="store_true",
                    help="write the site list even when no sites were "
                         "selected")
    ap.add_argument("--verbose", "-v", action="store_true")
    add_symbols_arg(ap)
    return ap


def main():
    program = getCurrentProgram()
    if program is None:
        sys.exit("error: no program open in Ghidra (import one, or run with "
                 "-process/-import under analyzeHeadless)")
    ap = build_arg_parser()
    args = ap.parse_args([str(a) for a in getScriptArgs()])
    args.symbols_map = load_symbols(args.symbols) if args.symbols else {}

    if (args.cutoff or args.max_depth is not None or args.print_reachable) \
            and not args.root:
        ap.error("--cutoff / --max-depth / --print-reachable require --root")

    run(args, program, monitor_or_dummy())


if __name__ == "__main__":
    main()
