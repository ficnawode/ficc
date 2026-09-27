#include "dwarfcheck.h"
#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int di_seq;

static void di_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20e_info_%u_%s.%s", di_seq++, tag, ext);
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

static DwarfCheckInfo *di_parse(const char *obj, DwarfCheck *out, Arena *a)
{
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_NOTNULL(out->debug_info);
    EXPECT_NOTNULL(out->debug_abbrev);
    DwarfCheckInfo *info = dwarf_check_info(out, a);
    EXPECT_NOTNULL(info);
    return info;
}

TEST(debug_info, function_dies)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = di_parse(obj, out, a);

    EXPECT_EQ(vec_size(dwarf_check_dies_by_tag(info, DW_TAG_compile_unit)), 1);
    EXPECT_EQ(vec_size(dwarf_check_dies_by_tag(info, DW_TAG_subprogram)), 3);
    EXPECT_EQ(vec_size(dwarf_check_dies_by_tag(info, DW_TAG_formal_parameter)), 4);

    EXPECT_NOTNULL(dwarf_check_die_named(info, "add3"));
    EXPECT_NOTNULL(dwarf_check_die_named(info, "helper"));
    EXPECT_NOTNULL(dwarf_check_die_named(info, "main"));

    DwarfCheckDie *add3 = dwarf_check_die_named(info, "add3");
    DwarfCheckAttr *low = dwarf_check_attr(add3, DW_AT_low_pc);
    DwarfCheckAttr *high = dwarf_check_attr(add3, DW_AT_high_pc);
    DwarfCheckAttr *fb = dwarf_check_attr(add3, DW_AT_frame_base);
    EXPECT_NOTNULL(low);
    EXPECT_TRUE(low->kind == DW_ATTR_ADDR);
    EXPECT_NOTNULL(high);
    EXPECT_TRUE(high->kind == DW_ATTR_NUM);
    EXPECT_TRUE(high->num > 0); /* high_pc is a length, not an address */
    EXPECT_NOTNULL(fb);
    EXPECT_TRUE(fb->kind == DW_ATTR_LOC);
    EXPECT_EQ(fb->loc_len, 1);
    EXPECT_EQ(fb->loc[0], DW_OP_call_frame_cfa);

    DwarfCheckDie *params[] = {dwarf_check_die_named(info, "x"), dwarf_check_die_named(info, "y"),
                               dwarf_check_die_named(info, "z")};
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++)
    {
        DwarfCheckDie *p = params[i];
        EXPECT_TRUE(p->tag == DW_TAG_formal_parameter);
        DwarfCheckAttr *loc = dwarf_check_attr(p, DW_AT_location);
        EXPECT_NOTNULL(loc);
        EXPECT_TRUE(loc->kind == DW_ATTR_LOC);
        EXPECT_TRUE(loc->loc_len >= 1);
        bool is_reg = loc->loc[0] >= DW_OP_reg0 && loc->loc[0] <= DW_OP_reg0 + 15;
        bool is_fbreg = loc->loc[0] == DW_OP_fbreg;
        EXPECT_TRUE(is_reg || is_fbreg);
    }

    char *paths[] = {src, obj};
    di_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_info, variable_locations)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = di_parse(obj, out, a);

    DwarfCheckDie *local_t = dwarf_check_die_named(info, "t");
    EXPECT_NOTNULL(local_t);
    EXPECT_TRUE(local_t->tag == DW_TAG_variable);
    DwarfCheckAttr *tloc = dwarf_check_attr(local_t, DW_AT_location);
    EXPECT_NOTNULL(tloc);
    EXPECT_TRUE(tloc->kind == DW_ATTR_LOC);
    Vec *tranges = dwarf_check_locs(out, tloc->num, a);
    EXPECT_NOTNULL(tranges);
    EXPECT_TRUE(vec_size(tranges) >= 1);
    for (size_t i = 0; i < vec_size(tranges); i++)
    {
        DwarfCheckLocRange *r = (DwarfCheckLocRange *) vec_get(tranges, i);
        EXPECT_TRUE(r->begin < r->end);
        EXPECT_TRUE(r->expr_len >= 1);
        bool rreg = r->expr[0] >= DW_OP_reg0 && r->expr[0] <= DW_OP_reg0 + 15;
        EXPECT_TRUE(rreg || r->expr[0] == DW_OP_fbreg);
    }

    DwarfCheckDie *shelf = dwarf_check_die_named(info, "shelf");
    DwarfCheckDie *counter = dwarf_check_die_named(info, "counter");
    DwarfCheckDie *imported = dwarf_check_die_named(info, "imported");
    EXPECT_TRUE(shelf->tag == DW_TAG_variable);
    EXPECT_TRUE(counter->tag == DW_TAG_variable);
    EXPECT_TRUE(imported->tag == DW_TAG_variable);
    EXPECT_NOTNULL(dwarf_check_attr(shelf, DW_AT_location));
    EXPECT_NOTNULL(dwarf_check_attr(counter, DW_AT_location));
    EXPECT_NULL(dwarf_check_attr(imported, DW_AT_location));

    char *paths[] = {src, obj};
    di_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_info, address_slots_relocated)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = di_parse(obj, out, a);

    EXPECT_TRUE(dwarf_check_info_relocs_covered(out, info, 1, 4));

    char *paths[] = {src, obj};
    di_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_info, external_var_has_no_location)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = di_parse(obj, out, a);
    DwarfCheckDie *imported = dwarf_check_die_named(info, "imported");
    EXPECT_NOTNULL(imported);
    EXPECT_TRUE(imported->tag == DW_TAG_variable);
    EXPECT_NULL(dwarf_check_attr(imported, DW_AT_location));
    DwarfCheckDie *shelf = dwarf_check_die_named(info, "shelf");
    DwarfCheckDie *counter = dwarf_check_die_named(info, "counter");
    EXPECT_NOTNULL(dwarf_check_attr(shelf, DW_AT_location));
    EXPECT_NOTNULL(dwarf_check_attr(counter, DW_AT_location));

    char *paths[] = {src, obj};
    di_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_info, non_debug_object_has_no_debug_sections)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    di_write_src(src, sizeof(src), di_src);
    di_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_TRUE(out->debug_info == NULL);
    EXPECT_TRUE(out->debug_line == NULL);
    EXPECT_TRUE(out->debug_abbrev == NULL);
    EXPECT_TRUE(out->debug_loc == NULL);
    EXPECT_TRUE(out->eh_frame == NULL);
    EXPECT_TRUE(out->text != NULL);

    char *paths[] = {src, obj};
    di_cleanup(paths, 2);
    arena_free(a);
}
