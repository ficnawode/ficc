#!/usr/bin/env bash
# fetch.sh [project...] — populate third_party/<project> at its pinned revision.
#
# For each project: clone from upstream if absent, fetch tags, check out the
# pinned tag/commit, reset the tree pristine, then apply every patch in
# compile-external/patches/<project>/*.patch.  With no arguments it fetches all
# five.  Override the checkout root with EXT_SRC_DIR=... .
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
# shellcheck source=revisions.sh
. "$HERE/revisions.sh"

export GIT_TERMINAL_PROMPT=0

[ $# -gt 0 ] || set -- $PROJECTS

mkdir -p "$SRC_DIR"

for project in "$@"; do
    is_project "$project" || die "unknown project: $project"
    url=$(extern_url "$project")
    rev=$(extern_rev "$project")
    dir="$SRC_DIR/$project"

    step "fetch $project @ $rev"
    if [ ! -d "$dir/.git" ]; then
        say "clone $url"
        git clone --quiet "$url" "$dir" \
            || die "clone failed (offline? try EXT_SRC_DIR=<existing checkout root>)"
    fi
    git -C "$dir" fetch --quiet --tags origin || warn "fetch failed; using local objects"
    git -C "$dir" checkout --quiet --force "$rev" \
        || die "$project: cannot check out $rev"
    git -C "$dir" reset --quiet --hard "$rev"
    git -C "$dir" clean --quiet -fd

    for patch in "$HERE/patches/$project"/*.patch; do
        [ -e "$patch" ] || continue
        say "apply $(basename "$patch")"
        git -C "$dir" apply --whitespace=nowarn "$patch" \
            || die "$project: failed to apply $(basename "$patch")"
    done
    say "$project ready at $(git -C "$dir" rev-parse --short HEAD)"
done
