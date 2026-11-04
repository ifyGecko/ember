#!/usr/bin/env python3
"""Turn a gpasm Intel hex file into an ELF image for uam-run, with gpasm's labels as symbols.

The PIC tools write hex files, not ELF; ember's loader and test scripts take ELF (load
segments, the entry point, a symbol table for readelf). Program memory addresses in the hex
file are already the bus addresses pic-ref uses (uam/isa/pic.h). Labels come from gpasm's
listing (its SYMBOL TABLE); on the 12- and 14-bit families gpasm counts them in words, so
--words doubles them.

usage: hex2elf.py IN.hex IN.lst OUT.elf ENTRY [--words] [--below ADDR]
  ENTRY    the reset vector (bus address)
  --below  drop hex data at and above ADDR (configuration words the board does not map)
"""
import re
import struct
import sys


def read_hex(path):
    mem, base = {}, 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ, data = b[0], (b[1] << 8) | b[2], b[3], b[4:4 + b[0]]
        if typ == 0:
            for i in range(n):
                mem[base + addr + i] = data[i]
        elif typ == 4:
            base = ((data[0] << 8) | data[1]) << 16
        elif typ == 2:
            base = ((data[0] << 8) | data[1]) << 4
    return mem


def read_labels(path, scale):
    """The ADDRESS symbols of gpasm's SYMBOL TABLE (labels), as bus addresses."""
    syms = {}
    for line in open(path, errors='replace'):
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s+ADDRESS\s+([0-9A-Fa-f]+)\s', line)
        if m:
            syms[m.group(1)] = int(m.group(2), 16) * scale
    return syms


def runs(mem):
    out, start, prev = [], None, None
    for a in sorted(mem):
        if start is None or a != prev + 1:
            if start is not None:
                out.append((start, bytes(mem[x] for x in range(start, prev + 1))))
            start = a
        prev = a
    if start is not None:
        out.append((start, bytes(mem[x] for x in range(start, prev + 1))))
    return out


def main():
    args = sys.argv[1:]
    words = '--words' in args
    below = None
    if '--below' in args:
        below = int(args[args.index('--below') + 1], 0)
    hexf, lstf, out, entry = args[0], args[1], args[2], int(args[3], 0)
    mem = read_hex(hexf)
    if below is not None:
        mem = {a: v for a, v in mem.items() if a < below}
    segs = runs(mem)
    labels = read_labels(lstf, 2 if words else 1)
    syms = sorted((v, k) for k, v in labels.items())

    strtab = b'\0' + b''.join(k.encode() + b'\0' for _, k in syms)
    symtab = b'\0' * 16
    off = 1
    for v, k in syms:
        symtab += struct.pack('<IIIBBH', off, v, 0, 0x12 if k == 'main' else 0x10, 0, 1)
        off += len(k) + 1
    shstr = b'\0.text\0.symtab\0.strtab\0.shstrtab\0'

    ehsize, phsize = 52, 32
    data_off = ehsize + phsize * len(segs)
    blob, phdrs, pos = b'', b'', data_off
    for a, d in segs:
        phdrs += struct.pack('<IIIIIIII', 1, pos, a, a, len(d), len(d), 5, 1)
        blob += d
        pos += len(d)
    symoff = pos
    stroff = symoff + len(symtab)
    shstroff = stroff + len(strtab)
    shoff = (shstroff + len(shstr) + 3) & ~3
    pad = shoff - (shstroff + len(shstr))
    first = segs[0] if segs else (0, b'')
    sh = [
        struct.pack('<10I', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        struct.pack('<10I', 1, 1, 6, first[0], data_off, len(first[1]), 0, 0, 2, 0),
        struct.pack('<10I', 7, 2, 0, 0, symoff, len(symtab), 3, 1, 4, 16),
        struct.pack('<10I', 15, 3, 0, 0, stroff, len(strtab), 0, 0, 1, 0),
        struct.pack('<10I', 23, 3, 0, 0, shstroff, len(shstr), 0, 0, 1, 0),
    ]
    ehdr = b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8
    ehdr += struct.pack('<HHIIIIIHHHHHH', 2, 0, 1, entry, ehsize, shoff, 0, ehsize, phsize, len(segs),
                        40, len(sh), 4)
    with open(out, 'wb') as f:
        f.write(ehdr + phdrs + blob + symtab + strtab + shstr + b'\0' * pad + b''.join(sh))


if __name__ == '__main__':
    main()
