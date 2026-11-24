#!/bin/sh
# uninit-heap-check.sh RUN FIRMWARE - --fuzz-uninit and the guarded heap (uam/heap.h): memory
# the heap hands out is filled with the pattern in the filled run, which also reaches memory
# the boot had already written (the firmware's heap sits in .bss, which the loader zeroes:
# no page fill touches it).
#
# FIRMWARE is a build of tests/firmware/heaptarget.c for rv32-test with the heap hooks of
# tests/boards/heap_hooks.c. Input 'm' reads a fresh block before writing it; 'k' is clean.
#   with the heap on: 'm' differs and the exit status is 26, 'k' does not;
#   without it: nothing differs (the arena was written, and nothing says where blocks are).
set -eu
run=$1
fw=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "uninit-heap-check: FAIL: $*" >&2; exit 1; }

sym() { printf '0x%x' "0x$("${READELF:-readelf}" -sW "$fw" | awk -v n="$1" '$NF == n { print $2; exit }')"; }
heap="-o heap=$(sym fw_heap)+4096 -o malloc=$(sym fw_malloc) -o free=$(sym fw_free) -o realloc=$(sym fw_realloc)"

mkdir "$tmp/in"
printf 'k' > "$tmp/in/clean"
printf 'm' > "$tmp/in/fresh"

replay() {
    rc=0
    # shellcheck disable=SC2086
    "$run" -b rv32-test "$@" --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic \
        --fuzz-input uart0 --fuzz-iter-insns 200000 -n 10000000 --fuzz-replay "$tmp/in" "$fw" \
        < /dev/null > "$tmp/out" 2> "$tmp/err" || rc=$?
    grep '^replay: ' "$tmp/out" | sed "s|$tmp/in/||" > "$tmp/lines" || true
}

# shellcheck disable=SC2086
replay $heap --fuzz-uninit
[ $rc -eq 26 ] || fail "heap on: exit $rc, expected 26: $(cat "$tmp/lines")"
grep -q '^replay: fresh: uninit differs=' "$tmp/lines" || fail "heap on: the fresh read is not reported: $(cat "$tmp/lines")"
! grep -q '^replay: clean: uninit' "$tmp/lines" || fail "heap on: the clean input is reported"
grep -q 'uninit: 2 runs repeated with memory filled, 1 differed' "$tmp/err" \
    || fail "heap on: the summary is wrong: $(grep uninit "$tmp/err")"

# shellcheck disable=SC2086
replay $heap
[ $rc -eq 0 ] || fail "heap on, no --fuzz-uninit: exit $rc"
! grep -q uninit "$tmp/lines" || fail "heap on, no --fuzz-uninit: an uninit line"

replay --fuzz-uninit
[ $rc -eq 0 ] || fail "heap off: exit $rc, expected 0 (nothing says where a block is): $(cat "$tmp/lines")"

echo "uninit-heap-check: ok"
