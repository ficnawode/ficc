#!/usr/bin/env bash
# build-libpng.sh — compile libpng (against a ficc-built zlib) and run the
# pngtest, pngvalid, and pngstest suites.
#
# libpng has no configure-generated constants that the compiler must know about
# beyond pnglibconf.h, which upstream ships prebuilt; we compile out-of-tree
# and run the tests/ driver scripts exactly as `make check` would.
set -euo pipefail

PROJECT=libpng
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=common.sh
. "$HERE/common.sh"
require_ficc

src=$(project_src libpng)
zsrc=$(project_src zlib)
out=$(project_build libpng)

PNG_SRCS="png pngerror pngget pngmem pngpread pngread pngrio pngrtran
          pngrutil pngset pngsimd pngtrans pngwio pngwrite pngwtran pngwutil"

step "configure pnglibconf.h"
rm -rf "$out"/*
cp "$src/pnglibconf.h.prebuilt" "$out/pnglibconf.h"
cp "$src/pngtest.png" "$out/pngtest.png"

PFLAGS=(-O2 -I"$out" -I"$src" -I"$zsrc")

step "compile libpng with $FICC"
png_objs=()
for f in $PNG_SRCS; do
    "$FICC" "${PFLAGS[@]}" -c "$src/$f.c" -o "$out/$f.o" || die "compile failed on $f.c"
    png_objs+=("$out/$f.o")
done

step "compile zlib with $FICC"
mkdir -p "$out/zlib"
mapfile -t zlib_objs < <(build_zlib_objects "$zsrc" "$out/zlib")

step "build test programs"
"$FICC" "${PFLAGS[@]}" -c "$src/pngtest.c" -o "$out/pngtest.o" || die "pngtest compile failed"
"$FICC" "${PFLAGS[@]}" -c "$src/contrib/libtests/pngvalid.c" -o "$out/pngvalid.o" || die "pngvalid compile failed"
"$FICC" "${PFLAGS[@]}" -c "$src/contrib/libtests/pngstest.c" -o "$out/pngstest.o" || die "pngstest compile failed"
"$FICC" "$out/pngtest.o"  "${png_objs[@]}" "${zlib_objs[@]}" -lm -o "$out/pngtest"
"$FICC" "$out/pngvalid.o" "${png_objs[@]}" "${zlib_objs[@]}" -lm -o "$out/pngvalid"
"$FICC" "$out/pngstest.o" "${png_objs[@]}" "${zlib_objs[@]}" -lm -o "$out/pngstest"

# The tests/ drivers assume cwd is the build dir with srcdir pointing at the
# source tree; the binaries are in cwd and the data under $srcdir/contrib.
run_driver() {
    local script="$1"
    say "TEST $(basename "$script")"
    ( cd "$out" && srcdir="$src" "$script" ) >"$out/$(basename "$script").log" 2>&1 \
        || { warn "FAIL $(basename "$script")"; return 1; }
}

step "run test suite"
failed=0
for script in "$src"/tests/pngtest-all \
              "$src"/tests/pngvalid-* \
              "$src"/tests/pngstest-*; do
    [ -e "$script" ] || continue
    run_driver "$script" || failed=1
done

[ "$failed" = 0 ] || die "libpng: one or more test drivers failed (see $out/*.log)"
say "PASS: libpng (pngtest + pngvalid + pngstest)"
