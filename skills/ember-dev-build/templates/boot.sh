#!/bin/sh
# boot.sh RUN_SH RUNNER VAR IMAGE EXPECT [RUN.SH ARGS...]: run the firmware as run.sh does
# (VAR=IMAGE names it: IMAGE=, or FLASH= on a PC), on the runner just built, within an
# instruction budget; every line of EXPECT must appear on the console, in order.
set -eu
run_sh=$1 runner=$2 var=$3 image=$4 expect=$5
shift 5
# Nothing to boot yet: skipped (exit status 77), not failed.
[ -f "$image" ] || { echo "boot: no image at $image: skipped" >&2; exit 77; }
grep -q . "$expect" 2> /dev/null || { echo "boot: $expect is empty: skipped" >&2; exit 77; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rc=0
env "$var=$image" EMU_BIN="$runner" TRACE= "$run_sh" -n "${BOOT_BUDGET:-200000000}" "$@" \
    < /dev/null > "$tmp/console" 2> "$tmp/log" || rc=$?
last=0
while IFS= read -r line; do
    [ -n "$line" ] || continue
    n=$(tail -n +$((last + 1)) "$tmp/console" | grep -a -n -F -m 1 -- "$line" | cut -d: -f1) || true
    if [ -z "$n" ]; then
        echo "boot: console lacks '$line' (runner exit $rc); its end:" >&2
        tail -n 5 "$tmp/console" "$tmp/log" >&2
        exit 1
    fi
    last=$((last + n))
done < "$expect"
