#!/usr/bin/env python3
"""Generate the 6502 self-test's instruction vectors for one model.

usage: gen_vectors.py nmos|2a03|65c02 out.s

Each vector runs one instruction from RAM (the opcode, an operand byte, RTS) with A, X,
the zero-page byte CELL and P given, and gives the A, X, CELL and P (as PHP pushes it)
that must follow. The expected values come from the instruction definitions written
out below: binary and decimal ADC and SBC (the NMOS and 65C02 rules for each flag, for
every input, BCD or not), the shifts, compares and increments, the NMOS combined
instructions and the 65C02's TRB, TSB, RMB, SMB and BIT #imm. The 2A03 ignores D.
"""
import random
import sys

CELL = 0x40      # the zero-page byte the memory forms use
NOP = 0xEA       # the operand byte of a one-byte instruction (runs as a NOP)
MAGIC = 0xEE     # the constant XAA and LXA OR into A (uam_m6502_nmos)
C, Z, I, D, B, U, V, N = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80


class Model:
    def __init__(self, name):
        self.cmos = name == "65c02"
        self.decimal = name != "2a03"


def nz(p, r):
    p &= ~(N | Z)
    if r & 0x80:
        p |= N
    if r == 0:
        p |= Z
    return p


def setf(p, f, cond):
    return p | f if cond else p & ~f


def adc(m, p, a, b):
    c = p & C
    if not (m.decimal and p & D):
        s = a + b + c
        r = s & 0xFF
        p = setf(p, C, s > 0xFF)
        p = setf(p, V, (a ^ r) & (b ^ r) & 0x80)
        return r, nz(p, r)
    al = (a & 0x0F) + (b & 0x0F) + c
    if al >= 0x0A:
        al = ((al + 0x06) & 0x0F) + 0x10
    s = (a & 0xF0) + (b & 0xF0) + al
    signed = (a & 0xF0) - (0x100 if a & 0x80 else 0) + (b & 0xF0) - (0x100 if b & 0x80 else 0) + al
    p = setf(p, V, signed < -128 or signed > 127)
    if s >= 0xA0:
        s += 0x60
    r = s & 0xFF
    p = setf(p, C, s >= 0x100)
    if m.cmos:
        return r, nz(p, r)
    p = setf(p, N, signed & 0x80)
    return r, setf(p, Z, (a + b + c) & 0xFF == 0)


def sbc(m, p, a, b):
    c = p & C
    bin_ = a - b - (1 - c)
    b8 = bin_ & 0xFF
    p = setf(p, C, bin_ >= 0)
    p = setf(p, V, (a ^ b) & (a ^ b8) & 0x80)
    if not (m.decimal and p & D):
        return b8, nz(p, b8)
    al = (a & 0x0F) - (b & 0x0F) + c - 1
    if m.cmos:
        s = bin_
        if s < 0:
            s -= 0x60
        if al < 0:
            s -= 0x06
        r = s & 0xFF
        return r, nz(p, r)
    if al < 0:
        al = ((al - 0x06) & 0x0F) - 0x10
    s = (a & 0xF0) - (b & 0xF0) + al
    if s < 0:
        s -= 0x60
    return s & 0xFF, nz(p, b8)


def cmp(p, r, b):
    p = setf(p, C, r >= b)
    return nz(p, (r - b) & 0xFF)


def asl(p, v):
    return (v << 1) & 0xFF, setf(p, C, v & 0x80)


def lsr(p, v):
    return v >> 1, setf(p, C, v & 1)


def rol(p, v):
    return ((v << 1) | (p & C)) & 0xFF, setf(p, C, v & 0x80)


def ror(p, v):
    return (v >> 1) | ((p & C) << 7), setf(p, C, v & 1)


def arr(m, p, a, imm):
    t = a & imm
    r = (t >> 1) | ((p & C) << 7)
    if not (m.decimal and p & D):
        p = nz(p, r)
        p = setf(p, C, r & 0x40)
        return r, setf(p, V, ((r >> 6) ^ (r >> 5)) & 1)
    p = setf(p, N, p & C)
    p = setf(p, Z, r == 0)
    p = setf(p, V, (r ^ t) & 0x40)
    if (t & 0x0F) + (t & 0x01) > 5:
        r = (r & 0xF0) | ((r + 6) & 0x0F)
    if (t & 0xF0) + (t & 0x10) > 0x50:
        r = (r + 0x60) & 0xFF
        p |= C
    else:
        p &= ~C
    return r, p


def run(m, op, opnd, a, x, cell, p):
    """The state after one instruction: (a, x, cell, p)."""
    imm = opnd
    if op in (0x69, 0x65):                                  # ADC #imm / zp
        a, p = adc(m, p, a, imm if op == 0x69 else cell)
    elif op in (0xE9, 0xE5, 0xEB):                          # SBC #imm / zp, SBC #imm (NMOS 0xEB)
        a, p = sbc(m, p, a, cell if op == 0xE5 else imm)
    elif op == 0xC9:                                        # CMP #imm
        p = cmp(p, a, imm)
    elif op == 0xE0:                                        # CPX #imm
        p = cmp(p, x, imm)
    elif op == 0x24:                                        # BIT zp
        p = setf(p, Z, (a & cell) == 0)
        p = setf(p, V, cell & 0x40)
        p = setf(p, N, cell & 0x80)
    elif op == 0x89:                                        # BIT #imm (65C02): Z only
        p = setf(p, Z, (a & imm) == 0)
    elif op in (0x06, 0x46, 0x26, 0x66):                    # ASL LSR ROL ROR zp
        cell, p = {0x06: asl, 0x46: lsr, 0x26: rol, 0x66: ror}[op](p, cell)
        p = nz(p, cell)
    elif op in (0x0A, 0x4A, 0x2A, 0x6A):                    # ... A
        a, p = {0x0A: asl, 0x4A: lsr, 0x2A: rol, 0x6A: ror}[op](p, a)
        p = nz(p, a)
    elif op in (0xE6, 0xC6):                                # INC DEC zp
        cell = (cell + (1 if op == 0xE6 else -1)) & 0xFF
        p = nz(p, cell)
    elif op in (0x1A, 0x3A):                                # INC A, DEC A (65C02)
        a = (a + (1 if op == 0x1A else -1)) & 0xFF
        p = nz(p, a)
    elif op == 0x07 and not m.cmos:                         # SLO
        cell, p = asl(p, cell)
        a |= cell
        p = nz(p, a)
    elif op == 0x27 and not m.cmos:                         # RLA
        cell, p = rol(p, cell)
        a &= cell
        p = nz(p, a)
    elif op == 0x47 and not m.cmos:                         # SRE
        cell, p = lsr(p, cell)
        a ^= cell
        p = nz(p, a)
    elif op == 0x67 and not m.cmos:                         # RRA
        cell, p = ror(p, cell)
        a, p = adc(m, p, a, cell)
    elif op == 0x87 and not m.cmos:                         # SAX
        cell = a & x
    elif op == 0xA7 and not m.cmos:                         # LAX
        a = x = cell
        p = nz(p, a)
    elif op == 0xC7 and not m.cmos:                         # DCP
        cell = (cell - 1) & 0xFF
        p = cmp(p, a, cell)
    elif op == 0xE7 and not m.cmos:                         # ISC
        cell = (cell + 1) & 0xFF
        a, p = sbc(m, p, a, cell)
    elif op in (0x0B, 0x2B):                                # ANC
        a &= imm
        p = nz(p, a)
        p = setf(p, C, a & 0x80)
    elif op == 0x4B:                                        # ALR
        a, p = lsr(p, a & imm)
        p = nz(p, a)
    elif op == 0x6B:                                        # ARR
        a, p = arr(m, p, a, imm)
    elif op == 0xCB:                                        # SBX
        t = a & x
        p = cmp(p, t, imm)
        x = (t - imm) & 0xFF
    elif op == 0xAB:                                        # LXA
        a = x = (a | MAGIC) & imm
        p = nz(p, a)
    elif op == 0x8B:                                        # XAA
        a = (a | MAGIC) & x & imm
        p = nz(p, a)
    elif op in (0x04, 0x14):                                # TSB TRB zp (65C02)
        p = setf(p, Z, (a & cell) == 0)
        cell = cell | a if op == 0x04 else cell & ~a & 0xFF
    elif m.cmos and op & 0x0F == 0x07:                      # RMBn / SMBn
        bit = 1 << ((op >> 4) & 7)
        cell = cell | bit if op & 0x80 else cell & ~bit & 0xFF
    else:
        raise ValueError("no model of opcode 0x%02x" % op)
    return a, x, cell, (p & 0xFF) | B | U


def vectors(m, rnd):
    out = []

    def vec(op, opnd, a, x, cell, p):
        p = (p & ~(B | U)) | I       # I set: no interrupt during a vector
        ea, ex, ecell, ep = run(m, op, opnd, a, x, cell, p)
        out.append((op, opnd, a, x, cell, p, ea, ex, ecell, ep))

    edge = [0x00, 0x01, 0x09, 0x0F, 0x10, 0x50, 0x7F, 0x80, 0x99, 0x9A, 0xF0, 0xFF]

    def operands(n):
        for a in edge:
            for b in edge[::3]:
                yield a, b
        for _ in range(n):
            yield rnd.randrange(256), rnd.randrange(256)

    def bcd():
        return (rnd.randrange(10) << 4) | rnd.randrange(10)

    for d in (0, D):
        for op in (0x69, 0xE9):
            for a, b in operands(120):
                vec(op, b, a, 0, 0, d | rnd.choice((0, C)) | rnd.choice((0, V, N, Z)))
            for _ in range(60):                                  # valid BCD, the common case
                vec(op, bcd(), bcd(), 0, 0, d | rnd.choice((0, C)))
        for _ in range(16):
            vec(0x65, CELL, rnd.randrange(256), 0, rnd.randrange(256), d | rnd.choice((0, C)))
            vec(0xE5, CELL, rnd.randrange(256), 0, rnd.randrange(256), d | rnd.choice((0, C)))
    for _ in range(24):
        a, b = rnd.randrange(256), rnd.randrange(256)
        vec(0xC9, b, a, 0, 0, rnd.choice((0, C, N | Z)))
        vec(0xE0, b, 0, a, 0, rnd.choice((0, C)))
        vec(0x24, CELL, a, 0, b, rnd.choice((0, V, N | Z)))
        for op in (0x06, 0x46, 0x26, 0x66, 0xE6, 0xC6):
            vec(op, CELL, 0, 0, b, rnd.choice((0, C)))
        for op in (0x0A, 0x4A, 0x2A, 0x6A):
            vec(op, NOP, a, 0, 0, rnd.choice((0, C)))
    if m.cmos:
        for _ in range(24):
            a, b = rnd.randrange(256), rnd.randrange(256)
            vec(0x04, CELL, a, 0, b, rnd.choice((0, Z)))
            vec(0x14, CELL, a, 0, b, rnd.choice((0, Z)))
            vec(0x89, b, a, 0, 0, rnd.choice((0, V | N, Z)))
            vec(0x1A, NOP, a, 0, 0, 0)
            vec(0x3A, NOP, a, 0, 0, 0)
        for n in range(16):
            vec((n << 4) | 0x07, CELL, 0, 0, rnd.randrange(256), 0)
    else:
        for d in (0, D):
            for _ in range(24):
                a, x, b = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
                p = d | rnd.choice((0, C)) | rnd.choice((0, V))
                for op in (0x07, 0x27, 0x47, 0x67, 0x87, 0xA7, 0xC7, 0xE7):
                    vec(op, CELL, a, x, b, p)
                for op in (0x0B, 0x2B, 0x4B, 0x6B, 0xCB, 0xAB, 0x8B, 0xEB):
                    vec(op, b, a, x, 0, p)
            for a in range(0, 256, 17):                          # ARR's digit adjustments
                vec(0x6B, 0xFF, a, 0, 0, d | C)
                vec(0x6B, 0xFF, a | 0x11, 0, 0, d)
    return out


def main():
    model, path = sys.argv[1], sys.argv[2]
    vs = vectors(Model(model), random.Random(6502))
    with open(path, "w") as f:
        f.write("; generated by gen_vectors.py %s: %d vectors\n" % (model, len(vs)))
        f.write("; op, operand, A, X, CELL, P in; A, X, CELL, P (as pushed) out\n")
        f.write("vectors:\n\t.dw %d\n" % len(vs))
        for v in vs:
            f.write("\t.db " + ", ".join("0x%02x" % b for b in v) + "\n")


if __name__ == "__main__":
    main()
