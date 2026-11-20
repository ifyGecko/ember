#!/bin/sh
# rules-check.sh RUNNER RV32_SELFTEST_ELF RV32_FUZZTARGET_ELF - checks of uam-run's --on
# rules (uam/rules.h) on rv32-ref:
#   quiet        a rule that only stores prints nothing; no rule, no rule lines
#   set          a store into the guest's memory changes what it prints
#   log          registers, memory and a symbol's pc, count=N times, the same lines under
#                the interpreter, --jit and --jit --no-chain; --trace-file takes them
#   if, stop     the run stops at the arrival the condition picks (exit 127) and says
#                which rule stopped it
#   --stats      each rule's arrivals, firings and failures
#   failures     an operand that cannot be read does not stop the run; the stop says how
#                many failed and -v names the first
#   --on-file    rules from a file; a bad line is refused with the file and line (exit 2)
#   bad rules    refused with a reason (exit 2)
#   snapshots    a count comes back with a snapshot: the rule fires only as often as it
#                had left; a snapshot taken with a counted rule does not load without it
#   fuzzing      a counted rule fires in every input's run alike
set -eu
RUN=$1
FW=$2
FUZZ=$3
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "rules-check: FAIL: $*" >&2; exit 1; }

# ---- quiet, and a store the guest shows --------------------------------------------------
$RUN "$FW" </dev/null >"$T/out" 2>"$T/err" || fail "the self-test failed"
head -1 "$T/out" | grep -q '^UAM self-test' || fail "no banner"
! grep -q '^rule \|^uam-run: rule ' "$T/err" || fail "rule lines without a rule"
$RUN --on 'pc=puts_,count=1,set [a0]:1=0x58' "$FW" </dev/null >"$T/out" 2>"$T/err" ||
    fail "the self-test failed with a set rule"
head -1 "$T/out" | grep -q '^XAM self-test' || fail "set: the banner's first byte was not replaced"
! grep -q '^rule \|^uam-run: rule ' "$T/err" || fail "a rule that only stores printed something"

# ---- log: the same on every backend --------------------------------------------------------
for mode in interp jit nochain; do
    case $mode in interp) a="";; jit) a="--jit";; nochain) a="--jit --no-chain";; esac
    # shellcheck disable=SC2086
    $RUN $a --on 'pc=puts_,count=3,name=say,log a0 [a0]:1 [sp]' "$FW" </dev/null >/dev/null 2>"$T/$mode" ||
        fail "$mode: the self-test failed with a log rule"
    grep '^rule ' "$T/$mode" >"$T/$mode.r" || fail "$mode: no rule lines"
done
[ "$(wc -l <"$T/interp.r")" -eq 3 ] || fail "log: count=3 gave $(wc -l <"$T/interp.r") lines"
head -1 "$T/interp.r" |
    grep -Eq '^rule say: cpu 0 pc 0x80000[0-9a-f]+ a0=0x8000[0-9a-f]+ \[a0\]:1=0x55 \[sp\]=0x[0-9a-f]+ icount=[0-9]+$' ||
    fail "log: the first line is '$(head -1 "$T/interp.r")'"
cmp -s "$T/interp.r" "$T/jit.r" || fail "log lines differ: interpreter vs --jit"
cmp -s "$T/interp.r" "$T/nochain.r" || fail "log lines differ: interpreter vs --jit --no-chain"
$RUN --on 'pc=puts_,count=3,name=say,log a0 [a0]:1 [sp]' --trace-file "$T/tf" "$FW" </dev/null \
    >/dev/null 2>"$T/err" || fail "the self-test failed with --trace-file"
grep '^rule ' "$T/tf" | cmp -s - "$T/interp.r" || fail "--trace-file did not get the rule lines"
! grep -q '^rule ' "$T/err" || fail "rule lines on stderr despite --trace-file"

# ---- if, stop, --stats ------------------------------------------------------------------------
# run_test(name, fn): stop at the test whose name starts with 'b' (branches), the second one
rc=0
$RUN --on 'pc=run_test,name=second,if [a0]:1==0x62,log a0,stop' --on 'pc=puts_,set t6=t6' -s "$FW" \
    </dev/null >"$T/out" 2>"$T/err" || rc=$?
[ "$rc" -eq 127 ] || fail "stop: exit $rc, expected 127"
at=$(sed -n 's/^rule second: cpu 0 pc \(0x[0-9a-f]*\) a0=0x[0-9a-f]* icount=\([0-9]*\)$/\1 \2/p' "$T/err")
[ -n "$at" ] || fail "stop: no log line before the stop"
grep -q "^uam-run: rule second stopped the run at ${at% *} (icount ${at#* })\$" "$T/err" ||
    fail "stop: the stop is not attributed to the rule"
grep -q "^uam-run: stopped by a pc hook .* after ${at#* } instructions\$" "$T/err" ||
    fail "stop: not stopped at the rule's arrival"
grep -q 'ok \] alu' "$T/out" || fail "stop: the first test did not run"
! grep -q 'branches' "$T/out" || fail "stop: the second test ran"
grep -q '^stats: rule second: arrivals=2 fired=1 failed=0$' "$T/err" || fail "--stats: the stop rule's counts"
grep -Eq '^stats: rule pc=puts_: arrivals=[0-9]+ fired=[0-9]+ failed=0$' "$T/err" ||
    fail "--stats: the other rule's counts"

# ---- what cannot be carried out ---------------------------------------------------------------
$RUN --on 'pc=puts_,count=2,name=nowhere,set [p:0x10]:4=1' -v "$FW" </dev/null >"$T/out" 2>"$T/err" ||
    fail "a failing rule stopped the self-test"
grep -q 'SELFTEST PASS' "$T/out" || fail "a failing rule changed the run"
grep -q '^uam-run: rule nowhere: 2 of its conditions and actions could not be carried out' "$T/err" ||
    fail "failures are not reported at the stop"
grep -q 'rule nowhere: set failed: physical 0x10 cannot be written' "$T/err" ||
    fail "-v does not name the first failure"

# ---- --on-file -----------------------------------------------------------------------------------
cat >"$T/rules" <<'EOF'
# the banner, then the first two strings after it

pc=puts_, count=1, set [a0]:1 = 0x58
  pc = puts_ , count = 2 , name = say , log a0
EOF
$RUN --on-file "$T/rules" "$FW" </dev/null >"$T/out" 2>"$T/err" || fail "the self-test failed with --on-file"
head -1 "$T/out" | grep -q '^XAM self-test' || fail "--on-file: the set rule did not run"
[ "$(grep -c '^rule say: ' "$T/err")" -eq 2 ] || fail "--on-file: the log rule did not run twice"
printf 'pc=puts_,log\npc=puts_,frob\n' >"$T/bad"
rc=0; $RUN --on-file "$T/bad" "$FW" </dev/null >/dev/null 2>"$T/err" || rc=$?
[ "$rc" -eq 2 ] && grep -q "^uam-run: --on-file: $T/bad:2: unknown item 'frob'" "$T/err" ||
    fail "--on-file: a bad line is not refused with its place (exit $rc)"
rc=0; $RUN --on-file "$T/none" "$FW" </dev/null >/dev/null 2>"$T/err" || rc=$?
[ "$rc" -eq 2 ] && grep -q 'cannot read' "$T/err" || fail "--on-file: a missing file is not refused"

# ---- bad rules ---------------------------------------------------------------------------------
for bad in 'puts_,stop' 'pc=puts_' 'pc=nosuch,stop' 'pc=puts_,set 5=1' 'pc=puts_,log [a0]:3' \
           'pc=puts_,cpu=1,stop' 'pc=puts_,if a0 ==,stop' 'pc=a0,stop'; do
    rc=0; $RUN --on "$bad" "$FW" </dev/null >/dev/null 2>"$T/err" || rc=$?
    [ "$rc" -eq 2 ] && grep -qF "uam-run: bad --on '$bad': " "$T/err" || fail "--on '$bad': exit $rc, no reason"
done

# ---- a count comes back with a snapshot -----------------------------------------------------
# run_test is first reached after one puts_ (the banner): one of the rule's two firings is used
C='pc=puts_,count=2,name=say,log a0'
$RUN --on "$C" --snapshot-at run_test --save-snapshot "$T/s.snap" --break run_test "$FW" \
    </dev/null >/dev/null 2>"$T/err" || true
[ -s "$T/s.snap" ] || fail "no snapshot saved"
[ "$(grep -c '^rule say: ' "$T/err")" -eq 1 ] || fail "snapshot: the rule did not fire once before it"
$RUN --on "$C" --load-snapshot "$T/s.snap" "$FW" </dev/null >"$T/out" 2>"$T/err" ||
    fail "the self-test failed from the snapshot"
grep -q 'SELFTEST PASS' "$T/out" || fail "snapshot: the run from it did not pass"
[ "$(grep -c '^rule say: ' "$T/err")" -eq 1 ] ||
    fail "snapshot: the rule fired $(grep -c '^rule say: ' "$T/err") times after it, not the 1 it had left"
rc=0; $RUN --load-snapshot "$T/s.snap" "$FW" </dev/null >/dev/null 2>"$T/err" || rc=$?
[ "$rc" -eq 2 ] && grep -q 'cannot resume from snapshot' "$T/err" ||
    fail "snapshot: one taken with a counted rule loaded without it (exit $rc)"
rc=0; $RUN --on 'pc=puts_,count=5,log' --load-snapshot "$T/s.snap" "$FW" </dev/null >/dev/null 2>"$T/err" || rc=$?
[ "$rc" -eq 2 ] && grep -q 'rules with count=N keep their counts in snapshots' "$T/err" ||
    fail "snapshot: loaded with another counted rule, or no hint why not (exit $rc)"
# rules without a count are not in a snapshot: any of them goes with any snapshot
$RUN --snapshot-at run_test --save-snapshot "$T/p.snap" --break run_test "$FW" </dev/null >/dev/null 2>&1 || true
$RUN --on 'pc=puts_,if [a0]:1==0x5b,name=tag,log' --load-snapshot "$T/p.snap" "$FW" </dev/null \
    >"$T/out" 2>"$T/err" || fail "the self-test failed from a snapshot with a rule added"
grep -q '^rule tag: ' "$T/err" || fail "snapshot: a rule added to a plain snapshot did not run"

# ---- fuzzing: every input's run sees the counted rule alike -------------------------------
mkdir "$T/in"
printf 'hello' >"$T/in/1"
printf 'again' >"$T/in/2"
printf 'third' >"$T/in/3"
begin=$(readelf -sW "$FUZZ" | awk '$NF == "fuzz_iteration" { print "0x" $2; exit }')
$RUN --fuzz-begin "$begin" --fuzz-input uart0 --fuzz-replay "$T/in" \
    --on "pc=$begin+4,count=1,name=once,log" "$FUZZ" </dev/null >/dev/null 2>"$T/err" ||
    fail "replay with a rule failed"
[ "$(grep -c '^rule once: ' "$T/err")" -eq 3 ] ||
    fail "fuzzing: a count=1 rule fired $(grep -c '^rule once: ' "$T/err") times over 3 inputs, not once in each"

echo "rules-check: ok"
