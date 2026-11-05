#!/usr/bin/env python3
"""mkimages.py FW64 OUTDIR - the PC fixture's flash images and SPD (synthetic test data).

fw.bin   128 KiB boot flash: 64 KiB of 0xFF, then the firmware (the top 64 KiB)
spi.bin  1 MiB SPI flash: a flash descriptor (regions: descriptor 0x0-0xFFF, ME
         0x1000-0x7FFFF, BIOS 0x80000-0xFFFFF), the BIOS region ending with fw.bin and holding
         a function at flash 0x81000 (mov eax, 1; ret) that the firmware rewrites in place
spd.bin  512-byte DDR4 SPD: byte 2 = 0x0C (DDR4), page 1 byte 0x40 = 0x80
"""
import struct
import sys

fw64, out = open(sys.argv[1], "rb").read(), sys.argv[2]
assert len(fw64) == 0x10000, len(fw64)
fw = b"\xff" * 0x10000 + fw64
open(out + "/fw.bin", "wb").write(fw)

spi = bytearray(b"\xff" * 0x100000)
spi[0x10:0x14] = struct.pack("<I", 0x0FF0A55A)              # FLVALSIG
spi[0x14:0x18] = struct.pack("<I", 0x00040003)              # FLMAP0: FCBA 0x30, FRBA 0x40
spi[0x18:0x1C] = struct.pack("<I", 0x00000008)              # FLMAP1: FMBA 0x80
for i, (base, limit) in enumerate([(0x0, 0x0), (0x80, 0xFF), (0x1, 0x7F), (0x7FFF, 0), (0x7FFF, 0)]):
    spi[0x40 + 4 * i:0x44 + 4 * i] = struct.pack("<I", base | limit << 16)
spi[0x1000:0x80000] = b"\xa5" * (0x80000 - 0x1000)         # "ME" region
spi[0x81000:0x81006] = bytes([0xB8, 0x01, 0, 0, 0, 0xC3])    # mov eax, 1 ; ret
spi[-len(fw):] = fw
open(out + "/spi.bin", "wb").write(spi)

spd = bytearray(512)
spd[2] = 0x0C
spd[0x140] = 0x80
open(out + "/spd.bin", "wb").write(spd)
