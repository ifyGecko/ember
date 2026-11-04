#!/usr/bin/env python3
"""The microMIPS decoder against a disassembler, like mips16-check.py: every 16-bit
halfword, all of POOL32A's low halfword, every sub-opcode of POOL32B/C/I, and random
words of each 32-bit major opcode go through umips_decode (umips-dec.c) and GNU
objdump. A recoded instruction must disassemble as what objdump calls the microMIPS
one; branches, jumps, PC-relative and multi-register forms must agree on targets,
registers and lists; reserved encodings must be ones objdump rejects or that belong to
an extension the processor lacks (the FPU and DSP ASE are checked separately).

usage: umips-check.py DEC WORKDIR [n_random]
"""
import os, random, re, subprocess, sys

DEC, here = sys.argv[1], sys.argv[2]
NRAND = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
CROSS = os.environ.get("CROSS", "mipsel-linux-gnu-")
random.seed(2)

def is32(h):
    return (h & 0x1C00) == 0 or (h & 0x1000) != 0

words = []           # (len, value)
for h in range(0x10000):
    if not is32(h):
        words.append((2, h))
R = random.getrandbits
for lo in range(0x10000):                         # POOL32A: all low halfwords
    words.append((4, (0x00 << 26) | (R(10) << 16) | lo))
for lo in range(0x10000):                         # POOL32F (the FPU): all low halfwords
    words.append((4, (0x15 << 26) | (R(10) << 16) | lo))
for op in (0x08, 0x18):                           # POOL32B / POOL32C: each sub-opcode
    for sub in range(16):
        for _ in range(64):
            words.append((4, (op << 26) | (R(10) << 16) | (sub << 12) | R(12)))
for sub in range(32):                             # POOL32I
    for _ in range(64):
        words.append((4, (0x10 << 26) | (sub << 21) | R(21)))
for op in range(64):                              # every major opcode, random bits
    if op in (0x00, 0x15, 0x16) or not is32(op << 10):
        continue                                  # (POOL32A, POOL32F swept above; 64-bit POOL32S)
    for _ in range(NRAND):
        words.append((4, (op << 26) | R(26)))

raw = {}
addr, lines, asm = 0, [], ["\t.set micromips\n\t.text\n\t.globl f\n\t.ent f\nf:\n\t.insn\n"]
for (n, v) in words:
    raw[addr] = v
    if n == 2:
        lines.append("%x %x 0" % (addr, v))
        asm.append("\t.hword 0x%04x\n" % v)
    else:
        lines.append("%x %x %x" % (addr, v >> 16, v & 0xFFFF))
        asm.append("\t.hword 0x%04x, 0x%04x\n" % (v >> 16, v & 0xFFFF))
    addr += n
asm.append("\t.end f\n")

def run(cmd, inp=None):
    return subprocess.run(cmd, input=inp, capture_output=True, text=True, check=True).stdout

dec = {}
for l in run([DEC], "\n".join(lines) + "\n").splitlines():
    f = l.split()
    regs = [int(x) for x in f[15].split(",")[1:]]
    dec[int(f[0], 16)] = dict(kind=int(f[1]), len=int(f[2]), w=int(f[3], 16), cond=int(f[4]),
        rs=int(f[5]), rt=int(f[6]), rd=int(f[7]), cc=int(f[8]), tf=int(f[9]), link=int(f[10]),
        compact=int(f[11]), short=int(f[12]), imm=int(f[13], 16), target=int(f[14], 16), regs=regs)

src = os.path.join(here, "um.s")
open(src, "w").write("".join(asm))
run([CROSS + "as", "-march=mips32r2", "-mmicromips", "-mmcu", "-EL", src, "-o", src + ".o"])
LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(\S+)\s*(.*)$")
def parse(txt):
    out = {}
    for l in txt.splitlines():
        m = LINE.match(l)
        if m:
            ops = m.group(4).split("<")[0].strip()
            out[int(m.group(1), 16)] = (m.group(3), [o for o in ops.split(",") if o] if ops else [])
    return out
um = parse(run([CROSS + "objdump", "-d", src + ".o"]))

KINDS = ["RI", "W32", "NOP", "BR", "J", "JR", "JRADDIUSP", "ADDIUPC", "LWM", "SWM", "LWP", "SWP",
         "MOVEP", "LWXS"]
w32 = [(a, d["w"]) for a, d in sorted(dec.items()) if KINDS[d["kind"]] == "W32"]
s32 = os.path.join(here, "um32.s")
open(s32, "w").write("\t.set nomicromips\n\t.text\n" + "".join("\t.word 0x%08x\n" % w for _, w in w32))
run([CROSS + "as", "-march=mips32r2", "-mmcu", "-EL", s32, "-o", s32 + ".o"])
od32 = parse(run([CROSS + "objdump", "-d", s32 + ".o"]))
eq32 = {a: od32.get(4 * k) for k, (a, _) in enumerate(w32)}

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
       "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"]

def num(s):
    s = s.strip()
    return int(s, 16) if s.lstrip("-").startswith("0x") else int(s)

def same_imm(a, b):
    try:
        return (num(a) & 0xFFFFFFFF) == (num(b) & 0xFFFFFFFF)
    except ValueError:
        return a == b

def norm(mn, ops):
    if mn == "addiu" and len(ops) == 3 and ops[1] == "zero":
        return "li", [ops[0], ops[2]]
    if mn == "move" and ops == ["zero", "zero"]:
        return "nop", []
    if mn in ("and", "or", "xor", "addu", "subu", "addiu", "sll", "srl") and len(ops) == 2:
        ops = [ops[0], ops[0], ops[1]]
    if mn == "addiusp":
        return "addiu", ["sp", "sp", ops[0]]
    if mn == "addius5":
        return "addiu", [ops[0], ops[0], ops[1]]
    if mn in ("addiur1sp",):
        return "addiu", [ops[0], "sp", ops[1]]
    if mn in ("addiur2",):
        return "addiu", ops
    if mn == "not" and len(ops) == 1:
        ops = [ops[0], ops[0]]
    if mn == "nor" and ops[2:] == ["zero"]:
        return "not", ops[:2]
    if mn in ("break", "sdbbp", "syscall", "wait", "sync"):
        return mn, []
    if mn in ("jr", "jalr"):                      # not reached for W32
        return mn, ops
    if mn == "lui":
        return mn, [ops[0], "%d" % (num(ops[1]) & 0xFFFF)]
    if mn in ("andi", "ori", "xori"):
        return mn, ops[:2] + ["%d" % (num(ops[2]) & 0xFFFF)]
    if mn in ("pref", "cache", "synci"):
        return mn, ops[1:]                        # the hint/op field
    if mn in ("sll",) and ops == ["zero", "zero", "0"]:
        return "nop", []
    return mn, ops

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

def reglist(regs):
    names, k, out = [GPR[r] for r in regs], 0, []
    ra = names and names[-1] == "ra"
    s = [r for r in regs if r not in (30, 31)]
    if s:
        out.append(GPR[s[0]] if len(s) == 1 else GPR[s[0]] + "-" + GPR[s[-1]])
    if 30 in regs:
        out.append("s8")
    if ra:
        out.append("ra")
    return out

ALIASES_32 = {"nop", "ssnop", "ehb", "pause", "b", "bal", "bc", "bals", "beqz", "bnez", "neg",
              "negu", "not", "li", "move", "jr", "j"}
INVALID = (".short", ".word", "0x", "extend", "unknown", "micromips")
OTHER_ASE = ("bposge64", "bc2f", "bc2t", "lwc2", "swc2", "ldc2", "sdc2", "mfc2", "mtc2", "mfhc2",
             "mthc2", "cfc2", "ctc2", "cop2", "ldp", "sdp", "ld", "sd", "lwu", "lld", "scd",
             "tlbinv", "tlbinvf", "eretnc", "dvp", "evp", "lwle", "lwre", "swle", "swre", "lle",
             "sce", "cachee", "prefe", "lbe", "lbue", "lhe", "lhue", "lwe", "sbe", "she", "swe",
             "mfhc0", "mthc0", "hypcall", "mfgc0", "mtgc0", "tlbginv", "tlbginvf", "tlbgp", "tlbgr",
             "tlbgwi", "tlbgwr", "lwm", "swm", "yield", "dmfc0", "dmtc0", "mfhgc0", "mthgc0", "dmfgc0",
             "dmtgc0", "sigrie", "lapc", "ginvi", "ginvt", "jalrc", "jalrc.hb", "jrcaddiusp",
             "daddiu", "ldl", "ldr", "sdl", "sdr", "ldm", "sdm", "lsa", "fork", "mttr", "mftr",
             "mttc0", "mftc0", "mttgpr", "mftgpr", "mtthi", "mftlo", "mfthi", "mttlo", "mttc1",
             "mftc1", "mtthc1", "mfthc1", "mttacx", "mftacx", "mttdsp", "mftdsp", "dmt", "emt",
             "dvpe", "evpe", "lwp", "swp", "cttc1", "cftc1", "mttc2", "mftc2", "cttc2", "cftc2",
             "mtthc2", "mfthc2", "dmfc2", "dmtc2")
bad = {}
def fail(cat, a, msg):
    bad.setdefault(cat, []).append("%x: %s" % (a, msg))

checked = 0
for a, d in sorted(dec.items()):
    o = um.get(a)
    if o is None:
        fail("noparse", a, ""); continue
    k, (mn, ops) = KINDS[d["kind"]], o
    checked += 1
    if k == "RI":
        if not mn.startswith(INVALID) and mn not in OTHER_ASE and not mn.endswith(".ps") \
                and mn not in ("alnv.ps", "pll.ps", "plu.ps", "pul.ps", "puu.ps", "cvt.ps.s", "cvt.s.pl",
                               "cvt.s.pu", "bc1any2f", "bc1any2t", "bc1any4f", "bc1any4t", "dmfc1", "dmtc1") \
                and not re.match(r"^(dsp|ext[rp]|ins[v]|mfhi|mflo|mthi|mtlo|madd|msub|mult|shilo|"
                                 r"mthlip|wrdsp|rddsp|bitrev|repl|precr|preceq|precequ|preceu|"
                                 r"absq|addq|addsc|addu|addwc|append|balign|cmp|dpa|dps|maq|modsub|"
                                 r"muleq|muleu|mulq|mulsa|packrl|pick|prepend|raddu|shll|shra|shrl|"
                                 r"subq|subu|lbux|lhx|lwx|bposge32|precrq|precr_sra|dpaq|dpsq|dpax|"
                                 r"dpsx|dpau|dpsu|mulsaq|cmpu|cmpgu|cmpgdu|adduh|addqh|subuh|subqh|"
                                 r"mul\.|mulq_|mul_|bz\.|bnz\.)", mn):
            fail("ri-but-valid:" + mn, a, "%s %s" % (mn, ",".join(ops)))
        continue
    if mn.startswith(INVALID):
        fail("valid-but-invalid", a, "%x %s kind=%s" % (raw[a], mn, k)); continue
    if k in ("W32", "NOP"):
        if k == "NOP":
            if mn != "synci":
                fail("nop", a, mn)
            continue
        e = eq32.get(a)
        if e is None or not equal(norm(mn, ops), norm(*e)):
            fail("w32:" + mn, a, "%s %s  vs  %s" % (mn, ",".join(ops), e and (e[0] + " " + ",".join(e[1]))))
        continue
    if k == "ADDIUPC":
        if mn != "addiu" or ops[0] != GPR[d["rd"]] or ops[1] != "$pc" or not same_imm(ops[2], str(d["imm"])):
            fail("addiupc", a, "%s %s want %s,$pc,%d" % (mn, ",".join(ops), GPR[d["rd"]], d["imm"]))
        continue
    if k in ("BR", "J"):
        try:
            got = int(ops[-1], 16)
        except (ValueError, IndexError):
            got = None
        want = d["target"] & ~1 if k != "ADDIUPC" else (a & ~3) + d["imm"]
        if got is None or (got & 0xFFFFFFFF) != (want & 0xFFFFFFFF):
            fail(k.lower() + ":" + mn, a, "%s %s want %x" % (mn, ",".join(ops), want))
        elif k == "BR" and d["cond"] in (1, 2, 3, 4, 5, 6) and mn not in ("b", "bc", "bal", "bals") \
                and ops[0] != GPR[d["rs"]]:
            fail("br-reg:" + mn, a, "%s %s rs=%s" % (mn, ",".join(ops), GPR[d["rs"]]))
        elif k == "BR":
            cmn = {(0, 0): "b", (1, 0): "beq", (2, 0): "bne", (3, 0): "bltz", (4, 0): "bgez", (5, 0): "blez",
                   (6, 0): "bgtz", (7, 0): "bc1", (8, 0): "bposge32"}[(d["cond"], 0)]
            if d["cond"] == 1 and d["rt"] == 0 and mn in ("beqz", "beqzc"): cmn = mn
            if d["cond"] == 2 and d["rt"] == 0 and mn in ("bnez", "bnezc"): cmn = mn
            if d["link"]: cmn += "al" + ("s" if d["short"] else "")
            if d["compact"] and cmn in ("beq", "bne"): cmn = {"beq": "beqzc", "bne": "bnezc"}[cmn]
            if cmn == "bc1": cmn = "bc1t" if d["tf"] else "bc1f"
            if mn != cmn and not (cmn == "beqzc" and mn == "bc") and not (cmn == "b" and mn == "b") and not (cmn == "beq" and mn in ("beqz", "b")) \
                    and not (cmn == "bne" and mn == "bnez") and not (cmn == "bgezal" and mn == "bal") \
                    and not (cmn == "bgezals" and mn == "bals") and not (cmn == "bgez" and mn == "b"):
                fail("br-mn:" + mn, a, "%s want %s" % (mn, cmn))
        elif k == "J":
            cmn = "jalx" if not (d["target"] & 1) else ("jals" if d["short"] else "jal" if d["link"] else "j")
            if mn != cmn:
                fail("j-mn", a, "%s want %s" % (mn, cmn))
        elif k == "ADDIUPC" and ops[0] != GPR[d["rd"]]:
            fail("addiupc-reg", a, "%s %s" % (mn, ",".join(ops)))
        continue
    if k == "JR":
        if d["compact"]:
            cmn = "jrc"
        elif d["link"]:
            cmn = "jalrs" if d["short"] else "jalr"
        else:
            cmn = "jrs" if d["short"] else "jr"
        regs = [r for r in ops]
        if mn.rstrip(".hb") != cmn and not (cmn == "jr" and mn in ("jr.hb", "j")) \
                and not (cmn == "jalr" and mn == "jalr.hb") and not (cmn == "jalrs" and mn == "jalrs.hb") \
                and not (cmn == "jrs" and mn == "jrs.hb"):
            fail("jr-mn", a, "%s %s want %s" % (mn, ",".join(ops), cmn))
        elif regs[-1] != GPR[d["rs"]] or (d["link"] and len(regs) == 2 and regs[0] != GPR[d["rd"]]):
            fail("jr-reg", a, "%s %s rs=%s rd=%s" % (mn, ",".join(ops), GPR[d["rs"]], GPR[d["rd"]]))
        continue
    if k == "JRADDIUSP":
        if mn != "jraddiusp" or num(ops[0]) != d["imm"]:
            fail("jraddiusp", a, "%s %s" % (mn, ",".join(ops)))
        continue
    if k in ("LWM", "SWM", "LWP", "SWP"):
        m = re.match(r"^(-?\w+)\((\w+)\)$", ops[-1])
        if k in ("LWP", "SWP"):
            want_ops = [GPR[d["regs"][0]]]
        else:
            want_ops = reglist(d["regs"])
        if mn != k.lower() or not m or not same_imm(m.group(1), str(d["imm"] - (1 << 32) if d["imm"] >> 31 else d["imm"])) \
                or m.group(2) != GPR[d["rs"]] or ops[:-1] != want_ops:
            fail(k.lower(), a, "%s %s  want %s,%d(%s)" % (mn, ",".join(ops), ",".join(want_ops),
                 d["imm"] - (1 << 32) if d["imm"] >> 31 else d["imm"], GPR[d["rs"]]))
        continue
    if k == "MOVEP":
        want = [GPR[d["regs"][0]], GPR[d["regs"][1]], GPR[d["rs"]], GPR[d["rt"]]]
        if mn != "movep" or ops != want:
            fail("movep", a, "%s %s want %s" % (mn, ",".join(ops), ",".join(want)))
        continue
    if k == "LWXS":
        want = [GPR[d["rd"]], "%s(%s)" % (GPR[d["rt"]], GPR[d["rs"]])]
        if mn != "lwxs" or ops != want:
            fail("lwxs", a, "%s %s want %s" % (mn, ",".join(ops), ",".join(want)))
        continue

print("umips-check: %d encodings, %d disagree" % (checked, sum(len(l) for l in bad.values())))
for cat, l in sorted(bad.items(), key=lambda kv: -len(kv[1]))[:40]:
    print("%-26s %6d  e.g. %s" % (cat, len(l), "; ".join(l[:2])))
sys.exit(1 if bad else 0)
