## -*- coding: utf-8 -*-
#@runtime Jython
"""
image_resolve.py -- name/address resolution shared by the Ghidra Jython
ports of ember's site-list and boundary-file generators (gen_sites.py,
gen_boundaries.py in this directory). Jython 2.7; imported, not run.

The Ghidra counterpart of fuzz/tools/image_resolve.py, with the same
resolution logic: both scripts resolve a user-supplied token (a function
name or an address) through resolve_to_offset(), so the same name lands on
the same site in both, as in the rizin pair.

What differs from the rizin version:
  - No --raw/--base/--arch/--bits/--endian/--cpu: Ghidra imported the image
    with a chosen processor, base and byte order, so those facts come from
    `currentProgram`. In place of the raw-image Image class, ProgramImage
    holds what the image file says about the program, in Ghidra's
    addresses.
  - rizin decorates names (sym./dbg./fcn./sym.imp.); Ghidra uses bare
    names (`main`) or namespaced ones (`ns::fn`). _resolve_name keeps the
    prefix tries anyway -- they never match in Ghidra and do no harm.
  - --symbols names are applied WITHOUT a "sym." prefix, because Ghidra
    function names are bare.

`all_fns` is a list of dicts, each at least {"offset": int, "name": str}.
Optional "size" / "maxbound" bound address lookups as in the rizin version.
"""
from __future__ import print_function

import io
import struct
import sys

import jarray


class NotFound(ValueError):
    """The token names no function (as opposed to an ambiguous name). Lets
    multi-module callers try the next module."""


def parse_int_maybe(tok):
    """Try to parse `tok` as an integer (hex with 0x prefix, plain hex of
    >=4 chars, or decimal). Return int on success, None on failure.

    The >=4-char heuristic for unprefixed hex avoids treating short
    identifiers like 'fd' as addresses. resolve_to_offset only reaches this
    for unprefixed tokens AFTER name resolution has failed, so a function
    actually named e.g. 'cafe' resolves as a symbol; use a '0x' prefix to
    force address interpretation."""
    s = tok.strip()
    try:
        if s.startswith(("0x", "0X")):
            return int(s, 16)
        if s.isdigit():
            return int(s)
        if len(s) >= 4 and all(c in "0123456789abcdefABCDEF" for c in s):
            return int(s, 16)
    except ValueError:
        pass
    return None


def _resolve_name(token, all_fns):
    """Return (offset, fn) for a name match, or None. Tries exact, then the
    common rizin prefixes (harmless no-ops in Ghidra), then a last-dotted-
    segment suffix match. Raises ValueError only for an ambiguous suffix
    match."""
    # exact name match
    for f in all_fns:
        if f["name"] == token:
            return int(f["offset"]), f
    # common-prefix tries (no-ops in Ghidra)
    for prefix in ("dbg.", "sym.", "fcn.", "sym.imp."):
        candidate = prefix + token
        for f in all_fns:
            if f["name"] == candidate:
                return int(f["offset"]), f
    # last-segment suffix match (e.g. user said 'main', match 'ns.main')
    matches = [f for f in all_fns if f["name"].rsplit(".", 1)[-1] == token]
    if len(matches) == 1:
        return int(matches[0]["offset"]), matches[0]
    if len(matches) > 1:
        names = ", ".join(m["name"] for m in matches)
        raise ValueError("ambiguous symbol '%s': matches %s" % (token, names))
    return None


def _fn_end(f):
    """Exclusive end of f's code, or None if unknown."""
    ends = []
    size = int(f.get("size") or 0)
    if size > 0:
        ends.append(int(f["offset"]) + size)
    maxbound = int(f.get("maxbound") or 0)
    if maxbound > int(f["offset"]):
        ends.append(maxbound)   # split functions (.cold parts) reach further
    return max(ends) if ends else None


def _resolve_addr(addr, token, all_fns):
    """Return (offset, fn) for an address, resolving an in-function address
    to the enclosing function's entry (with a stderr note). Raises NotFound
    if the address is not within any known function -- below the first one,
    or past the end of the one below it (when its extent is known)."""
    for f in all_fns:
        if int(f["offset"]) == addr:
            return int(f["offset"]), f
    # not a function entry -- find enclosing function by largest offset <= addr
    best = None
    for f in all_fns:
        o = int(f["offset"])
        if o <= addr and (best is None or o > int(best["offset"])):
            best = f
    if best is not None:
        end = _fn_end(best)
        if end is not None and addr >= end:
            raise NotFound("address %s is not within any known function "
                           "(past the end of %s, 0x%x-0x%x)"
                           % (token, best["name"], int(best["offset"]), end))
        print("note: address 0x%x not a function entry; using enclosing fn "
              "%s at 0x%x" % (addr, best["name"], int(best["offset"])),
              file=sys.stderr)
        return int(best["offset"]), best
    raise NotFound("address %s not within any known function" % token)


def resolve_to_offset(token, all_fns):
    """Resolve a user-supplied symbol-or-address token to a function entry
    offset and its function dict.

    Accepts:
      - Exact function name as Ghidra lists it ('main', 'parse_request',
        'ns::handler').
      - Bare name: tries the rizin-style prefixes (no-ops in Ghidra) then a
        suffix match against the last dotted segment of every function name.
      - hex address ('0x401234'), plain-hex address ('401234'), decimal.
        Addresses inside a function (not at entry) resolve to the enclosing
        function's entry, with a note printed to stderr.

    Resolution order: a '0x'-prefixed or decimal token is treated as an
    address first; every other token is resolved as a NAME first and only
    falls back to plain-hex address interpretation when no name matches.

    Raises NotFound (a ValueError) if unresolvable, ValueError if a suffix
    match is ambiguous."""
    s = token.strip()
    # Explicit address forms bind as addresses first.
    if s.startswith(("0x", "0X")) or s.isdigit():
        addr = parse_int_maybe(s)
        if addr is not None:
            return _resolve_addr(addr, token, all_fns)

    # Otherwise prefer a name match; fall back to a plain-hex address.
    hit = _resolve_name(token, all_fns)
    if hit is not None:
        return hit

    addr = parse_int_maybe(s)
    if addr is not None:
        return _resolve_addr(addr, token, all_fns)

    raise NotFound("could not resolve '%s' to any function" % token)


# ---- --symbols support (name functions Ghidra left as FUN_xxxx) -----------

# nm symbol types that name code.
_NM_CODE_TYPES = set("TtWw")


def _parse_addr(tok):
    tok = tok.strip()
    if ":" in tok:                      # Ghidra "ram:80010000"
        space, tok = tok.rsplit(":", 1)
        if space.upper() == "EXTERNAL":  # imports: no address in the image
            raise ValueError(tok)
    if tok.lower().startswith("0x"):
        tok = tok[2:]
    return int(tok, 16)


def load_symbols(path):
    """Parse a symbols file into {addr: name}. Formats (detected per file):
      - Ghidra symbol-table CSV export: a header row with a Name column and
        a Location / Address / Entry Point column (with a Type column, only
        Function rows are kept);
      - `nm` output: ADDR TYPE NAME (only code types T t W w are kept;
        undefined symbols, which have no address, are skipped);
      - ADDR NAME or NAME ADDR per line.
    Where several names share an address the first one not starting with
    '_' wins (else the first). Blank lines and '#' comments are skipped."""
    import csv

    try:
        lines = io.open(path, encoding="utf-8",
                        errors="replace").read().splitlines()
    except (OSError, IOError) as e:
        sys.exit("error: --symbols %s: %s" % (path, e))
    body = [l for l in lines if l.strip() and not l.lstrip().startswith("#")]
    pairs = []
    if body and "," in body[0]:
        rows = list(csv.reader(body))
        head = [c.strip().lower() for c in rows[0]]
        try:
            ni = head.index("name")
            ai = next(head.index(c) for c in ("location", "address",
                                              "entry point", "entry", "addr")
                      if c in head)
        except (ValueError, StopIteration):
            sys.exit("error: --symbols %s: CSV needs Name and Location "
                     "(or Address) columns, got %s" % (path, rows[0]))
        # Ghidra's symbol table lists data labels too; with a Type column,
        # only functions may become sites' functions.
        ti = head.index("type") if "type" in head else None
        for r in rows[1:]:
            if ti is not None and (len(r) <= ti
                                   or r[ti].strip().lower() != "function"):
                continue
            if len(r) > max(ni, ai):
                try:
                    pairs.append((_parse_addr(r[ai]), r[ni].strip()))
                except ValueError:
                    pass                 # External/no-address rows
    else:
        for l in body:
            f = l.split()
            try:
                if len(f) >= 3 and len(f[1]) == 1:
                    if f[1] in _NM_CODE_TYPES:
                        pairs.append((_parse_addr(f[0]), f[2]))
                elif len(f) == 2 and len(f[0]) == 1 and f[0].isalpha():
                    continue              # nm: undefined (U, w, v): no address
                elif len(f) == 2:
                    try:
                        pairs.append((_parse_addr(f[0]), f[1]))
                    except ValueError:
                        pairs.append((_parse_addr(f[1]), f[0]))
            except ValueError:
                sys.exit("error: --symbols %s: cannot parse line %r" % (path, l))
    out = {}
    for addr, name in pairs:
        if not name:
            continue
        cur = out.get(addr)
        if cur is None or (cur.startswith("_") and not name.startswith("_")):
            out[addr] = name
    if not out:
        sys.exit("error: --symbols %s: no code symbols found" % path)
    return out


def add_symbols_arg(ap):
    """The --symbols option, shared by both Ghidra scripts. (The rizin tools'
    raw-image options are chosen at import time under Ghidra.)"""
    ap.add_argument("--symbols", metavar="FILE",
                    help="name functions from FILE: `nm` output (ADDR TYPE "
                         "NAME; text symbols only), ADDR NAME or NAME ADDR "
                         "lines, or a Ghidra symbol-table CSV export (Name and "
                         "Location columns). Applied to the functions at "
                         "those addresses, so --root/--cutoff and marker "
                         "names can find them. The addresses are the "
                         "image's own (its link addresses), as in the rizin "
                         "tool.")


def apply_symbol_names(all_fns, symbols):
    """Rename entries in `all_fns` whose offset is named by `symbols`
    (in place); returns all_fns. Mirrors Image.name_functions() but keeps
    the bare name, since Ghidra names are undecorated."""
    if not symbols:
        return all_fns
    for f in all_fns:
        name = symbols.get(int(f.get("offset", -1)))
        if name:
            f["name"] = name
    return all_fns


# ---- which functions get sites: those at a function symbol ---------------

def elf_code_entries(data):
    """(e_machine, entries, compressed) of an ELF, or None if `data` is not
    one. `entries`: the addresses of its function symbols (STT_FUNC /
    STT_GNU_IFUNC in an executable section) and of its entry point, with
    the state bit of ARM Thumb and MIPS16e/microMIPS addresses cleared.
    Both generators (and their Ghidra ports) give sites only to functions
    that start at one of these (or at a --symbols address), so which
    functions have sites does not depend on the analyser. `compressed`: the
    MIPS16e/microMIPS function entries (st_other), a subset."""
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None
    cls, enc = struct.unpack_from("BB", data, 4)
    e = "<" if enc == 1 else ">"
    if cls == 1:
        machine, = struct.unpack_from(e + "H", data, 18)
        entry, = struct.unpack_from(e + "I", data, 24)
        shoff, = struct.unpack_from(e + "I", data, 32)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 46)
        sh_fmt, sym_fmt, sym_size = e + "IIIIIIIIII", e + "IIIBBH", 16
    elif cls == 2 and len(data) >= 64:
        machine, = struct.unpack_from(e + "H", data, 18)
        entry, = struct.unpack_from(e + "Q", data, 24)
        shoff, = struct.unpack_from(e + "Q", data, 40)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 58)
        sh_fmt, sym_fmt, sym_size = e + "IIQQQQIIQQ", e + "IBBHQQ", 24
    else:
        return None
    mask = ~1 if machine in (8, 40) else ~0     # EM_MIPS, EM_ARM
    secs = []                                   # (type, flags, addr, size, offset, link, entsize)
    for i in range(shnum):
        at = shoff + i * shentsize
        if at + struct.calcsize(sh_fmt) > len(data):
            break
        _n, typ, flags, addr, off, size, link, _i, _a, ent = struct.unpack_from(sh_fmt, data, at)
        secs.append((typ, flags, addr, size, off, link, ent))
    out, compressed = set(), set()
    tabs = [s for s in secs if s[0] == 2] or [s for s in secs if s[0] == 11]   # SYMTAB, else DYNSYM
    for _t, _f, _a, size, off, _l, _e in tabs:
        for at in range(off, min(off + size, len(data)) - sym_size + 1, sym_size):
            if cls == 1:
                _n, value, _s, info, other, shndx = struct.unpack_from(sym_fmt, data, at)
            else:
                _n, info, other, shndx, value, _s = struct.unpack_from(sym_fmt, data, at)
            if (info & 0xf) in (2, 10) and 0 < shndx < len(secs) and secs[shndx][1] & 4:
                out.add(value & mask)
                # STO_MIPS16; STO_MICROMIPS, alone or with STO_MIPS_PIC
                if machine == 8 and ((other & 0xf0) == 0xf0 or (other & 0xc0) == 0x80):
                    compressed.add(value & mask)
    for typ, flags, addr, size, _o, _l, _e in secs:
        if flags & 4 and addr <= (entry & mask) < addr + size:
            out.add(entry & mask)
            break
    return machine, out, compressed


def elf_layout(data):
    """(loads, code) of an ELF, or None if `data` is not one. `loads`: its
    PT_LOAD segments as (file offset, address, file size), in program-header
    order. `code`: the address ranges (start, end) that hold instructions --
    the allocated SHF_EXECINSTR sections when the ELF has section headers
    that name any, else the executable PT_LOAD segments; None when the ELF
    names neither. Both generators (and their Ghidra ports) put a site only
    on an instruction inside `code`: bytes an analyser disassembled anywhere
    else (a jump table in .rodata) are data."""
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None
    cls, enc = struct.unpack_from("BB", data, 4)
    e = "<" if enc == 1 else ">"
    if cls == 1:
        phoff, shoff = struct.unpack_from(e + "II", data, 28)
        phentsize, phnum, shentsize, shnum = struct.unpack_from(e + "HHHH", data, 42)
        ph_fmt, sh_fmt = e + "IIIIIIII", e + "IIIIIIIIII"
    elif cls == 2 and len(data) >= 64:
        phoff, shoff = struct.unpack_from(e + "QQ", data, 32)
        phentsize, phnum, shentsize, shnum = struct.unpack_from(e + "HHHH", data, 54)
        ph_fmt, sh_fmt = e + "IIQQQQQQ", e + "IIQQQQIIQQ"
    else:
        return None
    loads, exec_segs = [], []
    for i in range(phnum):
        at = phoff + i * phentsize
        if at + struct.calcsize(ph_fmt) > len(data):
            break
        if cls == 1:
            typ, off, vaddr, _p, filesz, memsz, flags, _a = struct.unpack_from(ph_fmt, data, at)
        else:
            typ, flags, off, vaddr, _p, filesz, memsz, _a = struct.unpack_from(ph_fmt, data, at)
        if typ != 1:                            # PT_LOAD
            continue
        loads.append((off, vaddr, filesz))
        if flags & 1 and memsz:                 # PF_X
            exec_segs.append((vaddr, vaddr + memsz))
    exec_secs = []
    for i in range(shnum):
        at = shoff + i * shentsize
        if at + struct.calcsize(sh_fmt) > len(data):
            break
        _n, _t, flags, addr, _o, size = struct.unpack_from(sh_fmt, data, at)[:6]
        if flags & 2 and flags & 4 and size:    # SHF_ALLOC, SHF_EXECINSTR
            exec_secs.append((addr, addr + size))
    return loads, (exec_secs or exec_segs or None)


def in_ranges(ranges, addr):
    """True if `addr` is inside one of the (start, end) `ranges`; None means
    no restriction."""
    if ranges is None:
        return True
    for start, end in ranges:
        if start <= addr < end:
            return True
    return False


class ProgramImage(object):
    """What the image file says about the program open in Ghidra, in
    Ghidra's addresses. Ghidra loads a position-independent ELF (ET_DYN: a
    PIE, a shared object) at an image base of its own, so its addresses
    are the ELF's plus `delta`; for every other image `delta` is 0.

      link_base  the module's link base in the site list: the ELF's lowest
                 PT_LOAD address (0 for a PIE), which is what rizin
                 reports; for a raw image the lowest address its bytes are
                 loaded at (the rizin tool's --base)
      delta      Ghidra address - image address
      base       Ghidra's address of link_base: a record's offset is its
                 Ghidra address - base
      entries    the function entries that get sites: the ELF's function
                 symbols and entry point (elf_code_entries) and every
                 --symbols address -- the rizin tool's symbol_entries()
      processor  Ghidra's name of the program's processor ("PowerPC")
      code       the (start, end) ranges that hold instructions
                 (elf_layout), or None when the image names none: then an
                 executable memory block is code"""

    def __init__(self, program, symbols):
        self.program = program
        self.processor = u"%s" % program.getLanguage().getProcessor().toString()
        image_base = program.getImageBase().getOffset() & 0xFFFFFFFFFFFFFFFF
        self.link_base, self.delta = _lowest_file_address(program), 0
        if self.link_base is None:
            self.link_base = image_base
        self.entries, self.code = set(), None
        for fb in program.getMemory().getAllFileBytes():
            n = int(fb.getSize())
            buf = jarray.zeros(n, "b")
            fb.getOriginalBytes(0, buf)
            data = buf.tostring()
            found = elf_code_entries(data)
            if found is None:
                continue
            loads, code = elf_layout(data)
            if loads:
                self.link_base = min(vaddr for _o, vaddr, _s in loads)
                delta = _load_delta(program, loads)
                self.delta = (image_base - self.link_base if delta is None
                              else delta)
            self.entries = set(a + self.delta for a in found[1])
            if code is not None:
                self.code = [(a + self.delta, b + self.delta) for a, b in code]
            break
        self.base = self.link_base + self.delta
        # --symbols addresses are the image's, as in the rizin tool.
        self.symbols = dict((a + self.delta, n)
                            for a, n in (symbols or {}).items())
        self.entries |= set(self.symbols)

    def is_code(self, addr):
        """True if the instruction at Ghidra address `addr` may be a site."""
        if self.code is not None:
            return in_ranges(self.code, addr)
        space = self.program.getAddressFactory().getDefaultAddressSpace()
        block = self.program.getMemory().getBlock(space.getAddress(addr))
        return block is not None and bool(block.isExecute())


def _lowest_file_address(program):
    """The lowest address of a loaded memory block that holds bytes of the
    imported file, or None."""
    low = None
    for block in program.getMemory().getBlocks():
        if not block.isLoaded():
            continue
        for info in block.getSourceInfos():
            if info.getFileBytes().isPresent():
                start = info.getMinAddress().getOffset() & 0xFFFFFFFFFFFFFFFF
                if low is None or start < low:
                    low = start
    return low


def _load_delta(program, loads):
    """Ghidra address - ELF address of the program's loaded bytes, from the
    first memory block that holds file bytes of a PT_LOAD segment (`loads`,
    elf_layout); None if no block does."""
    for block in program.getMemory().getBlocks():
        if not block.isLoaded():
            continue
        for info in block.getSourceInfos():
            if not info.getFileBytes().isPresent():
                continue
            at = int(info.getFileBytesOffset())
            start = info.getMinAddress().getOffset() & 0xFFFFFFFFFFFFFFFF
            for off, vaddr, filesz in loads:
                lo = max(at, off)
                if lo < min(at + int(info.getLength()), off + filesz):
                    return (start + lo - at) - (vaddr + lo - off)
    return None


def run_script(main):
    """Run a script's main(). Ghidra shows only "SystemExit" for a
    sys.exit("error: ..."), so the message is printed first: to stderr,
    which the headless log and the script console both show."""
    try:
        main()
    except SystemExit as e:
        if e.code is not None and not isinstance(e.code, int):
            print(e.code, file=sys.stderr)
            sys.stderr.flush()
        raise
