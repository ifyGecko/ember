#!/bin/sh
# Differential check: run the same firmware image under uam-run and under
# qemu-system-riscv32 (virt machine), then compare console output and exit
# status. Any difference means one of the two disagrees with the other about
# architectural behaviour.
#
# usage: qemu-check.sh UAM_RUN FIRMWARE.elf
set -u
UAM_RUN=$1
FW=$2
QEMU=${QEMU:-qemu-system-riscv32}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "qemu-check: $QEMU not found; skipping" >&2
    exit 0
fi

"$UAM_RUN" "$FW" > "$TMP/uam.out" 2> "$TMP/uam.err"
uam_rc=$?
timeout 30 "$QEMU" -M virt -bios none -m 128M -nographic -monitor none -serial stdio \
    -kernel "$FW" > "$TMP/qemu.out" 2> "$TMP/qemu.err"
qemu_rc=$?

tr -d '\r' < "$TMP/uam.out" > "$TMP/uam.txt"
tr -d '\r' < "$TMP/qemu.out" > "$TMP/qemu.txt"

status=0
if ! diff -u "$TMP/qemu.txt" "$TMP/uam.txt" > "$TMP/diff.txt"; then
    echo "qemu-check: console output differs (--- qemu, +++ uam):"
    cat "$TMP/diff.txt"
    status=1
fi
if [ "$uam_rc" -ne "$qemu_rc" ]; then
    echo "qemu-check: exit status differs: qemu=$qemu_rc uam=$uam_rc"
    status=1
fi
if [ $status -eq 0 ]; then
    echo "qemu-check: identical console output ($(wc -l < "$TMP/uam.txt") lines) and exit status ($uam_rc)"
fi
exit $status
