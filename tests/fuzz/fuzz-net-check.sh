#!/bin/sh
# fuzz-net-check.sh RUN BOARD FIRMWARE - uam-run --fuzz-net against fake_brain.py, with
# block coverage and with site lists from mk_lists.py.
set -eu
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 "$here/mk_lists.py" "$3" "$tmp" > /dev/null
python3 "$here/fake_brain.py" "$1" "$2" "$3" "$tmp/sites.uams" "$tmp/begin.uamb"
