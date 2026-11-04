#!/usr/bin/env python3
"""Lay out the CIP-51 self-test image as the part's flash is: the common area (the
self-test, below 0x8000), then 32 KiB blocks 1-3 (mcs51-ref's code banking: block n at
n * 0x8000). Each block holds bank.asm assembled for it; block 1 is also the window's
own addresses, 0x8000-0xFFFF.

usage: mkimage.py OUT.bin MAIN.bin BANK1.bin BANK2.bin BANK3.bin
"""
import sys

out, main, banks = sys.argv[1], sys.argv[2], sys.argv[3:]
img = bytearray(b"\xff" * 0x20000)
m = open(main, "rb").read()
if len(m) > 0x8000:
    sys.exit("mkimage.py: the self-test reaches the banked window (%#x bytes)" % len(m))
img[:len(m)] = m
for n, path in enumerate(banks, 1):
    b = open(path, "rb").read()[0x8000:]
    img[n * 0x8000:n * 0x8000 + len(b)] = b
open(out, "wb").write(img)
