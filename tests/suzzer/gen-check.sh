#!/bin/sh
# gen-check.sh RUN BOARD FIRMWARE SUZZER_DIR - lists made by suzzer's own generators.
#
# suzzer's tools/gen_patchlist.py and tools/gen_boundary.py (they need rizin and
# python3's rzpipe; without them the check is skipped) make the LCV3 site list and an
# LCVB1 begin marker at fuzz_iteration for FIRMWARE, a build of
# tests/fuzz/fuzztarget.h, with the entry strategy (docs/suzzer.md). ember must find
# every site's bytes in memory (--cov-strict) and give the planted bugs' verdicts.
set -eu
run=$1
board=$2
fw=$3
suzzer=$4
if ! command -v rizin > /dev/null 2>&1 || ! python3 -c 'import rzpipe' 2> /dev/null; then
    echo "gen-check: $board: skipped (suzzer's generators need rizin and rzpipe)"
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
panic=$(printf '0x%x' "0x$(readelf -sW "$fw" | awk '$8 == "fuzz_panic" { print $2; exit }')")   # unpadded, as the runner prints pcs
if ! python3 "$suzzer/tools/gen_patchlist.py" --strategy entry "$fw" "$tmp/fw.lcv3" \
        > "$tmp/gen.log" 2>&1 \
   || ! python3 "$suzzer/tools/gen_boundary.py" --strategy entry --patchlist "$tmp/fw.lcv3" \
        "$fw" "$tmp/fw.lcvb" fuzz_iteration:begin >> "$tmp/gen.log" 2>&1; then
    echo "gen-check: $board: suzzer's generators failed:" >&2
    cat "$tmp/gen.log" >&2
    exit 1
fi
mkdir "$tmp/in"
printf 'hello' > "$tmp/in/1-ok"
printf 'FUZ!'  > "$tmp/in/2-store"
printf 'FUZ?'  > "$tmp/in/3-panic"
rc=0
"$run" -b "$board" --cov-sites "$tmp/fw.lcv3" --cov-boundary "$tmp/fw.lcvb" --cov-strict \
    --cov-ngram 3 --cov-rotate --fuzz-crash-pc "$panic" --fuzz-input uart0 \
    --fuzz-iter-insns 200000 --fuzz-replay "$tmp/in" "$fw" \
    < /dev/null > "$tmp/run.raw" 2> "$tmp/run.err" || rc=$?
grep '^replay: ' "$tmp/run.raw" | sed "s|$tmp/in/||" > "$tmp/run.out" || true
if [ $rc -ne 20 ] || ! grep -q 'site check: \([0-9]*\) of \1 sites match' "$tmp/run.err" \
   || ! grep -q '^replay: 1-ok: ok .*buckets=[1-9]' "$tmp/run.out" \
   || ! grep -q '^replay: 2-store: crash-fault' "$tmp/run.out" \
   || ! grep -q "^replay: 3-panic: crash-oracle pc=$panic" "$tmp/run.out"; then
    echo "gen-check: $board: ember with suzzer's generated lists (exit $rc):" >&2
    cat "$tmp/run.out" "$tmp/run.err" >&2
    fail=1
fi
[ $fail -eq 0 ] && echo "gen-check: $board: $(basename "$fw"): $(grep -o 'site check: [0-9]* of [0-9]*' "$tmp/run.err"); verdicts agree"
exit $fail
