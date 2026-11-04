#!/usr/bin/env python3
"""Generates isa/mips32/umips_dsp.h: the DSP ASE's microMIPS encodings and how each
recodes to MIPS32, derived from the GNU assembler. Every instruction is assembled in
both ISAs with its operands zero, then with each operand bit set alone: the bits that
change in each encoding place that operand bit. An entry is the microMIPS word with
its operands zero, the mask of the bits that are operands, the MIPS32 word with its
operands zero, and the operand bits' positions in each.

usage: gen-umips-dsp.py OUT.h   (needs mipsel-linux-gnu-as/objdump with the DSP ASE)
"""
import os, subprocess, sys, tempfile

CROSS = os.environ.get("CROSS", "mipsel-linux-gnu-")

# operand kinds: (width, signed, spelling)
K = {"r": (5, 0, "${}"), "ac": (2, 0, "$ac{}"), "u3": (3, 0, "{}"), "u4": (4, 0, "{}"), "u5": (5, 0, "{}"),
     "u6": (6, 0, "{}"), "u8": (8, 0, "{}"), "s6": (6, 1, "{}"), "s10": (10, 1, "{}"), "u2": (2, 0, "{}")}
R3 = ["r", "r", "r"]
INSNS = [
    # three registers: rd, rs, rt
    *[(m, R3) for m in """addu.qb subu.qb addu_s.qb subu_s.qb muleu_s.ph.qbl muleu_s.ph.qbr addu.ph subu.ph
      addq.ph subq.ph addu_s.ph subu_s.ph addq_s.ph subq_s.ph addsc addwc modsub addq_s.w subq_s.w
      muleq_s.w.phl muleq_s.w.phr mulq_s.ph mulq_rs.ph adduh.qb subuh.qb adduh_r.qb subuh_r.qb addqh.ph
      subqh.ph addqh_r.ph subqh_r.ph mul.ph mul_s.ph addqh.w subqh.w addqh_r.w subqh_r.w mulq_s.w mulq_rs.w
      cmpgu.eq.qb cmpgu.lt.qb cmpgu.le.qb cmpgdu.eq.qb cmpgdu.lt.qb cmpgdu.le.qb pick.qb pick.ph
      precrq.qb.ph precr.qb.ph packrl.ph precrqu_s.qb.ph precrq.ph.w precrq_rs.ph.w
      shllv.qb shrlv.qb shrav.qb shrav_r.qb shllv.ph shrav.ph shllv_s.ph shrav_r.ph shrlv.ph
      shllv_s.w shrav_r.w""".split()],
    *[(m, ["r", "r"]) for m in """raddu.w.qb absq_s.qb absq_s.ph absq_s.w replv.qb replv.ph preceq.w.phl
      preceq.w.phr precequ.ph.qbl precequ.ph.qbr precequ.ph.qbla precequ.ph.qbra preceu.ph.qbl
      preceu.ph.qbr preceu.ph.qbla preceu.ph.qbra bitrev insv""".split()],
    *[(m, ["r", "r"]) for m in "cmpu.eq.qb cmpu.lt.qb cmpu.le.qb cmp.eq.ph cmp.lt.ph cmp.le.ph".split()],
    ("repl.qb", ["r", "u8"]), ("repl.ph", ["r", "s10"]),
    *[(m, ["r", "r", "u3"]) for m in "shll.qb shrl.qb shra.qb shra_r.qb".split()],
    *[(m, ["r", "r", "u4"]) for m in "shll.ph shra.ph shll_s.ph shra_r.ph shrl.ph".split()],
    *[(m, ["r", "r", "u5"]) for m in "shll_s.w shra_r.w".split()],
    *[(m, ["ac", "r", "r"]) for m in """dpau.h.qbl dpau.h.qbr dpsu.h.qbl dpsu.h.qbr dpaq_s.w.ph dpsq_s.w.ph
      mulsaq_s.w.ph dpaq_sa.l.w dpsq_sa.l.w maq_s.w.phl maq_s.w.phr maq_sa.w.phl maq_sa.w.phr dpa.w.ph
      dps.w.ph mulsa.w.ph dpax.w.ph dpsx.w.ph dpaqx_s.w.ph dpsqx_s.w.ph dpaqx_sa.w.ph dpsqx_sa.w.ph
      mult multu madd maddu msub msubu""".split()],
    *[(m, ["r", "ac", "u5"]) for m in "extr.w extr_r.w extr_rs.w extr_s.h extp extpdp".split()],
    *[(m, ["r", "ac", "r"]) for m in "extrv.w extrv_r.w extrv_rs.w extrv_s.h extpv extpdpv".split()],
    ("shilo", ["ac", "s6"]), ("shilov", ["ac", "r"]), ("mthlip", ["r", "ac"]),
    *[(m, ["r", "ac"]) for m in "mfhi mflo mthi mtlo".split()],
    ("rddsp", ["r", "u6"]), ("wrdsp", ["r", "u6"]),
    ("append", ["r", "r", "u5"]), ("prepend", ["r", "r", "u5"]), ("balign", ["r", "r", "u2"]),
    ("precr_sra.ph.w", ["r", "r", "u5"]), ("precr_sra_r.ph.w", ["r", "r", "u5"]),
    ("lwx", ["r", "r", "r"]), ("lhx", ["r", "r", "r"]), ("lbux", ["r", "r", "r"]),
]

def spell(mn, kinds, vals):
    ops = [K[k][2].format(v) for k, v in zip(kinds, vals)]
    if mn in ("lwx", "lhx", "lbux"):
        return "%s %s,%s(%s)" % (mn, ops[0], ops[1], ops[2])
    return "%s %s" % (mn, ",".join(ops))

def assemble(lines, micro):
    d = tempfile.mkdtemp()
    src, obj = os.path.join(d, "a.s"), os.path.join(d, "a.o")
    head = ("\t.set micromips\n\t.set insn32\n" if micro else "") + "\t.set noreorder\n\t.set nomacro\n\t.set noat\n"
    open(src, "w").write(head + "\t.text\n\t.globl f\n\t.ent f\nf:\n" + "".join("\t%s\n" % l for l in lines))
    r = subprocess.run([CROSS + "as", "-march=mips32r2", "-mdspr2", "-EL", src, "-o", obj],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[:2000])
    out = subprocess.run([CROSS + "objcopy", "-O", "binary", "-j", ".text", obj, "/dev/stdout"],
                         capture_output=True, check=True).stdout
    words = []
    for k in range(0, len(out), 4):
        h0, h1 = int.from_bytes(out[k:k + 2], "little"), int.from_bytes(out[k + 2:k + 4], "little")
        words.append((h0 << 16 | h1) if micro else int.from_bytes(out[k:k + 4], "little"))
    return words

# BALIGN's byte positions 0 and 2 the assembler writes as other instructions, so its
# bit 0 is not probed: it starts at 1, and bit 0 is placed just below bit 1 in each
# encoding (the field is contiguous) and cleared from the zero-operand words.
BASE = {"balign": [0, 0, 1]}
FIXED = {("balign", 2, 0)}

def probes(mn, kinds):
    """Operand value lists: the baseline (zero but where BASE says), then each bit of
    each operand toggled alone (but FIXED ones)."""
    base = BASE.get(mn, [0] * len(kinds))
    out = [(None, None, base)]
    for j, k in enumerate(kinds):
        w, sg, _ = K[k]
        for b in range(w):
            if (mn, j, b) in FIXED:
                continue
            v = list(base)
            x = (base[j] ^ (1 << b)) & ((1 << w) - 1)
            v[j] = x - (1 << w) if sg and x >> (w - 1) else x
            out.append((j, b, v))
    return out

rows = []
lines, idx = [], []
for mn, kinds in INSNS:
    for (j, b, v) in probes(mn, kinds):
        lines.append(spell(mn, kinds, v))
        idx.append((mn, kinds, j, b))
um, m32 = assemble(lines, True)[:len(lines)], assemble(lines, False)[:len(lines)]   # (.text is padded)
assert len(um) == len(m32) == len(lines), (len(um), len(m32), len(lines))

entries = {}
for (mn, kinds, j, b), u, w in zip(idx, um, m32):
    e = entries.setdefault(mn, {"kinds": kinds, "bits": []})
    if j is None:
        e["u0"], e["w0"] = u, w
        continue
    du, dw = u ^ e["u0"], w ^ e["w0"]
    if bin(du).count("1") != 1 or bin(dw).count("1") != 1:
        sys.exit("%s operand %d bit %d: %x / %x" % (mn, j, b, du, dw))
    e["bits"].append((du.bit_length() - 1, dw.bit_length() - 1))

for mn, e in entries.items():
    for (fm, j, b) in FIXED:
        if fm == mn:
            k = sum(1 for kk in e["kinds"][:j] for _ in range(K[kk][0])) + b   # bit b+1's slot
            ub, wb = e["bits"][k][0] - 1, e["bits"][k][1] - 1
            assert (e["u0"] >> ub) & 1 and (e["w0"] >> wb) & 1
            e["u0"] &= ~(1 << ub)
            e["w0"] &= ~(1 << wb)
            e["bits"].insert(k, (ub, wb))

with open(sys.argv[1], "w") as fp:
    fp.write("/* generated by tests/isa-mips/gen-umips-dsp.py from the GNU assembler: do not edit.\n"
             " * The DSP ASE in microMIPS: {microMIPS word with operands zero, operand mask, the\n"
             " * MIPS32 word with operands zero, then (microMIPS bit, MIPS32 bit) per operand bit}. */\n")
    fp.write("#define UMIPS_DSP_MAXBITS %d\n" % max(len(e["bits"]) for e in entries.values()))
    fp.write("static const struct umips_dsp { uint32_t u, umask, w; uint8_t n; uint8_t bit[%d][2]; } umips_dsp[] = {\n"
             % max(len(e["bits"]) for e in entries.values()))
    for mn, e in entries.items():
        mask = 0
        for ub, _ in e["bits"]:
            mask |= 1 << ub
        bits = ", ".join("{%d, %d}" % p for p in e["bits"])
        fp.write("    { 0x%08x, 0x%08x, 0x%08x, %d, { %s } }, /* %s */\n"
                 % (e["u0"], (~mask) & 0xFFFFFFFF, e["w0"], len(e["bits"]), bits, mn))
    fp.write("};\n")
print("%d instructions" % len(entries))
