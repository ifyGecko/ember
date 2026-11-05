#!/usr/bin/env bash
# repin.sh - move the project to the latest ember commit, carrying patches/ along.
#
# Fetches the clone's origin (the ember the project was cloned from), rebases the patch
# commits onto the new commit, writes it to ember.commit, re-exports patches/ and runs a full
# build. A patch that ember has meanwhile taken upstream drops out on its own (git rebase
# skips a commit whose change is already there). On a conflict it stops in the clone: resolve
# it there (git -C <clone> rebase --continue) and run `repin.sh --finish`.
#
# Afterwards: re-run the firmware, fix any API drift (board-skeleton.md, "API checklist"),
# re-take snapshots, and journal the move (old -> new commit, patches dropped or changed).
#
# Usage: repin.sh [--proj DIR] [--ember-src DIR] [--to REF] [--finish] [--no-build]
#   --to REF    pin to REF (a commit, tag or origin/<branch>) instead of origin's HEAD
#   --finish    after resolving a conflicted rebase: record the new pin, export, build
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P=""; S=""; TO=""; FINISH=0; BUILD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;; --ember-src) S="$2"; shift 2;;
        --to) TO="$2"; shift 2;; --finish) FINISH=1; shift;; --no-build) BUILD=0; shift;;
        -h|--help) sed -n '2,15p' "$0"; exit 0;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"; SRC="$(resolve_src "$S" "$PROJ")"
[[ -d "$SRC/.git" ]] || _die "no ember clone at $SRC; run build-ember.sh first"
OLD="$(head -1 "$PROJ/ember.commit")"
STATE_FILE="$PROJ/.ember-build/repin-target"

finish() {   # $1 = the new pinned commit
    git -C "$SRC" merge-base --is-ancestor "$1" HEAD || _die "the clone is not on top of ${1:0:12}"
    echo "$1" > "$PROJ/ember.commit"
    rm -f "$STATE_FILE"
    "$HERE/regen-patches.sh" --proj "$PROJ" --ember-src "$SRC"
    echo "repin: ${OLD:0:12} -> ${1:0:12} ($(git -C "$SRC" log -1 --format='%cs %s' "$1"))"
    [[ "$BUILD" -eq 0 ]] || "$HERE/build-ember.sh" --proj "$PROJ" --ember-src "$SRC"
    echo "repin: now re-run the firmware and journal the move."
}

if [[ "$FINISH" -eq 1 ]]; then
    [[ -s "$STATE_FILE" ]] || _die "no re-pin in progress"
    [[ ! -d "$(git -C "$SRC" rev-parse --git-path rebase-merge)" && ! -d "$(git -C "$SRC" rev-parse --git-path rebase-apply)" ]] \
        || _die "the rebase in $SRC is not finished (resolve, then git -C $SRC rebase --continue)"
    finish "$(head -1 "$STATE_FILE")"; exit 0
fi

[[ "$(clone_state "$SRC" "$PROJ" "$OLD")" == applied ]] \
    || _die "the clone is not ember.commit + patches/ (uncaptured ember work?); run regen-patches.sh or build-ember.sh first"
echo "--- fetching ember into $SRC ---"
git -C "$SRC" fetch -q --tags origin
git -C "$SRC" remote set-head origin -a >/dev/null 2>&1 || true
NEW="$(git -C "$SRC" rev-parse --verify -q "${TO:-origin/HEAD}^{commit}")" \
    || _die "cannot resolve ${TO:-origin/HEAD} in $SRC"
OLDC="$(git -C "$SRC" rev-parse "$OLD^{commit}")"
if [[ "$NEW" == "$OLDC" ]]; then echo "repin: already at ${NEW:0:12}, the latest"; exit 0; fi
mkdir -p "$PROJ/.ember-build"; echo "$NEW" > "$STATE_FILE"
n="$(git -C "$SRC" rev-list --count "$OLDC..HEAD")"
echo "--- rebasing $n patch commit(s) from ${OLDC:0:12} onto ${NEW:0:12} ---"
id_env=()
git -C "$SRC" config user.name >/dev/null 2>&1 || id_env+=(GIT_COMMITTER_NAME=ember-dev-build)
git -C "$SRC" config user.email >/dev/null 2>&1 || id_env+=(GIT_COMMITTER_EMAIL=ember-dev-build@localhost)
if ! env "${id_env[@]+"${id_env[@]}"}" git -C "$SRC" rebase -q --onto "$NEW" "$OLDC"; then
    echo "repin: a patch conflicts with the new ember. In $SRC: fix the files, git add them," >&2
    echo "       git rebase --continue (git rebase --skip if ember now does it itself), then" >&2
    _die "run repin.sh --finish. To give up: git -C $SRC rebase --abort && build-ember.sh"
fi
finish "$NEW"
