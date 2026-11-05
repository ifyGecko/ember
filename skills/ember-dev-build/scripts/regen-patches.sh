#!/usr/bin/env bash
# regen-patches.sh - capture edits to ember's own files back into patches/.
#
# Patches are the exception: an ember board, its devices and its hooks are written against
# ember's public headers and never need one. Use a patch only when ember itself lacks a
# capability the target needs (an ISA instruction, a CPU model field, a bus or runner
# feature). Because ember is a project you can commit to, the better fix is usually to land
# the change in ember and re-pin ember.commit; a patch is the stopgap until then.
#
# After editing tracked files in the checkout (git -C <ember> diff to review), run this so
# the build stays reproducible from ember.commit + patches/. Produces one patch per modified
# tracked file: patches/NNNN-<flattened-path>.patch. New files ember needs go in with
# --add PATH (repeatable, relative to the checkout); untracked files are otherwise ignored.
#
# Usage: regen-patches.sh [--proj DIR] [--ember-src DIR] [--add PATH ...]
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
P=""; S=""; ADD=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proj) P="$2"; shift 2;; --ember-src) S="$2"; shift 2;;
        --add) ADD+=("$2"); shift 2;;
        *) _die "unknown argument: $1";;
    esac
done
PROJ="$(resolve_proj "$P")"; SRC="$(resolve_src "$S" "$PROJ")"
[[ -d "$SRC/.git" ]] || _die "not a git checkout: $SRC"
COMMIT="$(head -1 "$PROJ/ember.commit")"

mapfile -t changed < <(git -C "$SRC" diff --name-only --diff-filter=M "$COMMIT")
# Previously captured new files stay captured while they still exist.
while read -r f; do
    [[ -n "$f" && -f "$SRC/$f" ]] || continue
    git -C "$SRC" ls-files --error-unmatch -- "$f" >/dev/null 2>&1 || ADD+=("$f")
done < <(owned_paths "$PROJ")
mapfile -t added < <(printf '%s\n' "${ADD[@]+"${ADD[@]}"}" | sed '/^$/d' | sort -u)

if [[ ${#changed[@]} -eq 0 && ${#added[@]} -eq 0 ]]; then
    echo "regen-patches: no edits to ember files; nothing to capture."
    rm -f "$PROJ/patches"/*.patch
    exit 0
fi
mkdir -p "$PROJ/patches"
rm -f "$PROJ/patches"/*.patch
i=1
emit() {   # $1 = path, $2 = diff text
    local out
    out="$(printf '%s/patches/%04d-%s.patch' "$PROJ" "$i" "$(echo "$1" | sed 's#[/.]#-#g')")"
    printf '%s\n' "$2" > "$out"
    echo "  wrote $(basename "$out")  ($(grep -c '^[+-][^+-]' "$out") changed lines)"
    i=$((i+1))
}
for f in "${changed[@]}"; do
    emit "$f" "$(git -C "$SRC" diff "$COMMIT" -- "$f")"
done
for f in "${added[@]}"; do
    [[ -f "$SRC/$f" ]] || _die "--add $f: no such file in $SRC"
    emit "$f" "$(cd "$SRC" && git diff --no-index -- /dev/null "$f" || true)"
done
echo "regen-patches: captured $((i-1)) file(s). Review them, then commit the project."
