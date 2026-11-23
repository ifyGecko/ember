#!/usr/bin/env python3
"""frontier_unit.py - unit tests of fuzz/tools/fuzz_frontier.py's analysis, on a synthetic
CFG and trace: no rizin, no ember.

  - a block ember ran through without a trace line of its own (a loop head or a join that
    the block before it falls into) counts as run, and its branch is a frontier candidate;
  - a conditional branch reports only the target that never ran;
  - a switch reports each case that never ran, with its value;
  - the smallest input reaching the branch is named;
  - the trace parser attributes blocks to `trace: input N`; --range parses SYM (and its clones), LO-HI, LO+LEN.
"""
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fuzz", "tools"))
import fuzz_frontier as ff  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


def blk(addr, size, jump=None, fail=None, cases=()):
    return ff.Block(addr, size, jump, fail, list(cases), "f", [(addr, "insn")])


def test_fall_into_join():
    # 0x100 (no branch) falls into the loop head 0x110, whose branch goes back to 0x110
    # or on to 0x120; 0x120 branches to 0x130 or 0x140. ember's block from 0x100 ran
    # through 0x110 on its one pass, so the trace has 0x100 and 0x120 but no 0x110.
    blocks = [blk(0x100, 0x10, jump=0x110), blk(0x110, 0x10, jump=0x110, fail=0x120),
              blk(0x120, 0x8, jump=0x140, fail=0x130), blk(0x130, 0x4), blk(0x140, 0x4)]
    cov = ff.covered_blocks(blocks, [0x100, 0x120, 0x130])
    check(cov == {0x100, 0x110, 0x120, 0x130}, "the loop head counts as run: %r" % sorted(cov))
    recs = ff.frontier_records(blocks, cov, lambda b: [(3, "a")], [])
    got = sorted((r["branch"], r["edge"], r["target"]) for r in recs)
    check(got == [(0x120, "taken", 0x140)],
          "only the target that never ran is a frontier: %r" % got)
    check(recs and recs[0]["smallest_input"] == "a", "the smallest input is named")
    # Without the fall-through rule the loop head would be missed: a block whose range has
    # no traced pc and that nothing flows into stays uncovered.
    cov2 = ff.covered_blocks(blocks, [0x120])
    check(0x110 not in cov2 and 0x100 not in cov2, "no flow, no coverage: %r" % sorted(cov2))


def test_switch():
    blocks = [blk(0x200, 0x8, cases=[(0, 0x210), (1, 0x220), (2, 0x230)]),
              blk(0x210, 0x4), blk(0x220, 0x4), blk(0x230, 0x4)]
    cov = ff.covered_blocks(blocks, [0x200, 0x220])
    check(cov == {0x200, 0x220}, "a switch is not a fall-through: %r" % sorted(cov))
    recs = ff.frontier_records(blocks, cov, lambda b: [], [])
    got = sorted((r["edge"], r.get("case_value"), r["target"]) for r in recs)
    check(got == [("case 0", 0, 0x210), ("case 2", 2, 0x230)], "uncovered cases: %r" % got)


def test_parse_trace():
    with tempfile.NamedTemporaryFile("w", suffix=".trace", delete=False) as f:
        f.write("blk 0x10 icount=1\ntrace: the boot to the begin pc ends\n"
                "trace: input 1\nblk 0x100 icount=5\ntrace: input 2\nblk 0x120 icount=9\n")
    try:
        covered, per = ff.parse_trace(f.name, 2)
    finally:
        os.unlink(f.name)
    check(covered == {0x10, 0x100, 0x120}, "covered pcs")
    check(per[0] == {0x10} and per[1] == {0x100} and per[2] == {0x120},
          "per-input attribution: %r" % per)


def test_ranges():
    syms = {"parse.constprop.0": (0x1000, 0x40), "parse_x": (0x2000, 4)}
    check(ff.parse_ranges(["parse"], syms) == [(0x1000, 0x1040)], "a clone matches SYM")
    check(ff.parse_ranges(["0x10-0x20", "0x30+0x8"], {}) == [(0x10, 0x20), (0x30, 0x38)],
          "LO-HI and LO+LEN")


for t in (test_fall_into_join, test_switch, test_parse_trace, test_ranges):
    t()
if failures:
    for f in failures:
        print("frontier_unit: FAIL: " + f)
    sys.exit(1)
print("frontier_unit: OK")
