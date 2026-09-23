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

mapfile -t files < <(find "$GIT" -name '*.c' -not -path '*/.git/*' | sort)
total="${#files[@]}"
echo "git-sweep: compiling $total translation units with $FICC"
printf '%s\0' "${files[@]}" |
    xargs -0 -P "$JOBS" -n1 bash -c 'sweep_one "$@"' _ >"$TMP/results"

pass="$(grep -c '^PASS$' "$TMP/results" || true)"
fail="$((total - pass))"
echo "git-sweep: PASS $pass/$total  FAIL $fail"
echo "top failure buckets:"
grep '^FAIL ' "$TMP/results" | sed 's/^FAIL //' | sort | uniq -c | sort -rn | head -20
