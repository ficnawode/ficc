#include "harness.h"
#include "testdriver.h"

#include "cfi.h"
#include "codegen.h"
#include "dwarf.h"
#include "dwarfcheck.h"
#include "util/bytebuf.h"

/* In-process decode cross-checked against codegen's own records. */

static Vec *norm_relas(Arena *a, Vec *src, u32 sym_override)
{
    Vec *v = vec_new(a);
    size_t n = src ? vec_size(src) : 0;
    for (size_t i = 0; i < n; i++)
    {
        DwarfReloc *r = (DwarfReloc *) vec_get(src, i);
        DwarfCheckReloc *o = arena_alloc(a, sizeof(DwarfCheckReloc), sizeof(void *));
        o->offset = r->offset;
        o->addend = r->addend;
        o->sym = sym_override ? sym_override : r->sym;
        vec_push(v, o);
    }
    return v;
}

static Vec *norm_cfi_relas(Arena *a, CfiOutput *cfi)
{
    Vec *v = vec_new(a);
    for (size_t i = 0; i < vec_size(cfi->relocs); i++)
    {
        CfiReloc *r = (CfiReloc *) vec_get(cfi->relocs, i);
        DwarfCheckReloc *o = arena_alloc(a, sizeof(DwarfCheckReloc), sizeof(void *));
        o->offset = r->offset;
        o->addend = r->addend;
        o->sym = 1; /* .text section symbol */
        vec_push(v, o);
    }
    return v;
}

static char unit_src[] = "struct Node\n"
                         "{\n"
                         "    int val;\n"
                         "    struct Node *next;\n"
                         "};\n"
                         "static int shelf = 100;\n"
                         "extern int imported;\n"
                         "int counter = 5;\n"
                         "int poke(struct Node *pn)\n"
                         "{\n"
                         "    return pn->val;\n"
                         "}\n"
                         "int fib(int nn, int extra)\n"
                         "{\n"
                         "    if (nn < 2)\n"
                         "    {\n"
                         "        return nn;\n"
                         "    }\n"
                         "    return fib(nn - 1, extra) + fib(nn - 2, extra) + extra;\n"
                         "}\n"
                         "static int helper(int hv)\n"
                         "{\n"
                         "    return hv * 2;\n"
                         "}\n"
                         "int main(void)\n"
                         "{\n"
                         "    return fib(5, 0) + helper(0) + shelf + counter - 115 == 0;\n"
                         "}\n";

static void show_dwarf_out(const char *what, DwarfCheck *out)
{
    if (out->err)
    {
        fprintf(stderr, "  dwarf_out(%s): %s\n", what, out->err);
    }
}

TEST(dwarf_out, reloc_counts_match_the_layout)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(unit_src, a);
    EXPECT_NOTNULL(m);
    CodegenConfig cfg = {.debug = true};
    CodegenModule *cm = codegen_ir_to_machine(m, &cfg, a);
    EXPECT_NOTNULL(cm);
    DwarfOutput out = {0};
    dwarf_build(cm, "unit.c", "/tmp", &out, a);
    CfiOutput *cfi = cfi_build(cm, a);

    size_t nfuncs = vec_size(cm->funcs);
    size_t nglob_loc = 0;
    for (size_t i = 0; i < vec_size(cm->globals); i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        if (g->linkage != IR_LINK_EXTERN)
        {
            nglob_loc++;
        }
    }
    /* CU low_pc, subprogram low_pc per func, DW_OP_addr per located global, set_address per func.
     */
    EXPECT_EQ(vec_size(out.rela_info), 1 + nfuncs + nglob_loc);
    EXPECT_EQ(vec_size(out.rela_line), nfuncs);
    EXPECT_EQ(vec_size(cfi->relocs), nfuncs);

    arena_free(a);
}

TEST(dwarf_out, line_table_matches_codegen_line_entries)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(unit_src, a);
    CodegenConfig cfg = {.debug = true};
    CodegenModule *cm = codegen_ir_to_machine(m, &cfg, a);
    DwarfOutput out = {0};
    dwarf_build(cm, "unit.c", "/tmp", &out, a);

    DwarfCheck dc;
    Vec *rela_line = norm_relas(a, out.rela_line, 0);
    dwarf_check_from_buffers(&dc, a, bytebuf_data(&out.debug_info), bytebuf_len(&out.debug_info),
                             bytebuf_data(&out.debug_abbrev), bytebuf_len(&out.debug_abbrev),
                             bytebuf_data(&out.debug_line), bytebuf_len(&out.debug_line), NULL, 0,
                             NULL, 0, NULL, rela_line, NULL);

    DwarfCheckLines *lines = dwarf_check_lines(&dc, a);
    show_dwarf_out("line", &dc);
    EXPECT_NOTNULL(lines);

    /* Expected rows from codegen's line entries, deduping same-line runs like dwarf.c. */
    size_t nfuncs = vec_size(cm->funcs);
    Vec *exp = vec_new(a); /* Vec<{addr,line}> */
    for (size_t f = 0; f < nfuncs; f++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, f);
        EXPECT_NOTNULL(cf->lines);
        i64 last = 0;
        for (size_t i = 0; i < vec_size(cf->lines); i++)
        {
            LineEntry *le = (LineEntry *) vec_get(cf->lines, i);
            if (i > 0 && le->line == last)
            {
                continue;
            }
            last = (i64) le->line;
            DwarfCheckRow *r = arena_alloc(a, sizeof(DwarfCheckRow), sizeof(void *));
            r->addr = cf->offset + le->offset;
            r->line = (i64) le->line;
            vec_push(exp, r);
        }
    }

    size_t nstmt = 0;
    for (size_t i = 0; i < vec_size(lines->rows); i++)
    {
        DwarfCheckRow *r = (DwarfCheckRow *) vec_get(lines->rows, i);
        if (r->end_seq)
        {
            nstmt++;
        }
    }
    EXPECT_EQ(nstmt, nfuncs);                                 /* one end_sequence per function */
    EXPECT_EQ(vec_size(lines->rows), vec_size(exp) + nfuncs); /* stmt rows + end rows */

    size_t ei = 0;
    for (size_t i = 0; i < vec_size(lines->rows); i++)
    {
        DwarfCheckRow *r = (DwarfCheckRow *) vec_get(lines->rows, i);
        if (r->end_seq)
        {
            continue;
        }
        DwarfCheckRow *want = (DwarfCheckRow *) vec_get(exp, ei);
        EXPECT_EQ(r->addr, want->addr);
        EXPECT_EQ(r->line, want->line);
        ei++;
    }
    EXPECT_EQ(ei, vec_size(exp));

    /* set_address operands equal the function .text offsets in order. */
    EXPECT_EQ(vec_size(lines->set_addresses), nfuncs);
    for (size_t f = 0; f < nfuncs; f++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, f);
        DwarfCheckSetAddr *sa = (DwarfCheckSetAddr *) vec_get(lines->set_addresses, f);
        EXPECT_EQ(sa->value, cf->offset);
    }

    arena_free(a);
}

TEST(dwarf_out, subprograms_params_and_globals_cross_checked)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(unit_src, a);
    CodegenConfig cfg = {.debug = true};
    CodegenModule *cm = codegen_ir_to_machine(m, &cfg, a);
    DwarfOutput out = {0};
    dwarf_build(cm, "unit.c", "/tmp", &out, a);

    DwarfCheck dc;
    dwarf_check_from_buffers(&dc, a, bytebuf_data(&out.debug_info), bytebuf_len(&out.debug_info),
                             bytebuf_data(&out.debug_abbrev), bytebuf_len(&out.debug_abbrev),
                             bytebuf_data(&out.debug_line), bytebuf_len(&out.debug_line), NULL, 0,
                             bytebuf_data(&out.debug_str), bytebuf_len(&out.debug_str),
                             norm_relas(a, out.rela_info, 0), norm_relas(a, out.rela_line, 0),
                             NULL);

    DwarfCheckInfo *info = dwarf_check_info(&dc, a);
    show_dwarf_out("info", &dc);
    EXPECT_NOTNULL(info);

    size_t nfuncs = vec_size(cm->funcs);
    /* One subprogram DIE per function; low_pc via reloc, high_pc the exact size. */
    EXPECT_EQ(vec_size(dwarf_check_dies_by_tag(info, DW_TAG_subprogram)), nfuncs);
    for (size_t f = 0; f < nfuncs; f++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, f);
        DwarfCheckDie *die = dwarf_check_die_named(info, cf->name);
        EXPECT_NOTNULL(die);
        if (!die)
        {
            continue;
        }
        DwarfCheckAttr *low = dwarf_check_attr(die, DW_AT_low_pc);
        EXPECT_NOTNULL(low);
        EXPECT_TRUE(low->kind == DW_ATTR_ADDR);
        DwarfCheckAttr *high = dwarf_check_attr(die, DW_AT_high_pc);
        EXPECT_TRUE(high && high->num == bytebuf_len(cf->bytes));
        /* Parameter DIEs in IR order: name + exact fbreg displacement. */
        for (size_t p = 0; p < vec_size(cf->func->params); p++)
        {
            IrParam *pp = (IrParam *) vec_get(cf->func->params, p);
            DwarfCheckDie *pd = dwarf_check_die_named(info, pp->name);
            EXPECT_NOTNULL(pd);
            if (!pd)
            {
                continue;
            }
            EXPECT_TRUE(pd->tag == DW_TAG_formal_parameter);
            DwarfCheckAttr *loc = dwarf_check_attr(pd, DW_AT_location);
            EXPECT_NOTNULL(loc);
            EXPECT_TRUE(loc->kind == DW_ATTR_LOC);
            EXPECT_TRUE(loc->loc_len >= 2);
            if (loc->kind == DW_ATTR_LOC && loc->loc_len >= 2)
            {
                EXPECT_EQ(loc->loc[0], DW_OP_fbreg);
                i64 disp;
                EXPECT_TRUE(dwarf_check_sleb128(loc->loc + 1, loc->loc_len - 1, &disp) > 0);
                EXPECT_EQ(disp, -(i64) cf->slot_off[pp->vreg] - 16);
            }
        }
    }

    /* Globals: defined ones carry DW_OP_addr, externs none, names match. */
    size_t nglobals = vec_size(cm->globals);
    ByteBuf ro, da;
    bytebuf_init(&ro, a);
    bytebuf_init(&da, a);
    u64 *g_off = codegen_global_offsets(cm, &ro, &da, a);
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        DwarfCheckDie *die = dwarf_check_die_named(info, g->name);
        EXPECT_NOTNULL(die);
        if (!die)
        {
            continue;
        }
        EXPECT_TRUE(die->tag == DW_TAG_variable);
        DwarfCheckAttr *loc = dwarf_check_attr(die, DW_AT_location);
        if (g->linkage == IR_LINK_EXTERN)
        {
            EXPECT_NULL(loc);
        }
        else
        {
            EXPECT_NOTNULL(loc);
            if (loc && loc->kind == DW_ATTR_LOC && loc->loc_len == 9 && loc->loc[0] == DW_OP_addr)
            {
                /* The slot's addend equals the computed global section offset. */
                u32 slot = (u32) (loc->loc - out.debug_info.data) + 1;
                bool seen = false;
                for (size_t r = 0; r < vec_size(out.rela_info); r++)
                {
                    DwarfReloc *rel = (DwarfReloc *) vec_get(out.rela_info, r);
                    if (rel->offset == slot)
                    {
                        seen = true;
                        EXPECT_EQ(rel->addend, (i64) g_off[i]);
                    }
                }
                EXPECT_TRUE(seen);
            }
        }
    }

    /* The self-referential struct terminates: Node's `next` ref lands on a pointer_type DIE. */
    EXPECT_EQ(vec_size(dwarf_check_dies_by_tag(info, DW_TAG_structure_type)), 1);
    DwarfCheckDie *node = dwarf_check_die_named(info, "Node");
    EXPECT_NOTNULL(node);
    DwarfCheckDie *nextm = dwarf_check_die_named(info, "next");
    EXPECT_NOTNULL(nextm);
    DwarfCheckAttr *nt = dwarf_check_attr(nextm, DW_AT_type);
    EXPECT_NOTNULL(nt);
    DwarfCheckDie *next_type = NULL;
    for (size_t i = 0; i < vec_size(info->dies); i++)
    {
        DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
        if (d->off == nt->ref)
        {
            next_type = d;
            break;
        }
    }
    EXPECT_NOTNULL(next_type);
    EXPECT_TRUE(next_type->tag == DW_TAG_pointer_type);

    EXPECT_TRUE(dwarf_check_info_relocs_covered(&dc, info, 1, 4));

    arena_free(a);
}

TEST(dwarf_out, eh_fdes_match_codegen)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(unit_src, a);
    CodegenConfig cfg = {.debug = true};
    CodegenModule *cm = codegen_ir_to_machine(m, &cfg, a);
    DwarfOutput out = {0};
    dwarf_build(cm, "unit.c", "/tmp", &out, a);
    CfiOutput *cfi = cfi_build(cm, a);

    size_t text_len = 0;
    for (size_t i = 0; i < vec_size(cm->funcs); i++)
    {
        text_len += bytebuf_len(((CodegenFunc *) vec_get(cm->funcs, i))->bytes);
    }

    DwarfCheck dc;
    dwarf_check_from_buffers(&dc, a, bytebuf_data(&out.debug_info), bytebuf_len(&out.debug_info),
                             bytebuf_data(&out.debug_abbrev), bytebuf_len(&out.debug_abbrev), NULL,
                             0, bytebuf_data(&cfi->eh_frame), bytebuf_len(&cfi->eh_frame),
                             bytebuf_data(&out.debug_str), bytebuf_len(&out.debug_str), NULL, NULL,
                             norm_cfi_relas(a, cfi));

    DwarfCheckEh *eh = dwarf_check_eh(&dc, a);
    show_dwarf_out("eh", &dc);
    EXPECT_NOTNULL(eh);

    size_t nfuncs = vec_size(cm->funcs);
    EXPECT_EQ(eh->nfde, nfuncs);
    for (size_t f = 0; f < nfuncs; f++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, f);
        DwarfCheckEhEntry *e = (DwarfCheckEhEntry *) vec_get(eh->entries, f + 1); /* skip CIE */
        EXPECT_FALSE(e->is_cie);
        EXPECT_EQ(e->fde_begin, cf->offset);
        EXPECT_EQ(e->fde_range, bytebuf_len(cf->bytes));
        /* Every initial_location slot has a reloc whose addend is the offset. */
        bool covered = false;
        for (size_t r = 0; r < vec_size(cfi->relocs); r++)
        {
            CfiReloc *cr = (CfiReloc *) vec_get(cfi->relocs, r);
            if (cr->offset == e->initial_slot)
            {
                covered = true;
                EXPECT_EQ(cr->addend, (i64) cf->offset);
            }
        }
        EXPECT_TRUE(covered);
    }
    /* The FDE pc ranges tile the whole .text. */
    u64 total = 0;
    for (size_t f = 0; f < nfuncs; f++)
    {
        DwarfCheckEhEntry *e = (DwarfCheckEhEntry *) vec_get(eh->entries, f + 1);
        total += e->fde_range;
    }
    EXPECT_EQ(total, text_len);

    arena_free(a);
}