#!/usr/bin/env python3
"""
gen_sites.py -- write an ember site list (UAMS) for a firmware image: one
coverage site per basic block, for one or more modules.

ember executes the code itself, so a site is only a pc it counts: nothing
is written into the image. Each record names the site's instruction (its
offset from the module's link base) and keeps up to its first 4 bytes, so
uam-run --cov-strict can check the list against the guest's memory.
uam-run takes the list with --cov-sites (the iteration boundaries made by
gen_boundaries.py with --cov-boundary); the format is specified in
docs/fuzz-formats.md and fuzz/formats.h.

Drives rizin through rzpipe: one rizin process per module, `aaa` once at
startup, every later query reuses that analysis.

Site selection per basic block (--strategy):
  terminator  the first call before the block's last instruction (rizin
              does not end a block at a call, so a callee that never
              returns would otherwise hide the block), else the block's
              last instruction. The default, except on MIPS.
  entry       the block's first instruction. The MIPS default: rizin keeps
              a branch's delay slot in the branch's block, so the terminator
              would be a delay slot; and a block that starts in a delay slot
              (the instruction before it is a branch) takes its second
              instruction instead.
  auto        the arch's default.
A block ends after every jump or return (on MIPS after its delay slot),
also where rizin's own block runs on (PowerPC returns), and what follows an
unconditional return there belongs to no block. On every arch a candidate
must be an instruction ember runs on its own -- never one it runs as part
of the one before it (a MIPS delay slot, an ARM Thumb instruction inside an
IT block after the IT: a site there stops the run with "translation
failed") -- and must be code: it lies in an executable section of the ELF
(an executable segment, if the ELF has no section headers), and rizin did
not decode it as data (a literal pool, a jump-table payload). Two sites
whose bytes overlap (a decoding disagreement) keep only the first.

Which functions get sites: those that start at a function symbol of the
ELF (STT_FUNC), at its entry point, or at a --symbols address; functions
rizin found only by analysis need --all-functions. Import stubs
(sym.imp.*) and the C runtime's start/stop stubs (_init, _fini,
frame_dummy, register_tm_clones, deregister_tm_clones,
__do_global_dtors_aux, __libc_csu_init, __libc_csu_fini; exact names) get
none unless --no-default-exclude. The records are in offset order; a
site's id is its index.

The Ghidra port (ghidra/gen_sites.py) follows the same rules, and they are
the image's where they can be, not the analyser's. So the two write the
same file when both analysers recover the same functions and blocks. They
can differ where one resolves a jump table and the other does not: the
case blocks of a switch are then blocks, with sites, in one list only
(seen with rizin on ARM `add pc, pc, rN, lsl #2`, MIPS `jr` and PowerPC
`bctr` tables that Ghidra resolves). Module 0's name is the image's file
name here and the program's name in the Ghidra project there, so the files
are identical only if the image was imported under its file name.

Arches: whatever rizin disassembles -- MIPS, ARM, RISC-V, PowerPC, x86
(16/32/64), AVR, Xtensa, 8051, Z80, 6502, PIC (raw images: --arch, and --cpu
for the PIC variant). ARM Thumb functions get the alternate-state flag
(offsets never carry the Thumb bit). An ELF of a machine rizin has no
disassembler for (AVR32, 8-bit PIC ELFs) is refused, as is an ARM BE8 ELF;
MIPS16e/microMIPS functions get no sites (rizin cannot decode them). The
Ghidra port handles those (with the alternate-state flag for MIPS16e/
microMIPS). A record's len is the instruction's length, at most 4.

Several modules: the positional `binary` is module 0. `--module NAME:PATH`
(repeatable) adds modules 1, 2, ...: NAME is the module's name in the list
(what uam-run --cov-module NAME=BASE refers to), PATH the ELF rizin reads.
The list always carries every module's link base (its base table).

Selective coverage (--root / --cutoff / --max-depth):
  By default every function that survives --include-pat / --exclude-pat
  gets sites. To cover only the input-handling part of a firmware, scope
  the list to a reachability slice of the static call graph:

    --root SYM_OR_ADDR  (repeatable) -- BFS seed. Function name or
                                        address (0x401234 or 4198964).
    --cutoff SYM_OR_ADDR (repeatable) -- boundary; gets no sites and is
                                         not traversed.
    --max-depth N        -- bound the BFS at N call-edges below each root.
    --print-reachable    -- print the resolved slice and exit (no output
                            file written; useful for iterating).

  Resolution rules for --root / --cutoff arguments:
    - 0x-prefixed hex, plain hex (>=4 chars), or decimal -> address.
      Address inside a function (not at entry) resolves to the enclosing
      function's entry, with a note printed to stderr.
    - Bare names try (in order): exact rizin name, then 'dbg.<name>',
      'sym.<name>', 'fcn.<name>', 'sym.imp.<name>', then suffix match on
      the last dotted segment of every function name.  Ambiguous suffix
      matches are an error.

  The call graph is built from rizin's `afij` callrefs: outgoing CALL
  edges to functions rizin found in the analysed binary. An import stub
  (sym.imp.*) makes no CALL, so the BFS stops there, and the default
  exclusion keeps stubs out of the list.

  With --include-pat / --exclude-pat the result is the intersection: a
  function must be both reachable AND pass the regex filters.

  Known limits:
    - Indirect calls (function pointers, vtables, handler tables) are
      invisible to static analysis; functions reached only through them
      are missing from the slice and need their own --root.
    - Tail calls implemented as a jump may or may not appear in callrefs
      depending on rizin's classification; check with --print-reachable.
    - Cutoffs are function-granular.

Usage:
  # an ELF
  ./gen_sites.py fw.elf fw.sites [--strategy auto|entry|terminator]
                                 [--include-pat REGEX] [--exclude-pat REGEX]

  # a raw image (no ELF): give rizin the load address, arch, width and
  # byte order; name functions from an nm / Ghidra symbols export:
  ./gen_sites.py fw.bin fw.sites --raw --base 0x80010000 \\
        --arch mips --bits 32 --endian big --symbols fw.nm

  # only the slice rooted at parse_request, not into logging or crypto:
  ./gen_sites.py fw.elf fw.sites --root parse_request \\
        --cutoff log_writev --cutoff crypto_hash_update --max-depth 6
"""
import argparse
import json
import os
import re
import shutil
import struct
import sys

import rzpipe

sys.dont_write_bytecode = True  # no __pycache__ next to the tools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from image_resolve import (NotFound, Image, add_image_args,  # noqa: E402
                           code_ranges, image_from_args, in_ranges,
                           resolve_to_offset, symbol_entries)


# Functions that get no sites by default: import stubs (rizin's sym.imp.*)
# and the C runtime's start/stop stubs, none of which are meaningful
# coverage. The stubs are matched by their whole name, whatever prefix
# rizin gives it (sym._init, dbg._init), so a firmware function named
# register_handler or _init_board keeps its sites. --exclude-pat adds to
# this; --no-default-exclude drops it.
DEFAULT_EXCLUDE_NAMES = frozenset((
    "_init", "_fini", "frame_dummy", "register_tm_clones",
    "deregister_tm_clones", "__do_global_dtors_aux", "__libc_csu_init",
    "__libc_csu_fini"))
_NAME_PREFIX_RE = re.compile(r"^(sym|dbg)\.")


def default_excluded(name):
    """True for a function the default exclusion drops."""
    return (name.startswith("sym.imp.")
            or _NAME_PREFIX_RE.sub("", name) in DEFAULT_EXCLUDE_NAMES)

# Site list layout (fuzz/formats.h, docs/fuzz-formats.md).
SITES_MAGIC = b"UAMS"
SITES_VERSION = 1
SITES_HEADER = struct.Struct("<4sIIBBHII")   # magic, version, count, flags,
assert SITES_HEADER.size == 24               # module_count, reserved,
                                             # modules_offset, modules_size
SITES_RECORD = struct.Struct("<QBBBB4s")     # offset, module, len, flags,
assert SITES_RECORD.size == 16               # reserved, orig[4]
SITES_FLAG_BASES = 1 << 0                    # header: module base table
SITE_ALT_STATE = 1 << 0                      # record: ARM Thumb
SITES_ORIG_MAX = 4
SITES_MAX_MODULES = 255
SITES_NAMES_MAX = 4096


# rizin still emits a few ANSI CSI sequences (clear-line etc.) even from
# rzpipe with default settings; strip them before raw-JSON decoding of
# `afij @@F` output. cmdj()-returned values are already JSON-parsed so
# this only matters for the raw `cmd()` path.
_ANSI_CSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

# ELF e_machine -> the rizin arch that disassembles it. rizin opens an ELF
# of any other machine as x86 (AVR32, 8-bit PIC, ...), i.e. as garbage.
ELF_MACHINES = {3: "x86", 62: "x86", 8: "mips", 10: "mips", 20: "ppc",
                40: "arm", 183: "arm", 243: "riscv", 83: "avr", 94: "xtensa",
                165: "8051", 220: "z80"}

# e_flags bit of an ARM BE8 ELF (little-endian code, big-endian data).
_EF_ARM_BE8 = 0x00800000


def _arm_be8(path):
    """True if `path` is a big-endian ARM ELF with BE8 code. rizin decodes
    its instructions in the data byte order, i.e. as garbage."""
    try:
        with open(path, "rb") as f:
            h = f.read(52)
    except OSError:
        return False
    if len(h) < 52 or h[:4] != b"\x7fELF" or h[4] != 1 or h[5] != 2:
        return False
    machine, = struct.unpack_from(">H", h, 18)
    flags, = struct.unpack_from(">I", h, 36)
    return machine == 40 and bool(flags & _EF_ARM_BE8)


def open_rizin(binary, image=None, entries=(), machine=None):
    """Open a persistent rizin session and run analysis exactly once.

    `image` (image_resolve.Image) loads a raw image with its --base/--arch/
    --bits/--endian; every address in `entries` (symbol_entries: function
    symbols, the entry point, --symbols) is made a function. `machine`, an
    ELF's e_machine, must be one rizin disassembles."""
    # rzpipe busy-loops forever (100% CPU, no error) when rizin cannot
    # open the file, so check up front.
    if not os.path.isfile(binary) or not os.access(binary, os.R_OK):
        sys.exit(f"error: cannot read binary {binary!r}")
    if shutil.which("rizin") is None:
        sys.exit("error: rizin not found on PATH (needed to analyse the binary)")
    image = image or Image()
    if machine is not None and machine not in ELF_MACHINES:
        sys.exit(f"error: {binary}: rizin cannot disassemble this ELF's "
                 f"machine (e_machine {machine}); use the Ghidra script, or "
                 f"--raw with an --arch rizin knows")
    if not image.raw and _arm_be8(binary):
        sys.exit(f"error: {binary}: an ARM BE8 ELF; rizin decodes its code in "
                 f"the wrong byte order (use the Ghidra script, or the "
                 f"little-endian build)")
    try:
        rz = rzpipe.open(binary, flags=image.rizin_flags())
    except Exception as e:
        sys.exit(f"error: failed to open {binary} with rizin: {e}")
    # Silence ANSI/colour from cmd() output; cmdj() is unaffected.
    rz.cmd("e scr.color=0")
    # A non-ELF (or anything rizin can't load) has no "bin" info; without
    # this check it surfaces much later as "no sites selected".
    b = (rz.cmdj("ij") or {}).get("bin") or {}
    if not image.raw and not b.get("arch"):
        try:
            rz.quit()
        except Exception:
            pass
        sys.exit(f"error: {binary}: not an ELF, or rizin could not analyse it "
                 f"(no 'bin' info from 'ij'; for a raw image pass --raw "
                 f"--base --arch --bits --endian)")
    if machine is not None and b.get("arch") != ELF_MACHINES[machine]:
        rz.quit()
        sys.exit(f"error: {binary}: rizin took the ELF (e_machine {machine}) "
                 f"for {b.get('arch')!r}, not {ELF_MACHINES[machine]!r}")
    rz.cmd("aaa")
    for a in sorted(entries):
        rz.cmd(f"af @ 0x{a:x}")       # a no-op where one exists
    return rz


def get_bin_info(rz, image=None):
    if image is not None and image.raw:
        return image.info()
    info = rz.cmdj("ij") or {}
    b = info.get("bin", {})
    return {
        "arch":   b.get("arch", ""),
        "bits":   int(b.get("bits", 32)),
        "endian": b.get("endian", "LE"),
        "baddr":  int(b.get("baddr", 0)),
    }


def get_all_functions(rz, image=None):
    """Unfiltered function list from `aflj`, --symbols names applied.
    Caller filters via regex."""
    fns = rz.cmdj("aflj") or []
    return image.name_functions(fns) if image is not None else fns


def site_flags(arch, fn_bits):
    """A site's record flags: ARM Thumb (and MIPS16e/microMIPS, which only
    the Ghidra port decodes) code is the alternate state."""
    return SITE_ALT_STATE if arch in ("arm", "mips") and fn_bits == 16 else 0


def filter_functions(all_fns, include_re, exclude_re, default_exclude):
    out = []
    for f in all_fns:
        name = f.get("name", "")
        if default_exclude and default_excluded(name):
            continue
        if include_re and not include_re.search(name):
            continue
        if exclude_re and exclude_re.search(name):
            continue
        out.append({
            "offset":   int(f["offset"]),
            "name":     name,
            "bits":     int(f.get("bits", 0)),
            # extent, so resolve_to_offset can bound address lookups
            "size":     int(f.get("size") or 0),
            "maxbound": int(f.get("maxbound") or 0),
        })
    return out


def report_include_matches(fns, pattern, module_name, verbose):
    """Make --include-pat's effect visible: list what it matched under -v,
    and warn when one function name matched under two rizin prefixes
    (sym.X and dbg.X), which usually means the pattern is looser than
    intended."""
    if verbose:
        print(f"--include-pat {pattern!r} matched {len(fns)} function(s) in "
              f"'{module_name}':", file=sys.stderr)
        for f in fns:
            print(f"  0x{f['offset']:x} {f['name']}", file=sys.stderr)
    by_base = {}
    for f in fns:
        base = re.sub(r"^(sym|dbg|fcn|loc)\.", "", f["name"])
        by_base.setdefault(base, []).append(f["name"])
    for base, names in sorted(by_base.items()):
        if len(names) > 1:
            print(f"warning: --include-pat {pattern!r} matched {base!r} under "
                  f"several names ({', '.join(names)}); sites at the same "
                  f"address are deduplicated", file=sys.stderr)


def get_block_insns(rz, block, code=None):
    """The instructions of an `afbj` block. Bounded by the block's size:
    on MIPS rizin's ninstr counts a delay slot twice, so `pdj ninstr`
    alone would run one instruction past the block. An instruction outside
    the image's `code` ranges (image_resolve.code_ranges) is marked
    "outside": it is data, whatever it decodes as."""
    addr, end = int(block["addr"]), int(block["addr"]) + int(block.get("size", 0))
    insns = rz.cmdj(f"pdj {int(block.get('ninstr', 0))} @ {addr}") or []
    insns = [i for i in insns if int(i.get("offset", end)) < end]
    for i in insns:
        if not in_ranges(code, int(i["offset"])):
            i["outside"] = True
    return insns


def op_class(insn_type):
    """Classify a rizin op type as "call", "jmp", "ret" or None.

    Matches by suffix rather than an exact list: rizin has many variants
    (ucall/rcall/icall/ircall/ccall/uccall, ujmp/rjmp/ijmp/irjmp/mjmp/
    rcjmp/mcjmp/cjmp/ucjmp, cret, ...), which differ across arches and
    versions. MIPS branch-likely forms (beql, bnezl, bgezall, bc1tl) come
    back as cjmp/ccall like their non-likely versions; an exact list that
    missed one would break the delay-slot check of the entry strategy."""
    t = insn_type or ""
    for cls in ("call", "jmp", "ret"):
        if t.endswith(cls):
            return cls
    return None


def is_indirect_transfer(insn_type):
    """Call or jump whose target comes from a register or memory (function
    pointers, vtables, jump tables, tail calls through a pointer). Static
    call-graph slicing can't see past these."""
    t = insn_type or ""
    return op_class(t) in ("call", "jmp") and t[:1] in ("u", "r", "i", "m")


def is_branch_at(rz, addr):
    insns = rz.cmdj(f"pdj 1 @ {addr}") or []
    if not insns:
        return False
    return op_class(insns[0].get("type")) is not None


# Mnemonics rizin emits when it decodes bytes as data, not code (literal
# pools, tbb/tbh jump-table payloads). Matched on the disasm mnemonic.
_DATA_MNEMONICS = frozenset(
    (".byte", ".word", ".short", ".hword", ".dword", ".qword", ".long",
     ".asciz", ".ascii", ".string", ".data", ".fill"))


def _insn_mnemonic(insn):
    disasm = (insn.get("disasm") or "").strip().lower()
    return disasm.split(None, 1)[0] if disasm else ""


def is_data(insn):
    """True if this "instruction" is data rather than code: it lies outside
    the image's code ranges (get_block_insns), or rizin decoded it as an
    invalid/undecodable opcode or a data directive (a literal-pool word, a
    tbb/tbh jump-table payload). Execution never reaches data, so a site
    there would never count, and its bytes would not be an instruction.

    A literal word that happens to decode as a plausible instruction can't be
    caught by type alone; the overlap check and --strategy entry are the
    backstops for that residual case."""
    if insn.get("outside") or (insn.get("type") or "") == "invalid":
        return True
    return _insn_mnemonic(insn) in _DATA_MNEMONICS


def _is_code(insn):
    return int(insn.get("size", 0)) > 0 and not is_data(insn)


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
    first call before the block's last instruction if there is one, else
    the last code instruction.

    - rizin doesn't end blocks at calls, so a block whose callee never
      returns (abort, exit, longjmp -- or the callee crashing, which is the
      interesting case when fuzzing) never reaches its terminator and would
      never count. The call itself executes before the callee runs.
    - Otherwise the LAST code instruction: every instruction of a
      straight-line block executes whenever the block completes.

    Sites never sit on an instruction ember runs as part of the one
    before it: a MIPS delay slot (see pick_entry) or an instruction inside
    an ARM IT block after the IT (_it_continuations).

    gen_boundaries.py uses this same helper, so a marker is the site this
    list has for the function's first block."""
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
    instruction, or its second if the block starts in a MIPS branch delay
    slot (the instruction before it is a branch) -- ember runs a delay slot
    as part of its branch, so it is never a site. Leading data words (a
    literal pool at the block head) and IT-block continuations
    (_it_continuations) are skipped.

    gen_boundaries.py uses this same helper, so a marker is the site this
    list has for the function's first block."""
    idx = 1 if in_delay_slot else 0
    cont = _it_continuations(insns)
    while idx < len(insns):
        insn = insns[idx]
        if _is_code(insn) and int(insn.get("offset", -1)) not in cont:
            return insn
        idx += 1
    return None


def returns_always(insn):
    """True for an unconditional return. rizin types every PowerPC return
    as conditional, the plain blr too."""
    t = insn.get("type") or ""
    return t == "ret" or (t == "cret" and _insn_mnemonic(insn) == "blr")


def split_block(insns, is_mips):
    """Cut a block's instructions after every jump or return (on MIPS after
    its delay slot), so a block never runs on past a conditional transfer.
    rizin lets some do -- PowerPC's returns (blr, bnelr, beqlr) do not end
    its blocks -- which would hide the code after them; Ghidra ends its
    blocks there. Calls do not cut, in either. What follows an
    unconditional return in the block is never reached from it (rizin runs
    the block on into the padding, or the next function, after a blr), so
    it is left out."""
    out, cur, cut, last = [], [], None, False
    for i, insn in enumerate(insns):
        cur.append(insn)
        if cut is None and op_class(insn.get("type")) in ("jmp", "ret"):
            cut = i + 1 if is_mips else i
            last = returns_always(insn)
        if cut is not None and i >= cut:
            out.append(cur)
            cur, cut = [], None
            if last:
                break
    if cur:
        out.append(cur)
    return out


def pick_sites(rz, fn, strategy, arch, code=None):
    """(address, size, bytes hex) of the site of each of fn's blocks; none
    outside the image's `code` ranges."""
    is_mips = arch == "mips"
    blocks = rz.cmdj(f"afbj @ {fn['offset']}") or []
    sites = []
    for b in blocks:
        for part in split_block(get_block_insns(rz, b, code), is_mips):
            start = int(part[0]["offset"])
            if strategy == "entry":
                pick = pick_entry(part, is_mips and is_branch_at(rz, start - 4))
            else:
                pick = pick_terminator(part)
            if pick is not None:
                sites.append((int(pick["offset"]), int(pick["size"]), pick["bytes"]))
    return sites


def default_strategy(arch):
    return "entry" if arch == "mips" else "terminator"


def check_strategy(arch, strategy):
    if arch == "mips" and strategy == "terminator":
        # rizin keeps a branch's delay slot in the branch's block, so the
        # terminator would be the delay slot, which ember runs as part of
        # its branch: a site there is a translation failure.
        sys.exit("error: --strategy terminator puts sites in branch delay "
                 "slots on MIPS; use entry")


# ----------------------------------------------------------------------
# Selective coverage: symbol resolution + call-graph BFS.
# ----------------------------------------------------------------------

def count_indirect_transfers(rz, offsets):
    """offset -> (indirect calls, indirect jumps) for each function in
    `offsets` that has any."""
    out = {}
    for off in offsets:
        ops = rz.cmdj(f"pdfj @ {off}") or {}
        calls = jumps = 0
        for op in ops.get("ops", []) if isinstance(ops, dict) else []:
            t = op.get("type")
            if is_indirect_transfer(t):
                if op_class(t) == "call":
                    calls += 1
                else:
                    jumps += 1
        if calls or jumps:
            out[off] = (calls, jumps)
    return out


def report_indirect_transfers(indirect):
    """The static call graph stops at indirect transfers, so code reached
    only through function pointers, vtables or handler tables is silently
    left out of a --root slice. Say how many there are."""
    calls = sum(c for c, _ in indirect.values())
    jumps = sum(j for _, j in indirect.values())
    if not calls and not jumps:
        return
    print(f"note: the slice has {calls} indirect call(s) and {jumps} indirect "
          f"jump(s) in {len(indirect)} function(s). Their targets are not "
          f"followed: code reached only through function pointers, vtables "
          f"or handler tables gets NO sites unless you add it with "
          f"--root (see --print-reachable for which functions).",
          file=sys.stderr)


def build_call_graph(rz, all_fns):
    """Build outgoing-call adjacency: offset -> set(offset).

    Runs `afij @@F` in the persistent session: `afij` at every function
    rizin discovered, each emitted as its own JSON array. The concatenated
    arrays are parsed with `json.JSONDecoder.raw_decode` and keyed by each
    entry's `offset` field (the iteration order isn't a documented
    contract). Edges are restricted to function entries in all_fns."""
    fn_set = {int(f["offset"]) for f in all_fns}
    raw = rz.cmd("afij @@F")
    raw = _ANSI_CSI_RE.sub("", raw).strip()

    decoder = json.JSONDecoder()
    pos = 0
    adj = {off: set() for off in fn_set}
    while pos < len(raw):
        while pos < len(raw) and raw[pos] in " \t\r\n":
            pos += 1
        if pos >= len(raw):
            break
        try:
            obj, end = decoder.raw_decode(raw, pos)
        except json.JSONDecodeError as e:
            print(f"warning: could not parse rizin 'afij @@F' output at "
                  f"offset {pos} ({e.msg}); the call graph is truncated and "
                  f"the slice may be missing functions", file=sys.stderr)
            break
        pos = end
        entries = obj if isinstance(obj, list) else [obj]
        for entry in entries:
            off = int(entry.get("offset", -1))
            if off not in fn_set:
                continue
            callees = adj.setdefault(off, set())
            for c in (entry.get("callrefs") or []):
                # {"from": <call-site>, "to": <callee>, "type": ...}; older
                # rizin used {"addr": <callee>, "at": <call-site>}. Only
                # CALL edges are followed ("CODE" picks up too many false
                # positives); a missing type (very old rizin) is accepted.
                ctype = c.get("type")
                if ctype is not None and ctype != "CALL":
                    continue
                try:
                    d = int(c.get("to", c.get("addr", -1)))
                except (TypeError, ValueError):
                    continue
                if d in fn_set:
                    callees.add(d)
    return adj


def bfs_reachable(roots, adj, cutoffs, max_depth=None):
    """BFS from `roots` over `adj`, treating each offset in `cutoffs` as
    a hard boundary: not added to the reachable set, not traversed
    through. Returns the set of reachable offsets (including roots that
    aren't themselves cutoffs).

    `max_depth` (if not None) bounds traversal to that many call-edges
    below each root. A root is depth 0; its direct callees are depth 1.
    """
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
    """Drop records whose [offset, offset+len) byte ranges overlap. Two real
    instructions never overlap, so an overlap means rizin decoded the same
    bytes two ways (data in text, a misaligned decode) and one of the two
    sites is not an instruction ember will execute. Exact-duplicate
    addresses are deduped upstream, so only partial overlaps reach here.

    `records` is a list of (offset, len, module, orig, flags), module-
    relative, so this is called per module; len is the instruction's whole
    length, also where the record will store fewer bytes. The later (higher-offset) site
    of an overlapping pair is dropped with a warning; with `fail` the first
    overlap is a hard error. Returns the surviving records in their
    original (discovery) order."""
    ordered = sorted(records, key=lambda r: (r[0], r[1]))
    dropped = set()
    prev = None
    for rec in ordered:
        off, size = rec[0], rec[1]
        if prev is not None:
            poff, psize = prev[0], prev[1]
            if off < poff + psize:
                msg = (f"site 0x{off:x} (len {size}) overlaps "
                       f"0x{poff:x}-0x{poff + psize:x}")
                if fail:
                    sys.exit(f"error: {module_name}: {msg}")
                print(f"warning: {module_name}: {msg}; dropping 0x{off:x}",
                      file=sys.stderr)
                dropped.add(id(rec))
                # Keep `prev` as the earlier site so a run of overlaps all
                # measure against the site actually retained.
                continue
        prev = rec
    if not dropped:
        return records
    return [r for r in records if id(r) not in dropped]


def parse_slice_specs(tokens, module_specs, flag):
    """Parse --root / --cutoff tokens into specs.

    `MOD!TOKEN` pins a spec to one module (MOD = the module's name, or the
    basename of its path); `!` can't clash with --module's NAME:PATH or a
    C++ `ns::fn`. An unqualified name resolves in every module that has
    it; an unqualified explicit address (0x..., decimal) means module 0,
    since module address ranges overlap."""
    specs = []
    for tok in tokens:
        mod = None
        body = tok
        if "!" in tok:
            mname, body = tok.split("!", 1)
            for i, (name, path) in enumerate(module_specs):
                if mname in (name, os.path.basename(path)):
                    mod = i
                    break
            else:
                names = ", ".join(n for n, _ in module_specs)
                sys.exit(f"error: {flag} '{tok}': no module '{mname}' "
                         f"(modules: {names})")
            if not body:
                sys.exit(f"error: {flag} '{tok}': empty name after '!'")
        b = body.strip()
        is_addr = b.startswith(("0x", "0X")) or b.isdigit()
        specs.append({"tok": body, "mod": mod, "addr": is_addr, "orig": tok})
    return specs


def resolve_slice_specs(specs, module_idx, all_fns, found, stubs, flag, verbose):
    """Resolve the specs that apply to module `module_idx` to function
    entry offsets. Records in `found[i]` which modules spec i resolved in.
    A qualified spec or an address must resolve where it applies; an
    unqualified name that isn't here may live in another module (run()
    checks it resolved somewhere). An unqualified name that only matches
    an import stub (sym.imp.NAME) doesn't count; `stubs[i]` remembers it
    for the error message."""
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
                sys.exit(f"error: {flag} '{sp['orig']}': {e}")
            continue
        except ValueError as e:          # ambiguous within this module
            sys.exit(f"error: {flag} '{sp['orig']}': {e}")
        if (sp["mod"] is None and fn["name"].startswith("sym.imp.")
                and not sp["tok"].startswith("sym.imp.")):
            stubs.setdefault(i, (module_idx, fn["name"]))
            continue
        out.add(off)
        found.setdefault(i, []).append(module_idx)
        if verbose:
            print(f"{flag[2:]}: module[{module_idx}] '{sp['orig']}' -> 0x{off:x} "
                  f"({fn['name']})", file=sys.stderr)
    return out


def collect_module_records(args, rz, module_idx, module_name, slicing,
                           entries, compressed=(), image=None, code=None):
    """Walk one module's functions and return its site records, each a
    tuple (offset, instruction length, module_idx, first bytes, flags).
    Only functions starting at an address in `entries` (symbol_entries)
    get sites, unless --all-functions; never the MIPS16e/microMIPS ones in
    `compressed`, which rizin cannot decode, and no site lies outside the
    `code` ranges (image_resolve.code_ranges).

    `slicing` carries the parsed --root / --cutoff specs and the per-spec
    "resolved in" maps shared across modules (see run()). With
    --print-reachable the slice is printed and no records are returned.
    `image` describes a raw image / --symbols (module 0 only)."""
    inc = re.compile(args.include_pat) if args.include_pat else None
    exc = re.compile(args.exclude_pat) if args.exclude_pat else None

    info = get_bin_info(rz, image)
    baddr = info["baddr"]
    arch = info["arch"]
    strategy = args.strategy if args.strategy != "auto" else default_strategy(arch)
    check_strategy(arch, strategy)

    if args.verbose:
        print(f"module[{module_idx}] '{module_name}': arch={arch} "
              f"bits={info['bits']} endian={info['endian']} baddr=0x{baddr:x} "
              f"strategy={strategy}", file=sys.stderr)

    # One `aflj` snapshot, reused for filtering and slice resolution.
    all_fns_raw = get_all_functions(rz, image)
    fns = filter_functions(all_fns_raw, inc, exc, not args.no_default_exclude)
    if inc:
        report_include_matches(fns, args.include_pat, module_name, args.verbose)

    # Selective coverage: resolve --root / --cutoff and intersect the
    # regex-filtered fns with the call-graph slice reachable from the roots.
    # Resolution and BFS run over the UNFILTERED function list, because the
    # regex may exclude intermediate functions between a --root and a
    # callee that should get sites.
    if args.root:
        all_fns = filter_functions(all_fns_raw, None, None, False)
        roots = resolve_slice_specs(slicing["roots"], module_idx, all_fns,
                                    slicing["found_roots"], slicing["stub_roots"],
                                    "--root", args.verbose)
        cutoffs = resolve_slice_specs(slicing["cutoffs"], module_idx, all_fns,
                                      slicing["found_cutoffs"],
                                      slicing["stub_cutoffs"], "--cutoff",
                                      args.verbose)
        if not roots:
            # Calls into another module go through an import stub and
            # aren't followed, so a slice never reaches a module without
            # its own root: giving all of it sites would ignore the slice.
            print(f"warning: module[{module_idx}] '{module_name}': no --root "
                  f"resolves here, so it gets no sites (add "
                  f"--root '{module_name}!FUNC')", file=sys.stderr)
            if args.print_reachable:
                print(f"module[{module_idx}] '{module_name}': no roots",
                      file=sys.stderr)
            return []

        bad_roots = roots & cutoffs
        if bad_roots:
            bad_hex = ", ".join(f"0x{x:x}" for x in sorted(bad_roots))
            print(f"warning: root(s) {bad_hex} are also cutoffs; slice will "
                  f"exclude them and BFS will not expand from them",
                  file=sys.stderr)

        print(f"building call graph over {len(all_fns)} functions...",
              file=sys.stderr)
        adj = build_call_graph(rz, all_fns)
        reachable_offsets = bfs_reachable(roots, adj, cutoffs, args.max_depth)
        depth_note = (f", max-depth={args.max_depth}"
                      if args.max_depth is not None else "")
        print(f"module[{module_idx}] '{module_name}': reachable: "
              f"{len(reachable_offsets)} function(s) from "
              f"{len(roots)} root(s), {len(cutoffs)} cutoff(s){depth_note}",
              file=sys.stderr)

        indirect = count_indirect_transfers(rz, reachable_offsets)
        report_indirect_transfers(indirect)

        if args.print_reachable:
            by_off = {int(f["offset"]): f for f in all_fns}
            for off in sorted(reachable_offsets):
                f = by_off.get(off, {"name": "?", "bits": 0})
                calls, jumps = indirect.get(off, (0, 0))
                note = (f" indirect: {calls} call(s), {jumps} jump(s)"
                        if calls or jumps else "")
                print(f"  0x{off:x} {f['name']} bits={f.get('bits', 0)}{note}",
                      file=sys.stderr)
            return []

        before = len(fns)
        fns = [f for f in fns if int(f["offset"]) in reachable_offsets]
        if args.verbose:
            print(f"regex ∩ reachable: {before} -> {len(fns)} functions",
                  file=sys.stderr)

    if args.verbose:
        print(f"functions with sites: {len(fns)}", file=sys.stderr)
        for fn in fns:
            print(f"  0x{fn['offset']:x} {fn['name']} bits={fn['bits']}",
                  file=sys.stderr)

    seen = set()
    records = []
    unnamed = 0
    skipped_compressed = [f["name"] for f in fns if f["offset"] in compressed]
    if skipped_compressed:
        print(f"warning: module[{module_idx}] '{module_name}': rizin cannot "
              f"decode MIPS16e/microMIPS code; {len(skipped_compressed)} such "
              f"function(s) get no sites ({', '.join(skipped_compressed)}); "
              f"the Ghidra script handles them", file=sys.stderr)
    for fn in fns:
        if fn["offset"] in compressed:
            continue
        # Functions found only by analysis (no function symbol: rizin's
        # fcn.*, or a code label) have the least reliable block boundaries
        # and code/data classification, so their sites are the likeliest to
        # be data; and which of them an analyser finds differs between
        # rizin and Ghidra. Skip them unless asked. Only their sites are
        # skipped -- the call-graph BFS still goes through them.
        if not args.all_functions and fn["offset"] not in entries:
            unnamed += 1
            if args.verbose:
                print(f"  skip fn {fn['name']} (no function symbol; use "
                      f"--all-functions)", file=sys.stderr)
            continue
        flags = site_flags(arch, fn["bits"])
        for addr, size, bhex in pick_sites(rz, fn, strategy, arch, code):
            if addr in seen:
                continue
            try:
                orig = bytes.fromhex(bhex)
            except ValueError:
                continue
            if size < 1 or len(orig) != size:
                continue
            # The record keeps the instruction's whole length for the
            # overlap check; write_sites stores at most SITES_ORIG_MAX.
            orig = orig[:SITES_ORIG_MAX]
            offset = addr - baddr
            records.append((offset, size, module_idx, orig, flags))
            seen.add(addr)
            if args.verbose:
                print(f"  site 0x{addr:x} off=0x{offset:x} len={size} "
                      f"module={module_idx} bytes={orig.hex()} flags={flags} "
                      f"({fn['name']})", file=sys.stderr)

    if unnamed:
        print(f"module[{module_idx}] '{module_name}': skipped {unnamed} "
              f"function(s) without a function symbol (--all-functions gives "
              f"them sites, --symbols FILE names them)", file=sys.stderr)

    return validate_no_overlap(records, module_name, args.fail_on_overlap)


def write_sites(output_path, module_names, records, module_bases):
    """Pack the records, the module name table and the base table into a
    site list (fuzz/formats.h). Records are in (module, offset) order, so
    the site ids do not depend on the analyser's iteration order. A record
    stores the instruction's first bytes (at most SITES_ORIG_MAX) and their
    count."""
    name_blob = b"".join(n.encode("utf-8") + b"\x00" for n in module_names)
    if len(name_blob) > SITES_NAMES_MAX:
        sys.exit(f"error: module names take {len(name_blob)} bytes; a site "
                 f"list holds at most {SITES_NAMES_MAX}")
    assert len(module_bases) == len(module_names)
    records = sorted(records, key=lambda r: (r[2], r[0]))
    modules_offset = SITES_HEADER.size + SITES_RECORD.size * len(records)
    modules_size = len(name_blob)
    pad = (-(modules_offset + modules_size)) % 8    # bases at align8(end)
    bases_blob = b"\x00" * pad + b"".join(struct.pack("<Q", b) for b in module_bases)

    with open(output_path, "wb") as f:
        f.write(SITES_HEADER.pack(SITES_MAGIC, SITES_VERSION, len(records),
                                  SITES_FLAG_BASES, len(module_names), 0,
                                  modules_offset, modules_size))
        for offset, size, module_idx, orig, flags in records:
            padded = orig + b"\x00" * (SITES_ORIG_MAX - len(orig))
            f.write(SITES_RECORD.pack(offset, module_idx, len(orig), flags, 0,
                                      padded))
        f.write(name_blob)
        f.write(bases_blob)


def run(args):
    """Collect records from every requested module and write the list."""
    # Module 0 is the positional `binary`; each `--module NAME:PATH` is the
    # next index, in command-line order.
    module_specs = [(os.path.basename(args.binary), args.binary)]
    for spec in args.module:
        if ":" not in spec:
            sys.exit(f"error: --module requires NAME:PATH form, got '{spec}'")
        name, path = spec.split(":", 1)
        if not name or not path:
            sys.exit(f"error: --module '{spec}' has empty NAME or PATH")
        module_specs.append((name, path))

    if len(module_specs) > SITES_MAX_MODULES:
        sys.exit(f"error: too many modules ({len(module_specs)}); a site list "
                 f"holds at most {SITES_MAX_MODULES}")

    slicing = {
        "roots":   parse_slice_specs(args.root, module_specs, "--root"),
        "cutoffs": parse_slice_specs(args.cutoff, module_specs, "--cutoff"),
        "found_roots": {}, "found_cutoffs": {},
        "stub_roots": {}, "stub_cutoffs": {},
    }

    all_records = []
    module_bases = []
    for module_idx, (name, path) in enumerate(module_specs):
        if args.verbose:
            print(f"opening rizin on module[{module_idx}] '{name}' ({path})",
                  file=sys.stderr)
        image = args.image if module_idx == 0 else None
        machine, entries, compressed = symbol_entries(path, image)
        rz = open_rizin(path, image, entries, machine)
        try:
            module_bases.append(get_bin_info(rz, image)["baddr"])
            all_records.extend(collect_module_records(
                args, rz, module_idx, name, slicing, entries, compressed,
                image, code_ranges(path, image)))
        finally:
            try:
                rz.quit()
            except Exception:
                pass

    # Every unqualified --root / --cutoff name must have resolved in at
    # least one module; say where a name resolved in several.
    for key, flag in (("roots", "--root"), ("cutoffs", "--cutoff")):
        found = slicing["found_" + key]
        for i, sp in enumerate(slicing[key]):
            mods = found.get(i, [])
            if not mods:
                stub = slicing["stub_" + key].get(i)
                why = (f" (module[{stub[0]}] has only the import stub {stub[1]}; "
                       f"add the module that defines it with --module)"
                       if stub else "")
                sys.exit(f"error: {flag} '{sp['orig']}': could not resolve it "
                         f"in any module{why}")
            if len(mods) > 1:
                where = ", ".join(f"module[{m}] '{module_specs[m][0]}'" for m in mods)
                print(f"note: {flag} '{sp['orig']}' resolved in {where} "
                      f"(qualify it as MODULE!NAME to pick one)", file=sys.stderr)

    if args.print_reachable:
        print("(--print-reachable: exiting before writing the site list)",
              file=sys.stderr)
        return

    if not all_records and not args.allow_empty:
        hint = ""
        if args.include_pat:
            hint = (" Check --include-pat: rizin names functions with debug "
                    "info 'dbg.NAME' and others 'sym.NAME' (run with -v to "
                    "see what matched).")
        if not args.all_functions:
            hint += (" Only functions at a --symbols address of a raw image "
                     "get sites; pass --symbols FILE or --all-functions."
                     if args.raw else
                     " Only functions at a function symbol of the ELF, at its "
                     "entry point or at a --symbols address get sites; for a "
                     "stripped ELF pass --symbols FILE or --all-functions.")
        sys.exit(f"error: no sites selected; not writing {args.output}."
                 f"{hint} Pass --allow-empty to write an empty site list "
                 f"anyway.")

    write_sites(args.output, [n for n, _ in module_specs], all_records,
                module_bases)
    print(f"wrote {len(all_records)} sites across {len(module_specs)} "
          f"module(s) to {args.output}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(
        description="Write an ember site list (UAMS) for uam-run --cov-sites: "
                    "one coverage site per basic block of a firmware image, "
                    "chosen with rizin. Format: docs/fuzz-formats.md.")
    ap.add_argument("binary",
                    help="the firmware image (module 0): an ELF, or a raw "
                         "image with --raw")
    ap.add_argument("output", help="the site list to write")
    ap.add_argument("--module", action="append", default=[],
                    metavar="NAME:PATH",
                    help="add another module: NAME is its name in the list "
                         "(uam-run --cov-module NAME=BASE refers to it), PATH "
                         "the ELF rizin analyses. Repeatable; each --module "
                         "gets the next module index, from 1.")
    ap.add_argument("--include-pat", default=None,
                    help="only functions whose name matches this regex get sites")
    ap.add_argument("--exclude-pat", default=None,
                    help="also exclude functions whose name matches this "
                         "regex; added to (not replacing) the built-in "
                         "exclusions. Use --no-default-exclude to drop those.")
    ap.add_argument("--no-default-exclude", action="store_true",
                    help="do not apply the built-in exclusions (import "
                         "stubs sym.imp.*, and the C runtime's _init, _fini, "
                         "frame_dummy, register_tm_clones, "
                         "deregister_tm_clones, __do_global_dtors_aux, "
                         "__libc_csu_init, __libc_csu_fini); only "
                         "--exclude-pat, if given, is used")
    ap.add_argument("--all-functions", action="store_true",
                    help="give every function rizin found sites, also those "
                         "that start at no function symbol (fcn.*, code "
                         "labels); by default only an ELF's STT_FUNC symbols, "
                         "its entry point and --symbols addresses get sites, "
                         "because the others' block boundaries and code/data "
                         "classification are least reliable")
    ap.add_argument("--fail-on-overlap", action="store_true",
                    help="treat overlapping sites as a hard error instead "
                         "of dropping the later site with a warning")
    ap.add_argument("--strategy", choices=["auto", "entry", "terminator"],
                    default="auto",
                    help="which instruction of a block is its site (auto = "
                         "the arch's default: entry on MIPS, terminator "
                         "elsewhere)")
    ap.add_argument("--root", action="append", default=[],
                    metavar="SYM_OR_ADDR",
                    help="give sites only to the call-graph slice reachable "
                         "from this function/address. Repeatable. Accepts "
                         "function names (main, dbg.main, sym.parse_request) "
                         "or addresses (0x401234, which must fall inside a "
                         "function). With --module, a name resolves in every "
                         "module that has it, an address means module 0, and "
                         "MODULE!NAME pins one module; a module no root "
                         "resolves in gets no sites. If omitted, all functions "
                         "matching --include-pat/--exclude-pat get sites.")
    ap.add_argument("--cutoff", action="append", default=[],
                    metavar="SYM_OR_ADDR",
                    help="stop call-graph traversal at this function. The "
                         "cutoff itself is excluded from the slice. "
                         "Repeatable. Resolution rules match --root.")
    ap.add_argument("--max-depth", type=int, default=None, metavar="N",
                    help="bound the call-graph BFS to N edges below each root "
                         "(default: unbounded). A root is depth 0.")
    ap.add_argument("--print-reachable", action="store_true",
                    help="resolve --root/--cutoff, print the reachable "
                         "function set of every module to stderr, and exit "
                         "without writing the site list")
    ap.add_argument("--allow-empty", action="store_true",
                    help="write the site list even when no sites were "
                         "selected (by default that is an error: an empty "
                         "list records no coverage)")
    ap.add_argument("--verbose", "-v", action="store_true")
    add_image_args(ap)
    args = ap.parse_args()
    args.image = image_from_args(ap, args)
    if args.raw and args.module:
        ap.error("--raw describes the positional binary only; --module takes ELFs "
                 "and can't be combined with it")
    if (args.cutoff or args.max_depth is not None or args.print_reachable) \
            and not args.root:
        ap.error("--cutoff / --max-depth / --print-reachable require --root")

    run(args)


if __name__ == "__main__":
    main()
