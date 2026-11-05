#!/usr/bin/env bash
# regen-patches.sh - export the project's ember changes as patches/NNNN-<subject>.patch.
#
# Every change the emulation needs in ember itself lives as a `git format-patch` file in
# patches/, applied by build-ember.sh as a commit on top of the pinned commit in the
# project's ember clone. A patch keeps its commit message and author, so when the change is
# general enough for ember, landing it upstream is `git -C <ember> am <patch>`.
#
# Board, device and hook code is written against ember's public headers and never needs a
# patch; a patch is for a capability ember lacks (an ISA instruction, a CPU-model field, a
# bus or runner feature). One patch per logical change, with a message written for ember's
# history: what changed and why, in ember's own terms (no board or firmware names).
#
# Workflow: edit ember's files in the clone (git -C <clone> diff to review), commit each
# change there (git -C <clone> commit -a, plus git add for new files), then run this. Or let
# -m commit the pending edits as one change: tracked edits plus each --add PATH (new files,
# relative to the clone). Untracked files are never captured otherwise.
#
# Usage: regen-patches.sh [--proj DIR] [--ember-src DIR] [-m MESSAGE] [--add PATH ...]
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
P=""; S=""; MSG=""; ADD=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;; --ember-src) S="$2"; shift 2;;
        -m|--message) MSG="$2"; shift 2;;
        --add) ADD+=("$2"); shift 2;;
        -h|--help) sed -n '2,19p' "$0"; exit 0;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"; SRC="$(resolve_src "$S" "$PROJ")"
[[ -d "$SRC/.git" ]] || _die "not a git checkout: $SRC"
COMMIT="$(head -1 "$PROJ/ember.commit")"
PIN="$(git -C "$SRC" rev-parse --verify -q "$COMMIT^{commit}")" || _die "pinned commit $COMMIT not in $SRC"
git -C "$SRC" merge-base --is-ancestor "$PIN" HEAD \
    || _die "$SRC is not on top of the pinned ${COMMIT:0:12} (run build-ember.sh, or repin.sh to move the pin)"

edited="$(git -C "$SRC" diff --name-only HEAD)"
if [[ -n "$edited" || ${#ADD[@]} -gt 0 ]]; then
    [[ -n "$MSG" ]] || _die "uncommitted ember edits: $(echo $edited ${ADD[@]+"${ADD[@]}"}) - commit them in $SRC (one commit per change, the message it would carry upstream), or pass -m MESSAGE to commit them as one"
    for f in ${ADD[@]+"${ADD[@]}"}; do [[ -e "$SRC/$f" ]] || _die "--add $f: no such file in $SRC"; done
    git -C "$SRC" add -u
    [[ ${#ADD[@]} -eq 0 ]] || git -C "$SRC" add -- "${ADD[@]}"
    git -C "$SRC" commit -q -m "$MSG" || _die "commit failed (is git user.name/user.email set? the patch carries them as its author)"
    echo "regen-patches: committed $(git -C "$SRC" rev-parse --short HEAD) $(git -C "$SRC" log -1 --format=%s)"
elif [[ -n "$MSG" ]]; then
    _die "-m given but there are no ember edits to commit"
fi

untracked="$(git -C "$SRC" ls-files --others --exclude-standard | head -10)"
[[ -z "$untracked" ]] || echo "regen-patches: note: untracked files in $SRC are not captured: $(echo $untracked) (commit them, or --add PATH)" >&2

mkdir -p "$PROJ/patches"
rm -f "$PROJ"/patches/*.patch
n="$(git -C "$SRC" rev-list --count "$PIN..HEAD")"
if [[ "$n" -eq 0 ]]; then
    echo "regen-patches: no ember changes since ${COMMIT:0:12}; patches/ is empty."
    exit 0
fi
# --zero-commit and no signature: the files depend only on the changes, not on the clone.
git -C "$SRC" format-patch -q --zero-commit --no-signature -o "$PROJ/patches" "$PIN..HEAD"
for p in "$PROJ"/patches/*.patch; do
    echo "  $(basename "$p")  ($(grep -c '^[+-][^+-]' "$p") changed lines)"
done
echo "regen-patches: exported $n change(s) against ${COMMIT:0:12}. Review them, journal why ember needed them, then commit the project."
echo "  upstream, if general enough: git -C <ember> am $PROJ/patches/NNNN-*.patch"
