#!/bin/sh
# boot.sh RUNNER BOARD IMAGE EXPECT [RUNNER ARGS...]: run IMAGE on BOARD within an
# instruction budget; every line of EXPECT must appear on the console, in order.
set -eu
runner=$1 board=$2 image=$3 expect=$4
shift 4
# Nothing to boot yet: skipped (exit status 77), not failed.
[ -f "$image" ] || { echo "boot: no image at $image: skipped" >&2; exit 77; }
grep -q . "$expect" 2> /dev/null || { echo "boot: $expect is empty: skipped" >&2; exit 77; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rc=0
"$runner" -b "$board" -n 200000000 "$@" "$image" < /dev/null > "$tmp/console" 2> "$tmp/log" || rc=$?
last=0
while IFS= read -r line; do
    [ -n "$line" ] || continue
    n=$(tail -n +$((last + 1)) "$tmp/console" | grep -n -F -m 1 -- "$line" | cut -d: -f1) || true
    if [ -z "$n" ]; then
        echo "boot: console lacks '$line' (runner exit $rc); its end:" >&2
        tail -n 5 "$tmp/console" "$tmp/log" >&2
        exit 1
    fi
    last=$((last + n))
done < "$expect"
