#!/usr/bin/env python3
"""fwinfo.py - what is this file, and which architecture? Stdlib only, no Ghidra.

The first look at an image, before basefind/Ghidra:
  * ELF: class, byte order, e_machine -> architecture and ember ISA, the flags that matter
    (MIPS revision and MIPS16e/microMIPS, ARM BE8/float ABI, RISC-V C/float ABI, AVR core
    family), entry (Thumb/compressed ISA bit), PT_LOAD segments, section names.
  * Intel HEX (.hex/.ihx): address ranges, start address, and the far ranges that are not
    program memory (PIC configuration words and EEPROM, AVR fuses). --bin OUT writes the
    program-memory part as a raw binary (ember loads raw binaries and ELF, not HEX).
  * Espressif images (magic 0xE9, ESP8266 v2 0xEA) and ESP32-family flash dumps (partition
    table at 0x8000): chip, entry, segments and the memory each one loads into. --elf OUT
    writes an ELF32 with one PT_LOAD per segment (Ghidra's ELF loader and uam-run load it).
  * raw: per-architecture instruction fingerprints (hits per KiB, both byte orders: MIPS,
    PowerPC, ARM/Thumb, RV32, Xtensa, AVR, AVR32, PIC18, x86) and
    the vector-table shapes of the 8-bit parts (AVR, 8051, Z80, 6502 vectors at the top,
    PIC word widths, ARM/Cortex-M vector tables). A hint, not proof: confirm in Ghidra.

usage:
  fwinfo.py IMAGE [--json] [--bin OUT] [--elf OUT] [--segments DIR] [--max-gap 0x10000]
  --bin OUT       HEX: write the lowest group of ranges (gaps <= --max-gap filled with 0xFF)
  --elf OUT       Espressif image: write it as an ELF32 (entry + one PT_LOAD per segment;
                  of a flash dump, the last app image found)
  --segments DIR  ELF / Espressif image: write each loadable segment as DIR/seg-<addr>.bin
                  (ELF: the virtual address, the one code runs at)

The last line of the text report is `suggest: arch=<recon.sh --arch> ...` when the format
names the architecture (recon.sh reads it with --json).
"""
import argparse
import json
import os
import re
import struct
import sys

# ---- architecture names: recon.sh --arch value -> (ember ISA, reference board, Ghidra id) ----
ARCH = {
    "mips32/be":   ("uam_isa_mips32be", "mips-ref", "MIPS:BE:32:default"),
    "mips32/le":   ("uam_isa_mips32le", "mipsel-ref (pic32-ref for PIC32)", "MIPS:LE:32:default"),
    "rv32/le":     ("uam_isa_rv32", "rv32-ref", "RISCV:LE:32:default"),
    "arm/le":      ("uam_isa_arm", "arm-ref", "ARM:LE:32:v7"),
    "armeb/be":    ("uam_isa_armeb", "armeb-ref", "ARM:LEBE:32:v7LEInstruction"),
    "x86_64/le":   ("uam_isa_x86_64", "x86_64-ref", "x86:LE:64:default"),
    "x86_32/le":   ("uam_isa_x86_64", "x86_64-ref", "x86:LE:32:default"),
    "ppc32/be":    ("uam_isa_ppc32", "ppc-ref", "PowerPC:BE:32:default"),
    "avr/le":      ("uam_isa_avr", "avr-ref", "avr8:LE:16:default"),
    "avr32/be":    ("uam_isa_avr32", "avr32-ref", "avr32:BE:32:default"),
    "pic-baseline/le": ("uam_isa_pic", "pic-ref -o model=baseline", "PIC-16:LE:16:PIC-16C5x"),
    "pic-mid/le":  ("uam_isa_pic", "pic-ref -o model=mid", "PIC-16:LE:16:PIC-16"),
    "pic-enhanced/le": ("uam_isa_pic", "pic-ref -o model=enhanced", "PIC-16:LE:16:PIC-16F"),
    "pic18/le":    ("uam_isa_pic", "pic-ref -o model=pic18", "PIC-18:LE:24:PIC-18"),
    "mcs51/be":    ("uam_isa_mcs51", "mcs51-ref", "8051:BE:16:default"),
    "6502/le":     ("uam_isa_m6502", "6502-ref", "6502:LE:16:default"),
    "65c02/le":    ("uam_isa_m6502", "6502-ref -o model=65c02", "65C02:LE:16:default"),
    "z80/le":      ("uam_isa_z80", "z80-ref", "z80:LE:16:default"),
    "xtensa/le":   ("uam_isa_xtensa", "xtensa-ref", "Xtensa:LE:32:default"),
    "xtensaeb/be": ("uam_isa_xtensaeb", "xtensaeb-ref", "Xtensa:BE:32:default"),
}

# ELF e_machine -> (name, recon arch or None when ember has no ISA module for it)
EM = {
    2: ("SPARC", None), 3: ("Intel 80386", "x86_32"), 4: ("Motorola 68000", None),
    8: ("MIPS", "mips32"), 10: ("MIPS RS3000 LE", "mips32"), 20: ("PowerPC", "ppc32"),
    21: ("PowerPC64", None), 40: ("ARM (AArch32)", "arm"), 42: ("SuperH", None),
    62: ("x86-64", "x86_64"), 83: ("AVR (8-bit)", "avr"), 94: ("Xtensa", "xtensa"),
    105: ("TI MSP430", None), 165: ("Intel 8051", "mcs51"), 183: ("AArch64", None),
    185: ("AVR32", "avr32"), 220: ("Zilog Z80", "z80"), 243: ("RISC-V", "rv32"),
    0x18AD: ("AVR32 (old number)", "avr32"),
}

# AVR ELF e_flags & 0x7F (the GNU tools' -mmcu architecture) -> ember core family
AVR_MACH = {
    1: ("avr1", "e", "no SRAM, no MUL (AVR core; ember: model e without MUL)"),
    2: ("avr2", "e", "AVR core, <= 8 KiB, no MUL"), 25: ("avr25", "e", "AVRe w/o MUL, MOVW/LPM Z+"),
    3: ("avr3", "e", "JMP/CALL, no MUL"), 31: ("avr31", "e", "128 KiB, ELPM, no MUL"),
    35: ("avr35", "e", "JMP/CALL + MOVW, no MUL"), 4: ("avr4", "e", "AVRe <= 8 KiB"),
    5: ("avr5", "e", "AVRe 16-64 KiB"), 51: ("avr51", "e", "AVRe 128 KiB with ELPM/RAMPZ"),
    6: ("avr6", "eplus", "AVRe+ 256 KiB, 22-bit PC, EIND"),
    100: ("avrtiny", "rc", "AVRrc reduced core (16 registers)"),
    102: ("avrxmega2", "xm|xt", "XMEGA (xm) or AVR Dx/tinyAVR 0/1 (xt): check the part"),
    103: ("avrxmega3", "xt", "flash mapped in data space: tinyAVR 0/1/2, megaAVR 0 (AVRxt)"),
    104: ("avrxmega4", "xm|xt", "XMEGA (xm) or AVR Dx (xt): check the part"),
    105: ("avrxmega5", "xm", "XMEGA, RAMPD"), 106: ("avrxmega6", "xm", "XMEGA > 128 KiB"),
    107: ("avrxmega7", "xm", "XMEGA > 128 KiB, RAMPD"),
}

ESP_CHIPS = {0x0000: "ESP32", 0x0002: "ESP32-S2", 0x0005: "ESP32-C3", 0x0009: "ESP32-S3",
             0x000C: "ESP32-C2", 0x000D: "ESP32-C6", 0x0010: "ESP32-H2", 0x0012: "ESP32-P4"}
ESP_RISCV = {"ESP32-C3", "ESP32-C2", "ESP32-C6", "ESP32-H2", "ESP32-P4"}

# Coarse memory maps (Espressif technical reference manuals), to name what a segment loads.
ESP_MAP = {
    "ESP8266": [(0x3FFE8000, 0x40000000, "DRAM"), (0x40000000, 0x40010000, "ROM"),
                (0x40100000, 0x4010C000, "IRAM"), (0x40200000, 0x40300000, "IROM (flash)"),
                (0x60000000, 0x60002000, "peripherals/RTC")],
    "ESP32": [(0x3F400000, 0x3F800000, "DROM (flash)"), (0x3F800000, 0x3FC00000, "ext. RAM"),
              (0x3FF80000, 0x3FF82000, "RTC FAST (data)"), (0x3FFAE000, 0x40000000, "DRAM"),
              (0x40000000, 0x40060000, "ROM"), (0x40070000, 0x400C0000, "IRAM"),
              (0x400C0000, 0x400C2000, "RTC FAST (code)"), (0x400C2000, 0x40C00000, "IROM (flash)"),
              (0x50000000, 0x50002000, "RTC SLOW")],
    "ESP32-S3": [(0x3C000000, 0x3E000000, "DROM (flash)"), (0x3FC88000, 0x3FD00000, "DRAM"),
                 (0x40000000, 0x40060000, "ROM"), (0x40370000, 0x403E0000, "IRAM"),
                 (0x42000000, 0x44000000, "IROM (flash)"), (0x50000000, 0x50002000, "RTC SLOW"),
                 (0x600FE000, 0x60100000, "RTC FAST")],
}


def suggestion(arch, endian, **extra):
    key = "%s/%s" % (arch, endian)
    isa, board, lang = ARCH.get(key, (None, None, None))
    d = {"arch": arch, "endian": endian, "isa": isa, "board": board, "ghidra": lang}
    d.update({k: v for k, v in extra.items() if v is not None})
    return d


# ---------------------------------------------------------------------------- ELF
def parse_elf(d):
    cls, data = d[4], d[5]
    if cls not in (1, 2) or data not in (1, 2):
        return {"format": "ELF (malformed header)"}
    e = "<" if data == 1 else ">"
    if cls == 1:
        (etype, mach, _v, entry, phoff, shoff, flags, _eh, phes, phn, shes, shn,
         shstr) = struct.unpack_from(e + "HHIIIIIHHHHHH", d, 16)
    else:
        (etype, mach, _v, entry, phoff, shoff, flags, _eh, phes, phn, shes, shn,
         shstr) = struct.unpack_from(e + "HHIQQQIHHHHHH", d, 16)
    r = {"format": "ELF%d %s" % (32 * cls, "little-endian" if data == 1 else "big-endian"),
         "e_machine": mach, "machine": EM.get(mach, ("unknown", None))[0],
         "e_flags": "0x%x" % flags, "entry": "0x%x" % entry, "type": etype, "notes": []}
    segs = []
    for i in range(phn):
        o = phoff + i * phes
        if o + phes > len(d):
            break
        if cls == 1:
            pt, off, va, pa, fs, ms, fl, _al = struct.unpack_from(e + "IIIIIIII", d, o)
        else:
            pt, fl, off, va, pa, fs, ms, _al = struct.unpack_from(e + "IIQQQQQQ", d, o)
        if pt == 1:
            segs.append({"vaddr": va, "paddr": pa, "offset": off, "filesz": fs, "memsz": ms,
                         "flags": "".join(c if fl & b else "-" for c, b in (("r", 4), ("w", 2), ("x", 1)))})
    r["segments"] = segs
    names = []
    if shoff and shn and shstr < shn and shoff + shn * shes <= len(d):
        fmt = e + ("IIIIIIIIII" if cls == 1 else "IIQQQQIIQQ")
        hdrs = [struct.unpack_from(fmt, d, shoff + i * shes) for i in range(shn)]
        so = hdrs[shstr][4]
        for h in hdrs[1:]:
            n = d[so + h[0]:so + h[0] + 64].split(b"\0", 1)[0].decode("ascii", "replace")
            if n:
                names.append(n)
    r["sections"] = names
    arch = EM.get(mach, ("unknown", None))[1]
    endian = "le" if data == 1 else "be"
    sugg = {}
    if mach in (8, 10):
        rev = {5: "MIPS32", 6: "MIPS64", 7: "MIPS32r2", 8: "MIPS64r2", 9: "MIPS32r6",
               10: "MIPS64r6", 0: "MIPS I", 1: "MIPS II", 2: "MIPS III"}.get(flags >> 28, "?")
        ases = [n for b, n in ((0x04000000, "MIPS16e"), (0x02000000, "microMIPS"),
                               (0x08000000, "MDMX")) if flags & b]
        r["notes"].append("ISA %s%s" % (rev, (" + " + ", ".join(ases)) if ases else ""))
        if cls == 2 or flags >> 28 in (6, 8, 9, 10):
            r["notes"].append("ember: MIPS32r2 only (no MIPS64, no release 6)")
        lang = None
        if flags & 0x02000000:
            lang = "MIPS:%s:32:micro" % endian.upper()
        if entry & 1:
            r["notes"].append("entry bit 0 set: starts in the compressed ISA (MIPS16e/microMIPS)")
        sugg = suggestion("mips32", endian, ghidra=lang)
    elif mach == 40:
        be8 = bool(flags & 0x00800000)
        r["notes"].append("EABI v%d%s%s" % (flags >> 24, ", BE8" if be8 else "",
                          ", hard-float" if flags & 0x400 else (", soft-float" if flags & 0x200 else "")))
        if entry & 1:
            r["notes"].append("entry bit 0 set: Thumb")
        if endian == "be" and not be8:
            r["notes"].append("big-endian without BE8: BE32 (ember has no BE32)")
            arch = None
        elif endian == "be":
            arch, endian = "armeb", "be"
        if flags & 0x400:
            r["notes"].append("hard-float ABI: VFP registers carry arguments (ember has no VFP)")
        if any(n.startswith(".isr_vector") for n in names):
            r["notes"].append(".isr_vector section: a Cortex-M (ARMv7-M) image? ember has no ARMv7-M")
        sugg = suggestion(arch, endian) if arch else {}
    elif mach == 243:
        if cls == 2:
            r["notes"].append("RV64 (ember: RV32 only)")
            arch = None
        abi = {0: "soft-float", 2: "single-float", 4: "double-float", 6: "quad-float"}[flags & 6]
        r["notes"].append("%sABI %s%s" % ("C extension, " if flags & 1 else "", abi,
                                           ", RVE" if flags & 8 else ""))
        if flags & 6:
            r["notes"].append("float ABI: needs F/D (ember: RV32IMAC)")
        sugg = suggestion("rv32", "le") if arch else {}
    elif mach == 83:
        m = flags & 0x7F
        name, fam, why = AVR_MACH.get(m, ("avr?%d" % m, "?", "unknown architecture number"))
        r["notes"].append("%s: ember model %s (%s)" % (name, fam, why))
        lang = ("avr8:LE:16:atmega256" if m == 6 else "avr8:LE:24:xmega" if 101 < m < 108
                else "avr8:LE:16:extended" if m == 51 else "avr8:LE:16:default")
        if m == 100:
            r["notes"].append("Ghidra's AVR8 languages decode the reduced core's 16-bit LDS/STS as AVRe ones")
        sugg = suggestion("avr", "le", ghidra=lang, model=fam)
    elif mach in (20,):
        r["notes"].append("32-bit PowerPC; classic (OEA) or Book E: read the MSR/SPR setup (IVORs, MAS)")
        sugg = suggestion("ppc32", "be") if endian == "be" else {}
        if endian == "le":
            r["notes"].append("little-endian PowerPC: ember's PowerPC is big-endian")
    elif mach == 94:
        sugg = suggestion("xtensa" if endian == "le" else "xtensaeb", endian)
        for n in names:
            if n in (".irom0.text", ".flash.text", ".iram0.text", ".iram1.text", ".dram0.data",
                     ".flash.rodata", ".rtc.text"):
                r["notes"].append("Espressif sections (%s ...): ESP8266 NONOS/RTOS SDK (.irom0.text) "
                                  "or ESP-IDF (.flash.text, .iram0.text)" % n)
                break
    elif arch:
        if arch == "x86_32" or arch == "x86_64":
            sugg = suggestion(arch, "le")
        else:
            sugg = suggestion(arch, endian)
    if mach in EM and EM[mach][1] is None:
        r["notes"].append("ember has no ISA module for %s" % EM[mach][0])
    r["suggest"] = sugg
    return r


# ---------------------------------------------------------------------------- Intel HEX
def parse_ihex(text):
    mem, base, start = {}, 0, None
    for ln, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError("line %d: not a HEX record" % ln)
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], b[1] << 8 | b[2], b[3]
        if len(b) != n + 5 or sum(b) & 0xFF:
            raise ValueError("line %d: bad length or checksum" % ln)
        payload = b[4:4 + n]
        if typ == 0:
            for i, v in enumerate(payload):
                mem[base + addr + i] = v
        elif typ == 1:
            break
        elif typ == 2:
            base = (payload[0] << 8 | payload[1]) << 4
        elif typ == 4:
            base = (payload[0] << 8 | payload[1]) << 16
        elif typ == 3:
            start = "%04x:%04x" % (payload[0] << 8 | payload[1], payload[2] << 8 | payload[3])
        elif typ == 5:
            start = "0x%x" % struct.unpack(">I", payload)[0]
    return mem, start


def ranges(addrs):
    out = []
    for a in sorted(addrs):
        if out and a == out[-1][1]:
            out[-1][1] = a + 1
        else:
            out.append([a, a + 1])
    return out


def hex_report(mem, start, max_gap):
    rs = ranges(mem)
    groups = []
    for lo, hi in rs:
        if groups and lo - groups[-1][-1][1] <= max_gap:
            groups[-1].append([lo, hi])
        else:
            groups.append([[lo, hi]])
    r = {"format": "Intel HEX", "start": start, "ranges": [("0x%x" % lo, "0x%x" % hi) for lo, hi in rs],
         "groups": [("0x%x" % g[0][0], "0x%x" % g[-1][1]) for g in groups], "notes": []}

    def word(a):
        return mem.get(a, 0xFF) | mem.get(a + 1, 0xFF) << 8

    far = [g for g in groups[1:]]
    for g in far:
        lo = g[0][0]
        hint = ""
        if 0x300000 <= lo < 0x300010:
            hint = "PIC18 configuration words (CONFIG1L-CONFIG7H)"
            if 0x300006 in mem:
                hint += "; CONFIG4L = 0x%02x: XINST %s" % (mem[0x300006], "on" if mem[0x300006] & 0x40 else "off")
        elif lo in (0x400E, 0x4000, 0x4006, 0x4008) or 0x4000 <= lo < 0x4010:
            hint = "mid-range PIC ID/configuration words (word 0x2000-0x2007; CONFIG = 0x%04x)" % word(0x400E)
        elif 0x10000 <= lo < 0x10014:
            hint = "enhanced mid-range PIC ID/configuration words (word 0x8000-0x8009)"
        elif lo in (0x4200,) or 0xF00000 <= lo < 0xF01000:
            hint = "PIC data EEPROM image"
        elif 0x810000 <= lo < 0x820000:
            hint = "AVR EEPROM section (GNU tools' 0x810000)"
        elif 0x820000 <= lo < 0x830000:
            hint = "AVR fuses (GNU tools' 0x820000)"
        elif 0x830000 <= lo < 0x840000:
            hint = "AVR lock bits (GNU tools' 0x830000)"
        r["notes"].append("range 0x%x-0x%x: %s" % (lo, g[-1][1], hint or "far from program memory: "
                                                    "configuration, EEPROM or another bank?"))
    if rs and rs[0][0] == 0 and 0x1FFE in mem and 0x2000 > rs[-1][1] - 1 >= 0x1FFE:
        r["notes"].append("a word at the top of a 4K-word space: a baseline PIC's reset word "
                          "or configuration word?")
    return r, groups


# ---------------------------------------------------------------------------- Espressif
def esp_region(chip, a):
    for lo, hi, n in ESP_MAP.get(chip, []):
        if lo <= a < hi:
            return n
    return "?"


def plausible_esp_addr(a):
    return 0x3F000000 <= a < 0x44000000 or 0x50000000 <= a < 0x50010000 or 0x60000000 <= a < 0x60100000


def parse_esp_image(d, off=0):
    """An image at d[off:] (magic 0xE9), or None. Returns dict with chip, entry, segments."""
    if off + 8 > len(d) or d[off] != 0xE9 or not 1 <= d[off + 1] <= 16:
        return None
    nseg, mode, sz = d[off + 1], d[off + 2], d[off + 3]
    entry = struct.unpack_from("<I", d, off + 4)[0]
    chip, hdr = None, 8
    a8 = struct.unpack_from("<I", d, off + 8)[0] if off + 12 <= len(d) else 0
    a24 = struct.unpack_from("<I", d, off + 24)[0] if off + 28 <= len(d) else 0
    if plausible_esp_addr(a8) and not plausible_esp_addr(a24):
        chip = "ESP8266"
    elif plausible_esp_addr(a24):
        cid = struct.unpack_from("<H", d, off + 12)[0]
        chip = ESP_CHIPS.get(cid, "ESP32-family chip id 0x%04x" % cid)
        hdr = 24
    else:
        return None
    segs, p = [], off + hdr
    for _ in range(nseg):
        if p + 8 > len(d):
            return None
        la, ln = struct.unpack_from("<II", d, p)
        if ln > len(d) - p - 8:
            return None
        segs.append({"addr": la, "size": ln, "offset": p + 8, "region": esp_region(chip, la)})
        p += 8 + ln
    sizes = {0: "1MB", 1: "2MB", 2: "4MB", 3: "8MB", 4: "16MB"} if chip != "ESP8266" else \
        {0: "512KB", 1: "256KB", 2: "1MB", 3: "2MB", 4: "4MB", 8: "8MB", 9: "16MB"}
    return {"chip": chip, "entry": entry, "segments": segs, "flash_mode": mode,
            "flash_size": sizes.get(sz >> 4, "?"), "end": p, "offset": off}


PART_TYPES = {0: {0x00: "factory", 0x20: "test"}, 1: {0x00: "ota", 0x01: "phy", 0x02: "nvs",
                                                       0x03: "coredump", 0x04: "nvs_keys",
                                                       0x05: "efuse", 0x81: "fat", 0x82: "spiffs"}}


def parse_partitions(d, off=0x8000):
    parts = []
    for p in range(off, min(len(d), off + 0xC00), 32):
        if d[p:p + 2] != b"\xaa\x50":
            break
        typ, sub, poff, size = d[p + 2], d[p + 3], *struct.unpack_from("<II", d, p + 4)
        label = d[p + 12:p + 28].split(b"\0", 1)[0].decode("ascii", "replace")
        name = ("app" if typ == 0 else "data" if typ == 1 else "type %d" % typ)
        sname = PART_TYPES.get(typ, {}).get(sub) or ("ota_%d" % (sub - 0x10) if typ == 0 and 0x10 <= sub < 0x20
                                                    else "0x%02x" % sub)
        parts.append({"label": label, "type": name, "subtype": sname, "offset": poff, "size": size})
    return parts


def esp_report(d):
    r = {"notes": []}
    parts = parse_partitions(d)
    if parts:
        r["format"] = "Espressif flash dump (partition table at 0x8000)"
        r["partitions"] = parts
        imgs = []
        for boot in (0x1000, 0x0):
            im = parse_esp_image(d, boot)
            if im:
                im["what"] = "second-stage bootloader @0x%x" % boot
                imgs.append(im)
                break
        for p in parts:
            if p["type"] == "app" and p["offset"] < len(d):
                im = parse_esp_image(d, p["offset"])
                if im:
                    im["what"] = "app partition %s @0x%x" % (p["label"], p["offset"])
                    imgs.append(im)
        r["images"] = imgs
        return r
    if d[0] == 0xEA and len(d) > 16:
        # ESP8266 v2 image: header, the IROM segment, then an ordinary 0xE9 image
        la, ln = struct.unpack_from("<II", d, 8)
        nxt = 16 + ln
        im = parse_esp_image(d, nxt) if nxt < len(d) else None
        if im:
            im["segments"].insert(0, {"addr": la, "size": ln, "offset": 16,
                                      "region": esp_region("ESP8266", la)})
            im["what"] = "ESP8266 v2 image (0xEA): IROM segment, then the 0xE9 image @0x%x" % nxt
            r["format"] = "Espressif ESP8266 v2 image"
            r["images"] = [im]
            return r
        return None
    im = parse_esp_image(d, 0)
    if not im:
        return None
    im["what"] = "image @0x0"
    r["format"] = "Espressif image (0xE9)"
    r["images"] = [im]
    if im["chip"] == "ESP8266" and len(d) > im["end"] + 0x1000:
        r["notes"].append("data after the image (0x%x..): a full ESP8266 flash dump? The NONOS SDK's "
                          "irom0 text is raw at flash 0x10000 or 0x40000, mapped at 0x40200000 + offset"
                          % im["end"])
    return r


def esp_suggest(r):
    ims = r.get("images") or []
    if not ims:
        return {}
    chip = ims[-1]["chip"]
    if chip in ESP_RISCV:
        return suggestion("rv32", "le", model=chip)
    model = {"ESP8266": "lx106", "ESP32": "lx6", "ESP32-S3": "lx7", "ESP32-S2": "lx7?"}.get(chip)
    return suggestion("xtensa", "le", model=model, chip=chip)


def write_elf(path, segs, entry, machine=94):
    n = len(segs)
    hdr = 52 + 32 * n
    out = bytearray(struct.pack("<4sBBBBB7sHHIIIIIHHHHHH", b"\x7fELF", 1, 1, 1, 0, 0, b"\0" * 7,
                                2, machine, 1, entry, 52, 0, 0x300 if machine == 94 else 0,
                                52, 32, n, 40, 0, 0))
    off = hdr
    for s in segs:
        out += struct.pack("<IIIIIIII", 1, off, s["addr"], s["addr"], len(s["data"]),
                           len(s["data"]), 7, 1)
        off += len(s["data"])
    for s in segs:
        out += s["data"]
    with open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------------------- raw
def _words(d, size, endian):
    fmt = {(2, "le"): "<H", (2, "be"): ">H", (4, "le"): "<I", (4, "be"): ">I"}[(size, endian)]
    n = len(d) // size
    return struct.unpack_from(fmt[0] + fmt[1] * n, d, 0) if n else ()


def _count(ws, mask, val):
    return sum(1 for w in ws if w & mask == val)


def fingerprints(d):
    """Per candidate: (label, total hits, hits per KiB, pattern kinds with >= 2 hits, of how many).

    Each candidate counts a few instructions every compiled function of that architecture
    has (return, prologue, epilogue). Counts of one kind alone are noise; the right
    architecture shows several kinds, far above the others."""
    w4 = {e: _words(d, 4, e) for e in ("be", "le")}
    w2 = {e: _words(d, 2, e) for e in ("be", "le")}
    cand = []

    def add(label, counts):
        cand.append((label, counts))

    for e in ("be", "le"):
        ws = w4[e]
        add("mips32/" + e, [_count(ws, 0xFFFFFFFF, 0x03E00008), _count(ws, 0xFFFF8000, 0x27BD8000),
                            _count(ws, 0xFFFF0000, 0x8FBF0000) + _count(ws, 0xFFFF0000, 0xAFBF0000)])
        add("ppc32/" + e, [_count(ws, 0xFFFFFFFF, 0x4E800020), _count(ws, 0xFFFFFFFF, 0x7C0802A6),
                           _count(ws, 0xFFFF8000, 0x94218000)])
    for e, lab in (("le", "arm(A32)/le-or-BE8"), ("be", "arm(A32)/BE32")):
        ws = w4[e]
        add(lab, [_count(ws, 0xFFFFFFFF, 0xE12FFF1E), _count(ws, 0xFFFFF000, 0xE92D4000),
                  _count(ws, 0xFFFFF000, 0xE8BD8000)])
    ws = w2["le"]
    add("thumb/le-or-BE8", [_count(ws, 0xFFFF, 0x4770), _count(ws, 0xFF00, 0xB500), _count(ws, 0xFF00, 0xBD00)])
    add("rv32/le", [_count(w4["le"], 0xFFFFFFFF, 0x00008067) + _count(ws, 0xFFFF, 0x8082),
                    _count(w4["le"], 0x800FFFFF, 0x80010113) + _count(ws, 0xFFFF, 0x1141)
                    + _count(ws, 0xFFFF, 0x1101)])
    add("xtensa/le", [d.count(b"\x0d\xf0") + d.count(b"\x1d\xf0"),
                      d.count(b"\x36\x41\x00") + d.count(b"\x36\x61\x00") + d.count(b"\x36\x81\x00")
                      + d.count(b"\x12\xc1\xf0") + d.count(b"\x12\xc1\xe0")])
    add("xtensaeb/be", [d.count(b"\xd0\x0f") + d.count(b"\xd1\x0f"),
                        d.count(b"\x6c\x10\x04") + d.count(b"\x6c\x10\x06") + d.count(b"\x6c\x10\x08")
                        + d.count(b"\x21\x1c\xf0") + d.count(b"\x21\x1c\xe0")])
    add("avr/le", [_count(ws, 0xFFFF, 0x9508) + _count(ws, 0xFFFF, 0x9518),
                   _count(ws, 0xFE0F, 0x920F), _count(ws, 0xFE0F, 0x900F)])
    # CALL/GOTO: two words, 0xEC/0xED/0xEF kk then 0xF kkk; the target must be in the image
    wb = w2["be"]
    add("avr32/be", [_count(wb, 0xFF0F, 0xD401), _count(wb, 0xFF0F, 0xD802)])   # PUSHM ..,LR / POPM ..,PC
    pairs = sum(1 for i in range(len(ws) - 1) if ws[i] >> 8 in (0xEC, 0xED, 0xEF) and ws[i + 1] >> 12 == 0xF
                and 2 * ((ws[i + 1] & 0xFFF) << 8 | (ws[i] & 0xFF)) < len(d))
    add("pic18/le", [_count(ws, 0xFFFF, 0x0012) + _count(ws, 0xFFFE, 0x0010), pairs])
    add("x86", [d.count(b"\x55\x89\xe5") + d.count(b"\x55\x48\x89\xe5"), d.count(b"\xf3\x0f\x1e\xfa"),
                d.count(b"\xc9\xc3")])
    kib = max(len(d) / 1024.0, 1 / 1024.0)
    out = [(lab, sum(c), round(sum(c) / kib, 1), sum(1 for x in c if x >= 2), len(c)) for lab, c in cand]
    return sorted(out, key=lambda t: (-t[3], -t[1]))


def mcu_shapes(d):
    """Vector-table and word-width checks for the 8-bit parts and Cortex-M: (text lines,
    {arch: base} for the strong ones)."""
    out, strong = [], {}
    n = len(d)
    if n >= 8:
        w = struct.unpack_from("<%dH" % min(64, n // 2), d, 0)
        jmp = sum(1 for i in range(0, len(w) - 1, 2) if w[i] & 0xFE0E == 0x940C)
        rjmp = sum(1 for x in w[:32] if x & 0xF000 == 0xC000)
        if jmp >= 8:
            out.append("AVR vector table: %d JMPs at 4-byte slots from 0 (devices > 8 KiB)" % jmp)
            strong["avr/le"] = 0
        elif rjmp >= 16:
            out.append("AVR vector table: %d RJMPs at 2-byte slots from 0 (devices <= 8 KiB)" % rjmp)
            strong["avr/le"] = 0
    if n >= 0x26 and d[0] == 0x02 and (d[1] << 8 | d[2]) < n:
        vec = [d[o] for o in (3, 0x0B, 0x13, 0x1B, 0x23)]
        good = sum(1 for v in vec if v in (0x02, 0x32) or v & 0x1F == 0x01)
        out.append("8051?: LJMP 0x%04x at 0 (reset)%s" % (
            d[1] << 8 | d[2], "; LJMP/AJMP/RETI at %d of the interrupt vectors 0x03, 0x0B, 0x13, 0x1B, 0x23"
            % good if good else ""))
    if n >= 4 and (d[0] == 0xF3 or d[0] == 0xC3 or d[0] == 0x31 and d[3] in (0xC3, 0xCD, 0xF3, 0xED)):
        out.append("Z80-style start at 0 (%s): DI / JP nn / LD SP,nn" % d[:4].hex(" "))
    if 0x100 <= n <= 0x10000:
        nmi, rst, irq = struct.unpack_from("<HHH", d, n - 6)
        lo = 0x10000 - n
        # a reset routine starts SEI / CLD / LDX # / LDA # / JMP / JSR / CLC / STZ / NOP
        if lo <= rst < 0xFFFA and all(lo <= v < 0xFFFA for v in (nmi, irq)) \
                and d[rst - lo] in (0x78, 0xD8, 0xA2, 0xA9, 0x4C, 0x20, 0x18, 0x9C, 0xEA):
            out.append("6502 vectors at the top: NMI 0x%04x RESET 0x%04x IRQ/BRK 0x%04x -> base 0x%04x "
                       "if the image ends at 0xFFFF (reset code starts 0x%02x)" % (nmi, rst, irq, lo, d[rst - lo]))
            strong["6502/le"] = lo
    if n >= 64:
        ws = [x for x in struct.unpack_from("<%dH" % (min(n, 0x8000) // 2), d, 0) if x not in (0xFFFF, 0)]
        if len(ws) > 32:
            f12 = sum(1 for x in ws if x <= 0x0FFF) / len(ws)
            f14 = sum(1 for x in ws if x <= 0x3FFF) / len(ws)
            if f12 > 0.97:
                out.append("%.0f%% of 16-bit words fit 12 bits: baseline PIC program memory?" % (100 * f12))
                if len(ws) >= 256:
                    strong["pic-baseline/le"] = 0
            elif f14 > 0.97:
                # MOVLB k (0x0021-0x003F) exists only on the enhanced core (NOP forms elsewhere)
                movlb = sum(1 for x in ws if 0x21 <= x <= 0x3F)
                enh = movlb >= 4
                out.append("%.0f%% of 16-bit words fit 14 bits: %s PIC program memory? (%d MOVLB)"
                           % (100 * f14, "enhanced mid-range" if enh else "mid-range", movlb))
                if len(ws) >= 256:
                    strong["pic-enhanced/le" if enh else "pic-mid/le"] = 0
    if n >= 64:
        w0, w1 = struct.unpack_from("<II", d, 0)
        if 0x20000000 <= w0 < 0x20100000 and w1 & 1 and w1 < 0x20000000:
            out.append("Cortex-M vector table (SP 0x%08x, reset 0x%08x, Thumb): ARMv7-M, ember has no ARMv7-M"
                       % (w0, w1))
        ws = struct.unpack_from("<8I", d, 0)
        if sum(1 for x in ws if x & 0xFFFFF000 == 0xE59FF000 or x >> 24 == 0xEA) >= 6:
            out.append("ARM exception vectors at 0 (LDR pc / B): A32 little-endian")
        ws = struct.unpack_from(">8I", d, 0)
        if sum(1 for x in ws if x & 0xFFFFF000 == 0xE59FF000 or x >> 24 == 0xEA) >= 6:
            out.append("ARM exception vectors at 0 in big-endian words: BE32 (ember: no) or a byte-swapped dump")
    if n >= 16 and d[-16] in (0xE9, 0xEA, 0xEB):
        out.append("x86 jump in the last 16 bytes (a reset vector at 0xFFFFFFF0?): run uefi_scan.py")
    return out, strong


LEADER_ARCH = {"mips32/be": ("mips32", "be"), "mips32/le": ("mips32", "le"), "ppc32/be": ("ppc32", "be"),
               "arm(A32)/le-or-BE8": ("arm", "le"), "thumb/le-or-BE8": ("arm", "le"),
               "rv32/le": ("rv32", "le"), "xtensa/le": ("xtensa", "le"), "xtensaeb/be": ("xtensaeb", "be"),
               "avr/le": ("avr", "le"), "pic18/le": ("pic18", "le"), "avr32/be": ("avr32", "be")}


def raw_report(d):
    fp = fingerprints(d)
    shapes, strong = mcu_shapes(d)
    r = {"format": "raw (no container header recognised)", "fingerprints": fp, "shapes": shapes,
         "notes": []}
    full = [t for t in fp if t[3] == t[4] and t[1] >= 10]
    if len(full) == 1 or len(full) > 1 and full[0][1] >= 3 * full[1][1]:
        r["leader"] = full[0][0]
    # a suggestion only when one line of evidence stands alone
    if r.get("leader") in LEADER_ARCH and not strong:
        arch, endian = LEADER_ARCH[r["leader"]]
        r["suggest"] = suggestion(arch, endian, basis="instruction fingerprints (a hint)")
    elif len(strong) == 1 and (not r.get("leader") or LEADER_ARCH.get(r["leader"], ("",))[0]
                               == next(iter(strong)).split("/")[0]):
        key = next(iter(strong))
        arch, endian = key.split("/")
        r["suggest"] = suggestion(arch, endian, base="0x%x" % strong[key],
                                  basis="vector table / word shape (a hint)")
    return r


# ---------------------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description="container and architecture triage")
    ap.add_argument("image")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--bin", help="HEX: write the program-memory part as a raw binary")
    ap.add_argument("--elf", help="Espressif image: write it as an ELF32")
    ap.add_argument("--segments", help="ELF/Espressif: write each loadable segment to DIR")
    ap.add_argument("--max-gap", type=lambda v: int(v, 0), default=0x10000,
                    help="HEX: ranges closer than this form one group (default 0x10000)")
    a = ap.parse_args(argv)
    d = open(a.image, "rb").read()
    rep = {"file": a.image, "size": len(d)}
    sugg = {}
    if d[:4] == b"\x7fELF":
        rep.update(parse_elf(d))
        sugg = rep.pop("suggest", {})
        if a.segments:
            os.makedirs(a.segments, exist_ok=True)
            for s in rep["segments"]:
                if s["filesz"]:
                    p = os.path.join(a.segments, "seg-%08x.bin" % s["vaddr"])
                    with open(p, "wb") as f:
                        f.write(d[s["offset"]:s["offset"] + s["filesz"]])
                    s["file"] = os.path.abspath(p)
    elif d[:1] == b":" and re.match(rb"^:[0-9A-Fa-f]{10,}\s*$", d.split(b"\n", 1)[0]):
        mem, start = parse_ihex(d.decode("ascii", "replace"))
        r, groups = hex_report(mem, start, a.max_gap)
        rep.update(r)
        if groups:
            lo, hi = groups[0][0][0], groups[0][-1][1]
            blob = bytes(mem.get(x, 0xFF) for x in range(lo, hi))
            rep["program"] = {"base": "0x%x" % lo, "size": hi - lo}
            raw = raw_report(blob)
            rep["fingerprints"], rep["shapes"] = raw["fingerprints"], raw["shapes"]
            if raw.get("leader"):
                rep["leader"] = raw["leader"]
            sugg = raw.get("suggest", {})
            if sugg.get("base"):
                sugg["base"] = "0x%x" % (lo + int(sugg["base"], 16))
            if a.bin:
                with open(a.bin, "wb") as f:
                    f.write(blob)
                rep["program"]["file"] = os.path.abspath(a.bin)
    else:
        r = esp_report(d) if d[:1] in (b"\xe9", b"\xea") or len(d) > 0x8020 else None
        if r and r.get("format"):
            rep.update(r)
            sugg = esp_suggest(r)
            ims = r.get("images") or []
            if ims and (a.elf or a.segments):
                im = ims[-1]
                segs = [{"addr": s["addr"], "data": d[s["offset"]:s["offset"] + s["size"]]}
                        for s in im["segments"]]
                if a.elf:
                    write_elf(a.elf, segs, im["entry"], 243 if im["chip"] in ESP_RISCV else 94)
                    rep["elf"] = os.path.abspath(a.elf)
                if a.segments:
                    os.makedirs(a.segments, exist_ok=True)
                    for s in segs:
                        with open(os.path.join(a.segments, "seg-%08x.bin" % s["addr"]), "wb") as f:
                            f.write(s["data"])
        else:
            rep.update(raw_report(d))
            sugg = rep.pop("suggest", {})
    if sugg:
        sugg.setdefault("basis", "ELF header" if d[:4] == b"\x7fELF" else "image header")
        rep["suggest"] = sugg

    if a.json:
        print(json.dumps(rep, indent=2))
        return 0
    print("file: %s (%d bytes)" % (a.image, len(d)))
    print("format: %s" % rep.get("format"))
    if "e_machine" in rep:
        print("machine: %s (e_machine %d), flags %s, entry %s" % (rep["machine"], rep["e_machine"],
                                                                  rep["e_flags"], rep["entry"]))
        for s in rep["segments"]:
            print("  LOAD vaddr 0x%08x paddr 0x%08x filesz 0x%-7x memsz 0x%-7x %s%s" % (
                s["vaddr"], s["paddr"], s["filesz"], s["memsz"], s["flags"],
                ("  -> " + s["file"]) if s.get("file") else ""))
        if rep["sections"]:
            print("  sections: %s" % " ".join(rep["sections"][:24]))
    if "ranges" in rep:
        print("start record: %s" % rep["start"])
        for lo, hi in rep["ranges"][:20]:
            print("  data 0x%s-0x%s" % (lo[2:], hi[2:]))
        if len(rep["ranges"]) > 20:
            print("  ... %d ranges" % len(rep["ranges"]))
        if "program" in rep:
            p = rep["program"]
            print("program memory: base %s, %d bytes%s" % (p["base"], p["size"],
                  ("  -> " + p["file"]) if p.get("file") else "  (--bin OUT writes it)"))
    for p in rep.get("partitions", []):
        print("  partition %-12s %-4s %-9s offset 0x%06x size 0x%06x" % (
            p["label"], p["type"], p["subtype"], p["offset"], p["size"]))
    for im in rep.get("images", []):
        print("  %s: chip %s, entry 0x%08x, flash %s" % (im["what"], im["chip"], im["entry"], im["flash_size"]))
        for s in im["segments"]:
            print("    segment @file 0x%06x load 0x%08x size 0x%-6x %s" % (s["offset"], s["addr"], s["size"],
                                                                         s["region"]))
    if rep.get("elf"):
        print("  -> ELF %s (entry + one PT_LOAD per segment)" % rep["elf"])
    if "fingerprints" in rep:
        print("instruction fingerprints (hits, per KiB, pattern kinds seen; a hint - the right arch "
              "shows every kind, far ahead):")
        print("  " + ("  ".join("%s %d (%.1f/KiB, %d/%d)" % t for t in rep["fingerprints"] if t[1]) or "none"))
        if rep.get("leader"):
            print("  leader: %s" % rep["leader"])
    for s in rep.get("shapes", []):
        print("shape: %s" % s)
    for n in rep.get("notes", []):
        print("note: %s" % n)
    if sugg:
        print("suggest: arch=%s endian=%s ember=%s board=%s ghidra=%s%s" % (
            sugg.get("arch"), sugg.get("endian"), sugg.get("isa"), sugg.get("board"), sugg.get("ghidra"),
            "".join(" %s=%s" % (k, sugg[k]) for k in ("model", "chip", "base") if k in sugg)
            + ("  (%s)" % sugg["basis"] if sugg.get("basis") else "")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
