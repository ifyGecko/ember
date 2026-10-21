#!/bin/sh
# brain-check.sh RUN BOARD FIRMWARE SUZZER_DIR - ember's SZR1 executor driven by
# suzzer's own brain (SUZZER_DIR/tools/suzzer_brain.py), not a stand-in.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h, with sites and a begin marker from
# mk_lists.py. One seed is a planted crash (finding it by mutation is the brain's
# business, not ember's): a fixed-seed session must report it, the brain must save it
# with its executor-neutral reason in the .info sidecar and minimize it, and the
# session must end with the brain's BYE, both sides exiting 0. The saved crash
# replays as a crash, and so does the whole crashes/ directory, running only the inputs
# (not their .info notes).
set -eu
run=$1
board=$2
fw=$3
suzzer=$4
here=$(dirname "$0")
tmp=$(mktemp -d)
brain_pid=
trap '[ -z "$brain_pid" ] || kill "$brain_pid" 2> /dev/null || true; rm -rf "$tmp"' EXIT

python3 "$here/../suzzer/mk_lists.py" "$fw" "$tmp" > /dev/null
panic=$(printf '0x%x' "0x$(readelf -sW "$fw" | awk '$8 == "fuzz_panic" { print $2; exit }')")   # unpadded, as the runner prints pcs
mkdir "$tmp/seeds"
printf 'hello' > "$tmp/seeds/a"
printf 'FUZ!x' > "$tmp/seeds/b"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

python3 "$suzzer/tools/suzzer_brain.py" --listen "127.0.0.1:$port" --seeds "$tmp/seeds" \
    --out "$tmp/out" --iters 3000 --seed-rng 1 --quiet > "$tmp/brain.log" 2>&1 &
brain_pid=$!
fail=0
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-boundary "$tmp/begin.lcvb" \
    --cov-ngram 3 --cov-rotate --fuzz-crash-pc "$panic" --fuzz-input uart0 \
    --fuzz-iter-insns 200000 --fuzz-szr "127.0.0.1:$port" "$fw" \
    < /dev/null > "$tmp/run.log" 2>&1 || rc=$?
brc=0
wait "$brain_pid" || brc=$?
brain_pid=
crash=$(ls "$tmp/out/crashes" 2> /dev/null | grep -v '\.info$' | head -n 1 || true)
if [ $rc -ne 0 ] || [ $brc -ne 0 ] || [ -z "$crash" ]; then
    echo "brain-check: $board: executor exit $rc, brain exit $brc, crash '${crash}':" >&2
    cat "$tmp/brain.log" "$tmp/run.log" >&2
    fail=1
elif ! grep -q -E 'GUEST_FAULT|ORACLE' "$tmp/out/crashes/$crash.info"; then
    echo "brain-check: $board: the crash's reason is not named:" >&2
    cat "$tmp/out/crashes/$crash.info" >&2
    fail=1
elif [ "$(wc -c < "$tmp/out/crashes/$crash" | tr -d ' ')" -ge 5 ]; then
    echo "brain-check: $board: the crash was not minimized below the 5-byte seed" >&2
    fail=1
else
    # The saved input reproduces a crash on replay.
    rc=0
    "$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-boundary "$tmp/begin.lcvb" \
        --fuzz-crash-pc "$panic" --fuzz-input uart0 --fuzz-iter-insns 200000 \
        --fuzz-replay "$tmp/out/crashes/$crash" "$fw" < /dev/null > "$tmp/replay.log" 2>&1 || rc=$?
    if [ $rc -ne 20 ] && [ $rc -ne 21 ]; then
        echo "brain-check: $board: the saved crash does not replay as a crash (exit $rc)" >&2
        cat "$tmp/replay.log" >&2
        fail=1
    fi
    # The whole directory replays its inputs, not the brain's .info notes beside them.
    rc=0
    "$run" -b "$board" --cov-sites "$tmp/sites.lcv3" --cov-boundary "$tmp/begin.lcvb" \
        --fuzz-crash-pc "$panic" --fuzz-input uart0 --fuzz-iter-insns 200000 \
        --fuzz-replay "$tmp/out/crashes" "$fw" < /dev/null > "$tmp/replay-dir.log" 2>&1 || rc=$?
    inputs=$(ls "$tmp/out/crashes" | grep -c -v '\.info$')
    ran=$(grep -c '^replay: ' "$tmp/replay-dir.log" || true)
    if grep -q '\.info: ' "$tmp/replay-dir.log" || [ "$ran" -ne "$inputs" ] \
       || { [ $rc -ne 20 ] && [ $rc -ne 21 ]; }; then
        echo "brain-check: $board: replaying the crash directory ran $ran files for $inputs" \
             "inputs (exit $rc):" >&2
        cat "$tmp/replay-dir.log" >&2
        fail=1
    fi
fi
[ $fail -eq 0 ] && echo "brain-check: $board: $(basename "$fw"): suzzer's brain saved and minimized the crash"
exit $fail
