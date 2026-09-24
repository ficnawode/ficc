#!/usr/bin/env bash
# Codegen-quality measurement harness (never run by `make test`).
#
#   bench/run.sh <ficc> <srcdir>
#
# Prints a `key value` report and, when bench/baseline.txt exists, the delta
# against it. LUA_DIR=<lua source tree> additionally builds and times the Lua
# interpreter (requires the tree's testes/all.lua and /usr/bin/time).
set -u

FICC=${1:?usage: run.sh <ficc> <srcdir>}
SRCDIR=${2:?usage: run.sh <ficc> <srcdir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BASELINE="$HERE/baseline.txt"
INCLUDE="$ROOT/include"

ELFSIZE="$HERE/elfsize"
if [ ! -x "$ELFSIZE" ] || [ "$HERE/elfsize.c" -nt "$ELFSIZE" ]; then
    cc -O2 -o "$ELFSIZE" "$HERE/elfsize.c" || exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
: >"$TMP/report"

report() {
    printf '%s %s\n' "$1" "$2" | tee -a "$TMP/report"
}

# --- self-compile ------------------------------------------------------------------
self_objects() {
    local out=$1 level=$2 f base
    mkdir -p "$out/util" "$out/optpasses"
    for f in "$SRCDIR"/*.c "$SRCDIR"/util/*.c "$SRCDIR"/optpasses/*.c; do
        [ -e "$f" ] || continue
        base=${f#"$SRCDIR"/}
        "$FICC" "$level" -I"$SRCDIR" "-DFICC_BUILTIN_INCLUDE=\"$INCLUDE\"" \
            -c "$f" -o "$out/${base%.c}.o" || return 1
    done
    return 0
}

SOBJ="$TMP/self"
if ! self_objects "$SOBJ" -O1; then
    echo "self: compilation failed" >&2
    exit 1
fi
"$ELFSIZE" "$SOBJ"/*.o "$SOBJ"/util/*.o "$SOBJ"/optpasses/*.o >"$TMP/self.size" || exit 1
report self.text "$(awk '{print $2}' "$TMP/self.size")"
report self.data "$(awk '{print $4}' "$TMP/self.size")"
report self.rodata "$(awk '{print $6}' "$TMP/self.size")"

if ! "$FICC" "$SOBJ"/*.o "$SOBJ"/util/*.o "$SOBJ"/optpasses/*.o -o "$TMP/self.bin"; then
    echo "self: link failed" >&2
    exit 1
fi
report self.bin "$(stat -c %s "$TMP/self.bin")"

# --- lua (optional) ----------------------------------------------------------------
if [ -n "${LUA_DIR:-}" ] && [ -f "$LUA_DIR/lua.c" ]; then
    LOBJ="$TMP/lua"
    mkdir -p "$LOBJ"
    lua_ok=1
    for f in "$LUA_DIR"/*.c; do
        base=$(basename "$f")
        timeout 120 "$FICC" -O2 -DLUA_USE_LINUX -I"$LUA_DIR" -c "$f" -o "$LOBJ/${base%.c}.o" || {
            echo "lua: compile failed on $base" >&2
            lua_ok=0
            break
        }
    done
    if [ "$lua_ok" = 1 ]; then
        timeout 120 "$FICC" -O2 -DLUA_USE_LINUX "$LOBJ"/*.o -o "$LOBJ/lua" -lm -ldl || {
            echo "lua: link failed" >&2
            lua_ok=0
        }
    fi
    if [ "$lua_ok" = 1 ]; then
        "$ELFSIZE" "$LOBJ"/*.o >"$TMP/lua.size" || exit 1
        report lua.text "$(awk '{print $2}' "$TMP/lua.size")"
        CPU=$( (cd "$LUA_DIR/testes" && timeout 600 /usr/bin/time -f %U "$LOBJ/lua" -W all.lua >/dev/null) 2>&1 )
        report lua.cpu "$CPU"
    fi
fi

# --- baseline comparison -----------------------------------------------------------
if [ -f "$BASELINE" ]; then
    echo "--- delta vs baseline ---"
    while read -r key val; do
        [ -n "$key" ] || continue
        case $key in \#*) continue ;; esac
        now=$(awk -v k="$key" '$1 == k {print $2}' "$TMP/report")
        [ -n "$now" ] || continue
        echo "$key: $val -> $now"
    done <"$BASELINE"
fi
