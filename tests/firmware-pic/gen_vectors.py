#!/usr/bin/env python3
"""Status-flag test vectors for the PIC self-tests, as straight-line gpasm code.

Each vector puts a and b in a register and W, STATUS = sin, runs one instruction and checks
the result and STATUS against values computed here from the data sheets' definitions of
each instruction (C: a carry out of bit 7, or for subtraction no borrow; DC: the same out of
bit 3; OV: the signed result out of range; N: bit 7; Z: zero), with plain integer
arithmetic - nothing shared with the lifter, which adds the complement. A failing vector
jumps to `vfail` with its number in VECH:VECL; the last one goes to `vectors_done`.

On the 12- and 14-bit families the code is laid out page by page (512 or 2048 words), each
page starting with a stub that reaches `vfail` in page 0; STATUS's page bits (baseline PA,
written with sin) and PCLATH (14-bit) always name the page the code is in.

usage: gen_vectors.py FAMILY START END OUT.inc   (word addresses; PIC18: byte addresses)
"""
import sys

C, DC, Z, OV, N = 1, 2, 4, 8, 16


def s8(x):
    return x - 256 if x & 0x80 else x


def add(a, b, cin):
    s = a + b + cin
    r = s & 0xFF
    f = (C if s > 0xFF else 0) | (DC if (a & 15) + (b & 15) + cin > 15 else 0)
    f |= OV if not -128 <= s8(a) + s8(b) + cin <= 127 else 0
    return r, f | (N if r & 0x80 else 0) | (Z if r == 0 else 0)


def sub(x, y, bw):
    d = x - y - bw
    r = d & 0xFF
    f = (C if d >= 0 else 0) | (DC if (x & 15) - (y & 15) - bw >= 0 else 0)
    f |= OV if not -128 <= s8(x) - s8(y) - bw <= 127 else 0
    return r, f | (N if r & 0x80 else 0) | (Z if r == 0 else 0)


def zn(r):
    return (Z if r == 0 else 0) | (N if r & 0x80 else 0)


def run(op, f, w, sin):
    """(result, flags set by the op, mask of the flags it affects) for PIC18 semantics; the
    14/12-bit families mask N and OV out (and differ only where noted)."""
    c = 1 if sin & C else 0
    if op == 'addwf':  r, fl = add(w, f, 0); return r, fl, C | DC | Z | OV | N
    if op == 'addwfc': r, fl = add(w, f, c); return r, fl, C | DC | Z | OV | N
    if op == 'subwf':  r, fl = sub(f, w, 0); return r, fl, C | DC | Z | OV | N
    if op == 'subwfb': r, fl = sub(f, w, 1 - c); return r, fl, C | DC | Z | OV | N
    if op == 'subfwb': r, fl = sub(w, f, 1 - c); return r, fl, C | DC | Z | OV | N
    if op == 'andwf':  r = w & f; return r, zn(r), Z | N
    if op == 'iorwf':  r = w | f; return r, zn(r), Z | N
    if op == 'xorwf':  r = w ^ f; return r, zn(r), Z | N
    if op == 'comf':   r = f ^ 0xFF; return r, zn(r), Z | N
    if op == 'movf':   r = f; return r, zn(r), Z | N
    if op == 'swapf':  return ((f << 4) | (f >> 4)) & 0xFF, 0, 0
    if op == 'incf18': r, fl = add(f, 1, 0); return r, fl, C | DC | Z | OV | N
    if op == 'decf18': r, fl = sub(f, 1, 0); return r, fl, C | DC | Z | OV | N
    if op == 'incf':   r = (f + 1) & 0xFF; return r, zn(r), Z
    if op == 'decf':   r = (f - 1) & 0xFF; return r, zn(r), Z
    if op == 'negf':   r, fl = sub(0, f, 0); return r, fl, C | DC | Z | OV | N
    if op in ('rrf', 'rrcf'):
        r = (c << 7) | (f >> 1)
        return r, (C if f & 1 else 0) | zn(r), C | (Z | N if op == 'rrcf' else 0)
    if op in ('rlf', 'rlcf'):
        r = ((f << 1) | c) & 0xFF
        return r, (C if f & 0x80 else 0) | zn(r), C | (Z | N if op == 'rlcf' else 0)
    if op == 'rrncf':  r = ((f >> 1) | (f << 7)) & 0xFF; return r, zn(r), Z | N
    if op == 'rlncf':  r = ((f << 1) | (f >> 7)) & 0xFF; return r, zn(r), Z | N
    if op == 'lslf':   r = (f << 1) & 0xFF; return r, (C if f & 0x80 else 0) | zn(r), C | Z
    if op == 'lsrf':   r = f >> 1; return r, (C if f & 1 else 0) | zn(r), C | Z
    if op == 'asrf':   r = (f >> 1) | (f & 0x80); return r, (C if f & 1 else 0) | zn(r), C | Z
    # literal ops: f is the literal k, w is W
    if op == 'addlw':  r, fl = add(w, f, 0); return r, fl, C | DC | Z | OV | N
    if op == 'sublw':  r, fl = sub(f, w, 0); return r, fl, C | DC | Z | OV | N
    if op == 'andlw':  r = w & f; return r, zn(r), Z | N
    if op == 'iorlw':  r = w | f; return r, zn(r), Z | N
    if op == 'xorlw':  r = w ^ f; return r, zn(r), Z | N
    raise ValueError(op)


FULL = [0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFE, 0xFF]
MEDIUM = [0x00, 0x01, 0x0F, 0x7F, 0x80, 0xFF]
SMALL = [0x00, 0x80, 0xFF]

# family: (binary f/W ops, unary f ops, literal ops, carry-using ops, values, flag bits, sins)
FAMILIES = {
    'baseline': (['addwf', 'subwf', 'andwf', 'iorwf', 'xorwf'],
                 ['comf', 'incf', 'decf', 'rrf', 'rlf', 'swapf', 'movf'], [],
                 {'rrf', 'rlf'}, SMALL, C | DC | Z, [0x03], [0x00, 0x03]),
    'mid':      (['addwf', 'subwf', 'andwf', 'iorwf', 'xorwf'],
                 ['comf', 'incf', 'decf', 'rrf', 'rlf', 'swapf', 'movf'], ['addlw', 'sublw'],
                 {'rrf', 'rlf'}, MEDIUM, C | DC | Z, [0x03], [0x00, 0x03]),
    'enhanced': (['addwf', 'subwf', 'andwf', 'iorwf', 'xorwf', 'addwfc', 'subwfb'],
                 ['comf', 'incf', 'decf', 'rrf', 'rlf', 'swapf', 'movf', 'lslf', 'lsrf', 'asrf'],
                 ['addlw', 'sublw', 'andlw', 'iorlw', 'xorlw'],
                 {'addwfc', 'subwfb', 'rrf', 'rlf'}, FULL, C | DC | Z, [0x03], [0x00, 0x07]),
    'pic18':    (['addwf', 'addwfc', 'subwf', 'subwfb', 'subfwb', 'andwf', 'iorwf', 'xorwf'],
                 ['comf', 'incf18', 'decf18', 'negf', 'rrcf', 'rlcf', 'rrncf', 'rlncf', 'swapf', 'movf'],
                 ['addlw', 'sublw', 'andlw', 'iorlw', 'xorlw'],
                 {'addwfc', 'subwfb', 'subfwb', 'rrcf', 'rlcf'}, FULL, C | DC | Z | OV | N,
                 [0x0B], [0x00, 0x1F]),
}


def vectors(fam):
    binops, unops, litops, carry, vals, bits, sin1, sin2 = FAMILIES[fam]
    out = []
    for op in binops:
        for a in vals:
            for b in vals:
                for sin in (sin2 if op in carry else sin1):
                    out.append((op, 'f', a, b, sin))
    for op in unops:
        for a in vals:
            for sin in (sin2 if op in carry else sin1):
                out.append((op, 'u', a, 0, sin))
    for op in litops:
        for a in vals:
            for b in vals:
                out.append((op, 'l', b, a, sin1[0]))   # W = a, k = b
    res = []
    for op, kind, f, w, sin in out:
        r, fl, mask = run(op, f, w, sin)
        mask &= bits
        sout = (sin & bits & ~mask) | (fl & mask)
        res.append((op, kind, f, w, sin, r, sout))
    return res, bits


def mnemonic(op):
    return {'incf18': 'incf', 'decf18': 'decf'}.get(op, op)


def body(fam, v, page_bits):
    """The code of one vector (one word per instruction on the 12/14-bit families), and the
    expected result and STATUS."""
    op, kind, f, w, sin, r, sout = v
    mn = mnemonic(op)
    p18 = fam == 'pic18'
    A = ',A' if p18 else ''
    st = ['movlw 0x%02x' % (sin | page_bits), 'movwf STATUS' + A]
    if kind == 'l':     # W = w, k = f; the result in W goes to VX
        code = st + ['movlw 0x%02x' % w, '%s 0x%02x' % (mn, f), 'movwf VX' + A]
    elif kind == 'f':
        code = ['movlw 0x%02x' % f, 'movwf VX' + A] + st + ['movlw 0x%02x' % w, '%s VX,f%s' % (mn, A)]
    else:   # NEGF has no destination operand
        code = ['movlw 0x%02x' % f, 'movwf VX' + A] + st + ['%s VX%s%s' % (mn, '' if op == 'negf' else ',f', A)]
    code += ['movff STATUS,VSTAT'] if p18 else ['swapf STATUS,w', 'movwf VSTAT']
    return code, r, sout


def gen(fam, start, end, out):
    vecs, bits = vectors(fam)
    p18 = fam == 'pic18'
    lines = ['; generated by gen_vectors.py for %s: do not edit' % fam,
             '; %d vectors' % len(vecs)]
    if p18:
        lines += ['vectors_start:', '        clrf VECL,A', '        clrf VECH,A']
        for v in vecs:
            code, r, sout = body(fam, v, 0)
            lines += ['        infsnz VECL,f,A', '        incf VECH,f,A']
            lines += ['        ' + c for c in code]
            lines += ['        movf VX,w,A', '        xorlw 0x%02x' % r, '        btfss STATUS,Z,A',
                      '        call vfail',
                      '        movf VSTAT,w,A', '        andlw 0x%02x' % bits, '        xorlw 0x%02x' % sout,
                      '        btfss STATUS,Z,A', '        call vfail']
        lines += ['        goto vectors_done']
        open(out, 'w').write('\n'.join(lines) + '\n')
        return
    base = fam == 'baseline'
    page = 512 if base else 2048
    pos = start
    cur_page = None

    def emit(instrs):
        nonlocal pos
        for i in instrs:
            lines.append('        ' + i)
            pos += 1

    def page_select(p):
        # baseline: PA bits in STATUS<6:5> (as sin carries them); 14-bit: PCLATH<4:3>
        if base:
            return ['movlw 0x%02x' % (p << 5), 'movwf STATUS']
        return ['movlw 0x%02x' % (p << 3), 'movwf PCLATH']

    def start_page(p):
        nonlocal pos, cur_page
        pos = p * page
        lines.append('        org 0x%x' % pos)
        lines.append('vstub%d:' % p)
        emit(page_select(0) + ['goto vfail'])
        lines.append('vpage%d:' % p)
        cur_page = p

    # the entry: page_select into the first page, fall into it
    first = start // page
    if start % page:
        raise SystemExit('vectors must start at a page boundary')
    start_page(first)
    lines.append('vectors_start:')
    emit(['clrf VECL', 'clrf VECH'])
    for i, v in enumerate(vecs):
        pb = (cur_page << 5) if base else 0
        code, r, sout = body(fam, v, pb)
        count = ['incf VECL,f'] if base else ['incf VECL,f', 'btfsc STATUS,Z', 'incf VECH,f']
        chk = count + code + [
            'movf VX,w', 'xorlw 0x%02x' % r, 'btfss STATUS,Z', 'goto vstub%d' % cur_page,
            'swapf VSTAT,w', 'andlw 0x%02x' % bits, 'xorlw 0x%02x' % sout, 'btfss STATUS,Z',
            'goto vstub%d' % cur_page]
        if pos + len(chk) + 3 > (cur_page + 1) * page:
            nxt = cur_page + 1
            emit(page_select(nxt) + ['goto vpage%d' % nxt])
            start_page(nxt)
            if base:   # the page bits sin carries changed
                code, r, sout = body(fam, v, nxt << 5)
                chk = chk[:len(count)] + code + chk[len(count) + len(code):]
        emit(chk)
    emit(page_select(0) + ['goto vectors_done'])
    if pos > end:
        raise SystemExit('%s: vectors end at word 0x%x, past 0x%x' % (fam, pos, end))
    open(out, 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    gen(sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4])
