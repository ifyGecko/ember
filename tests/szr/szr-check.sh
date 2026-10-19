#!/bin/sh
# szr-check.sh RUN BOARD FIRMWARE - ember's SZR1 executor (--fuzz-szr) against
# fake_brain.py, with block coverage and with suzzer lists from mk_lists.py.
set -eu
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 "$here/../suzzer/mk_lists.py" "$3" "$tmp" > /dev/null
python3 "$here/fake_brain.py" "$1" "$2" "$3" "$tmp/sites.lcv3" "$tmp/begin.lcvb"
