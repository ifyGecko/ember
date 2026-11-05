#!/usr/bin/env bash
# check-layering.sh - enforce ember's dependency rules (docs/architecture.md section 3) on
# the project's emulation/ sources. Run by rebuild.sh; runnable standalone.
#
#   board files   *_board.[ch], *_hle.[ch], main.c   may include any public ember header
#                 except uam/host.h (the runner owns the host side)
#   device files  everything else                    may include only the device-side
#                 interfaces: common.h access.h device.h signal.h sched.h chardev.h
#                 netdev.h blockdev.h state.h bus.h, stock device headers (uam/dev/*.h), and
#                 other device headers of this project - never an ISA, the CPU, the
#                 executor, the IR, the machine, the loader, the host, or a board header.
#
# A device that needs something from the machine (stop the run, reset the chip, the CPU
# clock) gets it as a callback, a uam_signal or a number from the board instead.
#
# Usage: check-layering.sh [--proj DIR]
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
P=""
while [[ $# -gt 0 ]]; do
    case "$1" in --proj) P="$2"; shift 2;; *) _die "unknown argument: $1";; esac
done
PROJ="$(resolve_proj "$P")"
ALLOWED='^uam/(common|access|device|signal|sched|chardev|netdev|blockdev|state|bus)\.h$|^uam/dev/[A-Za-z0-9_]+\.h$'
bad=0
shopt -s nullglob
for f in "$PROJ"/emulation/*.[ch]; do
    base="$(basename "$f")"
    while IFS= read -r inc; do
        case "$base" in
            *_board.[ch]|*_hle.[ch]|main.c)
                [[ "$inc" == "uam/host.h" ]] && { echo "layering: $base includes $inc (host side belongs to the runner)"; bad=1; }
                ;;
            *)
                if [[ "$inc" == uam/* ]]; then
                    [[ "$inc" =~ $ALLOWED ]] || { echo "layering: device file $base includes \"$inc\""; bad=1; }
                elif [[ "$inc" == *_board.h || "$inc" == *_hle.h ]]; then
                    echo "layering: device file $base includes board header \"$inc\""; bad=1
                fi
                ;;
        esac
    done < <(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$f")
done
[[ "$bad" -eq 0 ]] || _die "fix the includes above (see references/organization.md, rule 3)"
