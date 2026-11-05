#!/usr/bin/env python3
"""Lay a little-endian Xtensa image out the big-endian way, as a raw image.

    python3 -I le2be.py OBJDUMP IN.elf OUT.bin

No big-endian Xtensa toolchain is at hand, so the big-endian self-test is the little-endian
one, built with SELFTEST_BE (which adapts what differs: bit numbering, loads of bytes from
words), then rewritten here: every instruction of .vectors and .text (where objdump says
they are) with its fields in the big-endian order (opcodes.py's le_to_be), every 32-bit word
of .lit4 and .data byte-swapped, .strings as it is. Words no opcode matches keep their
fields (reserved stays reserved). OUT.bin starts at the lowest section address; the
caller passes it with --raw-base and the ELF's entry with --entry.
"""
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import opcodes  # noqa: E402

CODE = (".vectors", ".text")
WORDS = (".lit4", ".data")
BYTES = (".strings",)


def sections(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 1, "an ELF32 little-endian file"
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    hdrs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    out = {}
    for h in hdrs:
        name = d[strtab[4] + h[0]:].split(b"\0", 1)[0].decode()
        if h[1] == 1 and h[2] & 2:          # PROGBITS, ALLOC
            out[name] = (h[3], d[h[4]:h[4] + h[5]])
    return out


LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s")


def main():
    tool, src, dst = sys.argv[1:4]
    secs = sections(src)
    unknown = set(secs) - set(CODE + WORDS + BYTES)
    if unknown:
        sys.exit("le2be: sections le2be does not know: %s" % ", ".join(sorted(unknown)))
    base = min(a for a, _ in secs.values())
    end = max(a + len(b) for a, b in secs.values())
    img = bytearray(end - base)
    for name, (addr, data) in secs.items():
        if name in WORDS:
            assert len(data) % 4 == 0 and addr % 4 == 0, name
            data = b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
        img[addr - base:addr - base + len(data)] = data
    for name in CODE:
        if name not in secs:
            continue
        out = subprocess.run([tool, "-d", "-j", name, src], capture_output=True, text=True,
                             check=True).stdout
        for line in out.splitlines():
            m = LINE.match(line)
            if not m:
                continue
            addr, hx = int(m.group(1), 16), m.group(2)
            n = len(hx) // 2
            if n not in (2, 3):
                continue                     # .byte: data, left as it is
            w = int(hx, 16)
            ents = opcodes.lookup(w, n)
            fmt = ents[0]["fmt"] if ents else ("RRRN" if n == 2 else "RRR")
            be = opcodes.le_to_be(fmt, w).to_bytes(n, "big")
            img[addr - base:addr - base + n] = be
    with open(dst, "wb") as f:
        f.write(img)


if __name__ == "__main__":
    main()
