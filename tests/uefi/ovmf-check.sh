#!/bin/sh
# ovmf-check.sh RUN [OVMF_DIR] - boot an installed OVMF (code + vars images) on
# x86_64-ref to the UEFI shell and run a command there. Skipped when no OVMF
# is installed. Exit status 0 on success.
set -u
RUN=$1
DIR=${2:-/usr/share/OVMF}
CODE=$DIR/OVMF_CODE_4M.fd
VARS=$DIR/OVMF_VARS_4M.fd
if [ ! -f "$CODE" ] || [ ! -f "$VARS" ]; then
    echo "ovmf-check: skipped (no $CODE / $VARS)"
    exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$VARS" "$TMP/vars.fd"
# Escape skips startup.nsh, then `ver`; the run ends at the instruction limit.
{ sleep 30; printf '\033'; sleep 2; printf 'ver\r'; sleep 2; } |
    "$RUN" -b x86_64-ref --ram 512M --jit -n 12000000000 \
        --image code="$CODE" --image vars="$TMP/vars.fd" > "$TMP/out" 2>&1
if grep -q "UEFI Interactive Shell" "$TMP/out" && grep -q "UEFI v2" "$TMP/out"; then
    echo "ovmf-check: OVMF reached the UEFI shell"
    exit 0
fi
echo "ovmf-check: FAILED; output tail:"
tail -c 2000 "$TMP/out"
exit 1
