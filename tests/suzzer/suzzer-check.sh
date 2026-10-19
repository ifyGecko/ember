#!/bin/sh
# suzzer-check.sh RUN BOARD FIRMWARE - suzzer's file formats through uam-run.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h. mk_lists.py writes an LCV3 patch list
# (a site at every function) and an LCVB1 begin marker for it, with no suzzer checkout.
# The check requires
#   - a plain run with --cov-sites to find every site's bytes in memory;
#   - fuzzing with --cov-sites and --cov-boundary (no --fuzz-begin) to give the planted
#     bugs' verdicts and identical maps on every backend, of the size libcov would pick;
#   - longer N-grams (N=3 and N=8, with rotation) to give the same verdicts and identical
#     maps on every backend, of libcov's size for that N;
#   - several END markers each ending an iteration, and --fuzz-end refused next to a
#     boundary file;
#   - the hashed-PC scheme and the image link base (no base table) to work;
#   - --cov-strict to fail on a corrupted site, and N outside 1..8 and unknown
#     --cov-module names to be refused.
set -eu

run=$1
board=$2
fw=$3
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

n=$(python3 "$here/mk_lists.py" "$fw" "$tmp")
panic=0x$(readelf -sW "$fw" | awk '$8 == "fuzz_panic" { print $2; exit }')

# 1. A plain run: every site is where the list says.
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-out "$tmp/plain.map" -n 200000 "$fw" \
    < /dev/null > /dev/null 2> "$tmp/plain.err" || rc=$?
if [ $rc -ne 124 ] || ! grep -q "site check: $n of $n sites match" "$tmp/plain.err"; then
    echo "suzzer-check: $board: plain run (exit $rc):" >&2
    cat "$tmp/plain.err" >&2
    fail=1
fi

# 2. Fuzzing, boundaries from the LCVB1 file.
mkdir "$tmp/in"
printf 'hello' > "$tmp/in/1-ok"
printf 'FUZ!'  > "$tmp/in/2-store"
printf 'FUZ?'  > "$tmp/in/3-panic"
for mode in interp jit jit-nochain; do
    case $mode in
        interp)      flags= ;;
        jit)         flags=--jit ;;
        jit-nochain) flags="--jit --no-chain" ;;
    esac
    rc=0
    # shellcheck disable=SC2086
    "$run" -b "$board" $flags --cov-sites "$tmp/sites.lcv3" --cov-boundary "$tmp/begin.lcvb" \
        --cov-strict --fuzz-crash-pc "$panic" --fuzz-input uart0 --cov-out "$tmp/$mode.map" \
        --fuzz-replay "$tmp/in" "$fw" < /dev/null > "$tmp/$mode.raw" 2> "$tmp/$mode.err" || rc=$?
    grep '^replay: ' "$tmp/$mode.raw" | sed "s|$tmp/in/||" > "$tmp/$mode.out" || true
    if [ $rc -ne 21 ] || ! grep -q '^replay: 1-ok: ok' "$tmp/$mode.out" \
       || ! grep -q '^replay: 2-store: crash-fault' "$tmp/$mode.out" \
       || ! grep -q "^replay: 3-panic: crash-oracle pc=$panic" "$tmp/$mode.out"; then
        echo "suzzer-check: $board $mode: fuzzing with suzzer lists (exit $rc):" >&2
        cat "$tmp/$mode.out" "$tmp/$mode.err" >&2
        fail=1
    fi
done
for mode in jit jit-nochain; do
    if ! cmp -s "$tmp/interp.out" "$tmp/$mode.out" || ! cmp -s "$tmp/interp.map" "$tmp/$mode.map"; then
        echo "suzzer-check: $board: $mode differs from the interpreter" >&2
        fail=1
    fi
done
# libcov's site-id map size for n sites at N=2 (fudge 16, at least 4 KiB).
want=$(python3 -c "n=max($n,1); s=1
while s < n*16: s*=2
print(max(s, 4096))")
size=$(wc -c < "$tmp/interp.map" | tr -d ' ')
if [ "$size" != "$want" ]; then
    echo "suzzer-check: $board: map size $size, libcov would use $want" >&2
    fail=1
fi
if ! grep -q '^replay: 1-ok: ok .*buckets=[1-9]' "$tmp/interp.out"; then
    echo "suzzer-check: $board: the ok input counted no sites" >&2
    fail=1
fi

# 2b. Longer N-grams with rotation: N=3 (what suzzer's brain recommends) and N=8.
for scheme in "3 --cov-rotate" "8 --cov-rotate"; do
    ngram=${scheme%% *}
    tag=n$ngram
    for mode in interp jit jit-nochain; do
        case $mode in
            interp)      flags= ;;
            jit)         flags=--jit ;;
            jit-nochain) flags="--jit --no-chain" ;;
        esac
        rc=0
        # shellcheck disable=SC2086
        "$run" -b "$board" $flags --cov-sites "$tmp/sites.lcv3" --cov-boundary "$tmp/begin.lcvb" \
            --cov-ngram $scheme --cov-strict --fuzz-crash-pc "$panic" --fuzz-input uart0 \
            --cov-out "$tmp/$tag-$mode.map" --fuzz-replay "$tmp/in" "$fw" \
            < /dev/null > "$tmp/$tag-$mode.raw" 2> "$tmp/$tag-$mode.err" || rc=$?
        grep '^replay: ' "$tmp/$tag-$mode.raw" | sed "s|$tmp/in/||" > "$tmp/$tag-$mode.out" || true
        if [ $rc -ne 21 ] || ! grep -q '^replay: 1-ok: ok .*buckets=[1-9]' "$tmp/$tag-$mode.out" \
           || ! grep -q '^replay: 2-store: crash-fault' "$tmp/$tag-$mode.out" \
           || ! grep -q "^replay: 3-panic: crash-oracle pc=$panic" "$tmp/$tag-$mode.out"; then
            echo "suzzer-check: $board $mode: --cov-ngram $scheme (exit $rc):" >&2
            cat "$tmp/$tag-$mode.out" "$tmp/$tag-$mode.err" >&2
            fail=1
        fi
    done
    for mode in jit jit-nochain; do
        if ! cmp -s "$tmp/$tag-interp.out" "$tmp/$tag-$mode.out" \
           || ! cmp -s "$tmp/$tag-interp.map" "$tmp/$tag-$mode.map"; then
            echo "suzzer-check: $board: --cov-ngram $scheme: $mode differs from the interpreter" >&2
            fail=1
        fi
    done
    want=$(python3 -c "n=max($n,1); f=16 << ($ngram - 2); s=1
while s < n*f: s*=2
s=min(s, 1 << 24); t=1
while t < n: t*=2
print(max(s, 4096, t))")
    size=$(wc -c < "$tmp/$tag-interp.map" | tr -d ' ')
    if [ "$size" != "$want" ]; then
        echo "suzzer-check: $board: --cov-ngram $scheme: map size $size, libcov would use $want" >&2
        fail=1
    fi
done

# The history shape reaches the harness: rotation changes the map (the rotation
# amounts also depend on the depth, so N=3 and N=8 differ too). Without rotation the
# fuzz target's short paths give the same map at N=2 and N=3, which is correct.
if cmp -s "$tmp/interp.map" "$tmp/n3-interp.map" || cmp -s "$tmp/n3-interp.map" "$tmp/n8-interp.map"; then
    echo "suzzer-check: $board: --cov-ngram / --cov-rotate did not change the map" >&2
    fail=1
fi

# 2c. Two END markers (paired.lcvb: fuzz_panic and trap_handler). Each ends an iteration
# cleanly; an input that reaches neither runs out of budget.
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-boundary "$tmp/paired.lcvb" \
    --fuzz-crash-exc none --fuzz-input uart0 --fuzz-iter-insns 100000 --fuzz-replay "$tmp/in" "$fw" \
    < /dev/null > "$tmp/ends.raw" 2> "$tmp/ends.err" || rc=$?
grep '^replay: ' "$tmp/ends.raw" | sed "s|$tmp/in/||" > "$tmp/ends.out" || true
if [ $rc -ne 0 ] || ! grep -q '^replay: 1-ok: timeout' "$tmp/ends.out" \
   || ! grep -q '^replay: 2-store: ok' "$tmp/ends.out" || ! grep -q '^replay: 3-panic: ok' "$tmp/ends.out"; then
    echo "suzzer-check: $board: two END markers (exit $rc):" >&2
    cat "$tmp/ends.out" "$tmp/ends.err" >&2
    fail=1
fi
rc=0
"$run" -b "$board" --cov-boundary "$tmp/paired.lcvb" --fuzz-end 0x4 --fuzz-input uart0 \
    --fuzz-replay "$tmp/in" "$fw" < /dev/null > /dev/null 2> "$tmp/conflict.err" || rc=$?
if [ $rc -ne 2 ]; then
    echo "suzzer-check: $board: --fuzz-end with --cov-boundary was not refused (exit $rc)" >&2
    fail=1
fi

# 3. The hashed-PC model; the image link base instead of a base table.
python3 "$here/mk_lists.py" "$fw" "$tmp" --no-bases > /dev/null
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-scheme pc --cov-ngram 1 --cov-strict \
    --cov-out "$tmp/pc.map" -n 200000 "$fw" < /dev/null > /dev/null 2> "$tmp/pc.err" || rc=$?
if [ $rc -ne 124 ] || [ "$(wc -c < "$tmp/pc.map" | tr -d ' ')" != 65536 ]; then
    echo "suzzer-check: $board: hashed-PC run (exit $rc):" >&2
    cat "$tmp/pc.err" >&2
    fail=1
fi

# 4. Refusals.
python3 "$here/mk_lists.py" "$fw" "$tmp" --bad-orig > /dev/null
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-strict -n 100000 "$fw" \
    < /dev/null > /dev/null 2> "$tmp/strict.err" || rc=$?
if [ $rc -ne 2 ] || ! grep -q "do not match" "$tmp/strict.err"; then
    echo "suzzer-check: $board: --cov-strict with a bad site: exit $rc" >&2
    fail=1
fi
for bad in "--cov-ngram 0" "--cov-ngram 9" "--cov-module nosuch=0x1000"; do
    rc=0
    # shellcheck disable=SC2086
    "$run" -b "$board" --cov-sites "$tmp/sites.lcv3" $bad -n 1000 "$fw" \
        < /dev/null > /dev/null 2> "$tmp/bad.err" || rc=$?
    if [ $rc -ne 2 ]; then
        echo "suzzer-check: $board: '$bad' was not refused (exit $rc)" >&2
        fail=1
    fi
done

[ $fail -eq 0 ] && echo "suzzer-check: $board: $(basename "$fw"): $n sites; formats, schemes and backends agree"
exit $fail
