#!/bin/sh
# build-external.sh MODE SOURCE WORK EMBER RUNNER OUT: configure and build an out-of-tree
# board project (SOURCE) as an external project would, then copy its runner (RUNNER) to OUT.
#
#   MODE subproject   ember is the project's subproject: a copy of SOURCE in WORK/src gets
#                     subprojects/ember -> EMBER (ember's source tree), in place of the
#                     checkout its wrap file would fetch
#   MODE pkgconfig    ember is found through pkg-config, as an installed one would be:
#                     EMBER is the directory holding uam.pc (<build>/meson-uninstalled)
#
# A warning in the project's build fails it.
set -eu
mode=$1 src=$2 work=$3 ember=$4 runner=$5 out=$6
log=$work.log
mkdir -p "$work"
case $mode in
subproject)
    rm -rf "$work/src"
    cp -R "$src" "$work/src"
    rm -rf "$work/src/subprojects/ember"
    ln -s "$ember" "$work/src/subprojects/ember"
    src=$work/src
    set -- ;;
pkgconfig)
    set -- --pkg-config-path "$ember" -Dwrap_mode=nofallback ;;
*)  echo "build-external: unknown mode $mode" >&2; exit 2 ;;
esac
if [ ! -f "$work/build/build.ninja" ]; then
    meson setup "$work/build" "$src" "$@" > "$log" 2>&1 || { cat "$log" >&2; exit 1; }
fi
meson compile -C "$work/build" >> "$log" 2>&1 || { cat "$log" >&2; exit 1; }
if grep -q "warning:" "$log"; then cat "$log" >&2; echo "build-external: warnings" >&2; exit 1; fi
cp "$work/build/$runner" "$out"
