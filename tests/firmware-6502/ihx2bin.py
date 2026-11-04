#!/usr/bin/env python3
"""ihx2bin.py in.ihx out.bin: the 32 KiB ROM image (0x8000-0xFFFF) of an Intel hex file,
unwritten bytes 0xFF. Bytes outside the ROM are an error (6502-ref has RAM there)."""
import sys

BASE, SIZE = 0x8000, 0x8000
rom = bytearray(b"\xff" * SIZE)
upper = 0
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith(":"):
        continue
    raw = bytes.fromhex(line[1:])
    n, addr, kind, data = raw[0], raw[1] << 8 | raw[2], raw[3], raw[4:4 + raw[0]]
    if kind == 4:
        upper = data[0] << 8 | data[1]
    elif kind == 0:
        a = (upper << 16) + addr
        if a < BASE or a + n > BASE + SIZE:
            sys.exit("ihx2bin: %d bytes at 0x%x are outside the ROM" % (n, a))
        rom[a - BASE:a - BASE + n] = data
open(sys.argv[2], "wb").write(rom)
