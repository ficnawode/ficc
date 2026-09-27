#!/usr/bin/env bash
# Usage: test/debug_gdb.sh <path-to-ficc-binary>

set -eu

FICC_BIN=${1:-}
if [ -z "$FICC_BIN" ] || [ ! -x "$FICC_BIN" ]; then
    echo "test-gdb: usage: test/debug_gdb.sh <ficc-binary>" >&2
    exit 2
fi

for tool in gdb readelf; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-gdb: SKIP — '$tool' not installed; the real-debugger certificate is optional" >&2
        exit 0
    fi
done

WORK=$(mktemp -d /tmp/ficc_gdb.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/cert.c"
OBJ="$WORK/cert.o"
BIN="$WORK/cert"

cat > "$SRC" <<'EOF'
enum Color { RED = 1, GREEN = 2, BLUE = 3, TOTAL = 4 };

struct Flags
{
    unsigned a : 3;
    unsigned b : 5;
};

struct Node
{
    int value;
    struct Node *next;
    struct Flags flags;
    int payload[4];
};

union Tagged
{
    int i;
    float f;
};

static int static_shelf = 7;
int g_counter = 41;
const int const_cap = 3;
int (*g_op)(int);

int nodal(int x)             // line 28
{
    return x * 2;
}

int drawn(struct Node *n, enum Color c, struct Flags *fl, const int *cap,
          union Tagged *u)   // line 35
{
    n->value = 10;
    fl->a = 3;
    fl->b = 5;
    u->i = 42;
    return n->value + *cap + static_shelf + g_counter +
           (c == GREEN ? 0 : 100) + nodal(u->i) - 84;
}

int main(void)
{
    struct Node node = {0};
    struct Flags flags = {0};
    union Tagged u = {0};
    enum Color col = GREEN;
    g_op = nodal;
    return drawn(&node, col, &flags, &const_cap, &u) == 61;
}
EOF

fail()
{
    echo "test-gdb: FAIL — $1" >&2
    exit 1
}

"$FICC_BIN" -g -c "$SRC" -o "$OBJ" >/dev/null 2>&1 || fail "ficc -g -c"

readelf -SW "$OBJ" | grep -q '\.debug_line' || fail "no .debug_line section"
readelf -SW "$OBJ" | grep -q '\.eh_frame' || fail "no .eh_frame section"
if readelf --debug-dump=info "$OBJ" 2>&1 | grep -q 'Warning'; then
    fail "readelf reports a DWARF warning"
fi
LINES=$(readelf --debug-dump=decodedline "$OBJ" | awk '$2 ~ /^[0-9]+$/ { print $2 }' | tr '\n' ' ')
if [ "$LINES" != "30 36 37 38 39 40 41 46 47 48 50 51 " ]; then
    echo "test-gdb: FAIL — decoded lines '$LINES', want '30 36 37 38 39 40 41 46 47 48 50 51'" >&2
    exit 1
fi

"$FICC_BIN" -nostdlib "$OBJ" -o "$BIN" || fail "filc link"

if ! gdb -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' \
    -ex 'break cert.c:36' -ex run -ex next -ex next -ex next \
    -ex 'print n->value' -ex 'print c' -ex 'print fl->a' -ex 'print fl->b' \
    -ex 'print *fl' -ex 'print *n' -ex 'print n->next' -ex 'print n->payload' \
    -ex 'print static_shelf' -ex 'print g_counter' -ex 'print const_cap' \
    -ex 'print g_op' -ex 'info locals' -ex bt \
    -ex quit --args "$BIN" \
    | awk '
        /^\$1 = 10$/ {v1=1}
        /^#0  drawn / && /:39$/ {home=1}
        /^\$2 = 2$/ {v2=1}
        /^\$3 = 3$/ {v3=1}
        /^\$4 = 5$/ {v4=1}
        /^\$5 = \{a = 3, b = 5\}$/ {v5=1}
        /^\$6 = \{value = 10, next = 0x0, flags = \{a = 0, b = 0\}, payload = \{0, 0, 0, 0\}\}$/ {v6=1}
        /^\$7 = \(struct Node \*\) 0x0$/ {v7=1}
        /^\$8 = \{0, 0, 0, 0\}$/ {v8=1}
        /^\$9 = 7$/ {v9=1}
        /^\$10 = 41$/ {v10=1}
        /^\$11 = 3$/ {v11=1}
        /^\$12 = \(int \(\*\)\(int\)\) 0x[0-9a-f]+ <nodal>$/ {v12=1}
        /^No locals\.$/ {nl=1}
        /^#0 / && /drawn/ {f0=1}
        /^#1 / && /main/ {f1=1}
        END { exit !(v1 && v2 && v3 && v4 && v5 && v6 && v7 && v8 && v9 && v10 && v11 && v12 && nl && home && f0 && f1) }
    '; then
    fail "gdb rich-type values/backtrace assertions"
fi

if ! gdb -batch -ex 'set pagination off' -ex 'break cert.c:30' -ex run -ex bt -ex quit \
    --args "$BIN" \
    | awk '/^#0 / && /nodal/ {n=1} /^#1 / && /drawn/ {d=1} /^#2 / && /main/ {m=1} END { exit !(n && d && m) }'; then
    fail "three-frame backtrace via .eh_frame"
fi

echo "test-gdb: OK — real-debugger certificate passed"