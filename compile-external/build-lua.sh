#!/usr/bin/env bash
# build-lua.sh — compile the reference Lua interpreter with ficc and run the
# full test suite (testes/all.lua).
#
# Test mode builds the ltests C library (-DLUA_USER_H='"ltests.h"') and exports
# the interpreter's symbols (-rdynamic) so the dlopen-based tests resolve the
# API.
set -euo pipefail

PROJECT=lua
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=common.sh
. "$HERE/common.sh"
require_ficc

src=$(project_src lua)
out=$(project_build lua)

step "compile $PROJECT with $FICC"
rm -rf "$out"/*
for f in "$src"/*.c; do
    base=$(basename "$f")
    [ "$base" = onelua.c ] && continue
    say "CC ${base%.c}"
    "$FICC" -O2 -DLUA_USE_LINUX -DLUA_USER_H='"ltests.h"' -I"$src" \
        -c "$f" -o "$out/${base%.c}.o" || die "compile failed on $base"
done

step "link lua"
"$FICC" -O2 -rdynamic "$out"/*.o -o "$out/lua" -lm -ldl || die "link failed"

# all.lua dlopen()s the test C modules in testes/libs.  They are plain shared
# objects built by the host cc (ficc has no -shared/-fPIC); the interpreter is
# linked -rdynamic so the modules resolve the Lua API at load time.
step "build testes/libs (host cc)"
make -C "$src/testes/libs" -s all || die "testes/libs build failed"

# stdin must be a non-seekable stream: files.lua asserts that seeking stdin
# fails, which is false for /dev/null or a regular file.
step "test: testes/all.lua"
( cd "$src/testes" && printf '' | timeout 1800 "$out/lua" -W all.lua ) \
    || die "all.lua failed"

say "PASS: lua (testes/all.lua)"
