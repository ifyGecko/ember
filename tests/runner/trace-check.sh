#!/bin/sh
# trace-check.sh RUNNER RV32_SELFTEST_ELF RV32_SMPTEST_ELF RV32_FUZZTARGET_ELF - checks of
# uam-run's traces on rv32-ref:
#   default      a run without --trace prints none of the new trace lines
#   mmio, sysreg, irq, console
#                on the RISC-V self-test: the lines are there (the UART's LSR read and THR
#                writes, the first write of mtvec, timer line 7 rising, the banner) and every
#                one is identical under the interpreter, --jit and --jit --no-chain
#   --trace-mmio-range, --trace-after  narrow the mmio trace
#   catch-alls   the unknown-mmio first-access line names the instruction (pc, icount);
#                --log-stamp prefixes log lines with the icount
#   exc          exceptions and interrupt entries name the kind and carry the icount, and
#                --trace-after holds them back too
#   sysreg       with --trace-after, first accesses and the summary count from there
#   names        an unknown trace name is an error (exit 2), not a run that traces nothing
#   smp          block lines of processors other than 0 say which processor
#   replay       --fuzz-replay with traces: the boot's section, then one per input
set -eu
RUN=$1
FW=$2
SMP=$3
FUZZ=$4
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "trace-check: FAIL: $*" >&2; exit 1; }
R="$RUN --raw-base 0x80000000"

# ---- default: quiet --------------------------------------------------------------
$RUN "$FW" </dev/null >"$T/out" 2>"$T/err" || fail "self-test failed without traces"
grep -q 'SELFTEST PASS' "$T/out" || fail "self-test did not pass"
if grep -Eq '^(mmio|sysreg|irq-in|irq|exc|reset|console|trace):|^blk ' "$T/err"; then
    fail "trace lines printed without --trace"
fi

# ---- mmio, sysreg, irq, console: present and the same on every backend ----------------
for mode in interp jit nochain; do
    case $mode in interp) a="";; jit) a="--jit";; nochain) a="--jit --no-chain";; esac
    # shellcheck disable=SC2086
    $RUN $a -t mmio,sysreg,irq,console "$FW" </dev/null >/dev/null 2>"$T/$mode" ||
        fail "$mode: traced self-test failed"
    grep -v '^uam-run:' "$T/$mode" >"$T/$mode.tr"
done
cmp -s "$T/interp.tr" "$T/jit.tr" || { diff "$T/interp.tr" "$T/jit.tr" | head >&2; fail "traces differ: interpreter vs --jit"; }
cmp -s "$T/interp.tr" "$T/nochain.tr" || fail "traces differ: interpreter vs --jit --no-chain"
tr="$T/interp.tr"
grep -Eq '^mmio: r 1 0x10000005 uart0\+0x5 = 0x60 pc=0x[0-9a-f]+ icount=[0-9]+$' "$tr" || fail "no UART LSR read in the mmio trace"
grep -Eq '^mmio: w 1 0x10000000 uart0\+0x0 = 0x55 pc=0x[0-9a-f]+ icount=[0-9]+$' "$tr" || fail "no 'U' written to THR"
grep -Eq '^sysreg: first write 0x305 <- 0x[0-9a-f]+ pc=0x[0-9a-f]+ icount=' "$tr" || fail "no first write of mtvec"
grep -Eq '^sysreg: first write 0xc00 <- .*system register exception' "$tr" || fail "faulting CSR write not reported"
grep -Eq '^sysreg:   0x300: [0-9]+ reads, [0-9]+ writes' "$tr" || fail "no sysreg summary"
grep -Eq '^irq-in: cpu0 line 7 = 1 icount=[0-9]+ ' "$tr" || fail "timer line rise not traced"
grep -Eq '^irq-in: cpu0 line 7: [1-9][0-9]* rises, [1-9][0-9]* falls, level 0 at the stop$' "$tr" || fail "no irq line summary"
grep -Eq '^console: icount=[0-9]+ pc=0x[0-9a-f]+ "UAM self-test \(rv32imac, rv32-ref\)"$' "$tr" || fail "banner not in the console trace"
# the first console byte is the THR write the mmio trace shows at the same icount
ci=$(sed -n 's/^console: icount=\([0-9]*\) .*"UAM self-test.*/\1/p' "$tr")
grep -q "^mmio: w 1 0x10000000 uart0+0x0 = 0x55 pc=0x[0-9a-f]* icount=$ci\$" "$tr" ||
    fail "console line's icount ($ci) is not its first byte's"

# ---- narrowing --------------------------------------------------------------------
$RUN -t mmio --trace-mmio-range 0x10000005:0x10000006 "$FW" </dev/null >/dev/null 2>"$T/rng" || true
grep -q '^mmio: r 1 0x10000005' "$T/rng" || fail "--trace-mmio-range dropped the range"
if grep '^mmio:' "$T/rng" | grep -vq ' 0x10000005 '; then fail "--trace-mmio-range let other addresses through"; fi
$RUN -t mmio --trace-after 2000 "$FW" </dev/null >/dev/null 2>"$T/aft" || true
first=$(sed -n 's/^mmio: .* icount=\([0-9]*\)$/\1/p' "$T/aft" | head -1)
[ -n "$first" ] && [ "$first" -ge 2000 ] || fail "--trace-after: mmio line at icount $first"

# lui t0,0x20000; 1: lw t1,20(t0); andi t1,t1,0x20; beqz t1,1b; 2: j 2b
printf '\267\002\000\040\003\243\102\001\023\163\003\002\343\014\003\376\157\000\000\000' >"$T/bit.bin"

# ---- catch-all lines name the instruction; --log-stamp ---------------------------------
$R --unknown-mmio zero -n 50 "$T/bit.bin" </dev/null >/dev/null 2>"$T/u" || true
grep -q '^\[warn\] unknown-mmio: first read of 4 bytes at 0x20000014 pc=0x80000004 icount=2$' "$T/u" ||
    fail "unknown-mmio first access without its instruction"
$R --log-stamp --unknown-mmio zero -n 50 "$T/bit.bin" </dev/null >/dev/null 2>"$T/u" || true
grep -q '^@2 \[warn\] unknown-mmio: first read' "$T/u" || fail "--log-stamp: no icount prefix"

# ---- exc: names, icount, --trace-after ----------------------------------------------
$RUN -t exc "$FW" </dev/null >/dev/null 2>"$T/exc" || fail "self-test failed with -t exc"
grep -Eq '^exc: kind=0 \(trap\) pc=0x[0-9a-f]+ cause=0x[0-9a-f]+ tval=0x[0-9a-f]+ icount=[0-9]+$' "$T/exc" ||
    fail "exc line without its kind name or icount"
grep -Eq '^exc: kind=6 \(sysreg\) .* icount=[0-9]+$' "$T/exc" || fail "no faulting CSR access in the exc trace"
grep -Eq '^irq: entered handler at 0x[0-9a-f]+ from pc=0x[0-9a-f]+ lines=0x80 icount=[0-9]+$' "$T/exc" ||
    fail "interrupt entry without its origin, lines or icount"
last=$(sed -n 's/^exc: .* icount=\([0-9]*\)$/\1/p' "$T/exc" | tail -1)
$RUN -t exc --trace-after "$last" "$FW" </dev/null >/dev/null 2>"$T/exc2" || true
lo=$(sed -n 's/^\(exc\|irq\): .* icount=\([0-9]*\)$/\2/p' "$T/exc2" | sort -n | head -1)
[ -n "$lo" ] && [ "$lo" -ge "$last" ] || fail "--trace-after $last: an exc/irq line at icount $lo"

# ---- sysreg: the window decides what is first ---------------------------------------
$RUN -t sysreg --trace-after 3000 "$FW" </dev/null >/dev/null 2>"$T/sr" || true
if sed -n 's/^sysreg: first .* icount=\([0-9]*\)$/\1/p' "$T/sr" | awk '$1 < 3000 { bad = 1 } END { exit !bad }'; then
    fail "--trace-after 3000: a sysreg line from before the window"
fi
grep -Eq '^sysreg: first (read|write) ' "$T/sr" || fail "no first accesses in the --trace-after window"

# ---- an unknown trace name stops the run before it starts ----------------------------
rc=0; $RUN -t mmio,nosuch "$FW" </dev/null >/dev/null 2>"$T/bad" || rc=$?
[ "$rc" = 2 ] || fail "-t nosuch: exit $rc, not 2"
grep -q "unknown trace 'nosuch'" "$T/bad" || fail "-t nosuch: not named"

# ---- smp: which processor ran a block ------------------------------------------------
$RUN -o cpus=2 -t blocks -n 20000 "$SMP" </dev/null >/dev/null 2>"$T/smp" || true
grep -Eq '^blk 0x[0-9a-f]+ icount=[0-9]+ cpu=1$' "$T/smp" || fail "no block line marked cpu=1"
grep -Eq '^blk 0x[0-9a-f]+ icount=[0-9]+$' "$T/smp" || fail "no block line of processor 0"

# ---- replay: one section per input ---------------------------------------------------
mkdir "$T/in"
printf 'hello' >"$T/in/1"
printf 'again' >"$T/in/2"
begin=$(readelf -sW "$FUZZ" | awk '$NF == "fuzz_iteration" { print "0x" $2; exit }')
$RUN --fuzz-begin "$begin" --fuzz-input uart0 -t sysreg --fuzz-replay "$T/in" "$FUZZ" </dev/null \
    >/dev/null 2>"$T/rp" || fail "replay with traces failed"
grep '^trace: ' "$T/rp" >"$T/rp.s"
printf 'trace: the boot to the begin pc ends\ntrace: input 1\ntrace: input 2\n' >"$T/rp.want"
cmp -s "$T/rp.s" "$T/rp.want" || { cat "$T/rp.s" >&2; fail "replay: trace sections are not boot, input 1, input 2"; }

echo "trace-check: ok"
