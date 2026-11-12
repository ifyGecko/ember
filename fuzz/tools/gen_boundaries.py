#!/usr/bin/env python3
"""
gen_boundaries.py -- write an ember boundary file (UAMB): the markers that
say where a fuzz iteration begins and ends.

uam-run takes the file with --cov-boundary (in place of --fuzz-begin /
--fuzz-end); the format is specified in docs/fuzz-formats.md and
fuzz/formats.h. A file with an END marker brackets each iteration
BEGIN..END; one with only BEGIN markers runs each iteration from a BEGIN to
the next.

Each named function is resolved to the site gen_sites.py picks for its
FIRST basic block under the same --strategy (default the arch's): the
first instruction (the second, if it is a MIPS delay slot) under "entry",
the first mid-block call, else the last code instruction, under
"terminator". So a marker is also a coverage site of the list; pass
--sites to check that against the actual site list (include/exclude
filters and --root slicing can leave the function without sites). A
marker in ARM Thumb code gets the alternate-state flag; offsets are from
module 0's link base and never carry the Thumb bit.

Usage:
  ./gen_boundaries.py <binary> <out.bounds> SYM[:kind[:depth_filter]] ...

  kind         : "begin" (default) or "end"
  depth_filter : "any" (default) or "zero". Every marker of a file has the
                 same one. "zero" keeps nested arrivals (recursion) inside
                 one iteration, so it only means something with END
                 markers; a begin-only file with it is refused unless
                 --force.

Example:
  ./gen_boundaries.py --sites fw.sites fw.elf fw.bounds fuzz_iteration:begin

Raw images take the same --raw --base --arch --bits --endian [--cpu]
[--symbols] options as gen_sites.py; offsets are then relative to --base.
"""
import argparse
import os
import struct
import sys

sys.dont_write_bytecode = True  # no __pycache__ next to the tools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_sites import (SITES_HEADER, SITES_MAGIC, SITES_RECORD,  # noqa: E402
                       check_strategy, default_strategy, get_all_functions,
                       get_bin_info, get_block_insns, is_branch_at, open_rizin,
                       pick_entry, pick_terminator, site_flags, split_block)
from image_resolve import (NotFound, add_image_args,  # noqa: E402
                           image_from_args, resolve_to_offset, symbol_entries)

# Boundary file layout (fuzz/formats.h, docs/fuzz-formats.md).
BOUNDS_MAGIC = b"UAMB"
BOUNDS_VERSION = 1
BOUNDS_HEADER = struct.Struct("<4sIII")       # magic, version, count, reserved
BOUNDS_RECORD = struct.Struct("<QBBB5s")      # offset, kind, depth, flags,
assert BOUNDS_HEADER.size == 16 and BOUNDS_RECORD.size == 16   # reserved[5]

KIND_MAP = {"begin": 0, "end": 1}
DEPTH_MAP = {"any": 0, "zero": 1}


def get_function_addrs(rz, image):
    """Return ({name: vaddr}, all_fns): symbol-table names as a fallback
    dict, and rizin's analysed functions (sym.foo, dbg.foo, fcn.*) as the
    [{"offset", "name", "bits", ...}] list resolve_to_offset() expects."""
    all_fns = []
    for f in get_all_functions(rz, image):
        name = f.get("name", "")
        if name:
            all_fns.append({"offset": int(f["offset"]), "name": name,
                            "bits": int(f.get("bits", 0)),
                            "size": int(f.get("size") or 0),
                            "maxbound": int(f.get("maxbound") or 0)})
    syms = {}
    for s in rz.cmdj("isj") or []:
        name = s.get("name", "")
        vaddr = s.get("vaddr")  # imports lack vaddr
        if name and vaddr:
            syms[name] = int(vaddr)
    for addr, name in image.symbols.items():
        syms.setdefault("sym." + name, addr)
    return syms, all_fns


def resolve(name, all_fns, syms):
    """Resolve `name` to an entry address. Uses the shared resolve_to_offset
    against the analysed functions first (so gen_boundaries and gen_sites
    resolve names identically, including prefix and suffix matching), then
    falls back to the symbol table for names rizin didn't analyse as
    functions. Only a not-found result falls back; an ambiguous match is an
    error here too. Raises ValueError if unresolvable or ambiguous."""
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
    raise ValueError(f"could not resolve '{name}' to a function or symbol")


def first_bb_site(rz, fn_addr, strategy, is_mips):
    """Return (address, function bits) of the site gen_sites.py picks for
    the first basic block of the function at fn_addr under `strategy`, or
    None."""
    blocks = rz.cmdj(f"afbj @ {fn_addr}") or []
    if not blocks:
        # A symbol-table name rizin did not analyse as a function.
        rz.cmd(f"af @ {fn_addr}")
        blocks = rz.cmdj(f"afbj @ {fn_addr}") or []
    if not blocks:
        return None
    # The function's *entry* basic block is the one starting at fn_addr.
    # Cold-split blocks can sit below the entry, so not just min(addr); fall
    # back to the smallest addr >= fn_addr, and only then to the smallest.
    first = next((b for b in blocks if int(b["addr"]) == int(fn_addr)), None)
    if first is None:
        forward = [b for b in blocks if int(b["addr"]) >= int(fn_addr)]
        first = (min(forward, key=lambda b: int(b["addr"])) if forward
                 else min(blocks, key=lambda b: int(b["addr"])))
    insns = get_block_insns(rz, first)
    if not insns:
        return None
    insns = split_block(insns, is_mips)[0]
    start = int(insns[0]["offset"])
    if strategy == "entry":
        pick = pick_entry(insns, is_mips and is_branch_at(rz, start - 4))
    else:
        pick = pick_terminator(insns)
    if pick is None:
        return None
    fi = rz.cmdj(f"afij @ {fn_addr}") or []
    bits = int(fi[0].get("bits", 0)) if fi else 0
    return int(pick["offset"]), bits


def site_list_offsets(path):
    """{offset: flags} of the module-0 records of a site list."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        sys.exit(f"error: --sites {path}: {e}")
    if len(data) < SITES_HEADER.size or data[:4] != SITES_MAGIC:
        sys.exit(f"error: {path}: not a site list")
    count = struct.unpack_from("<I", data, 8)[0]
    if len(data) < SITES_HEADER.size + count * SITES_RECORD.size:
        sys.exit(f"error: {path}: truncated site list")
    out = {}
    for i in range(count):
        off, module, _len, flags, _r, _orig = SITES_RECORD.unpack_from(
            data, SITES_HEADER.size + i * SITES_RECORD.size)
        if module == 0:
            out[off] = flags
    return out


def parse_spec(spec):
    """Parse SYM[:kind[:depth_filter]] -- kind in {begin,end} (default begin),
    depth_filter in {any,zero} (default any).

    For the common case of a name without '::', the fields are positional
    (SYM[:kind[:depth]]) with strict validation, so a typo like 'foo:beign'
    gives a precise "bad kind" error. When the name contains '::' (C++), kind
    and depth are matched from the RIGHT against their fixed vocab so the name
    survives: 'ns::fn' -> name 'ns::fn'; 'ns::fn:begin:zero' -> name 'ns::fn',
    kind begin, depth zero. The one ambiguous case -- a '::' name literally
    ending in ':begin'/':end' -- must be written with an explicit trailing
    kind (e.g. 'ns::weird:end:begin')."""
    if "::" not in spec:
        parts = spec.split(":")
        if len(parts) > 3:
            raise ValueError(f"too many ':' fields in spec {spec!r} "
                             f"(want SYM[:kind[:depth_filter]])")
        name = parts[0]
        kind = parts[1] if len(parts) > 1 else "begin"
        depth = parts[2] if len(parts) > 2 else "any"
        if not name:
            raise ValueError(f"empty symbol in spec {spec!r}")
        if kind not in KIND_MAP:
            raise ValueError(f"bad kind {kind!r} (want begin|end)")
        if depth not in DEPTH_MAP:
            raise ValueError(f"bad depth_filter {depth!r} (want any|zero)")
        return name, KIND_MAP[kind], DEPTH_MAP[depth]

    # C++ symbol: consume a trailing depth only when a kind precedes it, then a
    # trailing kind, and rejoin the rest (which keeps the '::') as the name.
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
        raise ValueError(f"empty symbol in spec {spec!r}")
    return name, KIND_MAP[kind], DEPTH_MAP[depth]


def check_records(records, force):
    """The file-level rules of a boundary file: a BEGIN marker, one depth
    filter for every marker, and no depth-zero filter in a begin-only file
    (begin-only mode never nests, so the filter would do nothing: the spec
    almost certainly isn't what was meant, e.g. a forgotten END marker)."""
    if not any(k == 0 for _, k, _, _ in records):
        sys.exit("error: no begin marker (a boundary file needs at least one)")
    if len({d for _, _, d, _ in records}) > 1:
        sys.exit("error: the markers have different depth filters; every "
                 "marker of a boundary file has the same one")
    if not any(k == 1 for _, k, _, _ in records) and any(d for _, _, d, _ in records):
        msg = "depth_filter=zero has no effect without END markers (begin-only file)"
        if not force:
            sys.exit(f"error: {msg}; drop ':zero', add an END marker, "
                     f"or pass --force")
        print(f"warning: {msg}", file=sys.stderr)


def write_bounds(path, records):
    with open(path, "wb") as f:
        f.write(BOUNDS_HEADER.pack(BOUNDS_MAGIC, BOUNDS_VERSION, len(records), 0))
        for offset, kind, depth, flags in records:
            f.write(BOUNDS_RECORD.pack(offset, kind, depth, flags, b"\x00" * 5))


def main():
    ap = argparse.ArgumentParser(
        description="Write an ember boundary file (UAMB) for uam-run "
                    "--cov-boundary: where a fuzz iteration begins and ends, "
                    "as markers on functions of a firmware image, chosen "
                    "with rizin. Format: docs/fuzz-formats.md.")
    ap.add_argument("binary",
                    help="the firmware image (module 0 of the site list): an "
                         "ELF, or a raw image with --raw")
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
                    help="marker site selection; the site list's gen_sites.py "
                         "--strategy (auto = the arch's default: entry on "
                         "MIPS, terminator elsewhere)")
    ap.add_argument("--force", action="store_true",
                    help="write a begin-only file that uses depth_filter=zero "
                         "anyway (it has no effect without END markers)")
    ap.add_argument("--verbose", "-v", action="store_true")
    add_image_args(ap)
    args = ap.parse_args()
    image = image_from_args(ap, args)
    sites = site_list_offsets(args.sites) if args.sites else None

    machine, entries, compressed = symbol_entries(args.binary, image)
    rz = open_rizin(args.binary, image, entries, machine)
    try:
        info = get_bin_info(rz, image)
        baddr, arch = info["baddr"], info["arch"]
        strategy = args.strategy if args.strategy != "auto" else default_strategy(arch)
        check_strategy(arch, strategy)
        syms, all_fns = get_function_addrs(rz, image)
        if args.verbose:
            print(f"image base: 0x{baddr:x} strategy={strategy}", file=sys.stderr)
            print(f"functions: {len(all_fns)}", file=sys.stderr)

        records = []
        for spec in args.specs:
            try:
                name, kind, depth = parse_spec(spec)
                fn_addr = resolve(name, all_fns, syms)
            except ValueError as e:
                sys.exit(f"error: {e}")
            if fn_addr in compressed:
                sys.exit(f"error: {name} is MIPS16e/microMIPS code, which rizin "
                         f"cannot decode; use the Ghidra script")
            hit = first_bb_site(rz, fn_addr, strategy, arch == "mips")
            if hit is None:
                sys.exit(f"error: could not find a first-block {strategy} site "
                         f"for {name} @ 0x{fn_addr:x}")
            site, bits = hit
            offset = site - baddr
            flags = site_flags(arch, bits)
            if sites is not None and sites.get(offset) != flags:
                why = ("is not a site" if offset not in sites
                       else "has another state flag than its site")
                sys.exit(f"error: marker {name} at off=0x{offset:x} {why} in "
                         f"{args.sites}")
            records.append((offset, kind, depth, flags))
            if args.verbose:
                print(f"  marker {name} entry=0x{fn_addr:x} site=0x{site:x} "
                      f"off=0x{offset:x} kind={kind} depth={depth} flags={flags}",
                      file=sys.stderr)
    finally:
        try:
            rz.quit()
        except Exception:
            pass

    check_records(records, args.force)
    write_bounds(args.output, records)
    print(f"wrote {len(records)} marker(s) to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
