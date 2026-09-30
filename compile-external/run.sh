#!/usr/bin/env bash
# run.sh [project...] — fetch, build, and test the godmothers.
#
# With no arguments it runs all five (lua zlib libpng git sqlite).  A project's
# build script exits non-zero if its test suite fails; the summary at the end
# reports every result and run.sh exits non-zero if any failed.
#
#   compile-external/run.sh                 # everything
#   compile-external/run.sh lua zlib        # a subset
#   EXT_NO_FETCH=1 run.sh git               # skip the clone/checkout step
#
# The compiler defaults to build/bin/ficc; override with FICC=/path/to/ficc.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
# shellcheck source=revisions.sh
. "$HERE/revisions.sh"

[ $# -gt 0 ] || set -- $PROJECTS
for project in "$@"; do
    is_project "$project" || die "unknown project: $project"
done

if [ "${EXT_NO_FETCH:-0}" != 1 ]; then
    "$HERE/fetch.sh" "$@"
fi

results=()
failed=0
for project in "$@"; do
    script="$HERE/build-$project.sh"
    [ -x "$script" ] || { warn "no build script for $project"; results+=("$project SKIP"); continue; }
    if "$script"; then
        results+=("$project PASS")
    else
        results+=("$project FAIL")
        failed=1
    fi
done

printf '\n=== compile-external summary ===\n'
printf '%s\n' "${results[@]}"
exit "$failed"
