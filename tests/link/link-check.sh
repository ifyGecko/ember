#!/bin/sh
# link-check.sh HUB RUNNER TEST_RUNNER LINKTEST_ELF FUZZTARGET_ELF - links between
# processes (uam/link.h, docs/link.md): a uam-link-hub and two emulations of
# tests/firmware/linktest.c on rv32-test, which exchange numbered messages.
#   free         frames through the hub; 17-byte frames arrive 17 bytes long
#   loose        the same with a bounded skew
#   exact        the same in lockstep: the guests' final lines (count, time, interrupts)
#                and the --trace link output are identical in every run, on the
#                interpreter and the JIT, sleeping in WFI or polling
#   bytes        the console as the link (--console link:PORT), one message a byte
#   a device     the board's own device on a link port (rv32-test's msg0), beside a second
#                port that is not connected: two nodes in one emulation
#   three nodes  a broadcast bus: everyone hears everyone
#   refusals     an exact hub refuses a port without latency; a name is taken once
#   quiescence   two emulations that both wait for the other, in lockstep: nothing will
#                ever happen, and both stop as a deadlock (exit 125)
#   hub loss     the emulation stops with exit 132
#   snapshot     --link-stop-at on both, --save-snapshot, then both resumed on a new hub:
#                the run ends exactly as the uninterrupted one
#   fuzzing      refused while connected to a hub; an unconnected port is a fuzz input
#                (--fuzz-input PORT), with the same verdicts as the UART's own input
set -eu
HUB=$1
RUN=$2
TRUN=$3
FW=$4
FUZZ=$5
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT
fail() { echo "link-check: FAIL: $*" >&2; exit 1; }

# hub ARGS...: start a hub on $T/hub.sock and wait until it listens.
hub() {
    rm -f "$T/hub.sock"
    "$HUB" "unix:$T/hub.sock" "$@" 2>"$T/hub.err" &
    HUBPID=$!
    i=0
    while [ ! -S "$T/hub.sock" ]; do
        i=$((i + 1))
        [ $i -lt 100 ] || fail "the hub did not start: $(cat "$T/hub.err")"
        sleep 0.05
    done
}

# node NAME ROLE MODE ROUNDS(hex, 4 digits HH:LL) EXTRA...: one emulation, in the background.
node() {
    name=$1 role=$2 mode=$3 rounds=$4
    shift 4
    timeout 120 "$TRUN" -b rv32-test --link "p0=unix:$T/hub.sock,bus=wire,latency=50us" \
        --link-name "$name" --nic net0=link:p0 -o "net0.mac=02:00:$rounds:$mode:$role" \
        --console "file:$T/$name.out" "$@" "$FW" >"$T/$name.log" 2>&1 &
}

# pair TAG SYNC MODE EXTRA...: a hub and two nodes; leaves $T/TAG.a and $T/TAG.b (consoles).
pair() {
    tag=$1 sync=$2 mode=$3
    shift 3
    hub --sync "$sync" --nodes 2      # --nodes: nobody sends before the other is there
    node a 01 "$mode" 00:64 "$@"
    pa=$!
    node b 02 "$mode" 00:64 "$@"
    pb=$!
    ra=0; rb=0
    wait $pa || ra=$?
    wait $pb || rb=$?
    wait $HUBPID || true
    [ $ra -eq 0 ] && [ $rb -eq 0 ] \
        || fail "$tag: exit $ra / $rb: $(cat "$T/a.log" "$T/b.log" "$T/hub.err")"
    tr -d '\r' <"$T/a.out" >"$T/$tag.a"
    tr -d '\r' <"$T/b.out" >"$T/$tag.b"
    grep -q 'linktest: PASS rounds=100' "$T/$tag.a" && grep -q 'linktest: PASS rounds=100' "$T/$tag.b" \
        || fail "$tag: no PASS line: $(cat "$T/$tag.a" "$T/$tag.b")"
}

# ---- free and loose ------------------------------------------------------------
pair free free 00
pair free-poll free 02
pair loose loose 00
pair loose-tight loose=20us 02

# ---- exact: the same in every run, on every backend --------------------------------
pair exact1 exact 00
# ,pcap=FILE works on a link port as on any other: 100 frames of 17 bytes, each seen once
# going out and once coming in over the pair, behind the 24-byte file header.
hub --sync exact --nodes 2
timeout 120 "$TRUN" -b rv32-test --link "p0=unix:$T/hub.sock,bus=wire,latency=50us" --link-name a \
    --nic "net0=link:p0,pcap=$T/a.pcap" -o net0.mac=02:00:00:64:00:01 --console null "$FW" >"$T/a.log" 2>&1 &
pa=$!
node b 02 00 00:64
pb=$!
wait $pa || fail "pcap: a failed: $(cat "$T/a.log")"
wait $pb || fail "pcap: b failed"
wait $HUBPID || true
[ "$(wc -c <"$T/a.pcap")" -eq $((24 + 101 * (16 + 17))) ] \
    || fail "pcap on a link port: $(wc -c <"$T/a.pcap") bytes, wanted $((24 + 101 * 33))"
for run in 1 2; do
    hub --sync exact --nodes 2
    node a 01 00 00:64 --trace link --trace-file "$T/x$run.a.trace"
    pa=$!
    node b 02 00 00:64 --trace link --trace-file "$T/x$run.b.trace"
    pb=$!
    wait $pa || fail "exact run $run: a failed: $(cat "$T/a.log")"
    wait $pb || fail "exact run $run: b failed: $(cat "$T/b.log")"
    wait $HUBPID || true
    tr -d '\r' <"$T/a.out" >"$T/x$run.a"
    tr -d '\r' <"$T/b.out" >"$T/x$run.b"
done
cmp -s "$T/x1.a" "$T/x2.a" && cmp -s "$T/x1.b" "$T/x2.b" \
    || fail "exact: two runs ended differently: $(cat "$T/x1.a" "$T/x2.a")"
cmp -s "$T/x1.a" "$T/exact1.a" || fail "exact: a third run ended differently"
grep -q '^link p0 tx ' "$T/x1.a.trace" && grep -q '^link p0 rx ' "$T/x1.a.trace" \
    || fail "--trace link: no tx/rx lines: $(head -3 "$T/x1.a.trace")"
grep -q 'len=17 ' "$T/x1.a.trace" || fail "--trace link: frames are not 17 bytes long"
cmp -s "$T/x1.a.trace" "$T/x2.a.trace" && cmp -s "$T/x1.b.trace" "$T/x2.b.trace" \
    || fail "exact: the link traces of two runs differ"
[ "$(grep -c '^link p0 rx ' "$T/x1.a.trace")" -eq 50 ] || fail "exact: a did not receive 50 frames"
# One latency is 50 us = 5000 ticks at 100 M ticks/s: a hundred frames take at least that long.
mtime=$(sed -n 's/.*mtime=\([0-9]*\).*/\1/p' "$T/x1.a")
[ "$mtime" -ge 500000 ] || fail "exact: 100 frames at 50 us each ended at tick $mtime"

pair exact-jit exact 00 --jit
cmp -s "$T/x1.a" "$T/exact-jit.a" && cmp -s "$T/x1.b" "$T/exact-jit.b" \
    || fail "exact: the JIT ended differently from the interpreter: $(cat "$T/x1.a" "$T/exact-jit.a")"
pair exact-poll1 exact 02
pair exact-poll2 exact 02 --jit
cmp -s "$T/exact-poll1.a" "$T/exact-poll2.a" && cmp -s "$T/exact-poll1.b" "$T/exact-poll2.b" \
    || fail "exact, polling: the JIT ended differently from the interpreter"

# ---- bytes: the console is the link ------------------------------------------------
for sync in free exact; do
    hub --sync "$sync" --nodes 2
    for n in a b; do
        role=02; [ $n = a ] && role=01
        timeout 120 "$TRUN" -b rv32-test --link "u0=unix:$T/hub.sock,bus=serial,latency=87us" \
            --link-name $n --console link:u0 --nic net0=null -o "net0.mac=02:00:00:c8:01:$role" \
            "$FW" >"$T/$n.log" 2>&1 &
        eval "p$n=\$!"
    done
    wait $pa || fail "bytes ($sync): a failed ($?): $(cat "$T/a.log")"
    wait $pb || fail "bytes ($sync): b failed ($?): $(cat "$T/b.log")"
    wait $HUBPID || true
done

# ---- a device of the board's own on a link port ----------------------------------------
# msg0 is taken by the board (uam_board_config_link); spare is a second port on no hub,
# so each emulation has two nodes, and their state two places in a snapshot.
for sync in free exact; do
    hub --sync "$sync" --nodes 2
    for n in a b; do
        role=02; [ $n = a ] && role=01
        timeout 120 "$TRUN" -b rv32-test --link "msg0=unix:$T/hub.sock,bus=words,latency=20us" \
            --link spare=none --link-name $n --nic net0=link:spare -o "net0.mac=02:00:00:c8:08:$role" \
            --console null --save-snapshot "$T/dev-$n.snap" "$FW" >"$T/$n.log" 2>&1 &
        eval "p$n=\$!"
    done
    wait $pa || fail "device ($sync): a failed ($?): $(cat "$T/a.log")"
    wait $pb || fail "device ($sync): b failed ($?): $(cat "$T/b.log")"
    wait $HUBPID || true
    [ -s "$T/dev-a.snap" ] || fail "device ($sync): no snapshot with two nodes"
done
# The ports are part of what a snapshot is of: given in another order, it does not fit.
rc=0
"$TRUN" -b rv32-test --link spare=none --link msg0=none --link-name a --nic net0=link:spare \
    -o net0.mac=02:00:00:c8:08:01 --console null --load-snapshot "$T/dev-a.snap" --max-insns 1000 \
    "$FW" >"$T/out" 2>&1 || rc=$?
grep -q "cannot resume from snapshot" "$T/out" \
    || fail "a snapshot was loaded onto other link ports (exit $rc): $(cat "$T/out")"
rc=0
"$TRUN" -b rv32-test --link p0=none --link-name "a-name-that-is-far-too-long-for-a-hub" "$FW" \
    >"$T/out" 2>&1 || rc=$?
[ $rc -eq 2 ] && grep -q 'link-name' "$T/out" || fail "an over-long --link-name was taken (exit $rc)"

# ---- three nodes on a broadcast bus --------------------------------------------------
# a starts; b and c both answer every frame they have not seen the like of, so each
# hears the other's answers too: a number out of order (exit 3) is what a bus where
# everyone hears everyone produces, and a clean pass would mean someone heard too little.
hub --sync exact --nodes 3
node a 01 00 00:04
pa=$!
node b 02 00 00:04
pb=$!
node c 03 00 00:04
pc=$!
ra=0; rb=0; rc=0
wait $pa || ra=$?
wait $pb || rb=$?
wait $pc || rc=$?
kill $HUBPID 2>/dev/null || true
wait $HUBPID 2>/dev/null || true
[ $rb -eq 3 ] || [ $rc -eq 3 ] || [ $ra -eq 3 ] \
    || fail "three nodes: nobody heard a second answer (exit $ra / $rb / $rc)"

# ---- nothing will ever happen ----------------------------------------------------------
hub --sync exact --nodes 2
node a 02 00 00:64
pa=$!
node b 03 00 00:64
pb=$!
ra=0; rb=0
wait $pa || ra=$?
wait $pb || rb=$?
wait $HUBPID || true
[ $ra -eq 125 ] && [ $rb -eq 125 ] \
    || fail "two emulations waiting for each other: exit $ra / $rb, wanted 125: $(cat "$T/a.log" "$T/b.log")"

# ---- refusals ------------------------------------------------------------------------
hub --sync exact
rc=0
"$TRUN" -b rv32-test --link "p0=unix:$T/hub.sock" --nic net0=link:p0 "$FW" >"$T/out" 2>&1 || rc=$?
[ $rc -eq 2 ] && grep -q 'needs a latency' "$T/out" \
    || fail "an exact hub took a port without latency (exit $rc): $(cat "$T/out")"
node a 02 00 00:64
pa=$!
sleep 0.3
rc=0
"$TRUN" -b rv32-test --link "p0=unix:$T/hub.sock,latency=1us" --link-name a --nic net0=link:p0 \
    "$FW" >"$T/out" 2>&1 || rc=$?
[ $rc -eq 2 ] && grep -q 'already connected' "$T/out" \
    || fail "a second node of the same name was taken (exit $rc): $(cat "$T/out")"

# ---- hub loss ------------------------------------------------------------------------
# a (the answering role) waits for a frame that never comes; then the hub dies.
kill $HUBPID
ra=0
wait $pa || ra=$?
[ $ra -eq 132 ] && grep -q 'link hub went away' "$T/a.log" \
    || fail "hub loss: exit $ra, wanted 132: $(cat "$T/a.log")"
rc=0
"$TRUN" -b rv32-test --link "p0=unix:$T/nothing.sock" --nic net0=link:p0 "$FW" >"$T/out" 2>&1 || rc=$?
[ $rc -eq 2 ] && grep -q 'cannot reach a hub' "$T/out" || fail "no hub: exit $rc: $(cat "$T/out")"
rc=0
"$TRUN" -b rv32-test --nic net0=link:nope "$FW" >"$T/out" 2>&1 || rc=$?
[ $rc -eq 2 ] && grep -q 'no --link port of that name' "$T/out" || fail "unknown port: exit $rc"
"$TRUN" -b rv32-test --link "p9=none" --nic net0=null --console null --max-insns 100000 "$FW" \
    >"$T/out" 2>&1 </dev/null || true
grep -q "nothing uses link port 'p9'" "$T/out" || fail "an unused port is not reported: $(cat "$T/out")"

# ---- one snapshot of both, resumed -----------------------------------------------------
hub --sync exact --nodes 2
node a 01 00 00:64 --link-stop-at 250ms --save-snapshot "$T/a.snap"
pa=$!
node b 02 00 00:64 --link-stop-at 250ms --save-snapshot "$T/b.snap"
pb=$!
ra=0; rb=0
wait $pa || ra=$?
wait $pb || rb=$?
wait $HUBPID || true
# 250 ms is far beyond the exchange (5 ms): both would have finished. Stop inside it.
[ $ra -eq 0 ] && [ $rb -eq 0 ] || fail "snapshot: a run to the end failed ($ra / $rb)"
hub --sync exact --nodes 2
node a 01 00 00:64 --link-stop-at 2ms --save-snapshot "$T/a.snap"
pa=$!
node b 02 00 00:64 --link-stop-at 2ms --save-snapshot "$T/b.snap"
pb=$!
ra=0; rb=0
wait $pa || ra=$?
wait $pb || rb=$?
wait $HUBPID || true
[ $ra -eq 0 ] && [ $rb -eq 0 ] && [ -s "$T/a.snap" ] && [ -s "$T/b.snap" ] \
    || fail "snapshot: stopping both at 2 ms failed ($ra / $rb): $(cat "$T/a.log" "$T/b.log")"
! grep -q 'linktest: PASS' "$T/a.out" || fail "snapshot: a had already finished at 2 ms"
hub --sync exact --nodes 2
node a 01 00 00:64 --load-snapshot "$T/a.snap"
pa=$!
node b 02 00 00:64 --load-snapshot "$T/b.snap"
pb=$!
ra=0; rb=0
wait $pa || ra=$?
wait $pb || rb=$?
wait $HUBPID || true
[ $ra -eq 0 ] && [ $rb -eq 0 ] || fail "snapshot: the resumed pair failed ($ra / $rb): $(cat "$T/a.log" "$T/b.log")"
tr -d '\r' <"$T/a.out" >"$T/resumed.a"
tr -d '\r' <"$T/b.out" >"$T/resumed.b"
cmp -s "$T/x1.a" "$T/resumed.a" && cmp -s "$T/x1.b" "$T/resumed.b" \
    || fail "snapshot: resumed, the pair ended differently: $(cat "$T/x1.a" "$T/resumed.a" "$T/x1.b" "$T/resumed.b")"

# ---- fuzzing ---------------------------------------------------------------------------
mkdir "$T/raw" "$T/rec"
printf 'hello' >"$T/raw/1-ok"
printf 'FUZ!' >"$T/raw/2-store"
printf '\005\000hello' >"$T/rec/1-ok"
printf '\002\000FU\002\000Z!' >"$T/rec/2-store"         # two messages, the bytes in order
rc=0
"$RUN" -b rv32-ref --fuzz-begin fuzz_iteration --fuzz-input uart0 --fuzz-replay "$T/raw" "$FUZZ" \
    </dev/null >"$T/uart.raw" 2>"$T/uart.err" || rc=$?
grep '^replay: ' "$T/uart.raw" | sed "s|$T/raw/||" >"$T/uart.out"
grep -q '^replay: 2-store: crash-fault' "$T/uart.out" || fail "fuzz: the UART input finds no crash: $(cat "$T/uart.raw" "$T/uart.err")"
rc2=0
"$RUN" -b rv32-ref --link u0=none --console link:u0 --fuzz-begin fuzz_iteration --fuzz-input u0 \
    --fuzz-replay "$T/rec" "$FUZZ" </dev/null >"$T/port.raw" 2>"$T/port.err" || rc2=$?
grep '^replay: ' "$T/port.raw" | sed "s|$T/rec/||" >"$T/port.out"
[ $rc -eq $rc2 ] && cmp -s "$T/uart.out" "$T/port.out" \
    || fail "fuzz: through a link port the verdicts differ (exit $rc / $rc2): $(cat "$T/uart.out" "$T/port.out" "$T/port.err")"
hub
rc=0
"$RUN" -b rv32-ref --link "u0=unix:$T/hub.sock" --console link:u0 --fuzz-begin fuzz_iteration \
    --fuzz-input u0 --fuzz-replay "$T/rec" "$FUZZ" </dev/null >"$T/out" 2>&1 || rc=$?
[ $rc -eq 2 ] && grep -q 'cannot be connected to a link hub' "$T/out" \
    || fail "fuzz: a run connected to a hub was not refused (exit $rc): $(cat "$T/out")"
kill $HUBPID 2>/dev/null || true

echo "link-check: PASS"
