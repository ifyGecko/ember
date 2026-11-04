#!/usr/bin/env python3
"""The MIPS16e decoder against a disassembler: every non-extended halfword, a
sample of EXTEND forms and of JAL/JALX go through mips16_decode (the DEC program,
mips16-dec.c) and through GNU objdump. An instruction recoded as MIPS32 must
disassemble as what objdump calls the MIPS16e one (after objdump's two-operand
aliases are spelled out); branches, jumps, PC-relative loads and SAVE/RESTORE must
agree on their targets, registers and lists; reserved encodings must be ones objdump
rejects (or that only 64-bit or other-ASE processors have).

usage: mips16-check.py DEC WORKDIR [n_extended_samples]
"""
import random, re, subprocess, sys, os

DEC = sys.argv[1]
here = sys.argv[2]
NEXT = int(sys.argv[3]) if len(sys.argv) > 3 else 40000
CROSS = os.environ.get("CROSS", "mipsel-linux-gnu-")
random.seed(1)

def is_two(i):
    op = i >> 11
    return op == 0x1E or op == 0x03

# ---- cases: (ext, has_ext, i, i2) -------------------------------------------
cases = []
for i in range(0x10000):
    if is_two(i):
        continue
    cases.append((0, 0, i, 0))
for _ in range(NEXT):
    e = 0xF000 | random.getrandbits(11)
    while True:
        i = random.getrandbits(16)
        if not is_two(i):
            break
    cases.append((e, 1, i, 0))
for _ in range(4000):
    i = 0x1800 | random.getrandbits(11)
    cases.append((0, 0, i, random.getrandbits(16)))

addr = 0
lines, asm = [], ["\t.set mips16\n\t.text\n\t.globl f\n\t.ent f\nf:\n\t.insn\n"]
for (e, h, i, i2) in cases:
    lines.append("%x %x %x %x %x" % (addr, e, h, i, i2))
    if h:
        asm.append("\t.hword 0x%04x, 0x%04x\n" % (e, i)); n = 4
    elif is_two(i):
        asm.append("\t.hword 0x%04x, 0x%04x\n" % (i, i2)); n = 4
    else:
        asm.append("\t.hword 0x%04x\n" % i); n = 2
    addr += n
asm.append("\t.end f\n")

def run(cmd, inp=None):
    return subprocess.run(cmd, input=inp, capture_output=True, text=True, check=True).stdout

dec = {}
for l in run([DEC], "\n".join(lines) + "\n").splitlines():
    f = l.split()
    a = int(f[0], 16)
    dec[a] = dict(kind=int(f[1]), len=int(f[2]), w=int(f[3], 16), rx=int(f[4]), rd=int(f[5]),
                  imm=int(f[6], 16), target=int(f[7], 16), link=int(f[8]), compact=int(f[9]),
                  args=int(f[10]), astatic=int(f[11]), xsregs=int(f[12]), fs=int(f[13]),
                  ra=int(f[14]), s0=int(f[15]), s1=int(f[16]))

open(os.path.join(here, "m16.s"), "w").write("".join(asm))
run([CROSS + "as", "-march=m4k", "-EL", os.path.join(here, "m16.s"), "-o", os.path.join(here, "m16.o")])
od16 = run([CROSS + "objdump", "-d", os.path.join(here, "m16.o")])

LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(\S+)\s*(.*)$")
def parse(txt):
    out = {}
    for l in txt.splitlines():
        m = LINE.match(l)
        if m:
            ops = m.group(4).split("<")[0].strip()
            out[int(m.group(1), 16)] = (m.group(3), [o for o in ops.split(",") if o] if ops else [])
    return out
m16 = parse(od16)

# MIPS32 equivalents
KINDS = ["RI", "W32", "B", "BEQZ", "BNEZ", "JR", "JAL", "ADDIUPC", "LWPC", "SAVE", "RESTORE"]
w32 = [(a, d["w"]) for a, d in sorted(dec.items()) if KINDS[d["kind"]] == "W32"]
s32 = ["\t.set nomips16\n\t.text\n"] + ["\t.word 0x%08x\n" % w for _, w in w32]
open(os.path.join(here, "m32.s"), "w").write("".join(s32))
run([CROSS + "as", "-march=mips32r2", "-EL", os.path.join(here, "m32.s"), "-o", os.path.join(here, "m32.o")])
od32 = parse(run([CROSS + "objdump", "-d", os.path.join(here, "m32.o")]))
eq32 = {a: od32.get(4 * k) for k, (a, _) in enumerate(w32)}

def norm16(mn, ops):
    """MIPS16 objdump -> the MIPS32 objdump text of the equivalent."""
    if mn in ("and", "or", "xor") and len(ops) == 2:
        return mn, [ops[0], ops[0], ops[1]]
    if mn in ("sllv", "srlv", "srav") and len(ops) == 2:
        return mn, [ops[0], ops[0], ops[1]]
    if mn in ("addiu", "addu", "subu", "sll", "srl", "sra") and len(ops) == 2:
        return mn, [ops[0], ops[0], ops[1]]
    if mn in ("neg", "not") and len(ops) == 1:
        ops = [ops[0], ops[0]]
    if mn == "cmp":   return "xor", ["t8"] + ops
    if mn == "cmpi":  return "xori", ["t8"] + ops
    if mn in ("slt", "sltu", "slti", "sltiu") and len(ops) == 2:
        return mn, ["t8"] + ops
    if mn == "neg":   return "negu", ops
    if mn == "zeb":   return "andi", [ops[0], ops[0], "0xff"]
    if mn == "zeh":   return "andi", [ops[0], ops[0], "0xffff"]
    if mn in ("seb", "seh"): return mn, [ops[0], ops[0]]
    if mn == "nop":   return "move", ["zero", "s0"]
    if mn in ("break", "sdbbp"): return mn, []
    return mn, ops

def norm32(mn, ops):
    if mn in ("break", "sdbbp"): return mn, []
    if mn == "nor" and ops[2:] == ["zero"]:
        return "not", ops[:2]
    return mn, ops

def num(s):
    return int(s, 16) if s.startswith(("0x", "-0x")) else int(s)

def same_imm(a, b):
    try:
        return (num(a) & 0xFFFFFFFF) == (num(b) & 0xFFFFFFFF)
    except ValueError:
        return a == b

def equal(x, y):
    if x[0] != y[0] or len(x[1]) != len(y[1]):
        return False
    for p, q in zip(x[1], y[1]):
        if p == q:
            continue
        mp, mq = re.match(r"^(-?\w+)\((\w+)\)$", p), re.match(r"^(-?\w+)\((\w+)\)$", q)
        if mp and mq and mp.group(2) == mq.group(2) and same_imm(mp.group(1), mq.group(1)):
            continue
        if not same_imm(p, q):
            return False
    return True

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
       "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"]

def save_text(d):
    parts = []
    if d["args"]:
        parts.append("a0" if d["args"] == 1 else "a0-" + GPR[4 + d["args"] - 1])
    parts.append(str(d["fs"]))
    if d["ra"]:
        parts.append("ra")
    regs = []
    if d["s0"]: regs.append(16)
    if d["s1"]: regs.append(17)
    regs += [17 + k if k < 7 else 30 for k in range(1, d["xsregs"] + 1)]
    # binutils prints runs as a-b
    names = [GPR[r] for r in sorted(regs, key=lambda r: (r == 30, r))]
    runs, k = [], 0
    while k < len(names):
        j = k
        order = sorted(regs, key=lambda r: (r == 30, r))
        while j + 1 < len(order) and (order[j + 1] == order[j] + 1 or (order[j] == 23 and order[j + 1] == 30)):
            j += 1
        runs.append(names[k] if j == k else names[k] + "-" + names[j])
        k = j + 1
    parts += runs
    if d["astatic"] == 1:
        parts.append("a3")
    elif d["astatic"]:
        parts.append(GPR[7 - d["astatic"] + 1] + "-a3")
    return parts

INVALID = (".short", ".word", "0x", "extend", "unknown")
bad = {}
def fail(cat, a, msg):
    bad.setdefault(cat, []).append("%x: %s" % (a, msg))

checked = 0
for a, d in sorted(dec.items()):
    o = m16.get(a)
    if o is None:
        fail("noparse", a, "objdump gave nothing"); continue
    kind = KINDS[d["kind"]]
    mn, ops = o
    checked += 1
    if kind == "RI":
        if not mn.startswith(INVALID) and mn not in ("save", "restore", "asmacro", "entry", "exit", "daddu", "dsubu", "dsll", "daddiu",
                "ld", "sd", "lwu", "dsrl", "dsra", "dsllv", "dsrlv", "dsrav", "dmult", "dmultu", "ddiv",
                "ddivu", "dmfc0", "dmtc0", "zew", "sew", "dmove", "dli"):
            fail("ri-but-valid", a, "%s %s" % (mn, ",".join(ops)))
        continue
    if mn.startswith(INVALID):
        fail("valid-but-objdump-invalid", a, "%s kind=%s" % (mn, kind)); continue
    if kind == "W32":
        e = eq32.get(a)
        if e is None:
            fail("w32-noparse", a, "%08x" % d["w"]); continue
        if not equal(norm16(mn, ops), norm32(*e)):
            fail("w32-mismatch:" + mn, a, "%s %s  vs  %s %s" % (mn, ",".join(ops), e[0], ",".join(e[1])))
        continue
    if kind in ("B", "BEQZ", "BNEZ", "JAL", "ADDIUPC", "LWPC"):
        want = d["target"] & ~1 if kind not in ("ADDIUPC", "LWPC") else (a & ~3) + d["imm"]
        try:
            got = int(ops[-1], 16) if ops else None
        except ValueError:
            got = None
        exp_mn = {"B": ("b",), "BEQZ": ("beqz", "bteqz"), "BNEZ": ("bnez", "btnez"),
                  "JAL": ("jal", "jalx"), "ADDIUPC": ("addiu", "la"), "LWPC": ("lw",)}[kind]
        if mn not in exp_mn or got is None or (got & 0xFFFFFFFF) != (want & 0xFFFFFFFF):
            fail(kind.lower(), a, "%s %s  want target %x" % (mn, ",".join(ops), want))
        elif kind == "JAL" and (mn == "jalx") != ((d["target"] & 1) == 0):
            fail("jalx", a, "%s mode bit %d" % (mn, d["target"] & 1))
        elif kind in ("BEQZ", "BNEZ") and mn in ("beqz", "bnez") and ops[0] != GPR[d["rx"]]:
            fail("breg", a, "%s %s rx=%s" % (mn, ",".join(ops), GPR[d["rx"]]))
        elif kind in ("ADDIUPC", "LWPC") and ops[0] != GPR[d["rd"]]:
            fail("pcreg", a, "%s %s rd=%s" % (mn, ",".join(ops), GPR[d["rd"]]))
        continue
    if kind == "JR":
        want_mn = ("jalr" if d["link"] else "jr") + ("c" if d["compact"] else "")
        if mn != want_mn or ops[-1] != GPR[d["rx"]]:
            fail("jr", a, "%s %s  want %s %s" % (mn, ",".join(ops), want_mn, GPR[d["rx"]]))
        continue
    if kind in ("SAVE", "RESTORE"):
        if mn != kind.lower() or ops != save_text(d):
            fail("svrs", a, "%s %s  vs  %s" % (mn, ",".join(ops), ",".join(save_text(d))))
        continue

print("mips16-check: %d encodings, %d disagree" % (checked, sum(len(l) for l in bad.values())))
for cat, l in sorted(bad.items()):
    print("%-28s %6d  e.g. %s" % (cat, len(l), "; ".join(l[:3])))
sys.exit(1 if bad else 0)
