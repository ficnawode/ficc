#!/usr/bin/env bash
# build-git.sh — compile Git with ficc and run Git's own test suite.
#
# Git is Godmother #3 and the first hosted target (Phase 23): glibc supplies
# POSIX through the compiler wrapper's include model, while ficc links the
# binary with its in-tree linker.  Both halves of the suite run: the `t/`
# shell tests and the clar unit tests.
#
# The full `t/` suite is long (~1h serial, a few minutes with -j); control the
# parallelism with JOBS=n.
set -euo pipefail

PROJECT=git
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=common.sh
. "$HERE/common.sh"
require_ficc

src=$(project_src git)
out=$(project_build git)
wrapper="$HERE/wrap/git-cc.sh"
chmod +x "$wrapper"

# Git's Makefile probes and drives a gcc-style $(CC); the wrapper translates.
# Feature switches keep the dependency surface small, and HAVE_ALLOCA_H is
# cleared because ficc has no <alloca.h> (xalloca falls back to xmalloc).
GITMAKE=(
    "CC=$wrapper"
    "CFLAGS=-O1"
    NO_RUST=1 NO_OPENSSL=1 NO_CURL=1 NO_EXPAT=1 NO_GETTEXT=1 NO_ICONV=1
    NO_SYSLOG=1 NO_PERL=1 NO_PYTHON=1 NO_TCLTK=1
    HAVE_ALLOCA_H=
)
export FICC FICC_INCLUDE

step "build git with $FICC"
make -C "$src" -j"$JOBS" "${GITMAKE[@]}" all || die "git build failed"
[ -x "$src/git" ] || die "git binary was not produced"

step "test: unit-tests"
make -C "$src" -j"$JOBS" "${GITMAKE[@]}" unit-tests || die "git unit-tests failed"

step "test: t/ shell suite"
# The default is the whole suite; EXT_GIT_T_GLOB narrows it for a quick smoke
# run (e.g. EXT_GIT_T_GLOB='t00*.sh').
t_glob=${EXT_GIT_T_GLOB:-t[0-9][0-9][0-9][0-9]-*.sh}
logdir="$out/tlogs"
rm -rf "$logdir"
mkdir -p "$logdir"

run_one() {
    local s="$1"
    timeout 1800 "./$s" >"$logdir/${s%.sh}.log" 2>&1
    printf '%s %s\n' "$?" "$s"
}
export -f run_one
export logdir

( cd "$src/t" && compgen -G "$t_glob" | sort >"$logdir/list" )
( cd "$src/t" && xargs -d '\n' -P "$JOBS" -n1 bash -c 'run_one "$@"' _ \
      <"$logdir/list" >"$logdir/results" 2>&1 )

total=$(wc -l <"$logdir/results")
pass=$(awk '$1 == 0' "$logdir/results" | wc -l)
say "t/: $pass/$total scripts passed"
if [ "$pass" != "$total" ]; then
    warn "failing scripts:"
    awk '$1 != 0 {print "  " $1 " " $2}' "$logdir/results"
    die "git t/ suite failed (logs in $logdir)"
fi

say "PASS: git (unit-tests + $pass/$total t/ scripts)"
