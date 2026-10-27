#!/bin/sh
# xbench.sh - run the cross-ISA benchmark (tests/bench) on every guest and compare
#
#   tools/xbench.sh [--runs N] [--interp] [--rounds N] [--only K] [--per-kernel [--times]]
#                   [--check] [--run UAM_RUN] [--out DIR] [--variants LIST]
#
# Builds tests/bench (ROUNDS and ONLY passed through), runs each guest build on its
# reference board with --jit (--interp: the interpreter) under tools/jitbench.sh, and
# checks its console against the host build's. Every guest does the same work, so
# time_s is the number to compare across ISAs; vs_best is it divided by the fastest
# guest's. Prints one tab-separated row per guest:
#
#   variant board guest_insns time_s MIPS vs_best host_insn_per_guest translate_s ok
#
# --per-kernel instead runs each kernel on its own and prints, for each kernel, every
# guest's time divided by the fastest guest's on that kernel, and last the geometric
# mean of those ratios per guest (1.00 everywhere would be perfect parity). --times also
# prints each kernel's time_s per guest, and their geometric mean: the table to compare two
# builds by, as the ratios hide a change that speeds up every guest alike.
#
# --check is the quick correctness run of make bench-test: 2 rounds, one run each,
# with the interpreter and with the JIT.
#
# Builds go to DIR (default build/firmware-bench), DIR-kN per kernel, DIR-check.
# Variants (default all): rv32 mips mipsel arm thumb thumb-be ppc x86.
# Exits non-zero if any guest's console differs from the host build's.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
runs=5 mode=--jit rounds= only= per_kernel=0 times=0 check=0 run=$here/build/uam-run
base=$here/build/firmware-bench
variants="rv32 mips mipsel arm thumb thumb-be ppc x86"
while [ $# -gt 0 ]; do
    case $1 in
    --runs) runs=$2; shift 2;;
    --interp) mode=; shift;;
    --rounds) rounds=$2; shift 2;;
    --only) only=$2; shift 2;;
    --per-kernel) per_kernel=1; shift;;
    --times) times=1; shift;;
    --check) check=1 rounds=2 runs=1; shift;;
    --run) run=$2; shift 2;;
    --out) base=$2; shift 2;;
    --variants) variants=$2; shift 2;;
    *) echo "xbench.sh: unknown option $1" >&2; exit 2;;
    esac
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# run_all ONLY ROWS [SUFFIX]: build (kernel ONLY, or all when empty) into its own
# directory and append "board<TAB>jitbench row<TAB>ok" for every variant to ROWS.
run_all() {
    out=$base${1:+-k$1}${3:-}
    make -s -C "$here/tests/bench" OUT="$out" ${rounds:+ROUNDS=$rounds} ${1:+ONLY=$1} >&2
    ref=$("$out/bench-host" | sha256sum | cut -c1-12)
    for v in $variants; do
        extra=
        case $v in
        rv32)     board=rv32-ref   img=bench-rv32.elf;;
        mips)     board=mips-ref   img=bench-mips.elf;;
        mipsel)   board=mipsel-ref img=bench-mipsel.elf;;
        arm)      board=arm-ref    img=bench-arm.elf;;
        thumb)    board=arm-ref    img=bench-thumb.elf;;
        thumb-be) board=armeb-ref  img=bench-thumb-be.elf;;
        ppc)      board=ppc-ref    img=bench-ppc.elf;;
        x86)      board=x86_64-ref img=bench-x86.bin extra="--raw-base 0 --entry 0";;
        *) echo "xbench.sh: unknown variant $v" >&2; exit 2;;
        esac
        # shellcheck disable=SC2086
        row=$("$here/tools/jitbench.sh" --runs "$runs" --label "$v$([ -n "$mode" ] || echo /interp)" -- \
              "$run" -b "$board" $extra $mode --stats "$out/$img")
        ok=WRONG
        [ "$(printf '%s' "$row" | cut -f11)" = "$ref" ] && ok=ok
        printf '%s\t%s\t%s\n' "$board" "$row" "$ok" >> "$2"
    done
}

# Row columns: 1 board, then jitbench's (2 label, 4 time_s, 5 MIPS,
# 6 host_insn_per_guest, 7 translate_s, 13 guest_insns), 14 ok.
if [ $check = 1 ]; then
    mode=; run_all "" "$tmp/rows" -check
    mode=--jit; run_all "" "$tmp/rows" -check
    cut -f2,14 "$tmp/rows"
elif [ $per_kernel = 0 ]; then
    run_all "$only" "$tmp/rows"
    awk -F'\t' '
        { r[NR] = $0; if (NR == 1 || $4 < best) best = $4 }
        END {
            printf "variant\tboard\tguest_insns\ttime_s\tMIPS\tvs_best\thost_insn_per_guest\ttranslate_s\tok\n"
            for (i = 1; i <= NR; i++) {
                split(r[i], f, "\t")
                printf "%s\t%s\t%s\t%s\t%s\t%.2f\t%s\t%s\t%s\n", f[2], f[1], f[13], f[4], f[5],
                       (best > 0 ? f[4] / best : 0), f[6], f[7], f[14]
            }
        }' "$tmp/rows"
else
    k=0
    while :; do
        make -s -C "$here/tests/bench" OUT="$base-k$k" ONLY=$k ${rounds:+ROUNDS=$rounds} >&2
        name=$("$base-k$k/bench-host" | sed -n '1s/^bench: \([^ ]*\) .*/\1/p')
        [ "$name" != total ] || break
        run_all $k "$tmp/k$k"
        sed "s/^/$name\t/" "$tmp/k$k" >> "$tmp/rows"
        k=$((k + 1))
    done
    # columns now: 1 kernel, 2 board, 3 label, 5 time_s, 15 ok
    awk -F'\t' -v times=$times '
        { if (!($1 in seen)) { seen[$1] = 1; ks[++nk] = $1 }
          if (!($3 in vseen)) { vseen[$3] = 1; vs[++nv] = $3 }
          t[$1, $3] = $5; if (!(($1) in best) || $5 < best[$1]) best[$1] = $5
          if ($15 != "ok") bad[$1, $3] = 1 }
        END {
            printf "kernel"; for (j = 1; j <= nv; j++) printf "\t%s", vs[j]; printf "\n"
            for (i = 1; i <= nk; i++) {
                printf "%s", ks[i]
                for (j = 1; j <= nv; j++) {
                    q = best[ks[i]] > 0 ? t[ks[i], vs[j]] / best[ks[i]] : 1
                    lg[j] += log(q)
                    printf "\t%.2f%s", q, ((ks[i], vs[j]) in bad) ? "!" : ""
                }
                printf "\n"
            }
            printf "geomean"; for (j = 1; j <= nv; j++) printf "\t%.2f", exp(lg[j] / nk); printf "\n"
            if (!times) exit
            printf "\ntime_s"; for (j = 1; j <= nv; j++) printf "\t%s", vs[j]; printf "\n"
            for (i = 1; i <= nk; i++) {
                printf "%s", ks[i]
                for (j = 1; j <= nv; j++) { printf "\t%.4f", t[ks[i], vs[j]]; lt[j] += log(t[ks[i], vs[j]]) }
                printf "\n"
            }
            printf "geomean"; for (j = 1; j <= nv; j++) printf "\t%.4f", exp(lt[j] / nk); printf "\n"
        }' "$tmp/rows"
fi
! grep -q 'WRONG$' "$tmp/rows"
