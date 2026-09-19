#include "dwarfcheck.h"
#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Structural .eh_frame verification via the in-process decoder (no readelf). */

static unsigned int cfi_seq;

static void cfi_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_20e_cfi_%u_%s.%s", cfi_seq++, tag, ext);
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

static DwarfCheckEh *cfi_parse(const char *obj, DwarfCheck *out, Arena *a)
{
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_NOTNULL(out->eh_frame);
    DwarfCheckEh *eh = dwarf_check_eh(out, a);
    EXPECT_NOTNULL(eh);
    return eh;
}

TEST(debug_cfi, eh_frame_present_and_parseable)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    cfi_write_src(src, sizeof(src), cfi_src);
    cfi_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -g -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    DwarfCheckEh *eh = cfi_parse(obj, out, a);

    /* Exactly one CIE (the first entry), then one FDE per function. */
    size_t nentries = vec_size(eh->entries);
    EXPECT_EQ(nentries, 4); /* 1 CIE + 3 FDEs */
    DwarfCheckEhEntry *cie = (DwarfCheckEhEntry *) vec_get(eh->entries, 0);
    EXPECT_TRUE(cie->is_cie);
    EXPECT_EQ(eh->nfde, 3);

    /* Each FDE's CIE pointer targets the CIE at 0; its slot has a matching reloc. */
    u64 text_len = out->text_len;
    u64 expected_begin = 0;
    for (size_t i = 1; i < nentries; i++)
    {
        DwarfCheckEhEntry *e = (DwarfCheckEhEntry *) vec_get(eh->entries, i);
        EXPECT_FALSE(e->is_cie);
        EXPECT_EQ(e->cie_fde_pointer, e->offset + 4); /* back to the CIE at 0 */
        EXPECT_EQ(e->fde_begin, expected_begin);      /* ranges tile .text */
        EXPECT_TRUE(e->fde_range > 0);

        bool covered = false;
        for (size_t j = 0; j < vec_size(out->rela_eh); j++)
        {
            DwarfCheckReloc *r = (DwarfCheckReloc *) vec_get(out->rela_eh, j);
            if (r->offset == e->initial_slot)
            {
                covered = true;
                EXPECT_EQ(r->sym, 1); /* .text section symbol */
                EXPECT_EQ(r->addend, (i64) e->fde_begin);
            }
        }
        EXPECT_TRUE(covered);
        expected_begin = e->fde_begin + e->fde_range;
    }
    /* The union of the FDE ranges is exactly .text. */
    EXPECT_EQ(expected_begin, text_len);

    /* The CIE carries real unwind rules, not an empty instruction stream. */
    EXPECT_TRUE(vec_size(eh->cfa_ops) > 0);

    char *paths[] = {src, obj};
    cfi_cleanup(paths, 2);
    arena_free(a);
}

TEST(debug_cfi, eh_frame_absent_without_dash_g)
{
    Arena *a = arena_new();
    char src[256], obj[256];
    cfi_write_src(src, sizeof(src), cfi_src);
    cfi_path(obj, sizeof(obj), "dbg", "o");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -c %s -o %s >/dev/null 2>&1", FICC_BIN, src, obj);
    EXPECT_EQ(tc_run_shell(cmd), 0);

    DwarfCheck *out = (DwarfCheck *) arena_alloc(a, sizeof(DwarfCheck), sizeof(void *));
    dwarf_check_load(obj, out, a);
    EXPECT_TRUE(out->err == NULL);
    EXPECT_TRUE(out->eh_frame == NULL);
    EXPECT_EQ(vec_size(out->rela_eh), 0);

    char *paths[] = {src, obj};
    cfi_cleanup(paths, 2);
    arena_free(a);
}