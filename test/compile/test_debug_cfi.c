#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int cfi_seq;

static void cfi_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20a_%u_%s.%s", cfi_seq++, tag, ext);
}

static void cfi_write_src(char *out, size_t sz, const char *src)
{
    cfi_path(out, sz, "dbg", "c");
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void cfi_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

static char cfi_src[] =
    "static int mul(int a, int b) { return a * b; }\n"
    "int add3(int x, int y, int z) { int t = x + y; return t + z; }\n"
    "int main(void) { int q = add3(10, 20, 12); return q + mul(q, 0) == 42; }\n";

TEST(debug_cfi, eh_frame_present_and_parseable)
{
    char src[256], obj[256];
    cfi_write_src(src, sizeof(src), cfi_src);
    cfi_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Both sections present; readelf prints one FDE row per function. */
    snprintf(cmd, sizeof(cmd), "readelf -SW %s | grep -q '\\.eh_frame'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf -SW %s | grep -q '\\.rela\\.eh_frame'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd),
             "test \"$(readelf --debug-dump=frames %s | grep -c 'FDE cie=')\" -eq 3", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj};
    cfi_cleanup(paths, 2);
}

TEST(debug_cfi, gdb_bt_through_caller_callee)
{
    char src[256], obj[256], bin[256];
    cfi_write_src(src, sizeof(src), cfi_src);
    cfi_path(obj, sizeof(obj), "dbg", "o");
    cfi_path(bin, sizeof(bin), "dbg", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "gcc -no-pie -g %s -o %s >/dev/null 2>&1", obj, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* The backtrace must list the callee as frame #0 and main as frame #1. */
    snprintf(cmd, sizeof(cmd),
             "gdb -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' "
             "-ex 'break add3' -ex run -ex bt -ex quit %s "
             "| awk '/^#0 / && /add3/ {a=1} /^#1 / && /main/ {b=1} END {exit !(a && b)}'",
             bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj, bin};
    cfi_cleanup(paths, 3);
}