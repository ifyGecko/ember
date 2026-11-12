#!/bin/sh
# parallel-check.sh RUN BOARD FIRMWARE - a parallel campaign through fuzz/tools/fuzz_launch.py.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h. Two workers (main on fast, w1 on rare)
# with distinct seeds, no crashing seed and a dictionary holding both triggers, sharing
# through one sync directory every second. Required:
#   - both workers end cleanly and the launcher exits 0;
#   - at least one queue entry was imported from the other worker (a sync: name);
#   - between them, the workers saved the planted FAULT ("FUZ!") and ORACLE ("FUZ?");
#   - killing w1's ember mid-run makes the launcher restart the worker, which resumes from
#     its queue (its log says so) and still ends cleanly;
#   - status_tui.py --sync-dir shows both workers.
set -eu
run=$1
board=$2
fw=$3
here=$(cd "$(dirname "$0")" && pwd)
tools=$here/../../fuzz/tools
tmp=$(mktemp -d)
lpid=
trap '[ -z "$lpid" ] || kill "$lpid" 2> /dev/null || true; rm -rf "$tmp"' EXIT
fail() { echo "parallel-check: $board: FAIL: $*" >&2; exit 1; }

sym() { printf '0x%x' "0x$(readelf -sW "$fw" | awk -v n="$1" '$NF == n { print $2; exit }')"; }
begin=$(sym fuzz_iteration)
panic=$(sym fuzz_panic)
mkdir "$tmp/seeds"
printf 'hello' > "$tmp/seeds/a"
printf '"FUZ!"\n"FUZ?"\n' > "$tmp/fuzz.dict"

python3 "$tools/fuzz_launch.py" -n 2 --out "$tmp/camp" --seeds "$tmp/seeds" \
    --dict "$tmp/fuzz.dict" --schedules fast,rare --iters 30000 --sync-interval 1 \
    --status-interval 0.5 --seed-rng 11 -- \
    "$run" -b "$board" --cov-blocks --fuzz-begin "$begin" --fuzz-crash-pc "$panic" \
    --fuzz-input uart0 --fuzz-iter-insns 200000 "$fw" > "$tmp/launch.log" 2>&1 &
lpid=$!

# Kill w1's ember once it is fuzzing: the launcher must restart the worker.
port=
for _ in $(seq 1 100); do
    port=$(sed -n 's/.*listening on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$tmp/camp/w1/brain.log" 2> /dev/null | head -n 1)
    [ -n "$port" ] && grep -q 'baseline:' "$tmp/camp/w1/brain.log" 2> /dev/null && break
    sleep 0.1
done
[ -n "$port" ] || { cat "$tmp/launch.log" >&2; fail "w1 never started"; }
sleep 1
epid=$(pgrep -f -- "--fuzz-net 127.0.0.1:$port" | head -n 1 || true)
[ -n "$epid" ] || fail "w1's ember (port $port) not found"
kill -9 "$epid"

rc=0
wait "$lpid" || rc=$?
lpid=
[ $rc -eq 0 ] || { cat "$tmp/launch.log" >&2; fail "fuzz_launch exited $rc"; }
grep -q 'w1: exited unexpectedly.*restarting from its queue' "$tmp/launch.log" ||
    { cat "$tmp/launch.log" >&2; fail "the killed worker was not restarted"; }
grep -q 'resumed [1-9][0-9]* queue entries' "$tmp/camp/w1/brain.log" ||
    fail "the restarted worker did not resume from its queue"
for w in main w1; do
    grep -q "^fuzz_launch: $w: finished (brain 0, ember 0)" "$tmp/launch.log" ||
        { cat "$tmp/launch.log" >&2; fail "$w did not end cleanly"; }
done
ls "$tmp/camp/main/queue" "$tmp/camp/w1/queue" | grep -q ',sync:' ||
    fail "no queue entry was imported from the other worker"
for r in FAULT ORACLE; do
    grep -q -l "^reason_name=$r\$" "$tmp"/camp/*/crashes/*.info 2> /dev/null ||
        fail "the workers did not find the planted $r"
done
python3 "$tools/status_tui.py" --sync-dir "$tmp/camp" --once > "$tmp/tui.txt"
grep -q ' main ' "$tmp/tui.txt" && grep -q ' w1 ' "$tmp/tui.txt" && grep -q '2 workers' "$tmp/tui.txt" ||
    { cat "$tmp/tui.txt" >&2; fail "the workers view does not show both workers"; }
echo "parallel-check: $board: two workers shared their queues, found both planted crashes, survived a killed ember"
