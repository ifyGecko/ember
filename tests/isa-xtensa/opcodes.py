"""Xtensa instruction-encoding table (little-endian bit numbering).

Ground truth for encodings: the GNU assemblers/objdumps of four Xtensa cores
(see scripts/verify.py; results in opcodes-check.txt):
    lx106   ESP8266 (Xtensa LX106)          Debian binutils xtensa-lx106-elf
    esp32   ESP32   (Xtensa LX6)            Espressif xtensa-esp32-elf
    esp32s2 ESP32-S2 (Xtensa LX7, no FPU/MAC16/boolean/loop)
    esp32s3 ESP32-S3 (Xtensa LX7)           Espressif xtensa-esp32s3-elf
Meaning/semantics: Tensilica Xtensa ISA Reference Manual (2010).

Word layout (24-bit word w = b0 | b1<<8 | b2<<16 for bytes b0,b1,b2 at
increasing addresses, i.e. little-endian; 16-bit narrow word w = b0 | b1<<8):

    field   bits (LE)      field   bits (LE)
    op0     w[3:0]         imm8    w[23:16]
    t       w[7:4]         imm12   w[23:12]   (BRI12: s is w[11:8])
    s       w[11:8]        imm16   w[23:8]    (RI16)
    r       w[15:12]       offset  w[23:6]    (CALL)
    op1     w[19:16]       n       w[5:4]     (CALL/CALLX/BRI8/BRI12)
    op2     w[23:20]       m       w[7:6]     (CALLX/BRI8/BRI12)
    sr      w[15:8]  = r<<4|s  (RSR format: RSR/WSR/XSR/WUR)
Narrow (16-bit) formats use the same op0/t/s/r positions (w[15:0]).
For the big-endian layout see encoding.md.

Each OPCODES entry is a dict:
    mn      mnemonic (as GNU as/objdump spell it)
    key     unique id: mn, or mn@variant for a model-specific variant
    variant None, or the name of the only toolchain using this variant
    opt     option (see OPTIONS)
    fmt     instruction format (RRR, RRI4, RRI8, RI16, RSR, CALL, CALLX, BRI8,
            BRI12, RRRN, RI7, RI6)
    len     2 or 3 bytes
    fixed   {field: value} -- every bit not covered by `fixed` or by an
            operand is listed in `fixed` (so mask/value fully identify the
            encoding); sub-fields are named like 'r3' (bit 3 of r) or 't10'
            (bits 1..0 of t) -- see FIELDS.
    ops     operand list in assembler order: (name, kind, field) or
            (name, kind) for kinds whose field set is implicit -- see KINDS.
    syntax  assembler syntax
    models  list of models whose assembler accepts it AND whose objdump
            decodes it to the same text (filled by scripts/verify.py; [] =
            manual-only, no toolchain here knows it)
    note    optional remark
mask/value: computed by match(entry) -> (mask, value) over w.
"""

# ----------------------------------------------------------------- fields
# name: (lsb, width) in the LE instruction word
FIELDS = {
    "op0": (0, 4), "t": (4, 4), "s": (8, 4), "r": (12, 4),
    "op1": (16, 4), "op2": (20, 4),
    "imm8": (16, 8), "imm12": (12, 12), "imm16": (8, 16), "offset": (6, 18),
    "n": (4, 2), "m": (6, 2), "sr": (8, 8),
    # sub-fields
    "t0": (4, 1), "t3": (7, 1), "t2": (6, 1), "t10": (4, 2), "t31": (5, 3),
    "t32": (6, 2), "t20": (4, 3),
    "r0": (12, 1), "r3": (15, 1), "r2": (14, 1), "r10": (12, 2), "r31": (13, 3),
    "r32": (14, 2),
    "op1_0": (16, 1), "op1_31": (17, 3), "op2_0": (20, 1), "op2_31": (21, 3),
    "imm8_hi": (20, 4),     # RRI4 imm4 (cache-lock ops): w[23:20]
}


def getf(w, f):
    lsb, n = FIELDS[f]
    return (w >> lsb) & ((1 << n) - 1)


def putf(f, v):
    lsb, n = FIELDS[f]
    assert 0 <= v < (1 << n), (f, v)
    return v << lsb


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


B4CONST = [-1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256]
B4CONSTU = [32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256]

# ----------------------------------------------------------------- operand kinds
# Each kind: dec(w, pc) -> value, enc(value, pc) -> bits, fmt(value) -> str,
# samples (list of representative legal values; pc-relative kinds give
# displacements relative to pc), fields covered, and a description.
# Register kinds take the field name as the 3rd tuple element.


def _imm_fmt(v):
    # binutils xtensa-dis prints non-register, non-pcrel operands as %d when
    # -256 < v < 256, else 0x%x of the 32-bit value
    return "%d" % v if -256 < v < 256 else "0x%x" % (v & 0xffffffff)


def _addr_fmt(v):
    return "%x" % (v & 0xffffffff)


class Kind:
    def __init__(self, desc, fields, dec, enc, fmt=_imm_fmt, samples=(),
                 pcrel=False):
        self.desc, self.fields, self.dec, self.enc = desc, fields, dec, enc
        self.fmt, self.samples, self.pcrel = fmt, list(samples), pcrel


def _reg(prefix, desc):
    def mk(field):
        return Kind(desc + " in " + field, [field],
                    lambda w, pc, f=field: getf(w, f),
                    lambda v, pc, f=field: putf(f, v),
                    lambda v: "%s%d" % (prefix, v), [0, 1, 7, 15])
    return mk


def _bgroup(k):
    def mk(field):
        return Kind("BR group b[%s..%s+%d] (%s must be a multiple of %d; the "
                    "manual leaves other values undefined, objdump decodes "
                    "them as %s & ~%d)" % (field, field, k - 1, field, k, field, k - 1),
                    [field],
                    lambda w, pc, f=field: getf(w, f) & ~(k - 1),
                    lambda v, pc, f=field: putf(f, v),
                    lambda v: ":".join("b%d" % (v + i) for i in range(k)),
                    [i for i in range(0, 16, k)])
    return mk


def K(desc, fields, dec, enc, samples, fmt=_imm_fmt, pcrel=False):
    return Kind(desc, fields, dec, enc, fmt, samples, pcrel)


def _u(field, scale=1, add=0):
    n = FIELDS[field][1]
    lo, hi = add, ((1 << n) - 1) * scale + add
    return K("unsigned: %s%s%s (range %d..%d%s)" % (
        field, "*%d" % scale if scale != 1 else "",
        "+%d" % add if add else "", lo, hi,
        ", step %d" % scale if scale != 1 else ""),
        [field], lambda w, pc: getf(w, field) * scale + add,
        lambda v, pc: putf(field, (v - add) // scale),
        sorted({lo, hi, lo + scale, (lo + hi) // 2 // scale * scale + (add % scale)}))


def _s(field, scale=1):
    n = FIELDS[field][1]
    lo, hi = -(1 << (n - 1)) * scale, ((1 << (n - 1)) - 1) * scale
    return K("signed: sext(%s)%s (range %d..%d)" % (
        field, "*%d" % scale if scale != 1 else "", lo, hi),
        [field], lambda w, pc: sext(getf(w, field), n) * scale,
        lambda v, pc: putf(field, (v // scale) & ((1 << n) - 1)),
        [lo, hi, 0, scale, -scale])


AREG, FREG, BREG = _reg("a", "AR"), _reg("f", "FR"), _reg("b", "BR")
B4, B8 = _bgroup(4), _bgroup(8)

KINDS = {
    # MAC16 registers
    "mw": K("MR m0..m3 = r[1:0]", ["r10"], lambda w, pc: getf(w, "r10"),
            lambda v, pc: putf("r10", v), [0, 1, 2, 3], fmt=lambda v: "m%d" % v),
    "mx": K("MR m0/m1 = r[2]", ["r2"], lambda w, pc: getf(w, "r2"),
            lambda v, pc: putf("r2", v), [0, 1], fmt=lambda v: "m%d" % v),
    "my": K("MR m2/m3 = 2 + t[2]", ["t2"], lambda w, pc: 2 + getf(w, "t2"),
            lambda v, pc: putf("t2", v - 2), [2, 3], fmt=lambda v: "m%d" % v),
    # immediates
    "movi12": K("signed 12-bit: sext(s<<8 | imm8) (-2048..2047)", ["s", "imm8"],
                lambda w, pc: sext(getf(w, "s") << 8 | getf(w, "imm8"), 12),
                lambda v, pc: putf("s", (v >> 8) & 15) | putf("imm8", v & 255),
                [-2048, 2047, 0, -1, 255, 256]),
    "movin7": K("MOVI.N imm7 = t[2:0]<<4 | r; value = imm7 if imm7 < 96 "
                "else imm7 - 128 (-32..95)", ["t20", "r"],
                lambda w, pc: (lambda i: i - 128 if i >= 96 else i)(
                    getf(w, "t20") << 4 | getf(w, "r")),
                lambda v, pc: putf("t20", ((v & 127) >> 4)) | putf("r", v & 15),
                [-32, 95, 0, -1, 64]),
    "addin4": K("ADDI.N: t==0 -> -1, else t (-1, 1..15)", ["t"],
                lambda w, pc: -1 if getf(w, "t") == 0 else getf(w, "t"),
                lambda v, pc: putf("t", 0 if v == -1 else v), [-1, 1, 15, 8]),
    "b4const": K("B4CONST[r] = {-1,1,2,3,4,5,6,7,8,10,12,16,32,64,128,256}",
                 ["r"], lambda w, pc: B4CONST[getf(w, "r")],
                 lambda v, pc: putf("r", B4CONST.index(v)), [-1, 1, 256, 10, 128]),
    "b4constu": K("B4CONSTU[r] = {32768,65536,2,3,4,5,6,7,8,10,12,16,32,64,128,256}",
                  ["r"], lambda w, pc: B4CONSTU[getf(w, "r")],
                  lambda v, pc: putf("r", B4CONSTU.index(v)),
                  [32768, 65536, 2, 256, 12]),
    "slli_sa": K("SLLI: sa = 32 - (op2[0]<<4 | t) (1..31; field value 0 "
                 "would mean 32 and is not assemblable)", ["op2_0", "t"],
                 lambda w, pc: 32 - (getf(w, "op2_0") << 4 | getf(w, "t")),
                 lambda v, pc: putf("op2_0", (32 - v) >> 4) | putf("t", (32 - v) & 15),
                 [1, 31, 16, 15, 17]),
    "srai_sa": K("SRAI: sa = op2[0]<<4 | s (0..31)",
                 ["op2_0", "s"], lambda w, pc: getf(w, "op2_0") << 4 | getf(w, "s"),
                 lambda v, pc: putf("op2_0", v >> 4) | putf("s", v & 15),
                 [0, 31, 16, 15, 1]),
    "ssai_sa": K("SSAI: sa = t[0]<<4 | s (0..31); t[3:1] = 0", ["t0", "s"],
                 lambda w, pc: getf(w, "t0") << 4 | getf(w, "s"),
                 lambda v, pc: putf("t0", v >> 4) | putf("s", v & 15),
                 [0, 31, 16, 15, 1]),
    "extui_sh": K("EXTUI shift = op1[0]<<4 | s (0..31)", ["op1_0", "s"],
                  lambda w, pc: getf(w, "op1_0") << 4 | getf(w, "s"),
                  lambda v, pc: putf("op1_0", v >> 4) | putf("s", v & 15),
                  [0, 31, 16, 15, 1]),
    "extui_mk": K("EXTUI mask bits = op2 + 1 (1..16)", ["op2"],
                  lambda w, pc: getf(w, "op2") + 1,
                  lambda v, pc: putf("op2", v - 1), [1, 1, 16, 1, 16]),
    "sext7": K("t + 7 (7..22)", ["t"], lambda w, pc: getf(w, "t") + 7,
               lambda v, pc: putf("t", v - 7), [7, 22, 8, 15, 16]),
    "l32e_off": K("L32E/S32E: (r<<2) - 64 (-64..-4, step 4)", ["r"],
                  lambda w, pc: (getf(w, "r") << 2) - 64,
                  lambda v, pc: putf("r", (v + 64) >> 2), [-64, -4, -32, -60]),
    "rotw": K("signed 4-bit t (-8..7)", ["t"], lambda w, pc: sext(getf(w, "t"), 4),
              lambda v, pc: putf("t", v & 15), [-8, 7, 0, -1, 1]),
    "bbi": K("bit number = r[0]<<4 | t (0..31)", ["r0", "t"],
             lambda w, pc: getf(w, "r0") << 4 | getf(w, "t"),
             lambda v, pc: putf("r0", v >> 4) | putf("t", v & 15),
             [0, 31, 16, 15, 1]),
    "entry_imm": K("imm12 << 3 (0..32760, step 8)", ["imm12"],
                   lambda w, pc: getf(w, "imm12") << 3,
                   lambda v, pc: putf("imm12", v >> 3), [0, 32760, 8, 16, 256]),
    "dce_imm": K("imm4 (w[23:20]) << 4 (0..240, step 16)", ["imm8_hi"],
                 lambda w, pc: getf(w, "imm8_hi") << 4,
                 lambda v, pc: putf("imm8_hi", v >> 4), [0, 240, 16, 128]),
    "sr": K("special register number = r<<4 | s (w[15:8]); objdump/as spell "
            "it as a suffix: rsr.<name>", ["sr"], lambda w, pc: getf(w, "sr"),
            lambda v, pc: putf("sr", v), [3]),
    "ur_rur": K("user register number = s<<4 | t (w[11:4])", ["s", "t"],
                lambda w, pc: getf(w, "s") << 4 | getf(w, "t"),
                lambda v, pc: putf("s", v >> 4) | putf("t", v & 15), [231]),
    "ur_wur": K("user register number = r<<4 | s (w[15:8])", ["sr"],
                lambda w, pc: getf(w, "sr"), lambda v, pc: putf("sr", v), [231]),
    # pc-relative (value = absolute target address; samples are target - pc)
    "l32r": K("L32R: addr = ((pc + 3) & ~3) + ((imm16 | 0xffff0000) << 2), i.e. "
              "imm16 is one-extended: -262144..-4 bytes from (pc+3)&~3 "
              "(LITBASE[0]=1 instead gives LITBASE[31:12]<<12 + same offset)",
              ["imm16"],
              lambda w, pc: (((pc + 3) & ~3) + ((getf(w, "imm16") | ~0xffff) << 2))
              & 0xffffffff,
              lambda v, pc: putf("imm16", ((v - ((pc + 3) & ~3)) >> 2) & 0xffff),
              [-4, -262144, -64, -1024], fmt=_addr_fmt, pcrel=True),
    "call": K("CALLn: target = (pc & ~3) + (sext(offset,18) << 2) + 4 "
              "(-524284..+524288 from pc&~3)", ["offset"],
              lambda w, pc: ((pc & ~3) + (sext(getf(w, "offset"), 18) << 2) + 4)
              & 0xffffffff,
              lambda v, pc: putf("offset", ((v - (pc & ~3) - 4) >> 2) & 0x3ffff),
              [4, 524288, -524284, 0, 400], fmt=_addr_fmt, pcrel=True),
    "j": K("J: target = pc + 4 + sext(offset,18) (-131068..+131075)", ["offset"],
           lambda w, pc: (pc + 4 + sext(getf(w, "offset"), 18)) & 0xffffffff,
           lambda v, pc: putf("offset", (v - pc - 4) & 0x3ffff),
           [4, 131075, -131068, 0, 1000], fmt=_addr_fmt, pcrel=True),
    "br8": K("target = pc + 4 + sext(imm8) (-124..+131)", ["imm8"],
             lambda w, pc: (pc + 4 + sext(getf(w, "imm8"), 8)) & 0xffffffff,
             lambda v, pc: putf("imm8", (v - pc - 4) & 0xff),
             [4, 131, -124, 0, 20], fmt=_addr_fmt, pcrel=True),
    "br12": K("target = pc + 4 + sext(imm12) (-2044..+2051)", ["imm12"],
              lambda w, pc: (pc + 4 + sext(getf(w, "imm12"), 12)) & 0xffffffff,
              lambda v, pc: putf("imm12", (v - pc - 4) & 0xfff),
              [4, 2051, -2044, 0, 300], fmt=_addr_fmt, pcrel=True),
    "loop": K("LEND = pc + 4 + imm8 (unsigned, +4..+259); LBEG = pc + 3",
              ["imm8"], lambda w, pc: (pc + 4 + getf(w, "imm8")) & 0xffffffff,
              lambda v, pc: putf("imm8", v - pc - 4), [4, 259, 100, 5],
              fmt=_addr_fmt, pcrel=True),
    "brn6": K("BEQZ.N/BNEZ.N: target = pc + 4 + imm6, imm6 = t[1:0]<<4 | r "
              "(unsigned, +4..+67)", ["t10", "r"],
              lambda w, pc: (pc + 4 + (getf(w, "t10") << 4 | getf(w, "r"))) & 0xffffffff,
              lambda v, pc: putf("t10", (v - pc - 4) >> 4) | putf("r", (v - pc - 4) & 15),
              [4, 67, 20, 36], fmt=_addr_fmt, pcrel=True),
}


_KCACHE = {}


def kind_of(op):
    """op is (name, kind[, field]); returns the Kind object."""
    try:
        return _KCACHE[op]
    except KeyError:
        k = _KCACHE[op] = _kind_of(op)
        return k


def _kind_of(op):
    k = op[1]
    if k == "a":
        return AREG(op[2])
    if k == "f":
        return FREG(op[2])
    if k == "b":
        return BREG(op[2])
    if k == "b4":
        return B4(op[2])
    if k == "b8":
        return B8(op[2])
    if k == "u":
        return _u(op[2], *(op[3:] if len(op) > 3 else ()))
    if k == "s":
        return _s(op[2], *(op[3:] if len(op) > 3 else ()))
    return KINDS[k]


# ----------------------------------------------------------------- options
OPTIONS = {
    "core": "Core architecture",
    "density": "Code Density option (16-bit .N instructions, op0 = 8..13)",
    "windowed": "Windowed Register option",
    "loop": "Loop option (zero-overhead loops)",
    "boolean": "Boolean option (b0..b15)",
    "fp": "Single-precision Floating-Point coprocessor option (f0..f15)",
    "fp_ext": "FP division/sqrt/ext ops (DIV0.S etc.; ESP32/ESP32-S3 FPU)",
    "mac16": "MAC16 option (ACC, m0..m3)",
    "mul16": "16-bit Integer Multiply option",
    "mul32": "32-bit Integer Multiply option (MULL)",
    "mul32_high": "32-bit Integer Multiply option, high result (MULUH/MULSH)",
    "div32": "32-bit Integer Divide option",
    "nsa": "Miscellaneous Operations: NSA/NSAU",
    "minmax": "Miscellaneous Operations: MIN/MAX/MINU/MAXU",
    "sext": "Miscellaneous Operations: SEXT",
    "clamps": "Miscellaneous Operations: CLAMPS",
    "s32c1i": "Conditional Store option",
    "mp_sync": "Multiprocessor Synchronization option (L32AI/S32RI)",
    "exception": "Exception option",
    "interrupt": "Interrupt / High-Priority Interrupt options",
    "debug": "Debug option (incl. OCD LDDR32.P/SDDR32.P, RFDO/RFDD)",
    "tlb": "Region Protection / Region Translation / MMU option",
    "cache": "Instruction/Data Cache options (implementation specific)",
    "cache_lock": "Instruction/Data Cache Index Lock option",
    "cache_test": "Cache test (LICT/SICT/LICW/SICW/LDCT/SDCT)",
    "salt": "LX7 set-less-than (SALT/SALTU)",
    "s32nb": "LX6+ non-buffered store (S32NB)",
    "sim": "Simulator call (SIMCALL; NOP on hardware)",
    "coproc": "Coprocessor context option",
}

OPCODES = []


def I(mn, opt, fmt, fixed, ops=(), syntax=None, note=None, length=None,
      variant=None):
    e = dict(mn=mn, opt=opt, fmt=fmt, variant=variant,
             key=mn + ("@" + variant if variant else ""),
             len=length or (2 if fmt in ("RRRN", "RI7", "RI6") else 3),
             fixed=dict(fixed), ops=[tuple(o) for o in ops], note=note)
    if syntax is None:
        syntax = mn + (" " + ", ".join(o[0] for o in ops) if ops else "")
    e["syntax"] = syntax
    e["models"] = None
    OPCODES.append(e)
    return e


def A(n, f):
    return (n, "a", f)


ar, as_, at = A("ar", "r"), A("as", "s"), A("at", "t")
fr, fs, ft = ("fr", "f", "r"), ("fs", "f", "s"), ("ft", "f", "t")
br_, bs, bt = ("br", "b", "r"), ("bs", "b", "s"), ("bt", "b", "t")

# =========================================================== op0 = 0 (QRST)
# --- RST0 (op1=0) / ST0 (op2=0) / SNM0 (r=0): CALLX format, t = m<<2 | n
Q0 = dict(op0=0, op1=0, op2=0, r=0)
I("ill", "core", "CALLX", dict(Q0, t=0, s=0))
I("ret", "core", "CALLX", dict(Q0, t=8, s=0))
I("retw", "windowed", "CALLX", dict(Q0, t=9, s=0))
I("jx", "core", "CALLX", dict(Q0, t=0xa), [as_])
I("callx0", "core", "CALLX", dict(Q0, t=0xc), [as_])
I("callx4", "windowed", "CALLX", dict(Q0, t=0xd), [as_])
I("callx8", "windowed", "CALLX", dict(Q0, t=0xe), [as_])
I("callx12", "windowed", "CALLX", dict(Q0, t=0xf), [as_])
S0 = dict(op0=0, op1=0, op2=0)
I("movsp", "windowed", "RRR", dict(S0, r=1), [at, as_])
for t, mn in [(0, "isync"), (1, "rsync"), (2, "esync"), (3, "dsync"),
              (8, "excw"), (0xc, "memw"), (0xd, "extw"), (0xf, "nop")]:
    I(mn, "exception" if mn == "excw" else "core", "RRR", dict(S0, r=2, s=0, t=t))
I("rfe", "exception", "RRR", dict(S0, r=3, t=0, s=0))
I("rfue", "exception", "RRR", dict(S0, r=3, t=0, s=1),
  note="manual only (XEA1); no toolchain here accepts it")
I("rfde", "exception", "RRR", dict(S0, r=3, t=0, s=2))
I("rfwo", "windowed", "RRR", dict(S0, r=3, t=0, s=4))
I("rfwu", "windowed", "RRR", dict(S0, r=3, t=0, s=5))
I("rfi", "interrupt", "RRR", dict(S0, r=3, t=1), [("level", "u", "s")])
I("rfme", "exception", "RRR", dict(S0, r=3, t=2, s=0),
  note="manual only (Memory ECC/Parity option)")
I("break", "debug", "RRR", dict(S0, r=4), [("imm_s", "u", "s"), ("imm_t", "u", "t")])
I("syscall", "exception", "RRR", dict(S0, r=5, s=0, t=0))
I("simcall", "sim", "RRR", dict(S0, r=5, s=1, t=0),
  note="not in the 2010 manual's tables; ISS simulator call")
I("rsil", "interrupt", "RRR", dict(S0, r=6), [at, ("level", "u", "s")])
I("waiti", "interrupt", "RRR", dict(S0, r=7, t=0), [("level", "u", "s")])
I("lddr32.p", "debug", "RRR", dict(S0, r=7, t=0xe), [as_],
  note="OCD: load DDR from mem[as], as += 4; not in the 2010 manual tables")
I("sddr32.p", "debug", "RRR", dict(S0, r=7, t=0xf), [as_],
  note="OCD: store DDR to mem[as], as += 4; not in the 2010 manual tables")
I("any4", "boolean", "RRR", dict(S0, r=8), [bt, ("bs", "b4", "s")])
I("all4", "boolean", "RRR", dict(S0, r=9), [bt, ("bs", "b4", "s")])
I("any8", "boolean", "RRR", dict(S0, r=0xa), [bt, ("bs", "b8", "s")])
I("all8", "boolean", "RRR", dict(S0, r=0xb), [bt, ("bs", "b8", "s")])
# --- RST0 3-register ALU
for op2, mn in [(1, "and"), (2, "or"), (3, "xor"), (8, "add"), (9, "addx2"),
                (0xa, "addx4"), (0xb, "addx8"), (0xc, "sub"), (0xd, "subx2"),
                (0xe, "subx4"), (0xf, "subx8")]:
    I(mn, "core", "RRR", dict(op0=0, op1=0, op2=op2), [ar, as_, at])
# --- ST1 (op2=4)
S1 = dict(op0=0, op1=0, op2=4)
I("ssr", "core", "RRR", dict(S1, r=0, t=0), [as_])
I("ssl", "core", "RRR", dict(S1, r=1, t=0), [as_])
I("ssa8l", "core", "RRR", dict(S1, r=2, t=0), [as_])
I("ssa8b", "core", "RRR", dict(S1, r=3, t=0), [as_])
I("ssai", "core", "RRR", dict(S1, r=4, t31=0), [("sa", "ssai_sa")])
I("rer", "core", "RRR", dict(S1, r=6), [at, as_],
  note="external register read: AR[t] <- ERI[AR[s]]")
I("wer", "core", "RRR", dict(S1, r=7), [at, as_],
  note="external register write: ERI[AR[s]] <- AR[t]")
I("rer", "core", "RRR", dict(S1, r=6, s=0, t=0), variant="lx106",
  note="lx106 toolchain only: RER/WER take NO operands (as rejects "
       "'rer a2, a3'); its objdump prints 'rer'/'wer' for any s,t")
I("wer", "core", "RRR", dict(S1, r=7, s=0, t=0), variant="lx106",
  note="see rer@lx106")
I("rotw", "windowed", "RRR", dict(S1, r=8, s=0), [("inc", "rotw")])
I("nsa", "nsa", "RRR", dict(S1, r=0xe), [at, as_])
I("nsau", "nsa", "RRR", dict(S1, r=0xf), [at, as_])
# --- TLB (op2=5)
T5 = dict(op0=0, op1=0, op2=5)
for r, mn, ops in [(3, "ritlb0", [at, as_]), (4, "iitlb", [as_]),
                   (5, "pitlb", [at, as_]), (6, "witlb", [at, as_]),
                   (7, "ritlb1", [at, as_]), (0xb, "rdtlb0", [at, as_]),
                   (0xc, "idtlb", [as_]), (0xd, "pdtlb", [at, as_]),
                   (0xe, "wdtlb", [at, as_]), (0xf, "rdtlb1", [at, as_])]:
    fx = dict(T5, r=r)
    if len(ops) == 1:
        fx["t"] = 0
    I(mn, "tlb", "RRR", fx, ops)
# --- RT0 (op2=6)
I("neg", "core", "RRR", dict(op0=0, op1=0, op2=6, s=0), [ar, at])
I("abs", "core", "RRR", dict(op0=0, op1=0, op2=6, s=1), [ar, at])

# --- RST1 (op1=1)
R1 = dict(op0=0, op1=1)
I("slli", "core", "RRR", dict(R1, op2_31=0), [ar, as_, ("sa", "slli_sa")],
  note="op2 = 0 or 1; op2[0] is bit 4 of (32 - sa)")
I("srai", "core", "RRR", dict(R1, op2_31=1), [ar, at, ("sa", "srai_sa")],
  note="op2 = 2 or 3; op2[0] is bit 4 of sa")
I("srli", "core", "RRR", dict(R1, op2=4), [ar, at, ("sa", "u", "s")])
I("xsr", "core", "RSR", dict(R1, op2=6), [at, ("sr", "sr")], syntax="xsr.<sr> at")
I("src", "core", "RRR", dict(R1, op2=8), [ar, as_, at])
I("srl", "core", "RRR", dict(R1, op2=9, s=0), [ar, at])
I("sll", "core", "RRR", dict(R1, op2=0xa, t=0), [ar, as_])
I("sra", "core", "RRR", dict(R1, op2=0xb, s=0), [ar, at])
I("mul16u", "mul16", "RRR", dict(R1, op2=0xc), [ar, as_, at])
I("mul16s", "mul16", "RRR", dict(R1, op2=0xd), [ar, as_, at])
IMP = dict(R1, op2=0xf)
for r, mn in [(0, "lict"), (1, "sict"), (2, "licw"), (3, "sicw"), (8, "ldct"),
              (9, "sdct")]:
    I(mn, "cache_test", "RRR", dict(IMP, r=r), [at, as_],
      note="manual only (Table 7-207 IMP, implementation specific)")
I("rfdo", "debug", "RRR", dict(IMP, r=0xe, t=0), [("imm", "u", "s")],
  note="GNU as requires an operand (s field); manual: RFDO with s=0")
I("rfdd", "debug", "RRR", dict(IMP, r=0xe, t=1, s=0),
  note="manual allows s=0,1")

# --- RST2 (op1=2)
R2 = dict(op0=0, op1=2)
for op2, mn in [(0, "andb"), (1, "andbc"), (2, "orb"), (3, "orbc"), (4, "xorb")]:
    I(mn, "boolean", "RRR", dict(R2, op2=op2), [br_, bs, bt])
I("saltu", "salt", "RRR", dict(R2, op2=6), [ar, as_, at],
  note="AR[r] <- (AR[s] <u AR[t]) ? 1 : 0; LX7; not in the 2010 manual")
I("salt", "salt", "RRR", dict(R2, op2=7), [ar, as_, at],
  note="AR[r] <- (AR[s] <s AR[t]) ? 1 : 0; LX7; not in the 2010 manual")
for op2, mn, opt in [(8, "mull", "mul32"), (0xa, "muluh", "mul32_high"),
                     (0xb, "mulsh", "mul32_high"), (0xc, "quou", "div32"),
                     (0xd, "quos", "div32"), (0xe, "remu", "div32"),
                     (0xf, "rems", "div32")]:
    I(mn, opt, "RRR", dict(R2, op2=op2), [ar, as_, at])

# --- RST3 (op1=3)
R3 = dict(op0=0, op1=3)
I("rsr", "core", "RSR", dict(R3, op2=0), [at, ("sr", "sr")], syntax="rsr.<sr> at")
I("wsr", "core", "RSR", dict(R3, op2=1), [at, ("sr", "sr")], syntax="wsr.<sr> at")
I("sext", "sext", "RRR", dict(R3, op2=2), [ar, as_, ("imm", "sext7")])
I("clamps", "clamps", "RRR", dict(R3, op2=3), [ar, as_, ("imm", "sext7")])
for op2, mn in [(4, "min"), (5, "max"), (6, "minu"), (7, "maxu")]:
    I(mn, "minmax", "RRR", dict(R3, op2=op2), [ar, as_, at])
for op2, mn in [(8, "moveqz"), (9, "movnez"), (0xa, "movltz"), (0xb, "movgez")]:
    I(mn, "core", "RRR", dict(R3, op2=op2), [ar, as_, at])
I("movf", "boolean", "RRR", dict(R3, op2=0xc), [ar, as_, bt])
I("movt", "boolean", "RRR", dict(R3, op2=0xd), [ar, as_, bt])
I("rur", "core", "RRR", dict(R3, op2=0xe), [ar, ("ur", "ur_rur")], syntax="rur.<ur> ar")
I("wur", "core", "RSR", dict(R3, op2=0xf), [at, ("ur", "ur_wur")], syntax="wur.<ur> at")

# --- EXTUI (op1 = 4,5)
I("extui", "core", "RRR", dict(op0=0, op1_31=2),
  [ar, at, ("shiftimm", "extui_sh"), ("maskimm", "extui_mk")],
  note="op1 = 4 or 5 (op1[0] = shift bit 4); AR[r] <- (AR[t] >> shift) & "
       "((1<<maskimm)-1); assembler requires shift+mask <= 32")
# op1 = 6,7: CUST0/CUST1 (designer-defined; Espressif TIE) -- see encoding.md

# --- LSCX (op1=8): FP indexed loads/stores
for op2, mn in [(0, "lsx"), (1, "lsxp"), (4, "ssx"), (5, "ssxp")]:
    I(mn, "fp", "RRR", dict(op0=0, op1=8, op2=op2), [fr, as_, at],
      note=("manual name %s; GNU name ends in 'p'" % mn.replace("p", "u"))
      if mn.endswith("p") else None)
# --- LSC4 (op1=9)
I("l32e", "windowed", "RRI4", dict(op0=0, op1=9, op2=0), [at, as_, ("imm", "l32e_off")])
I("s32e", "windowed", "RRI4", dict(op0=0, op1=9, op2=4), [at, as_, ("imm", "l32e_off")])
I("s32nb", "s32nb", "RRI4", dict(op0=0, op1=9, op2=5), [at, as_, ("imm", "u", "r", 4)],
  note="store 32-bit non-buffered, offset r*4 (0..60); not in the 2010 manual")
# --- FP0 (op1=10)
F0 = dict(op0=0, op1=0xa)
for op2, mn in [(0, "add.s"), (1, "sub.s"), (2, "mul.s"), (4, "madd.s"),
                (5, "msub.s")]:
    I(mn, "fp", "RRR", dict(F0, op2=op2), [fr, fs, ft])
for op2, mn in [(6, "maddn.s"), (7, "divn.s")]:
    I(mn, "fp_ext", "RRR", dict(F0, op2=op2), [fr, fs, ft])
for op2, mn in [(8, "round.s"), (9, "trunc.s"), (0xa, "floor.s"), (0xb, "ceil.s"),
                (0xe, "utrunc.s")]:
    I(mn, "fp", "RRR", dict(F0, op2=op2), [ar, fs, ("scale", "u", "t")])
for op2, mn in [(0xc, "float.s"), (0xd, "ufloat.s")]:
    I(mn, "fp", "RRR", dict(F0, op2=op2), [fr, as_, ("scale", "u", "t")])
FP1OP = dict(F0, op2=0xf)
I("mov.s", "fp", "RRR", dict(FP1OP, t=0), [fr, fs])
I("abs.s", "fp", "RRR", dict(FP1OP, t=1), [fr, fs])
I("const.s", "fp_ext", "RRR", dict(FP1OP, t=3), [fr, ("imm", "u", "s")])
I("rfr", "fp", "RRR", dict(FP1OP, t=4), [ar, fs])
I("wfr", "fp", "RRR", dict(FP1OP, t=5), [fr, as_])
I("neg.s", "fp", "RRR", dict(FP1OP, t=6), [fr, fs])
for t, mn in [(7, "div0.s"), (8, "recip0.s"), (9, "sqrt0.s"), (0xa, "rsqrt0.s"),
              (0xb, "nexp01.s"), (0xc, "mksadj.s"), (0xd, "mkdadj.s"),
              (0xe, "addexp.s"), (0xf, "addexpm.s")]:
    I(mn, "fp_ext", "RRR", dict(FP1OP, t=t), [fr, fs])
# --- FP1 (op1=11)
F1 = dict(op0=0, op1=0xb)
for op2, mn in [(1, "un.s"), (2, "oeq.s"), (3, "ueq.s"), (4, "olt.s"),
                (5, "ult.s"), (6, "ole.s"), (7, "ule.s")]:
    I(mn, "fp", "RRR", dict(F1, op2=op2), [br_, fs, ft])
for op2, mn in [(8, "moveqz.s"), (9, "movnez.s"), (0xa, "movltz.s"), (0xb, "movgez.s")]:
    I(mn, "fp", "RRR", dict(F1, op2=op2), [fr, fs, at])
I("movf.s", "fp", "RRR", dict(F1, op2=0xc), [fr, fs, bt])
I("movt.s", "fp", "RRR", dict(F1, op2=0xd), [fr, fs, bt])

# =========================================================== op0 = 1 (L32R)
I("l32r", "core", "RI16", dict(op0=1), [at, ("label", "l32r")])

# =========================================================== op0 = 2 (LSAI)
L2 = dict(op0=2)
for r, mn, sc in [(0, "l8ui", 1), (1, "l16ui", 2), (2, "l32i", 4), (4, "s8i", 1),
                  (5, "s16i", 2), (6, "s32i", 4), (9, "l16si", 2)]:
    I(mn, "core", "RRI8", dict(L2, r=r), [at, as_, ("imm", "u", "imm8", sc)])
I("movi", "core", "RRI8", dict(L2, r=0xa), [at, ("imm", "movi12")])
I("l32ai", "mp_sync", "RRI8", dict(L2, r=0xb), [at, as_, ("imm", "u", "imm8", 4)])
I("addi", "core", "RRI8", dict(L2, r=0xc), [at, as_, ("imm", "s", "imm8")])
I("addmi", "core", "RRI8", dict(L2, r=0xd), [at, as_, ("imm", "s", "imm8", 256)])
I("s32c1i", "s32c1i", "RRI8", dict(L2, r=0xe), [at, as_, ("imm", "u", "imm8", 4)])
I("s32ri", "mp_sync", "RRI8", dict(L2, r=0xf), [at, as_, ("imm", "u", "imm8", 4)])
# CACHE (r=7): manual only -- none of the four configs has caches in the
# Xtensa-cache-option sense
C7 = dict(L2, r=7)
for t, mn in [(0, "dpfr"), (1, "dpfw"), (2, "dpfro"), (3, "dpfwo"), (4, "dhwb"),
              (5, "dhwbi"), (6, "dhi"), (7, "dii"), (0xc, "ipf"), (0xe, "ihi"),
              (0xf, "iii")]:
    I(mn, "cache", "RRI8", dict(C7, t=t), [as_, ("imm", "u", "imm8", 4)],
      note="manual only (Table 7-217)")
for t, op1, mn, opt in [(8, 0, "dpfl", "cache_lock"), (8, 2, "dhu", "cache_lock"),
                        (8, 3, "diu", "cache_lock"), (8, 4, "diwb", "cache"),
                        (8, 5, "diwbi", "cache"), (0xd, 0, "ipfl", "cache_lock"),
                        (0xd, 2, "ihu", "cache_lock"), (0xd, 3, "iiu", "cache_lock")]:
    I(mn, opt, "RRI4", dict(C7, t=t, op1=op1), [as_, ("imm", "dce_imm")],
      note="manual only (Tables 7-218/7-219); op1 = w[19:16] sub-opcode, "
           "imm4 = w[23:20]")

# =========================================================== op0 = 3 (LSCI)
for r, mn in [(0, "lsi"), (4, "ssi"), (8, "lsip"), (0xc, "ssip")]:
    I(mn, "fp", "RRI8", dict(op0=3, r=r), [ft, as_, ("imm", "u", "imm8", 4)],
      note=("manual name %s; GNU name ends in 'p'" % mn.replace("p", "u"))
      if mn.endswith("p") else None)

# =========================================================== op0 = 4 (MAC16)
HALF = ["ll", "hl", "lh", "hh"]          # op1[1:0]: bit0 = half of 1st, bit1 = 2nd
mw, mx, my = ("mw", "mw"), ("mx", "mx"), ("my", "my")
M4 = dict(op0=4)
for h, hn in enumerate(HALF):
    # op2=0 MACID / op2=1 MACCD: MULA.DD.*.LDINC/LDDEC mw, as, mx, my
    for op2, sfx in [(0, "ldinc"), (1, "lddec")]:
        I("mula.dd.%s.%s" % (hn, sfx), "mac16", "RRR",
          dict(M4, op2=op2, op1=8 + h, r3=0, t3=0, t10=0), [mw, as_, mx, my])
    # op2=4 MACIA / op2=5 MACCA: MULA.DA.*.LDINC/LDDEC mw, as, mx, at
    for op2, sfx in [(4, "ldinc"), (5, "lddec")]:
        I("mula.da.%s.%s" % (hn, sfx), "mac16", "RRR",
          dict(M4, op2=op2, op1=8 + h, r3=0), [mw, as_, mx, at])
    for base, pfx in [(4, "mul"), (8, "mula"), (0xc, "muls")]:
        # op2=2 MACDD: mx, my
        I("%s.dd.%s" % (pfx, hn), "mac16", "RRR",
          dict(M4, op2=2, op1=base + h, s=0, r3=0, r10=0, t3=0, t10=0), [mx, my])
        # op2=3 MACAD: as, my
        I("%s.ad.%s" % (pfx, hn), "mac16", "RRR",
          dict(M4, op2=3, op1=base + h, r=0, t3=0, t10=0), [as_, my])
        # op2=6 MACDA: mx, at
        I("%s.da.%s" % (pfx, hn), "mac16", "RRR",
          dict(M4, op2=6, op1=base + h, s=0, r3=0, r10=0), [mx, at])
        # op2=7 MACAA: as, at
        I("%s.aa.%s" % (pfx, hn), "mac16", "RRR",
          dict(M4, op2=7, op1=base + h, r=0), [as_, at])
    I("umul.aa.%s" % hn, "mac16", "RRR", dict(M4, op2=7, op1=h, r=0), [as_, at])
I("ldinc", "mac16", "RRR", dict(M4, op2=8, op1=0, t=0, r32=0), [mw, as_])
I("lddec", "mac16", "RRR", dict(M4, op2=9, op1=0, t=0, r32=0), [mw, as_])

# =========================================================== op0 = 5 (CALLN)
for n, mn in enumerate(["call0", "call4", "call8", "call12"]):
    I(mn, "core" if n == 0 else "windowed", "CALL", dict(op0=5, n=n),
      [("label", "call")])

# =========================================================== op0 = 6 (SI)
I("j", "core", "CALL", dict(op0=6, n=0), [("label", "j")])
for m, mn in enumerate(["beqz", "bnez", "bltz", "bgez"]):
    I(mn, "core", "BRI12", dict(op0=6, n=1, m=m), [as_, ("label", "br12")])
for m, mn in enumerate(["beqi", "bnei", "blti", "bgei"]):
    I(mn, "core", "BRI8", dict(op0=6, n=2, m=m), [as_, ("imm", "b4const"),
                                                  ("label", "br8")])
I("entry", "windowed", "BRI12", dict(op0=6, n=3, m=0), [as_, ("imm", "entry_imm")])
I("bf", "boolean", "BRI8", dict(op0=6, n=3, m=1, r=0), [bs, ("label", "br8")])
I("bt", "boolean", "BRI8", dict(op0=6, n=3, m=1, r=1), [bs, ("label", "br8")])
for r, mn in [(8, "loop"), (9, "loopnez"), (0xa, "loopgtz")]:
    I(mn, "loop", "BRI8", dict(op0=6, n=3, m=1, r=r), [as_, ("label", "loop")])
for m, mn in [(2, "bltui"), (3, "bgeui")]:
    I(mn, "core", "BRI8", dict(op0=6, n=3, m=m), [as_, ("imm", "b4constu"),
                                                  ("label", "br8")])

# =========================================================== op0 = 7 (B)
for r, mn in [(0, "bnone"), (1, "beq"), (2, "blt"), (3, "bltu"), (4, "ball"),
              (5, "bbc"), (8, "bany"), (9, "bne"), (0xa, "bge"), (0xb, "bgeu"),
              (0xc, "bnall"), (0xd, "bbs")]:
    I(mn, "core", "RRI8", dict(op0=7, r=r), [as_, at, ("label", "br8")])
I("bbci", "core", "RRI8", dict(op0=7, r31=3), [as_, ("bit", "bbi"), ("label", "br8")],
  note="r = 6 or 7 (r[0] = bit 4 of the bit number); little-endian bit "
       "numbering (BBCI.L alias); a big-endian core numbers bits from the MSB")
I("bbsi", "core", "RRI8", dict(op0=7, r31=7), [as_, ("bit", "bbi"), ("label", "br8")],
  note="r = 14 or 15; see bbci")

# =========================================================== narrow (density)
I("l32i.n", "density", "RRRN", dict(op0=8), [at, as_, ("imm", "u", "r", 4)])
I("s32i.n", "density", "RRRN", dict(op0=9), [at, as_, ("imm", "u", "r", 4)])
I("add.n", "density", "RRRN", dict(op0=0xa), [ar, as_, at])
I("addi.n", "density", "RRRN", dict(op0=0xb), [ar, as_, ("imm", "addin4")])
I("movi.n", "density", "RI7", dict(op0=0xc, t3=0), [as_, ("imm", "movin7")])
I("beqz.n", "density", "RI6", dict(op0=0xc, t32=2), [as_, ("label", "brn6")])
I("bnez.n", "density", "RI6", dict(op0=0xc, t32=3), [as_, ("label", "brn6")])
I("mov.n", "density", "RRRN", dict(op0=0xd, r=0), [at, as_])
I("ret.n", "density", "RRRN", dict(op0=0xd, r=0xf, s=0, t=0))
I("retw.n", "density", "RRRN", dict(op0=0xd, r=0xf, s=0, t=1),
  note="also requires the Windowed Register option")
I("break.n", "density", "RRRN", dict(op0=0xd, r=0xf, t=2), [("imm", "u", "s")],
  note="also requires the Debug option; manual Table 7-240 says s=0 for S3, "
       "but BREAK.N carries imm4 in s")
I("nop.n", "density", "RRRN", dict(op0=0xd, r=0xf, s=0, t=3))
I("ill.n", "density", "RRRN", dict(op0=0xd, r=0xf, s=0, t=6))


# ----------------------------------------------------------------- formats
# Piece layouts, MSB first, as drawn in manual section 7.1 ("Formats").  The
# vertical bars of the manual separate the pieces; the big-endian layout is
# the little-endian piece list reversed (each piece keeps its internal bit
# order).  Note the t-nibble of RI7/RI6 is ONE piece (i||imm7[6:4],
# i||z||imm6[5:4]) while CALL/CALLX/BRI8/BRI12 split it into n and m.
FORMATS = {
    "RRR":   [("op2", 4), ("op1", 4), ("r", 4), ("s", 4), ("t", 4), ("op0", 4)],
    "RRI4":  [("imm4", 4), ("op1", 4), ("r", 4), ("s", 4), ("t", 4), ("op0", 4)],
    "RRI8":  [("imm8", 8), ("r", 4), ("s", 4), ("t", 4), ("op0", 4)],
    "RI16":  [("imm16", 16), ("t", 4), ("op0", 4)],
    "RSR":   [("op2", 4), ("op1", 4), ("sr", 8), ("t", 4), ("op0", 4)],
    "CALL":  [("offset", 18), ("n", 2), ("op0", 4)],
    "CALLX": [("op2", 4), ("op1", 4), ("r", 4), ("s", 4), ("m", 2), ("n", 2), ("op0", 4)],
    "BRI8":  [("imm8", 8), ("r", 4), ("s", 4), ("m", 2), ("n", 2), ("op0", 4)],
    "BRI12": [("imm12", 12), ("s", 4), ("m", 2), ("n", 2), ("op0", 4)],
    "RRRN":  [("r", 4), ("s", 4), ("t", 4), ("op0", 4)],
    "RI7":   [("imm7[3:0]", 4), ("s", 4), ("i||imm7[6:4]", 4), ("op0", 4)],
    "RI6":   [("imm6[3:0]", 4), ("s", 4), ("i||z||imm6[5:4]", 4), ("op0", 4)],
}


def layout(fmt, big_endian=False):
    """[(piece, lsb, width)] of format fmt in the instruction word read in
    the given byte order (LE: w = b0|b1<<8|b2<<16; BE: w = b0<<16|b1<<8|b2,
    16-bit: LE b0|b1<<8, BE b0<<8|b1)."""
    pieces = FORMATS[fmt]
    if big_endian:
        pieces = pieces[::-1]
    out, pos = [], sum(w for _, w in pieces)
    for name, wd in pieces:
        pos -= wd
        out.append((name, pos, wd))
    return out


def le_to_be(fmt, w):
    """Convert an LE instruction word to the BE instruction word of the same
    instruction (piece reversal); returns the BE word (read big-endian)."""
    le = {n: (w >> l) & ((1 << wd) - 1) for n, l, wd in layout(fmt)}
    out = 0
    for n, l, wd in layout(fmt, True):
        out |= le[n] << l
    return out


def be_bytes(e, w_le):
    """Bytes (lowest address first) of entry e's instruction on a BE core."""
    return le_to_be(e["fmt"], w_le).to_bytes(e["len"], "big")


# ----------------------------------------------------------------- helpers
def match(e):
    """(mask, value) of the fixed bits of entry e over its LE word."""
    mask = val = 0
    for f, v in e["fixed"].items():
        lsb, n = FIELDS[f]
        mask |= ((1 << n) - 1) << lsb
        val |= v << lsb
    return mask, val


def operand_mask(e):
    m = 0
    for op in e["ops"]:
        for f in kind_of(op).fields:
            lsb, n = FIELDS[f]
            m |= ((1 << n) - 1) << lsb
    return m


def encode(e, values, pc=0):
    """Instruction word for entry e with operand values (list, assembler
    order; pc-relative operands given as absolute targets)."""
    w = match(e)[1]
    for op, v in zip(e["ops"], values):
        w |= kind_of(op).enc(v, pc)
    return w


def to_bytes(e, w):
    return w.to_bytes(e["len"], "little")


def decode_operands(e, w, pc=0):
    return [kind_of(op).dec(w, pc) for op in e["ops"]]


def disasm(e, w, pc=0, srnames=None, urnames=None):
    """objdump-style text for word w (pc = address of the instruction)."""
    vals = decode_operands(e, w, pc)
    mn, ops = e["mn"], []
    for op, v in zip(e["ops"], vals):
        k = kind_of(op)
        if op[1] == "sr":
            mn = "%s.%s" % (mn, (srnames or {}).get(v, str(v)))
            continue
        if op[1] in ("ur_rur", "ur_wur"):
            mn = "%s.%s" % (mn, (urnames or {}).get(v, str(v)))
            continue
        ops.append(k.fmt(v))
    return mn + (" " + ", ".join(ops) if ops else "")


def lookup(w, length, model=None):
    """All entries matching word w of the given length (2 or 3)."""
    out = []
    for e in OPCODES:
        if e["len"] != length:
            continue
        if model and e["models"] is not None and model not in e["models"]:
            continue
        mask, val = match(e)
        if (w & mask) == val:
            out.append(e)
    return out


def insn_length(b0, model="esp32"):
    """Length in bytes from the first byte (LE: op0 = b0 & 15)."""
    op0 = b0 & 15
    if op0 < 8:
        return 3
    if op0 < 14:
        return 2
    return 4 if model == "esp32s3" else None   # op0 14/15: s3 = 32-bit TIE format


# models: filled in from scripts/verify.py results (do not edit by hand)
# BEGIN MODELS
MODELS = {
    'abs': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'abs.s': ['esp32', 'esp32s3'],
    'add': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'add.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'add.s': ['esp32', 'esp32s3'],
    'addexp.s': ['esp32', 'esp32s3'],
    'addexpm.s': ['esp32', 'esp32s3'],
    'addi': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'addi.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'addmi': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'addx2': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'addx4': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'addx8': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'all4': ['esp32', 'esp32s3'],
    'all8': ['esp32', 'esp32s3'],
    'and': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'andb': ['esp32', 'esp32s3'],
    'andbc': ['esp32', 'esp32s3'],
    'any4': ['esp32', 'esp32s3'],
    'any8': ['esp32', 'esp32s3'],
    'ball': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bany': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bbc': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bbci': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bbs': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bbsi': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'beq': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'beqi': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'beqz': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'beqz.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bf': ['esp32', 'esp32s3'],
    'bge': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bgei': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bgeu': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bgeui': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bgez': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'blt': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'blti': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bltu': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bltui': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bltz': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bnall': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bne': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bnei': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bnez': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bnez.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bnone': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'break': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'break.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'bt': ['esp32', 'esp32s3'],
    'call0': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'call12': ['esp32', 'esp32s2', 'esp32s3'],
    'call4': ['esp32', 'esp32s2', 'esp32s3'],
    'call8': ['esp32', 'esp32s2', 'esp32s3'],
    'callx0': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'callx12': ['esp32', 'esp32s2', 'esp32s3'],
    'callx4': ['esp32', 'esp32s2', 'esp32s3'],
    'callx8': ['esp32', 'esp32s2', 'esp32s3'],
    'ceil.s': ['esp32', 'esp32s3'],
    'clamps': ['esp32', 'esp32s2', 'esp32s3'],
    'const.s': ['esp32', 'esp32s3'],
    'dhi': [],
    'dhu': [],
    'dhwb': [],
    'dhwbi': [],
    'dii': [],
    'diu': [],
    'div0.s': ['esp32', 'esp32s3'],
    'divn.s': ['esp32', 'esp32s3'],
    'diwb': [],
    'diwbi': [],
    'dpfl': [],
    'dpfr': [],
    'dpfro': [],
    'dpfw': [],
    'dpfwo': [],
    'dsync': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'entry': ['esp32', 'esp32s2', 'esp32s3'],
    'esync': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'excw': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'extui': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'extw': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'float.s': ['esp32', 'esp32s3'],
    'floor.s': ['esp32', 'esp32s3'],
    'idtlb': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ihi': [],
    'ihu': [],
    'iii': [],
    'iitlb': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'iiu': [],
    'ill': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ill.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ipf': [],
    'ipfl': [],
    'isync': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'j': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'jx': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'l16si': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'l16ui': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'l32ai': ['esp32', 'esp32s2', 'esp32s3'],
    'l32e': ['esp32', 'esp32s2', 'esp32s3'],
    'l32i': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'l32i.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'l32r': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'l8ui': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ldct': [],
    'lddec': ['esp32', 'esp32s3'],
    'lddr32.p': ['esp32', 'esp32s2', 'esp32s3'],
    'ldinc': ['esp32', 'esp32s3'],
    'lict': [],
    'licw': [],
    'loop': ['esp32', 'esp32s3'],
    'loopgtz': ['esp32', 'esp32s3'],
    'loopnez': ['esp32', 'esp32s3'],
    'lsi': ['esp32', 'esp32s3'],
    'lsip': ['esp32', 'esp32s3'],
    'lsx': ['esp32', 'esp32s3'],
    'lsxp': ['esp32', 'esp32s3'],
    'madd.s': ['esp32', 'esp32s3'],
    'maddn.s': ['esp32', 'esp32s3'],
    'max': ['esp32', 'esp32s2', 'esp32s3'],
    'maxu': ['esp32', 'esp32s2', 'esp32s3'],
    'memw': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'min': ['esp32', 'esp32s2', 'esp32s3'],
    'minu': ['esp32', 'esp32s2', 'esp32s3'],
    'mkdadj.s': ['esp32', 'esp32s3'],
    'mksadj.s': ['esp32', 'esp32s3'],
    'mov.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'mov.s': ['esp32', 'esp32s3'],
    'moveqz': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'moveqz.s': ['esp32', 'esp32s3'],
    'movf': ['esp32', 'esp32s3'],
    'movf.s': ['esp32', 'esp32s3'],
    'movgez': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'movgez.s': ['esp32', 'esp32s3'],
    'movi': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'movi.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'movltz': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'movltz.s': ['esp32', 'esp32s3'],
    'movnez': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'movnez.s': ['esp32', 'esp32s3'],
    'movsp': ['esp32', 'esp32s2', 'esp32s3'],
    'movt': ['esp32', 'esp32s3'],
    'movt.s': ['esp32', 'esp32s3'],
    'msub.s': ['esp32', 'esp32s3'],
    'mul.aa.hh': ['esp32', 'esp32s3'],
    'mul.aa.hl': ['esp32', 'esp32s3'],
    'mul.aa.lh': ['esp32', 'esp32s3'],
    'mul.aa.ll': ['esp32', 'esp32s3'],
    'mul.ad.hh': ['esp32', 'esp32s3'],
    'mul.ad.hl': ['esp32', 'esp32s3'],
    'mul.ad.lh': ['esp32', 'esp32s3'],
    'mul.ad.ll': ['esp32', 'esp32s3'],
    'mul.da.hh': ['esp32', 'esp32s3'],
    'mul.da.hl': ['esp32', 'esp32s3'],
    'mul.da.lh': ['esp32', 'esp32s3'],
    'mul.da.ll': ['esp32', 'esp32s3'],
    'mul.dd.hh': ['esp32', 'esp32s3'],
    'mul.dd.hl': ['esp32', 'esp32s3'],
    'mul.dd.lh': ['esp32', 'esp32s3'],
    'mul.dd.ll': ['esp32', 'esp32s3'],
    'mul.s': ['esp32', 'esp32s3'],
    'mul16s': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'mul16u': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'mula.aa.hh': ['esp32', 'esp32s3'],
    'mula.aa.hl': ['esp32', 'esp32s3'],
    'mula.aa.lh': ['esp32', 'esp32s3'],
    'mula.aa.ll': ['esp32', 'esp32s3'],
    'mula.ad.hh': ['esp32', 'esp32s3'],
    'mula.ad.hl': ['esp32', 'esp32s3'],
    'mula.ad.lh': ['esp32', 'esp32s3'],
    'mula.ad.ll': ['esp32', 'esp32s3'],
    'mula.da.hh': ['esp32', 'esp32s3'],
    'mula.da.hh.lddec': ['esp32', 'esp32s3'],
    'mula.da.hh.ldinc': ['esp32', 'esp32s3'],
    'mula.da.hl': ['esp32', 'esp32s3'],
    'mula.da.hl.lddec': ['esp32', 'esp32s3'],
    'mula.da.hl.ldinc': ['esp32', 'esp32s3'],
    'mula.da.lh': ['esp32', 'esp32s3'],
    'mula.da.lh.lddec': ['esp32', 'esp32s3'],
    'mula.da.lh.ldinc': ['esp32', 'esp32s3'],
    'mula.da.ll': ['esp32', 'esp32s3'],
    'mula.da.ll.lddec': ['esp32', 'esp32s3'],
    'mula.da.ll.ldinc': ['esp32', 'esp32s3'],
    'mula.dd.hh': ['esp32', 'esp32s3'],
    'mula.dd.hh.lddec': ['esp32', 'esp32s3'],
    'mula.dd.hh.ldinc': ['esp32', 'esp32s3'],
    'mula.dd.hl': ['esp32', 'esp32s3'],
    'mula.dd.hl.lddec': ['esp32', 'esp32s3'],
    'mula.dd.hl.ldinc': ['esp32', 'esp32s3'],
    'mula.dd.lh': ['esp32', 'esp32s3'],
    'mula.dd.lh.lddec': ['esp32', 'esp32s3'],
    'mula.dd.lh.ldinc': ['esp32', 'esp32s3'],
    'mula.dd.ll': ['esp32', 'esp32s3'],
    'mula.dd.ll.lddec': ['esp32', 'esp32s3'],
    'mula.dd.ll.ldinc': ['esp32', 'esp32s3'],
    'mull': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'muls.aa.hh': ['esp32', 'esp32s3'],
    'muls.aa.hl': ['esp32', 'esp32s3'],
    'muls.aa.lh': ['esp32', 'esp32s3'],
    'muls.aa.ll': ['esp32', 'esp32s3'],
    'muls.ad.hh': ['esp32', 'esp32s3'],
    'muls.ad.hl': ['esp32', 'esp32s3'],
    'muls.ad.lh': ['esp32', 'esp32s3'],
    'muls.ad.ll': ['esp32', 'esp32s3'],
    'muls.da.hh': ['esp32', 'esp32s3'],
    'muls.da.hl': ['esp32', 'esp32s3'],
    'muls.da.lh': ['esp32', 'esp32s3'],
    'muls.da.ll': ['esp32', 'esp32s3'],
    'muls.dd.hh': ['esp32', 'esp32s3'],
    'muls.dd.hl': ['esp32', 'esp32s3'],
    'muls.dd.lh': ['esp32', 'esp32s3'],
    'muls.dd.ll': ['esp32', 'esp32s3'],
    'mulsh': ['esp32', 'esp32s2', 'esp32s3'],
    'muluh': ['esp32', 'esp32s2', 'esp32s3'],
    'neg': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'neg.s': ['esp32', 'esp32s3'],
    'nexp01.s': ['esp32', 'esp32s3'],
    'nop': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'nop.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'nsa': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'nsau': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'oeq.s': ['esp32', 'esp32s3'],
    'ole.s': ['esp32', 'esp32s3'],
    'olt.s': ['esp32', 'esp32s3'],
    'or': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'orb': ['esp32', 'esp32s3'],
    'orbc': ['esp32', 'esp32s3'],
    'pdtlb': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'pitlb': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'quos': ['esp32', 'esp32s2', 'esp32s3'],
    'quou': ['esp32', 'esp32s2', 'esp32s3'],
    'rdtlb0': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rdtlb1': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'recip0.s': ['esp32', 'esp32s3'],
    'rems': ['esp32', 'esp32s2', 'esp32s3'],
    'remu': ['esp32', 'esp32s2', 'esp32s3'],
    'rer': ['esp32', 'esp32s2', 'esp32s3'],
    'rer@lx106': ['lx106'],
    'ret': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ret.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'retw': ['esp32', 'esp32s2', 'esp32s3'],
    'retw.n': ['esp32', 'esp32s2', 'esp32s3'],
    'rfdd': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rfde': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rfdo': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rfe': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rfi': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rfme': [],
    'rfr': ['esp32', 'esp32s3'],
    'rfue': [],
    'rfwo': ['esp32', 'esp32s2', 'esp32s3'],
    'rfwu': ['esp32', 'esp32s2', 'esp32s3'],
    'ritlb0': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ritlb1': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rotw': ['esp32', 'esp32s2', 'esp32s3'],
    'round.s': ['esp32', 'esp32s3'],
    'rsil': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rsqrt0.s': ['esp32', 'esp32s3'],
    'rsr': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rsync': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'rur': ['esp32', 'esp32s2', 'esp32s3'],
    's16i': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    's32c1i': ['esp32', 'esp32s3'],
    's32e': ['esp32', 'esp32s2', 'esp32s3'],
    's32i': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    's32i.n': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    's32nb': ['esp32', 'esp32s2', 'esp32s3'],
    's32ri': ['esp32', 'esp32s2', 'esp32s3'],
    's8i': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'salt': ['esp32s2', 'esp32s3'],
    'saltu': ['esp32s2', 'esp32s3'],
    'sdct': [],
    'sddr32.p': ['esp32', 'esp32s2', 'esp32s3'],
    'sext': ['esp32', 'esp32s2', 'esp32s3'],
    'sict': [],
    'sicw': [],
    'simcall': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'sll': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'slli': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'sqrt0.s': ['esp32', 'esp32s3'],
    'sra': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'srai': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'src': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'srl': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'srli': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ssa8b': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ssa8l': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ssai': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ssi': ['esp32', 'esp32s3'],
    'ssip': ['esp32', 'esp32s3'],
    'ssl': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ssr': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'ssx': ['esp32', 'esp32s3'],
    'ssxp': ['esp32', 'esp32s3'],
    'sub': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'sub.s': ['esp32', 'esp32s3'],
    'subx2': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'subx4': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'subx8': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'syscall': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'trunc.s': ['esp32', 'esp32s3'],
    'ueq.s': ['esp32', 'esp32s3'],
    'ufloat.s': ['esp32', 'esp32s3'],
    'ule.s': ['esp32', 'esp32s3'],
    'ult.s': ['esp32', 'esp32s3'],
    'umul.aa.hh': ['esp32', 'esp32s3'],
    'umul.aa.hl': ['esp32', 'esp32s3'],
    'umul.aa.lh': ['esp32', 'esp32s3'],
    'umul.aa.ll': ['esp32', 'esp32s3'],
    'un.s': ['esp32', 'esp32s3'],
    'utrunc.s': ['esp32', 'esp32s3'],
    'waiti': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'wdtlb': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'wer': ['esp32', 'esp32s2', 'esp32s3'],
    'wer@lx106': ['lx106'],
    'wfr': ['esp32', 'esp32s3'],
    'witlb': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'wsr': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'wur': ['esp32', 'esp32s2', 'esp32s3'],
    'xor': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
    'xorb': ['esp32', 'esp32s3'],
    'xsr': ['lx106', 'esp32', 'esp32s2', 'esp32s3'],
}
# END MODELS
for _e in OPCODES:
    _e["models"] = MODELS.get(_e["key"], [])
    _e["mask"], _e["value"] = match(_e)
    _e["operands"] = [dict(name=_o[0], kind=_o[1], fields=kind_of(_o).fields,
                           desc=kind_of(_o).desc, pcrel=kind_of(_o).pcrel)
                      for _o in _e["ops"]]

# self-check: fixed bits and operand bits partition the word exactly
for _e in OPCODES:
    _m, _v = match(_e)
    _o = operand_mask(_e)
    _full = (1 << (8 * _e["len"])) - 1
    assert _m & _o == 0, (_e["mn"], hex(_m), hex(_o))
    assert _m | _o == _full, (_e["mn"], hex(_m | _o))
