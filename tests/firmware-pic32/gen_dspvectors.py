#!/usr/bin/env python3
"""DSP ASE test vectors for the PIC32 self-test (mz: DSP revisions 1 and 2).

Each vector is an instruction word and the machine state before and after it: GPRs
$4, $5, $6 (the instructions use rs = $4, rt = $5, rd = $6 where they name registers),
accumulator ac1 and DSPControl. The expected state comes from the model below, written
from the DSP ASE's definitions with Python integers: lanes as lists, saturation and
rounding spelled out, independent of the emulator's C.

usage: gen_dspvectors.py OUT.h
"""
import random, sys

M32 = 0xFFFFFFFF
random.seed(11)

def s(v, n):
    v &= (1 << n) - 1
    return v - (1 << n) if v >> (n - 1) else v

def lanes(v, n, signed):          # n-bit lanes, low first
    return [s(v >> (n * i), n) if signed else (v >> (n * i)) & ((1 << n) - 1) for i in range(32 // n)]

def pack(l, n):
    return sum((x & ((1 << n) - 1)) << (n * i) for i, x in enumerate(l)) & M32

class St:
    def __init__(self, a, b, c, acc, ctl):
        self.a, self.b, self.c, self.acc, self.ctl = a, b, c, acc, ctl
    def flag(self, bit):
        self.ctl |= 1 << bit

def clamp(st, v, lo, hi, bit):
    if v < lo or v > hi:
        st.flag(bit)
        return lo if v < lo else hi
    return v

def q15mul(st, x, y, bit):
    if x == -32768 and y == -32768:
        st.flag(bit)
        return 0x7FFFFFFF
    return x * y * 2

def word(fn, op, rs=4, rt=5, rd=6):
    return (0x1F << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (op << 6) | fn

# ---- the instructions: (name, fn, op, encoder(), semantics(st, w)) --------------------
OPS = []
def op(name, fn, code, enc=None):
    def deco(f):
        OPS.append((name, fn, code, enc or (lambda: word(fn, code)), f))
        return f
    return deco

def lanewise(n, signed, f):
    def g(st, w):
        A, B = lanes(st.a, n, signed), lanes(st.b, n, signed)
        st.c = pack([f(st, x, y) for x, y in zip(A, B)], n)
    return g

def addsub_lane(n, signed, sub, satur, bit=20):
    lo, hi = (-(1 << (n - 1)), (1 << (n - 1)) - 1) if signed else (0, (1 << n) - 1)
    def f(st, x, y):
        v = x - y if sub else x + y
        if satur:
            return clamp(st, v, lo, hi, bit)
        if v < lo or v > hi:
            st.flag(bit)
        return v
    return f

for nm, code, n, sg, sub, sat_ in [("addu.qb", 0, 8, 0, 0, 0), ("subu.qb", 1, 8, 0, 1, 0), ("addu_s.qb", 4, 8, 0, 0, 1),
        ("subu_s.qb", 5, 8, 0, 1, 1), ("addu.ph", 8, 16, 0, 0, 0), ("subu.ph", 9, 16, 0, 1, 0),
        ("addq.ph", 10, 16, 1, 0, 0), ("subq.ph", 11, 16, 1, 1, 0), ("addu_s.ph", 12, 16, 0, 0, 1),
        ("subu_s.ph", 13, 16, 0, 1, 1), ("addq_s.ph", 14, 16, 1, 0, 1), ("subq_s.ph", 15, 16, 1, 1, 1)]:
    op(nm, 0x10, code)(lanewise(n, sg, addsub_lane(n, sg, sub, sat_)))

@op("addq_s.w", 0x10, 22)
def _(st, w): st.c = clamp(st, s(st.a, 32) + s(st.b, 32), -2**31, 2**31 - 1, 20) & M32
@op("subq_s.w", 0x10, 23)
def _(st, w): st.c = clamp(st, s(st.a, 32) - s(st.b, 32), -2**31, 2**31 - 1, 20) & M32
@op("addsc", 0x10, 16)
def _(st, w):
    v = st.a + st.b
    st.ctl = (st.ctl & ~(1 << 13)) | ((v >> 32) << 13)
    st.c = v & M32
@op("addwc", 0x10, 17)
def _(st, w):
    v = s(st.a, 32) + s(st.b, 32) + ((st.ctl >> 13) & 1)
    if not -2**31 <= v < 2**31:
        st.flag(20)
    st.c = v & M32
@op("modsub", 0x10, 18)
def _(st, w): st.c = ((st.b >> 8) & 0xFFFF) if st.a == 0 else (st.a - (st.b & 0xFF)) & M32
@op("raddu.w.qb", 0x10, 20, lambda: word(0x10, 20, rt=0))
def _(st, w): st.c = sum(lanes(st.a, 8, 0))
for nm, code, hi in [("muleu_s.ph.qbl", 6, 3), ("muleu_s.ph.qbr", 7, 1)]:
    def mk(hi):
        def f(st, w):
            A, B = lanes(st.a, 8, 0), lanes(st.b, 16, 0)
            st.c = pack([clamp(st, A[hi - 1] * B[0], 0, 0xFFFF, 21), clamp(st, A[hi] * B[1], 0, 0xFFFF, 21)], 16)
        return f
    op(nm, 0x10, code)(mk(hi))
for nm, code, i in [("muleq_s.w.phl", 28, 1), ("muleq_s.w.phr", 29, 0)]:
    def mk(i):
        def f(st, w): st.c = q15mul(st, lanes(st.a, 16, 1)[i], lanes(st.b, 16, 1)[i], 21) & M32
        return f
    op(nm, 0x10, code)(mk(i))
for nm, code, rnd in [("mulq_rs.ph", 31, 1), ("mulq_s.ph", 30, 0)]:
    def mk(rnd):
        def f(st, x, y):
            p = q15mul(st, x, y, 21)
            return 0x7FFF if p == 0x7FFFFFFF else ((p + 0x8000) >> 16 if rnd else p >> 16)
        return lanewise(16, 1, f)
    op(nm, 0x10, code)(mk(rnd))

# halving (rev 2)
for nm, code, n, sg, sub, rnd in [("adduh.qb", 0, 8, 0, 0, 0), ("subuh.qb", 1, 8, 0, 1, 0), ("adduh_r.qb", 2, 8, 0, 0, 1),
        ("subuh_r.qb", 3, 8, 0, 1, 1), ("addqh.ph", 8, 16, 1, 0, 0), ("subqh.ph", 9, 16, 1, 1, 0),
        ("addqh_r.ph", 10, 16, 1, 0, 1), ("subqh_r.ph", 11, 16, 1, 1, 1), ("addqh.w", 16, 32, 1, 0, 0),
        ("subqh.w", 17, 32, 1, 1, 0), ("addqh_r.w", 18, 32, 1, 0, 1), ("subqh_r.w", 19, 32, 1, 1, 1)]:
    def mk(sub, rnd):
        return lambda st, x, y: ((x - y if sub else x + y) + rnd) >> 1
    op(nm, 0x18, code)(lanewise(n, sg, mk(sub, rnd)))
@op("mul.ph", 0x18, 12)
def _(st, w):
    def f(st, x, y):
        p = x * y
        if not -32768 <= p <= 32767:
            st.flag(21)
        return p
    lanewise(16, 1, f)(st, w)
@op("mul_s.ph", 0x18, 14)
def _(st, w): lanewise(16, 1, lambda st, x, y: clamp(st, x * y, -32768, 32767, 21))(st, w)
for nm, code, rnd in [("mulq_s.w", 22, 0), ("mulq_rs.w", 23, 1)]:
    def mk(rnd):
        def f(st, w):
            x, y = s(st.a, 32), s(st.b, 32)
            if x == y == -2**31:
                st.flag(21)
                st.c = 0x7FFFFFFF
            else:
                st.c = ((x * y * 2 + (2**31 if rnd else 0)) >> 32) & M32
        return f
    op(nm, 0x18, code)(mk(rnd))

# compares and picks
def cmpq(c, x, y): return [x == y, x < y, x <= y][c]
for nm, code, c in [("cmpu.eq.qb", 0, 0), ("cmpu.lt.qb", 1, 1), ("cmpu.le.qb", 2, 2)]:
    def mk(c):
        def f(st, w):
            r = sum(cmpq(c, x, y) << i for i, (x, y) in enumerate(zip(lanes(st.a, 8, 0), lanes(st.b, 8, 0))))
            st.ctl = (st.ctl & ~(0xF << 24)) | (r << 24)
        return f
    op(nm, 0x11, code, lambda code=code: word(0x11, code, rd=0))(mk(c))
for nm, code, c, both in [("cmpgu.eq.qb", 4, 0, 0), ("cmpgu.lt.qb", 5, 1, 0), ("cmpgu.le.qb", 6, 2, 0),
                          ("cmpgdu.eq.qb", 24, 0, 1), ("cmpgdu.lt.qb", 25, 1, 1), ("cmpgdu.le.qb", 26, 2, 1)]:
    def mk(c, both):
        def f(st, w):
            r = sum(cmpq(c, x, y) << i for i, (x, y) in enumerate(zip(lanes(st.a, 8, 0), lanes(st.b, 8, 0))))
            st.c = r
            if both:
                st.ctl = (st.ctl & ~(0xF << 24)) | (r << 24)
        return f
    op(nm, 0x11, code)(mk(c, both))
for nm, code, c in [("cmp.eq.ph", 8, 0), ("cmp.lt.ph", 9, 1), ("cmp.le.ph", 10, 2)]:
    def mk(c):
        def f(st, w):
            r = sum(cmpq(c, x, y) << i for i, (x, y) in enumerate(zip(lanes(st.a, 16, 1), lanes(st.b, 16, 1))))
            st.ctl = (st.ctl & ~(3 << 24)) | (r << 24)
        return f
    op(nm, 0x11, code, lambda code=code: word(0x11, code, rd=0))(mk(c))
@op("pick.qb", 0x11, 3)
def _(st, w):
    A, B = lanes(st.a, 8, 0), lanes(st.b, 8, 0)
    st.c = pack([A[i] if (st.ctl >> (24 + i)) & 1 else B[i] for i in range(4)], 8)
@op("pick.ph", 0x11, 11)
def _(st, w):
    A, B = lanes(st.a, 16, 0), lanes(st.b, 16, 0)
    st.c = pack([A[i] if (st.ctl >> (24 + i)) & 1 else B[i] for i in range(2)], 16)
@op("precrq.qb.ph", 0x11, 12)
def _(st, w): st.c = pack([st.b >> 8, st.b >> 24, st.a >> 8, st.a >> 24], 8)
@op("precr.qb.ph", 0x11, 13)
def _(st, w): st.c = pack([st.b, st.b >> 16, st.a, st.a >> 16], 8)
@op("packrl.ph", 0x11, 14)
def _(st, w): st.c = ((st.a & 0xFFFF) << 16) | (st.b >> 16)
@op("precrqu_s.qb.ph", 0x11, 15)
def _(st, w):
    def u8(h):
        if h < 0: st.flag(22); return 0
        if h > 0x7F80: st.flag(22); return 0xFF
        return (h >> 7) & 0xFF
    A, B = lanes(st.a, 16, 1), lanes(st.b, 16, 1)
    st.c = pack([u8(B[0]), u8(B[1]), u8(A[0]), u8(A[1])], 8)
@op("precrq.ph.w", 0x11, 20)
def _(st, w): st.c = (st.a & 0xFFFF0000) | (st.b >> 16)
@op("precrq_rs.ph.w", 0x11, 21)
def _(st, w):
    def r(x):
        v = s(x, 32) + 0x8000
        if v > 2**31 - 1: st.flag(22); return 0x7FFF
        return (v >> 16) & 0xFFFF
    st.c = (r(st.a) << 16) | r(st.b)
def sra_enc(code):
    return lambda: word(0x11, code, rd=random.randrange(32))
for nm, code, rnd in [("precr_sra.ph.w", 30, 0), ("precr_sra_r.ph.w", 31, 1)]:
    def mk(rnd):
        def f(st, w):
            sa = (w >> 11) & 31
            def sh(x):
                x = s(x, 32)
                if rnd and sa:
                    x += 1 << (sa - 1)
                return (x >> sa) & 0xFFFF
            st.b = (sh(st.b) << 16) | sh(st.a)
        return f
    op(nm, 0x11, code, sra_enc(code))(mk(rnd))

# unary (rt -> rd)
def r2(code): return lambda: word(0x12, code, rs=0)
@op("absq_s.qb", 0x12, 1, r2(1))
def _(st, w): st.c = pack([clamp(st, abs(x), 0, 127, 20) for x in lanes(st.b, 8, 1)], 8)
@op("absq_s.ph", 0x12, 9, r2(9))
def _(st, w): st.c = pack([clamp(st, abs(x), 0, 32767, 20) for x in lanes(st.b, 16, 1)], 16)
@op("absq_s.w", 0x12, 17, r2(17))
def _(st, w): st.c = clamp(st, abs(s(st.b, 32)), 0, 2**31 - 1, 20)
def repl_qb(): return (0x1F << 26) | (random.getrandbits(8) << 16) | (6 << 11) | (2 << 6) | 0x12
def repl_ph(): return (0x1F << 26) | (random.getrandbits(10) << 16) | (6 << 11) | (10 << 6) | 0x12
@op("repl.qb", 0x12, 2, repl_qb)
def _(st, w): st.c = ((w >> 16) & 0xFF) * 0x01010101
@op("repl.ph", 0x12, 10, repl_ph)
def _(st, w): st.c = (s(w >> 16, 10) & 0xFFFF) * 0x00010001
@op("replv.qb", 0x12, 3, r2(3))
def _(st, w): st.c = (st.b & 0xFF) * 0x01010101
@op("replv.ph", 0x12, 11, r2(11))
def _(st, w): st.c = (st.b & 0xFFFF) * 0x00010001
@op("preceq.w.phl", 0x12, 12, r2(12))
def _(st, w): st.c = st.b & 0xFFFF0000
@op("preceq.w.phr", 0x12, 13, r2(13))
def _(st, w): st.c = (st.b << 16) & M32
for nm, code, hi, lo, shift in [("precequ.ph.qbl", 4, 3, 2, 7), ("precequ.ph.qbr", 5, 1, 0, 7),
        ("precequ.ph.qbla", 6, 3, 1, 7), ("precequ.ph.qbra", 7, 2, 0, 7), ("preceu.ph.qbl", 28, 3, 2, 0),
        ("preceu.ph.qbr", 29, 1, 0, 0), ("preceu.ph.qbla", 30, 3, 1, 0), ("preceu.ph.qbra", 31, 2, 0, 0)]:
    def mk(hi, lo, shift):
        def f(st, w):
            B = lanes(st.b, 8, 0)
            st.c = pack([B[lo] << shift, B[hi] << shift], 16)
        return f
    op(nm, 0x12, code, r2(code))(mk(hi, lo, shift))
@op("bitrev", 0x12, 27, r2(27))
def _(st, w): st.c = int("{:016b}".format(st.b & 0xFFFF)[::-1], 2)

# shifts
def sh_imm(code, bits): return lambda: word(0x13, code, rs=random.getrandbits(bits))
def shl(st, x, sa, n, satur):
    v = x << sa
    lo, hi = -(1 << (n - 1)), (1 << (n - 1)) - 1
    if not lo <= v <= hi:
        st.flag(22)
        if satur:
            return lo if x < 0 else hi
    return v
def sra(x, sa, rnd):
    if rnd and sa:
        x += 1 << (sa - 1)
    return x >> sa
for nm, code, n, kind, var in [
        ("shll.qb", 0, 8, "lu", 0), ("shllv.qb", 2, 8, "lu", 1), ("shrl.qb", 1, 8, "r", 0), ("shrlv.qb", 3, 8, "r", 1),
        ("shra.qb", 4, 8, "a", 0), ("shra_r.qb", 5, 8, "ar", 0), ("shrav.qb", 6, 8, "a", 1), ("shrav_r.qb", 7, 8, "ar", 1),
        ("shll.ph", 8, 16, "l", 0), ("shllv.ph", 10, 16, "l", 1), ("shll_s.ph", 12, 16, "ls", 0), ("shllv_s.ph", 14, 16, "ls", 1),
        ("shra.ph", 9, 16, "a", 0), ("shrav.ph", 11, 16, "a", 1), ("shra_r.ph", 13, 16, "ar", 0), ("shrav_r.ph", 15, 16, "ar", 1),
        ("shrl.ph", 25, 16, "r", 0), ("shrlv.ph", 27, 16, "r", 1), ("shll_s.w", 20, 32, "ls", 0), ("shllv_s.w", 22, 32, "ls", 1),
        ("shra_r.w", 21, 32, "ar", 0), ("shrav_r.w", 23, 32, "ar", 1)]:
    def mk(n, kind, var):
        mbits = {8: 3, 16: 4, 32: 5}[n]
        def f(st, w):
            sa = (st.a if var else (w >> 21)) & ((1 << mbits) - 1)
            signed = kind not in ("lu", "r")
            out = []
            for x in lanes(st.b, n, signed):
                if kind == "lu":
                    v = x << sa
                    if v >> n:
                        st.flag(22)
                    out.append(v)
                elif kind == "r":
                    out.append(x >> sa)
                elif kind in ("a", "ar"):
                    out.append(sra(x, sa, kind == "ar"))
                else:
                    out.append(shl(st, x, sa, n, kind == "ls"))
            st.c = pack(out, n)
        return f
    enc = (lambda code=code: word(0x13, code)) if var else sh_imm(code, {8: 3, 16: 4, 32: 5}[n])
    op(nm, 0x13, code, enc)(mk(n, kind, var))

# accumulators (ac1)
def accw(fn, code): return lambda: word(fn, code, rd=1)
def acc_op(name, code, f):
    def g(st, w): st.acc = f(st, s(st.acc, 64)) & ((1 << 64) - 1)
    op(name, 0x30, code, accw(0x30, code))(g)
H = lambda v, i: lanes(v, 16, 1)[i]
U8 = lambda v, i: lanes(v, 8, 0)[i]
acc_op("dpau.h.qbl", 3, lambda st, A: A + U8(st.a, 3) * U8(st.b, 3) + U8(st.a, 2) * U8(st.b, 2))
acc_op("dpau.h.qbr", 7, lambda st, A: A + U8(st.a, 1) * U8(st.b, 1) + U8(st.a, 0) * U8(st.b, 0))
acc_op("dpsu.h.qbl", 11, lambda st, A: A - U8(st.a, 3) * U8(st.b, 3) - U8(st.a, 2) * U8(st.b, 2))
acc_op("dpsu.h.qbr", 15, lambda st, A: A - U8(st.a, 1) * U8(st.b, 1) - U8(st.a, 0) * U8(st.b, 0))
acc_op("dpaq_s.w.ph", 4, lambda st, A: A + q15mul(st, H(st.a, 1), H(st.b, 1), 17) + q15mul(st, H(st.a, 0), H(st.b, 0), 17))
acc_op("dpsq_s.w.ph", 5, lambda st, A: A - q15mul(st, H(st.a, 1), H(st.b, 1), 17) - q15mul(st, H(st.a, 0), H(st.b, 0), 17))
acc_op("mulsaq_s.w.ph", 6, lambda st, A: A + q15mul(st, H(st.a, 1), H(st.b, 1), 17) - q15mul(st, H(st.a, 0), H(st.b, 0), 17))
def l_w(sub):
    def f(st, A):
        x, y = s(st.a, 32), s(st.b, 32)
        if x == y == -2**31:
            p = 2**63 - 1
            st.flag(17)
        else:
            p = x * y * 2
        return clamp(st, A - p if sub else A + p, -2**63, 2**63 - 1, 17)
    return f
acc_op("dpaq_sa.l.w", 12, l_w(0))
acc_op("dpsq_sa.l.w", 13, l_w(1))
acc_op("maq_s.w.phl", 20, lambda st, A: A + q15mul(st, H(st.a, 1), H(st.b, 1), 17))
acc_op("maq_s.w.phr", 22, lambda st, A: A + q15mul(st, H(st.a, 0), H(st.b, 0), 17))
# the 64-bit sum (wrapping), then saturated to Q31
acc_op("maq_sa.w.phl", 16, lambda st, A: clamp(st, s(A + q15mul(st, H(st.a, 1), H(st.b, 1), 17), 64), -2**31, 2**31 - 1, 17))
acc_op("maq_sa.w.phr", 18, lambda st, A: clamp(st, s(A + q15mul(st, H(st.a, 0), H(st.b, 0), 17), 64), -2**31, 2**31 - 1, 17))
acc_op("dpa.w.ph", 0, lambda st, A: A + H(st.a, 1) * H(st.b, 1) + H(st.a, 0) * H(st.b, 0))
acc_op("dps.w.ph", 1, lambda st, A: A - H(st.a, 1) * H(st.b, 1) - H(st.a, 0) * H(st.b, 0))
acc_op("mulsa.w.ph", 2, lambda st, A: A + H(st.a, 1) * H(st.b, 1) - H(st.a, 0) * H(st.b, 0))
acc_op("dpax.w.ph", 8, lambda st, A: A + H(st.a, 1) * H(st.b, 0) + H(st.a, 0) * H(st.b, 1))
acc_op("dpsx.w.ph", 9, lambda st, A: A - H(st.a, 1) * H(st.b, 0) - H(st.a, 0) * H(st.b, 1))
def qx(sub, satur):
    def f(st, A):
        p = q15mul(st, H(st.a, 1), H(st.b, 0), 17) + q15mul(st, H(st.a, 0), H(st.b, 1), 17)
        v = s(A - p if sub else A + p, 64)
        return clamp(st, v, -2**31, 2**31 - 1, 17) if satur else v
    return f
acc_op("dpaqx_s.w.ph", 24, qx(0, 0))
acc_op("dpsqx_s.w.ph", 25, qx(1, 0))
acc_op("dpaqx_sa.w.ph", 26, qx(0, 1))
acc_op("dpsqx_sa.w.ph", 27, qx(1, 1))

# extraction (into rt = $5), from ac1
def ext_imm(code): return lambda: word(0x38, code, rs=random.getrandbits(5), rd=1)
def ext_var(code): return lambda: word(0x38, code, rd=1)
for nm, code, var, kind in [("extr.w", 0, 0, ""), ("extrv.w", 1, 1, ""), ("extr_r.w", 4, 0, "r"), ("extrv_r.w", 5, 1, "r"),
                            ("extr_rs.w", 6, 0, "rs"), ("extrv_rs.w", 7, 1, "rs"), ("extr_s.h", 14, 0, "h"),
                            ("extrv_s.h", 15, 1, "h")]:
    def mk(var, kind):
        def f(st, w):
            sa = (st.a if var else w >> 21) & 31
            A = s(st.acc, 64)
            if kind == "h":
                st.b = clamp(st, A >> sa, -32768, 32767, 23) & M32
                return
            v = ((A + (1 << (sa - 1))) >> sa) if (kind and sa) else (A >> sa)
            fits = -2**31 <= v < 2**31
            if not fits:
                st.flag(23)
            if kind == "rs" and not fits:
                v = -2**31 if v < 0 else 2**31 - 1
            st.b = v & M32
        return f
    op(nm, 0x38, code, ext_var(code) if var else ext_imm(code))(mk(var, kind))
for nm, code, var, dp in [("extp", 2, 0, 0), ("extpv", 3, 1, 0), ("extpdp", 10, 0, 1), ("extpdpv", 11, 1, 1)]:
    def mk(var, dp):
        def f(st, w):
            size = (st.a if var else w >> 21) & 31
            pos = st.ctl & 0x3F
            if pos - size < 0:
                st.ctl |= 1 << 14
                return
            st.ctl &= ~(1 << 14)
            st.b = (st.acc >> (pos - size)) & ((1 << (size + 1)) - 1)
            if dp:
                st.ctl = (st.ctl & ~0x3F) | ((pos - size - 1) & 0x3F)
        return f
    op(nm, 0x38, code, ext_var(code) if var else ext_imm(code))(mk(var, dp))
def shilo_enc(): return (0x1F << 26) | (random.getrandbits(6) << 20) | (1 << 11) | (26 << 6) | 0x38
@op("shilo", 0x38, 26, shilo_enc)
def _(st, w):
    sh = s(w >> 20, 6)
    st.acc = (st.acc >> sh if sh >= 0 else st.acc << -sh) & ((1 << 64) - 1)
@op("shilov", 0x38, 27, lambda: word(0x38, 27, rt=0, rd=1))
def _(st, w):
    sh = s(st.a, 6)
    st.acc = (st.acc >> sh if sh >= 0 else st.acc << -sh) & ((1 << 64) - 1)
@op("mthlip", 0x38, 31, lambda: word(0x38, 31, rt=0, rd=1))
def _(st, w):
    st.acc = ((st.acc << 32) | st.a) & ((1 << 64) - 1)
    st.ctl = (st.ctl & ~0x3F) | (((st.ctl & 0x3F) + 32) & 0x3F)
FIELDS = [0x3F, 0x3F << 7, 1 << 13, 0xFF << 16, 0xF << 24, 1 << 14]
def rddsp_enc(): return (0x1F << 26) | (random.getrandbits(6) << 16) | (6 << 11) | (18 << 6) | 0x38
@op("rddsp", 0x38, 18, rddsp_enc)
def _(st, w):
    m = (w >> 16) & 0x3F
    st.c = sum(st.ctl & FIELDS[k] for k in range(6) if m >> k & 1)
def wrdsp_enc(): return (0x1F << 26) | (4 << 21) | (random.getrandbits(6) << 11) | (19 << 6) | 0x38
@op("wrdsp", 0x38, 19, wrdsp_enc)
def _(st, w):
    m = (w >> 11) & 0x3F
    keep = sum(FIELDS[k] for k in range(6) if m >> k & 1)
    st.ctl = (st.ctl & ~keep) | (st.a & keep)

# append / prepend / balign (rt = $5 is source and destination)
def app_enc(code, bits): return lambda: word(0x31, code, rd=random.getrandbits(bits))
@op("append", 0x31, 0, app_enc(0, 5))
def _(st, w):
    sa = (w >> 11) & 31
    st.b = ((st.b << sa) | (st.a & ((1 << sa) - 1))) & M32
@op("prepend", 0x31, 1, app_enc(1, 5))
def _(st, w):
    sa = (w >> 11) & 31
    if sa:
        st.b = ((st.a << (32 - sa)) | (st.b >> sa)) & M32
@op("balign", 0x31, 16, app_enc(16, 2))
def _(st, w):
    bp = (w >> 11) & 3
    if bp:
        st.b = ((st.b << (8 * bp)) | (st.a >> (32 - 8 * bp))) & M32
@op("insv", 0x0C, 0, lambda: word(0x0C, 0, rd=0))
def _(st, w):
    pos, size = st.ctl & 0x3F, (st.ctl >> 7) & 0x3F
    if size and pos + size <= 32:
        m = ((1 << size) - 1) << pos
        st.b = (st.b & ~m) | ((st.a << pos) & m)

# ---- operands -------------------------------------------------------------------
EDGE = [0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x80008000, 0x7FFF7FFF, 0x00FF00FF, 0xFF00FF00, 0x80808080,
        0x7F7F7F7F, 0x8000FFFF, 0x00010000, 0x7F80FF00]
def r32():
    return random.choice(EDGE) if random.random() < 0.35 else random.getrandbits(32)
def r64():
    k = random.random()
    if k < 0.2: return random.choice([0, (1 << 63) - 1, 1 << 63, (1 << 64) - 1, 0x7FFFFFFF, 0xFFFFFFFF80000000])
    if k < 0.5: return random.getrandbits(40) | (((1 << 64) - (1 << 40)) if random.random() < 0.5 else 0)
    return random.getrandbits(64)
WMASK = 0x3F | (0x3F << 7) | (1 << 13) | (0xFF << 16) | (0xF << 24) | (1 << 14)

def main():
    rows = []
    for name, fn, code, enc, sem in OPS:
        for _ in range(12):
            w = enc()
            st = St(r32(), r32(), r32(), r64(), random.getrandbits(32) & WMASK)
            if fn == 0x0C or name.startswith("extp"):
                st.ctl = (st.ctl & ~0x1FFF) | random.randrange(64) | (random.randrange(1, 33) << 7)
            before = (st.a, st.b, st.c, st.acc, st.ctl)
            sem(st, w)
            rows.append((w,) + before + (st.a & M32, st.b & M32, st.c & M32, st.acc, st.ctl & WMASK, name))
    with open(sys.argv[1], "w") as fp:
        fp.write("/* generated by gen_dspvectors.py: do not edit */\n")
        fp.write("static const struct dspvec { uint32_t w, a, b, c; uint64_t acc; uint32_t ctl;\n"
                 "    uint32_t a2, b2, c2; uint64_t acc2; uint32_t ctl2; } DSPV[] = {\n")
        for r in rows:
            fp.write("    {0x%08x, 0x%08x, 0x%08x, 0x%08x, 0x%016xull, 0x%08x, 0x%08x, 0x%08x, 0x%08x, "
                     "0x%016xull, 0x%08x}, /* %s */\n" % r)
        fp.write("};\n")

if __name__ == "__main__":
    main()
