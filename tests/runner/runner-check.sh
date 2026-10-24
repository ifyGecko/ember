#!/bin/sh
# runner-check.sh RUNNER X86_SELFTEST_BIN - checks of uam-run's debugging options on
# x86_64-ref with the raw x86 self-test (loaded at 0, entered at 0):
#   --log-pc     more than 64 addresses all take effect
#   --dump-mem   at --break: the bytes are the image's (raw file and hex dump, virtual and
#                p: physical); an unreadable range is reported
#   --trace-after  +N counts from a loaded snapshot; an absolute N before the snapshot is
#                warned about; interpreter and --jit block traces are the same after it
set -eu
RUN=$1
FW=$2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
X="$RUN -b x86_64-ref --raw-base 0 --entry 0"
fail() { echo "runner-check: FAIL: $*" >&2; exit 1; }

# --log-pc: the 100 block starts the trace reports first, all of them reached
$X -t blocks -n 20000 "$FW" </dev/null 2>"$T/tr" >/dev/null || true
grep '^blk' "$T/tr" | awk '{print $2}' | awk '!seen[$0]++' | head -100 >"$T/pcs"
[ "$(wc -l <"$T/pcs")" -eq 100 ] || fail "fewer than 100 distinct blocks traced"
args=$(sed 's/^/--log-pc /' "$T/pcs" | tr '\n' ' ')
# shellcheck disable=SC2086
$X -n 20000 $args "$FW" </dev/null 2>"$T/log" >/dev/null || true
hits=$(grep -c 'reached (hit 1,' "$T/log" || true)
[ "$hits" -eq 100 ] || fail "--log-pc: $hits of 100 addresses reported"

# --dump-mem at a breakpoint on the entry (nothing has run): the image's first bytes
$X --break 0 --dump-mem 0+64="$T/v.bin" --dump-mem p:0+64="$T/p.bin" --dump-mem 0+32 \
    "$FW" </dev/null 2>"$T/log" >/dev/null || true
head -c 64 "$FW" >"$T/want.bin"
cmp -s "$T/v.bin" "$T/want.bin" || fail "--dump-mem: virtual bytes differ from the image"
cmp -s "$T/p.bin" "$T/want.bin" || fail "--dump-mem: physical bytes differ from the image"
first=$(od -An -tx1 -N16 "$FW" | tr -s ' ' | sed 's/^ //; s/ $//')
grep -q "^  0000000000000000  $first " "$T/log" || fail "--dump-mem: hex dump line wrong"
$X --break 0 --dump-mem p:0xfffffffffff0+16 "$FW" </dev/null 2>"$T/log" >/dev/null || true
grep -q 'not readable; 0 of 16 bytes' "$T/log" || fail "--dump-mem: unreadable range not reported"
if $X --dump-mem 0x1000 "$FW" </dev/null >/dev/null 2>&1; then fail "--dump-mem without +LEN accepted"; fi

# --trace-after after --load-snapshot
pc=$(grep '^blk' "$T/tr" | sed -n 3000p | awk '{print $2}')
$X --snapshot-at "$pc" --save-snapshot "$T/s.snap" "$FW" </dev/null >/dev/null 2>&1 || true
[ -s "$T/s.snap" ] || fail "no snapshot saved at $pc"
snap=$($X --load-snapshot "$T/s.snap" -n 1 "$FW" </dev/null 2>&1 | sed -n 's/.*resumed from .*(icount \([0-9]*\)).*/\1/p')
[ -n "$snap" ] || fail "resume line has no icount"
first=$($X --load-snapshot "$T/s.snap" -t blocks --trace-after +200 "$FW" </dev/null 2>&1 |
        sed -n 's/^blk .* icount=\([0-9]*\)$/\1/p' | head -1)
[ -n "$first" ] && [ "$first" -ge $((snap + 200)) ] && [ "$first" -lt $((snap + 300)) ] ||
    fail "--trace-after +200 from icount $snap: first traced block at icount ${first:-none}"
$X --load-snapshot "$T/s.snap" -t blocks --trace-after 10 -n 100 "$FW" </dev/null 2>"$T/log" >/dev/null || true
grep -q "trace-after 10 is before the snapshot (icount $snap)" "$T/log" ||
    fail "--trace-after before the snapshot not warned about"
$X --load-snapshot "$T/s.snap" -t blocks "$FW" </dev/null 2>&1 | grep '^blk' >"$T/interp"
$X --load-snapshot "$T/s.snap" -t blocks --jit "$FW" </dev/null 2>&1 | grep '^blk' >"$T/jit"
[ -s "$T/interp" ] || fail "no blocks traced after the snapshot"
cmp -s "$T/interp" "$T/jit" || fail "block trace differs between the interpreter and --jit"

echo "runner-check: ok ($hits --log-pc addresses, --dump-mem, --trace-after +N from icount $snap, \
$(wc -l <"$T/jit") blocks traced alike with --jit)"
