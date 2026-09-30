#!/usr/bin/env bash
#
# sqlite-cc.sh - gcc-style compiler wrapper that lets SQLite's Makefile drive a
# ficc build (mirrors wrap/git-cc.sh).
#
# ficc's CLI knows only its own option table, so the wrapper drops the
# gcc-only flags (warnings, -f/-m/-std, -pthread, dependency files) and injects
# the hosted include model.  _LARGEFILE64_SOURCE is defined because ficc's
# default feature set does not expose glibc's off64_t, which os_unix.c uses.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
FICC=${FICC:-$root/build/bin/ficc}
FICC_INCLUDE=${FICC_INCLUDE:-$root/include}

compile=0
preprocess=0
args=()
i=1
while [ "$i" -le "$#" ]; do
    a="${!i}"
    case "$a" in
        -c)
            compile=1
            args+=("$a")
            ;;
        -E | -P | -tokens | -ast | -ir)
            preprocess=1
            args+=("$a")
            ;;
        -o)
            i=$((i + 1))
            args+=(-o "${!i}")
            ;;
        -MF | -MQ | -MT)
            i=$((i + 1))
            ;;
        -MMD | -MP | -MD)
            ;;
        # -g is dropped: DWARF for the amalgamation is what pushes ficc past
        # 10 GB and OOM-kills the session, and it does not affect the tests.
        -W* | -f* | -std=* | -pipe | -pthread | -m* | -g)
            ;;
        -O* | -D* | -U* | -I* | -isystem | -include)
            args+=("$a")
            ;;
        *)
            args+=("$a")
            ;;
    esac
    i=$((i + 1))
done

# testfixture's rule compiles and links in a single invocation, so the hosted
# include model and _LARGEFILE64_SOURCE are needed for link commands too (they
# are ignored when the input is only pre-built objects).
#
# tcl.h picks Tcl_WideInt from its __GNUC__ branch (long long) on this platform;
# ficc deliberately does not define __GNUC__, so tcl.h's fallback guesses "long"
# and then mismatches sqlite3_int64 (long long).  Pin the type to what gcc
# effectively uses.
HOSTED=(-nostdinc -isystem /usr/include -isystem /usr/include/x86_64-linux-gnu
        -isystem "$FICC_INCLUDE" -D_LARGEFILE64_SOURCE=1
        '-DTCL_WIDE_INT_TYPE=long long')

if [ "$compile" = 1 ] || [ "$preprocess" = 1 ]; then
    exec "$FICC" "${HOSTED[@]}" "${args[@]}"
fi
if [ "${FICC_LINK_GCC:-0}" = 1 ]; then
    exec gcc -no-pie "${args[@]}"
fi
exec "$FICC" "${HOSTED[@]}" "${args[@]}"
