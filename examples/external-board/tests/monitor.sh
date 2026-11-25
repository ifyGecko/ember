#!/bin/sh
# monitor.sh RUNNER BOARD: the board's own monitor command ("ext-id"), from a monitor
# script run before the first instruction.
set -e
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf 'ext-id scratch 0x1234\next-id\nhelp ext-id\nquit\n' > "$T/s.mon"
: > "$T/empty.bin"
"$1" -b "$2" --monitor-script "$T/s.mon" "$T/empty.bin" 2> "$T/err" || true
grep -q 'uam-run: monitor: id: 0x45585431' "$T/err" &&
grep -q 'uam-run: monitor: scratch: 0x00001234' "$T/err" &&
grep -q 'uam-run: monitor:   the ID block' "$T/err" ||
    { echo "the board's monitor command did not answer:" >&2; cat "$T/err" >&2; exit 1; }
