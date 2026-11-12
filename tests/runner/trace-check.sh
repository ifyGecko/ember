#!/bin/sh
# trace-check.sh RUNNER RV32_SELFTEST_ELF - checks of uam-run's bring-up traces on rv32-ref:
#   default      a run without --trace prints none of the new trace lines
#   mmio, sysreg, irq, console
#                on the RISC-V self-test: the lines are there (the UART's LSR read and THR
#                writes, the first write of mtvec, timer line 7 rising, the banner) and every
#                one is identical under the interpreter, --jit and --jit --no-chain
#   --trace-mmio-range, --trace-after  narrow the mmio trace
#   polls        on hand-assembled loops polling an --mmio-rule register: the bit a masked
#                test waits for, the value an equality test waits for, a register that never
#                ends the loop (andi with zero), waiting:/done: lines for an input register
#                that changes, the STILL POLLING stop summary; the same with --jit; nothing
#                reported when the register is ready at once
#   catch-alls   the unknown-mmio first-access line names the instruction (pc, icount);
#                --log-stamp prefixes log lines with the icount
set -eu
RUN=$1
FW=$2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "trace-check: FAIL: $*" >&2; exit 1; }
R="$RUN --raw-base 0x80000000"

# ---- default: quiet --------------------------------------------------------------
$RUN "$FW" </dev/null >"$T/out" 2>"$T/err" || fail "self-test failed without traces"
grep -q 'SELFTEST PASS' "$T/out" || fail "self-test did not pass"
if grep -Eq '^(mmio|sysreg|irq-in|poll|console):' "$T/err"; then
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

# ---- polls ---------------------------------------------------------------------------
# lui t0,0x20000; 1: lw t1,20(t0); andi t1,t1,0x20; beqz t1,1b; 2: j 2b
printf '\267\002\000\040\003\243\102\001\023\163\003\002\343\014\003\376\157\000\000\000' >"$T/bit.bin"
# lui t0,0x20000; 1: li t2,0x5a; lw t1,20(t0); bne t1,t2,1b; 2: j 2b
printf '\267\002\000\040\223\003\240\005\003\243\102\001\343\034\163\376\157\000\000\000' >"$T/eq.bin"
# as bit.bin with "andi t1,zero,0x23": nothing read can end it
printf '\267\002\000\040\003\243\102\001\023\163\060\002\343\014\003\376\157\000\000\000' >"$T/never.bin"
RULE="--mmio-rule 0x20000014=fixed:0"
for a in "" "--jit"; do
    rc=0
    # shellcheck disable=SC2086
    $R $a $RULE -t polls -n 200000 "$T/bit.bin" </dev/null >/dev/null 2>"$T/p" || rc=$?
    [ "$rc" -eq 124 ] || fail "polls$a: exit $rc"
    grep -q '^poll: waiting: pc 0x80000004: loop at 0x80000004 reads 4 bytes at 0x20000014 (rule@0x20000014+0x0), last 0x0; leaves the loop if bit 5 (0x20) becomes 1 (10000 passes) icount=' "$T/p" ||
        fail "polls$a: no waiting line naming bit 5"
    grep -q '^poll: STILL POLLING at the stop: pc 0x80000004: .*leaves the loop if bit 5 (0x20) becomes 1; [0-9]* passes in 1 arrivals' "$T/p" ||
        fail "polls$a: no STILL POLLING summary"
done
$R --mmio-rule 0x20000014=fixed:3 -t polls -n 100000 "$T/eq.bin" </dev/null >/dev/null 2>"$T/p" || true
grep -q '^poll: STILL POLLING at the stop: pc 0x80000008: loop at 0x80000004 .*last 0x3; leaves the loop on a read of 0x5a (no single bit does)' "$T/p" ||
    fail "polls: the equality test's value not found"
$R $RULE -t polls -n 100000 "$T/never.bin" </dev/null >/dev/null 2>"$T/p" || true
grep -q 'STILL POLLING.*no value tried leaves the loop' "$T/p" || fail "polls: a loop no read can end not called so"
# 20 reads of 0, then 0x20: a wait that ends
python3 -c "import sys; sys.stdout.buffer.write(b'\0' * 80 + bytes([0x20, 0, 0, 0]))" >"$T/in.bin"
$R --mmio-rule 0x20000014=input --mmio-input "$T/in.bin" -t polls --poll-report 5 -n 2000 \
    "$T/bit.bin" </dev/null >/dev/null 2>"$T/p" || true
grep -q '^poll: waiting: .*last 0x0; leaves the loop if bit 5' "$T/p" || fail "polls: no waiting line for the input register"
grep -q '^poll: done: .*last 0x20; ' "$T/p" || fail "polls: no done line with the value that ended the wait"
if grep -q 'STILL POLLING' "$T/p"; then fail "polls: a finished wait reported as still polling"; fi
$R --mmio-rule 0x20000014=fixed:0x20 -t polls -n 2000 "$T/bit.bin" </dev/null >/dev/null 2>"$T/p" || true
if grep -q '^poll:' "$T/p"; then fail "polls: a register ready at once reported"; fi

# ---- catch-all lines name the instruction; --log-stamp ---------------------------------
$R --unknown-mmio zero -n 50 "$T/bit.bin" </dev/null >/dev/null 2>"$T/u" || true
grep -q '^\[warn\] unknown-mmio: first read of 4 bytes at 0x20000014 pc=0x80000004 icount=2$' "$T/u" ||
    fail "unknown-mmio first access without its instruction"
$R --log-stamp --unknown-mmio zero -n 50 "$T/bit.bin" </dev/null >/dev/null 2>"$T/u" || true
grep -q '^@2 \[warn\] unknown-mmio: first read' "$T/u" || fail "--log-stamp: no icount prefix"

echo "trace-check: ok"
