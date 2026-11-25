#!/bin/sh
# llm-check.sh RUN BOARD FIRMWARE - the brain's LLM-assist route (docs/fuzzing.md,
# "LLM-assisted fuzzing"): a helper writes inputs into a peer queue and tokens into a
# watched dictionary, and reads back what each input did.
#
# FIRMWARE is a build of tests/fuzz/fuzztarget.h. The brain runs with --sync-dir and a
# peer directory "llm"; a "helper" (this script) drops inputs into llm/queue. The ones
# whose verdict depends on coverage are there before the brain starts, so its first import
# (right after the baseline, before any mutation) judges them against the seed alone --
# the mutator finds this parser's shallow paths within a second. The crash input comes
# once the campaign runs (the mutator cannot guess a 32-bit magic value without compare
# logging, which is off), so a live import is exercised too:
#   - a crash input (the 'M' magic-value bug, "Mxxx" + the magic little-endian): the
#     brain must import it, save it as a crash whose .info names src_peer=llm, and write
#     an import-log line with verdict "crash" and the peer id (written live, id 5);
#   - "ZZZZ": on most targets no new bucket, but new hit-count classes (the parser's loop
#     runs 4 times, not 5): new coverage all the same, verdict "newcov";
#   - a ,keep input that runs like the seed ("hellk"): it must still enter the queue
#     (verdict "kept" in the import log), because ,keep says so;
#   - an input that reaches new code ("FUZx", judged before any mutation
#     against the seed alone): verdict "newcov", and its queue entry carries
#     its orig: label;
#   - an input that runs like the seed ("hellx"): verdict "nocov", not queued;
#   - one named ,keeper: not the ,keep tag, so also "nocov".
# Every import-log line carries t and time; .synced/llm records the last id.
# The helper also appends a token to a --watch-dict file; the brain must merge it live
# (a log line "watch-dict: merged").
#
# Then, if rizin and rzpipe are present, fuzz/tools/fuzz_frontier.py must replay a small
# corpus and report at least one frontier branch inside the parser with its compare byte.
# It is skipped where rizin cannot read the machine's branches: no disassembler for it
# (Homebrew's has no RISC-V), or an analysis that leaves them untyped (rizin 0.9's PowerPC).
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

sym() { printf '0x%x' "0x$("${READELF:-readelf}" -sW "$fw" | awk -v n="$1" '$NF == n { print $2; exit }')"; }
begin=$(sym fuzz_iteration)
panic=$(sym fuzz_panic)
mkdir -p "$tmp/sync/llm/queue" "$tmp/seeds"
printf 'hello' > "$tmp/seeds/a"
printf '"XYZ"\n' > "$tmp/live.dict"
q=$tmp/sync/llm/queue
printf 'ZZZZ' > "$q/id:000000,orig:classes"
printf 'FUZx' > "$q/id:000001,orig:fuzx"
printf 'hellk' > "$q/id:000002,orig:plain,keep"
printf 'hellx' > "$q/id:000003,orig:same"
printf 'hellz' > "$q/id:000004,orig:notkeep,keeper"
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
    > "$q/.t1" && mv "$q/.t1" "$q/id:000005,orig:magic"
printf '"XYZ"\nhex:deadbeef\n' > "$tmp/live.dict.t" && mv "$tmp/live.dict.t" "$tmp/live.dict"

# Wait for all six imports to be logged.
for _ in $(seq 1 300); do
    cnt=$(grep -c '"verdict"' "$tmp/imports.jsonl" 2> /dev/null || true)
    [ "${cnt:-0}" -ge 6 ] && break
    sleep 0.1
done

kill "$brain_pid" 2> /dev/null || true
brc=0
wait "$brain_pid" 2> /dev/null || brc=$?
brain_pid=
wait "$ember_pid" 2> /dev/null || true
ember_pid=

# The import log: one crash from llm, one kept, two new coverage, two dropped.
python3 - "$tmp/imports.jsonl" "$tmp/sync/main/queue" <<'PY' || fail "import log is wrong (see above)"
import json, os, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
byorig = {r.get("orig"): r for r in rows}
ok = True
if len(rows) != 6:
    print("llm-check: %d import-log lines, not 6: %r" % (len(rows), rows)); ok = False
z = byorig.get("classes")
if not z or z.get("verdict") != "newcov" or not z.get("new_classes"):
    print("llm-check: 'ZZZZ' (new hit-count classes) was not logged as newcov: %r" % z); ok = False
for r in rows:
    if not isinstance(r.get("t"), (int, float)) or not isinstance(r.get("time"), (int, float)):
        print("llm-check: an import-log line lacks t or time: %r" % r); ok = False
n = byorig.get("fuzx")
if not n or n.get("verdict") != "newcov" or "orig:fuzx" not in n.get("entry", ""):
    print("llm-check: the 'FUZx' input was not logged as newcov with its label: %r" % n); ok = False
elif not os.path.exists(os.path.join(sys.argv[2], n["entry"])):
    print("llm-check: the newcov entry %s is not in the queue" % n["entry"]); ok = False
for o in ("same", "notkeep"):
    r = byorig.get(o)
    if not r or r.get("verdict") != "nocov":
        print("llm-check: the %r input was not logged as nocov: %r" % (o, r)); ok = False
c = byorig.get("magic")
if not c or c.get("verdict") != "crash" or c.get("peer") != "llm" or c.get("reason_name") != "FAULT":
    print("llm-check: the magic input was not logged as an llm crash: %r" % c); ok = False
k = byorig.get("plain")
if not k or k.get("verdict") != "kept":
    print("llm-check: the ,keep input was not logged as kept: %r" % k); ok = False
sys.exit(0 if ok else 1)
PY

# The brain recorded the last id it took from the helper.
[ "$(cat "$tmp/sync/main/.synced/llm" 2> /dev/null)" = 5 ] || fail ".synced/llm is not 5"

# The saved crash names its origin.
info=$(grep -l 'src_peer=llm' "$tmp/sync/main/crashes/"*.info 2> /dev/null | head -n 1 || true)
[ -n "$info" ] || fail "no saved crash names src_peer=llm"
grep -q 'src_orig=magic' "$info" || fail "the crash .info does not carry src_orig"

# The watched dictionary token was merged live.
grep -q 'watch-dict: merged' "$tmp/brain.log" || { cat "$tmp/brain.log" >&2; fail "the live dictionary token was not merged"; }

echo "llm-check: $board: brain assist OK (import log, keep, provenance, live dictionary)"

# The frontier tool. Its CFG is backend-neutral (fuzz/tools/frontier_cfg.py): rizin driven
# directly (no rzpipe module), headless Ghidra, a Ghidra project, or a ready CFG file. This
# checks the rizin backend where rizin can read the machine, and -- when headless Ghidra with
# the Jython-free ExportCfg.java is around -- the Ghidra backend too, so the frontier does
# not depend on one disassembler. The ELF loads as an ELF; the raw-image route is exercised
# by fuzz-check.sh's images and does not need repeating per board here.
mkdir "$tmp/corp"
printf 'ok' > "$tmp/corp/a"
printf 'hello' > "$tmp/corp/b"

# run_frontier BACKEND CFG_FROM: run the report with that CFG source; write it to
# $tmp/frontier.out and its stderr to $tmp/frontier.err. Return 0 on a good report, 2 on a
# skip (a machine this backend cannot decode -- reason in $skip), or call fail (which exits
# the whole script) on any other error. Not run in a subshell, so fail propagates.
skip=
run_frontier() {
    cf=$2
    if python3 "$tools/fuzz_frontier.py" "$fw" --corpus "$tmp/corp" --range parse \
        --cfg-from "$cf" -- \
        "$run" -b "$board" --fuzz-begin "$begin" --fuzz-crash-pc "$panic" --fuzz-input uart0 \
        > "$tmp/frontier.out" 2> "$tmp/frontier.err"; then
        :
    else
        # A backend that cannot read this machine (rizin has no disassembler for it, or an
        # ARM BE8 ELF it decodes the wrong way) prints "could not read the firmware's CFG"
        # and exits: a skip, not a test failure. Any other nonzero exit is a real failure.
        if grep -q "could not read the firmware's CFG" "$tmp/frontier.err"; then
            skip="$1 cannot read this machine ($(sed -n 's/^  '"$cf"': //p' "$tmp/frontier.err" | head -1))"
            return 2
        fi
        cat "$tmp/frontier.err" >&2; fail "fuzz_frontier.py (--cfg-from $cf) exited nonzero"
    fi
    if grep -q 'branch(es) in range untyped' "$tmp/frontier.err"; then
        skip="$1 leaves the parser's branches untyped"; return 2
    fi
    grep -q 'frontier branch target' "$tmp/frontier.out" || { cat "$tmp/frontier.out" >&2; fail "no frontier branches reported ($1)"; }
    # The parser's letter checks must show as frontier targets with their compare bytes.
    grep -Eq '0x46|0x4c|0x55' "$tmp/frontier.out" || { cat "$tmp/frontier.out" >&2; fail "no compare byte in the frontier report ($1)"; }
    return 0
}

# rizin, if installed and it can read this machine.
if command -v rizin > /dev/null 2>&1; then
    if run_frontier rizin rizin; then
        echo "llm-check: $board: frontier report OK (rizin)"
    else
        echo "llm-check: $board: frontier report skipped, rizin ($skip)"
    fi
else
    echo "llm-check: $board: frontier report skipped, rizin (not installed)"
fi

# headless Ghidra (ExportCfg.java is plain Java, no Jython), if one is found.
gh=${GHIDRA_HEADLESS:-}
if [ -z "${GHIDRA_HEADLESS+set}" ]; then
    for c in "${GHIDRA_HOME:+$GHIDRA_HOME/support/analyzeHeadless}" \
             "${GHIDRA_INSTALL_DIR:+$GHIDRA_INSTALL_DIR/support/analyzeHeadless}" \
             "$(command -v analyzeHeadless 2> /dev/null || true)" \
             $(ls -d "$HOME"/ghidra*/support/analyzeHeadless /opt/ghidra*/support/analyzeHeadless 2> /dev/null || true); do
        [ -n "$c" ] && [ -x "$c" ] && { gh=$c; break; }
    done
fi
if [ -n "$gh" ]; then
    GHIDRA_HEADLESS="$gh"; export GHIDRA_HEADLESS
    if run_frontier "headless Ghidra" ghidra; then
        echo "llm-check: $board: frontier report OK (headless Ghidra)"
    else
        echo "llm-check: $board: frontier report skipped, Ghidra ($skip)"
    fi
else
    echo "llm-check: $board: frontier report skipped, Ghidra (no headless Ghidra found)"
fi
