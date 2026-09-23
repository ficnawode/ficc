#!/usr/bin/env bash
#
# git-smoke.sh - run a few Git commands in a throwaway repo against a ficc-built
# `git` binary, proving it is functional rather than merely linked.

set -eu

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GIT="${GIT:-$root/../git}"
GITBIN="${GITBIN:-$GIT/git}"

if [ ! -x "$GITBIN" ]; then
    echo "git-smoke: git binary not found: $GITBIN (build Git with ficc first)" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export HOME="$tmp"
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=smoke GIT_AUTHOR_EMAIL=smoke@example.com
export GIT_COMMITTER_NAME=smoke GIT_COMMITTER_EMAIL=smoke@example.com

cd "$tmp"
"$GITBIN" init -q
printf 'hello\n' > a.txt
"$GITBIN" add a.txt
"$GITBIN" commit -q -m first
"$GITBIN" log --oneline | grep -q first
"$GITBIN" status --porcelain >/dev/null
"$GITBIN" rev-parse HEAD >/dev/null
"$GITBIN" hash-object a.txt >/dev/null
"$GITBIN" cat-file -p HEAD >/dev/null
"$GITBIN" ls-tree HEAD >/dev/null
"$GITBIN" --version >/dev/null

echo "git-smoke: OK"
