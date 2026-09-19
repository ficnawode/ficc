#include "dwarfcheck.h"
#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Structural .debug_line verification via the in-process decoder (no readelf). */

static unsigned int dl_seq;

static void dl_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20e_line_%u_%s.%s", dl_seq++, tag, ext);
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

/* Expected statement rows: mul return (3), add3 (7, 8), main (12, 13). */
static DwarfCheckLines *dl_parse(const char *obj, DwarfCheck *out, Arena *a)
{
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_NOTNULL(out->debug_line);
    DwarfCheckLines *lines = dwarf_check_lines(out, a);
    EXPECT_NOTNULL(lines);
    return lines;
}

TEST(debug_line, gated_sections_present)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_NOTNULL(out->debug_line);
    EXPECT_NOTNULL(out->debug_info);
    EXPECT_NOTNULL(out->debug_abbrev);
    EXPECT_NOTNULL(out->rela_line);
    EXPECT_EQ(vec_size(out->rela_line), 3); /* one set_address per function */

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_line, absent_without_dash_g)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_TRUE(out->debug_line == NULL);
    EXPECT_TRUE(out->debug_info == NULL);
    EXPECT_EQ(vec_size(out->rela_line), 0);

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
    arena_free(a);
}

/* Rows map source lines to monotone .text offsets, with one EndOfSequence per function. */
TEST(debug_line, decoded_rows_map_monotonic_offsets)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckLines *lines = dl_parse(obj, out, a);

    /* The statement rows, in order: one per distinct source statement. */
    const i64 want_lines[] = {3, 7, 8, 12, 13};
    size_t nwant = sizeof(want_lines) / sizeof(want_lines[0]);
    size_t nrows = vec_size(lines->rows);
    EXPECT_EQ(nrows, nwant + 3); /* 5 statements + one EndOfSequence per function */
    size_t ri = 0;
    for (size_t i = 0; i < nrows; i++)
    {
        DwarfCheckRow *r = (DwarfCheckRow *) vec_get(lines->rows, i);
        if (r->end_seq)
        {
            continue;
        }
        EXPECT_TRUE(ri < nwant);
        if (ri < nwant)
        {
            EXPECT_EQ(r->line, want_lines[ri]);
        }
        ri++;
    }
    EXPECT_EQ(ri, nwant);

    /* Statement addresses increase monotonically across the whole file. */
    u64 prev = 0;
    bool first_row = true;
    for (size_t i = 0; i < nrows; i++)
    {
        DwarfCheckRow *r = (DwarfCheckRow *) vec_get(lines->rows, i);
        if (r->end_seq)
        {
            continue;
        }
        if (!first_row)
        {
            EXPECT_TRUE(r->addr > prev);
            prev = r->addr;
        }
        else
        {
            prev = r->addr;
            first_row = false;
        }
    }

    /* One EndOfSequence per function, each closing inside .text. */
    size_t nend = 0;
    for (size_t i = 0; i < nrows; i++)
    {
        DwarfCheckRow *r = (DwarfCheckRow *) vec_get(lines->rows, i);
        if (r->end_seq)
        {
            nend++;
            EXPECT_TRUE(r->addr > 0 && r->addr <= out->text_len);
        }
    }
    EXPECT_EQ(nend, 3);

    /* set_address count matches the functions; every slot's addend is a valid .text offset. */
    EXPECT_EQ(vec_size(lines->set_addresses), 3);
    EXPECT_TRUE(dwarf_check_line_relocs_covered(out, lines, 1, 4));

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
    arena_free(a);
}

/* The set_address relocations carry the actual function .text offsets. */
TEST(debug_line, set_address_slots_resolve_to_function_text)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    dl_write_src(src, sizeof(src), dl_src);
    dl_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckLines *lines = dl_parse(obj, out, a);

    /* Function start offsets are monotone from 0; the last function ends at .text size. */
    u64 prev_off = 0;
    bool first = true;
    for (size_t i = 0; i < vec_size(lines->set_addresses); i++)
    {
        DwarfCheckSetAddr *sa = (DwarfCheckSetAddr *) vec_get(lines->set_addresses, i);
        if (first)
        {
            EXPECT_EQ(sa->value, 0);
            first = false;
        }
        else
        {
            EXPECT_TRUE(sa->value > prev_off);
        }
        prev_off = sa->value;
    }
    DwarfCheckRow *last = (DwarfCheckRow *) vec_get(lines->rows, vec_size(lines->rows) - 1);
    EXPECT_TRUE(last->end_seq);
    EXPECT_EQ(last->addr, out->text_len);

    char *paths[] = {src, obj};
    dl_cleanup(paths, 2);
    arena_free(a);
}