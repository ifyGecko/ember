#!/bin/sh
# brain-check.sh RUN BOARD FIRMWARE - uam-run --fuzz-net driven by the fuzz brain
# (fuzz/tools/fuzz_brain.py), not a stand-in.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h, with sites and a begin marker from
# mk_lists.py. One seed is a planted crash (finding it by mutation is the brain's
# business, not ember's): a fixed-seed session must report it, the brain must save it
# with its reason in the .info sidecar and minimize it, and the
# session must end with the brain's BYE, both sides exiting 0. The saved crash
# replays as a crash, and so does the whole crashes/ directory, running only the inputs
# (not their .info notes). The brain asks for SPARSE replies and must get them.
#
# Then a short session per power schedule (explore, exploit, coe, rare: each must save the
# seeded crash, keep a queue and end cleanly, with no unstable entry), and one with no
# crashing seed at all but an ember dictionary holding the trigger (an integer token in the
# target's byte order and a string): the brain must find the planted FAULT by mutation.
set -eu
run=$1
board=$2
fw=$3
here=$(dirname "$0")
tmp=$(mktemp -d)
brain_pid=
trap '[ -z "$brain_pid" ] || kill "$brain_pid" 2> /dev/null || true; rm -rf "$tmp"' EXIT

python3 "$here/mk_lists.py" "$fw" "$tmp" > /dev/null
panic=$(printf '0x%x' "0x$(readelf -sW "$fw" | awk '$8 == "fuzz_panic" { print $2; exit }')")   # unpadded, as the runner prints pcs
mkdir "$tmp/seeds"
printf 'hello' > "$tmp/seeds/a"
printf 'FUZ!x' > "$tmp/seeds/b"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

python3 "$here/../../fuzz/tools/fuzz_brain.py" --listen "127.0.0.1:$port" --seeds "$tmp/seeds" \
    --out "$tmp/out" --iters 3000 --seed-rng 1 --quiet > "$tmp/brain.log" 2>&1 &
brain_pid=$!
fail=0
rc=0
"$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
    --cov-ngram 3 --cov-rotate --fuzz-crash-pc "$panic" --fuzz-input uart0 \
    --fuzz-iter-insns 200000 --fuzz-net "127.0.0.1:$port" "$fw" \
    < /dev/null > "$tmp/run.log" 2>&1 || rc=$?
brc=0
wait "$brain_pid" || brc=$?
brain_pid=
crash=$(ls "$tmp/out/crashes" 2> /dev/null | grep -v '\.info$' | head -n 1 || true)
if [ $rc -ne 0 ] || [ $brc -ne 0 ] || [ -z "$crash" ]; then
    echo "brain-check: $board: ember exit $rc, brain exit $brc, crash '${crash}':" >&2
    cat "$tmp/brain.log" "$tmp/run.log" >&2
    fail=1
elif ! grep -q -E '^reason_name=(FAULT|GUARD|ORACLE)$' "$tmp/out/crashes/$crash.info"; then
    echo "brain-check: $board: the crash's reason is not named:" >&2
    cat "$tmp/out/crashes/$crash.info" >&2
    fail=1
elif [ "$(wc -c < "$tmp/out/crashes/$crash" | tr -d ' ')" -ge 5 ]; then
    echo "brain-check: $board: the crash was not minimized below the 5-byte seed" >&2
    fail=1
else
    # The saved input reproduces a crash on replay.
    rc=0
    "$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
        --fuzz-crash-pc "$panic" --fuzz-input uart0 --fuzz-iter-insns 200000 \
        --fuzz-replay "$tmp/out/crashes/$crash" "$fw" < /dev/null > "$tmp/replay.log" 2>&1 || rc=$?
    if [ $rc -ne 20 ] && [ $rc -ne 21 ]; then
        echo "brain-check: $board: the saved crash does not replay as a crash (exit $rc)" >&2
        cat "$tmp/replay.log" >&2
        fail=1
    fi
    # The whole directory replays its inputs, not the brain's .info notes beside them.
    rc=0
    "$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
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
# session NAME ITERS EXTRA-BRAIN-ARGS...: one brain + ember session into $tmp/NAME
session() {
    sname=$1; iters=$2; shift 2
    sport=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    python3 "$here/../../fuzz/tools/fuzz_brain.py" --listen "127.0.0.1:$sport" --out "$tmp/$sname" \
        --iters "$iters" --seed-rng 7 --status-file "$tmp/$sname.status" "$@" > "$tmp/$sname.log" 2>&1 &
    brain_pid=$!
    src=0
    "$run" -b "$board" --cov-sites "$tmp/sites.uams" --cov-boundary "$tmp/begin.uamb" \
        --cov-ngram 3 --cov-rotate --fuzz-crash-pc "$panic" --fuzz-input uart0 \
        --fuzz-iter-insns 200000 --fuzz-net "127.0.0.1:$sport" "$fw" \
        < /dev/null > "$tmp/$sname.run" 2>&1 || src=$?
    sbrc=0
    wait "$brain_pid" || sbrc=$?
    brain_pid=
    if [ $src -ne 0 ] || [ $sbrc -ne 0 ]; then
        echo "brain-check: $board: $sname: ember exit $src, brain exit $sbrc:" >&2
        cat "$tmp/$sname.log" "$tmp/$sname.run" >&2
        return 1
    fi
    if ! tail -n 1 "$tmp/$sname.status" | grep -q '"sparse": true'; then
        echo "brain-check: $board: $sname: ember did not send SPARSE replies" >&2
        return 1
    fi
    if ! tail -n 1 "$tmp/$sname.status" | grep -q '"unstable": 0,'; then
        echo "brain-check: $board: $sname: unstable queue entries (ember ran an input differently twice):" >&2
        grep UNSTABLE "$tmp/$sname.log" >&2
        return 1
    fi
    if ! ls "$tmp/$sname/queue" | grep -q '^id:000000,'; then
        echo "brain-check: $board: $sname: no queue saved" >&2
        return 1
    fi
    return 0
}

for sched in explore exploit coe rare; do
    if session "s-$sched" 600 --seeds "$tmp/seeds" --schedule "$sched"; then
        if ! ls "$tmp/s-$sched/crashes" | grep -q -v '\.info$'; then
            echo "brain-check: $board: schedule $sched: the seeded crash was not saved" >&2
            fail=1
        fi
    else
        fail=1
    fi
done

# The trigger only in the dictionary: "FUZ!" as a string and as a u32 in the target's byte
# order (resolved from HELLO), plus a big-endian u16 to exercise explicit orders.
mkdir "$tmp/plain"
printf 'hello' > "$tmp/plain/a"
printf '# the planted bug\n"FUZ!"\nu32be:0x46555a21\nu16be:0x4c78\n' > "$tmp/fuzz.dict"
if session dict 6000 --seeds "$tmp/plain" --dict "$tmp/fuzz.dict"; then
    if ! grep -q -l '^reason_name=FAULT$' "$tmp/dict/crashes/"*.info 2> /dev/null; then
        echo "brain-check: $board: the dictionary did not lead the brain to the planted FAULT:" >&2
        tail -n 5 "$tmp/dict.log" >&2
        fail=1
    elif ! grep -q 'dictionary: 3 tokens (2 integers' "$tmp/dict.log"; then
        echo "brain-check: $board: the dictionary was not loaded as written:" >&2
        grep dictionary "$tmp/dict.log" >&2
        fail=1
    fi
else
    fail=1
fi

[ $fail -eq 0 ] && echo "brain-check: $board: $(basename "$fw"): the brain saved and minimized the crash; every schedule; a dictionary find"
exit $fail
