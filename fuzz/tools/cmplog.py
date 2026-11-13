#!/usr/bin/env python3
"""cmplog.py -- input-to-state candidates from ember's compare logs.

ember logs the values an input's compares saw when the brain sends it as CMP_INPUT
(docs/fuzz-formats.md, "Fuzz protocol"; the compare pass is uam/cmp.h): one CMPS frame,
each record (site, width, kind, a, b). A firmware that compares part of its input with a
constant usually holds that part in the input as is, in one byte order or the other.
So for each record this module finds one operand's bytes in the input and writes the
other operand there (AFL++'s input-to-state replacement, without colorization). It also
tries the other operand plus and minus one for ordered compares, to land on either side
of a boundary. Both byte orders are tried, at the compare's width and at the narrowest
width that holds both values (a byte compared in a 32-bit register is one byte of input).

Constants of the code (records with the IMM kind) that the input does not contain are
returned as dictionary tokens, for compares on a transformed copy of the input.

  python3 fuzz/tools/cmplog.py --selftest
"""
import struct
import sys
from dataclasses import dataclass
from typing import List, Tuple

EQ, ORDER, SIGNED, IMM = 0x1, 0x2, 0x4, 0x8     # CMPS kinds (fuzz/formats.h)
HDR = struct.Struct(">QII")                     # id, n, dropped
ENTRY = struct.Struct(">IBBQQ")                 # site, width, kind, a, b
MAX_OCCURRENCES = 8                             # positions of one pattern tried


@dataclass(frozen=True)
class Cmp:
    site: int
    width: int      # bits
    kind: int
    a: int
    b: int


def parse_cmps(payload: bytes) -> Tuple[int, int, List[Cmp]]:
    """A CMPS payload as (id, dropped, records). ValueError on a bad layout."""
    if len(payload) < HDR.size:
        raise ValueError("CMPS payload is %d bytes (needs %d)" % (len(payload), HDR.size))
    iid, n, dropped = HDR.unpack_from(payload)
    if len(payload) != HDR.size + n * ENTRY.size:
        raise ValueError("CMPS with %d records in %d bytes" % (n, len(payload)))
    recs = []
    for k in range(n):
        site, width, kind, a, b = ENTRY.unpack_from(payload, HDR.size + k * ENTRY.size)
        if width not in (8, 16, 32, 64):
            raise ValueError("CMPS record %d: width %d" % (k, width))
        recs.append(Cmp(site, width, kind, a, b))
    return iid, dropped, recs


def _bytelen(v: int) -> int:
    return max(1, (v.bit_length() + 7) // 8)


def _widths(x: int, y: int, nb: int) -> List[int]:
    """The compare's width in bytes, and the narrowest of 1, 2, 4 holding both values."""
    w = [nb]
    m = max(_bytelen(x), _bytelen(y))
    for c in (1, 2, 4):
        if m <= c < nb:
            w.append(c)
            break
    return w


def candidates(data: bytes, cmps: List[Cmp], limit: int = 256) -> List[bytes]:
    """Inputs derived from `data` by writing one operand of a compare where the other one
    is found in it; distinct, never `data` itself, at most `limit`."""
    out: List[bytes] = []
    seen = {data}
    for c in cmps:
        nb = c.width // 8
        mask = (1 << c.width) - 1
        # a constant of the code (b, with IMM) is never in the input as the compared value
        dirs = [(c.a, c.b)] if c.kind & IMM else [(c.a, c.b), (c.b, c.a)]
        for x, y in dirs:
            if x == y:
                continue
            ys = [y]
            if c.kind & ORDER:
                ys += [(y + 1) & mask, (y - 1) & mask]
            for w in _widths(x, y, nb):
                if x >> (8 * w):
                    continue
                for order in (("little",) if w == 1 else ("little", "big")):
                    pat = x.to_bytes(w, order)
                    if not any(pat):
                        continue                    # zeros are everywhere
                    pos, found = data.find(pat), 0
                    while pos >= 0 and found < MAX_OCCURRENCES:
                        found += 1
                        for yv in ys:
                            if yv >> (8 * w):
                                continue
                            cand = data[:pos] + yv.to_bytes(w, order) + data[pos + w:]
                            if cand not in seen:
                                seen.add(cand)
                                out.append(cand)
                                if len(out) >= limit:
                                    return out
                        pos = data.find(pat, pos + 1)
    return out


def tokens(data: bytes, cmps: List[Cmp]) -> List[Tuple[int, int]]:
    """(value, width in bytes) of the code's constants (IMM records) with two or more
    significant bytes that `data` holds in neither byte order: dictionary tokens."""
    out = []
    seen = set()
    for c in cmps:
        if not c.kind & IMM or c.b == 0:
            continue
        w = min(c.width // 8, 8)
        sig = _bytelen(c.b)
        if sig < 2:
            continue
        w = next(x for x in (2, 4, 8) if x >= sig and x <= w) if sig <= w else w
        key = (c.b, w)
        if key in seen:
            continue
        seen.add(key)
        if c.b.to_bytes(w, "little") in data or c.b.to_bytes(w, "big") in data:
            continue
        out.append(key)
    return out


def _selftest() -> int:
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    magic = 0x6A3C91E5
    data = b"Mabc" + struct.pack("<I", magic ^ 0x01000000) + b"tail"
    # a 32-bit equality of the input's little-endian word with a register holding magic
    c = Cmp(1, 32, EQ, magic ^ 0x01000000, magic)
    got = candidates(data, [c])
    check(b"Mabc" + struct.pack("<I", magic) + b"tail" in got, "the LE word replaced by magic")
    check(data not in got and len(got) == len(set(got)), "distinct, never the input itself")
    # the same compare on a big-endian core that loaded the word as is: a swapped constant
    be = Cmp(2, 32, EQ | IMM, int.from_bytes(data[4:8], "big"), int.from_bytes(struct.pack("<I", magic), "big"))
    check(b"Mabc" + struct.pack("<I", magic) + b"tail" in candidates(data, [be]), "the BE view")
    # IMM: only a -> b
    imm = Cmp(3, 32, EQ | IMM, 0x6C696174, 0x41414141)        # 'tail' as a BE word
    g = candidates(data, [imm])
    check(g == [b"Mabc" + data[4:8] + b"AAAA"], "IMM: only a is searched, b written (%r)" % g)
    # a byte compared in a 32-bit register: one byte of input
    byte = Cmp(4, 32, EQ | IMM, ord("a"), ord("Z"))
    check(candidates(b"xax", [byte]) == [b"xZx"], "a byte in a wide compare is one byte")
    # ordered: y, y + 1, y - 1
    order = Cmp(5, 16, ORDER | IMM, 0x1234, 0x2000)
    g = candidates(struct.pack("<H", 0x1234), [order])
    check(set(g) == {struct.pack("<H", v) for v in (0x2000, 0x2001, 0x1FFF)}, "ORDER: y and y +- 1")
    # zero patterns are not searched; equal operands give nothing
    check(candidates(b"\0\0\0\0", [Cmp(6, 32, EQ, 0, 5)]) == [], "zeros are not searched")
    check(candidates(data, [Cmp(7, 32, EQ, 9, 9)]) == [], "equal operands: nothing")
    # every occurrence, up to MAX_OCCURRENCES
    many = b"ab-" * 20
    g = candidates(many, [Cmp(8, 16, EQ | IMM, 0x6261, 0x7a7a)])
    check(len(g) == MAX_OCCURRENCES, "at most %d positions (%d)" % (MAX_OCCURRENCES, len(g)))
    check(len(candidates(many, [Cmp(8, 16, EQ | IMM, 0x6261, 0x7a7a)], limit=3)) == 3, "limit")
    # tokens: constants the input lacks
    t = tokens(data, [Cmp(9, 32, EQ | IMM, 1, magic), Cmp(10, 32, EQ | IMM, 1, 0x7461),
                      Cmp(11, 32, EQ | IMM, 1, 0x41), Cmp(12, 32, EQ, 1, 0xBEEF)])
    check(t == [(magic, 4)], "tokens: the missing multi-byte constants (%r)" % t)
    check(tokens(data, [Cmp(13, 32, EQ | IMM, 1, 0xBEEF)]) == [(0xBEEF, 2)], "a 16-bit token")
    # the frame
    p = HDR.pack(7, 2, 3) + ENTRY.pack(1, 32, EQ, 5, 6) + ENTRY.pack(2, 8, ORDER, 1, 2)
    iid, dropped, recs = parse_cmps(p)
    check(iid == 7 and dropped == 3 and recs == [Cmp(1, 32, EQ, 5, 6), Cmp(2, 8, ORDER, 1, 2)], "parse")
    for bad in (p[:10], p + b"x", HDR.pack(1, 1, 0) + ENTRY.pack(1, 12, EQ, 1, 2)):
        try:
            parse_cmps(bad)
            check(False, "a bad CMPS is refused")
        except ValueError:
            pass
    for f in fails:
        print("cmplog selftest: FAIL: " + f, file=sys.stderr)
    if not fails:
        print("cmplog selftest: ok")
    return 1 if fails else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(_selftest())
    print(__doc__.strip())
    sys.exit(2)
