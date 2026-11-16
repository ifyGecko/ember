#!/bin/sh
# oracle-check.sh TRUN FIRMWARE - the memory oracles end to end: the guarded heap
# (uam/heap.h) behind a board's hooks, and the return guard (--guard-returns).
#
# FIRMWARE is a build of tests/firmware/heaptarget.c for rv32-test (tests/boards/: rv32-ref
# plus heap_hooks.c, which sends the firmware's fw_malloc/fw_free/fw_realloc to a guarded
# heap when given -o heap=...). Each one-byte input triggers one planted bug. Required, on
# the interpreter, the JIT and the JIT without chaining alike:
#   - without the oracles every input ends ok: nothing faults on flat memory (the gap);
#   - with them each bug is a guard crash of its kind, at the same pc on every backend,
#     and the inputs with no bug stay ok (a longjmp, a grown block, a context switch);
#   - a switch into a new context by returning into its entry function is reported until
#     the entry is named with --return-ok; --guard-returns=log lists it and goes on;
#   - a campaign's shared boot (--fuzz-snapshot) carries the heap and the call tracking:
#     a worker started from the snapshot reports the same lines;
#   - a snapshot taken without the oracles is not loaded with them.
set -eu
run=$1
fw=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "oracle-check: FAIL: $*" >&2; exit 1; }

sym() { printf '0x%x' "0x$(readelf -sW "$fw" | awk -v n="$1" '$NF == n { print $2; exit }')"; }
heap="-o heap=$(sym fw_heap)+4096 -o malloc=$(sym fw_malloc) -o free=$(sym fw_free) -o realloc=$(sym fw_realloc)"

mkdir "$tmp/in"
for c in b d f g j k o r s t u w x; do printf '%s' "$c" > "$tmp/in/$c"; done

# replay BACKEND-FLAGS OTHER-FLAGS: the replay lines, input names relative, no counts.
replay() {
    # shellcheck disable=SC2086
    "$run" -b rv32-test $1 $2 --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic \
        --fuzz-input uart0 --fuzz-iter-insns 200000 -n 10000000 --fuzz-replay "$tmp/in" "$fw" \
        < /dev/null 2> "$tmp/err" | grep '^replay: ' | sed "s|$tmp/in/||; s| insns=.*||" || true
}

want_plain=$(for c in b d f g j k o r s t u w x; do echo "replay: $c: ok"; done)
kinds="b:heap-bad-free d:heap-double-free f:heap-freed g:heap-freed o:heap-redzone r:heap-redzone
       s:return u:heap-redzone w:heap-wild"

for flags in "" "--jit" "--jit --no-chain"; do
    name=${flags:-interp}
    [ "$(replay "$flags" "")" = "$want_plain" ] || { cat "$tmp/err" >&2; fail "$name: without the oracles an input is not ok"; }

    out=$(replay "$flags" "$heap --guard-returns --return-ok co_entry")
    echo "$out" > "$tmp/$(echo "$name" | tr -d ' -').out"
    for k in $kinds; do
        c=${k%%:*}
        echo "$out" | grep -q "^replay: $c: crash-guard pc=0x[0-9a-f]* (${k#*:}: " ||
            { echo "$out" >&2; cat "$tmp/err" >&2; fail "$name: input $c is not a ${k#*:} guard crash"; }
    done
    for c in j k t x; do
        echo "$out" | grep -q "^replay: $c: ok\$" || { echo "$out" >&2; fail "$name: input $c (no bug) is not ok"; }
    done
done
cmp -s "$tmp/interp.out" "$tmp/jit.out" && cmp -s "$tmp/interp.out" "$tmp/jitnochain.out" ||
    fail "the backends report different crashes"

# The details a report must carry.
grep -q '^replay: o: .*write 1 at 0x[0-9a-f]*: 1 past the end of the 24-byte block at 0x[0-9a-f]*, allocated at 0x' \
    "$tmp/interp.out" || fail "the overflow's report lacks the block or its allocation site"
grep -q '^replay: u: .*: 1 before the 24-byte block' "$tmp/interp.out" || fail "the underflow's report"
grep -q '^replay: f: .*in the freed 24-byte block at 0x[0-9a-f]*, allocated at 0x[0-9a-f]*, freed at 0x' \
    "$tmp/interp.out" || fail "the use-after-free's report lacks the free site"
recover=$(sym recover)
grep -q "^replay: s: .*returns to $recover, which no call returns to" "$tmp/interp.out" ||
    fail "the smashed return's report does not name recover ($recover)"

# A new context entered by a return into its entry function: reported until named.
out=$(replay "--jit" "--guard-returns")
echo "$out" | grep -q "^replay: t: crash-guard pc=0x[0-9a-f]* (return: returns to $(sym co_entry)," ||
    { echo "$out" >&2; fail "the return into co_entry is not reported without --return-ok"; }
"$run" -b rv32-test --guard-returns=log --uart-input 't' -n 30000 "$fw" < /dev/null > /dev/null 2> "$tmp/log" || true
[ "$(grep -c "^guard return (log): .*returns to $(sym co_entry)," "$tmp/log")" = 1 ] ||
    { cat "$tmp/log" >&2; fail "--guard-returns=log does not list co_entry once"; }

# --backtrace names the call stack of the crash.
# shellcheck disable=SC2086
"$run" -b rv32-test --jit $heap --backtrace --fuzz-begin fuzz_iteration --fuzz-input uart0 \
    --fuzz-iter-insns 200000 --fuzz-replay "$tmp/in/o" "$fw" < /dev/null > "$tmp/bt" 2>&1 || true
grep -q '^backtrace: #0 0x[0-9a-f]* <fuzz_iteration> called, returns to 0x[0-9a-f]* <main' "$tmp/bt" ||
    grep -q '^backtrace: #[01] 0x[0-9a-f]* <fuzz_iteration> called' "$tmp/bt" ||
    { cat "$tmp/bt" >&2; fail "--backtrace does not show fuzz_iteration"; }

# A shared boot: the snapshot carries the heap and the frames.
# shellcheck disable=SC2086
"$run" -b rv32-test --jit $heap --guard-returns --return-ok co_entry --fuzz-snapshot "$tmp/begin.snap" \
    --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic --fuzz-input uart0 --fuzz-iter-insns 200000 \
    --fuzz-replay "$tmp/in" "$fw" < /dev/null > "$tmp/boot.raw" 2> "$tmp/boot.err" || true
[ -s "$tmp/begin.snap" ] || { cat "$tmp/boot.err" >&2; fail "--fuzz-snapshot wrote no snapshot"; }
# shellcheck disable=SC2086
"$run" -b rv32-test --jit $heap --guard-returns --return-ok co_entry --fuzz-snapshot "$tmp/begin.snap" \
    --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic --fuzz-input uart0 --fuzz-iter-insns 200000 \
    --fuzz-replay "$tmp/in" "$fw" < /dev/null > "$tmp/load.raw" 2> "$tmp/load.err" || true
[ "$(grep '^replay: ' "$tmp/boot.raw")" = "$(grep '^replay: ' "$tmp/load.raw")" ] && grep -q '^replay: .*/s: crash-guard' "$tmp/load.raw" ||
    { cat "$tmp/load.err" >&2; fail "a worker started from the shared snapshot reports other lines"; }

# A snapshot without the oracles' state does not fit a machine with them.
"$run" -b rv32-test -n 2000 --save-snapshot "$tmp/plain.snap" "$fw" < /dev/null > /dev/null 2>&1 || true
[ -s "$tmp/plain.snap" ] || fail "no plain snapshot to test with"
rc=0
"$run" -b rv32-test --guard-returns -n 2000 --load-snapshot "$tmp/plain.snap" "$fw" < /dev/null > /dev/null 2> "$tmp/snap.err" || rc=$?
[ "$rc" != 0 ] && grep -q 'snapshot' "$tmp/snap.err" ||
    { cat "$tmp/snap.err" >&2; fail "a snapshot taken without call tracking was loaded with it (exit $rc)"; }

echo "oracle-check: ok"
