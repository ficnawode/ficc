#!/usr/bin/env bash
#
# git-cc.sh - compiler wrapper that lets Git's Makefile drive a ficc build.
#
# Git invokes $(CC) in gcc style with -c/-o and a pile of gcc-only flags. This
# wrapper forwards a compile to ficc (adding the hosted include model and
# dropping flags ficc does not understand) and delegates any link step to the
# host gcc with -no-pie (ficc objects carry R_X86_64_32S data relocations).

set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FICC="${FICC:-$root/build/bin/ficc}"
FICC_INCLUDE="${FICC_INCLUDE:-$root/include}"
GITDIR="${GITDIR:-$PWD}"

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
        -W* | -f* | -std=* | -pipe | -pthread | -m* | -mno-*)
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
exec gcc -no-pie "${args[@]}"
