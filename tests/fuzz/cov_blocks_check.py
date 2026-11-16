#!/usr/bin/env python3
"""cov_blocks_check.py RUN BUILD - block coverage with N-grams, ranges, splits and interrupt
contexts, and symbol names for the pc options (docs/fuzzing.md "Block coverage").

BUILD is the build directory (its firmware*/fuzztarget*.elf and firmware/irqtarget.elf).
  - On RV32, MIPS32 BE, ARM and PowerPC fuzz targets, --cov-blocks --cov-ngram 3
    --cov-rotate: the map of every input is the same, byte for byte, with blocks cut at 32
    or 2 instructions, on the interpreter and the JIT (splits do not make blocks).
  - irqtarget (RV32, a timer interrupting a parser at a period the first input byte sets):
    with --cov-irq isolate, inputs that differ only in that byte set the same buckets on
    both backends; with --cov-irq follow they do not (so the check sees the difference).
    The handler and the parser are covered (--cov-range on each).
  - --fuzz-begin / --fuzz-crash-pc / --cov-range take ELF symbols: the same verdicts as the
    numbers; SYM, SYM+LEN, SYM-SYM2, LO-HI and LO+LEN ranges; ranges narrow the map.
  - Block coverage is the default when fuzzing: the same map without --cov-blocks.
  - The errors: an unknown symbol, a symbol with a raw image, a symbol without a size,
    --cov-range or --cov-irq with a site list, --cov-scheme with block coverage.
Exit status 0 when everything holds.
"""
import os
import subprocess
import sys
import tempfile

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
    return cond


def sym(fw, name):
    out = subprocess.run(["readelf", "-sW", fw], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[7] == name:
            return int(f[1], 16)
    raise KeyError(name)


def fuzz(run, board, fw, inp, extra, cov_out=None):
    args = [run, "-b", board, "--fuzz-input", "uart0", "--fuzz-iter-insns", "2000000",
            *extra, "--fuzz-replay", inp, fw]
    if cov_out:
        args[1:1] = ["--cov-out", cov_out]
    return subprocess.run(args, capture_output=True, text=True)


def buckets(path):
    return frozenset(i for i, b in enumerate(open(path, "rb").read()) if b)


def main():
    run, build = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as tmp:
        inp = os.path.join(tmp, "in")
        out = os.path.join(tmp, "map")

        # Splits: the map does not depend on where ember cuts blocks.
        targets = [("rv32-ref", "firmware/fuzztarget.elf"), ("mips-ref", "firmware-mips/fuzztarget-be.elf"),
                   ("arm-ref", "firmware-arm/fuzztarget.elf"), ("ppc-ref", "firmware-ppc/fuzztarget.elf")]
        inputs = [b"hello", b"ok", b"Mabcdefgh", b"hello world, a longer input " * 3, b"FUZ?"]
        for board, rel in targets:
            fw = os.path.join(build, rel)
            maps = {}
            for k, data in enumerate(inputs):
                open(inp, "wb").write(data)
                for cut in ("32", "2"):
                    for jit in ([], ["--jit"]):
                        r = fuzz(run, board, fw, inp, ["--cov-blocks", "--cov-ngram", "3", "--cov-rotate",
                                                       "--block-insns", cut, *jit,
                                                       "--fuzz-begin", "fuzz_iteration",
                                                       "--fuzz-crash-pc", "fuzz_panic"], out)
                        if not check(r.returncode in (0, 20, 21), "%s: input %d: exit %d: %s"
                                     % (board, k, r.returncode, r.stderr[-300:])):
                            continue
                        maps.setdefault(k, set()).add(open(out, "rb").read())
            check(all(len(v) == 1 for v in maps.values()) and len(maps) == len(inputs),
                  "%s: the block maps depend on the block size or the backend (%s)"
                  % (board, [len(v) for v in maps.values()]))

        # Interrupt contexts: where the timer lands does not change the buckets.
        irq = os.path.join(build, "firmware/irqtarget.elf")
        payload = b"hello world 12345 the quick brown fox jumps 0987 zz"

        def sets(mode, extra=()):
            got = []
            for first in (1, 2, 3, 5, 8, 13):
                open(inp, "wb").write(bytes([first]) + payload)
                r = fuzz(run, "rv32-ref", irq, inp, ["--cov-blocks", "--cov-ngram", "3", "--cov-rotate",
                                                     "--cov-irq", mode, *extra,
                                                     "--fuzz-begin", "fuzz_iteration"], out)
                check(r.returncode == 0, "irqtarget %s: exit %d: %s" % (mode, r.returncode, r.stderr[-300:]))
                got.append(buckets(out) if r.returncode == 0 else None)
            return got

        iso = sets("isolate")
        check(len(set(iso)) == 1 and iso[0], "irqtarget: --cov-irq isolate: the buckets depend on the "
              "timer period (%s)" % [len(s or ()) for s in iso])
        check(set(sets("isolate", ["--jit"])) == {iso[0]}, "irqtarget: the JIT's buckets differ")
        check(len(set(sets("follow"))) > 1, "irqtarget: --cov-irq follow: no difference to see")
        for r in ("trap_handler", "work.constprop.0"):
            part = sets("isolate", ["--cov-range", r])
            check(len(set(part)) == 1 and part[0] and len(part[0]) < len(iso[0]),
                  "irqtarget: --cov-range %s: %s" % (r, [len(s or ()) for s in part]))

        # Symbols for the pc options: the same as the numbers.
        fw = os.path.join(build, "firmware/fuzztarget.elf")
        open(inp, "wb").write(b"FUZ?")
        by_sym = fuzz(run, "rv32-ref", fw, inp, ["--cov-blocks", "--fuzz-begin", "fuzz_iteration",
                                                 "--fuzz-crash-pc", "fuzz_panic"])
        by_num = fuzz(run, "rv32-ref", fw, inp, ["--cov-blocks", "--fuzz-begin", hex(sym(fw, "fuzz_iteration")),
                                                 "--fuzz-crash-pc", hex(sym(fw, "fuzz_panic"))])
        check(by_sym.returncode == by_num.returncode == 21 and by_sym.stdout == by_num.stdout,
              "symbols: --fuzz-begin/--fuzz-crash-pc by name differ from by number (%d, %d)"
              % (by_sym.returncode, by_num.returncode))
        it, pa = sym(fw, "fuzz_iteration"), sym(fw, "parse.constprop.0")
        open(inp, "wb").write(b"hello there")
        whole = None
        for rng in (["parse.constprop.0"], ["parse.constprop.0+16"], ["parse.constprop.0-fuzz_iteration"],
                    ["%#x-%#x" % (pa, it)], ["%#x+0x40" % pa], ["fuzz_iteration", "parse.constprop.0"]):
            r = fuzz(run, "rv32-ref", fw, inp, ["--cov-blocks", "--fuzz-begin", "fuzz_iteration",
                                                *sum((["--cov-range", x] for x in rng), [])], out)
            if check(r.returncode == 0, "--cov-range %s: exit %d: %s" % (rng, r.returncode, r.stderr[-200:])):
                b = buckets(out)
                if whole is None:
                    r0 = fuzz(run, "rv32-ref", fw, inp, ["--cov-blocks", "--fuzz-begin", "fuzz_iteration"], out)
                    whole = buckets(out) if r0.returncode == 0 else frozenset()
                check(b and len(b) < len(whole), "--cov-range %s: %d of %d buckets" % (rng, len(b), len(whole)))

        # The default: block coverage without asking for it.
        open(inp, "wb").write(b"hello there")
        a = fuzz(run, "rv32-ref", fw, inp, ["--cov-ngram", "3", "--fuzz-begin", "fuzz_iteration"], out)
        m_default = open(out, "rb").read() if a.returncode == 0 else None
        b = fuzz(run, "rv32-ref", fw, inp, ["--cov-blocks", "--cov-ngram", "3", "--fuzz-begin",
                                            "fuzz_iteration"], out)
        check(a.returncode == b.returncode == 0 and m_default == open(out, "rb").read()
              and any(m_default), "the default is not block coverage (exit %d, %d)"
              % (a.returncode, b.returncode))

        # Errors, each with a useful message.
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.run([sys.executable, os.path.join(here, "mk_lists.py"), fw, tmp],
                       check=True, capture_output=True)
        sites = os.path.join(tmp, "sites.uams")
        raw = os.path.join(build, "firmware-arm/fuzztarget.bin")
        for args, board, image, want in (
                (["--cov-blocks", "--fuzz-begin", "no_such_function"], "rv32-ref", fw, "no such symbol"),
                (["--cov-blocks", "--fuzz-begin", "fuzz_iteration"], "arm-ref", raw, "no ELF file"),
                (["--cov-blocks", "--cov-range", "fuzz_panic+", "--fuzz-begin", "fuzz_iteration"], "rv32-ref", fw,
                 "bad range end"),
                (["--cov-blocks", "--cov-range", "main-fuzz_panic", "--fuzz-begin", "fuzz_iteration"], "rv32-ref",
                 fw, "empty range"),
                (["--cov-sites", sites, "--cov-range", "main", "--fuzz-begin", "fuzz_iteration"], "rv32-ref",
                 fw, "not --cov-sites"),
                (["--cov-sites", sites, "--cov-irq", "follow", "--fuzz-begin", "fuzz_iteration"], "rv32-ref",
                 fw, "not --cov-sites"),
                (["--cov-blocks", "--cov-scheme", "pc", "--fuzz-begin", "fuzz_iteration"], "rv32-ref", fw,
                 "site lists")):
            r = fuzz(run, board, image, inp, args)
            check(r.returncode == 2 and want in r.stderr, "error %s: exit %d: %s"
                  % (args, r.returncode, r.stderr.strip()[-200:]))
        r = fuzz(run, "rv32-ref", fw, inp, ["--cov-blocks", "--cov-range", "__stack_top",
                                            "--fuzz-begin", "fuzz_iteration"])
        check(r.returncode == 2 and "no size" in r.stderr, "a symbol without a size: %s" % r.stderr[-200:])

    if failures:
        for f in failures:
            print("cov_blocks_check: FAIL: " + f, file=sys.stderr)
        return 1
    print("cov_blocks_check: block coverage by default; maps independent of block size (4 ISAs, "
          "both backends) and of interrupt timing; ranges and symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
