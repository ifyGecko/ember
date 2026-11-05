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

# Files our patches touch in the ember checkout ("+++ b/<path>" lines).
owned_paths() {    # $1 = project dir
    local p
    for p in "$1"/patches/*.patch; do
        [[ -f "$p" ]] && sed -n 's#^+++ b/##p' "$p"
    done
}

# Refuse to reset a checkout that holds tracked edits this project does not own (the
# user's own work on ember, or another project's patches), or - for a checkout that is not
# the project's own clone - one whose HEAD is not the pinned commit, since resetting would
# move the user's branch. Untracked files are never touched, so they are not checked.
guard_checkout() { # $1 = src, $2 = project dir, $3 = force (0/1), $4 = pinned commit
    local foreign head pinned
    foreign="$(git -C "$1" diff --name-only HEAD 2>/dev/null \
               | sort -u | grep -vxF -f <(owned_paths "$2" | sort -u; echo /dev/null) || true)"
    if [[ -n "$foreign" ]]; then
        echo "The ember checkout $1 has tracked changes this project does not own:" >&2
        echo "$foreign" | head -20 | sed 's/^/    /' >&2
        [[ "$3" -eq 1 ]] && { echo "--force given: they will be discarded." >&2; return; }
        _die "refusing to reset it. Commit/stash them, use a separate checkout (--ember-src, or a clone in the project), or pass --force."
    fi
    if ! project_owned_src "$1" "$2"; then
        head="$(git -C "$1" rev-parse HEAD 2>/dev/null || true)"
        pinned="$(git -C "$1" rev-parse --verify -q "$4^{commit}" 2>/dev/null || true)"
        if [[ -n "$head" && "$head" != "$pinned" && "$3" -ne 1 ]]; then
            _die "$1 is at ${head:0:12}, not the pinned ${4:0:12}; checking out would move a working copy you did not create. Re-pin (echo $head > ember.commit), use a project clone, or pass --force."
        fi
    fi
}

# Board name (from run.sh "-b NAME") and the ISA recorded by new-emulation.sh.
board_name_of() { grep -oE '(^|[[:space:]])-b[[:space:]]+[A-Za-z0-9_.-]+' "$1/run.sh" | head -1 | awk '{print $2}'; }
project_isa()  { sed -n 's/^ISA=//p' "$1/run.sh" | head -1; }
