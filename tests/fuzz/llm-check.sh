#!/bin/sh
# llm-check.sh RUN BOARD FIRMWARE - the brain's LLM-assist route (docs/fuzzing.md,
# "LLM-assisted fuzzing"): a helper writes inputs into a peer queue and tokens into a
# watched dictionary, and reads back what each input did.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h. The brain runs with --sync-dir and a
# peer directory "llm"; a "helper" (this script) drops two inputs into llm/queue:
#   - a crash input (the 'M' magic-value bug, "Mxxx" + the magic little-endian): the
#     brain must import it, save it as a crash whose .info names src_peer=llm, and write
#     an import-log line with verdict "crash" and the peer id;
#   - a ,keep input that runs without new coverage: it must still enter the queue (verdict
#     "kept" in the import log), because ,keep says so.
# The helper also appends a token to a --watch-dict file; the brain must merge it live
# (a log line "watch-dict: merged").
#
# Then, if rizin and rzpipe are present, fuzz/tools/fuzz_frontier.py must replay a small
# corpus and report at least one frontier branch inside the parser with its compare byte.
set -eu
run=$1
board=$2
fw=$3
here=$(dirname "$0")
tools=$here/../../fuzz/tools
tmp=$(mktemp -d)
brain_pid=
ember_pid=
trap '[ -z "$ember_pid" ] || kill "$ember_pid" 2> /dev/null || true
      [ -z "$brain_pid" ] || kill "$brain_pid" 2> /dev/null || true
      rm -rf "$tmp"' EXIT
fail() { echo "llm-check: $board: FAIL: $*" >&2; exit 1; }

sym() { printf '0x%x' "0x$(readelf -sW "$fw" | awk -v n="$1" '$NF == n { print $2; exit }')"; }
begin=$(sym fuzz_iteration)
panic=$(sym fuzz_panic)
mkdir -p "$tmp/sync/llm/queue" "$tmp/seeds"
printf 'hello' > "$tmp/seeds/a"
printf '"XYZ"\n' > "$tmp/live.dict"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

python3 "$tools/fuzz_brain.py" --listen "127.0.0.1:$port" --seeds "$tmp/seeds" \
    --name main --sync-dir "$tmp/sync" --sync-interval 0.5 --no-cmplog --seed-rng 7 \
    --import-log "$tmp/imports.jsonl" --watch-dict "$tmp/live.dict" --watch-dict-interval 0.5 \
    --status-interval 0.5 > "$tmp/brain.log" 2>&1 &
brain_pid=$!
"$run" -b "$board" --fuzz-begin "$begin" --fuzz-crash-pc "$panic" --fuzz-input uart0 \
    --fuzz-iter-insns 200000 --fuzz-net "127.0.0.1:$port" "$fw" \
    < /dev/null > "$tmp/ember.log" 2>&1 &
ember_pid=$!

# Wait until the brain has booted the firmware and begun fuzzing.
for _ in $(seq 1 200); do
    grep -q 'baseline:' "$tmp/brain.log" 2> /dev/null && break
    sleep 0.1
done
grep -q 'baseline:' "$tmp/brain.log" || { cat "$tmp/brain.log" >&2; fail "the brain never reached baseline"; }

# The helper writes its inputs atomically (temp name, then rename: the brain ignores a
# name without an id: prefix, so it never reads a half-written file), and a new token.
python3 -c 'import struct,sys; sys.stdout.buffer.write(b"Mxxx"+struct.pack("<I",0x6A3C91E5))' \
    > "$tmp/sync/llm/queue/.t1" && mv "$tmp/sync/llm/queue/.t1" "$tmp/sync/llm/queue/id:000000,orig:magic"
printf 'ZZZZ' > "$tmp/sync/llm/queue/.t2" && mv "$tmp/sync/llm/queue/.t2" "$tmp/sync/llm/queue/id:000001,orig:plain,keep"
printf '"XYZ"\nhex:deadbeef\n' > "$tmp/live.dict.t" && mv "$tmp/live.dict.t" "$tmp/live.dict"

# Wait for both imports to be logged.
for _ in $(seq 1 300); do
    cnt=$(grep -c '"verdict"' "$tmp/imports.jsonl" 2> /dev/null || true)
    [ "${cnt:-0}" -ge 2 ] && break
    sleep 0.1
done

kill "$brain_pid" 2> /dev/null || true
brc=0
wait "$brain_pid" 2> /dev/null || brc=$?
brain_pid=
wait "$ember_pid" 2> /dev/null || true
ember_pid=

# The import log: one crash from llm, one kept.
python3 - "$tmp/imports.jsonl" <<'PY' || fail "import log is wrong (see above)"
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
byorig = {r.get("orig"): r for r in rows}
ok = True
c = byorig.get("magic")
if not c or c.get("verdict") != "crash" or c.get("peer") != "llm" or c.get("reason_name") != "FAULT":
    print("llm-check: the magic input was not logged as an llm crash: %r" % c); ok = False
k = byorig.get("plain")
if not k or k.get("verdict") != "kept":
    print("llm-check: the ,keep input was not logged as kept: %r" % k); ok = False
sys.exit(0 if ok else 1)
PY

# The saved crash names its origin.
info=$(grep -l 'src_peer=llm' "$tmp/sync/main/crashes/"*.info 2> /dev/null | head -n 1 || true)
[ -n "$info" ] || fail "no saved crash names src_peer=llm"
grep -q 'src_orig=magic' "$info" || fail "the crash .info does not carry src_orig"

# The watched dictionary token was merged live.
grep -q 'watch-dict: merged' "$tmp/brain.log" || { cat "$tmp/brain.log" >&2; fail "the live dictionary token was not merged"; }

echo "llm-check: $board: brain assist OK (import log, keep, provenance, live dictionary)"

# The frontier tool (rizin + rzpipe, else skipped).
if command -v rizin > /dev/null 2>&1 && python3 -c 'import rzpipe' 2> /dev/null; then
    mkdir "$tmp/corp"
    printf 'ok' > "$tmp/corp/a"
    printf 'hello' > "$tmp/corp/b"
    out=$(python3 "$tools/fuzz_frontier.py" "$fw" --corpus "$tmp/corp" --range parse -- \
        "$run" -b "$board" --fuzz-begin "$begin" --fuzz-crash-pc "$panic" --fuzz-input uart0 \
        2> "$tmp/frontier.err") || { cat "$tmp/frontier.err" >&2; fail "fuzz_frontier.py exited nonzero"; }
    echo "$out" | grep -q 'frontier branch target' || { echo "$out" >&2; fail "no frontier branches reported"; }
    # The parser's letter checks must show as frontier targets with their compare bytes.
    echo "$out" | grep -Eq '0x46|0x4c|0x55' || { echo "$out" >&2; fail "no compare byte in the frontier report"; }
    echo "llm-check: $board: frontier report OK"
else
    echo "llm-check: $board: frontier report skipped (needs rizin and rzpipe)"
fi