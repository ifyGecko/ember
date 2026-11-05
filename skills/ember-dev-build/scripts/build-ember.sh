#!/usr/bin/env bash
# build-ember.sh - FULL build: pristine ember checkout at ember.commit -> apply patches/ ->
# build ember's libraries out of tree (<proj>/.ember-build) -> build the project's runner
# (build/<board>-run). Nothing of ours is ever written or committed into ember: the
# project includes ember's public headers and links its library (libuam.a).
# For day-to-day edits of emulation/ files use rebuild.sh (no reset).
#
# Usage:
#   build-ember.sh [--proj DIR] [--ember-src DIR] [--jobs N] [--cflags "-O2 -g"] [--fresh] [--force]
#                  [--suzzer DIR|off]
#
#   --force   discard tracked changes in the checkout that this project does not own, or
#             move a working copy's HEAD to the pinned commit. Without it the script refuses.
#   --fresh   delete the project-local clone (<proj>/ember-src) and the library build, re-clone.
#   --suzzer  build ember's suzzer integration (--cov-sites, --cov-boundary, --fuzz-szr,
#             --cov-shm) against this suzzer checkout; "off" turns it off. Remembered in
#             <proj>/suzzer.dir. Only for the ember-fuzz skill; bring-up needs no suzzer.
# Env: PROJ_DIR, EMBER_SRC, EMBER_UPSTREAM (clone source; else <proj>/ember.upstream), CFLAGS.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

P=""; S=""; JOBS="$(nproc 2>/dev/null || echo 4)"; FRESH=0; FORCE=0; CF="${CFLAGS:--O2 -g}"; SZ=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;;
        --ember-src) S="$2"; shift 2;;
        --jobs) JOBS="$2"; shift 2;;
        --cflags) CF="$2"; shift 2;;
        --fresh) FRESH=1; shift;;
        --force) FORCE=1; shift;;
        --suzzer) SZ="$2"; shift 2;;
        -h|--help) sed -n '2,18p' "$0"; exit 0;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"
SRC="$(resolve_src "$S" "$PROJ")"
COMMIT="$(head -1 "$PROJ/ember.commit")"; [[ -n "$COMMIT" ]] || _die "empty ember.commit"
BOARD="$(board_name_of "$PROJ")"
LIBBUILD="$PROJ/.ember-build"
if [[ -n "$SZ" ]]; then
    [[ "$SZ" == off || -f "$SZ/src/lcv_formats.h" ]] || _die "not a suzzer checkout (no src/lcv_formats.h): $SZ"
    [[ "$SZ" == off ]] && echo off > "$PROJ/suzzer.dir" || (cd "$SZ" && pwd) > "$PROJ/suzzer.dir"
fi
SZ="$(head -1 "$PROJ/suzzer.dir" 2>/dev/null || true)"
SZARG=(); [[ -n "$SZ" ]] && SZARG=("SUZZER_DIR=${SZ/#off/}")

if [[ "$FRESH" -eq 1 ]]; then
    project_owned_src "$SRC" "$PROJ" && rm -rf "$SRC"
    rm -rf "$LIBBUILD"
fi
if [[ ! -d "$SRC/.git" ]]; then
    project_owned_src "$SRC" "$PROJ" || _die "not a git checkout: $SRC"
    UPSTREAM="${EMBER_UPSTREAM:-$(head -1 "$PROJ/ember.upstream" 2>/dev/null || true)}"
    [[ -n "$UPSTREAM" ]] || _die "no ember checkout and no clone source (set EMBER_UPSTREAM or write ember.upstream)"
    echo "--- cloning ember from $UPSTREAM into $SRC ---"
    git clone -q "$UPSTREAM" "$SRC"
fi

echo "--- pristine checkout of ${COMMIT:0:12} in $SRC ---"
git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch -q --tags origin
git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || _die "commit $COMMIT not found in $SRC or its origin"
guard_checkout "$SRC" "$PROJ" "$FORCE" "$COMMIT"
git -C "$SRC" checkout -q --force "$COMMIT"
git -C "$SRC" reset -q --hard "$COMMIT"
# Files a previous run's patches created are untracked now; remove only those.
while read -r f; do
    [[ -n "$f" ]] || continue
    git -C "$SRC" ls-files --error-unmatch -- "$f" >/dev/null 2>&1 || rm -f "$SRC/$f"
done < <(owned_paths "$PROJ")

shopt -s nullglob
for p in "$PROJ"/patches/*.patch; do
    echo "  apply $(basename "$p")"
    git -C "$SRC" apply --whitespace=nowarn "$p" \
        || _die "failed to apply $(basename "$p") (regenerate it against ${COMMIT:0:12})"
done

echo "--- ember libraries -> $LIBBUILD ---"
make -C "$SRC" BUILD="$LIBBUILD" CFLAGS="$CF" "${SZARG[@]}" -j"$JOBS" lib > "$PROJ/build-lib.log" 2>&1 \
    || { tail -30 "$PROJ/build-lib.log"; _die "ember library build failed (full log: build-lib.log)"; }
rm -f "$PROJ/build-lib.log"
cat > "$LIBBUILD/paths.mk" <<EOF
# written by build-ember.sh - where the project's Makefile finds ember
EMBER_SRC    := $SRC
EMBER_BUILD  := $LIBBUILD
EMBER_COMMIT := $COMMIT
EOF

echo "--- project runner ---"
make -C "$PROJ" -j"$JOBS" CFLAGS="$CF"
BIN="$PROJ/build/${BOARD}-run"
echo "== built: $BIN =="
if [[ -n "$BOARD" ]] && "$BIN" -b list 2>/dev/null | grep -qw -- "$BOARD"; then
    echo "== board '$BOARD' registered =="
else
    echo "note: board '${BOARD:-?}' not listed by '$BIN -b list' (registered in main.c?)"
fi
