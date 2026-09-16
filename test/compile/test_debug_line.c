#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int dl_seq;

static void dl_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20b_%u_%s.%s", dl_seq++, tag, ext);
}

static void dl_write_src(char *out, size_t sz, const char *src)
{
    dl_path(out, sz, "dbg", "c");
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void dl_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

static char dl_src[] = "static int mul(int a, int b)\n"
                       "{\n"
                       "    return a * b;\n"
                       "}\n"
                       "int add3(int x, int y, int z)\n"
                       "{\n"
                       "    int t = x + y;\n"
                       "    return t + z;\n"
                       "}\n"
                       "int main(void)\n"
                       "{\n"
                       "    int q = add3(10, 20, 12);\n"
                       "    return q + mul(q, 0) == 42;\n"
                       "}\n";

TEST(debug_line, gated_sections_present)
{
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "readelf -SW %s | grep -q '\\.debug_line'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf -SW %s | grep -q '\\.rela\\.debug_line'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf -SW %s | grep -q '\\.debug_info'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
}

TEST(debug_line, absent_without_dash_g)
{
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf -SW %s | grep -q '\\.debug_line'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 1);

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
}

/* One row per statement at monotonically increasing .text offsets. */
TEST(debug_line, decoded_rows_map_monotonic_offsets)
{
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Rows carry the exact source lines; end-of-sequence marks '-' rows. */
    snprintf(cmd, sizeof(cmd),
             "out=\"$(readelf --debug-dump=decodedline %s "
             "| awk -v f=\"%s\" '$1==f && $2!=\"-\" { s = s (n++ ? \" \" : \"\") $2; "
             "if (prev>strtonum($3)) bad=1; prev=strtonum($3) } END { printf \"%%s\", s; "
             "if (bad) exit 1 }')\"; test \"$out\" = "
             "\"3 7 8 12 13\"",
             obj, src);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
}

/* gdb breaks on a source line, steps, and sees both frames via the CFI. */
TEST(debug_line, gdb_break_next_backtrace)
{
    char src[256], obj[256], bin[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");
    dl_path(bin, sizeof(bin), "dbg", "bin");

    const char *base = strrchr(src, '/') + 1;
    char breakpoint[64];
    snprintf(breakpoint, sizeof(breakpoint), "break %s:8", base);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "gcc -no-pie -g %s -o %s >/dev/null 2>&1", obj, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Break at the add3 return statement; bt must show add3 over main. */
    snprintf(cmd, sizeof(cmd),
             "gdb -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' "
             "-ex '%s' -ex run -ex next -ex bt -ex quit %s "
             "| awk '/^#0 / && /add3/ {a=1} /^#1 / && /main/ {m=1} END {exit !(a && m)}'",
             breakpoint, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj, bin};
    dl_cleanup(paths, 3);
}