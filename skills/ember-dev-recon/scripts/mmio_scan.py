#!/usr/bin/env python3
"""mmio_scan.py - list device-register accesses in a raw MIPS32 blob. Stdlib only, no Ghidra.

MIPS code reaches a register with `lui rX, HI` followed by `lw/sw rY, OFF(rX)` (or
`addiu/ori rX, rX, LO` and then a load/store). This linear sweep tracks the HI value
held in each register and reconstructs the absolute address of every load/store that
uses it. Addresses in the chosen window (default kseg1 0xA0000000-0xBFFFFFFF, excluding
the kseg1 alias of RAM) are grouped by device page and reported with:
  * total accesses, reads, writes, address-formations (lui+addiu/ori without a load/store)
  * the register offsets touched within the page, with counts
  * the code addresses of the first few accesses (needs --base to print real VAs)

It also finds **polling loops**: an MMIO load whose value (optionally masked with
and/andi) feeds a conditional branch back to at or before the load. Each one is a register
the emulated device must eventually satisfy ("waits until (reg & mask) != 0"). These are
the first blockers of a bring-up, so recon should hand them over explicitly. A poll is
proof that the code *depends* on a register; an access count alone is not.

This is a heuristic (no control flow), but it is exactly the table the build phase needs:
which device pages exist, which registers inside them are touched, read vs write, the
highest offset seen (minimum window size), and what the code waits for.

Run it on EVERY code stage at its run address, including an XIP bootstrap at the reset
vector (e.g. --base 0xBFC00000 on the first flash bytes): early stages program clocks,
DRAM and on-chip SRAM that later stages never touch.

usage:
  mmio_scan.py BLOB [--endian be|le] [--base 0x80010000] [--page 0x10000]
               [--window 0xA0000000-0xBFFFFFFF] [--ram-size 0x10000000] [--top 40]
               [--no-polls] [--limit BYTES]
"""
import argparse
import struct
import sys
from collections import Counter, defaultdict

LOADS = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr"}
STORES = {0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr"}


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def decode(data, endian):
    n = len(data) // 4
    return struct.unpack_from(("<" if endian == "le" else ">") + "%dI" % n, data, 0)


def scan(data, endian, base, loads=None):
    """Reconstruct absolute addresses of loads/stores/address formations.

    If `loads` is a list, (index, address, dest_reg) of every reconstructed load is
    appended to it (used by find_polls)."""
    ws = decode(data, endian)
    hi = {}
    acc = []                                   # (address, kind 'R'/'W'/'A', code VA)
    for i, w in enumerate(ws):
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        imm = w & 0xFFFF
        pc = base + 4 * i
        if op == 0x0F:                                         # lui
            hi[rt] = imm << 16
        elif op in (0x09, 0x0D):                               # addiu / ori
            if rs in hi:
                full = (hi[rs] + s16(imm)) & 0xFFFFFFFF if op == 0x09 else hi[rs] | imm
                acc.append((full, "A", pc))
                hi[rt] = full
            else:
                hi.pop(rt, None)
        elif op in LOADS or op in STORES:
            if rs in hi:
                addr = (hi[rs] + s16(imm)) & 0xFFFFFFFF
                acc.append((addr, "R" if op in LOADS else "W", pc))
                if op in LOADS and loads is not None:
                    loads.append((i, addr, rt))
            if op in LOADS:
                hi.pop(rt, None)
        elif 0x08 <= op <= 0x0E:                               # other I-type ALU -> rt
            hi.pop(rt, None)
        elif op == 0:
            if (w & 0x3F) in (0x08, 0x09):                     # jr / jalr: function boundary
                hi.clear()
            else:
                hi.pop(rd, None)
        elif op == 0x03:                                       # jal clobbers caller-saved
            for r in list(hi):
                if r < 16 or r in (24, 25, 31):
                    hi.pop(r)
    return acc


def _const_before(ws, i, reg, back=6):
    """Value of `reg` if set by lui/li/ori/addiu from $zero in the few instructions before i."""
    for j in range(i - 1, max(-1, i - 1 - back), -1):
        w = ws[j]
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if rt != reg:
            continue
        if op == 0x0F:
            return imm << 16
        if op in (0x09, 0x0D) and rs == 0:
            return s16(imm) & 0xFFFFFFFF if op == 0x09 else imm
        return None
    return None


def find_polls(ws, base, loads, keep, ahead=8, reach=0x100):
    """MMIO loads whose (masked) value decides a backward branch to at/before the load.

    Returns (address, mask, op, cmp, load_pc, branch_pc); the loop continues while
    `(value & mask) <op> cmp` holds, i.e. the code waits for it to become false."""
    polls = []
    for i, addr, dest in loads:
        if not keep(addr):
            continue
        val = {dest}
        mask = None
        consts = {0: 0}
        for j in range(i + 1, min(len(ws), i + 1 + ahead)):
            w = ws[j]
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xFFFF
            fn = w & 0x3F
            if op == 0x0F:                                         # lui const
                consts[rt] = imm << 16
                val.discard(rt)
            elif op in (0x09, 0x0D) and rs in consts:              # li / ori const
                consts[rt] = ((consts[rs] + s16(imm)) if op == 0x09 else (consts[rs] | imm)) & 0xFFFFFFFF
                val.discard(rt)
            elif op == 0x0C and rs in val:                         # andi
                mask = imm
                val.add(rt)
            elif op == 0 and fn == 0x24 and (rs in val or rt in val):   # and
                other = rt if rs in val else rs
                m = consts.get(other, _const_before(ws, i, other))
                mask = m if m is not None else mask
                val.add(rd)
            elif op in (0x04, 0x05) and (rs in val or rt in val):  # beq / bne
                other = rt if rs in val else rs
                cmp = consts.get(other, _const_before(ws, i, other))
                target = 4 * (j + 1) + 4 * s16(imm)
                if 0 <= 4 * i - target <= reach:
                    polls.append((addr, mask, "==" if op == 0x04 else "!=", cmp,
                                  base + 4 * i, base + 4 * j))
                break
            elif op == 0x01 and rs in val and rt in (0, 1):        # bltz / bgez: bit 31
                target = 4 * (j + 1) + 4 * s16(imm)
                if 0 <= 4 * i - target <= reach:
                    polls.append((addr, 0x80000000, "!=" if rt == 0 else "==", 0,
                                  base + 4 * i, base + 4 * j))
                break
            elif op in (0x02, 0x03) or (op == 0 and fn in (0x08, 0x09)):
                break                                              # left the loop body
            elif op == 0 and rd in val:
                val.discard(rd)
            elif op not in (0x2B, 0x28, 0x29) and rt in val and op != 0:
                val.discard(rt)
    return polls


def main(argv=None):
    ap = argparse.ArgumentParser(description="MIPS32 raw-blob MMIO access scanner")
    ap.add_argument("blob")
    ap.add_argument("--endian", default="be", choices=["be", "le"])
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0,
                    help="load address (only used to print code addresses)")
    ap.add_argument("--page", type=lambda x: int(x, 0), default=0x10000)
    ap.add_argument("--window", default="0xA0000000-0xBFFFFFFF")
    ap.add_argument("--ram-size", type=lambda x: int(x, 0), default=0x10000000,
                    help="kseg1 addresses whose physical address is below this are RAM, not MMIO")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--offsets", type=int, default=12, help="offsets listed per page")
    ap.add_argument("--no-polls", action="store_true", help="skip the polling-loop table")
    ap.add_argument("--limit", type=lambda x: int(x, 0), default=0,
                    help="only scan the first N bytes (e.g. the bootstrap part of a flash dump)")
    a = ap.parse_args(argv)

    lo, hi = (int(x, 0) for x in a.window.split("-"))
    data = open(a.blob, "rb").read()
    if a.limit:
        data = data[:a.limit]

    def keep(addr):
        if not (lo <= addr <= hi):
            return False
        return not ((addr & 0x1FFFFFFF) < a.ram_size and 0x80000000 <= addr <= 0xBFFFFFFF)

    pages = defaultdict(lambda: {"R": 0, "W": 0, "A": 0, "offs": Counter(), "sites": []})
    loads = []
    for addr, kind, pc in scan(data, a.endian, a.base, loads):
        if not keep(addr):
            continue                                           # outside window / RAM alias
        p = pages[addr & ~(a.page - 1)]
        p[kind] += 1
        if kind != "A":
            p["offs"][addr & (a.page - 1)] += 1
            if len(p["sites"]) < 4:
                p["sites"].append(pc)

    ranked = sorted(pages.items(), key=lambda kv: kv[1]["R"] + kv[1]["W"], reverse=True)
    print("page_base   phys        total  R     W     A     max_off  offsets (off:count)")
    for page, p in ranked[:a.top]:
        tot = p["R"] + p["W"]
        if tot == 0:
            continue
        offs = " ".join("+0x%x:%d" % (o, c) for o, c in sorted(p["offs"].most_common(a.offsets)))
        print("0x%08x  0x%08x  %5d  %-5d %-5d %-5d +0x%-5x %s" % (
            page, page & 0x1FFFFFFF, tot, p["R"], p["W"], p["A"], max(p["offs"]), offs))
        if a.base:
            print("            first sites: %s" % " ".join("0x%08x" % s for s in p["sites"]))
    formed_only = [pg for pg, p in ranked if p["R"] + p["W"] == 0]
    if formed_only:
        print("\naddress formed but never dereferenced directly (constants, base pointers kept in "
              "structs, or KSEG conversions): %s"
              % " ".join("0x%08x" % pg for pg in formed_only[:20]))

    if not a.no_polls:
        polls = find_polls(decode(data, a.endian), a.base, loads, keep)
        print("\npolling loops (load -> [mask] -> backward branch): the code spins while the "
              "condition holds,\nso the emulated register must eventually make it false. "
              "POLL = single status bit;\nLOOPCMP = multi-bit compare inside a loop (often a "
              "chip-ID/mode check in a retry loop: read the code)")
        if not polls:
            print("  (none found by the linear heuristic; also look for wait helpers taking "
                  "(reg, mask, value, timeout))")
        seen = set()
        for addr, mask, op, cmp, lpc, bpc in polls:
            key = (addr, mask, op, cmp)
            if key in seen:
                continue
            seen.add(key)
            m = "0x%x" % mask if mask is not None else "?"
            c = "0x%x" % cmp if cmp is not None else "reg"
            single_bit = mask is not None and mask & (mask - 1) == 0
            status = (single_bit and cmp in (0, mask)) or (mask is None and cmp == 0)
            tag = "POLL   " if status else "LOOPCMP"     # LOOPCMP: ID/mode compare in a loop
            print("  %s phys 0x%08x  spins while (v & %s) %s %s   load@0x%08x branch@0x%08x"
                  % (tag, addr & 0x1FFFFFFF, m, op, c, lpc, bpc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
