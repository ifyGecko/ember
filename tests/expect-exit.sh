#!/bin/sh
# expect-exit.sh CODE COMMAND...: run COMMAND; succeed only if it exits with CODE.
code=$1; shift
rc=0; "$@" || rc=$?
[ "$rc" = "$code" ] || { echo "expect-exit: exit $rc, expected $code: $*" >&2; exit 1; }
