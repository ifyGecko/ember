#!/bin/sh
# formats-check.sh RUN BOARD FIRMWARE - site lists, boundary files and the coverage region
# through uam-run.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h. mk_lists.py writes a site list (a site
# at every function) and boundary files for it (docs/fuzz-formats.md). The check requires
#   - a plain run with --cov-sites to find every site's bytes in memory;
#   - fuzzing with --cov-sites and --cov-boundary (no --fuzz-begin) to give the planted
#     bugs' verdicts and identical maps on every backend, of the default map size;
#   - longer N-grams (N=3 and N=8, with rotation) to give the same verdicts and identical
#     maps on every backend, of the default size for that N;
#   - several END markers each ending an iteration, and --fuzz-end refused next to a
#     boundary file;
#   - --cov-shm to publish INIT and the ok iterations (not the crashes) in a coverage
#     region that an independent reader (cov_region_check.py) and fuzz/tools/cov_region.py
#     both read back;
#   - the hashed-pc scheme and the image link base (no base table) to work;
#   - --cov-strict to fail on a corrupted site, and N outside 1..8 and unknown
#     --cov-module names to be refused.
set -eu

run=$1
board=$2
fw=$3
here=$(dirname "$0")
tools=$here/../../fuzz/tools
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

n=$(python3 "$here/mk_lists.py" "$fw" "$tmp")
panic=$(printf '0x%x' "0x$(readelf -sW "$fw" | awk '$8 == "fuzz_panic" { print $2; exit }')")   # unpadded, as the runner prints pcs

# 1. A plain run: every site is where the list says.
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-out "$tmp/plain.map" -n 200000 "$fw" \
    < /dev/null > /dev/null 2> "$tmp/plain.err" || rc=$?
if [ $rc -ne 124 ] || ! grep -q "site check: $n of $n sites match" "$tmp/plain.err"; then
    echo "formats-check: $board: plain run (exit $rc):" >&2
    cat "$tmp/plain.err" >&2
    fail=1
fi

# 2. Fuzzing, boundaries from the boundary file.
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
    "$run" -b "$board" $flags --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
        --cov-strict --fuzz-crash-pc "$panic" --fuzz-input uart0 --cov-out "$tmp/$mode.map" \
        --fuzz-replay "$tmp/in" "$fw" < /dev/null > "$tmp/$mode.raw" 2> "$tmp/$mode.err" || rc=$?
    grep '^replay: ' "$tmp/$mode.raw" | sed "s|$tmp/in/||" > "$tmp/$mode.out" || true
    if [ $rc -ne 20 ] || ! grep -q '^replay: 1-ok: ok' "$tmp/$mode.out" \
       || ! grep -q '^replay: 2-store: crash-fault' "$tmp/$mode.out" \
       || ! grep -q "^replay: 3-panic: crash-oracle pc=$panic" "$tmp/$mode.out"; then
        echo "formats-check: $board $mode: fuzzing with site lists (exit $rc):" >&2
        cat "$tmp/$mode.out" "$tmp/$mode.err" >&2
        fail=1
    fi
done
for mode in jit jit-nochain; do
    if ! cmp -s "$tmp/interp.out" "$tmp/$mode.out" || ! cmp -s "$tmp/interp.map" "$tmp/$mode.map"; then
        echo "formats-check: $board: $mode differs from the interpreter" >&2
        fail=1
    fi
done
# The default site-id map size for n sites at N=2 (16 buckets a site, at least 4 KiB).
want=$(python3 -c "n=max($n,1); s=1
while s < n*16: s*=2
print(max(s, 4096))")
size=$(wc -c < "$tmp/interp.map" | tr -d ' ')
if [ "$size" != "$want" ]; then
    echo "formats-check: $board: map size $size, expected $want" >&2
    fail=1
fi
if ! grep -q '^replay: 1-ok: ok .*buckets=[1-9]' "$tmp/interp.out"; then
    echo "formats-check: $board: the ok input counted no sites" >&2
    fail=1
fi

# 2b. Longer N-grams with rotation: N=3 (what the brain recommends) and N=8.
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
        "$run" -b "$board" $flags --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
            --cov-ngram $scheme --cov-strict --fuzz-crash-pc "$panic" --fuzz-input uart0 \
            --cov-out "$tmp/$tag-$mode.map" --fuzz-replay "$tmp/in" "$fw" \
            < /dev/null > "$tmp/$tag-$mode.raw" 2> "$tmp/$tag-$mode.err" || rc=$?
        grep '^replay: ' "$tmp/$tag-$mode.raw" | sed "s|$tmp/in/||" > "$tmp/$tag-$mode.out" || true
        if [ $rc -ne 20 ] || ! grep -q '^replay: 1-ok: ok .*buckets=[1-9]' "$tmp/$tag-$mode.out" \
           || ! grep -q '^replay: 2-store: crash-fault' "$tmp/$tag-$mode.out" \
           || ! grep -q "^replay: 3-panic: crash-oracle pc=$panic" "$tmp/$tag-$mode.out"; then
            echo "formats-check: $board $mode: --cov-ngram $scheme (exit $rc):" >&2
            cat "$tmp/$tag-$mode.out" "$tmp/$tag-$mode.err" >&2
            fail=1
        fi
    done
    for mode in jit jit-nochain; do
        if ! cmp -s "$tmp/$tag-interp.out" "$tmp/$tag-$mode.out" \
           || ! cmp -s "$tmp/$tag-interp.map" "$tmp/$tag-$mode.map"; then
            echo "formats-check: $board: --cov-ngram $scheme: $mode differs from the interpreter" >&2
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
        echo "formats-check: $board: --cov-ngram $scheme: map size $size, expected $want" >&2
        fail=1
    fi
done

# The history shape reaches the harness: rotation changes the map. Compared on the ok
# input, whose path has two sites after the begin: an iteration's first count has an
# empty history and is the same bucket under every scheme, and the crashing inputs count
# only that one. Two sites use only hist[0], which N=3 and N=8 both rotate by 1, so those
# two agree here; the golden maps (test_cov_formats) cover the deeper histories.
for scheme in 2 "3 --cov-rotate"; do
    # shellcheck disable=SC2086
    "$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
        --cov-ngram $scheme --cov-map-size 4096 --fuzz-input uart0 \
        --cov-out "$tmp/ok-n${scheme%% *}.map" --fuzz-replay "$tmp/in/1-ok" "$fw" \
        < /dev/null > /dev/null 2>&1 || true
done
if cmp -s "$tmp/ok-n2.map" "$tmp/ok-n3.map"; then
    echo "formats-check: $board: --cov-ngram / --cov-rotate did not change the map" >&2
    fail=1
fi

# 2c. Two END markers (paired.uamb: fuzz_panic and trap_handler). Each ends an iteration
# cleanly; an input that reaches neither runs out of budget.
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/paired.uamb" \
    --fuzz-crash-exc none --fuzz-input uart0 --fuzz-iter-insns 100000 --fuzz-replay "$tmp/in" "$fw" \
    < /dev/null > "$tmp/ends.raw" 2> "$tmp/ends.err" || rc=$?
grep '^replay: ' "$tmp/ends.raw" | sed "s|$tmp/in/||" > "$tmp/ends.out" || true
if [ $rc -ne 23 ] || ! grep -q '^replay: 1-ok: timeout' "$tmp/ends.out" \
   || ! grep -q '^replay: 2-store: ok' "$tmp/ends.out" || ! grep -q '^replay: 3-panic: ok' "$tmp/ends.out"; then
    echo "formats-check: $board: two END markers (exit $rc):" >&2
    cat "$tmp/ends.out" "$tmp/ends.err" >&2
    fail=1
fi
rc=0
"$run" -b "$board" --cov-boundary "$tmp/paired.uamb" --fuzz-end 0x4 --fuzz-input uart0 \
    --fuzz-replay "$tmp/in" "$fw" < /dev/null > /dev/null 2> "$tmp/conflict.err" || rc=$?
if [ $rc -ne 2 ]; then
    echo "formats-check: $board: --fuzz-end with --cov-boundary was not refused (exit $rc)" >&2
    fail=1
fi

# 2d. The coverage region: INIT, then the two ok inputs; the crash in between is not published.
mkdir "$tmp/shm-in"
cp "$tmp/in/1-ok" "$tmp/shm-in/1-ok"
cp "$tmp/in/2-store" "$tmp/shm-in/2-store"
printf 'okay' > "$tmp/shm-in/3-ok"
for i in 1-ok 3-ok; do
    "$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
        --fuzz-input uart0 --cov-out "$tmp/shm-$i.map" --fuzz-replay "$tmp/shm-in/$i" "$fw" \
        < /dev/null > /dev/null 2>&1 || true
done
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
    --fuzz-input uart0 --cov-shm "$tmp/region" --cov-shm-ring 4 --fuzz-replay "$tmp/shm-in" "$fw" \
    < /dev/null > /dev/null 2> "$tmp/shm.err" || rc=$?
# flags: SCHEME | N=2 (site model, no rotation)
if [ $rc -ne 20 ] \
   || ! python3 "$here/cov_region_check.py" "$tmp/region" 0x201 "$tmp/shm-1-ok.map" "$tmp/shm-3-ok.map" \
        > "$tmp/shm.out" 2>&1 \
   || [ "$(python3 "$tools/cov_region.py" gen "$tmp/region")" != 3 ] \
   || [ "$(python3 "$tools/cov_region.py" scheme "$tmp/region")" != "n=2 model=site rotate=0" ]; then
    echo "formats-check: $board: --cov-shm (exit $rc):" >&2
    cat "$tmp/shm.err" "$tmp/shm.out" >&2
    fail=1
fi
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-shm "$tmp/region2" -n 1000 "$fw" \
    < /dev/null > /dev/null 2>&1 || rc=$?
if [ $rc -ne 2 ]; then
    echo "formats-check: $board: --cov-shm without fuzzing was not refused (exit $rc)" >&2
    fail=1
fi

# 3. The hashed-pc model; the image link base instead of a base table.
python3 "$here/mk_lists.py" "$fw" "$tmp" --no-bases > /dev/null
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-scheme pc --cov-ngram 1 --cov-strict \
    --cov-out "$tmp/pc.map" -n 200000 "$fw" < /dev/null > /dev/null 2> "$tmp/pc.err" || rc=$?
if [ $rc -ne 124 ] || [ "$(wc -c < "$tmp/pc.map" | tr -d ' ')" != 65536 ]; then
    echo "formats-check: $board: hashed-pc run (exit $rc):" >&2
    cat "$tmp/pc.err" >&2
    fail=1
fi

# 4. Refusals.
python3 "$here/mk_lists.py" "$fw" "$tmp" --bad-orig > /dev/null
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-strict -n 100000 "$fw" \
    < /dev/null > /dev/null 2> "$tmp/strict.err" || rc=$?
if [ $rc -ne 2 ] || ! grep -q "do not match" "$tmp/strict.err"; then
    echo "formats-check: $board: --cov-strict with a bad site: exit $rc" >&2
    fail=1
fi
for bad in "--cov-ngram 0" "--cov-ngram 9" "--cov-module nosuch=0x1000"; do
    rc=0
    # shellcheck disable=SC2086
    "$run" -b "$board" --cov-sites "$tmp/sites.uams" $bad -n 1000 "$fw" \
        < /dev/null > /dev/null 2> "$tmp/bad.err" || rc=$?
    if [ $rc -ne 2 ]; then
        echo "formats-check: $board: '$bad' was not refused (exit $rc)" >&2
        fail=1
    fi
done

[ $fail -eq 0 ] && echo "formats-check: $board: $(basename "$fw"): $n sites; formats, schemes and backends agree"
exit $fail
