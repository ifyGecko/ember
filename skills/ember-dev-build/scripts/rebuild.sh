#!/usr/bin/env bash
# rebuild.sh - FAST incremental rebuild after editing emulation/ files (new files are picked
# up automatically). Checks that the ember checkout is still at ember.commit with the patch
# set applied, re-runs ember's library make (a no-op unless a patch changed), checks the
# layering rules (check-layering.sh), then rebuilds build/<board>-run.
# Falls back to telling you to run build-ember.sh when the checkout drifted.
#
# Usage: rebuild.sh [--proj DIR] [--ember-src DIR] [--jobs N]
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P=""; S=""; JOBS="$(nproc 2>/dev/null || echo 4)"; CF="${CFLAGS:--O2 -g}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;; --ember-src) S="$2"; shift 2;; --jobs) JOBS="$2"; shift 2;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"; SRC="$(resolve_src "$S" "$PROJ")"
[[ -f "$PROJ/.ember-build/paths.mk" ]] || _die "ember not built yet; run build-ember.sh first"
COMMIT="$(head -1 "$PROJ/ember.commit")"
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$(git -C "$SRC" rev-parse "$COMMIT^{commit}" 2>/dev/null)" ]] \
    || _die "$SRC is no longer at the pinned ${COMMIT:0:12} - run build-ember.sh"
uncaptured="$(git -C "$SRC" diff --name-only HEAD | grep -vxF -f <(owned_paths "$PROJ"; echo /dev/null) || true)"
[[ -z "$uncaptured" ]] || _die "ember files edited but not captured in patches/: $(echo $uncaptured) - run regen-patches.sh to keep them, or build-ember.sh --force to discard them"
shopt -s nullglob
for p in "$PROJ"/patches/*.patch; do
    git -C "$SRC" apply --reverse --check "$p" 2>/dev/null \
        || _die "$(basename "$p") is not applied in $SRC (changed or tree reset) - run build-ember.sh"
done
make -C "$SRC" BUILD="$PROJ/.ember-build" CFLAGS="$CF" -j"$JOBS" lib >/dev/null 2>"$PROJ/.ember-build/lib.err" \
    || { cat "$PROJ/.ember-build/lib.err"; _die "ember library build failed"; }
"$HERE/check-layering.sh" --proj "$PROJ"
rc=0; make -C "$PROJ" -j"$JOBS" CFLAGS="$CF" > "$PROJ/.ember-build/make.out" 2>&1 || rc=$?
grep -E "error|warning:|Error [0-9]" "$PROJ/.ember-build/make.out" || true
[[ "$rc" -eq 0 ]] || _die "build failed (full output: .ember-build/make.out)"
echo "== rebuild ok: $PROJ/build/$(board_name_of "$PROJ")-run =="
