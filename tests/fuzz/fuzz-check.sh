#!/bin/sh
# fuzz-check.sh RUN BOARD FIRMWARE [RAW] - the fuzz harness through the runner (--fuzz-replay).
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h. Its inputs arrive through the
# board's "uart0" route; iterations begin at fuzz_iteration (begin-only) and
# fuzz_panic is a crash pc. The check requires
#   - the expected verdict for each planted bug,
#   - identical output and coverage maps from the interpreter, the JIT and the JIT
#     without chaining,
#   - a crash file replayed on its own reproducing the same line and exit status,
#   - the exception-class policy: with --fuzz-crash-exc none, the bad store reaches
#     the firmware's trap handler, which calls fuzz_panic (an oracle crash);
#   - with RAW (FIRMWARE as a flat binary), the raw image loaded at the ELF's first
#     segment and started at _start with --entry giving the same verdicts.
set -eu

run=$1
board=$2
fw=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

sym() {
    v=$(readelf -sW "$fw" | awk -v n="$1" '$8 == n { print $2; exit }')
    [ -n "$v" ] || { echo "fuzz-check: no symbol $1 in $fw" >&2; exit 1; }
    printf '0x%x\n' "0x$v"   # readelf pads with zeros; the runner prints pcs without
}
begin=$(sym fuzz_iteration)
panic=$(sym fuzz_panic)

mkdir "$tmp/in"
printf 'hello' > "$tmp/in/1-ok"
printf 'FUZ!'  > "$tmp/in/2-store"
printf 'FUZ?'  > "$tmp/in/3-panic"
printf 'Lxx'   > "$tmp/in/4-loop"
printf 'ok'    > "$tmp/in/5-ok"

# fuzz BACKEND-FLAGS OTHER-FLAGS: the replay lines on stdout (input names relative to
# the input directory); returns the runner's exit status.
fuzz() {
    frc=0
    # shellcheck disable=SC2086
    "$run" -b "$board" $1 --cov-blocks --fuzz-begin "$begin" --fuzz-crash-pc "$panic" \
        --fuzz-input uart0 --fuzz-iter-insns 200000 -n 10000000 $2 "$fw" \
        < /dev/null > "$tmp/raw" 2> "$tmp/err" || frc=$?
    grep '^replay: ' "$tmp/raw" | sed "s|$tmp/in/||"
    return $frc
}

fail=0
for mode in interp jit jit-nochain; do
    case $mode in
        interp)      flags= ;;
        jit)         flags=--jit ;;
        jit-nochain) flags="--jit --no-chain" ;;
    esac
    rc=0
    fuzz "$flags" "--cov-out $tmp/$mode.map --fuzz-replay $tmp/in" > "$tmp/$mode.out" || rc=$?
    if [ $rc -ne 20 ]; then
        echo "fuzz-check: $board $mode: exit $rc, expected 20 (the first crash, 2-store)" >&2
        cat "$tmp/err" >&2
        fail=1
    fi
done

out=$tmp/interp.out
expect() {
    if ! grep -q "^replay: $1: $2" "$out"; then
        echo "fuzz-check: $board: expected '$1: $2', got:" >&2
        cat "$out" >&2
        fail=1
    fi
}
expect 1-ok ok
expect 2-store "crash-fault pc=0x[0-9a-f]* (store-fault"
expect 3-panic "crash-oracle pc=$panic"
expect 4-loop timeout
expect 5-ok ok

for mode in jit jit-nochain; do
    if ! cmp -s "$tmp/interp.out" "$tmp/$mode.out" || ! cmp -s "$tmp/interp.map" "$tmp/$mode.map"; then
        echo "fuzz-check: $board: $mode differs from the interpreter:" >&2
        diff "$tmp/interp.out" "$tmp/$mode.out" >&2 || true
        fail=1
    fi
done

# A crash file on its own reproduces the same verdict and detail (the line names
# the file by its full path then, so compare what follows the name).
rc=0
fuzz --jit "--fuzz-replay $tmp/in/2-store" > "$tmp/one.out" || rc=$?
a=$(sed 's|^replay: [^:]*: ||' "$tmp/one.out")
b=$(grep '^replay: 2-store' "$out" | sed 's|^replay: [^:]*: ||')
if [ $rc -ne 20 ] || [ "$a" != "$b" ]; then
    echo "fuzz-check: $board: replaying the crash alone: exit $rc (expected 20), '$a' vs '$b'" >&2
    fail=1
fi

# Exceptions the policy lets through reach the firmware's handler.
rc=0
fuzz "" "--fuzz-crash-exc none --fuzz-replay $tmp/in/2-store" > "$tmp/none.out" || rc=$?
if [ $rc -ne 21 ] || ! grep -q "crash-oracle pc=$panic" "$tmp/none.out"; then
    echo "fuzz-check: $board: --fuzz-crash-exc none: exit $rc (expected 21):" >&2
    cat "$tmp/none.out" >&2
    fail=1
fi

# --fuzz-begin-skip: the snapshot at a later arrival at the begin pc (an empty UART at
# boot runs the handler and comes back) gives the same verdicts; an arrival the boot
# never reaches fails it.
rc=0
fuzz "" "--fuzz-begin-skip 3 --fuzz-replay $tmp/in" > "$tmp/skip.out" || rc=$?
cut -d' ' -f2-3 "$out" > "$tmp/want.v"
cut -d' ' -f2-3 "$tmp/skip.out" > "$tmp/skip.v"
if [ $rc -ne 20 ] || ! cmp -s "$tmp/want.v" "$tmp/skip.v"; then
    echo "fuzz-check: $board: --fuzz-begin-skip 3: exit $rc, verdicts:" >&2
    diff "$tmp/want.v" "$tmp/skip.v" >&2 || true
    fail=1
fi
rc=0
fuzz "" "-n 300000 --fuzz-begin-skip 100000000 --fuzz-replay $tmp/in/1-ok" > /dev/null || rc=$?
if [ $rc -ne 2 ] || ! grep -q 'often enough' "$tmp/err"; then
    echo "fuzz-check: $board: --fuzz-begin-skip past the last arrival: exit $rc (expected 2):" >&2
    cat "$tmp/err" >&2
    fail=1
fi

if [ -n "${4:-}" ]; then
    lo=$(readelf -lW "$fw" | awk '$1 == "LOAD" { print $3; exit }')
    rc=0
    "$run" -b "$board" --raw-base "$lo" --entry "$(sym _start)" --cov-blocks --fuzz-begin "$begin" \
        --fuzz-crash-pc "$panic" --fuzz-input uart0 --fuzz-iter-insns 200000 -n 10000000 \
        --fuzz-replay "$tmp/in" "$4" < /dev/null > "$tmp/raw.raw" 2> "$tmp/raw.err" || rc=$?
    grep '^replay: ' "$tmp/raw.raw" | sed "s|$tmp/in/||" > "$tmp/raw.out" || true
    if [ $rc -ne 20 ] || ! cmp -s "$tmp/raw.out" "$out"; then
        echo "fuzz-check: $board: the raw image with --entry (exit $rc):" >&2
        diff "$out" "$tmp/raw.out" >&2 || true
        cat "$tmp/raw.err" >&2
        fail=1
    fi
fi

[ $fail -eq 0 ] && echo "fuzz-check: $board: $(basename "$fw"): verdicts, backends and replay agree"
exit $fail
