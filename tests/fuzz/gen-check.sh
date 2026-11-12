#!/bin/sh
# gen-check.sh RUN BOARD FIRMWARE - lists made by fuzz/tools' generators.
#
# gen_sites.py and gen_boundaries.py (they need rizin and python3's rzpipe; without them
# the check is skipped) make the site list and a begin marker at fuzz_iteration for
# FIRMWARE, a build of tests/fuzz/fuzztarget.h, with the entry strategy. ember must find
# every site's bytes in memory (--cov-strict) and give the planted bugs' verdicts. With
# headless Ghidra (analyzeHeadless on PATH, or GHIDRA_HEADLESS), the Ghidra route
# (fuzz/tools/ghidra/) must write the same two files byte for byte, and so again with
# each tool's default strategy where that is not entry (every arch but MIPS); for an ARM
# BE8 image, which rizin cannot decode, the lists come from the Ghidra route alone.
set -eu
run=$1
board=$2
fw=$3
tools=$(dirname "$0")/../../fuzz/tools
if ! command -v rizin > /dev/null 2>&1 || ! python3 -c 'import rzpipe' 2> /dev/null; then
    echo "gen-check: $board: skipped (the generators need rizin and rzpipe)"
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
panic=$(printf '0x%x' "0x$(readelf -sW "$fw" | awk '$8 == "fuzz_panic" { print $2; exit }')")   # unpadded, as the runner prints pcs
headless=${GHIDRA_HEADLESS:-$(command -v analyzeHeadless || true)}
ghidra_lists() {   # ghidra_lists SITES BOUNDS [STRATEGY]: entry lists on import, others after
    gt=$(cd "$tools/ghidra" && pwd)
    if [ "${3:-entry}" = entry ]; then
        rm -rf "$tmp/gp" && mkdir "$tmp/gp"
        set -- "$1" "$2" entry -import "$fw"
    else
        set -- "$1" "$2" "$3" -process "$(basename "$fw")" -noanalysis
    fi
    sites=$1 bounds=$2 strategy=$3
    shift 3
    rm -f "$sites" "$bounds"
    "$headless" "$tmp/gp" p "$@" -scriptPath "$gt" \
        -postScript gen_sites.py "$sites" --strategy "$strategy" \
        -postScript gen_boundaries.py "$bounds" fuzz_iteration:begin \
        --strategy "$strategy" --sites "$sites" > "$tmp/ghidra.log" 2>&1 || return 1
    [ -s "$sites" ] && [ -s "$bounds" ]
}
ghidra_failed() {   # ghidra_failed SITES BOUNDS WHAT: say which, with the log's tail if a file is missing
    if [ -s "$1" ] && [ -s "$2" ]; then
        echo "gen-check: $board: the Ghidra route's $3 differ from rizin's" >&2
    else
        echo "gen-check: $board: the Ghidra route wrote no $3; its log ends:" >&2
        grep -v -e '^INFO ' -e '^    /' "$tmp/ghidra.log" | tail -n 30 >&2
    fi
}
route=rizin
if ! python3 "$tools/gen_sites.py" --strategy entry "$fw" "$tmp/fw.uams" > "$tmp/gen.log" 2>&1 \
   || ! python3 "$tools/gen_boundaries.py" --strategy entry --sites "$tmp/fw.uams" \
        "$fw" "$tmp/fw.uamb" fuzz_iteration:begin >> "$tmp/gen.log" 2>&1; then
    if grep -q 'BE8' "$tmp/gen.log"; then
        # rizin cannot decode BE8 code: the Ghidra route only, when it is there.
        if [ -z "$headless" ]; then
            echo "gen-check: $board: skipped (rizin cannot decode BE8 code; no headless Ghidra)"
            exit 0
        fi
        if ! ghidra_lists "$tmp/fw.uams" "$tmp/fw.uamb"; then
            ghidra_failed "$tmp/fw.uams" "$tmp/fw.uamb" lists
            exit 1
        fi
        route=ghidra
    else
        echo "gen-check: $board: the generators failed:" >&2
        cat "$tmp/gen.log" >&2
        exit 1
    fi
fi
mkdir "$tmp/in"
printf 'hello' > "$tmp/in/1-ok"
printf 'FUZ!'  > "$tmp/in/2-store"
printf 'FUZ?'  > "$tmp/in/3-panic"
rc=0
"$run" -b "$board" --cov-sites "$tmp/fw.uams" --cov-boundary "$tmp/fw.uamb" --cov-strict \
    --cov-ngram 3 --cov-rotate --fuzz-crash-pc "$panic" --fuzz-input uart0 \
    --fuzz-iter-insns 200000 --fuzz-replay "$tmp/in" "$fw" \
    < /dev/null > "$tmp/run.raw" 2> "$tmp/run.err" || rc=$?
grep '^replay: ' "$tmp/run.raw" | sed "s|$tmp/in/||" > "$tmp/run.out" || true
if [ $rc -ne 20 ] || ! grep -q 'site check: \([0-9]*\) of \1 sites match' "$tmp/run.err" \
   || ! grep -q '^replay: 1-ok: ok .*buckets=[1-9]' "$tmp/run.out" \
   || ! grep -q '^replay: 2-store: crash-fault' "$tmp/run.out" \
   || ! grep -q "^replay: 3-panic: crash-oracle pc=$panic" "$tmp/run.out"; then
    echo "gen-check: $board: ember with the generated lists (exit $rc):" >&2
    cat "$tmp/run.out" "$tmp/run.err" >&2
    fail=1
fi

# The Ghidra route, when headless Ghidra is there, must write the same files.
ghidra=
if [ $route = ghidra ]; then
    ghidra="; lists from the Ghidra route (rizin cannot decode BE8)"
elif [ -n "$headless" ]; then
    if ghidra_lists "$tmp/g.uams" "$tmp/g.uamb" \
       && cmp -s "$tmp/fw.uams" "$tmp/g.uams" && cmp -s "$tmp/fw.uamb" "$tmp/g.uamb"; then
        ghidra="; the Ghidra route wrote the same files"
    else
        ghidra_failed "$tmp/g.uams" "$tmp/g.uamb" "entry lists"
        fail=1
    fi
    # Each tool's default strategy: entry on MIPS (compared above), terminator elsewhere.
    case $board in
    mips*) ;;
    *)
        if ! python3 "$tools/gen_sites.py" "$fw" "$tmp/d.uams" > "$tmp/gen.log" 2>&1 \
           || ! python3 "$tools/gen_boundaries.py" --sites "$tmp/d.uams" \
                "$fw" "$tmp/d.uamb" fuzz_iteration:begin >> "$tmp/gen.log" 2>&1; then
            echo "gen-check: $board: the generators failed with their default strategy:" >&2
            cat "$tmp/gen.log" >&2
            fail=1
        elif [ $fail -eq 0 ]; then
            if ghidra_lists "$tmp/gd.uams" "$tmp/gd.uamb" auto \
               && cmp -s "$tmp/d.uams" "$tmp/gd.uams" && cmp -s "$tmp/d.uamb" "$tmp/gd.uamb"; then
                ghidra="$ghidra, also with the default strategy"
            else
                ghidra_failed "$tmp/gd.uams" "$tmp/gd.uamb" "default-strategy lists"
                fail=1
            fi
        fi ;;
    esac
fi
[ $fail -eq 0 ] && echo "gen-check: $board: $(basename "$fw"): $(grep -o 'site check: [0-9]* of [0-9]*' "$tmp/run.err"); verdicts agree$ghidra"
exit $fail
