#!/usr/bin/env python3
"""basefind.py - find the load (base) address of a raw firmware blob. Self-contained, stdlib only.

Method: string-pointer voting.
  * Collect the file offsets of NUL-terminated printable strings.
  * Collect every aligned pointer-sized word in the blob.
  * If the image is loaded at base B, then many words equal B + (offset of some string).
    For each DISTINCT word value v and string offset s with matching low bits, vote for
    B = v - s. Counting distinct values (not occurrences) stops frequently repeated
    instruction words from swamping the vote: a real base is supported by many different
    pointers to many different strings.
  * Instruction words can look like pointers (e.g. MIPS `addiu` = 0x24xxxxxx) and give
    runner-up bases a few hundred random hits. So the top candidates are re-ranked by
    "paired hits": adjacent words that BOTH resolve to strings. Real pointer tables
    (message tables, option tables, ops structs) produce many pairs; noise produces ~0.

Decisive evidence = a winner with many paired hits while the runner-up has few/none,
plus sample pointers that resolve to real strings (both printed).

usage:
  basefind.py BLOB [--endian auto|be|le] [--word 2|4|8] [--align 0x1000]
              [--min 0x0] [--max 0xfffff000] [--min-str 6] [--top 10] [--samples 5] [--json]

--word 2 is for 16-bit address spaces (6502, Z80, 8051 code banks): pointer tables of 16-bit
words in the compiler's byte order (6502 and Z80 little-endian; 8051 Keil C51 big-endian,
SDCC little-endian; --endian auto tries both). Short words collide far
more often, so take --align 0x100 and trust only a clear paired-hit lead. Harvard parts
whose strings are reached through LPM/RETLW/MOVC (AVR, PIC, most 8051 code) rarely keep
plain string pointers at all; their program memory starts at 0 anyway.

Exit status 0 if a candidate with >0 paired hits was found, 1 otherwise.
"""
import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict


def find_strings(data, min_len):
    """File offsets of NUL-terminated printable ASCII strings (start offsets)."""
    pat = re.compile(rb"[\x20-\x7e\t\r\n]{%d,}\x00" % min_len)
    return [m.start() for m in pat.finditer(data)]


def phases(size):
    """Byte offsets the words are read at: aligned, and for 16-bit words (8-bit CPUs keep
    pointer tables at any address) the odd offset too."""
    return (0, 1) if size == 2 else (0,)


def word_list(data, endian, size, phase=0):
    fmt = (">" if endian == "be" else "<") + {2: "H", 4: "I", 8: "Q"}[size]
    n = (len(data) - phase) // size
    return struct.unpack_from(fmt[0] + fmt[1] * n, data, phase) if n > 0 else ()


def words(data, endian, size):
    c = Counter()
    for ph in phases(size):
        c.update(word_list(data, endian, size, ph))
    return c


def vote(data, endian, size, align, lo, hi, str_offs):
    mask = align - 1
    buckets = defaultdict(list)
    for s in str_offs:
        buckets[s & mask].append(s)
    votes = Counter()
    for v in words(data, endian, size):
        if v < lo:
            continue
        for s in buckets.get(v & mask, ()):
            b = v - s
            if lo <= b <= hi:
                votes[b] += 1
    return votes


def paired_hits(data, endian, size, base, str_set):
    """Number of adjacent word pairs that both point at string starts under `base`."""
    pairs = 0
    for ph in phases(size):
        prev = False
        for w in word_list(data, endian, size, ph):
            cur = (w - base) in str_set
            if cur and prev:
                pairs += 1
            prev = cur
    return pairs


def samples(data, endian, size, base, str_set, n):
    fmt = (">" if endian == "be" else "<") + {2: "H", 4: "I", 8: "Q"}[size]
    out = []
    for off in range(0, len(data) - size + 1, 1 if size == 2 else size):
        v = struct.unpack_from(fmt, data, off)[0]
        s = v - base
        if s in str_set:
            end = data.index(b"\0", s)
            out.append((off, v, data[s:end][:60].decode("ascii", "replace")))
            if len(out) >= n:
                break
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="string-pointer voting base-address finder")
    ap.add_argument("blob")
    ap.add_argument("--endian", default="auto", choices=["auto", "be", "le"])
    ap.add_argument("--word", type=int, default=4, choices=[2, 4, 8],
                    help="pointer size in bytes (2: 16-bit address spaces)")
    ap.add_argument("--align", type=lambda x: int(x, 0), default=0x1000,
                    help="candidate base alignment (default 0x1000; try 0x100 if nothing wins)")
    ap.add_argument("--min", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--max", type=lambda x: int(x, 0), default=None)
    ap.add_argument("--min-str", type=int, default=6)
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--rerank", type=int, default=20,
                    help="re-rank this many top vote-getters by paired hits")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)

    data = open(a.blob, "rb").read()
    hi = a.max if a.max is not None else (1 << (8 * a.word)) - a.align
    str_offs = find_strings(data, a.min_str)
    if not str_offs:
        print("no strings found (compressed/encrypted blob? decompress first)", file=sys.stderr)
        return 1

    endians = ["be", "le"] if a.endian == "auto" else [a.endian]
    str_set = set(str_offs)
    results = {}
    for e in endians:
        v = vote(data, e, a.word, a.align, a.min, hi, str_offs)
        cands = [(b, h, paired_hits(data, e, a.word, b, str_set))
                 for b, h in v.most_common(max(a.rerank, a.top))]
        cands.sort(key=lambda t: (t[2], t[1]), reverse=True)
        results[e] = cands[:a.top]

    def best(e):
        return (results[e][0][2], results[e][0][1]) if results[e] else (0, 0)
    endian = max(endians, key=best)
    ranked = results[endian]

    report = {
        "blob": a.blob, "size": len(data), "strings": len(str_offs),
        "endian": endian, "align": a.align,
        "per_endian_best": {e: (hex(results[e][0][0]), results[e][0][1], results[e][0][2])
                            if results[e] else None for e in endians},
        "candidates": [(hex(b), h, p) for b, h, p in ranked],
    }
    if ranked:
        b0, h0, p0 = ranked[0]
        p1 = ranked[1][2] if len(ranked) > 1 else 0
        report["winner"] = hex(b0)
        report["hits"] = h0
        report["paired"] = p0
        report["runner_up_paired"] = p1
        report["separation"] = round(p0 / p1, 2) if p1 else None
        report["samples"] = [(hex(o), hex(v), s)
                             for o, v, s in samples(data, endian, a.word, b0, str_set, a.samples)]

    if a.json:
        print(json.dumps(report, indent=2))
    else:
        print("blob: %s (%d bytes, %d strings >= %d chars)" % (a.blob, len(data), len(str_offs),
                                                               a.min_str))
        for e in endians:
            pb = report["per_endian_best"][e]
            print("  best %s: %s" % (e, "%s (%d hits, %d paired)" % pb if pb else "none"))
        print("endian: %s   alignment: %#x" % (endian, a.align))
        print("rank  base        ptr hits  paired hits")
        for i, (b, h, p) in enumerate(ranked):
            print("%4d  0x%08x  %8d  %11d%s" % (i + 1, b, h, p, "   <-- winner" if i == 0 else ""))
        if ranked:
            sep = report["separation"]
            print("paired-hit separation (winner / runner-up): %s"
                  % (sep if sep else "inf (runner-up has 0 paired hits)"))
            print("spot checks (file offset: word -> string at word-base):")
            for o, v, s in report["samples"]:
                print("  %s: %s -> %r" % (o, v, s))
            if report["hits"] == 0 or report["paired"] == 0:
                print("WARNING: no paired hits - base NOT corroborated. Check that the blob is "
                      "decompressed code+data, try --align 0x100, --word 8 for 64-bit or "
                      "--word 2 for a 16-bit address space.")
            elif sep is not None and sep < 3:
                print("WARNING: weak separation (<3x) - corroborate with a second method "
                      "(loader handoff address, reset vector, absolute jumps).")
            print("image would span 0x%08x - 0x%08x" % (ranked[0][0], ranked[0][0] + len(data)))
            if ranked[0][0] + len(data) > 1 << (8 * a.word):
                print("WARNING: the image would not fit the %d-bit address space at this base"
                      % (8 * a.word))
    return 0 if ranked and ranked[0][2] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
