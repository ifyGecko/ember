# _common.sh - shared helpers for the ember-dev-build scripts (sourced, not executed).
#
# Project resolution (first match wins):
#   --proj DIR | $PROJ_DIR | the directory above this script if it holds ember.commit
#   (scripts copied into a project) | $PWD if it holds ember.commit
# Ember checkout resolution:
#   --ember-src DIR | $EMBER_SRC | path recorded in <proj>/ember.src | <proj>/ember-src (cloned
#   from <proj>/ember.upstream on the first full build)

_die() { echo "${0##*/}: $*" >&2; exit 1; }

resolve_proj() {   # $1 = explicit --proj value (may be empty)
    local here cand
    here="$(cd "$(dirname "${BASH_SOURCE[1]}")" && pwd)"
    for cand in "$1" "${PROJ_DIR:-}" "$here/.." "$PWD"; do
        [[ -n "$cand" && -f "$cand/ember.commit" ]] && { (cd "$cand" && pwd); return; }
    done
    _die "no emulation project found (need a dir with ember.commit; use --proj DIR or run inside it)"
}

resolve_src() {    # $1 = explicit --ember-src value, $2 = project dir
    if [[ -n "$1" ]]; then (cd "$1" 2>/dev/null && pwd) || echo "$1"
    elif [[ -n "${EMBER_SRC:-}" ]]; then echo "$EMBER_SRC"
    elif [[ -s "$2/ember.src" ]]; then eval echo "$(head -1 "$2/ember.src")"
    else echo "$2/ember-src"
    fi
}

# Is this checkout the project's own clone (safe to reset), or someone's working copy?
project_owned_src() { [[ "$1" == "$2/ember-src" ]]; }   # $1 = src, $2 = project dir

# The ember changes a project carries are patches/NNNN-*.patch, `git format-patch` files: one
# commit each, with its message and author. In the clone they are applied as commits on top
# of the pinned commit (build-ember.sh), so a patch upstream is `git am` of the same file.
patch_files() { local p; for p in "$1"/patches/*.patch; do [[ -f "$p" ]] && echo "$p"; done; }

# Files our patches touch in the ember checkout ("+++ b/<path>" lines).
owned_paths() {    # $1 = project dir
    local p
    while read -r p; do sed -n 's#^+++ b/##p' "$p"; done < <(patch_files "$1")
}

# Content ids of the patches, in order, and of the clone's commits since the pin.
patch_ids()  { patch_files "$1" | while read -r p; do cat "$p"; done | git patch-id --stable | awk '{print $1}'; }
commit_ids() { git -C "$1" format-patch --stdout --no-signature "$2..HEAD" | git patch-id --stable | awk '{print $1}'; }

# Where the clone stands against ember.commit + patches/:
#   applied      HEAD is the pin plus exactly the patches, as commits; no tracked edits
#   uncommitted  tracked files edited and not committed
#   unexported   HEAD is the pin plus commits that differ from patches/ (regen-patches.sh)
#   elsewhere    HEAD does not descend from the pin
clone_state() {    # $1 = src, $2 = project dir, $3 = pinned commit
    local pin
    pin="$(git -C "$1" rev-parse --verify -q "$3^{commit}" 2>/dev/null || true)"
    if [[ -n "$(git -C "$1" diff --name-only HEAD 2>/dev/null)" ]]; then echo uncommitted
    elif [[ -z "$pin" ]] || ! git -C "$1" merge-base --is-ancestor "$pin" HEAD 2>/dev/null; then echo elsewhere
    elif [[ "$(commit_ids "$1" "$pin")" == "$(patch_ids "$2")" ]]; then echo applied
    else echo unexported
    fi
}

# Refuse to reset a checkout that holds ember work not captured in patches/ (tracked edits,
# or commits since the pin that regen-patches.sh has not exported), or - for a checkout that
# is not the project's own clone - one that is not at the pin (plus our patches), since the
# reset would move a working copy someone else made. Untracked files are never touched.
guard_checkout() { # $1 = src, $2 = project dir, $3 = force (0/1), $4 = pinned commit
    local state; state="$(clone_state "$1" "$2" "$4")"
    case "$state" in
        uncommitted)
            echo "The ember checkout $1 has tracked edits not captured in patches/:" >&2
            git -C "$1" diff --name-only HEAD | head -20 | sed 's/^/    /' >&2
            [[ "$3" -eq 1 ]] && { echo "--force given: they will be discarded." >&2; return; }
            _die "refusing to reset it. Commit the change in $1 and run regen-patches.sh (or regen-patches.sh -m MESSAGE), or pass --force to discard it." ;;
        unexported)
            [[ "$3" -eq 1 ]] && { echo "--force given: commits not in patches/ will be dropped." >&2; return; }
            _die "$1 has commits since ${4:0:12} that are not in patches/; run regen-patches.sh to export them, or pass --force to drop them." ;;
        elsewhere)
            if ! project_owned_src "$1" "$2" && [[ "$3" -ne 1 ]]; then
                _die "$1 is at $(git -C "$1" rev-parse --short HEAD), not the pinned ${4:0:12}; checking out would move a working copy you did not create. Use the project's own clone, or pass --force."
            fi ;;
    esac
}

# Board name (from run.sh "-b NAME") and the ISA recorded by new-emulation.sh.
board_name_of() { grep -oE '(^|[[:space:]])-b[[:space:]]+[A-Za-z0-9_.-]+' "$1/run.sh" | head -1 | awk '{print $2}'; }
project_isa()  { sed -n 's/^ISA=//p' "$1/run.sh" | head -1; }
