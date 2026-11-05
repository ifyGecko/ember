#!/usr/bin/env python3
"""Check ember's Xtensa decoder (tests/isa-xtensa/xt-dec) against the GNU disassemblers.

    python3 -I xt-check.py XT_DEC WORKDIR MODEL=OBJDUMP...

MODEL is lx106, lx6 or lx7 and OBJDUMP the disassembler of a toolchain built for that
core (xtensa-lx106-elf-objdump, xtensa-esp32-elf-objdump, xtensa-esp32s3-elf-objdump).
For every 16-bit word, a sample of the 24-bit words (fixed seed) and every opcode of the
table with random operands, the decoder and objdump must agree on whether the word is an
instruction of the model, its length and its mnemonic. Words of designer-defined TIE
extensions the toolchains know and ember does not model (the ESP32's double-precision
helpers, the ESP32-S3's AI and GPIO instructions), the OCD-mode and simulator
instructions, and the special and user register accesses (whose register numbers the
lifter checks) are left out. The big-endian decoder must decode each word, laid out the
big-endian way (opcodes.py's le_to_be), as the little-endian decoder does.
"""
import os
import random
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import opcodes  # noqa: E402

SLOT = 4
SKIP_PREFIX = ("ee.", "f64", "rf64r", "wf64r", "rur.", "wur.", "rsr.", "wsr.", "xsr.",
               "rur", "wur", "rsr", "wsr", "xsr", "setb_expstate", "clrb_expstate",
               "wrmsk_expstate", "read_impwire", "set_bit_gpio_out", "clr_bit_gpio_out",
               "wr_mask_gpio_out", "get_gpio_in", "simcall", "rfdo", "rfdd", "lddr32.p",
               "sddr32.p", "ld.qr", "st.qr", "mv.qr")
SR_ACCESS = ("rsr", "wsr", "xsr", "rur", "wur")


def make_elf(text, nslots):
    """An ELF32 (EM_XTENSA) object whose .text is `text`, with a symbol at every slot so
    that objdump starts decoding afresh there."""
    shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    strtab = b"\0x\0"
    syms = [struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)]
    syms += [struct.pack("<IIIBBH", 1, i * SLOT, 0, 2, 0, 1) for i in range(nslots)]
    symtab = b"".join(syms)
    off_sym = (52 + len(text) + 3) & ~3
    off_str = off_sym + len(symtab)
    off_shs = off_str + len(strtab)
    off_sh = (off_shs + len(shstr) + 3) & ~3
    body = bytearray(off_sh)
    body[52:52 + len(text)] = text
    body[off_sym:off_sym + len(symtab)] = symtab
    body[off_str:off_str + len(strtab)] = strtab
    body[off_shs:off_shs + len(shstr)] = shstr

    def sh(name, typ, flags, off, size, link=0, info=0, align=1, entsize=0):
        return struct.pack("<IIIIIIIIII", name, typ, flags, 0, off, size, link, info, align,
                           entsize)
    shdrs = [sh(0, 0, 0, 0, 0), sh(1, 1, 6, 52, len(text), align=4),
             sh(7, 2, 0, off_sym, len(symtab), link=3, info=len(syms), align=4, entsize=16),
             sh(15, 3, 0, off_str, len(strtab)), sh(23, 3, 0, off_shs, len(shstr))]
    eh = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8 + struct.pack(
        "<HHIIIIIHHHHHH", 1, 94, 1, 0, 0, off_sh, 0, 52, 0, 0, 40, len(shdrs), 4)
    return bytes(eh) + bytes(body[52:]) + b"".join(shdrs)


LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]+)\s*\t(\S+)")


def objdump(tool, words, work):
    """(length, mnemonic or None) objdump decodes for each word (bytes, LE)."""
    text = b"".join(w.ljust(SLOT, b"\0") for w in words)
    path = os.path.join(work, "xt-check-%d.o" % os.getpid())
    with open(path, "wb") as f:
        f.write(make_elf(text, len(words)))
    out = subprocess.run([tool, "-dz", path], capture_output=True, text=True, check=True).stdout
    os.unlink(path)
    res = [None] * len(words)
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if addr % SLOT or res[addr // SLOT] is not None:
            continue
        mn = m.group(3)
        res[addr // SLOT] = (len(m.group(2)) // 2, None if mn.startswith(".") else mn)
    return res


def decode(xtdec, model, words, be=False):
    args = [xtdec, model] + (["be"] if be else [])
    out = subprocess.run(args, input="".join(w.hex() + "\n" for w in words),
                         capture_output=True, text=True, check=True).stdout.split("\n")
    res = []
    for line in out[:len(words)]:
        n, mn = line.split()
        res.append((int(n), None if mn == "-" else mn))
    return res


# The table's own encodings, for objdump's catch-all (lx106 and esp32 print the first
# opcode of their table, `excw` / `lsi`, for a word they cannot decode).
CATCH = {e["mn"]: opcodes.match(e) for e in opcodes.OPCODES if e["mn"] in ("excw", "lsi")}


def words_for(model):
    rnd = random.Random(1234)
    ws = []
    for w in range(1 << 16):           # every 16-bit word
        if 8 <= (w & 15) <= 13:
            ws.append(struct.pack("<H", w))
    for _ in range(200000):            # a sample of the 24-bit ones
        w = rnd.getrandbits(24) & ~15 | rnd.randrange(8)
        ws.append(struct.pack("<I", w)[:3])
    for e in opcodes.OPCODES:          # every opcode, operand bits random
        mask, val = opcodes.match(e)
        for _ in range(16):
            w = (rnd.getrandbits(8 * e["len"]) & ~mask) | val
            ws.append(struct.pack("<I", w)[:e["len"]])
    return ws


def main():
    xtdec, work = sys.argv[1], sys.argv[2]
    bad = 0
    for arg in sys.argv[3:]:
        model, tool = arg.split("=", 1)
        ws = words_for(model)
        ours = decode(xtdec, model, ws)
        theirs = objdump(tool, ws, work)
        checked = mism = 0
        for w, o, t in zip(ws, ours, theirs):
            if t is None:
                continue
            tl, tm = t
            if (tm and tm.startswith(SKIP_PREFIX)) or o[1] in SR_ACCESS:
                continue
            iw = int.from_bytes(w, "little")
            if tm in CATCH:
                mask, val = CATCH[tm]
                if (iw & mask) != val:
                    tm = None          # objdump's catch-all: not an instruction
            if o[1] is None and tm is None:
                checked += 1
                continue
            # an undecodable word: objdump steps one byte, the decoder its length
            if (o[1], o[0]) != (tm, tl):
                mism += 1
                if mism <= int(os.environ.get('XT_CHECK_SHOW', '20')):
                    print("%s: %s: ember %s (%d bytes), objdump %s (%d bytes)"
                          % (model, w.hex(), o[1] or "illegal", o[0], tm or "invalid", tl))
            checked += 1
        # the big-endian layout of the same words decodes alike
        bews, idx = [], []
        for i, (w, o) in enumerate(zip(ws, ours)):
            if o[1] is not None:
                iw = int.from_bytes(w, "little")
                e = (opcodes.lookup(iw, len(w))
                     or [x for x in opcodes.OPCODES if x["mn"] == o[1]])[0]
                bews.append(opcodes.be_bytes(e, iw))
                idx.append(i)
        be = decode(xtdec, model, bews, be=True)
        bemism = 0
        for i, b in zip(idx, be):
            if b != ours[i]:
                bemism += 1
                if bemism <= 20:
                    print("%s: %s big-endian %s: %s, little-endian %s"
                          % (model, ws[i].hex(), bews[i].hex(), b, ours[i]))
        print("%s: %d words checked against %s: %d differ; %d big-endian words: %d differ"
              % (model, checked, os.path.basename(tool), mism, len(bews), bemism))
        bad += mism + bemism
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
