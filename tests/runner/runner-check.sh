#!/bin/sh
# runner-check.sh RUNNER X86_SELFTEST_BIN - checks of uam-run's debugging options on
# x86_64-test (RUNNER is build/uam-test-run) with the raw x86 self-test (loaded at 0,
# entered at 0):
#   bad board options  a setup error (exit 2), not a crash, however early the board refuses
#   --log-pc     more than 64 addresses all take effect
#   --dump-mem   at --break: the bytes are the image's (raw file and hex dump, virtual and
#                p: physical); an unreadable range is reported with the reason (unmapped,
#                a device without a peek handler: no empty file); device memory with a
#                peek handler (VGA) is readable
#   --trace-after  +N counts from a loaded snapshot; an absolute N before the snapshot is
#                warned about; interpreter and --jit block traces are the same after it
#   --watch      logs writes (address, size, value) and does not change the run
#   --guard      stops before the first write into a range given around an address
#                (exit 129), at the same instruction with --jit; a read guard lets the
#                writes pass and stops at the read-back; a bad SPEC is refused
set -eu
RUN=$1
FW=$2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
X="$RUN -b x86_64-test --raw-base 0 --entry 0"
fail() { echo "runner-check: FAIL: $*" >&2; exit 1; }

# a board that refuses its options is a setup error (exit 2), whether it fails before
# uam_machine_init (most of these) or after it (pic32-ref)
for bad in "avr-ref -o model=none" "avr-ref -o cpus=2" "pic-ref -o cpus=2" "mcs51-ref -o cpus=2" \
           "6502-ref -o cpus=2" "z80-ref -o cpus=2" "xtensa-ref -o cpus=99" "xtensaeb-ref -o cpus=99" \
           "z80-ref --ram 0x20000" "mcs51-ref --ram 0x20000" "rv32-ref -o cpus=99" "pic32-ref -o model=none"; do
    rc=0
    # shellcheck disable=SC2086
    $RUN -b $bad </dev/null >/dev/null 2>"$T/bad" || rc=$?
    [ "$rc" -eq 2 ] || fail "-b $bad: exit $rc, expected 2"
    grep -q "building board" "$T/bad" || fail "-b $bad: no build failure reported"
done

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
grep -q 'not readable (nothing is mapped at physical 0xfffffffffff0); 0 of 16 bytes' "$T/log" ||
    fail "--dump-mem: unmapped range not reported"
# a device region without a peek handler (HPET registers): named, and no empty file
$X --break 0 --dump-mem p:0xfed00000+16="$T/h.bin" "$FW" </dev/null 2>"$T/log" >/dev/null || true
grep -q "device region 'hpet', whose device (ia-pc,hpet) has no side-effect-free read" "$T/log" ||
    fail "--dump-mem: device without peek not explained"
grep -q "0 of 16 bytes, nothing written" "$T/log" || fail "--dump-mem: empty file not refused"
[ ! -e "$T/h.bin" ] || fail "--dump-mem: an empty file was written"
# device memory served through a peek handler (VGA text buffer) is readable
$X -o vga=1 --break 0 --dump-mem p:0xb8000+32="$T/vga.bin" "$FW" </dev/null >/dev/null 2>&1 || true
[ "$(wc -c <"$T/vga.bin" 2>/dev/null || echo 0)" -eq 32 ] || fail "--dump-mem: VGA memory not read through peek"
if $X --dump-mem 0x1000 "$FW" </dev/null >/dev/null 2>&1; then fail "--dump-mem without +LEN accepted"; fi

# --watch / --guard: the self-test's first writes into its scratch area at 0x3000
$X --watch p:0x3000+4:w -n 20000 "$FW" </dev/null 2>"$T/log" >/dev/null || true
grep -q '^guard p:0x3000+4:w (log): cpu 0 pc 0x[0-9a-f]* write 2 at 0x3000 (+0) value 0xa55a icount=[0-9]*$' "$T/log" ||
    fail "--watch: the first write is not logged"
[ "$(grep -c '^guard p:0x3000+4:w' "$T/log")" -ge 2 ] || fail "--watch: the second write is not logged"
grc=0
$X --guard p:0x3002-2:w,name=lo -n 20000 "$FW" </dev/null 2>"$T/log" >/dev/null || grc=$?
[ $grc -eq 129 ] || fail "--guard: exit $grc, expected 129"
gline=$(grep '^uam-run: guard lo (stop): cpu 0 pc 0x[0-9a-f]* write 2 at 0x3000 (+0) value 0xa55a$' "$T/log") ||
    fail "--guard: no hit line"
gstop=$(grep 'guarded memory access' "$T/log")
$X --guard p:0x3002-2:w,name=lo -n 20000 --jit "$FW" </dev/null 2>"$T/log" >/dev/null || true
[ "$(grep '^uam-run: guard lo' "$T/log")" = "$gline" ] && [ "$(grep 'guarded memory access' "$T/log")" = "$gstop" ] ||
    fail "--guard: --jit stops elsewhere"
# a read guard lets those writes pass and stops later, when the self-test reads them back
$X --guard p:0x3000+4:r -n 20000 "$FW" </dev/null 2>"$T/log" >/dev/null || true
grep -q '^uam-run: guard p:0x3000+4:r (stop): .* read [0-9]* at 0x300' "$T/log" || fail "--guard :r: no read hit"
wn=$(echo "$gstop" | sed 's/.* after \([0-9]*\) instructions/\1/')
rn=$(sed -n 's/.*guarded memory access.* after \([0-9]*\) instructions/\1/p' "$T/log")
[ -n "$rn" ] && [ "$rn" -gt "$wn" ] || fail "--guard :r: stopped at instruction ${rn:-?}, not after the writes ($wn)"
if $X --guard 0x3000:x "$FW" </dev/null >/dev/null 2>&1; then fail "--guard with a bad direction accepted"; fi

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

echo "runner-check: ok ($hits --log-pc addresses, --dump-mem, --watch, --guard, --trace-after +N from icount $snap, \
$(wc -l <"$T/jit") blocks traced alike with --jit)"
