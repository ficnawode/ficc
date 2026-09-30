#!/usr/bin/env bash
# Pinned upstream revisions for the five godmothers.
#
# Every project is pinned to an exact upstream commit or tag so a run is
# reproducible.  `extern_url <name>` and `extern_rev <name>` are the only
# lookups fetch.sh and the build scripts need.

PROJECTS="lua zlib libpng git sqlite"

extern_url() {
    case "$1" in
        lua) printf '%s\n' "https://github.com/lua/lua.git" ;;
        zlib) printf '%s\n' "https://github.com/madler/zlib.git" ;;
        libpng) printf '%s\n' "https://github.com/pnggroup/libpng.git" ;;
        git) printf '%s\n' "https://github.com/git/git.git" ;;
        sqlite) printf '%s\n' "https://github.com/sqlite/sqlite.git" ;;
        *) return 1 ;;
    esac
}

extern_rev() {
    case "$1" in
        lua) printf '%s\n' "v5.5.1" ;;
        zlib) printf '%s\n' "767c4c947852e143f582c85f14cf573411df1b35" ;;
        libpng) printf '%s\n' "964b4135949703b705fc760fc3fb546b86e5ab47" ;;
        git) printf '%s\n' "v2.56.0-rc2" ;;
        sqlite) printf '%s\n' "2acb2ea9089c604d5786f56ee3e50a0479a268dd" ;;
        *) return 1 ;;
    esac
}

is_project() {
    local p
    for p in $PROJECTS; do
        [ "$p" = "$1" ] && return 0
    done
    return 1
}
