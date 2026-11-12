#!/usr/bin/env python3
"""mk_lists.py ELF OUTDIR [--no-bases] [--bad-orig] - test site lists and boundary files.

Writes OUTDIR/sites.uams (a site list: a site at every function symbol, in the alternate
state with 2 bytes checked in an ARM Thumb function (bit 0 of the symbol) and 4 bytes
otherwise, orig[] taken from the image, module 0 named after the file, with the module base
table unless --no-bases), OUTDIR/begin.uamb (one BEGIN marker at fuzz_iteration) and
OUTDIR/paired.uamb (BEGIN at fuzz_iteration, END markers at fuzz_panic and trap_handler, any
depth). --bad-orig corrupts the first site's orig[] (for --cov-strict).

Written from docs/fuzz-formats.md alone, apart from fuzz/tools' generators.
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
                syms[name] = value
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
    # An ARM Thumb function (bit 0 of its symbol) gets an alternate-state site at its even
    # address, checking 2 bytes; everything else 4.
    sites = sorted(set(syms.values()))
    records = []
    for value in sites:
        addr, n, alt = value & ~1, 2 if value & 1 else 4, value & 1
        orig = file_bytes(data, segs, addr, n)
        if orig is not None:
            records.append((addr - base, n, alt, orig + b"\0" * (4 - n)))
    if "--bad-orig" in flags:
        off, n, alt, orig = records[0]
        records[0] = (off, n, alt, bytes(b ^ 0xFF for b in orig[:n]) + orig[n:])
    name = os.path.basename(elf).encode() + b"\0"
    with_bases = "--no-bases" not in flags
    mod_off = 24 + 16 * len(records)
    hdr = b"UAMS" + struct.pack("<IIBBHII", 1, len(records), 1 if with_bases else 0, 1, 0,
                                mod_off, len(name))
    body = b"".join(struct.pack("<QBBBB", off, 0, n, alt, 0) + orig for off, n, alt, orig in records)
    blob = hdr + body + name
    if with_bases:
        blob += b"\0" * ((-len(blob)) % 8) + struct.pack("<Q", base)
    open(os.path.join(out, "sites.uams"), "wb").write(blob)

    def marker(value, kind):
        return struct.pack("<QBBB5x", (value & ~1) - base, kind, 0, value & 1)

    begin = marker(syms["fuzz_iteration"], 0)
    open(os.path.join(out, "begin.uamb"), "wb").write(b"UAMB" + struct.pack("<III", 1, 1, 0) + begin)
    ends = [marker(syms[n], 1) for n in ("fuzz_panic", "trap_handler")]
    paired = b"UAMB" + struct.pack("<III", 1, 1 + len(ends), 0) + begin + b"".join(ends)
    open(os.path.join(out, "paired.uamb"), "wb").write(paired)
    print(len(records))


main()
