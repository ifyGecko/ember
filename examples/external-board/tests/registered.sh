#!/bin/sh
# registered.sh RUNNER BOARD: the runner lists the board.
"$1" -b list | grep -qw -- "$2" || { echo "board $2 is not registered by $1" >&2; exit 1; }
