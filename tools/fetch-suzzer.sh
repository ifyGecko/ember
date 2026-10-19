#!/bin/sh
# fetch-suzzer.sh [DIR] - clone suzzer for use with ember (`make suzzer`).
#
# ember and suzzer are separate projects in separate repositories; neither carries the
# other's files. To use them together, ember's build needs a suzzer checkout: an existing
# one passed as SUZZER_DIR=path, or a clone made by this script into DIR (default
# ./suzzer, which git ignores and the build picks up by itself). See docs/suzzer.md.
#
#   SUZZER_URL  repository to clone (default git@github.com:ifyGecko/suzzer.git)
#   SUZZER_REV  commit, tag or branch to check out (default: the default branch)
set -eu

dir=${1:-suzzer}
url=${SUZZER_URL:-git@github.com:ifyGecko/suzzer.git}
rev=${SUZZER_REV:-}

if [ -e "$dir/.git" ]; then
    echo "fetch-suzzer: $dir is already a suzzer checkout ($(git -C "$dir" rev-parse --short HEAD));" \
         "update it with git, or remove it to clone again"
else
    if [ -e "$dir" ]; then
        echo "fetch-suzzer: $dir exists and is not a git checkout; not touching it" >&2
        exit 1
    fi
    git clone "$url" "$dir"
    [ -z "$rev" ] || git -C "$dir" checkout --quiet "$rev"
    echo "fetch-suzzer: cloned $url into $dir ($(git -C "$dir" rev-parse --short HEAD))"
fi

if [ ! -f "$dir/src/lcv_formats.h" ]; then
    echo "fetch-suzzer: warning: $dir has no src/lcv_formats.h. ember needs a suzzer that" \
         "publishes its format header; until this one does, the build leaves the suzzer" \
         "integration out (or use SUZZER_DIR= with a checkout that has it)." >&2
fi
