#!/usr/bin/env python3
"""Generate xtensa_fpu_vectors.h (BUILD/gen/, made by the Makefile), the test vectors of isa/xtensa/xtensa_fpu.c.

Expected results come from exact rational arithmetic (fractions.Fraction), not from the
host's floating point: every arithmetic result is the exact value rounded once with the
instruction's rounding mode, with the IEEE flags (tininess detected before rounding,
U only when also inexact) and the default NaN 0x7FC00000 for every arithmetic NaN.

The divide and square-root step instructions have no documented bit patterns; they
follow the encoding described at the top of xtensa_fpu.c. Each step instruction gets
vectors of its own, and libgcc's __divsf3, __ieee754_sqrtf, __recipsf2 and __rsqrtsf2
run as instruction sequences: the quotients and roots are checked here against the
correctly rounded result before they are written, the reciprocals and reciprocal square
roots (fast-math approximations) are whatever the model gives.

Each table entry runs one instruction: the vectors cover every instruction in all four
rounding modes over zeros of both signs, subnormals, normals, the largest values,
infinities and NaNs, ties, overflow and underflow, cancellation, and the conversions at
their range limits with every scale factor.

    python3 -I -B tests/isa-xtensa/gen-fpu-vectors.py > build/gen/xtensa_fpu_vectors.h
"""
import math
import os
import random
import sys
from fractions import Fraction as F

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import opcodes  # noqa: E402

SIGN, DNAN, PINF, FMAX, ONE = 0x80000000, 0x7FC00000, 0x7F800000, 0x7F7FFFFF, 0x3F800000
RNE, RZ, RUP, RDN = 0, 1, 2, 3
FLAG = {"I": 1 << 7, "U": 1 << 8, "O": 1 << 9, "Z": 1 << 10, "V": 1 << 11}
ENC = {e["key"]: e["value"] for e in opcodes.OPCODES if not e.get("variant")}


def nan(b): return (b & 0x7FFFFFFF) > PINF
def inf(b): return (b & 0x7FFFFFFF) == PINF
def zero(b): return (b & 0x7FFFFFFF) == 0
def neg(b): return b >> 31


def value(b):
    e, m = (b >> 23) & 0xFF, b & 0x7FFFFF
    assert e != 255
    v = F(m, 1 << 23) * F(2) ** -126 if e == 0 else F((1 << 23) | m, 1 << 23) * F(2) ** (e - 127)
    return -v if neg(b) else v


def round_f32(v, rm, fl):
    """v != 0 rounded to binary32."""
    assert v != 0
    s = 1 if v < 0 else 0
    a = -v if s else v
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if F(2) ** e > a:
        e -= 1
    q = F(2) ** (max(e, -126) - 23)
    n = a / q
    i = n.numerator // n.denominator
    rem = n - i
    if rem:
        fl.add("I")
        if e < -126:
            fl.add("U")
        if rm == RNE:
            i += rem > F(1, 2) or (rem == F(1, 2) and i & 1)
        elif rm == RUP:
            i += not s
        elif rm == RDN:
            i += s
    ee = max(e, -126)
    if i == 1 << 24:
        i >>= 1
        ee += 1
    if ee > 127:
        fl.update("OI")
        big = rm == RNE or (rm == RUP and not s) or (rm == RDN and s)
        return (s << 31) | (PINF if big else FMAX)
    if i < 1 << 23:
        return (s << 31) | i
    return (s << 31) | ((ee + 127) << 23) | (i & 0x7FFFFF)


def exact(v):
    fl = set()
    b = round_f32(v, RNE, fl)
    assert not fl
    return b


def zero_sum(sa, sb, rm):
    """sign of an exact zero sum of terms with signs sa, sb (both exact zeros: their sign)"""
    return SIGN if (sa if sa == sb else rm == RDN) else 0


# ---- the instructions ----------------------------------------------------------------
def f_add(a, b, rm, fl):
    if nan(a) or nan(b):
        return DNAN
    if inf(a) and inf(b) and neg(a) != neg(b):
        fl.add("V")
        return DNAN
    if inf(a):
        return a
    if inf(b):
        return b
    v = value(a) + value(b)
    if v == 0:
        if zero(a) and zero(b):
            return zero_sum(neg(a), neg(b), rm)
        return zero_sum(0, 1, rm)
    return round_f32(v, rm, fl)


def f_mul(a, b, rm, fl):
    s = (a ^ b) & SIGN
    if nan(a) or nan(b):
        return DNAN
    if inf(a) or inf(b):
        if zero(a) or zero(b):
            fl.add("V")
            return DNAN
        return s | PINF
    v = value(a) * value(b)
    return s if v == 0 else round_f32(v, rm, fl)


def f_fma(r, s, t, flip, rm, fl):
    pn = neg(s) ^ neg(t) ^ flip
    if nan(r) or nan(s) or nan(t):
        return DNAN
    if (inf(s) and zero(t)) or (zero(s) and inf(t)):
        fl.add("V")
        return DNAN
    if inf(s) or inf(t):
        if inf(r) and neg(r) != pn:
            fl.add("V")
            return DNAN
        return (pn << 31) | PINF
    if inf(r):
        return r
    p = value(s) * value(t) * (-1 if flip else 1)
    v = value(r) + p
    if v == 0:
        if p == 0 and zero(r):
            return zero_sum(pn, neg(r), rm)
        return zero_sum(0, 1, rm)
    return round_f32(v, rm, fl)


def key(b):
    return -(b & 0x7FFFFFFF) if neg(b) else b


def f_cmp(op, a, b, fl):
    un = nan(a) or nan(b)
    if op in ("olt.s", "ole.s", "ult.s", "ule.s") and un:
        fl.add("V")
    if op == "un.s":
        return int(un)
    if un:
        return int(op[0] == "u")
    ka, kb = key(a), key(b)
    return int({"eq": ka == kb, "lt": ka < kb, "le": ka <= kb}[op[1:3]])


def f_from_int(a, t, uns, rm, fl):
    v = a if uns or not a >> 31 else a - (1 << 32)
    return 0 if v == 0 else round_f32(F(v) / 2 ** t, rm, fl)


def f_to_int(x, t, mode, uns, fl):
    s = neg(x)
    pmax = 0xFFFFFFFF if uns else 0x7FFFFFFF
    if nan(x) or inf(x):
        fl.add("V")
        return pmax if nan(x) or not s else SIGN
    if zero(x):
        return 0
    v = abs(value(x)) * 2 ** t
    n = v.numerator // v.denominator
    rem = v - n
    if rem:
        if mode == RNE:
            n += rem > F(1, 2) or (rem == F(1, 2) and n & 1)
        elif mode == RUP:
            n += not s
        elif mode == RDN:
            n += s
    if uns and s:
        if n:
            fl.add("V")
            return SIGN
    elif n > (0x80000000 if s else pmax):
        fl.add("V")
        return SIGN if s else pmax
    if rem:
        fl.add("I")
    return (-n) & 0xFFFFFFFF if s else n


TOINT = {"trunc.s": RZ, "round.s": RNE, "floor.s": RDN, "ceil.s": RUP, "utrunc.s": RZ}


# divide / square-root steps (the encoding of xtensa_fpu.c)
def norm(b):
    """finite non-zero -> (sign, m in [1,2), e)"""
    v = abs(value(b))
    e = v.numerator.bit_length() - v.denominator.bit_length()
    if F(2) ** e > v:
        e -= 1
    return neg(b), v / F(2) ** e, e


def split(b):
    """-> (sign, X in [1,4), E even)"""
    s, m, e = norm(b)
    k = e & 1
    return s, m * 2 ** k, e - k


def trunc8(x):
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if F(2) ** e > x:
        e -= 1
    q = F(2) ** (e - 7)
    return (x / q).__floor__() * q


def rsqrt8(X):
    """1/sqrt(X) truncated to 8 bits, X in [1,4): the largest q/256 with (q/256)^2 X <= 1"""
    return F(math.isqrt((F(1 << 16) / X).__floor__()), 256)


def f_const(imm): return {1: ONE, 2: 0x40000000, 3: 0x3F000000}.get(imm, 0)


def f_nexp01(x):
    if nan(x):
        return DNAN
    if inf(x) or zero(x):
        return x ^ SIGN
    s, X, _ = split(x)
    return exact(X if s else -X)


def f_div0(x):
    if nan(x) or inf(x) or zero(x):
        return ONE
    s, X, _ = split(x)
    y = trunc8(1 / X)
    return exact(-y if s else y)


def f_sqrt0(x):
    if nan(x) or inf(x) or zero(x) or neg(x):
        return ONE
    return exact(rsqrt8(split(x)[1]))


def f_recip0(x, fl):
    if nan(x):
        return DNAN
    if zero(x):
        fl.add("Z")
        return (x & SIGN) | PINF
    if inf(x):
        return x & SIGN
    s, m, e = norm(x)
    y = trunc8(1 / m) * F(2) ** -e
    if y >= F(2) ** 128:
        return (x & SIGN) | PINF
    return round_f32(-y if s else y, RZ, set())


def f_rsqrt0(x, fl):
    if nan(x):
        return DNAN
    if zero(x):
        fl.add("Z")
        return (x & SIGN) | PINF
    if neg(x):
        fl.add("V")
        return DNAN
    if inf(x):
        return 0
    _, X, E = split(x)
    return exact(rsqrt8(X) * F(2) ** (-E // 2))


def adj(d):
    d = max(-186, min(186, d))
    h = 1 if d < 0 else 0
    return (((d + 128 * h + 127) & 0xFF) << 23) | ((d & 0xFF) << 14)


def adj_special(code, s): return 0x8 | (code << 1) | s
def adj_is_special(a): return (a & 0xFFFFFFF8) == 0x8


def adj_value(a):
    code, s = (a >> 1) & 3, a & 1
    return DNAN if code >= 2 else (s << 31) | (PINF if code == 1 else 0)


def f_mkdadj(b, a, fl):
    sg = neg(a ^ b)
    if nan(a) or nan(b):
        return adj_special(2, 0)
    if (inf(a) and inf(b)) or (zero(a) and zero(b)):
        fl.add("V")
        return adj_special(2, 0)
    if inf(a):
        return adj_special(1, sg)
    if zero(b):
        fl.add("Z")
        return adj_special(1, sg)
    if zero(a) or inf(b):
        return adj_special(0, sg)
    allones = norm(b)[1] == 2 - F(1, 1 << 23)
    return adj(split(a)[2] - split(b)[2]) | allones


def f_mksadj(x, fl):
    if nan(x):
        return adj_special(2, 0)
    if zero(x):
        return adj_special(0, neg(x))
    if neg(x):
        fl.add("V")
        return adj_special(2, 0)
    if inf(x):
        return adj_special(1, 0)
    return adj(split(x)[2] // 2)


def f_addexp(r, s):
    if adj_is_special(s):
        return 63 << 23
    e = ((r >> 23) + ((s >> 23) & 0xFF) - 127) & 0xFF
    return ((r ^ s) & SIGN) | (e << 23) | (1 if s & 1 else r & 0x7FFFFF)


def f_addexpm(r, s):
    if adj_is_special(s):
        return adj_value(s)
    e = ((r >> 23) + (s >> 14)) & 0xFF
    return ((r ^ (s << 9)) & SIGN) | (e << 23) | (r & 0x7FFFFF)


def f_divn(r, s, t, rm, fl):
    fq, fy = (r >> 23) & 0xFF, (t >> 23) & 0xFF
    diff = (fy - fq) & 0xFF
    if diff <= 4 or diff >= 252:
        rel, lo, hi = (diff if diff <= 4 else diff - 256), -65, 190
    elif 124 <= diff <= 132:
        rel, lo, hi = diff - 128, -190, 65
    else:
        return r
    eq = fq - 127
    while eq < lo:
        eq += 256
    while eq > hi:
        eq -= 256
    if nan(s):
        return DNAN
    if inf(s):
        return ((s ^ t) & SIGN) | PINF
    q = F((1 << 23) | (r & 0x7FFFFF), 1 << 23) * F(2) ** eq * (-1 if neg(r) else 1)
    y = F((1 << 23) | (t & 0x7FFFFF), 1 << 23) * F(2) ** (eq + rel) * (-1 if neg(t) else 1)
    v = q + value(s) * y
    return zero_sum(0, 1, rm) if v == 0 else round_f32(v, rm, fl)


def exec_fp(op, fr, fs, ft, s, t, rm, fl):
    """FR result of a register-to-register op"""
    if op == "add.s": return f_add(fs, ft, rm, fl)
    if op == "sub.s": return f_add(fs, ft ^ SIGN, rm, fl)
    if op == "mul.s": return f_mul(fs, ft, rm, fl)
    if op == "madd.s": return f_fma(fr, fs, ft, 0, rm, fl)
    if op == "msub.s": return f_fma(fr, fs, ft, 1, rm, fl)
    if op == "maddn.s": return f_fma(fr, fs, ft, 0, RNE, set())
    if op == "const.s": return f_const(s)
    if op == "nexp01.s": return f_nexp01(fs)
    if op == "div0.s": return f_div0(fs)
    if op == "sqrt0.s": return f_sqrt0(fs)
    if op == "recip0.s": return f_recip0(fs, fl)
    if op == "rsqrt0.s": return f_rsqrt0(fs, fl)
    if op == "mkdadj.s": return f_mkdadj(fr, fs, fl)
    if op == "mksadj.s": return f_mksadj(fs, fl)
    if op == "addexp.s": return f_addexp(fr, fs)
    if op == "addexpm.s": return f_addexpm(fr, fs)
    if op == "divn.s": return f_divn(fr, fs, ft, rm, fl)
    if op == "mov.s": return fs
    if op == "neg.s": return fs ^ SIGN
    raise KeyError(op)


def flagbits(fl): return sum(FLAG[c] for c in fl)


def enc(op, r, s, t): return ENC[op] | (r << 12) | (s << 8) | (t << 4)


def cname(op): return "XTO_" + op.upper().replace(".", "_")


# ---- single-instruction vectors -----------------------------------------------------
rng = random.Random(20261104)
VECS = []
DIVN_SEEN = []
READS_FR = {"madd.s", "msub.s", "maddn.s", "divn.s", "mkdadj.s", "addexp.s", "addexpm.s"}
UNARY = {"nexp01.s", "div0.s", "sqrt0.s", "recip0.s", "rsqrt0.s", "mksadj.s", "const.s"}
# t is part of the opcode: FR[t] is not an operand
FIXED_T = UNARY | {"mkdadj.s", "addexp.s", "addexpm.s", "mov.s", "neg.s"}


def fcr_for(rm):
    return rm | (rng.getrandbits(32) & ~3 if rng.random() < 0.2 else 0)


def fsr_in():
    return rng.getrandbits(32) if rng.random() < 0.2 else 0


def regs3(alias_ok=True):
    if alias_ok and rng.random() < 0.08:
        r = rng.randrange(16)
        return r, rng.choice([r, rng.randrange(16)]), rng.choice([r, rng.randrange(16)])
    return tuple(rng.sample(range(16), 3))


def vec_fp(op, a, b=0, c=0, rm=None, imm=0):
    """FR-to-FR op: a = FR[r] before, b = FR[s] (CONST.S: imm), c = FR[t]"""
    for m in range(4) if rm is None else [rm]:
        r, s, t = regs3()
        if op == "const.s":
            s = imm
        if op in FIXED_T:
            t = 0
        f = {r: a}
        if op != "const.s":
            f[s] = b
        if op not in FIXED_T:
            f[t] = c
        fs = f[s] if op != "const.s" else 0
        ft = f[t] if op not in FIXED_T else 0
        fl = set()
        out = exec_fp(op, f[r], fs, ft, s, t, m, fl)
        fi = fsr_in()
        VECS.append((cname(op), enc(op, r, s, t), fcr_for(m), fi, f[r], fs, ft, out,
                     fi | flagbits(fl)))


def vec_cmp(op, a, b):
    s, t = rng.choice([(x, y) for x in range(16) for y in range(16) if x != y])
    r = rng.randrange(16)
    if rng.random() < 0.05:
        t = s
        b = a
    br = rng.getrandbits(32)
    fl = set()
    res = f_cmp(op, a, b, fl)
    out = (br & ~(1 << r)) | (res << r)
    fi = fsr_in()
    m = rng.randrange(4)
    VECS.append((cname(op), enc(op, r, s, t), fcr_for(m), fi, br, a, b, out, fi | flagbits(fl)))


def vec_toint(op, x, t):
    r, s = rng.randrange(16), rng.randrange(16)
    fl = set()
    out = f_to_int(x, t, TOINT[op], op == "utrunc.s", fl)
    fi = fsr_in()
    m = rng.randrange(4)
    VECS.append((cname(op), enc(op, r, s, t), fcr_for(m), fi, rng.getrandbits(32), x, 0, out,
                 fi | flagbits(fl)))


def vec_fromint(op, a, t):
    for m in range(4):
        r, s = rng.randrange(16), rng.randrange(16)
        fl = set()
        out = f_from_int(a, t, op == "ufloat.s", m, fl)
        fi = fsr_in()
        VECS.append((cname(op), enc(op, r, s, t), fcr_for(m), fi, rng.getrandbits(32), a, 0,
                     out, fi | flagbits(fl)))


SPECIAL = [0, SIGN, 1, 0x80000001, 0x00000003, 0x007FFFFF, 0x807FFFFF, 0x00400000, 0x00800000,
           0x80800000, 0x00800001, 0x00FFFFFF, 0x3F800000, 0xBF800000, 0x3F800001, 0x3F7FFFFF,
           0x40000000, 0x3F000000, 0x3FC00000, 0xBFC00000, 0x40400000, 0x33800000, 0x34000000,
           0x4B000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F000000, 0x7E800000, 0x01000000, 0x0C000000,
           0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFFFFFFFF, 0x7FBFFFFF]


def rand_f32():
    k = rng.random()
    if k < 0.15:   # subnormal or tiny, or huge
        return rng.getrandbits(32) & 0x807FFFFF | (rng.choice([0, 0, 1, 2, 3, 252, 253, 254]) << 23)
    if k < 0.25:
        return rng.choice(SPECIAL)
    if k < 0.6:    # moderate exponents, so operands interact
        return rng.getrandbits(32) & 0x807FFFFF | (rng.randrange(100, 155) << 23)
    return rng.getrandbits(32)


def finite_nz():
    while True:
        b = rand_f32()
        if not (nan(b) or inf(b) or zero(b)):
            return b


def pairs(n):
    out = [(rng.choice(SPECIAL), rng.choice(SPECIAL)) for _ in range(n // 3)]
    out += [(rng.choice(SPECIAL), rand_f32()) for _ in range(n // 6)]
    out += [(rand_f32(), rng.choice(SPECIAL)) for _ in range(n // 6)]
    out += [(rand_f32(), rand_f32()) for _ in range(n // 3)]
    return out


def gen_arith():
    # crafted: ties, cancellation, signed zeros, overflow, underflow (tininess before rounding)
    add = [(ONE, 0x33800000), (ONE, 0x33C00000), (ONE, 0xB3800000), (ONE, 0xB3000000),
           (0x3F800001, 0xBF800000), (ONE, 0xBF800000), (0x40490FDB, 0xC0490FDB), (0, SIGN),
           (SIGN, SIGN), (SIGN, 0), (0, 0), (FMAX, FMAX), (0xFF7FFFFF, 0xFF7FFFFF),
           (FMAX, 0x73000000), (FMAX, 0x72FFFFFF), (1, 0x80000001), (0x00800000, 0x80000001),
           (0x007FFFFF, 1), (PINF, 0xFF800000), (PINF, PINF), (PINF, FMAX), (DNAN, PINF),
           (0x7F800001, 0), (0x4B800000, 0x3F800000), (0x4B800000, 0xBF800000),
           (0x4B7FFFFF, 0x3F000000), (0x4B7FFFFF, 0xBF000000)]
    mul = [(0x3F7FFFFF, 0x00800000), (0x3F7FFFFE, 0x00800000), (0x3F000000, 0x00800001),
           (0x3F000000, 0x00800003), (0x3F800001, 0x00800000), (0x3F7FFFFF, 0x80800000),
           (0x34000000, 0x00800000), (0x33800000, 0x00000001), (0x3F000000, 1),
           (0x3EFFFFFF, 1), (0x3F000001, 1), (0x7F000000, 0x40000000), (0x7F000000, 0x3FFFFFFF),
           (0xFF000000, 0x40000000), (PINF, 0), (SIGN, PINF), (PINF, 0xBF800000), (SIGN, 0),
           (SIGN, 0x3F800000), (0x3F800001, 0x3F800001), (0x3FFFFFFF, 0x3FFFFFFF),
           (0x5F800000, 0x5F800000), (0x1F800000, 0x1F800000), (0x1F800000, 0x9F800000)]
    for op, crafted in (("add.s", add), ("sub.s", add), ("mul.s", mul)):
        crafted = crafted + [(b, a) for a, b in crafted]
        if op == "sub.s":   # the same edges: a - (-b)
            crafted = [(a, b ^ SIGN) for a, b in crafted]
        for a, b in crafted + pairs(300):
            vec_fp(op, rng.getrandbits(32), a, b)
    # fused: exact error terms, signed zeros, 0*inf + NaN, inf - inf
    fma = [(0, 0, 0), (0, SIGN, ONE), (SIGN, SIGN, ONE), (SIGN, 0, ONE), (ONE, 0x33800000, ONE),
           (ONE, 0x33800001, ONE), (ONE, 0xB3800000, ONE), (DNAN, PINF, 0), (0, PINF, 0),
           (0xFF800000, PINF, ONE), (PINF, PINF, ONE), (PINF, ONE, ONE), (ONE, PINF, 0x7F800001),
           (FMAX, FMAX, ONE), (0xFF7FFFFF, FMAX, 0x40000000), (1, 0x3F000000, 0x80000001),
           (0x80800000, 0x3F7FFFFF, 0x00800000), (0, 0x3F7FFFFF, 0x00800000),
           (0x00000001, 0x3F000000, 0x80000001), (SIGN, 0x00000001, 0x00000001),
           (0, 0x00000001, 0x80000001), (0x7F000000, 0x7F000000, 0xBF800000)]
    for _ in range(60):     # r = -round(s*t): the product's rounding error
        s, t = finite_nz(), finite_nz()
        p = f_mul(s, t, RNE, set())
        if not (nan(p) or inf(p)):
            fma.append((p ^ SIGN, s, t))
            fma.append((p, s ^ SIGN, t))
    trip = fma + [(rng.choice(SPECIAL), rand_f32(), rand_f32()) for _ in range(80)]
    trip += [(rand_f32(), rand_f32(), rand_f32()) for _ in range(200)]
    trip += [(rand_f32(), rng.choice(SPECIAL), rng.choice(SPECIAL)) for _ in range(60)]
    for op in ("madd.s", "msub.s", "maddn.s"):
        for r, s, t in trip:
            if op == "msub.s":
                s ^= SIGN
            vec_fp(op, r, s, t, rm=None if op != "maddn.s" else rng.randrange(4))


def gen_cmp():
    base = [(rng.choice(SPECIAL), rng.choice(SPECIAL)) for _ in range(60)]
    base += [(0, SIGN), (SIGN, 0), (ONE, ONE), (ONE, 0x3F800001), (0x3F800001, ONE),
             (0xBF800000, 0xBF800001), (PINF, PINF), (0xFF800000, PINF), (DNAN, DNAN),
             (DNAN, ONE), (ONE, 0xFFC00000), (1, 0x80000001), (0x80000001, 0x80000002)]
    base += [(x, x) for x in rng.sample(SPECIAL, 8)]
    base += [(rand_f32(), rand_f32()) for _ in range(60)]
    for op in ("oeq.s", "olt.s", "ole.s", "ueq.s", "ult.s", "ule.s", "un.s"):
        for a, b in base:
            vec_cmp(op, a, b)


def gen_conv():
    targets = [F(2) ** 31 - 1, F(2) ** 31 - F(1, 2), F(2) ** 31, F(2) ** 31 + 128, F(2) ** 32 - 128,
               F(2) ** 32 - F(1, 2), F(2) ** 32, F(2) ** 33, -F(2) ** 31, -F(2) ** 31 - F(1, 2),
               -F(2) ** 31 - 1, -F(2) ** 31 - 256, -F(2) ** 31 + F(1, 2), F(1, 2), F(3, 2), F(5, 2),
               F(-1, 2), F(-3, 2), F(-5, 2), F(1, 4), F(3, 4), F(-3, 4), F(1), F(-1), F(247, 2),
               F(-247, 2), F(1, 1 << 30), F(-1, 1 << 30), F(1 << 24) + 1]
    xs = set()
    for t in range(16):
        for v in targets:
            b = round_f32(v / 2 ** t, RNE, set())
            for d in (-1, 0, 1):
                xs.add(((b + d) & 0xFFFFFFFF, t))
    xs = sorted(xs)
    xs += [(x, rng.randrange(16)) for x in SPECIAL]
    xs += [(rand_f32(), rng.randrange(16)) for _ in range(60)]
    for op in TOINT:
        for x, t in rng.sample(xs, 260):
            vec_toint(op, x, t)
        for x in (0, SIGN, PINF, 0xFF800000, DNAN, 0xFFFFFFFF, 1, 0x80000001):
            vec_toint(op, x, rng.randrange(16))
    ints = [0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0x00FFFFFF, 0x01000000, 0x01000001,
            0x01000002, 0x01000003, 0xFEFFFFFF, 0xFF000001, 0x7FFFFFC0, 0x7FFFFFBF, 0xFFFFFF80,
            0x00000003, 0x0000FFFF]
    for op in ("float.s", "ufloat.s"):
        for a in ints:
            vec_fromint(op, a, rng.randrange(16))
        for t in range(16):
            vec_fromint(op, rng.choice(ints), t)
            vec_fromint(op, rng.getrandbits(32), t)


def gen_steps():
    pool = SPECIAL + [rand_f32() for _ in range(70)] + [0xC0800000, 0x40800000, 0x40E00000]
    for op in ("nexp01.s", "div0.s", "sqrt0.s", "recip0.s", "rsqrt0.s", "mksadj.s"):
        for x in pool:
            vec_fp(op, rng.getrandbits(32), x, rm=rng.randrange(4))
    for imm in range(16):
        vec_fp("const.s", rng.getrandbits(32), imm=imm, rm=rng.randrange(4))
    for a, b in pairs(150) + [(ONE, 1), (1, 0x7F7FFFFF), (0x7F7FFFFF, 1), (0, 0), (PINF, 0)]:
        vec_fp("mkdadj.s", a, b, rm=rng.randrange(4))
    adjs = [adj(d) for d in (-186, -100, -64, -1, 0, 1, 2, 63, 64, 186)]
    adjs += [adj_special(c, s) for c in range(3) for s in range(2)]
    for op in ("addexp.s", "addexpm.s"):
        for _ in range(60):
            s = rng.choice(adjs + [rand_f32()])
            vec_fp(op, rand_f32(), s, rm=rng.randrange(4))
    # DIVN.S with operands the sequences produce, plus window misses, specials, NaN/inf fs
    for r, s, t in rng.sample(DIVN_SEEN, 200):
        vec_fp("divn.s", r, s, t)
    for _ in range(60):
        vec_fp("divn.s", rng.getrandbits(32), rng.choice(SPECIAL + [rand_f32()]),
               rng.getrandbits(32), rm=rng.randrange(4))


# ---- libgcc sequences ----------------------------------------------------------------
PROGS = {
    "div": """div0.s 3 2; nexp01.s 4 2; const.s 5 1; maddn.s 5 4 3; mov.s 6 3; mov.s 7 2;
              nexp01.s 2 1; maddn.s 6 5 6; const.s 5 1; const.s 0 0; neg.s 8 2;
              maddn.s 5 4 6; maddn.s 0 8 3; mkdadj.s 7 1; maddn.s 6 5 6; maddn.s 8 4 0;
              const.s 3 1; maddn.s 3 4 6; maddn.s 0 8 6; neg.s 2 2; maddn.s 6 3 6;
              maddn.s 2 4 0; addexpm.s 0 7; addexp.s 6 7; divn.s 0 2 6""",
    "sqrt": """sqrt0.s 2 1; const.s 3 0; maddn.s 3 2 2; nexp01.s 4 1; const.s 0 3;
               addexp.s 4 0; maddn.s 0 3 4; nexp01.s 3 1; neg.s 5 3; maddn.s 2 0 2;
               const.s 0 0; const.s 6 0; const.s 7 0; maddn.s 0 5 2; maddn.s 6 2 4;
               const.s 4 3; maddn.s 7 4 2; maddn.s 3 0 0; maddn.s 4 6 2; neg.s 2 7;
               maddn.s 0 3 2; maddn.s 7 4 7; mksadj.s 2 1; nexp01.s 1 1; maddn.s 1 0 0;
               neg.s 3 7; addexpm.s 0 2; addexp.s 3 2; divn.s 0 1 3""",
    "recip": """recip0.s 0 1; const.s 2 1; msub.s 2 1 0; maddn.s 0 0 2; const.s 2 1;
                msub.s 2 1 0; maddn.s 0 0 2""",
    "rsqrt": """rsqrt0.s 0 1; mul.s 2 1 0; const.s 3 3; mul.s 4 3 0; const.s 5 1;
                msub.s 5 2 0; maddn.s 0 4 5; mul.s 2 1 0; mul.s 1 3 0; const.s 3 1;
                msub.s 3 2 0; maddn.s 0 1 3""",
}


def parse(prog):
    steps = []
    for ins in prog.split(";"):
        w = ins.split()
        r, s, t = ([int(x) for x in w[1:]] + [0, 0])[:3]
        steps.append((w[0], r, s, 0 if w[0] in FIXED_T else t))
    return steps


STEPS = {k: parse(v) for k, v in PROGS.items()}


def run(name, a, b, rm):
    f = [0] * 16
    f[1], f[2] = a, b
    fl = set()
    for op, r, s, t in STEPS[name]:
        if op == "divn.s":
            DIVN_SEEN.append((f[r], f[s], f[t]))
        f[r] = exec_fp(op, f[r], f[s], f[t], s, t, rm, fl)
    return f[0], fl


def ref_div(a, b, rm, fl):
    if nan(a) or nan(b):
        return DNAN
    sg = (a ^ b) & SIGN
    if (inf(a) and inf(b)) or (zero(a) and zero(b)):
        fl.add("V")
        return DNAN
    if inf(a):
        return sg | PINF
    if zero(b):
        fl.add("Z")
        return sg | PINF
    if zero(a) or inf(b):
        return sg
    return round_f32(value(a) / value(b), rm, fl)


def ref_sqrt(x, rm, fl):
    if nan(x):
        return DNAN
    if zero(x):
        return x
    if neg(x):
        fl.add("V")
        return DNAN
    if inf(x):
        return x
    v = value(x)
    n, p = v.numerator, v.denominator.bit_length() - 1
    m = max(0, 130 - n.bit_length())
    m += (p + m) & 1
    k = n << m
    r = math.isqrt(k)
    approx = F(2 * r + (0 if r * r == k else 1), 2 << ((p + m) // 2))
    return round_f32(approx, rm, fl)


SEQ = []
STATS = {"div_flags_differ": 0, "sqrt_flags_differ": 0, "recip_max_err": F(0), "rsqrt_max_err": F(0)}


def gen_seq():
    divs = pairs(330)
    for _ in range(110):   # hard cases: a = b*q +- 1 ulp
        b = finite_nz()
        q = rng.getrandbits(32) & 0x3FFFFFFF | 0x30000000
        a = (f_mul(b, q, rng.randrange(4), set()) + rng.choice([-1, 0, 1])) & 0xFFFFFFFF
        if nan(a) or inf(a):
            a = ONE
        divs.append((a, b))
    divs += [(ONE, 0x40400000), (0x00000001, 0x7F7FFFFF), (0x7F7FFFFF, 0x00000001), (1, 0x40000000),
             (0x00800000, 0x40000000), (0x00800000, 0x3F800001), (0x00FFFFFF, 0x40000000),
             (0x7F7FFFFF, 0x3F7FFFFF), (0x7F7FFFFF, 0x3F800000), (0, 0), (PINF, PINF), (ONE, 0),
             (SIGN, ONE), (ONE, 0xFF800000), (PINF, 0x80000001)]
    sqrts = [rand_f32() & (0x7FFFFFFF if rng.random() < 0.9 else 0xFFFFFFFF) for _ in range(260)]
    sqrts += [0, SIGN, 1, 2, 3, 0x007FFFFF, 0x00800000, ONE, 0x40000000, 0x40800000, FMAX, PINF,
              0xFF800000, DNAN, 0xBF800000, 0x80000001, 0x3F800001, 0x3F7FFFFF]
    for name, ins in (("div", divs), ("sqrt", [(x, 0) for x in sqrts])):
        for a, b in ins:
            for rm in range(4):
                got, fl = run(name, a, b, rm)
                rfl = set()
                want = (ref_div(a, b, rm, rfl) if name == "div" else ref_sqrt(a, rm, rfl))
                if got != want:
                    sys.exit("gen-fpu-vectors: %s(%08x, %08x) rm %d: model %08x, correctly "
                             "rounded %08x" % (name, a, b, rm, got, want))
                if fl != rfl:
                    STATS[name + "_flags_differ"] += 1
                SEQ.append((name, rm, a, b, got, flagbits(fl)))
    for name in ("recip", "rsqrt"):
        xs = [rand_f32() for _ in range(90)] + [ONE, 0x40000000, 0x40400000, 0x00800000, 1, FMAX,
                                                0x7E800000, 0, SIGN, PINF, 0xFF800000, DNAN,
                                                0xBF800000, 0x3F7FFFFF]
        for x in xs:
            for rm in range(4):
                got, fl = run(name, x, 0, rm)
                if not (nan(x) or inf(x) or zero(x) or nan(got) or inf(got) or zero(got)
                        or (name == "rsqrt" and neg(x))):
                    v = value(x)
                    ex = 1 / v if name == "recip" else None
                    if ex is not None:
                        err = abs(value(got) - ex) / abs(ex)
                    else:
                        err = abs(value(got) ** 2 * v - 1) / 2
                    k = name + "_max_err"
                    STATS[k] = max(STATS[k], err)
                SEQ.append((name, rm, x, 0, got, flagbits(fl)))


# ---- output --------------------------------------------------------------------------
def main():
    gen_seq()        # first: DIVN.S vectors reuse its operands
    gen_arith()
    gen_cmp()
    gen_conv()
    gen_steps()
    o = []
    w = o.append
    w("/* xtensa_fpu_vectors.h - Test vectors for isa/xtensa/xtensa_fpu.c (test_xtensa_fpu.c).")
    w(" * Generated by tests/isa-xtensa/gen-fpu-vectors.py: do not edit. Exact rational results,")
    w(" * see the generator for the conventions. */")
    w("#ifndef XTENSA_FPU_VECTORS_H")
    w("#define XTENSA_FPU_VECTORS_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("/* One instruction. in[0]: FR[r] (BR for a compare, AR[r] for a conversion to an")
    w(" * integer), in[1]: FR[s] (AR[s] for FLOAT.S/UFLOAT.S), in[2]: FR[t]; out: FR[r], BR,")
    w(" * or AR[r]. The registers are written in that order (they may alias). */")
    w("typedef struct xt_fpu_vec {")
    w("    uint16_t id;")
    w("    uint32_t insn, fcr, fsr_in, in[3], out, fsr_out;")
    w("} xt_fpu_vec;")
    w("")
    w("static const xt_fpu_vec xt_fpu_vecs[] = {")
    for v in VECS:
        w("    { %s, 0x%06x, 0x%x, 0x%x, { 0x%x, 0x%x, 0x%x }, 0x%x, 0x%x }," % v)
    w("};")
    w("")
    w("/* libgcc's sequences: inputs in f1 (and f2), the result in f0, FSR starts at 0. */")
    w("typedef struct xt_fpu_step {")
    w("    uint16_t id;")
    w("    uint32_t insn;")
    w("} xt_fpu_step;")
    w("")
    for name, steps in STEPS.items():
        w("static const xt_fpu_step xt_fpu_prog_%s[] = {" % name)
        for op, r, s, t in steps:
            ops = [r] + ([] if op == "const.s" else [s]) + ([] if op in FIXED_T else [t])
            w("    { %s, 0x%06x },   /* %s %s */" % (
                cname(op), enc(op, r, s, t), op,
                ", ".join("f%d" % x for x in ops) + (", %d" % s if op == "const.s" else "")))
        w("};")
    w("")
    w("enum { XT_FPU_DIV, XT_FPU_SQRT, XT_FPU_RECIP, XT_FPU_RSQRT };")
    w("")
    w("static const struct { const xt_fpu_step *steps; unsigned n; const char *name; } xt_fpu_progs[] = {")
    for name in STEPS:
        w("    { xt_fpu_prog_%s, sizeof xt_fpu_prog_%s / sizeof xt_fpu_prog_%s[0], \"%s\" }," % (
            name, name, name, name))
    w("};")
    w("")
    w("typedef struct xt_fpu_seqvec {")
    w("    uint8_t  prog, rm;")
    w("    uint32_t a, b, out, fsr_out;")
    w("} xt_fpu_seqvec;")
    w("")
    w("static const xt_fpu_seqvec xt_fpu_seqvecs[] = {")
    pn = {"div": "XT_FPU_DIV", "sqrt": "XT_FPU_SQRT", "recip": "XT_FPU_RECIP", "rsqrt": "XT_FPU_RSQRT"}
    for name, rm, a, b, out, fl in SEQ:
        w("    { %s, %d, 0x%x, 0x%x, 0x%x, 0x%x }," % (pn[name], rm, a, b, out, fl))
    w("};")
    w("")
    w("#endif /* XTENSA_FPU_VECTORS_H */")
    sys.stdout.write("\n".join(o) + "\n")
    sys.stderr.write("gen-fpu-vectors: %d instruction vectors, %d sequence runs; div/sqrt flags "
                     "differing from IEEE: %d/%d; recip max rel err %.3g, rsqrt %.3g\n" % (
                         len(VECS), len(SEQ), STATS["div_flags_differ"], STATS["sqrt_flags_differ"],
                         float(STATS["recip_max_err"]), float(STATS["rsqrt_max_err"])))


if __name__ == "__main__":
    main()
