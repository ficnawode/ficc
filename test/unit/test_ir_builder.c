#include "harness.h"
#include "testdriver.h"

#include "ast.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"

#include <string.h>

TEST(ir_builder, return42)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) { return 42; }", a);
    EXPECT_NOTNULL(m);
    EXPECT_EQ(vec_size(m->funcs), 1);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    EXPECT_STR_EQ(f->name, "main");
    EXPECT_EQ(vec_size(f->blocks), 1);

    IrBlock *bb = (IrBlock *) vec_get(f->blocks, 0);
    EXPECT_EQ(vec_size(bb->instrs), 1);

    IrInstr *ret = (IrInstr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->nops, 1);
    EXPECT_TRUE(ret->ops[0].is_imm);
    EXPECT_EQ(ret->ops[0].u.imm, 42);

    arena_free(a);
}

TEST(ir_builder, return_void)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("void f(void) { return; }", a);
    EXPECT_NOTNULL(m);

    IrFunction *fn = (IrFunction *) vec_get(m->funcs, 0);
    EXPECT_STR_EQ(fn->name, "f");
    IrBlock *bb = (IrBlock *) vec_get(fn->blocks, 0);
    IrInstr *ret = (IrInstr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->nops, 0);

    arena_free(a);
}

TEST(ir_builder, empty_body_gets_unreachable)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) { }", a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *bb = (IrBlock *) vec_get(f->blocks, 0);
    EXPECT_EQ(vec_size(bb->instrs), 1);

    IrInstr *u = (IrInstr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(u->opcode, OP_UNREACHABLE);

    arena_free(a);
}

TEST(ir_builder, wrong_toplevel_returns_null)
{
    Arena *a = arena_new();
    /* Build an int literal AST directly, bypass parser */
    ASTNode *lit = ast_int_literal(42, false, SUFFIX_NONE, false, (Loc) {"t", 1, 1}, a);
    IrModule *m = ir_build_module(lit, a);
    EXPECT_NULL(m);
    arena_free(a);
}

TEST(ir_builder, if_else_then_taken)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x;\n"
                                  "    if (1) {\n"
                                  "        x = 10;\n"
                                  "    } else {\n"
                                  "        x = 20;\n"
                                  "    }\n"
                                  "    return x;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}

TEST(ir_builder, if_else_else_taken)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x;\n"
                                  "    if (0) {\n"
                                  "        x = 10;\n"
                                  "    } else {\n"
                                  "        x = 20;\n"
                                  "    }\n"
                                  "    return x;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 20);
    arena_free(a);
}

TEST(ir_builder, if_no_else)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x;\n"
                                  "    x = 5;\n"
                                  "    if (1) {\n"
                                  "        x = 10;\n"
                                  "    }\n"
                                  "    return x;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}

static IrBlock *find_block(IrFunction *f, const char *label)
{
    size_t n = vec_size(f->blocks);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        if (strcmp(bb->label, label) == 0)
        {
            return bb;
        }
    }
    return NULL;
}

static IrBlock *find_block_by_prefix(IrFunction *f, const char *prefix)
{
    size_t n = vec_size(f->blocks);
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < n; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        if (strncmp(bb->label, prefix, plen) == 0)
        {
            return bb;
        }
    }
    return NULL;
}

TEST(ir_builder, while_loop_structure)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int i = 0;\n"
                                  "    while (i < 3) {\n"
                                  "        i = i + 1;\n"
                                  "    }\n"
                                  "    return i;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    /* entry + while_header + while_body + while_exit */
    EXPECT_EQ(vec_size(f->blocks), 4);

    IrBlock *header = (IrBlock *) vec_get(f->blocks, 1);
    EXPECT_STR_EQ(header->label, "while_header_1");
    EXPECT_TRUE(header->is_loop_header);

    /* The loop header is the merge point (entry + back edge): its PHI carries
       the loop-carried value of i. */
    IrInstr *first = (IrInstr *) vec_get(header->instrs, 0);
    EXPECT_EQ(first->opcode, OP_PHI);

    IrInstr *last = (IrInstr *) vec_last(header->instrs);
    EXPECT_EQ(last->opcode, OP_BRCOND);

    EXPECT_EQ(ir_interp_run(m), 3);
    arena_free(a);
}

TEST(ir_builder, for_loop_structure)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 4; i = i + 1) {\n"
                                  "        s = s + i;\n"
                                  "    }\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    /* entry + for_header + for_body + for_latch + for_exit */
    EXPECT_EQ(vec_size(f->blocks), 5);

    IrBlock *header = (IrBlock *) vec_get(f->blocks, 1);
    EXPECT_STR_EQ(header->label, "for_header_1");
    EXPECT_TRUE(header->is_loop_header);
    IrInstr *last = (IrInstr *) vec_last(header->instrs);
    EXPECT_EQ(last->opcode, OP_BRCOND);

    EXPECT_EQ(ir_interp_run(m), 6);

    /* No-cond form: the header falls through to the body with a plain BR. */
    IrModule *m2 = tc_build_module("int main(void) {\n"
                                   "    int i = 0;\n"
                                   "    for (;;) {\n"
                                   "        i = i + 1;\n"
                                   "        if (i > 2) {\n"
                                   "            break;\n"
                                   "        }\n"
                                   "    }\n"
                                   "    return i;\n"
                                   "}\n",
                                   a);
    EXPECT_NOTNULL(m2);

    IrFunction *f2 = (IrFunction *) vec_get(m2->funcs, 0);
    IrBlock *hdr2 = (IrBlock *) vec_get(f2->blocks, 1);
    EXPECT_STR_EQ(hdr2->label, "for_header_1");
    IrInstr *last2 = (IrInstr *) vec_last(hdr2->instrs);
    EXPECT_EQ(last2->opcode, OP_BR);

    EXPECT_EQ(ir_interp_run(m2), 3);
    arena_free(a);
}

TEST(ir_builder, short_circuit_structure)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) { return (1 && 0) ? 1 : 0; }", a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    EXPECT_NOTNULL(find_block_by_prefix(f, "land_true"));
    EXPECT_NOTNULL(find_block_by_prefix(f, "land_false"));
    EXPECT_NOTNULL(find_block_by_prefix(f, "land_rhs"));

    IrBlock *merge = find_block_by_prefix(f, "land_merge");
    EXPECT_NOTNULL(merge);
    IrInstr *first = (IrInstr *) vec_get(merge->instrs, 0);
    EXPECT_EQ(first->opcode, OP_PHI);
    EXPECT_EQ(first->extra.phi.nentries, 2);
    EXPECT_TRUE(first->extra.phi.entries[0].val.is_imm);
    EXPECT_EQ(first->extra.phi.entries[0].val.u.imm, 1);
    EXPECT_TRUE(first->extra.phi.entries[1].val.is_imm);
    EXPECT_EQ(first->extra.phi.entries[1].val.u.imm, 0);

    EXPECT_EQ(ir_interp_run(m), 0);
    arena_free(a);
}

TEST(ir_builder, goto_label_block)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    goto a;\n"
                                  "    return 0;\n"
                                  "a:\n"
                                  "    return 42;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *label_bb = find_block(f, "a");
    EXPECT_NOTNULL(label_bb);
    EXPECT_TRUE(label_bb->is_loop_header);

    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

TEST(ir_builder, nested_break_continue_preds)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 10; i = i + 1) {\n"
                                  "        if (i == 3) {\n"
                                  "            continue;\n"
                                  "        }\n"
                                  "        if (i == 7) {\n"
                                  "            break;\n"
                                  "        }\n"
                                  "        s = s + 1;\n"
                                  "    }\n"
                                  "    return s;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, bi);
        size_t ninstrs = vec_size(bb->instrs);
        for (size_t ii = 0; ii < ninstrs; ii++)
        {
            IrInstr *ins = (IrInstr *) vec_get(bb->instrs, ii);
            if (ins->opcode == OP_PHI)
            {
                EXPECT_EQ(ins->extra.phi.nfilled, ins->extra.phi.nentries);
            }
        }
    }

    EXPECT_EQ(ir_interp_run(m), 6);
    arena_free(a);
}

TEST(ir_builder, switch_emits_op_switch)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x = 2;\n"
                                  "    switch (x) {\n"
                                  "    case 1:\n"
                                  "        return 1;\n"
                                  "    case 2:\n"
                                  "        return 42;\n"
                                  "    default:\n"
                                  "        return 0;\n"
                                  "    }\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrInstr *sw = NULL;
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks && !sw; bi++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(bb->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, ii);
            if (in->opcode == OP_SWITCH)
            {
                sw = in;
                break;
            }
        }
    }
    EXPECT_NOTNULL(sw);
    EXPECT_EQ(sw->nops, 1);
    EXPECT_EQ(sw->extra.sw.ncases, 2);
    EXPECT_EQ(sw->extra.sw.cases[0].val, 1);
    EXPECT_EQ(sw->extra.sw.cases[1].val, 2);
    EXPECT_NOTNULL(sw->extra.sw.default_label);

    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

TEST(ir_builder, global_string_literal)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    char *s = \"abc\";\n"
                                  "    return s[1];\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    EXPECT_EQ(vec_size(m->globals), 1);

    IrGlobal *g = (IrGlobal *) vec_get(m->globals, 0);
    EXPECT_NOTNULL(g->name);
    EXPECT_TRUE(strncmp(g->name, "__str_", 6) == 0);
    EXPECT_EQ(g->type->kind, TYPE_ARRAY);
    EXPECT_EQ(type_array_elem(g->type), type_char());
    EXPECT_EQ(type_array_len(g->type), 4); /* "abc" + NUL */
    EXPECT_EQ(g->init_len, 4);
    EXPECT_NOTNULL(g->init_data);
    EXPECT_TRUE(memcmp(g->init_data, "abc", 4) == 0);
    EXPECT_EQ(g->align, 1);
    EXPECT_EQ(g->section, IR_SECTION_RODATA);

    EXPECT_EQ(ir_interp_run(m), 'b');
    arena_free(a);
}

TEST(ir_builder, multiple_string_literals)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    char *x = \"hello\";\n"
                                  "    char *y = \"world\";\n"
                                  "    return x[0] + y[0];\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    EXPECT_EQ(vec_size(m->globals), 2);

    IrGlobal *g0 = (IrGlobal *) vec_get(m->globals, 0);
    IrGlobal *g1 = (IrGlobal *) vec_get(m->globals, 1);
    EXPECT_STR_NE(g0->name, g1->name); /* distinct names */
    EXPECT_EQ(g0->section, IR_SECTION_RODATA);
    EXPECT_EQ(g1->section, IR_SECTION_RODATA);

    EXPECT_EQ(ir_interp_run(m), 'h' + 'w');
    arena_free(a);
}

/* --- Phase 11: cast lowering shape --- */

TEST(ir_builder, cast_narrowing_emits_trunc)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int v = 300;\n"
                                  "    return (char)v;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    bool saw_trunc = false;
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    for (size_t bi = 0; bi < vec_size(f->blocks); bi++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, bi);
        for (size_t ii = 0; ii < vec_size(bb->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, ii);
            if (in->opcode == OP_TRUNC)
            {
                saw_trunc = true;
            }
        }
    }
    EXPECT_TRUE(saw_trunc);
    EXPECT_EQ(ir_interp_run(m), 44);
    arena_free(a);
}

TEST(ir_builder, cast_pointer_passthrough)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x = 1;\n"
                                  "    int *p = (int *)(void *)&x;\n"
                                  "    return *p;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    EXPECT_EQ(ir_interp_run(m), 1);
    arena_free(a);
}

TEST(ir_builder, alignof_type_folds_to_imm)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) { return (int) _Alignof(long); }", a);
    EXPECT_NOTNULL(m);
    /* _Alignof folds to a compile-time constant: the first instruction is a
       width conversion of the imm 8 (size_t result converted to int), with no
       memory access or arithmetic on the way. */
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *bb = (IrBlock *) vec_get(f->blocks, 0);
    EXPECT_EQ(vec_size(bb->instrs), 3);
    IrInstr *in = (IrInstr *) vec_get(bb->instrs, 0);
    EXPECT_TRUE(in->opcode == OP_ZEXT || in->opcode == OP_SEXT);
    EXPECT_TRUE(in->ops[0].is_imm);
    EXPECT_EQ(in->ops[0].u.imm, 8);
    EXPECT_EQ(ir_interp_run(m), 8);
    arena_free(a);
}

TEST(ir_builder, alignof_expr_folds_to_imm)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    long x;\n"
                                  "    return (int) _Alignof(x);\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *bb = (IrBlock *) vec_get(f->blocks, 0);
    IrInstr *in = (IrInstr *) vec_get(bb->instrs, 0);
    EXPECT_TRUE(in->opcode == OP_ZEXT || in->opcode == OP_SEXT);
    EXPECT_TRUE(in->ops[0].is_imm);
    EXPECT_EQ(in->ops[0].u.imm, 8);
    EXPECT_EQ(ir_interp_run(m), 8);
    arena_free(a);
}

TEST(ir_builder, bool_assign_normalizes_to_icmp)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int main(void) {\n"
                                  "    int x = 300;\n"
                                  "    _Bool b = x;\n"
                                  "    return (int)b;\n"
                                  "}\n",
                                  a);
    EXPECT_NOTNULL(m);
    bool saw_icmp_ne = false;
    bool saw_trunc = false;
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    for (size_t bi = 0; bi < vec_size(f->blocks); bi++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, bi);
        for (size_t ii = 0; ii < vec_size(bb->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(bb->instrs, ii);
            if (in->opcode == OP_ICMP_NE)
            {
                saw_icmp_ne = true;
            }
            if (in->opcode == OP_TRUNC)
            {
                saw_trunc = true;
            }
        }
    }
    EXPECT_TRUE(saw_icmp_ne);
    EXPECT_TRUE(saw_trunc);
    EXPECT_EQ(ir_interp_run(m), 1);
    arena_free(a);
}

TEST(ir_builder, bool_casts_wrap_to_zero_one)
{
    Arena *a = arena_new();
    EXPECT_EQ(ir_interp_run(tc_build_module("_Bool g = 300;\n"
                                            "int main(void) {\n"
                                            "    return (int)g;\n"
                                            "}\n",
                                            a)),
              1);
    arena_free(a);
}
