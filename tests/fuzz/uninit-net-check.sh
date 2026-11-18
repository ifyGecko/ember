#!/bin/sh
# uninit-net-check.sh RUN BOARD FIRMWARE - --fuzz-uninit in a campaign: uam-run --fuzz-net
# driven by the fuzz brain (fuzz/tools/fuzz_brain.py).
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h with UNWRITTEN_ADDR. One seed ('Uxx') is the
# input that reads RAM nothing wrote. With the option on, ember must send an UNINIT frame
# for it, and the brain must save the input under uninit/ with a .info that names the
# differences and the read (pc and address) and end the session cleanly with ember, both
# exiting 0; the saved input replays as an uninit finding (exit 26). Without the option
# there is no uninit/ at all.
set -eu
run=$1
board=$2
fw=$3
here=$(dirname "$0")
tmp=$(mktemp -d)
brain_pid=
trap '[ -z "$brain_pid" ] || kill "$brain_pid" 2> /dev/null || true; rm -rf "$tmp"' EXIT
fail() { echo "uninit-net-check: $board: FAIL: $*" >&2; exit 1; }

mkdir "$tmp/seeds"
printf 'hello' > "$tmp/seeds/a"
printf 'Uxx' > "$tmp/seeds/b"

# session OUT [uam-run flags]: one brain and one ember; sets ember_rc and brain_rc
session() {
    out=$1
    shift
    port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    python3 "$here/../../fuzz/tools/fuzz_brain.py" --listen "127.0.0.1:$port" --seeds "$tmp/seeds" \
        --out "$out" --iters 600 --seed-rng 1 --quiet > "$tmp/brain.log" 2>&1 &
    brain_pid=$!
    ember_rc=0
    "$run" -b "$board" "$@" --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic --fuzz-input uart0 \
        --fuzz-iter-insns 200000 --fuzz-net "127.0.0.1:$port" "$fw" \
        < /dev/null > "$tmp/run.log" 2>&1 || ember_rc=$?
    brain_rc=0
    wait "$brain_pid" || brain_rc=$?
    brain_pid=
}

session "$tmp/off"
[ $ember_rc -eq 0 ] && [ $brain_rc -eq 0 ] || { cat "$tmp/brain.log" "$tmp/run.log" >&2; fail "without the option: ember $ember_rc, brain $brain_rc"; }
[ ! -e "$tmp/off/uninit" ] || fail "without the option there is an uninit/ directory"

session "$tmp/on" --fuzz-uninit
[ $ember_rc -eq 0 ] && [ $brain_rc -eq 0 ] || { cat "$tmp/brain.log" "$tmp/run.log" >&2; fail "with the option: ember $ember_rc, brain $brain_rc"; }
saved=$(ls "$tmp/on/uninit" 2> /dev/null | grep -v '\.info$' || true)
[ "$(echo "$saved" | grep -c .)" -eq 1 ] || { cat "$tmp/brain.log" >&2; fail "expected one saved input, got: $saved"; }
info="$tmp/on/uninit/$saved.info"
grep -q '^differs=.*path' "$info" || fail "the .info names no difference: $(cat "$info")"
grep -q '^read_before_write=pc 0x[0-9a-f]* at 0x' "$info" || fail "the .info names no read: $(cat "$info")"
case $(head -c 1 "$tmp/on/uninit/$saved") in U) ;; *) fail "the saved input is not the U one" ;; esac
grep -q 'uninit: [0-9]* runs repeated with memory filled, [1-9][0-9]* differed' "$tmp/run.log" \
    || fail "no summary on ember's side: $(grep uninit "$tmp/run.log")"

rc=0
"$run" -b "$board" --fuzz-uninit --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic \
    --fuzz-input uart0 --fuzz-iter-insns 200000 -n 10000000 --fuzz-replay "$tmp/on/uninit" "$fw" \
    < /dev/null > "$tmp/replay.log" 2>&1 || rc=$?
[ $rc -eq 26 ] || { cat "$tmp/replay.log" >&2; fail "the saved input replays with exit $rc, expected 26"; }
echo "uninit-net-check: $board: ok"
