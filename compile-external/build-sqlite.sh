#!/usr/bin/env bash
# build-sqlite.sh — compile SQLite with ficc and run its TCL test suite.
#
# Everything ficc can compile, it does: the generated amalgamation (sqlite3.c)
# *and* SQLite's TCL test harness (testfixture) are built by ficc, and ficc's
# own linker produces the executable.  The harness is
# compiled by setting CC (not T.cc), because T.cc is a command-line override
# that would suppress the Makefile's `T.cc += $(OPT_FEATURE_FLAGS)` (math, fts5,
# ...); CC feeds `T.cc = $(CC)` and the appends still apply.
#
# A host-cc testfixture is kept as testfixture.gcc: some tests fail on this
# platform regardless of compiler (see sqlite-baseline.txt), and the harness
# requires ficc's failures to be a subset of the baseline's.
#
# Test selection: the default is veryquick.test (a few minutes); set
# EXT_SQLITE_TEST=quicktest (or devtest) for the full developer suite
# (`testrunner.tcl mdevtest`), or to a .test filename, e.g. select1.test.
#
# The amalgamation needs several gigabytes; the build runs in its own cgroup
# (see heavy() in common.sh).  Tune with EXT_MEM_MAX / EXT_SWAP_MAX.
set -euo pipefail

PROJECT=sqlite
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=common.sh
. "$HERE/common.sh"
require_ficc

src=$(project_src sqlite)
out=$(project_build sqlite)
wrapper="$HERE/wrap/sqlite-cc.sh"
chmod +x "$wrapper"
export FICC FICC_INCLUDE

# --- configure and generate the amalgamation --------------------------------------
if [ ! -f "$out/Makefile" ]; then
    step "configure $PROJECT (host tools)"
    ( cd "$out" && "$src/configure" --dev ) >"$out/configure.log" 2>&1 \
        || die "configure failed (see $out/configure.log)"
fi

step "generate sqlite3.c amalgamation"
make -C "$out" -j"$JOBS" sqlite3.c >"$out/amalgamation.log" 2>&1 \
    || die "amalgamation generation failed (see $out/amalgamation.log)"

# --- host-cc reference build ------------------------------------------------------
step "build baseline testfixture (host cc)"
rm -f "$out/testfixture"
make -C "$out" -j"$JOBS" testfixture USE_AMALGAMATION=1 \
    >"$out/testfixture-cc.log" 2>&1 || die "baseline testfixture build failed"
cp "$out/testfixture" "$out/testfixture.gcc"

# --- ficc build -------------------------------------------------------------------
step "build testfixture with $FICC"
rm -f "$out/testfixture"
( cd "$out" && heavy make -j"$JOBS" testfixture USE_AMALGAMATION=1 CC="$wrapper" ) \
    >"$out/testfixture-ficc.log" 2>&1 \
    || die "ficc testfixture build failed (see $out/testfixture-ficc.log)"
[ -x "$out/testfixture" ] || die "testfixture was not produced"

# --- run the test suite -----------------------------------------------------------
# `testfixture` is the ficc build, `testfixture.gcc` the host-cc reference.  Some
# SQLite tests fail on this platform no matter which compiler built the library;
# those are listed in sqlite-baseline.txt and the harness requires ficc's
# failures to be a subset.  For the developer suite, testrunner does not name
# the failing cases, so the error count is compared against the baseline.
tclsh=$(command -v tclsh || command -v tclsh8.6 || true)
baseline="$HERE/sqlite-baseline.txt"
suite=${EXT_SQLITE_TEST:-veryquick.test}

known_fails() {
    [ -f "$baseline" ] || return 0
    sed -e 's/#.*//' "$baseline" | tr -s ' \t' '\n' | grep -v '^$' | tr '\n' ' '
}

fail_names() {
    grep -h '!Failures on these tests:' "$1" 2>/dev/null \
        | sed -e 's/.*!Failures on these tests://' | tr -s ' \t' '\n' | grep -v '^$'
}

case "$suite" in
    quicktest | devtest)
        [ -n "$tclsh" ] || die "tclsh not found"
        step "test: testrunner.tcl mdevtest (ficc testfixture)"
        ( cd "$out" && "$tclsh" "$src/test/testrunner.tcl" mdevtest ) \
            >"$out/quicktest.ficc.log" 2>&1
        ficc_err=$(awk '/errors out of/{e=$1} END{print e+0}' "$out/quicktest.ficc.log")
        step "test: testrunner.tcl mdevtest (host-cc baseline)"
        ( cd "$out" && ./testfixture.gcc "$src/test/testrunner.tcl" mdevtest ) \
            >"$out/quicktest.gcc.log" 2>&1
        gcc_err=$(awk '/errors out of/{e=$1} END{print e+0}' "$out/quicktest.gcc.log")
        say "quicktest errors: ficc=$ficc_err host-cc=$gcc_err"
        [ "$ficc_err" -le "$gcc_err" ] \
            || die "ficc quicktest has more errors than the host-cc baseline (see $out/quicktest.*.log)"
        ;;
    *)
        file=$suite
        case "$file" in *.test) ;; *) file="$file.test" ;; esac
        [ -f "$src/test/$file" ] || die "no such test: $src/test/$file"
        step "test: $file"
        log="$out/${file%.test}.log"
        ( cd "$out" && ./testfixture "$src/test/$file" ) >"$log" 2>&1 || true
        known=$(known_fails)
        unexpected=
        while IFS= read -r name; do
            [ -n "$name" ] || continue
            case " $known " in *" $name "*) ;; *) unexpected="$unexpected $name" ;; esac
        done < <(fail_names "$log")
        if [ -n "$unexpected" ]; then
            die "sqlite $file: ficc-specific failures:$unexpected (log: $log)"
        fi
        say "failures are all known host-cc baseline failures"
        ;;
esac

say "PASS: sqlite ($suite)"
