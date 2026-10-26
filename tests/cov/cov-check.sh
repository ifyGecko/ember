#!/bin/sh
# cov-check.sh RUN BOARD FIRMWARE [-- EXTRA RUNNER ARGS] - block coverage must not depend on
# the backend.
#
# Runs FIRMWARE on BOARD without coverage, then with --cov-blocks under the
# interpreter, the JIT and the JIT without chaining. Every coverage run must end
# exactly like the run without coverage (same console output, stop line: pc,
# status, instruction count), and the three coverage maps must be byte-identical
# and not empty.
set -eu

run=$1
board=$2
fw=$3
shift 3
[ "${1:-}" = "--" ] && shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The console output plus the runner's stop line ("... at pc=... after N instructions").
outcome() {
    grep -v '^stats:' "$1"
}

"$run" -b "$board" "$@" "$fw" < /dev/null > "$tmp/base.out" 2>&1 || true
outcome "$tmp/base.out" > "$tmp/base.txt"

fail=0
for mode in interp jit jit-nochain; do
    case $mode in
        interp)      flags= ;;
        jit)         flags=--jit ;;
        jit-nochain) flags="--jit --no-chain" ;;
    esac
    # shellcheck disable=SC2086
    "$run" -b "$board" $flags "$@" --cov-blocks --cov-out "$tmp/$mode.map" "$fw" \
        < /dev/null > "$tmp/$mode.out" 2>&1 || true
    outcome "$tmp/$mode.out" > "$tmp/$mode.txt"
    if ! cmp -s "$tmp/base.txt" "$tmp/$mode.txt"; then
        echo "cov-check: $board $mode: the run differs from the run without coverage:" >&2
        diff "$tmp/base.txt" "$tmp/$mode.txt" >&2 || true
        fail=1
    fi
done

if ! od -An -v -tu1 "$tmp/interp.map" | tr -s ' ' '\n' | grep -qv '^0*$'; then
    echo "cov-check: $board: the coverage map is empty" >&2
    fail=1
fi
for mode in jit jit-nochain; do
    if ! cmp -s "$tmp/interp.map" "$tmp/$mode.map"; then
        echo "cov-check: $board: the $mode coverage map differs from the interpreter's" >&2
        fail=1
    fi
done

[ $fail -eq 0 ] && echo "cov-check: $board: $(basename "$fw"): maps identical across backends"
exit $fail
