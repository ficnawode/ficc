#!/usr/bin/env bash
# Shared helpers for the compile-external harness.
#
# Each build-<project>.sh sources this file, then compiles the pinned upstream
# tree in third_party/<project> with ficc and runs that project's own test
# suite.  Nothing here touches the ficc source tree; all work lands under
# compile-external/build/.

set -u

EXTERNAL_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$EXTERNAL_ROOT/.." && pwd)

# The compiler under test.  By default the freshly built binary in this repo;
# override with FICC=/path/to/ficc.
FICC=${FICC:-$REPO_ROOT/build/bin/ficc}
FICC_INCLUDE=${FICC_INCLUDE:-$REPO_ROOT/include}

# Where fetch.sh puts the pinned upstream checkouts.
SRC_DIR=${EXT_SRC_DIR:-$EXTERNAL_ROOT/third_party}
# Where each project is built and tested.
BUILD_ROOT=${EXT_BUILD_DIR:-$EXTERNAL_ROOT/build}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 1)}

say() { printf '>>> [%s] %s\n' "${PROJECT:-external}" "$*"; }
step() { printf '\n>>> [%s] == %s ==\n' "${PROJECT:-external}" "$*"; }
warn() { printf '!!! [%s] %s\n' "${PROJECT:-external}" "$*" >&2; }
die() { printf '!!! [%s] %s\n' "${PROJECT:-external}" "$*" >&2; exit 1; }

require_ficc() {
    [ -x "$FICC" ] || die "ficc not found at $FICC (run 'make', or set FICC=/path/to/ficc)"
}

# Run a memory-hungry command in its own cgroup.  SQLite's amalgamation needs
# several gigabytes, and an unbounded runaway OOM-kills the whole desktop
# session; a scope confines the damage and lets the harness choose the cap.
# The limits are tunable with EXT_MEM_MAX / EXT_SWAP_MAX.
heavy() {
    local mem=${EXT_MEM_MAX:-10G} swap=${EXT_SWAP_MAX:-8G}
    if command -v systemd-run >/dev/null 2>&1; then
        systemd-run --user --scope --quiet -p "MemoryMax=$mem" -p "MemorySwapMax=$swap" "$@"
    else
        "$@"
    fi
}

project_src() {
    local dir="$SRC_DIR/$1"
    [ -d "$dir" ] || die "missing $dir; run compile-external/fetch.sh $1 first"
    printf '%s\n' "$dir"
}

project_build() {
    local dir="$BUILD_ROOT/$1"
    mkdir -p "$dir"
    printf '%s\n' "$dir"
}

# zlib's fifteen library translation units, shared by build-zlib (which tests
# zlib itself) and build-libpng (which links libpng against it).
ZLIB_SRCS="adler32 crc32 deflate infback inffast inflate inftrees trees zutil \
compress uncompr gzclose gzlib gzread gzwrite"

# build_zlib_objects <zlib-src> <obj-dir> — compile the library and print the
# object paths, one per line.
build_zlib_objects() {
    local zsrc="$1" out="$2" f objs=()
    mkdir -p "$out"
    for f in $ZLIB_SRCS; do
        "$FICC" -O2 -I"$zsrc" -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN \
            -DHAVE_UNISTD_H=1 -DHAVE_STDARG_H=1 \
            -c "$zsrc/$f.c" -o "$out/$f.o" || die "zlib compile failed on $f.c"
        objs+=("$out/$f.o")
    done
    printf '%s\n' "${objs[@]}"
}
