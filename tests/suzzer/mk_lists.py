#!/usr/bin/env python3
"""mk_lists.py ELF OUTDIR [--no-bases] [--bad-orig] - suzzer-format test lists from an ELF.

Writes OUTDIR/sites.lcv3 (an LCV3 patch list: one 4-byte site at every function symbol,
orig[] taken from the image, module 0 named after the file, with the S2 module base
table unless --no-bases) and OUTDIR/begin.lcvb (an LCVB1 file with one BEGIN marker at
fuzz_iteration) and OUTDIR/paired.lcvb (BEGIN at fuzz_iteration, END markers at
fuzz_panic and trap_handler, any depth). --bad-orig corrupts the first site's orig[] (for --cov-strict).

Follows suzzer's docs/FORMATS.md; it needs nothing from suzzer.
"""
import os
import struct
import sys


def elf32(data):
    if data[:4] != b"\x7fELF" or data[4] != 1:
        sys.exit("mk_lists.py: not a 32-bit ELF")
    e = "<" if data[5] == 1 else ">"
    shoff, = struct.unpack_from(e + "I", data, 32)
    phoff, = struct.unpack_from(e + "I", data, 28)
    phentsize, phnum, shentsize, shnum = struct.unpack_from(e + "HHHH", data, 42)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _p, p_filesz, _m, _f, _a = struct.unpack_from(
            e + "8I", data, phoff + i * phentsize)
        if p_type == 1:
            segs.append((p_vaddr, p_off, p_filesz))
    secs = [struct.unpack_from(e + "10I", data, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in secs:
        if s[1] != 2:   # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for k in range(s[5] // 16):
            name_off, value, _size, info, _o, _ndx = struct.unpack_from(e + "IIIBBH", data, s[4] + 16 * k)
            if info & 0xF == 2:   # STT_FUNC
                start = strtab[4] + name_off
                name = data[start:data.index(b"\0", start)].decode()
                syms[name] = value & ~1
    return segs, syms


def file_bytes(data, segs, addr, n):
    for vaddr, off, size in segs:
        if vaddr <= addr and addr + n <= vaddr + size:
            return data[off + addr - vaddr: off + addr - vaddr + n]
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    elf, out = args
    data = open(elf, "rb").read()
    segs, syms = elf32(data)
    base = min(v for v, _o, _s in segs)
    sites = sorted(set(syms.values()))
    records = []
    for addr in sites:
        orig = file_bytes(data, segs, addr, 4)
        if orig is not None:
            records.append((addr - base, orig))
    if "--bad-orig" in flags:
        off, orig = records[0]
        records[0] = (off, bytes(b ^ 0xFF for b in orig))
    name = os.path.basename(elf).encode() + b"\0"
    with_bases = "--no-bases" not in flags
    mod_off = 24 + 14 * len(records)
    hdr = b"LCV3" + struct.pack("<IIBBBBII", 3, len(records), 0, 1 if with_bases else 0, 1, 0,
                                mod_off, len(name))
    body = b"".join(struct.pack("<QBB", off, 4, 0) + orig for off, orig in records)
    blob = hdr + body + name
    if with_bases:
        blob += b"\0" * ((-len(blob)) % 8) + struct.pack("<Q", base)
    open(os.path.join(out, "sites.lcv3"), "wb").write(blob)

    begin = syms["fuzz_iteration"] - base
    lcvb = b"LCVB" + struct.pack("<III", 1, 1, 0) + struct.pack("<QBB6x", begin, 0, 0)
    open(os.path.join(out, "begin.lcvb"), "wb").write(lcvb)

    ends = [syms[n] - base for n in ("fuzz_panic", "trap_handler")]
    recs = struct.pack("<QBB6x", begin, 0, 0) + b"".join(struct.pack("<QBB6x", e, 1, 0) for e in ends)
    paired = b"LCVB" + struct.pack("<III", 1, 1 + len(ends), 0) + recs
    open(os.path.join(out, "paired.lcvb"), "wb").write(paired)
    print(len(records))


main()
