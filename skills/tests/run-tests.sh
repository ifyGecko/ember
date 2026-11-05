#!/usr/bin/env bash
# run-tests.sh - end-to-end regression test of the skills' tooling (ember-dev-build, and the
# ember-fuzz checks in steps 3 and 4) against an ember checkout. Re-run it after moving the pin.
#
#   1. scaffold a project per ISA (MIPS BE, RV32, ARM LE and BE8, x86-64 PC, PowerPC classic
#      and Book E, AVR, AVR32A and B, PIC18, PIC32, 8051, 6502, Z80, Xtensa LE and BE); every
#      template must build warning-free and register its board; every other --model's board
#      must compile warning-free
#   2. run ember's own self-tests on the bare boards: each must stall on (or, Z80, write
#      to) the reference board's console UART, which the bare board does not have, and
#      log_summary.py must report that address; the x86-64 PC template must run the x86
#      self-test (real, protected and long mode, SMM) to PASS, interpreter and JIT
#   3. wire the example devices (references/examples) into the fixture board, build the
#      fixture firmware, and run it: stride-4 16550 behind reg_stride, soc_uart byte stores,
#      entry registers from the board hook, a guest-triggered chip reset (image re-placed),
#      NOR flash as ROMD (ID, program, erase; kseg0/kseg1/mirror views),
#      timer -> intc -> CPU interrupt with WAIT, the test finisher - interpreter and --jit;
#      with the interrupt unwired the run must end in a clean deadlock (exit 125); an HLE
#      PC hook (fixture/testbrd_hle.c) replaces a firmware function; an --mmio-rule answers
#      an unmodelled register; runs split by --save-snapshot / --load-snapshot (interpreter
#      and JIT on either side) equal the uninterrupted run, so every example device saves
#      and restores its state; ember-fuzz: the harness replays inputs through the board's
#      named input (uart0) from a snapshot at a begin pc, and replay_summary.py groups them;
#      a --guard read guard gives the crash-guard verdict (exit 25), grouped by guard
#   4. the PC platform: the x86 template with the PC examples (references/examples/pc) wired
#      in as a Q35-class chipset (fixture/pc/pcbrd_board.c) runs fixture/pc/fw from the reset
#      vector: Super I/O deciding COM1's decode, PCI IDs, ECAM through PCIEXBAR, PAM
#      shadowing, the ACPI PM timer, an APM SMI with TSEG visible only in SMM, the SPI
#      controller (descriptor, write gate, program/erase seen through the BIOS window, code
#      rewritten in place), SMBus SPD reads, a 0xCF9 reset - from CFI and from SPI flash,
#      interpreter and JIT; snapshot splits; x86 fuzz replay through the board's com1 input
#      (an oracle crash and a #UD); gdb_trace_calls.py on 32-bit code; and, when OVMF is
#      installed, OVMF boots to the UEFI shell on the same board
#   5. layering: a device may include uam/state.h and uam/blockdev.h; check-layering.sh
#      rejects one including uam/machine.h. Guardrails: rebuild.sh rejects uncaptured ember
#      edits; regen-patches.sh exports them as a format-patch file that build-ember.sh applies
#      as a commit on the pin and `git am` lands upstream; repin.sh rebases it onto a newer
#      upstream and drops it once upstream has it; build-ember.sh refuses a dirty working copy
#
# Usage: skills/tests/run-tests.sh [EMBER_GIT_SOURCE]   (default: $EMBER_UPSTREAM or ~/ember)
# Needs: git, cc, python3; mips-linux-gnu-gcc (step 3); gcc with -m32 support (step 4);
# ember's own `make firmware firmware-mips firmware-arm firmware-x86 firmware-ppc ...` output
# (one target per ISA, in EMBER_GIT_SOURCE/build) for step 2 (each run skipped when
# absent); gdb-multiarch and an OVMF install (OVMF_DIR, default
# /usr/share/OVMF: OVMF_CODE_4M.fd + OVMF_VARS_4M.fd) for the optional checks in step 4.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SK="$HERE/../ember-dev-build"
UP="${1:-${EMBER_UPSTREAM:-$HOME/ember}}"
UP="$(cd "$UP" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { echo "PASS  $*"; pass=$((pass+1)); }
bad() { echo "FAIL  $*"; fail=$((fail+1)); }
check() { local what="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$what"; else bad "$what"; fi; }

# 1. scaffolds
"$SK/scripts/new-emulation.sh" testbrd --dir "$T/mips" --isa mips32be --ember-upstream "$UP" \
    --image-base 0x80000000 --entry 0x80000000 >/dev/null
"$SK/scripts/new-emulation.sh" rv-test --dir "$T/rv" --isa rv32 --ember-upstream "$UP" >/dev/null
"$SK/scripts/new-emulation.sh" arm-test --dir "$T/arm" --isa arm --ember-upstream "$UP" >/dev/null
"$SK/scripts/new-emulation.sh" armeb-test --dir "$T/armeb" --isa armeb --ember-upstream "$UP" >/dev/null
"$SK/scripts/new-emulation.sh" pcbrd --dir "$T/pc" --isa x86_64 --ember-upstream "$UP" >/dev/null
# the other ISAs: name isa model [scaffold options]
NEW_ISAS=("ppc ppc classic" "ppcbe ppc booke" "avr avr eplus" "avr32 avr32 a" "avr32b avr32 b"
          "pic pic pic18" "pic32 pic32 mx" "mcs51 mcs51 8052" "m6502 m6502 nmos --ram-size 0x7f00"
          "z80 z80 nmos" "xtensa xtensa lx6" "xtensaeb xtensaeb lx6")
for v in "${NEW_ISAS[@]}"; do
    read -r p isa model extra <<< "$v"
    "$SK/scripts/new-emulation.sh" "$p-test" --dir "$T/$p" --isa "$isa" --model "$model" $extra \
        --ember-upstream "$UP" >/dev/null || bad "$p: scaffold"
done
# one hash, even from a local clone (ls-remote HEAD also lists refs/remotes/origin/HEAD)
[[ "$(cat "$T/pc/ember.commit")" =~ ^[0-9a-f]{40}$ ]] && ok "new-emulation.sh: ember.commit is one hash" \
    || { bad "new-emulation.sh: ember.commit is one hash"; cat "$T/pc/ember.commit"; }
for p in mips rv arm armeb pc ppc ppcbe avr avr32 avr32b pic pic32 mcs51 m6502 z80 xtensa xtensaeb; do
    out="$("$T/$p/scripts/build-ember.sh" 2>&1)" && ok "$p: template builds" || { bad "$p: template builds"; echo "$out" | tail -20; }
    echo "$out" | grep -q "warning:" && bad "$p: template has warnings" || ok "$p: no warnings"
    echo "$out" | grep -q "registered" && ok "$p: board registered" || bad "$p: board registered"
done
# every other CPU model: the generated board compiles warning-free
for v in "ppc booke" "avr e" "avr xm" "avr xt" "avr rc" "pic baseline" "pic mid" "pic enhanced" \
         "pic32 mm" "pic32 mz" "mcs51 8051" "mcs51 cip51" "m6502 2a03" "m6502 65c02" "m6502 r65c02" \
         "z80 cmos" "xtensa lx106" "xtensa lx7" "xtensaeb lx106" "xtensaeb lx7"; do
    read -r isa model <<< "$v"; d="$T/models/$isa-$model"
    "$SK/scripts/new-emulation.sh" mt --dir "$d" --isa "$isa" --model "$model" --ember-upstream "$UP" >/dev/null \
        && out="$(cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
                  -I"$T/mips/ember-src/include" -I"$d/emulation" -c "$d/emulation/mt_board.c" -o /dev/null 2>&1)" \
        && [[ -z "$out" ]] && ok "$isa --model $model: board compiles warning-free" \
        || { bad "$isa --model $model: board compiles warning-free"; echo "$out" | head -10; }
done

# 2. bare boards stall on the console poll
run_bare() {  # proj image expect-addr-substring [exit code, default 124]
    [[ -f "$2" ]] || { echo "skip  $1 bare run ($2 missing: make -C $UP firmware-<isa>)"; return; }
    (cd "$T/$1" && mkdir -p build && IMAGE="$2" timeout -k 5 60 ./run.sh -n 2000000 > build/c.txt 2> build/e.log < /dev/null) || true
    python3 "$T/$1/scripts/log_summary.py" "$T/$1/build/e.log" > "$T/$1/build/s.txt"
    grep -q "exit ${4:-124}" "$T/$1/build/s.txt" && grep -q "$3" "$T/$1/build/s.txt" \
        && ok "$1: bare board stalls on the console UART ($3)" \
        || { bad "$1: bare board stalls on the console UART ($3)"; sed -n '2,3p;/first distinct/,/^$/p' "$T/$1/build/s.txt" | head -12; }
}
run_bare mips "$UP/build/firmware-mips/selftest-be.elf" "0x1fd003fd"
run_bare rv "$UP/build/firmware/selftest.elf" "0x10000005"
run_bare arm "$UP/build/firmware-arm/selftest-arm.elf" "0x1c000005"
run_bare armeb "$UP/build/firmware-arm/selftest-arm-be.elf" "0x1c000005"
run_bare ppc "$UP/build/firmware-ppc/selftest.elf" "0xf0000005"
run_bare ppcbe "$UP/build/firmware-ppc/selftest-booke.elf" "0xf0000005"
run_bare avr "$UP/build/firmware-avr/selftest-eplus.elf" "phys 0x8000e5"
run_bare avr32 "$UP/build/firmware-avr32/selftest-a.elf" "0xffff1405"
run_bare avr32b "$UP/build/firmware-avr32/selftest-b.elf" "0xffff1405"
run_bare pic "$UP/build/firmware-pic/selftest-pic18.elf" "phys 0x800f65"
run_bare pic32 "$UP/build/firmware-pic32/selftest-mx.elf" "0x1f820005"
run_bare mcs51 "$UP/build/firmware-mcs51/selftest-8052.bin" "phys 0x2000098"
run_bare m6502 "$UP/build/firmware-6502/selftest-nmos.bin" "0x7f05"
run_bare z80 "$UP/build/firmware-z80/selftest-nmos.bin" "phys 0x10000)" 125
run_bare xtensa "$UP/build/firmware-xtensa/selftest-lx6.elf" "0x60000005"
run_bare xtensaeb "$UP/build/firmware-xtensa/selftest-lx6-be.bin" "0x60000005"
# the PC template runs ember's x86 self-test as a raw stage (real mode at 0); its software
# SMIs need the stand-in APM port
X86ST="$UP/build/firmware-x86/selftest.bin"
if [[ -f "$X86ST" ]]; then
    for v in "" "--jit"; do
        (cd "$T/pc" && mkdir -p build && IMAGE="$X86ST" timeout -k 5 60 ./run.sh -o apm-smi=1 $v -n 20000000 \
            > build/st.txt 2> build/st.log < /dev/null) || true
        grep -q "x86 selftest: real prot long PASS" "$T/pc/build/st.txt" && ok "pc: template runs the x86 self-test ${v:-(interp)}" \
            || { bad "pc: template runs the x86 self-test $v"; cat "$T/pc/build/st.txt"; tail -3 "$T/pc/build/st.log"; }
    done
    # without the APM port the SMI goes nowhere: log_summary must say so
    (cd "$T/pc" && IMAGE="$X86ST" timeout -k 5 60 ./run.sh -n 20000000 > build/nosmi.txt 2> build/nosmi.log < /dev/null) || true
    python3 "$T/pc/scripts/log_summary.py" "$T/pc/build/nosmi.log" | grep -q "software SMI lost" \
        && ok "log_summary: a software SMI nothing decodes is reported" \
        || { bad "log_summary: lost software SMI"; grep -i "b2" "$T/pc/build/nosmi.log" | head -3; }
else
    echo "skip  pc: x86 self-test ($X86ST missing: make -C $UP firmware-x86)"
fi

# 3. fixture firmware on the wired board
P="$T/mips"
cp "$SK"/references/examples/*.[ch] "$P/emulation/"
cp "$HERE"/fixture/testbrd_*.[ch] "$P/emulation/"
check "fixture: rebuild" "$P/scripts/rebuild.sh"
if command -v mips-linux-gnu-gcc >/dev/null; then
    mkdir -p "$T/fw"
    mips-linux-gnu-gcc -march=mips32r2 -mabi=32 -EB -O2 -std=gnu11 -ffreestanding -fno-builtin \
        -fno-pic -mno-abicalls -fno-stack-protector -nostdlib -nostartfiles -G0 -static -no-pie \
        -Wl,-T,"$HERE/fixture/fw/link.ld" -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
        "$HERE/fixture/fw/start.S" "$HERE/fixture/fw/main.c" -o "$T/fw/fw.elf"
    mips-linux-gnu-objcopy -O binary "$T/fw/fw.elf" "$T/fw/fw.bin"
    expect=$'boot a0=00001234 sp=803fffe0\nchip reset\nboot a0=00001234 sp=803fffe0\nafter reset\nflash ok\nirq ok'
    for v in "con=stride" "con=soc" "con=stride --jit"; do
        rc=0; (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -o $v -n 5000000 > build/fx.txt 2> build/fx.log < /dev/null) || rc=$?
        [[ "$rc" -eq 0 && "$(cat "$P/build/fx.txt")" == "$expect" ]] && ok "fixture run ($v)" \
            || { bad "fixture run ($v): exit $rc"; cat "$P/build/fx.txt"; tail -5 "$P/build/fx.log"; }
    done
    rc=0; (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -o hle=1 -n 5000000 > build/hx.txt 2> build/hx.log < /dev/null) || rc=$?
    [[ "$rc" -eq 0 && "$(cat "$P/build/hx.txt")" == "00001234803fffe000001234803fffe0" ]] \
        && ok "HLE hook replaces puts_" || bad "HLE hook replaces puts_ (exit $rc)"
    rc=0; (cd "$P" && IMAGE="$T/fw/fw.bin" timeout -k 5 30 ./run.sh -o noirq=1 -n 5000000 > build/nx.txt 2> build/nx.log < /dev/null) || rc=$?
    [[ "$rc" -eq 125 ]] && ok "unwired interrupt ends in deadlock (exit 125)" || bad "unwired interrupt: exit $rc, expected 125"
    rc=0; (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -o con=stride -n 5000000 --mmio-rule 0x18070000=fixed:0x600d \
        > build/rx.txt 2> build/rx.log < /dev/null) || rc=$?
    [[ "$rc" -eq 0 && "$(cat "$P/build/rx.txt")" == "${expect/flash ok/flash ok$'\n'rule ok}" ]] \
        && ok "--mmio-rule answers an unmodelled register" || bad "--mmio-rule answers an unmodelled register (exit $rc)"
    # snapshot split runs: same console, same final icount as the uninterrupted run
    (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -o con=stride -n 5000000 > build/sn-full.txt 2> build/sn-full.log < /dev/null) || true
    full_end="$(grep -o 'after [0-9]* instructions' "$P/build/sn-full.log")"
    for v in "con=stride 1500 - --jit" "con=stride 3150 --jit -" "con=soc 2600 - -" "con=soc 3150 --jit --jit"; do
        read -r con n a b <<< "$v"; [[ "$a" == - ]] && a=""; [[ "$b" == - ]] && b=""
        rm -f "$P/build/s.snap"; rc=0
        (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -o $con -n $n $a --save-snapshot build/s.snap \
            > build/s1.txt 2> build/s1.log < /dev/null) || true
        (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -o $con -n 5000000 $b --load-snapshot build/s.snap \
            > build/s2.txt 2> build/s2.log < /dev/null) || rc=$?
        [[ "$rc" -eq 0 && "$(cat "$P/build/s1.txt" "$P/build/s2.txt")" == "$expect" ]] \
            && grep -q "$full_end" "$P/build/s2.log" && ok "snapshot split ($v)" \
            || { bad "snapshot split ($v): exit $rc"; grep -h "error" "$P/build/s1.log" "$P/build/s2.log" | head -3; }
    done
    # ember-fuzz: snapshot at fuzz_entry, inputs through the board's "uart0" input
    nm_of() { mips-linux-gnu-nm "$T/fw/fw.elf" | awk -v s="$1" '$3==s {print "0x"$1}'; }
    mkdir -p "$T/in"; printf 'ok!' > "$T/in/1-ok"; printf 'BUG' > "$T/in/2-bug"
    rc=0; (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -n 5000000 --cov-blocks --fuzz-begin "$(nm_of fuzz_entry)" \
        --fuzz-crash-pc "$(nm_of fuzz_bug)" --fuzz-input uart0 --fuzz-iter-insns 100000 \
        --fuzz-replay "$T/in" > build/fz.txt 2> build/fz.log < /dev/null) || rc=$?
    [[ "$rc" -eq 21 ]] && grep -q "1-ok: ok" "$P/build/fz.txt" && grep -q "2-bug: crash-oracle" "$P/build/fz.txt" \
        && ok "ember-fuzz: replay through the board's uart0 input (exit 21)" || bad "ember-fuzz: replay (exit $rc, expected 21)"
    python3 "$HERE/../ember-fuzz/scripts/replay_summary.py" "$P/build/fz.txt" > "$P/build/fz-sum.txt" 2>&1 \
        && grep -q "^2 inputs: 1 crash-oracle, 1 ok" "$P/build/fz-sum.txt" \
        && grep -q "^crash-oracle *pc=$(nm_of fuzz_bug)  x1" "$P/build/fz-sum.txt" \
        && ok "ember-fuzz: replay_summary groups the verdicts" || { bad "ember-fuzz: replay_summary"; cat "$P/build/fz-sum.txt"; }
    # ember-fuzz: a read guard on the soc_uart RX register, which only fuzz_entry reads:
    # the boot passes, every input is a crash-guard (exit 25), grouped by pc and guard
    rc=0; (cd "$P" && IMAGE="$T/fw/fw.bin" ./run.sh -n 5000000 --fuzz-begin "$(nm_of fuzz_entry)" \
        --fuzz-crash-pc "$(nm_of fuzz_bug)" --fuzz-input uart0 --fuzz-iter-insns 100000 \
        --guard p:0x18030000+4:r,name=rx --fuzz-replay "$T/in" > build/fg.txt 2> build/fg.log < /dev/null) || rc=$?
    python3 "$HERE/../ember-fuzz/scripts/replay_summary.py" "$P/build/fg.txt" > "$P/build/fg-sum.txt" 2>&1 || true
    [[ "$rc" -eq 25 ]] && grep -q "1-ok: crash-guard pc=0x[0-9a-f]* (read 4 at 0x18030000: +0 into guard rx " "$P/build/fg.txt" \
        && grep -q "^2 inputs: 2 crash-guard" "$P/build/fg-sum.txt" \
        && grep -q "^crash-guard *pc=0x[0-9a-f]* (read 4, guard rx)  x2" "$P/build/fg-sum.txt" \
        && ok "ember-fuzz: --guard gives crash-guard (exit 25), replay_summary groups it" \
        || { bad "ember-fuzz: crash-guard (exit $rc, expected 25)"; cat "$P/build/fg.txt" "$P/build/fg-sum.txt"; tail -3 "$P/build/fg.log"; }
    python3 "$P/scripts/log_summary.py" "$P/build/fx.log" > "$P/build/fx-sum.txt"
    grep -q "resets: {'system': 1}" "$P/build/fx-sum.txt" && grep -q "interrupt entries: 1" "$P/build/fx-sum.txt" \
        && ok "log_summary reports reset + interrupt" || bad "log_summary reports reset + interrupt"
else
    echo "skip  fixture firmware (no mips-linux-gnu-gcc)"
fi

# 4. the PC platform: the x86 template + references/examples/pc as a Q35-class chipset
Q="$T/pc"
cp "$SK"/references/examples/pc/*.[ch] "$Q/emulation/"
cp "$HERE"/fixture/pc/pcbrd_board.c "$Q/emulation/"
check "pc fixture: rebuild" "$Q/scripts/rebuild.sh"
if echo 'int x;' | cc -m32 -c -x c -o /dev/null - 2>/dev/null; then
    F="$T/pcfw"; mkdir -p "$F"
    make -s -C "$HERE/fixture/pc/fw" OUT="$F" >/dev/null 2>&1 && ok "pc fixture firmware builds" || bad "pc fixture firmware builds"
    pc_run() {  # out-file flash extra-args...
        local out="$1" fl="$2"; shift 2
        (cd "$Q" && FLASH="$fl" timeout -k 5 120 ./run.sh --ram 64M -n 50000000 "$@" \
            > "build/$out.txt" 2> "build/$out.log" < /dev/null)
    }
    cfi_expect=$'pcfw boot bist=00000000\nchipset ok\nsmm ok\nreset ok\ndone'
    spi_expect=$'sio ok\npcfw boot bist=00000000\nchipset ok\nsmm ok\nspi ok\nsmbus ok\nsio ok\nreset ok\ndone'
    SPI_ARGS=(-o flash=spi -o superio=1 --image spd="$F/spd.bin")
    for v in "" "--jit"; do
        rc=0; pc_run cfi "$F/fw.bin" $v || rc=$?
        [[ "$rc" -eq 0 && "$(cat "$Q/build/cfi.txt")" == "$cfi_expect" ]] && ok "pc fixture from CFI flash ${v:-(interp)}" \
            || { bad "pc fixture from CFI flash $v: exit $rc"; cat "$Q/build/cfi.txt"; tail -3 "$Q/build/cfi.log"; }
        rc=0; pc_run spi "$F/spi.bin" "${SPI_ARGS[@]}" $v || rc=$?
        [[ "$rc" -eq 0 && "$(cat "$Q/build/spi.txt")" == "$spi_expect" ]] && ok "pc fixture from SPI flash, Super I/O, SMBus ${v:-(interp)}" \
            || { bad "pc fixture from SPI flash $v: exit $rc"; cat "$Q/build/spi.txt"; tail -3 "$Q/build/spi.log"; }
    done
    python3 "$Q/scripts/log_summary.py" "$Q/build/spi.log" > "$Q/build/spi-sum.txt"
    grep -q "resets: {'system': 1}" "$Q/build/spi-sum.txt" && grep -q "handler pcs: {'38000': 1}" "$Q/build/spi-sum.txt" \
        && ok "log_summary reports the 0xCF9 reset and the SMI" || bad "log_summary reports the 0xCF9 reset and the SMI"
    # snapshot splits across the chipset, SMM, SPI and post-reset phases
    pc_run full "$F/spi.bin" "${SPI_ARGS[@]}" || true
    full_end="$(grep -o 'after [0-9]* instructions' "$Q/build/full.log")"
    for v in "3000 - --jit" "9000 --jit -" "12000 --jit --jit" "13500 - -"; do
        read -r n a b <<< "$v"; [[ "$a" == - ]] && a=""; [[ "$b" == - ]] && b=""
        rm -f "$Q/build/s.snap"; rc=0
        (cd "$Q" && FLASH="$F/spi.bin" ./run.sh --ram 64M "${SPI_ARGS[@]}" -n $n $a --save-snapshot build/s.snap \
            > build/s1.txt 2> build/s1.log < /dev/null) || true
        (cd "$Q" && FLASH="$F/spi.bin" ./run.sh --ram 64M "${SPI_ARGS[@]}" -n 50000000 $b --load-snapshot build/s.snap \
            > build/s2.txt 2> build/s2.log < /dev/null) || rc=$?
        [[ "$rc" -eq 0 && "$(cat "$Q/build/s1.txt" "$Q/build/s2.txt")" == "$spi_expect" ]] \
            && grep -q "$full_end" "$Q/build/s2.log" && ok "pc snapshot split ($v)" \
            || { bad "pc snapshot split ($v): exit $rc"; grep -h "error" "$Q/build/s1.log" "$Q/build/s2.log" | head -3; }
    done
    # ember-fuzz on x86: inputs through the board's com1 input; an oracle crash and a #UD
    pc_nm() { nm "$F/fw.elf" | awk -v s="$1" '$3==s {print "0x"$1}'; }
    mkdir -p "$T/pcin"; printf 'ok!' > "$T/pcin/1-ok"; printf 'BUG' > "$T/pcin/2-bug"; printf 'UD!' > "$T/pcin/3-ud"
    rc=0; (cd "$Q" && FLASH="$F/fw.bin" TRACE= ./run.sh --ram 64M -n 50000000 --cov-blocks \
        --fuzz-begin "$(pc_nm fuzz_entry)" --fuzz-crash-pc "$(pc_nm fuzz_bug)" \
        --fuzz-crash-exc fetch,load,store,align,trap --fuzz-input com1 --fuzz-iter-insns 100000 \
        --fuzz-replay "$T/pcin" > build/fz.txt 2> build/fz.log < /dev/null) || rc=$?
    python3 "$HERE/../ember-fuzz/scripts/replay_summary.py" "$Q/build/fz.txt" > "$Q/build/fz-sum.txt" 2>&1 || true
    [[ "$rc" -eq 20 || "$rc" -eq 21 ]] && grep -q "^3 inputs: 1 crash-fault, 1 crash-oracle, 1 ok" "$Q/build/fz-sum.txt" \
        && grep -q "^crash-fault *pc=0x[0-9a-f]* (trap cause 0x6)  x1" "$Q/build/fz-sum.txt" \
        && ok "ember-fuzz on x86: com1 route, oracle crash and #UD" \
        || { bad "ember-fuzz on x86 (exit $rc)"; cat "$Q/build/fz-sum.txt"; }
    # gdb_trace_calls.py on 32-bit protected-mode code
    if command -v gdb-multiarch >/dev/null; then
        (cd "$Q" && FLASH="$F/fw.bin" timeout -k 5 60 ./run.sh --ram 64M --gdb 47391 -n 50000000 \
            > build/g.txt 2> build/g.log < /dev/null &)
        sleep 1
        timeout -k 5 60 gdb-multiarch -q -batch -ex 'target remote :47391' -ex "source $Q/scripts/gdb_trace_calls.py" \
            -ex 'trace-abi i386' -ex "trace $(pc_nm fw_main) fw_main" -ex "trace $(pc_nm fuzz_entry) fuzz_entry" \
            -ex continue > "$Q/build/gdb.txt" 2>&1 || true
        [[ "$(grep -c '^-> fw_main(0x0, ' "$Q/build/gdb.txt")" -eq 2 ]] && grep -q "<- fuzz_entry returned" "$Q/build/gdb.txt" \
            && ok "gdb_trace_calls.py traces x86 calls" || { bad "gdb_trace_calls.py traces x86 calls"; tail -5 "$Q/build/gdb.txt"; }
    else
        echo "skip  gdb_trace_calls.py on x86 (no gdb-multiarch)"
    fi
    # OVMF to the UEFI shell on the same board (CFI code + vars), JIT, about a minute
    OV="${OVMF_DIR:-/usr/share/OVMF}"
    if [[ -f "$OV/OVMF_CODE_4M.fd" && -f "$OV/OVMF_VARS_4M.fd" ]]; then
        cp "$OV/OVMF_VARS_4M.fd" "$Q/build/vars.fd"
        (cd "$Q" && CODE="$OV/OVMF_CODE_4M.fd" VARS=build/vars.fd TRACE= timeout -k 5 600 ./run.sh --jit \
            -n 12000000000 > build/ovmf.txt 2> build/ovmf.log < /dev/null) || true
        tr -d '\000-\010\016-\037' < "$Q/build/ovmf.txt" | grep -aq "UEFI Interactive Shell" \
            && tr -d '\000-\010\016-\037' < "$Q/build/ovmf.txt" | grep -aq "Shell>" \
            && ok "OVMF boots to the UEFI shell on the PC fixture board" \
            || { bad "OVMF boots to the UEFI shell"; tail -3 "$Q/build/ovmf.log"; }
    else
        echo "skip  OVMF boot (no $OV/OVMF_CODE_4M.fd + OVMF_VARS_4M.fd)"
    fi
else
    echo "skip  pc fixture firmware (cc has no -m32 support: install gcc-multilib)"
fi

# 4b. recon tools on PC images (ember-dev-recon): uefi_scan.py on the fixture SPI image and,
# when installed, OVMF; FindX86Io.java on the fixture firmware when Ghidra is installed, and
# ghidra-query.sh (the MCP fallback) on the project that import made, also while it is locked
RS="$HERE/../ember-dev-recon/scripts"
if [[ -f "$T/pcfw/spi.bin" ]]; then
    python3 "$RS/uefi_scan.py" "$T/pcfw/spi.bin" > "$T/scan-spi.txt" 2>&1 || true
    grep -q "BIOS region flash 0x80000-0xfffff -> physical 0xfff80000-0xffffffff" "$T/scan-spi.txt" \
        && grep -q "jmp near 0x0000 -> 0xffff0000" "$T/scan-spi.txt" \
        && ok "uefi_scan.py: flash descriptor, BIOS window and reset vector" \
        || { bad "uefi_scan.py on the fixture SPI image"; head -20 "$T/scan-spi.txt"; }
fi
OV="${OVMF_DIR:-/usr/share/OVMF}"
if [[ -f "$OV/OVMF_CODE_4M.fd" ]]; then
    python3 "$RS/uefi_scan.py" "$OV/OVMF_CODE_4M.fd" --extract "$T/ovmf-mods" > "$T/scan-ovmf.txt" 2>&1 || true
    grep -q "SEC .*SecMain .*XIP 0x[0-9a-f]* (rebased)" "$T/scan-ovmf.txt" && grep -q "PEI_CORE .*PeiCore" "$T/scan-ovmf.txt" \
        && grep -q "DXE_CORE .*DxeCore" "$T/scan-ovmf.txt" && grep -q "PlatformPei" "$T/ovmf-mods/modules.tsv" \
        && ok "uefi_scan.py: OVMF volumes (LZMA), modules, XIP SEC, extraction" \
        || { bad "uefi_scan.py on OVMF"; tail -20 "$T/scan-ovmf.txt"; }
    # recon.sh symlinks the modules.tsv paths into a selection dir: they must be absolute
    # even when --extract is relative
    (cd "$T" && python3 "$RS/uefi_scan.py" "$OV/OVMF_CODE_4M.fd" --extract ovmf-rel >/dev/null 2>&1) || true
    [[ -f "$T/ovmf-rel/modules.tsv" ]] && awk -F'\t' 'NR > 1 && $9 != "-" && $9 !~ /^\// { bad = 1 } END { exit bad }' "$T/ovmf-rel/modules.tsv" \
        && ok "uefi_scan.py: modules.tsv paths are absolute with a relative --extract" \
        || { bad "uefi_scan.py: relative paths in modules.tsv"; head -3 "$T/ovmf-rel/modules.tsv"; }
fi
GH=""
for c in "${GHIDRA_HOME:-}/support/analyzeHeadless" "$(command -v analyzeHeadless 2>/dev/null)" "$HOME"/ghidra*/support/analyzeHeadless; do
    [[ -n "$c" && -x "$c" ]] && { GH="$c"; break; }
done
if [[ -n "$GH" && -f "$T/pcfw/fw.elf" ]]; then
    mkdir -p "$T/gp"
    timeout -k 5 900 "$GH" "$T/gp" pcfw -import "$T/pcfw/fw.elf" -scriptPath "$RS/ghidra" \
        -postScript FindX86Io.java "$T/x86io.txt" > "$T/gh.log" 2>&1 || true
    grep -q "^0x002e " "$T/x86io.txt" && grep -q "^00:1f.5 reg 0x0dc" "$T/x86io.txt" && grep -q "^0x0cf9 " "$T/x86io.txt" \
        && ok "FindX86Io.java: ports, PCI configuration addresses" \
        || { bad "FindX86Io.java on the fixture firmware"; tail -5 "$T/gh.log"; }
    # the MCP fallback: headless queries on that project, read-only
    GQ=(env GHIDRA_HOME="$(dirname "$(dirname "$GH")")" GHIDRA_PROJECTS="$T/gp" "$RS/ghidra-query.sh" --project pcfw)
    "${GQ[@]}" info > "$T/gq-info.txt" 2>&1 && grep -q "^=== program fw.elf (x86:LE:32" "$T/gq-info.txt" \
        && ok "ghidra-query.sh: info" || { bad "ghidra-query.sh: info"; tail -5 "$T/gq-info.txt"; }
    "${GQ[@]}" decompile fail > "$T/gq-dec.txt" 2>&1 && grep -q "^--- fail: fail @" "$T/gq-dec.txt" \
        && grep -q "^void fail(" "$T/gq-dec.txt" \
        && ok "ghidra-query.sh: decompile by name" || { bad "ghidra-query.sh: decompile"; tail -5 "$T/gq-dec.txt"; }
    "${GQ[@]}" xrefs fail > "$T/gq-xr.txt" 2>&1 && grep -q "CALL" "$T/gq-xr.txt" \
        && ok "ghidra-query.sh: xrefs (callers)" || { bad "ghidra-query.sh: xrefs"; tail -5 "$T/gq-xr.txt"; }
    a="$(awk '$NF == "puts_" {print $1}' <("${GQ[@]}" functions puts_ 2>/dev/null))" || true
    [[ -n "$a" ]] && "${GQ[@]}" disasm "0x$a" 2>/dev/null | grep -q "^--- 0x$a: puts_ @" \
        && ok "ghidra-query.sh: functions + disasm by address" || bad "ghidra-query.sh: functions + disasm"
    "${GQ[@]}" decompile no_such_function >/dev/null 2>&1 && bad "ghidra-query.sh: unknown name fails" \
        || ok "ghidra-query.sh: unknown name fails"
    # a project the GUI holds (a channel lock on <name>.lock~) is queried on a copy
    touch "$T/gp/pcfw.lock"
    python3 -c 'import fcntl, subprocess, sys
f = open(sys.argv[1], "w"); fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
sys.exit(subprocess.run(sys.argv[2:]).returncode)' "$T/gp/pcfw.lock~" "${GQ[@]}" info > "$T/gq-lock.txt" 2>&1 \
        && grep -q "querying a temporary copy" "$T/gq-lock.txt" && grep -q "^=== program fw.elf" "$T/gq-lock.txt" \
        && ok "ghidra-query.sh: locked project queried on a copy" || { bad "ghidra-query.sh: locked project"; tail -5 "$T/gq-lock.txt"; }
else
    echo "skip  FindX86Io.java (no Ghidra analyzeHeadless, or no fixture firmware)"
fi

# 5. layering and guardrails
printf '#include "uam/state.h"\n#include "uam/blockdev.h"\nint ok_dev_x;\n' > "$P/emulation/ok_dev.c"
check "device may include uam/state.h and uam/blockdev.h" "$P/scripts/check-layering.sh"
rm -f "$P/emulation/ok_dev.c"
printf '#include "uam/machine.h"\nint bad_dev_x;\n' > "$P/emulation/bad_dev.c"
if "$P/scripts/rebuild.sh" >/dev/null 2>&1; then bad "layering violation rejected"; else ok "layering violation rejected"; fi
rm -f "$P/emulation/bad_dev.c" "$P/build/obj/bad_dev."*
# Every ember change is a git format-patch file applied to the clone at the pin as a commit.
export GIT_AUTHOR_NAME="${GIT_AUTHOR_NAME:-skills-test}" GIT_AUTHOR_EMAIL="${GIT_AUTHOR_EMAIL:-skills-test@localhost}"
export GIT_COMMITTER_NAME="${GIT_COMMITTER_NAME:-skills-test}" GIT_COMMITTER_EMAIL="${GIT_COMMITTER_EMAIL:-skills-test@localhost}"
PIN="$(head -1 "$P/ember.commit")"
echo "/* stray */" >> "$P/ember-src/src/core/common.c"
if "$P/scripts/rebuild.sh" >/dev/null 2>&1; then bad "uncaptured ember edit rejected"; else ok "uncaptured ember edit rejected"; fi
if "$P/scripts/regen-patches.sh" >/dev/null 2>&1; then bad "regen-patches wants a commit message"; else ok "regen-patches wants a commit message"; fi
check "regen-patches -m commits and exports it" "$P/scripts/regen-patches.sh" -m "core: a test change"
PF="$(ls "$P"/patches/*.patch 2>/dev/null | head -1)"
[[ "$(ls "$P"/patches/*.patch 2>/dev/null | wc -l)" -eq 1 ]] && grep -q '^Subject: \[PATCH\] core: a test change$' "$PF" \
    && ok "one format-patch file with the message" || bad "one format-patch file with the message"
check "rebuild accepts the exported patch" "$P/scripts/rebuild.sh"
check "full build re-applies the patch" "$P/scripts/build-ember.sh"
[[ "$(git -C "$P/ember-src" log --format=%s "$PIN..HEAD")" == "core: a test change" && -z "$(git -C "$P/ember-src" status --porcelain -uno)" ]] \
    && grep -q "stray" "$P/ember-src/src/core/common.c" \
    && ok "patch applied as a commit on the pin" || bad "patch applied as a commit on the pin"
git clone -q "$UP" "$T/upx"; git -C "$T/upx" checkout -q -B main "$PIN"
git -C "$T/upx" am -q "$PF" && [[ "$(git -C "$T/upx" log -1 --format=%s)" == "core: a test change" ]] \
    && ok "the patch lands upstream with git am" || bad "the patch lands upstream with git am"
# Re-pin to a newer upstream: the patch is rebased; once upstream has taken it, it drops out.
git -C "$T/upx" reset -q --hard "$PIN"; echo "repin test" > "$T/upx/REPIN-TEST"
git -C "$T/upx" add REPIN-TEST; git -C "$T/upx" commit -q -m "upstream moves on"
git -C "$P/ember-src" remote set-url origin "$T/upx"
check "repin.sh moves to the latest commit" "$P/scripts/repin.sh" --no-build
[[ "$(head -1 "$P/ember.commit")" == "$(git -C "$T/upx" rev-parse HEAD)" && "$(ls "$P"/patches/*.patch | wc -l)" -eq 1 ]] \
    && ok "pin moved, patch carried" || bad "pin moved, patch carried"
git -C "$T/upx" am -q "$PF"
check "repin.sh onto an upstream that took the patch" "$P/scripts/repin.sh" --no-build
[[ "$(head -1 "$P/ember.commit")" == "$(git -C "$T/upx" rev-parse HEAD)" ]] && ! ls "$P"/patches/*.patch >/dev/null 2>&1 \
    && ok "a patch taken upstream drops out" || bad "a patch taken upstream drops out"
git -C "$P/ember-src" remote set-url origin "$UP"
git clone -q "$UP" "$T/wc"; echo "/* user work */" >> "$T/wc/Makefile"
"$SK/scripts/new-emulation.sh" wc-test --dir "$T/wcp" --ember-src "$T/wc" >/dev/null
if "$T/wcp/scripts/build-ember.sh" >/dev/null 2>&1; then bad "dirty working copy protected"; else ok "dirty working copy protected"; fi
grep -q "user work" "$T/wc/Makefile" && ok "working copy untouched" || bad "working copy untouched"

echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
