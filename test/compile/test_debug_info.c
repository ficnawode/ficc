#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int di_seq;

static void di_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20c_%u_%s.%s", di_seq++, tag, ext);
}

static void di_write_src(char *out, size_t sz, const char *src)
{
    di_path(out, sz, "dbg", "c");
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void di_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

static char di_src[] = "static int shelf = 100;\n"
                       "extern int imported;\n"
                       "int counter = 5;\n"
                       "int add3(int x, int y, int z)\n"
                       "{\n"
                       "    int t = x + y;\n"
                       "    return t + z + shelf;\n"
                       "}\n"
                       "static int helper(int v)\n"
                       "{\n"
                       "    return v * 2;\n"
                       "}\n"
                       "int main(void)\n"
                       "{\n"
                       "    return add3(10, 20, 12) + helper(0) == 142;\n"
                       "}\n";

TEST(debug_info, program_surface_dies)
{
    char src[256], obj[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_TAG_compile_unit'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_TAG_subprogram'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_TAG_formal_parameter'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_TAG_variable'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_TAG_base_type'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : add3'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : x'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : shelf'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_OP_call_frame_cfa'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_OP_fbreg'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_OP_addr'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : helper'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : imported'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* The extern (no DW_AT_location) and every DIE parse without warnings. */
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s 2>&1 | grep -q 'Warning'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 1);

    char *paths[] = {src, obj};
    di_cleanup(paths, 2);
}

/* gdb reads params from their slot and globals via DW_OP_addr at a breakpoint. */
TEST(debug_info, gdb_prints_params_and_globals)
{
    char src[256], obj[256], bin[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");
    di_path(bin, sizeof(bin), "dbg", "bin");

    const char *base = strrchr(src, '/') + 1;
    char breakpoint[64];
    snprintf(breakpoint, sizeof(breakpoint), "break %s:7", base);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "gcc -no-pie -g %s -o %s >/dev/null 2>&1", obj, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Params land in their slots after the prologue; globals are addressable. */
    snprintf(cmd, sizeof(cmd),
             "gdb -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' "
             "-ex '%s' -ex run -ex 'print x' -ex 'print z' -ex 'print shelf' -ex 'print counter' "
             "-ex bt -ex quit %s "
             "| awk '/^\\$1 = 10$/ {p1=1} /^\\$2 = 12$/ {p2=1} /^\\$3 = 100$/ {p3=1} "
             "/^\\$4 = 5$/ {p4=1} /^#0 / && /add3/ {f0=1} /^#1 / && /main/ {f1=1} "
             "END {exit !(p1 && p2 && p3 && p4 && f0 && f1)}'",
             breakpoint, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj, bin};
    di_cleanup(paths, 3);
}