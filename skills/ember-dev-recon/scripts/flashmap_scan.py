#!/usr/bin/env python3
"""flashmap_scan.py - find how a code stage sees its flash: partition tables and flash references.

Two independent views, both needed to label flash regions correctly:

1. Partition-table candidates (data). Firmware often keeps its layout as a small array of
   plain flash OFFSETS (not kseg addresses), e.g. {0x40000, 0x1c0000, 0x1d0000, 0x1e0000,
   0x1f0000}, and reaches each partition as flash_base + table[i]. Such a table never shows
   up as an absolute address, so labeling regions from absolute references alone misses or
   mislabels them. Candidates = runs of >= --min-run consecutive aligned words that are
   strictly increasing, erase-block aligned (--align) and below the flash size. Runs with a
   constant small stride (0x1000, 0x2000, ...) are reported separately as "regular" (often
   size tables, not layouts).

2. Flash references (code). Every lui-reconstructed address (via mmio_scan.scan) and every
   data word that falls in the flash window (kseg0 and kseg1 aliases of --flash-phys), folded
   by --flash-size when the part is mirrored, grouped by 64 KiB block with R/W/formed counts
   and the first code sites. A block only *read* at a fixed address is not proof of what it
   holds: label a partition from the code that WRITES it (and that code's strings), and
   cross-check against the table from view 1.

usage:
  flashmap_scan.py BLOB --base 0x80001000 [--endian be|le] [--flash-phys 0x1f000000]
                   [--flash-size 0x200000] [--window 0x1000000] [--align 0x1000] [--min-run 3]
                   [--limit BYTES]
"""
import argparse
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mmio_scan import scan  # noqa: E402


def table_candidates(ws, base, limit, align, min_run):
    runs, cur = [], []

    def flush():
        if len(cur) >= min_run:
            runs.append(list(cur))
        cur.clear()

    for i, v in enumerate(ws):
        ok = 0 < v < limit and v % align == 0
        if ok and (not cur or v > cur[-1][1]):
            cur.append((i, v))
        else:
            flush()
            if ok:
                cur.append((i, v))
    flush()
    out = []
    for r in runs:
        vals = [v for _, v in r]
        strides = {b - a for a, b in zip(vals, vals[1:])}
        regular = len(strides) == 1 and vals[0] == min(strides)
        score = sum(1 for v in vals if v % 0x10000 == 0) + len(vals)
        out.append((regular, -score, base + 4 * r[0][0], vals))
    out.sort()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="flash partition-table and flash-reference scanner")
    ap.add_argument("blob")
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--endian", default="be", choices=["be", "le"])
    ap.add_argument("--flash-phys", type=lambda x: int(x, 0), default=0x1F000000)
    ap.add_argument("--flash-size", type=lambda x: int(x, 0), default=0,
                    help="part size; offsets are folded modulo this (mirrors). 0 = don't fold")
    ap.add_argument("--window", type=lambda x: int(x, 0), default=0x1000000,
                    help="size of the flash window in the physical map")
    ap.add_argument("--align", type=lambda x: int(x, 0), default=0x1000)
    ap.add_argument("--min-run", type=int, default=3)
    ap.add_argument("--limit", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args(argv)

    data = open(a.blob, "rb").read()
    if a.limit:
        data = data[:a.limit]
    n = len(data) // 4
    ws = struct.unpack_from(("<" if a.endian == "le" else ">") + "%dI" % n, data, 0)
    size_limit = a.flash_size or a.window

    print("== partition-table candidates (runs of increasing, %#x-aligned offsets < %#x) =="
          % (a.align, size_limit))
    cands = table_candidates(ws, a.base, size_limit, a.align, a.min_run)
    shown = 0
    for regular, _neg, va, vals in cands:
        if regular:
            continue
        print("  @0x%08x  %s" % (va, " ".join("0x%x" % v for v in vals)))
        shown += 1
        if shown >= a.top:
            break
    if not shown:
        print("  (none)")
    reg = [c for c in cands if c[0]]
    if reg:
        print("  regular-stride runs (often size/sector tables, not layouts): %d, e.g. %s"
              % (len(reg), " ".join("@0x%08x" % c[2] for c in reg[:5])))
    print("  -> xref each candidate (Ghidra get_xrefs_to); the accessor functions name the "
          "partitions")

    def flash_off(addr):
        phys = addr & 0x1FFFFFFF if 0x80000000 <= addr <= 0xBFFFFFFF else addr
        if not (a.flash_phys <= phys < a.flash_phys + a.window):
            return None
        off = phys - a.flash_phys
        return off % a.flash_size if a.flash_size else off

    blocks = defaultdict(lambda: {"R": 0, "W": 0, "A": 0, "D": 0, "offs": Counter(), "sites": []})
    for addr, kind, pc in scan(data, a.endian, a.base):
        off = flash_off(addr)
        if off is None:
            continue
        b = blocks[off & ~0xFFFF]
        b[kind] += 1
        b["offs"][off] += 1
        if len(b["sites"]) < 3:
            b["sites"].append(pc)
    for i, v in enumerate(ws):                       # data words pointing into the window
        if v >> 28 in (0x9, 0xB) and v % 4 == 0:     # unaligned values are mostly noise
            off = flash_off(v)
            if off is not None:
                b = blocks[off & ~0xFFFF]
                b["D"] += 1
                b["offs"][off] += 1

    print("\n== flash references by 64 KiB block (offsets%s) =="
          % (" folded mod %#x" % a.flash_size if a.flash_size else ""))
    print("  block     R    W    formed data  offsets (off:count)          first code sites")
    for blk in sorted(blocks):
        b = blocks[blk]
        offs = " ".join("0x%x:%d" % (o, c) for o, c in sorted(b["offs"].most_common(4)))
        print("  0x%06x  %-4d %-4d %-6d %-4d  %-28s %s" % (
            blk, b["R"], b["W"], b["A"], b["D"], offs,
            " ".join("0x%08x" % s for s in b["sites"])))
    print("  -> 'formed' blocks are addresses passed to helpers (read/erase/write routines):\n"
          "     label each block from the function that writes it and that function's strings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
