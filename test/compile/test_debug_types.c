#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Fixture: bit-fields, self-referential struct/array, union, enum, const, function pointer. */
static unsigned int dt_seq;

static void dt_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20d_%u_%s.%s", dt_seq++, tag, ext);
}

static void dt_write_src(char *out, size_t sz, const char *src)
{
    dt_path(out, sz, "dbg", "c");
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void dt_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

static char dt_src[] = "enum Color { RED = 1, GREEN = 2, BLUE = 3, TOTAL = 4 };\n"
                       "\n"
                       "struct Flags\n"
                       "{\n"
                       "    unsigned a : 3;\n"
                       "    unsigned b : 5;\n"
                       "};\n"
                       "\n"
                       "struct Node\n"
                       "{\n"
                       "    int value;\n"
                       "    struct Node *next;\n"
                       "    struct Flags flags;\n"
                       "    int payload[4];\n"
                       "};\n"
                       "\n"
                       "union Tagged\n"
                       "{\n"
                       "    int i;\n"
                       "    float f;\n"
                       "};\n"
                       "\n"
                       "static int static_shelf = 7;\n"
                       "int g_counter = 41;\n"
                       "const int const_cap = 3;\n"
                       "int (*g_op)(int);\n"
                       "\n"
                       "int nodal(int x)\n"
                       "{\n"
                       "    return x * 2;\n"
                       "}\n"
                       "\n"
                       "int drawn(struct Node *n, enum Color c, struct Flags *fl, "
                       "const int *cap, union Tagged *u)\n"
                       "{\n"
                       "    n->value = 10;\n"
                       "    fl->a = 3;\n"
                       "    fl->b = 5;\n"
                       "    u->i = 42;\n"
                       "    return n->value + *cap + static_shelf + g_counter + "
                       "(c == GREEN ? 0 : 100) + nodal(u->i) - 84;\n"
                       "}\n"
                       "\n"
                       "int main(void)\n"
                       "{\n"
                       "    struct Node node = {0};\n"
                       "    struct Flags flags = {0};\n"
                       "    union Tagged u = {0};\n"
                       "    enum Color col = GREEN;\n"
                       "    g_op = nodal;\n"
                       "    return drawn(&node, col, &flags, &const_cap, &u) == 61;\n"
                       "}\n";

/* readelf sees every rich-type tag with clean (warning-free) parse. */
TEST(debug_types, rich_type_dies)
{
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    struct
    {
        const char *pattern;
    } tags[] = {
        {"DW_TAG_pointer_type"},     {"DW_TAG_array_type"}, {"DW_TAG_subrange_type"},
        {"DW_TAG_structure_type"},   {"DW_TAG_union_type"}, {"DW_TAG_member"},
        {"DW_TAG_enumeration_type"}, {"DW_TAG_const_type"}, {"DW_TAG_subroutine_type"},
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
    {
        snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q '%s'", obj,
                 tags[i].pattern);
        EXPECT_EQ(tc_run_shell(cmd), 0);
    }

    /* Attributes that only type-rich output carries. */
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_bit_size'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_bit_offset'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info %s | grep -q 'DW_AT_data_member_location'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_count'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Names of the fixture's record/enum types survive to the DIE tree. */
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : Node'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : Flags'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : Tagged'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s | grep -q 'DW_AT_name        : Color'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Deterministic DIE counts for the fixture's type surface. */
    struct
    {
        const char *tag;
        int count;
    } counts[] = {
        {"DW_TAG_structure_type", 2}, {"DW_TAG_union_type", 1}, {"DW_TAG_enumeration_type", 1},
        {"DW_TAG_member", 8},         {"DW_TAG_const_type", 1}, {"DW_TAG_subroutine_type", 1},
        {"DW_TAG_pointer_type", 5},
    };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    {
        snprintf(cmd, sizeof(cmd),
                 "test \"$(readelf --debug-dump=info %s | grep -c '%s')\" = \"%d\"", obj,
                 counts[i].tag, counts[i].count);
        EXPECT_EQ(tc_run_shell(cmd), 0);
    }

    /* bit-field rows match gcc's encoding: a is bits 0..3 of a 32-bit unit. */
    snprintf(cmd, sizeof(cmd),
             "readelf --debug-dump=info %s | grep -A4 'DW_AT_name        : a' | "
             "grep -q 'DW_AT_bit_offset  : 29'",
             obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* A parse that readelf accepts without complaint. */
    snprintf(cmd, sizeof(cmd), "readelf --debug-dump=info %s 2>&1 | grep -q 'Warning'", obj);
    EXPECT_EQ(tc_run_shell(cmd), 1);

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
}

/* gdb resolves every rich type and unwinds a two-function call through the CFI. */
TEST(debug_types, gdb_rich_types_certificate)
{
    char src[256], obj[256], bin[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");
    dt_path(bin, sizeof(bin), "dbg", "bin");

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "gcc -no-pie -g %s -o %s >/dev/null 2>&1", obj, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* Step once (next homes to the next statement), then continue to the return. */
    char break_first[320], break_last[320];
    snprintf(break_first, sizeof(break_first), "break %s:35", src);
    snprintf(break_last, sizeof(break_last), "break %s:39", src);
    snprintf(cmd, sizeof(cmd),
             "gdb -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' "
             "-ex '%s' -ex '%s' -ex run "
             "-ex next -ex 'print n->value' -ex bt "
             "-ex continue "
             "-ex 'print fl->a' -ex 'print fl->b' -ex 'print c' "
             "-ex 'print *fl' -ex 'print *n' -ex 'print n->next' -ex 'print n->payload' "
             "-ex 'print static_shelf' -ex 'print g_counter' -ex 'print const_cap' "
             "-ex 'print g_op' -ex 'info locals' -ex bt "
             "-ex quit %s "
             "| awk "
             "'/^\\$1 = 10$/ {v1=1} "
             "/^#0  drawn / && /:36$/ {home=1} "
             "/^\\$2 = 3$/ {v2=1} "
             "/^\\$3 = 5$/ {v3=1} "
             "/^\\$4 = 2$/ {v4=1} "
             "/^\\$5 = \\{a = 3, b = 5\\}$/ {v5=1} "
             "/^\\$6 = \\{value = 10, next = 0x0, flags = \\{a = 0, b = 0\\}, "
             "payload = \\{0, 0, 0, 0\\}\\}$/ {v6=1} "
             "/^\\$7 = \\(struct Node \\*\\) 0x0$/ {v7=1} "
             "/^\\$8 = \\{0, 0, 0, 0\\}$/ {v8=1} "
             "/^\\$9 = 7$/ {v9=1} "
             "/^\\$10 = 41$/ {v10=1} "
             "/^\\$11 = 3$/ {v11=1} "
             "/^\\$12 = \\(int \\(\\*\\)\\(int\\)\\) 0x[0-9a-f]+ <nodal>$/ {v12=1} "
             "/^No locals\\.$/ {nl=1} "
             "/^#0 / && /drawn/ {f0=1} "
             "/^#1 / && /main/ {f1=1} "
             "END {exit !(v1 && v2 && v3 && v4 && v5 && v6 && v7 && v8 && v9 && v10 && "
             "v11 && v12 && nl && home && f0 && f1)}'",
             break_first, break_last, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    /* A three-frame backtrace through the CFI (nodal <- drawn <- main). */
    char break_nodal[320];
    snprintf(break_nodal, sizeof(break_nodal), "break %s:30", src);
    snprintf(cmd, sizeof(cmd),
             "gdb -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' "
             "-ex '%s' -ex run -ex bt -ex quit %s "
             "| awk '/^#0 / && /nodal/ {n=1} /^#1 / && /drawn/ {d=1} /^#2 / && /main/ {m=1} "
             "END {exit !(n && d && m)}'",
             break_nodal, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    char *paths[] = {src, obj, bin};
    dt_cleanup(paths, 3);
}