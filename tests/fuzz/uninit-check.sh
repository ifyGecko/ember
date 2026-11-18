#!/bin/sh
# uninit-check.sh RUN BOARD FIRMWARE PHYS - --fuzz-uninit on the planted bug of
# tests/fuzz/fuzztarget.h: an input starting with 'U' branches on a byte of RAM (at the
# physical address PHYS: where the report and --fuzz-uninit-exclude name it) that nothing
# ever writes (the boot leaves it zero, a filled run finds the pattern).
#
#   off by default: without the option the replay is plain, no uninit line, exit 0;
#   with it, on the interpreter, the JIT and the JIT without chaining alike: the U input
#     differs (path and instruction count), the line names the read (a pc and PHYS) and the
#     exit status is 26; the other inputs go through as before, with the same lines as
#     without the option (the check puts back what it disturbs);
#   --fuzz-uninit=0 finds nothing (a fill of zero is what the boot left), another pattern
#     does; --fuzz-uninit-exclude over PHYS finds nothing; bad values are refused;
#   a shared boot (--fuzz-snapshot) carries the written pages: a worker started from the
#     snapshot reports the same line, and a snapshot made without the option is replaced
#     when it is given.
set -eu
run=$1
board=$2
fw=$3
addr=$(printf '0x%x' "$4")   # physical
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "uninit-check: $board: FAIL: $*" >&2; exit 1; }

mkdir "$tmp/in"
printf 'hello' > "$tmp/in/1-hello"
printf 'ok' > "$tmp/in/2-ok"
printf 'Uxx' > "$tmp/in/3-U"

# replay FLAGS...: the replay lines (input names relative) go to $tmp/lines, the exit status
# is in $rc (not a command substitution: that would lose it).
replay() {
    rc=0
    "$run" -b "$board" "$@" --fuzz-begin fuzz_iteration --fuzz-crash-pc fuzz_panic \
        --fuzz-input uart0 --fuzz-iter-insns 200000 -n 10000000 --fuzz-replay "$tmp/in" "$fw" \
        < /dev/null > "$tmp/out" 2> "$tmp/err" || rc=$?
    grep '^replay: ' "$tmp/out" | sed "s|$tmp/in/||" > "$tmp/lines" || true
}

replay
plain=$(cat "$tmp/lines")
[ $rc -eq 0 ] || fail "the plain replay exits $rc"
[ "$(echo "$plain" | grep -c ': ok ')" -eq 3 ] || fail "the plain replay is not three ok lines: $plain"
! echo "$plain" | grep -q uninit || fail "an uninit line without --fuzz-uninit"
! grep -q 'uninit' "$tmp/err" || fail "uninit on stderr without --fuzz-uninit"

first=
for mode in "" "--jit" "--jit --no-chain"; do
    # shellcheck disable=SC2086
    replay $mode --fuzz-uninit
    got=$(cat "$tmp/lines")
    [ $rc -eq 26 ] || fail "[$mode] --fuzz-uninit: exit $rc, expected 26: $got"
    line=$(echo "$got" | grep ': uninit differs=' || true)
    [ "$(echo "$line" | grep -c .)" -eq 1 ] || fail "[$mode] not exactly one uninit line: $got"
    echo "$line" | grep -q '^replay: 3-U: uninit differs=' || fail "[$mode] the uninit line is not the U input's: $line"
    echo "$line" | grep -q "at $addr" || fail "[$mode] the line does not name $addr: $line"
    echo "$line" | grep -q 'pc=0x' || fail "[$mode] the line names no pc: $line"
    # the lines of the runs themselves are the plain replay's
    [ "$(echo "$got" | grep -v 'uninit differs=')" = "$plain" ] \
        || fail "[$mode] the replay lines changed with --fuzz-uninit:
$got
--- plain:
$plain"
    grep -q 'uninit: [0-9]* runs repeated with memory filled, 1 differed' "$tmp/err" \
        || fail "[$mode] no summary of the check on stderr: $(grep uninit "$tmp/err")"
    [ -n "$first" ] || first=$line
    [ "$line" = "$first" ] || fail "[$mode] another backend, another line:
$line
$first"
done

replay --fuzz-uninit=0
[ $rc -eq 0 ] || fail "--fuzz-uninit=0 found something (exit $rc): a zero fill is what the boot left"
replay --fuzz-uninit=0xdeadbeef
grep -q ': uninit differs=' "$tmp/lines" || fail "--fuzz-uninit=0xdeadbeef found nothing"
[ $rc -eq 26 ] || fail "--fuzz-uninit=0xdeadbeef: exit $rc"
replay --fuzz-uninit --fuzz-uninit-exclude "$addr+0x1000"
[ $rc -eq 0 ] || fail "--fuzz-uninit-exclude over $addr still found it (exit $rc)"
replay --fuzz-uninit --fuzz-uninit-exclude "$addr-$(printf '0x%x' $(($4 + 0x1000)))"
[ $rc -eq 0 ] || fail "--fuzz-uninit-exclude LO-HI over $addr still found it (exit $rc)"

for bad in "--fuzz-uninit=zz" "--fuzz-uninit=0x100000000" "--fuzz-uninit-exclude $addr+0x10" "--fuzz-uninit=1 --fuzz-uninit-exclude $addr" \
           "--fuzz-uninit=1 --fuzz-uninit-exclude 0x10+0"; do
    # shellcheck disable=SC2086
    replay $bad
    [ $rc -eq 2 ] || fail "'$bad' is not refused (exit $rc)"
done

# A shared boot: the worker started from the snapshot has the same written pages.
replay --fuzz-uninit --fuzz-snapshot "$tmp/b.snap"
[ -f "$tmp/b.snap" ] || fail "no snapshot saved"
replay --fuzz-uninit --fuzz-snapshot "$tmp/b.snap"
second=$(cat "$tmp/lines")
[ $rc -eq 26 ] || fail "from the snapshot: exit $rc"
grep -q 'booting and replacing' "$tmp/err" && fail "the snapshot was replaced, not used"
[ "$(echo "$second" | grep ': uninit differs=')" = "$first" ] || fail "from the snapshot: another line:
$second"
# ... and one made without the option is not used with it (the command line differs).
rm -f "$tmp/c.snap" "$tmp/c.snap.key"
replay --fuzz-snapshot "$tmp/c.snap"
[ -f "$tmp/c.snap" ] || fail "no plain snapshot saved"
replay --fuzz-uninit --fuzz-snapshot "$tmp/c.snap"
grep -q ': uninit differs=' "$tmp/lines" || fail "with a snapshot made without the option: nothing found"
[ $rc -eq 26 ] || fail "with a snapshot made without the option: exit $rc"
grep -q 'booting and replacing' "$tmp/err" || fail "the plain snapshot was not replaced"

echo "uninit-check: $board: ok"
