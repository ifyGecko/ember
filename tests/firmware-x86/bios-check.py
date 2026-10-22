#!/usr/bin/env python3
"""bios-check.py - a legacy BIOS boot on x86_64-ref's generic legacy devices.

    bios-check.py RUNNER BIOSROM

Runs biosrom.bin (--image bios=) with a floppy whose boot sector reports to the
test finisher, an IDE disk whose sector n holds bytes n, a VGA and a printer,
on the interpreter and on the JIT. The run must end with status 0 (the boot
sector ran), the printer must have printed "BIOS OK" on the console, and the VGA
text screen (-o vgadump=1) must show it.
"""
import os
import subprocess
import sys
import tempfile

runner, rom = sys.argv[1:3]
fails = 0
with tempfile.TemporaryDirectory() as tmp:
    fd = os.path.join(tmp, "fd.img")
    boot = bytes([0x66, 0xB8, 0x55, 0x55, 0x00, 0x00,     # mov eax, 0x5555
                  0xBA, 0x00, 0x07,                       # mov dx, 0x700
                  0x66, 0xEF,                             # out dx, eax
                  0xF4, 0xEB, 0xFE])                      # hlt; jmp $
    sector = boot + bytes(510 - len(boot)) + b"\x55\xAA"
    with open(fd, "wb") as f:
        f.write(sector + bytes(1474560 - 512))
    hd = os.path.join(tmp, "hd.img")
    with open(hd, "wb") as f:
        for n in range(128):
            f.write(bytes([n]) * 512)
    for backend in ([], ["--jit"]):
        cmd = [runner, "-b", "x86_64-ref", "--image", "bios=" + rom, "-o", "floppy=" + fd,
               "-o", "ide0=" + hd, "-o", "vga=1", "-o", "vgadump=1", "-o", "lpt=1",
               "-n", "2000000"] + backend
        p = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
        name = "jit" if backend else "interp"
        out, err = p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")
        ok = p.returncode == 0 and "BIOS OK" in out and "\nBIOS OK" in "\n" + err
        print(("[ ok ] " if ok else "[FAIL] ") + "legacy BIOS boot (%s)" % name)
        if not ok:
            fails += 1
            print("  status %d\n  stdout: %r\n  stderr: %s" % (p.returncode, out[-200:], err[-600:]))
sys.exit(1 if fails else 0)
