#!/usr/bin/env python3
"""Generate the Z80 self-test's vectors (sdasz80 source) from a model of the instructions.

The model is written from the instruction set's definitions, flags bits 3 and 5 included
(selftest.s says which), independently of ember's lifter. Three tables, each run by a
generic harness in selftest.s that patches the instruction into its code (so the harness
also exercises self-modifying code):

  vec_op    op1 op2 a f b -> a f b        an instruction on A and B, A and F set by POP AF
  vec_16    op1 op2 hl bc f -> hl f       ADD/ADC/SBC HL,BC
  vec_blk   op a f v c b l -> f b c       an ED block instruction, one iteration that ends:
                                          HL = 0xF100 | l holds v, DE = 0xF200, the UART
                                          scratch register (port 7) holds v

Usage: gen_vectors.py OUT.s
"""
import random
import sys


def sz53(v):
    return (v & 0xA8) | (0x40 if v == 0 else 0)


def par(v):
    return 0x04 if bin(v & 0xFF).count("1") % 2 == 0 else 0


# ---- the model ---------------------------------------------------------------------

def alu(y, a, b, f):
    """ADD ADC SUB SBC AND XOR OR CP: (A, F)."""
    if y in (0, 1, 2, 3, 7):
        sub = y >= 2
        cin = f & 1 if y in (1, 3) else 0
        s = a - b - cin if sub else a + b + cin
        r = s & 0xFF
        cy = 1 if s & 0x100 else 0
        h = ((a ^ b ^ r) >> 4) & 1
        ov = (((a ^ b) & (a ^ r)) if sub else ((a ^ r) & (b ^ r))) >> 7 & 1
        xy = b if y == 7 else r
        nf = (r & 0x80) | (0x40 if r == 0 else 0) | (xy & 0x28) | h << 4 | ov << 2 | (2 if sub else 0) | cy
        return (a if y == 7 else r), nf
    r = (a & b) if y == 4 else (a ^ b) if y == 5 else (a | b)
    return r, sz53(r) | par(r) | (0x10 if y == 4 else 0)


def inc_dec(v, f, dec):
    r = (v - 1 if dec else v + 1) & 0xFF
    h = ((v if dec else r) & 0xF) == 0
    ov = r == (0x7F if dec else 0x80)
    return r, (f & 1) | sz53(r) | (0x10 if h else 0) | (0x04 if ov else 0) | (0x02 if dec else 0)


def rot(y, v, f, acc):
    """CB rotates/shifts (RLC RRC RL RR SLA SRA SLL SRL); acc: RLCA RRCA RLA RRA."""
    c = f & 1
    left = y % 2 == 0
    out = (v >> 7) & 1 if left else v & 1
    if y == 0: r = (v << 1 | out)
    elif y == 1: r = (v >> 1 | out << 7)
    elif y == 2: r = (v << 1 | c)
    elif y == 3: r = (v >> 1 | c << 7)
    elif y == 4: r = v << 1
    elif y == 5: r = v >> 1 | (v & 0x80)
    elif y == 6: r = v << 1 | 1
    else: r = v >> 1
    r &= 0xFF
    if acc:
        return r, (f & 0xC4) | (r & 0x28) | out
    return r, sz53(r) | par(r) | out


def daa(a, f):
    c, h, n = f & 1, (f >> 4) & 1, (f >> 1) & 1
    lo = a & 0xF
    diff = (6 if (h or lo > 9) else 0)
    c2 = 1 if (c or a > 0x99) else 0
    if c2:
        diff |= 0x60
    r = (a - diff if n else a + diff) & 0xFF
    h2 = (h and lo < 6) if n else lo > 9
    return r, sz53(r) | par(r) | (0x10 if h2 else 0) | (f & 2) | c2


def cb(op, b, f):
    """CB op on register B (op & 7 == 0): (B, F)."""
    x, y = op >> 6, (op >> 3) & 7
    if x == 0:
        return rot(y, b, f, False)
    if x == 1:
        t = b & (1 << y)
        nf = (f & 1) | 0x10 | (b & 0x28)
        nf |= (0x80 if y == 7 else 0) if t else 0x44
        return b, nf
    if x == 2:
        return b & ~(1 << y) & 0xFF, f
    return b | (1 << y), f


def op_ab(op1, op2, a, f, b):
    """The vec_op instructions: (A, F, B) after. The harness sets AF with POP AF, so Q = 0
    for SCF/CCF: their bits 3 and 5 are (F | A)."""
    if op1 == 0xCB:
        nb, nf = cb(op2, b, f)
        return a, nf, nb
    if op1 == 0xED:                       # NEG and its copies
        r, nf = alu(2, 0, a, f)
        return r, nf, b
    if 0x80 <= op1 <= 0xBF:
        r, nf = alu((op1 >> 3) & 7, a, b, f)
        return r, nf, b
    if op1 in (0x3C, 0x3D):
        r, nf = inc_dec(a, f, op1 == 0x3D)
        return r, nf, b
    if op1 in (0x04, 0x05):
        r, nf = inc_dec(b, f, op1 == 0x05)
        return a, nf, r
    if op1 in (0x07, 0x0F, 0x17, 0x1F):
        r, nf = rot((op1 >> 3) & 3, a, f, True)
        return r, nf, b
    if op1 == 0x27:
        r, nf = daa(a, f)
        return r, nf, b
    if op1 == 0x2F:
        r = a ^ 0xFF
        return r, (f & 0xC5) | 0x12 | (r & 0x28), b
    xy = (f | a) & 0x28
    if op1 == 0x37:                       # SCF
        return a, (f & 0xC4) | xy | 1, b
    if op1 == 0x3F:                       # CCF
        return a, (f & 0xC4) | xy | ((f & 1) << 4) | ((f & 1) ^ 1), b
    raise ValueError(hex(op1))


def arith16(kind, x, y, f):
    """0 ADD, 1 ADC, 2 SBC HL,BC: (HL, F)."""
    sub = kind == 2
    cin = f & 1 if kind else 0
    s = x - y - cin if sub else x + y + cin
    r = s & 0xFFFF
    h = ((x ^ y ^ r) >> 12) & 1
    nf = ((r >> 8) & 0x28) | h << 4 | (1 if s & 0x10000 else 0)
    if kind == 0:
        nf |= f & 0xC4
    else:
        ov = (((x ^ y) & (x ^ r)) if sub else ((x ^ r) & (y ^ r))) >> 15 & 1
        nf |= ((r >> 8) & 0x80) | (0x40 if r == 0 else 0) | ov << 2 | (2 if sub else 0)
    return r, nf


def block(op, a, f, v, c, b, l):
    """One iteration of ED op that ends: (F, B, C)."""
    y, z = (op >> 3) & 7, op & 7
    dec = y & 1
    step = -1 if dec else 1
    bc = (b << 8 | c)
    hl = 0xF100 | l
    if z == 0:                              # LDI ...
        bc2 = (bc - 1) & 0xFFFF
        n = (v + a) & 0xFF
        nf = (f & 0xC1) | (0x04 if bc2 else 0) | (n & 0x08) | ((n & 0x02) << 4)
        return nf, bc2 >> 8, bc2 & 0xFF
    if z == 1:                              # CPI ...
        r = (a - v) & 0xFF
        h = ((a ^ v ^ r) >> 4) & 1
        n = (r - h) & 0xFF
        bc2 = (bc - 1) & 0xFFFF
        nf = (f & 1) | 0x02 | (r & 0x80) | (0x40 if r == 0 else 0) | h << 4 | (0x04 if bc2 else 0)
        nf |= (n & 0x08) | ((n & 0x02) << 4)
        return nf, bc2 >> 8, bc2 & 0xFF
    b2 = (b - 1) & 0xFF
    if z == 2:                              # INI ...
        k = v + ((c + step) & 0xFF)
    else:                                   # OUTI ...
        k = v + ((hl + step) & 0xFF)
    nf = sz53(b2) | (0x02 if v & 0x80 else 0) | (0x11 if k > 255 else 0) | par((k & 7) ^ b2)
    return nf, b2, c


# ---- the vectors ---------------------------------------------------------------------

EDGE = [0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0x81, 0xFF, 0x09, 0x99, 0x9A, 0x28, 0xD7]
FLAGS = [0x00, 0x01, 0xFF, 0xFE, 0x10, 0x11, 0x02, 0x13, 0x28, 0xD5]


def vec_op(rnd):
    out = []
    def add(op1, op2, a, f, b):
        ra, rf, rb = op_ab(op1, op2, a, f, b)
        out.append((op1, op2, a, f, b, ra, rf, rb))
    # ALU A,B: edges against edges, then random pairs; both carries.
    for y in range(8):
        for a in EDGE:
            for b in EDGE:
                add(0x80 | y << 3, 0, a, rnd.choice(FLAGS) & 0xFE | (a ^ b) & 1, b)
        for _ in range(30):
            add(0x80 | y << 3, 0, rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
    # INC/DEC A and B: every value.
    for v in range(256):
        add(0x3C, 0, v, rnd.randrange(256), 0)
        add(0x3D, 0, v, rnd.randrange(256), 0)
    for v in EDGE:
        add(0x04, 0, 0x55, rnd.randrange(256), v)
        add(0x05, 0, 0x55, rnd.randrange(256), v)
    # DAA: every A, with four of the N/H/C combinations.
    for a in range(256):
        for nhc in rnd.sample(range(8), 4):
            f = (nhc & 4) >> 1 | (nhc & 2) << 3 | (nhc & 1) | (rnd.randrange(256) & 0xEC)
            add(0x27, 0, a, f, 0)
    # RLCA RRCA RLA RRA, CPL, SCF, CCF.
    for op in (0x07, 0x0F, 0x17, 0x1F, 0x2F, 0x37, 0x3F):
        for _ in range(40):
            add(op, 0, rnd.randrange(256), rnd.randrange(256), 0)
    # NEG (ED 44) and two of its copies.
    for op2 in (0x44, 0x4C, 0x7C):
        for a in EDGE + [rnd.randrange(256) for _ in range(12)]:
            add(0xED, op2, a, rnd.randrange(256), 0)
    # Every CB operation on B.
    for op2 in range(0, 256, 8):
        for v in EDGE + [rnd.randrange(256) for _ in range(6)]:
            add(0xCB, op2, rnd.randrange(256), rnd.randrange(256), v)
    return out


def vec_16(rnd):
    out = []
    E16 = [0x0000, 0x0001, 0x0FFF, 0x1000, 0x7FFF, 0x8000, 0xFFFF, 0x8FFF, 0x2800, 0xD700]
    for kind, (op1, op2) in enumerate(((0x09, 0x00), (0xED, 0x4A), (0xED, 0x42))):
        pairs = [(x, y) for x in E16 for y in E16] + [(rnd.randrange(65536), rnd.randrange(65536)) for _ in range(40)]
        for x, y in pairs:
            f = rnd.randrange(256)
            r, nf = arith16(kind, x, y, f)
            out.append((op1, op2, x, y, f, r, nf))
    return out


def vec_blk(rnd):
    out = []
    for op in (0xA0, 0xA8, 0xB0, 0xB8, 0xA1, 0xA9, 0xB1, 0xB9):
        for _ in range(40):
            a, f, v = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
            rep = op >= 0xB0
            if rep and op & 1 and rnd.randrange(2):
                v = a                        # CPIR/CPDR ending on a match
                bc = rnd.choice([1, 2, 0x1234])
            elif rep:
                bc = 1                       # the last iteration
            else:
                bc = rnd.choice([1, 2, 0x0100, 0xFFFF, rnd.randrange(65536)])
            c, b, l = bc & 0xFF, bc >> 8, 0
            out.append((op, a, f, v, c, b, l) + block(op, a, f, v, c, b, l))
    for op in (0xA2, 0xAA, 0xB2, 0xBA, 0xA3, 0xAB, 0xB3, 0xBB):
        for _ in range(48):
            f, v = rnd.randrange(256), rnd.randrange(256)
            b = 1 if op >= 0xB0 else rnd.choice([1, 2, 0x80, 0x00, 0x10, 0x11, rnd.randrange(256)])
            l = rnd.randrange(256) if op & 1 else 0
            out.append((op, 0x5A, f, v, 7, b, l) + block(op, 0x5A, f, v, 7, b, l))
    return out


def emit(f, name, rows, fmt):
    f.write("\n%s_n == %d\n%s:\n" % (name, len(rows), name))
    for r in rows:
        f.write(fmt(r) + "\n")


def h(*v):
    return ",".join("0x%02x" % x for x in v)


def main():
    rnd = random.Random(0x280)
    with open(sys.argv[1], "w") as f:
        f.write("; Generated by gen_vectors.py - do not edit.\n")
        emit(f, "vec_op", vec_op(rnd), lambda r: "\t.db " + h(*r))
        emit(f, "vec_16", vec_16(rnd),
             lambda r: "\t.db %s\n\t.dw 0x%04x,0x%04x\n\t.db 0x%02x\n\t.dw 0x%04x\n\t.db 0x%02x"
             % (h(r[0], r[1]), r[2], r[3], r[4], r[5], r[6]))
        emit(f, "vec_blk", vec_blk(rnd), lambda r: "\t.db " + h(*r))


if __name__ == "__main__":
    main()
