#!/usr/bin/env python3
"""The DSP ASE's encodings against GNU binutils: for every SPECIAL3 function of the ASE
and every sub-opcode (bits 10:6), words with random register fields go through
mips_dsp_rev (dsp-dec.c) and through objdump; an encoding objdump disassembles is
revision 1 if a revision-1 assembler (as -mdsp) accepts objdump's text of it, else
revision 2; one it does not is reserved (fields that must be zero included).

usage: dsp-check.py DEC WORKDIR [samples_per_subop]
"""
import os, random, re, subprocess, sys

DEC, here = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 24
CROSS = os.environ.get("CROSS", "mipsel-linux-gnu-")
random.seed(3)
FUNCTS = (0x0A, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x18, 0x30, 0x31, 0x38)

words = []
for fn in FUNCTS:
    for op in range(32):
        for k in range(N):
            f = random.getrandbits(15) if k else 0   # one sample with all fields zero
            words.append((0x1F << 26) | (f << 11) | (op << 6) | fn)

def run(cmd, inp=None):
    return subprocess.run(cmd, input=inp, capture_output=True, text=True, check=True).stdout

def disasm():
    src = os.path.join(here, "dsp.s")
    open(src, "w").write("\t.text\n" + "".join("\t.word 0x%08x\n" % w for w in words))
    run([CROSS + "as", "-march=mips32r2", "-mdspr2", "-EL", src, "-o", src + ".o"])
    out = []
    for l in run([CROSS + "objdump", "-d", "-M", "gpr-names=numeric", src + ".o"]).splitlines():
        m = re.match(r"^\s*[0-9a-f]+:\t[0-9a-f]+\s*\t(\S+)\s*(.*)$", l)
        if m:
            out.append((m.group(1), m.group(2).split("<")[0].strip()))
    return out

dis = disasm()
mine = [int(l.split()[1]) for l in run([DEC], "\n".join("%x" % w for w in words) + "\n").splitlines()]
assert len(dis) == len(mine) == len(words)

rev1 = {}
def is_rev1(mn, ops):
    if mn not in rev1:
        src = os.path.join(here, "dsp-r1.s")
        open(src, "w").write("\t.text\n\t%s %s\n" % (mn, ops))
        rev1[mn] = subprocess.run([CROSS + "as", "-march=mips32r2", "-mdsp", "-EL", src, "-o", src + ".o"],
                                  capture_output=True).returncode == 0
    return rev1[mn]

def ok(mn):
    return not mn.startswith((".word", "0x"))

bad = {}
for w, (mn, ops), m in zip(words, dis, mine):
    want = 0 if not ok(mn) else 1 if is_rev1(mn, ops) else 2
    if want != m:
        key = "fn%02x op%02d: %s (r%d), mine r%d" % (w & 0x3F, (w >> 6) & 31, mn, want, m)
        bad.setdefault(key, []).append(w)

print("dsp-check: %d encodings, %d disagree" % (len(words), sum(len(v) for v in bad.values())))
for k, v in sorted(bad.items())[:40]:
    print("  %-60s x%d e.g. %08x" % (k, len(v), v[0]))
sys.exit(1 if bad else 0)
