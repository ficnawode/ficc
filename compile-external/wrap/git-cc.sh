#!/usr/bin/env bash
#
# git-cc.sh - compiler wrapper that lets Git's Makefile drive a ficc build.
#
# Git invokes $(CC) in gcc style with -c/-o and a pile of gcc-only flags.  This
# wrapper forwards a compile to ficc (using the hosted include model) while
# dropping the flags ficc's CLI does not know, and delegates any link step to
# ficc's own linker.  Set FICC_LINK_GCC=1 to link with the host gcc -no-pie
# instead (ficc objects carry R_X86_64_32S data relocations).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
FICC=${FICC:-$root/build/bin/ficc}
FICC_INCLUDE=${FICC_INCLUDE:-$root/include}
GITDIR=${GITDIR:-$PWD}

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
        -W* | -f* | -std=* | -pipe | -pthread | -m*)
            ;;
        -O* | -g | -D* | -U* | -I* | -isystem | -include)
            args+=("$a")
            ;;
        *)
            args+=("$a")
            ;;
    esac
    i=$((i + 1))
done

if [ "$compile" = 1 ] || [ "$preprocess" = 1 ]; then
    exec "$FICC" -nostdinc \
        -isystem /usr/include -isystem /usr/include/x86_64-linux-gnu \
        -isystem "$FICC_INCLUDE" -I"$GITDIR" "${args[@]}"
fi
if [ "${FICC_LINK_GCC:-0}" = 1 ]; then
    exec gcc -no-pie "${args[@]}"
fi
exec "$FICC" "${args[@]}"
