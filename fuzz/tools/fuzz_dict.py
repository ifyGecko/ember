#!/usr/bin/env python3
"""fuzz_dict.py -- ember dictionaries for the fuzz brain.

ember mutates inputs itself (fuzz/mutate.c, uam/fuzz_mutate.h); the brain reads the
dictionary files and hands the tokens to it in its MUTATOR frame (docs/fuzz-formats.md).

The format, one token per line:

    # comment; blank lines are ignored
    "GET /"              bytes; escapes \\\\ \\" \\t \\n \\r \\xHH
    hex:7f454c46         raw bytes
    u8:0x5a              an integer of 1, 2, 4 or 8 bytes, laid out in
    u16:0x8100           the target's byte order (`target_order`: the
    u32:0xdeadbeef       brain takes it from ember's HELLO)
    u64:0x0123456789abcdef
    u32le:0xdeadbeef     an explicit byte order, whatever the target
    u32be:0xdeadbeef     (network fields are big-endian everywhere)

Integer values may be written in any base Python's int(x, 0) reads, and negative ones
within the width's signed range. A byte token is 1..256 bytes. ember inserts and
overwrites with every token (DICT_INSERT, DICT_OVERWRITE) and writes an integer token
plus or minus 1 in its own width and order (DICT_INT_ADJ): the off-by-one neighbour of a
boundary check, which a byte string cannot express.

  python3 fuzz/tools/fuzz_dict.py --selftest
  python3 fuzz/tools/fuzz_dict.py --check FILE [--order little|big]
"""
import argparse
import struct
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Union

MAX_TOKEN = 256          # bytes in one byte token (UAM_MUT_MAX_TOKEN)


@dataclass(frozen=True)
class IntToken:
    """An integer dictionary token: `width` bytes, in `order` ("little" or "big")."""
    value: int
    width: int
    order: str

    def encode(self, value: int) -> bytes:
        return (value % (1 << (8 * self.width))).to_bytes(self.width, self.order)


Token = Union[bytes, IntToken]


@dataclass
class Dictionary:
    """Byte and integer tokens, in file order, without duplicates."""
    tokens: List[Token] = field(default_factory=list)

    def add(self, tok: Token) -> bool:
        if isinstance(tok, (bytes, bytearray)):
            tok = bytes(tok)
            if not tok or len(tok) > MAX_TOKEN:
                raise ValueError("a byte token must be 1..%d bytes" % MAX_TOKEN)
        if tok in self.tokens:
            return False
        self.tokens.append(tok)
        return True

    def merge(self, other: "Dictionary"):
        for t in other.tokens:
            self.add(t)

    @property
    def ints(self) -> List[IntToken]:
        return [t for t in self.tokens if isinstance(t, IntToken)]

    def __len__(self):
        return len(self.tokens)

    def wire(self) -> bytes:
        """The tokens as MUTATOR carries them: u32 n, then each token."""
        out = [struct.pack(">I", len(self.tokens))]
        for t in self.tokens:
            if isinstance(t, IntToken):
                out.append(struct.pack(">BBBQ", 1, t.width, t.order == "big",
                                       t.value % (1 << (8 * t.width))))
            else:
                out.append(struct.pack(">BH", 0, len(t)) + t)
        return b"".join(out)


_ESCAPES = {"\\": 0x5c, '"': 0x22, "t": 0x09, "n": 0x0a, "r": 0x0d}


def _unquote(body: str, where: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        if i + 1 >= len(body):
            raise ValueError(f"{where}: trailing backslash")
        nxt = body[i + 1]
        if nxt in _ESCAPES:
            out.append(_ESCAPES[nxt])
            i += 2
        elif nxt == "x":
            hx = body[i + 2:i + 4]
            if len(hx) != 2:
                raise ValueError(f"{where}: truncated \\x escape")
            try:
                out.append(int(hx, 16))
            except ValueError:
                raise ValueError(f"{where}: bad \\x escape") from None
            i += 4
        else:
            raise ValueError(f"{where}: unknown escape \\{nxt}")
    return bytes(out)


def parse_dict(path, target_order: str = "little") -> Dictionary:
    """Read an ember dictionary file. `target_order` ("little" or "big") lays out the
    integer tokens that do not name a byte order. Errors raise ValueError naming the file
    and line."""
    if target_order not in ("little", "big"):
        raise ValueError(f"target_order must be 'little' or 'big', not {target_order!r}")
    d = Dictionary()
    p = Path(path)
    with p.open("rb") as fh:
        for lineno, raw in enumerate(fh, 1):
            where = f"{p}:{lineno}"
            try:
                line = raw.decode("utf-8", "strict").strip()
            except UnicodeDecodeError as e:
                raise ValueError(f"{where}: invalid utf-8: {e}") from None
            if not line or line.startswith("#"):
                continue
            if line.startswith('"'):
                if len(line) < 2 or not line.endswith('"'):
                    raise ValueError(f"{where}: unterminated string")
                frag = _unquote(line[1:-1], where)
            else:
                kind, sep, val = line.partition(":")
                if not sep or not val.strip():
                    raise ValueError(f"{where}: expected \"text\", hex:BYTES or uN[le|be]:VALUE")
                kind, val = kind.strip(), val.strip()
                if kind == "hex":
                    try:
                        frag = bytes.fromhex(val)
                    except ValueError:
                        raise ValueError(f"{where}: bad hex bytes") from None
                else:
                    order, base = target_order, kind
                    for suffix, o in (("le", "little"), ("be", "big")):
                        if kind.endswith(suffix):
                            base, order = kind[:-2], o
                    widths = {"u8": 1, "u16": 2, "u32": 4, "u64": 8}
                    if base not in widths:
                        raise ValueError(f"{where}: unknown token kind {kind!r}")
                    width = widths[base]
                    try:
                        v = int(val, 0)
                    except ValueError:
                        raise ValueError(f"{where}: bad integer {val!r}") from None
                    if not -(1 << (8 * width - 1)) <= v < (1 << (8 * width)):
                        raise ValueError(f"{where}: {val} does not fit {width * 8} bits")
                    d.add(IntToken(v % (1 << (8 * width)), width, order))
                    continue
            if not frag:
                raise ValueError(f"{where}: empty token")
            if len(frag) > MAX_TOKEN:
                raise ValueError(f"{where}: a token is at most {MAX_TOKEN} bytes")
            d.add(frag)
    return d


def _selftest() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / "t.dict"
        p.write_text('# c\n\n"GET \\x2f"\nhex:7f45\nu16:0x8100\nu32be:-1\nu8le:5\n"GET /"\n')
        d = parse_dict(p, "little")
        assert d.tokens == [b"GET /", b"\x7f\x45", IntToken(0x8100, 2, "little"),
                            IntToken(0xffffffff, 4, "big"), IntToken(5, 1, "little")], d.tokens
        assert len(d.ints) == 3
        assert parse_dict(p, "big").tokens[2] == IntToken(0x8100, 2, "big")
        w = d.wire()
        assert w[:4] == b"\0\0\0\5" and w[4:12] == b"\0\0\5GET /", w[:12]
        assert b"\1\4\1" + b"\0\0\0\0\xff\xff\xff\xff" in w
        for bad in ('"x', "u24:1", "u8:256", "u8:-129", "hex:zz", '""', "x:1", '"\\q"',
                    '"%s"' % ("a" * 257)):
            p.write_text(bad + "\n")
            try:
                parse_dict(p)
            except ValueError:
                continue
            raise AssertionError("accepted %r" % bad)
    print("fuzz_dict: selftest ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--check", metavar="FILE", help="parse FILE and list its tokens")
    ap.add_argument("--order", default="little", choices=("little", "big"))
    a = ap.parse_args()
    if a.selftest:
        return _selftest()
    if not a.check:
        ap.error("--selftest or --check FILE")
    try:
        d = parse_dict(a.check, a.order)
    except (OSError, ValueError) as e:
        print("fuzz_dict: %s" % e, file=sys.stderr)
        return 1
    for t in d.tokens:
        print("u%d%s:0x%x" % (8 * t.width, t.order[0] + "e", t.value) if isinstance(t, IntToken)
              else "hex:" + t.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
