#!/usr/bin/env bash
# Static-checkpoint certificate (`make selftest-filc-static`): link the
# freestanding golden corpus with ficc's own linker and run each binary,
# asserting interp == filc. Also links one program twice and requires
# byte-identical output (determinism).
#
# Usage: test/filc_static.sh <path-to-ficc-binary>

set -eu

FICC_BIN=${1:-}
if [ -z "$FICC_BIN" ] || [ ! -x "$FICC_BIN" ]; then
    echo "selftest-filc-static: usage: test/filc_static.sh <ficc-binary>" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/ficc_static.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
for src in "$ROOT"/test/golden/*.c; do
    exp=$("$FICC_BIN" -run "$src" 2>/dev/null | sed -n 's/^interp: //p')
    if [ -z "$exp" ]; then
        echo "selftest-filc-static: no interpreter result for $(basename "$src")" >&2
        fail=$((fail + 1))
        continue
    fi
    exp=$((exp & 255))
    bin="$WORK/$(basename "$src" .c)"
    if ! "$FICC_BIN" "$src" -o "$bin" >/dev/null 2>&1; then
        echo "selftest-filc-static: link failed for $(basename "$src")" >&2
        fail=$((fail + 1))
        continue
    fi
    "$bin" && act=0 || act=$?
    if [ "$act" != "$exp" ]; then
        echo "selftest-filc-static: $(basename "$src"): interp=$exp filc=$act" >&2
        fail=$((fail + 1))
        continue
    fi
    pass=$((pass + 1))
done

det="$ROOT/test/golden/smoke_return42.c"
"$FICC_BIN" "$det" -o "$WORK/det1" >/dev/null 2>&1
"$FICC_BIN" "$det" -o "$WORK/det2" >/dev/null 2>&1
if ! cmp -s "$WORK/det1" "$WORK/det2"; then
    echo "selftest-filc-static: output is not deterministic" >&2
    exit 1
fi

if [ "$fail" -ne 0 ]; then
    echo "selftest-filc-static: FAIL — $pass passed, $fail failed" >&2
    exit 1
fi
echo "selftest-filc-static: OK — $pass golden programs linked and ran; output deterministic"
