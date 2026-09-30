#!/usr/bin/env bash
# build-zlib.sh — compile zlib with ficc and run its test programs.
#
# zlib has no tagged godmother plan of its own; the recipe here mirrors
# `./configure --static && make test` without running configure: the fifteen
# library translation units plus test/minigzip.c and test/example.c, linked
# into the standard round-trip tests.
set -euo pipefail

PROJECT=zlib
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=common.sh
. "$HERE/common.sh"
require_ficc

src=$(project_src zlib)
out=$(project_build zlib)

# The configure-generated constants are supplied on the command line so the
# upstream zconf.h needs no patching.
ZFLAGS=(-O2 -I"$src"
        -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN
        -DHAVE_UNISTD_H=1 -DHAVE_STDARG_H=1)

step "compile $PROJECT with $FICC"
rm -rf "$out"/*
mapfile -t lib_objs < <(build_zlib_objects "$src" "$out") || die "compile failed"

step "build test programs"
"$FICC" "${ZFLAGS[@]}" -c "$src/test/minigzip.c" -o "$out/minigzip.o" || die "minigzip compile failed"
"$FICC" "${ZFLAGS[@]}" -c "$src/test/example.c" -o "$out/example.o" || die "example compile failed"
"$FICC" "${lib_objs[@]}" "$out/minigzip.o" -o "$out/minigzip" || die "minigzip link failed"
"$FICC" "${lib_objs[@]}" "$out/example.o" -o "$out/example" || die "example link failed"

step "test: minigzip round-trip"
run=$(mktemp -d "$out/run.XXXXXX")
trap 'rm -rf "$run"' EXIT
printf 'hello world\n' >"$run/in"
"$out/minigzip" <"$run/in" >"$run/out.gz" || die "minigzip compress failed"
"$out/minigzip" -d <"$run/out.gz" >"$run/out" || die "minigzip decompress failed"
cmp -s "$run/in" "$run/out" || die "minigzip round-trip mismatch"

step "test: example"
( cd "$run" && "$out/example" "$run/example.out" ) || die "example failed"

say "PASS: zlib (minigzip round-trip + example)"
