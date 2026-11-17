#!/usr/bin/env bash
# rebuild.sh - FAST incremental rebuild after editing emulation/ files (a new file needs
# its line in emulation/meson.build). Checks that the ember clone is still the pin plus
# patches/ (as commits, nothing uncommitted), that every emulation/*.c is listed, and the
# layering rules (check-layering.sh), then runs `meson compile -C build`: ember's libraries
# (a no-op unless ember changed) and build/<board>-run.
# Falls back to telling you to run build-ember.sh when the checkout drifted.
#
# Usage: rebuild.sh [--proj DIR] [--ember-src DIR] [--jobs N]
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P=""; S=""; JOBS="$(nproc 2>/dev/null || echo 4)"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;; --ember-src) S="$2"; shift 2;; --jobs) JOBS="$2"; shift 2;;
        -h|--help) sed -n "2,9p" "$0"; exit 0;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"; SRC="$(resolve_src "$S" "$PROJ")"
[[ -f "$PROJ/build/build.ninja" ]] || _die "not configured yet; run build-ember.sh first"
COMMIT="$(pin_of "$PROJ")"
case "$(clone_state "$SRC" "$PROJ" "$COMMIT")" in
    applied) ;;
    uncommitted)
        _die "ember files edited but not captured in patches/: $(git -C "$SRC" diff --name-only HEAD | xargs) - commit the change in $SRC with the message it would carry upstream and run regen-patches.sh (or regen-patches.sh -m MESSAGE), or build-ember.sh --force to discard it" ;;
    unexported)
        _die "$SRC has commits since ${COMMIT:0:12} that differ from patches/ - run regen-patches.sh to export them, or build-ember.sh --force to drop them" ;;
    *)
        _die "$SRC is no longer at the pinned ${COMMIT:0:12} plus patches/ - run build-ember.sh" ;;
esac
check_sources "$PROJ"
"$HERE/check-layering.sh" --proj "$PROJ"
rc=0; meson compile -C "$PROJ/build" -j "$JOBS" > "$PROJ/build/compile.out" 2>&1 || rc=$?
grep -E "error|warning:|FAILED" "$PROJ/build/compile.out" || true
[[ "$rc" -eq 0 ]] || _die "build failed (full output: build/compile.out)"
echo "== rebuild ok: $PROJ/build/$(board_name_of "$PROJ")-run =="
