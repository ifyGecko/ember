#!/usr/bin/env bash
# build-ember.sh - FULL build: the project's ember clone (subprojects/ember) reset to the
# pin (`revision` in subprojects/ember.wrap: the latest ember commit when the project was
# pinned) -> patches/ applied as commits on top of it (git am) -> `meson setup build` (the
# first time) -> `meson compile -C build`: ember's libraries, as the project's subproject,
# and the project's runner (build/<board>-run). The project's own code is never written
# into ember: it includes ember's public headers and links its library (libuam.a). The only
# ember changes are the patch files, each a commit ready to `git am` upstream.
# For day-to-day edits of emulation/ files use rebuild.sh (no reset).
#
# Usage:
#   build-ember.sh [--proj DIR] [--ember-src DIR] [--jobs N] [--cflags "..."] [--fresh] [--force]
#
#   --force   discard ember edits or commits in the clone that patches/ does not hold, or
#             move a working copy's HEAD to the pinned commit. Without it the script refuses.
#   --fresh   delete the project-local clone (<proj>/subprojects/ember) and the build
#             directory (build/, with the run outputs kept there), re-clone, configure anew.
#   --cflags  extra compiler flags for the project and ember (meson's c_args), e.g.
#             "-fsanitize=address"; meson options proper: `meson configure build -D...`.
# Env: PROJ_DIR, EMBER_SRC, EMBER_UPSTREAM (clone source; else the wrap file's url), CFLAGS.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

P=""; S=""; JOBS="$(nproc 2>/dev/null || echo 4)"; FRESH=0; FORCE=0; CF="${CFLAGS:-}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;;
        --ember-src) S="$2"; shift 2;;
        --jobs) JOBS="$2"; shift 2;;
        --cflags) CF="$2"; shift 2;;
        --fresh) FRESH=1; shift;;
        --force) FORCE=1; shift;;
        -h|--help) sed -n '2,20p' "$0"; exit 0;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"
SRC="$(resolve_src "$S" "$PROJ")"
COMMIT="$(pin_of "$PROJ")"; [[ -n "$COMMIT" ]] || _die "no revision in subprojects/ember.wrap"
BOARD="$(board_name_of "$PROJ")"
BUILD="$PROJ/build"
command -v meson >/dev/null || _die "meson not found (ember builds with meson and ninja)"

if [[ "$FRESH" -eq 1 ]]; then
    project_owned_src "$SRC" "$PROJ" && rm -rf "$SRC"
    rm -rf "$BUILD"
fi
if [[ ! -d "$SRC/.git" ]]; then
    project_owned_src "$SRC" "$PROJ" || _die "not a git checkout: $SRC"
    UPSTREAM="${EMBER_UPSTREAM:-$(upstream_of "$PROJ")}"
    [[ -n "$UPSTREAM" ]] || _die "no ember checkout and no clone source (set EMBER_UPSTREAM or the url in subprojects/ember.wrap)"
    echo "--- cloning ember from $UPSTREAM into $SRC ---"
    git clone -q "$UPSTREAM" "$SRC"
fi
link_src "$SRC" "$PROJ"

echo "--- pristine checkout of ${COMMIT:0:12} in $SRC ---"
git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch -q --tags origin
git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || _die "commit $COMMIT not found in $SRC or its origin"
guard_checkout "$SRC" "$PROJ" "$FORCE" "$COMMIT"
git -C "$SRC" am --abort >/dev/null 2>&1 || true
git -C "$SRC" checkout -q --force --detach "$COMMIT"
git -C "$SRC" reset -q --hard "$COMMIT"
# Files an older (plain-diff) patch set left untracked; remove only those.
while read -r f; do
    [[ -n "$f" ]] || continue
    git -C "$SRC" ls-files --error-unmatch -- "$f" >/dev/null 2>&1 || rm -f "$SRC/$f"
done < <(owned_paths "$PROJ")

# Each patch becomes a commit on top of the pin, with its own message and author, exactly
# as `git am` would land it upstream. The committer is whoever runs the build (a neutral
# name when git has no identity configured): those commits never leave the clone.
id_env=()
git -C "$SRC" config user.name >/dev/null 2>&1 || id_env+=(GIT_COMMITTER_NAME=ember-dev-build)
git -C "$SRC" config user.email >/dev/null 2>&1 || id_env+=(GIT_COMMITTER_EMAIL=ember-dev-build@localhost)
while read -r p; do
    echo "  apply $(basename "$p")"
    if grep -q '^Subject: ' "$p"; then
        env "${id_env[@]+"${id_env[@]}"}" git -C "$SRC" am -q --keep-cr --whitespace=nowarn "$p" \
            || { git -C "$SRC" am --abort >/dev/null 2>&1 || true
                 _die "$(basename "$p") does not apply to ${COMMIT:0:12}: rebase it onto the pin (repin.sh does this) or fix it in the clone and re-run regen-patches.sh"; }
    else
        # An older plain diff with no commit message: apply it as a commit named after the file.
        git -C "$SRC" apply --index --whitespace=nowarn "$p" \
            || _die "failed to apply $(basename "$p") to ${COMMIT:0:12}"
        env "${id_env[@]+"${id_env[@]}"}" GIT_AUTHOR_NAME="${GIT_AUTHOR_NAME:-ember-dev-build}" \
            GIT_AUTHOR_EMAIL="${GIT_AUTHOR_EMAIL:-ember-dev-build@localhost}" \
            git -C "$SRC" commit -q -m "$(basename "$p" .patch)" \
            || _die "failed to commit $(basename "$p")"
        echo "    (a plain diff: run regen-patches.sh to turn it into a commit with a message)"
    fi
done < <(patch_files "$PROJ")

check_sources "$PROJ"
echo "--- ember + project runner -> $BUILD ---"
if [[ ! -f "$BUILD/build.ninja" ]]; then
    mkdir -p "$BUILD"
    meson setup "$BUILD" "$PROJ" > "$PROJ/meson-setup.log" 2>&1 \
        || { tail -30 "$PROJ/meson-setup.log"; _die "meson setup failed (full log: meson-setup.log)"; }
    rm -f "$PROJ/meson-setup.log"
fi
if [[ -n "$CF" ]]; then
    meson configure "$BUILD" -Dc_args="$CF" >/dev/null || _die "meson configure -Dc_args failed"
fi
meson compile -C "$BUILD" -j "$JOBS" || _die "build failed"
BIN="$PROJ/build/${BOARD}-run"
echo "== built: $BIN =="
if [[ -n "$BOARD" ]] && "$BIN" -b list 2>/dev/null | grep -qw -- "$BOARD"; then
    echo "== board '$BOARD' registered =="
else
    echo "note: board '${BOARD:-?}' not listed by '$BIN -b list' (registered in main.c?)"
fi
