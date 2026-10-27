#!/bin/sh
# jitbench.sh - time a uam-run style command several times (docs/jit-perf.md)
#
#   tools/jitbench.sh [--runs N] [--label NAME] [--header] -- COMMAND [ARGS...]
#
# COMMAND should print the runner's --stats lines on stderr (without them - an older
# runner - the wall time and the 'after N instructions' exit line are used). Its stdout (the guest
# console) is discarded, but its hash is reported, so runs of two builds can be checked
# for identical guest behaviour. Prints one tab-separated row:
#
#   label runs time_s MIPS host_insn_per_guest translate_s eval_calls load_calls
#   store_calls code_bytes console_sha guest_insns
#
# time_s and MIPS are medians over the runs; host_insn_per_guest is user-space host
# instructions (perf stat, if usable) per guest instruction, the least noisy measure.
# The other columns come from the last run (they do not vary: runs are deterministic).
set -eu

runs=5 label=run header=0
while [ $# -gt 0 ]; do
    case $1 in
    --runs) runs=$2; shift 2;;
    --label) label=$2; shift 2;;
    --header) header=1; shift;;
    --) shift; break;;
    *) echo "jitbench.sh: unknown option $1" >&2; exit 2;;
    esac
done
[ $# -gt 0 ] || { echo "usage: jitbench.sh [--runs N] [--label NAME] [--header] -- COMMAND..." >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
use_perf=0
if command -v perf >/dev/null 2>&1 && perf stat -x, -e instructions:u -o "$tmp/probe" true 2>/dev/null \
        && grep -q instructions "$tmp/probe"; then
    use_perf=1
fi

i=0
: > "$tmp/times"; : > "$tmp/mips"; : > "$tmp/hpg"
while [ $i -lt "$runs" ]; do
    i=$((i + 1))
    t0=$(date +%s.%N)
    if [ $use_perf = 1 ]; then
        perf stat -x, -e instructions:u -o "$tmp/perf" "$@" > "$tmp/out" 2> "$tmp/err" || true
    else
        "$@" > "$tmp/out" 2> "$tmp/err" || true
    fi
    t1=$(date +%s.%N)
    line=$(grep -a '^stats: time=' "$tmp/err" | tail -1 || true)
    if [ -n "$line" ]; then
        t=$(echo "$line" | sed -n 's/.*time=\([0-9.]*\)s.*/\1/p')
        n=$(echo "$line" | sed -n 's/.* instructions=\([0-9]*\).*/\1/p')
    else
        t=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }')
        n=$(grep -a ' after [0-9]* instructions' "$tmp/err" | tail -1 | sed -n 's/.* after \([0-9]*\) instructions.*/\1/p')
        [ -n "$n" ] || { echo "jitbench.sh: neither a 'stats: time=' nor an exit line:" >&2; tail -5 "$tmp/err" >&2; exit 1; }
    fi
    echo "$t" >> "$tmp/times"
    awk -v t="$t" -v n="$n" 'BEGIN { printf "%.1f\n", (t > 0) ? n / t / 1e6 : 0 }' >> "$tmp/mips"
    if [ $use_perf = 1 ]; then
        h=$(grep instructions "$tmp/perf" | cut -d, -f1)
        awk -v h="$h" -v n="$n" 'BEGIN { if (n > 0 && h ~ /^[0-9]+$/) printf "%.2f\n", h / n; else print "-" }' >> "$tmp/hpg"
    else
        echo "-" >> "$tmp/hpg"
    fi
done

median() { sort -n "$1" | awk '{ a[NR] = $1 } END { print (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2 }'; }
field() { grep -a "^stats: $1" "$tmp/err" | tail -1 | sed -n "s/.*$2=\([0-9.]*\).*/\1/p"; }

[ $header = 1 ] && printf 'label\truns\ttime_s\tMIPS\thost_insn_per_guest\ttranslate_s\teval_calls\tload_calls\tstore_calls\tcode_bytes\tconsole_sha\tguest_insns\n'
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$runs" "$(median "$tmp/times")" \
    "$(median "$tmp/mips")" "$(grep -v - "$tmp/hpg" | sort -n | awk '{ a[NR] = $1 } END { print NR ? a[int((NR + 1) / 2)] : "-" }')" \
    "$(field time translate | sed 's/s$//')" "$(field 'jit calls' eval)" "$(field 'jit calls' load)" \
    "$(field 'jit calls' store)" "$(field time 'code bytes')" "$(sha256sum < "$tmp/out" | cut -c1-12)" "$n"
