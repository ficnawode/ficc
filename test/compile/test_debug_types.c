#include "dwarfcheck.h"
#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int dt_seq;

static void dt_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20e_types_%u_%s.%s", dt_seq++, tag, ext);
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

static DwarfCheckInfo *dt_parse(const char *obj, DwarfCheck *out, Arena *a)
{
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_NOTNULL(out->debug_info);
    EXPECT_NOTNULL(out->debug_abbrev);
    DwarfCheckInfo *info = dwarf_check_info(out, a);
    EXPECT_NOTNULL(info);
    return info;
}

TEST(debug_types, type_die_counts_and_names)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = dt_parse(obj, out, a);

    struct
    {
        u32 tag;
        size_t count;
    } counts[] = {
        {DW_TAG_structure_type, 2}, {DW_TAG_union_type, 1}, {DW_TAG_enumeration_type, 1},
        {DW_TAG_member, 8},         {DW_TAG_const_type, 1}, {DW_TAG_subroutine_type, 1},
        {DW_TAG_pointer_type, 5},   {DW_TAG_subprogram, 3}, {DW_TAG_base_type, 5},
    };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    {
        EXPECT_EQ(vec_size(dwarf_check_dies_by_tag(info, counts[i].tag)), counts[i].count);
    }

    struct
    {
        const char *name;
        u32 tag;
    } named[] = {
        {"Node", DW_TAG_structure_type},   {"Flags", DW_TAG_structure_type},
        {"Tagged", DW_TAG_union_type},     {"Color", DW_TAG_enumeration_type},
        {"static_shelf", DW_TAG_variable}, {"g_counter", DW_TAG_variable},
        {"const_cap", DW_TAG_variable},    {"g_op", DW_TAG_variable},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
    {
        DwarfCheckDie *d = dwarf_check_die_named(info, named[i].name);
        EXPECT_NOTNULL(d);
        if (d)
        {
            EXPECT_EQ(d->tag, named[i].tag);
        }
    }

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_types, type_references_resolve)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = dt_parse(obj, out, a);

    DwarfCheckDie *payload = dwarf_check_die_named(info, "payload");
    EXPECT_NOTNULL(payload);
    DwarfCheckAttr *ptype = dwarf_check_attr(payload, DW_AT_type);
    EXPECT_NOTNULL(ptype);
    EXPECT_TRUE(ptype->kind == DW_ATTR_REF);
    DwarfCheckDie *arr = NULL;
    {
        size_t n = vec_size(info->dies);
        for (size_t i = 0; i < n; i++)
        {
            DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
            if (d->off == ptype->ref)
            {
                arr = d;
                break;
            }
        }
    }
    EXPECT_NOTNULL(arr);
    EXPECT_EQ(arr->tag, DW_TAG_array_type);
    /* The subrange carrying DW_AT_count is emitted immediately after the array. */
    size_t arr_idx = (size_t) -1;
    for (size_t i = 0; i < vec_size(info->dies); i++)
    {
        DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
        if (d->off == arr->off)
        {
            arr_idx = i;
            break;
        }
    }
    EXPECT_TRUE(arr_idx != (size_t) -1 && arr_idx + 1 < vec_size(info->dies));
    if (arr_idx != (size_t) -1 && arr_idx + 1 < vec_size(info->dies))
    {
        DwarfCheckDie *sub = (DwarfCheckDie *) vec_get(info->dies, arr_idx + 1);
        EXPECT_EQ(sub->tag, DW_TAG_subrange_type);
        DwarfCheckAttr *count = dwarf_check_attr(sub, DW_AT_count);
        EXPECT_NOTNULL(count);
        EXPECT_TRUE(count->kind == DW_ATTR_NUM);
        EXPECT_EQ(count->num, 4);
    }

    DwarfCheckDie *next = dwarf_check_die_named(info, "next");
    EXPECT_NOTNULL(next);
    DwarfCheckAttr *ntype = dwarf_check_attr(next, DW_AT_type);
    EXPECT_NOTNULL(ntype);
    EXPECT_TRUE(ntype->kind == DW_ATTR_REF);
    bool is_pointer = false;
    {
        size_t n = vec_size(info->dies);
        for (size_t i = 0; i < n; i++)
        {
            DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
            if (d->off == ntype->ref)
            {
                is_pointer = d->tag == DW_TAG_pointer_type;
                break;
            }
        }
    }
    EXPECT_TRUE(is_pointer);

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_types, bitfield_member_offsets)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = dt_parse(obj, out, a);

    DwarfCheckDie *a_mem = dwarf_check_die_named(info, "a");
    DwarfCheckDie *b_mem = dwarf_check_die_named(info, "b");
    EXPECT_NOTNULL(a_mem);
    EXPECT_NOTNULL(b_mem);
    DwarfCheckAttr *asize = dwarf_check_attr(a_mem, DW_AT_bit_size);
    DwarfCheckAttr *aoff = dwarf_check_attr(a_mem, DW_AT_bit_offset);
    DwarfCheckAttr *bsize = dwarf_check_attr(b_mem, DW_AT_bit_size);
    EXPECT_TRUE(asize && asize->num == 3);
    EXPECT_TRUE(aoff && aoff->num == 29); /* bit_offset counts from the storage unit's MSB */
    EXPECT_TRUE(bsize && bsize->num == 5);

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_types, record_layout_attributes)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = dt_parse(obj, out, a);

    DwarfCheckDie *node = dwarf_check_die_named(info, "Node");
    EXPECT_NOTNULL(node);
    DwarfCheckAttr *node_size = dwarf_check_attr(node, DW_AT_byte_size);
    EXPECT_NOTNULL(node_size);
    EXPECT_EQ(node_size->num, 40);

    DwarfCheckDie *value = dwarf_check_die_named(info, "value");
    DwarfCheckDie *next = dwarf_check_die_named(info, "next");
    DwarfCheckDie *payload = dwarf_check_die_named(info, "payload");
    EXPECT_NOTNULL(value);
    EXPECT_NOTNULL(next);
    EXPECT_NOTNULL(payload);
    DwarfCheckAttr *value_loc = dwarf_check_attr(value, DW_AT_data_member_location);
    DwarfCheckAttr *next_loc = dwarf_check_attr(next, DW_AT_data_member_location);
    DwarfCheckAttr *payload_loc = dwarf_check_attr(payload, DW_AT_data_member_location);
    EXPECT_TRUE(value_loc && value_loc->num == 0);
    EXPECT_TRUE(next_loc && next_loc->num == 8);
    EXPECT_TRUE(payload_loc && payload_loc->num == 20);

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_types, address_slots_relocated)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = dt_parse(obj, out, a);

    EXPECT_TRUE(dwarf_check_info_relocs_covered(out, info, 1, 4));

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_types, synthetic_zero_inits_decode_cleanly)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dt_write_src(src, sizeof(src), dt_src);
    dt_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckInfo *info = dt_parse(obj, out, a);

    size_t nzero = 0;
    size_t n = vec_size(info->dies);
    for (size_t i = 0; i < n; i++)
    {
        DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
        DwarfCheckAttr *name = dwarf_check_attr(d, DW_AT_name);
        if (name && name->kind == DW_ATTR_STR && strncmp(name->str, "__zero_", 7) == 0)
        {
            nzero++;
            EXPECT_NOTNULL(dwarf_check_attr(d, DW_AT_location));
        }
    }
    EXPECT_EQ(nzero, 3);

    char *paths[] = {src, obj};
    dt_cleanup(paths, 2);
    arena_free(a);
}
