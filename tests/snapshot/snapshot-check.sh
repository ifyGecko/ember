#!/bin/sh
# Snapshot round trips through a runner: a run split in two by
# --save-snapshot / --load-snapshot must match the uninterrupted run exactly
# (console output, final pc, instruction count and hart state), whichever
# backend (interpreter or JIT) runs each half.
#
#   snapshot-check.sh RUNNER BOARD FIRMWARE SPLIT... [-- EXTRA RUNNER ARGS]
set -eu
run=$1 board=$2 fw=$3
shift 3
splits=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do splits="$splits $1"; shift; done
[ $# -gt 0 ] && shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$run" -b "$board" -d "$@" "$fw" </dev/null >"$tmp/full.out" 2>"$tmp/full.err" || true
fail=0
for n in $splits; do
    for backends in ":" "--jit:" ":--jit"; do
        first=${backends%%:*} second=${backends#*:}
        "$run" -b "$board" $first -n "$n" --save-snapshot "$tmp/s.snap" "$@" "$fw" \
            </dev/null >"$tmp/a.out" 2>"$tmp/a.err" || true
        "$run" -b "$board" $second -d --load-snapshot "$tmp/s.snap" "$@" \
            </dev/null >"$tmp/b.out" 2>"$tmp/b.err" || true
        cat "$tmp/a.out" "$tmp/b.out" >"$tmp/ab.out"
        grep -v '^uam-run: resumed from' "$tmp/b.err" >"$tmp/b2.err" || true
        if cmp -s "$tmp/full.out" "$tmp/ab.out" && cmp -s "$tmp/full.err" "$tmp/b2.err"; then
            echo "[ ok ] $board split at $n (${first:-interp} -> ${second:-interp})"
        else
            echo "[FAIL] $board split at $n (${first:-interp} -> ${second:-interp})"
            diff "$tmp/full.err" "$tmp/b2.err" | head -5 || true
            fail=1
        fi
    done
done
exit $fail
