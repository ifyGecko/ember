#!/usr/bin/env python3
"""Floating-point test vectors for the PIC32 self-test (MIPS32r2 FPU, legacy NaNs).

Each vector is (op, rm, a, b, c, result, cause): the instruction `op` on operands a, b
(c: the addend of MADD/MSUB/NMADD/NMSUB) with FCSR.RM = rm and no exceptions enabled,
its result bits and FCSR.Cause (bits V Z O U I = 16 8 4 2 1). The expected values come
from exact rational arithmetic and IEEE 754 rounding written out here, independent of
the emulator and of any host's floating point: tininess before rounding, every NaN
result the default NaN, a signalling NaN (legacy: fraction MSB set) an invalid
operation, and an invalid conversion to an integer 2^31-1 (2^63-1).

usage: gen_fpvectors.py OUT.h
"""
import random, sys
from fractions import Fraction
from math import isqrt

S = dict(p=24, emin=-126, emax=127, ebits=8, w=32, dnan=0x7FBFFFFF)
D = dict(p=53, emin=-1022, emax=1023, ebits=11, w=64, dnan=0x7FF7FFFFFFFFFFFF)
I, U, O, Z, V = 1, 2, 4, 8, 16
RN, RZ, RP, RM = 0, 1, 2, 3

def fields(f, x):
    fb = f["p"] - 1
    s = x >> (f["w"] - 1)
    e = (x >> fb) & ((1 << f["ebits"]) - 1)
    m = x & ((1 << fb) - 1)
    return s, e, m

def classify(f, x):
    s, e, m = fields(f, x)
    top = (1 << f["ebits"]) - 1
    if e == top:
        if m == 0:
            return ("inf", s, None)
        return ("snan" if m >> (f["p"] - 2) else "qnan", s, None)
    fb = f["p"] - 1
    if e == 0:
        return ("num", s, Fraction(m, 1) * Fraction(2) ** (f["emin"] - fb))
    return ("num", s, Fraction(m | (1 << fb), 1) * Fraction(2) ** (e - 127 if f is S else e - 1023) / (1 << fb))

def value(f, x):
    k, s, v = classify(f, x)
    return (-v if s else v) if k == "num" else None

def encode(f, s, e, m):
    """e: unbiased exponent of a normal (or emin for subnormal with m < 2^(p-1))."""
    fb = f["p"] - 1
    if m < (1 << fb):
        be = 0
    else:
        be = e + (127 if f is S else 1023)
        m &= (1 << fb) - 1
    return (s << (f["w"] - 1)) | (be << fb) | m

def inf(f, s):
    return (s << (f["w"] - 1)) | (((1 << f["ebits"]) - 1) << (f["p"] - 1))

def maxfin(f, s):
    return inf(f, s) - 1

def round_up(rm, s, rem, half_cmp, lsb):
    """Round away from zero? rem: nonzero remainder; half_cmp: rem vs 1/2 (-1,0,1)."""
    if not rem:
        return False
    if rm == RN:
        return half_cmp > 0 or (half_cmp == 0 and lsb)
    if rm == RZ:
        return False
    if rm == RP:
        return not s
    return bool(s)

def rnd(f, x, rm, zero_sign=0):
    """Round the exact rational x; return (bits, flags)."""
    if x == 0:
        return (zero_sign << (f["w"] - 1)), 0
    s = 1 if x < 0 else 0
    a = -x if s else x
    p, emin, emax = f["p"], f["emin"], f["emax"]
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    while Fraction(2) ** (e + 1) <= a:
        e += 1
    tiny = e < emin
    eq = max(e, emin)
    q = a / Fraction(2) ** (eq - (p - 1))
    m = q.numerator // q.denominator
    rem = q - m
    half = (rem > Fraction(1, 2)) - (rem < Fraction(1, 2))
    if round_up(rm, s, rem != 0, half, m & 1):
        m += 1
        if m == 1 << p:
            m >>= 1
            eq += 1
    flags = I if rem else 0
    if tiny and rem:
        flags |= U
    if eq > emax:
        flags |= O | I
        big = rm == RN or (rm == RP and not s) or (rm == RM and s)
        return (inf(f, s) if big else maxfin(f, s)), flags
    return encode(f, s, eq, m), flags

def is_nan(f, x):
    return classify(f, x)[0] in ("qnan", "snan")

def is_snan(f, x):
    return classify(f, x)[0] == "snan"

def arith(f, op, a, b, rm):
    if is_nan(f, a) or (op != "sqrt" and is_nan(f, b)):
        return f["dnan"], (V if is_snan(f, a) or (op != "sqrt" and is_snan(f, b)) else 0)
    ka, sa, va = classify(f, a)
    kb, sb, vb = classify(f, b) if op != "sqrt" else ("num", 0, Fraction(0))
    A = None if ka == "inf" else (-va if sa else va)
    B = None if kb == "inf" else (-vb if sb else vb)
    if op in ("add", "sub"):
        if op == "sub":
            sb ^= 1
            B = None if B is None else -B
        if ka == "inf" or kb == "inf":
            if ka == "inf" and kb == "inf" and sa != sb:
                return f["dnan"], V
            return inf(f, sa if ka == "inf" else sb), 0
        r = A + B
        if r == 0:
            zs = (sa & sb) if (A == 0 and B == 0 and rm != RM) else (1 if rm == RM else 0)
            if A == 0 and B == 0:
                zs = (sa & sb) if rm != RM else (sa | sb)
            return rnd(f, r, rm, zs)
        return rnd(f, r, rm)
    if op == "mul":
        s = sa ^ sb
        if ka == "inf" or kb == "inf":
            if (ka == "inf" and B == 0) or (kb == "inf" and A == 0):
                return f["dnan"], V
            return inf(f, s), 0
        return rnd(f, A * B, rm, s)
    if op == "div":
        s = sa ^ sb
        if ka == "inf":
            return (f["dnan"], V) if kb == "inf" else (inf(f, s), 0)
        if kb == "inf":
            return s << (f["w"] - 1), 0
        if B == 0:
            return (f["dnan"], V) if A == 0 else (inf(f, s), Z)
        return rnd(f, A / B, rm, s)
    if op == "sqrt":
        if ka == "inf":
            return (f["dnan"], V) if sa else (a, 0)
        if A == 0:
            return a, 0
        if A < 0:
            return f["dnan"], V
        # floor(sqrt(A) * 2^K) for a K that leaves many bits below the rounding point;
        # an inexact root is replaced by a value strictly between r and r + 1 (sticky)
        K = 1200
        N = A * Fraction(4) ** K
        n = N.numerator // N.denominator
        r = isqrt(n)
        exact = N.denominator == 1 and r * r == n
        approx = (Fraction(r) if exact else Fraction(2 * r + 1, 2)) / Fraction(2) ** K
        return rnd(f, approx, rm)
    raise ValueError(op)

def tofmt(fdst, fsrc, x, rm):
    if is_nan(fsrc, x):
        return fdst["dnan"], (V if is_snan(fsrc, x) else 0)
    k, s, v = classify(fsrc, x)
    if k == "inf":
        return inf(fdst, s), 0
    return rnd(fdst, -v if s else v, rm, s)

def toint(f, x, rm, bits):
    lim = 1 << (bits - 1)
    dflt = lim - 1
    if is_nan(f, x) or classify(f, x)[0] == "inf":
        return dflt, V
    v = value(f, x)
    n = v.numerator // v.denominator
    rem = v - n
    if rem:
        half = (rem > Fraction(1, 2)) - (rem < Fraction(1, 2))
        if rm == RN:
            up = half > 0 or (half == 0 and n & 1)
        elif rm == RZ:
            up = v < 0      # floor went below zero: truncate toward zero
        elif rm == RP:
            up = True
        else:
            up = False
        if up:
            n += 1
    if n >= lim or n < -lim:
        return dflt, V
    return n & ((1 << bits) - 1), (I if rem else 0)

def fromint(f, n, bits, rm):
    if n >> (bits - 1):
        n -= 1 << bits
    return rnd(f, Fraction(n), rm)

def compare(f, cond, a, b):
    un = is_nan(f, a) or is_nan(f, b)
    x = V if (is_snan(f, a) or is_snan(f, b) or (un and cond & 8)) else 0
    if un:
        r = cond & 1
    else:
        A = value(f, a) if classify(f, a)[0] == "num" else (Fraction(10) ** 400 * (-1 if fields(f, a)[0] else 1))
        B = value(f, b) if classify(f, b)[0] == "num" else (Fraction(10) ** 400 * (-1 if fields(f, b)[0] else 1))
        r = (A == B and cond & 2) or (A < B and cond & 4)
    return (1 if r else 0), x

def madd(f, kind, a, b, c, rm):
    p, x1 = arith(f, "mul", a, b, rm)
    r, x2 = arith(f, "sub" if kind in ("msub", "nmsub") else "add", p, c, rm)
    if kind in ("nmadd", "nmsub") and not is_nan(f, r):
        r ^= 1 << (f["w"] - 1)
    return r, x1 | x2

# ---- operands -------------------------------------------------------------------
def specials(f):
    w = f["w"]
    sign = 1 << (w - 1)
    one = encode(f, 0, 0, 1 << (f["p"] - 1))
    l = [0, sign, one, one | sign, 1, sign | 1, (1 << (f["p"] - 1)) - 1,
         encode(f, 0, f["emin"], 1 << (f["p"] - 1)), maxfin(f, 0), maxfin(f, 1), inf(f, 0), inf(f, 1),
         f["dnan"], f["dnan"] | (1 << (f["p"] - 2)) | 1,           # quiet (default) NaN, a signalling NaN
         encode(f, 0, 1, 3 << (f["p"] - 2)), encode(f, 1, -1, (1 << f["p"]) - 1)]
    return l

random.seed(7)
def rand(f):
    w = f["w"]
    k = random.random()
    if k < 0.15:
        return random.choice(specials(f))
    if k < 0.3:   # near the subnormal boundary
        return encode(f, random.getrandbits(1), f["emin"] + random.randint(0, 2),
                      (1 << (f["p"] - 1)) | random.getrandbits(f["p"] - 1)) if random.random() < 0.5 \
            else (random.getrandbits(1) << (w - 1)) | random.getrandbits(f["p"] - 1)
    e = random.randint(-30, 30)
    return encode(f, random.getrandbits(1), e, (1 << (f["p"] - 1)) | random.getrandbits(f["p"] - 1))

OPS = ["add.s", "sub.s", "mul.s", "div.s", "sqrt.s", "add.d", "sub.d", "mul.d", "div.d", "sqrt.d",
       "cvt.s.d", "cvt.d.s", "cvt.w.d", "round.w.d", "trunc.w.d", "ceil.w.d", "floor.w.d", "cvt.w.s",
       "cvt.l.d", "cvt.d.w", "cvt.s.w", "cvt.d.l", "cvt.s.l", "c.olt.d", "c.ueq.d", "c.lt.d", "c.ngle.s",
       "madd.d", "nmsub.s", "abs.d", "neg.s"]

def gen(op, rm):
    fmt = S if op.endswith(".s") and not op.startswith(("cvt.s.", "cvt.d.s")) else D
    if op in ("cvt.s.d", "cvt.w.d", "round.w.d", "trunc.w.d", "ceil.w.d", "floor.w.d", "cvt.l.d"):
        a = rand(D); b = c = 0
        if op == "cvt.s.d":
            r, x = tofmt(S, D, a, rm)
        elif op == "cvt.l.d":
            r, x = toint(D, a, rm, 64)
        else:
            m = {"cvt": rm, "round": RN, "trunc": RZ, "ceil": RP, "floor": RM}[op.split(".")[0]]
            r, x = toint(D, a, m, 32)
        return a, b, c, r, x
    if op == "cvt.d.s":
        a = rand(S); r, x = tofmt(D, S, a, rm); return a, 0, 0, r, x
    if op == "cvt.w.s":
        a = rand(S); r, x = toint(S, a, rm, 32); return a, 0, 0, r, x
    if op in ("cvt.d.w", "cvt.s.w"):
        n = random.getrandbits(32) if random.random() < 0.7 else random.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 16777217])
        r, x = fromint(D if op == "cvt.d.w" else S, n, 32, rm); return n, 0, 0, r, x
    if op in ("cvt.d.l", "cvt.s.l"):
        n = random.getrandbits(64) if random.random() < 0.7 else random.choice([0, 1, 1 << 63, (1 << 64) - 1, (1 << 53) + 1])
        r, x = fromint(D if op == "cvt.d.l" else S, n, 64, rm); return n, 0, 0, r, x
    a, b, c = rand(fmt), rand(fmt), rand(fmt)
    if op.startswith("c."):
        cond = {"olt": 4, "ueq": 3, "lt": 12, "ngle": 9}[op.split(".")[1]]
        r, x = compare(fmt, cond, a, b); return a, b, 0, r, x
    if op.startswith(("madd", "nmsub")):
        r, x = madd(fmt, op.split(".")[0], a, b, c, rm); return a, b, c, r, x
    if op.startswith(("abs", "neg")):
        if is_nan(fmt, a):
            return a, 0, 0, fmt["dnan"], (V if is_snan(fmt, a) else 0)
        return a, 0, 0, (a & ~(1 << (fmt["w"] - 1))) if op.startswith("abs") else a ^ (1 << (fmt["w"] - 1)), 0
    r, x = arith(fmt, op.split(".")[0], a, b, rm)
    return a, b, c, r, x

def main():
    out = sys.argv[1]
    rows = []
    for k, op in enumerate(OPS):
        for rm in range(4):
            for _ in range(20):
                rows.append((k, rm) + gen(op, rm))
    with open(out, "w") as fp:
        fp.write("/* generated by gen_fpvectors.py: do not edit */\n")
        fp.write("enum {" + ", ".join("FP_" + o.replace(".", "_").upper() for o in OPS) + "};\n")
        fp.write("static const struct fpvec { uint8_t op, rm, cause; uint64_t a, b, c, r; } FPV[] = {\n")
        for (op, rm, a, b, c, r, x) in rows:
            fp.write("    {%d, %d, 0x%02x, 0x%xull, 0x%xull, 0x%xull, 0x%xull},\n" % (op, rm, x, a, b, c, r))
        fp.write("};\n")

if __name__ == "__main__":
    main()
