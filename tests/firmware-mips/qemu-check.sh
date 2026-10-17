#!/bin/sh
# Differential check for MIPS: run the same ELF under uam-run and under
# qemu-system-mips(el) -M mipssim, then compare console output. The firmware
# writes a UAM-only finisher and then spins; QEMU ignores that write and keeps
# spinning until the timeout, by which point its console output is complete.
#
# usage: qemu-check.sh UAM_RUN BOARD FIRMWARE.elf QEMU_SYSTEM
set -u
UAM_RUN=$1
BOARD=$2
FW=$3
QEMU=$4
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "qemu-check-mips: $QEMU not found; skipping" >&2
    exit 0
fi

"$UAM_RUN" -b "$BOARD" "$FW" > "$TMP/uam.out" 2>/dev/null
timeout 10 "$QEMU" -M mipssim -cpu 24Kf -m 32M -nographic -kernel "$FW" \
    > "$TMP/qemu.out" 2>/dev/null
# The finisher is UAM-only; QEMU spins until the timeout kills it. Compare the
# console text only (strip CR), ignoring that QEMU produced no clean exit.
tr -d '\r' < "$TMP/uam.out" > "$TMP/uam.txt"
tr -d '\r' < "$TMP/qemu.out" > "$TMP/qemu.txt"

if diff -u "$TMP/qemu.txt" "$TMP/uam.txt" > "$TMP/diff.txt"; then
    echo "qemu-check-mips[$BOARD]: identical console output ($(wc -l < "$TMP/uam.txt") lines)"
    exit 0
fi
echo "qemu-check-mips[$BOARD]: console output differs (--- qemu, +++ uam):"
cat "$TMP/diff.txt"
exit 1
