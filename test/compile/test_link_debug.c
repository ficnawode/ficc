#include "dwarfcheck.h"
#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Phase 24 I: -g pass-through. Linking a -g object with filc carries the
   .debug_* and .eh_frame sections into the executable and relocates their
   address slots to the linked image (no .rela.* survives). */

static unsigned int ld_seq;

static void ld_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_24i_link_%u_%s.%s", ld_seq++, tag, ext);
}

static void ld_write_src(char *out, size_t sz, const char *src)
{
    ld_path(out, sz, "dbg", "c");
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static const char ld_src[] = "static int shelf = 100;\n"
                             "int add3(int x, int y, int z)\n"
                             "{\n"
                             "    int t = x + y;\n"
                             "    return t + z + shelf;\n"
                             "}\n"
                             "int main(void)\n"
                             "{\n"
                             "    return add3(10, 20, 12) == 142 ? 0 : 1;\n"
                             "}\n";

TEST(link_debug, sections_relocated_into_image)
{
    Arena *a = arena_new();
    char src[256], bin[256];
    ld_write_src(src, sizeof(src), ld_src);
    ld_path(bin, sizeof(bin), "dbg", "bin");

    char cmd[2048];
    /* Freestanding link: debug pass-through is independent of libc. */
    snprintf(cmd, sizeof(cmd), "%s -g -nostdlib %s -o %s >/dev/null 2>&1", FICC_BIN, src, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    dwarf_check_load(bin, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_TRUE(out->linked);
    EXPECT_NOTNULL(out->debug_info);
    EXPECT_NOTNULL(out->debug_line);
    EXPECT_NOTNULL(out->debug_abbrev);
    EXPECT_NOTNULL(out->eh_frame);

    /* .debug_line set_address slots hold the linked .text addresses. */
    DwarfCheckLines *lines = dwarf_check_lines(out, a);
    EXPECT_NOTNULL(lines);
    EXPECT_TRUE(vec_size(lines->set_addresses) > 0);
    for (size_t i = 0; i < vec_size(lines->set_addresses); i++)
    {
        DwarfCheckSetAddr *sa = (DwarfCheckSetAddr *) vec_get(lines->set_addresses, i);
        EXPECT_TRUE(sa->value >= 0x400000);
    }

    /* Subprogram low_pc is a linked address, and the DIEs survive the merge. */
    DwarfCheckInfo *info = dwarf_check_info(out, a);
    EXPECT_NOTNULL(info);
    DwarfCheckDie *add3 = dwarf_check_die_named(info, "add3");
    EXPECT_NOTNULL(add3);
    DwarfCheckAttr *low = dwarf_check_attr(add3, DW_AT_low_pc);
    EXPECT_NOTNULL(low);
    EXPECT_TRUE(low->addr >= 0x400000);

    /* .eh_frame FDEs cover the linked functions. */
    DwarfCheckEh *eh = dwarf_check_eh(out, a);
    EXPECT_NOTNULL(eh);
    EXPECT_TRUE(eh->nfde >= 2);
    for (size_t i = 0; i < vec_size(eh->entries); i++)
    {
        DwarfCheckEhEntry *e = (DwarfCheckEhEntry *) vec_get(eh->entries, i);
        if (!e->is_cie)
        {
            EXPECT_TRUE(e->fde_begin >= 0x400000);
        }
    }

    unlink(src);
    unlink(bin);
    arena_free(a);
}

TEST(link_debug, linked_program_runs)
{
    char src[256], bin[256];
    ld_write_src(src, sizeof(src), ld_src);
    ld_path(bin, sizeof(bin), "run", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -nostdlib %s -o %s >/dev/null 2>&1", FICC_BIN, src, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>&1", bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    unlink(src);
    unlink(bin);
}

TEST(link_debug, non_debug_link_has_no_debug_sections)
{
    Arena *a = arena_new();
    char src[256], bin[256];
    ld_write_src(src, sizeof(src), ld_src);
    ld_path(bin, sizeof(bin), "plain", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -nostdlib %s -o %s >/dev/null 2>&1", FICC_BIN, src, bin);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    dwarf_check_load(bin, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_TRUE(out->linked);
    EXPECT_EQ(out->debug_info_len, 0);
    EXPECT_EQ(out->debug_line_len, 0);
    EXPECT_EQ(out->debug_abbrev_len, 0);
    EXPECT_EQ(out->eh_frame_len, 0);
    EXPECT_NOTNULL(out->text);

    unlink(src);
    unlink(bin);
    arena_free(a);
}
