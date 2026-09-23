#!/usr/bin/env bash
#
# git-sweep.sh - the Phase 23 progress meter.
#
# Compiles every Git translation unit with ficc using the pinned hosted flag
# set, buckets the first diagnostic of each failure by normalized message, and
# prints a pass/fail count plus the top buckets. Every later sub-phase must
# move the universal bucket and shrink the failure count.
#
# Overridable: FICC, GIT, FICC_INCLUDE, JOBS.

set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FICC="${FICC:-$root/build/bin/ficc}"
GIT="${GIT:-$(cd "$root/../git" && pwd)}"
FICC_INCLUDE="${FICC_INCLUDE:-$root/include}"
JOBS="${JOBS:-$(nproc)}"
DEFS="-DNO_OPENSSL -DNO_CURL -DNO_EXPAT -DNO_GETTEXT -DNO_ICONV -DNO_PTHREADS -DNO_SYSLOG"
# Makefile-supplied path/CPU defines (normally injected per-object by Git's
# Makefile; pinned here so the sweep is self-contained).
DEFS="$DEFS -DGIT_HOST_CPU=\"x86_64\""
DEFS="$DEFS -DGIT_HTML_PATH=\"share/doc/git-doc\" -DGIT_MAN_PATH=\"share/man\""
DEFS="$DEFS -DGIT_INFO_PATH=\"share/info\" -DGIT_EXEC_PATH=\"libexec/git-core\""
DEFS="$DEFS -DGIT_LOCALE_PATH=\"share/locale\""

if [ ! -x "$FICC" ]; then
    echo "git-sweep: ficc binary not found: $FICC (run 'make')" >&2
    exit 1
fi
if [ ! -d "$GIT" ]; then
    echo "git-sweep: Git source not found: $GIT" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

sweep_one() {
    local f="$1" err out msg
    err="$TMP/err.$$.$RANDOM"
    out="$TMP/o.$$.$RANDOM.o"
    if "$FICC" -nostdinc \
        -isystem /usr/include -isystem /usr/include/x86_64-linux-gnu \
        -isystem "$FICC_INCLUDE" -I"$GIT" \
        $DEFS -O0 -c "$f" -o "$out" >/dev/null 2>"$err"; then
        echo "PASS"
    else
        msg="$(grep -m1 'error:' "$err" | sed -E "s/^[^ ]+:[0-9]+:[0-9]+: //; s/'[^']*'/'<x>'/g")"
        [ -n "$msg" ] || msg="(no error line)"
        echo "FAIL $msg"
    fi
    rm -f "$err" "$out"
}
export -f sweep_one
export FICC FICC_INCLUDE GIT TMP DEFS

# Only the translation units Git's Linux build compiles: exclude the
# Windows/Darwin ports, contrib/, and test-fixture / vendored-clar sources.
mapfile -t files < <(find "$GIT" -name '*.c' -not -path '*/.git/*' \
    -not -path '*/compat/win32/*' -not -path '*/compat/darwin/*' \
    -not -path '*/compat/mingw.c' -not -path '*/compat/msvc.c' \
    -not -path '*/compat/winansi.c' -not -path '*/compat/fsmonitor/fsm-listen-darwin.c' \
    -not -path '*/compat/simple-ipc/ipc-win32.c' -not -path '*/compat/regex/*' \
    -not -path '*/contrib/*' -not -path '*/t/unit-tests/clar/*' \
    -not -path '*/t/t4051/*' -not -path '*/t/t4256/*' | sort)
total="${#files[@]}"
echo "git-sweep: compiling $total translation units with $FICC"
printf '%s\0' "${files[@]}" |
    xargs -0 -P "$JOBS" -n1 bash -c 'sweep_one "$@"' _ >"$TMP/results"

pass="$(grep -c '^PASS$' "$TMP/results" || true)"
fail="$((total - pass))"
echo "git-sweep: PASS $pass/$total  FAIL $fail"
echo "top failure buckets:"
grep '^FAIL ' "$TMP/results" | sed 's/^FAIL //' | sort | uniq -c | sort -rn | head -20
