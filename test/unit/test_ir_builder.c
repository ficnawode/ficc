#include "harness.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "util/arena.h"
#include <string.h>

static IrModule *build_from_source(const char *src, Arena *arena)
{
    LexResult lexed = lex("<test>", src, arena);
    if (!lexed.tokens)
    {
        return NULL;
    }
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
    if (!ast)
    {
        return NULL;
    }
    ast = semantic_check(ast, arena);
    if (!ast)
    {
        return NULL;
    }
    return ir_build_module(ast, arena);
}

TEST(ir_builder, return42)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source("int main(void) { return 42; }", a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(vec_size(m->funcs), 1);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    EXPECT_TRUE(strcmp(f->name, "main") == 0);
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
    IrModule *m = build_from_source("void f(void) { return; }", a);
    EXPECT_TRUE(m != NULL);

    IrFunction *fn = (IrFunction *) vec_get(m->funcs, 0);
    EXPECT_TRUE(strcmp(fn->name, "f") == 0);
    IrBlock *bb = (IrBlock *) vec_get(fn->blocks, 0);
    IrInstr *ret = (IrInstr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->nops, 0);

    arena_free(a);
}

TEST(ir_builder, empty_body_gets_unreachable)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source("int main(void) { }", a);
    EXPECT_TRUE(m != NULL);

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
    EXPECT_TRUE(m == NULL);
    arena_free(a);
}

TEST(ir_builder, if_else_then_taken)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source(
        "int main(void) { int x; if (1) { x = 10; } else { x = 20; } return x; }", a);
    EXPECT_TRUE(m != NULL);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}

TEST(ir_builder, if_else_else_taken)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source(
        "int main(void) { int x; if (0) { x = 10; } else { x = 20; } return x; }", a);
    EXPECT_TRUE(m != NULL);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 20);
    arena_free(a);
}

TEST(ir_builder, if_no_else)
{
    Arena *a = arena_new();
    IrModule *m =
        build_from_source("int main(void) { int x; x = 5; if (1) { x = 10; } return x; }", a);
    EXPECT_TRUE(m != NULL);
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
    IrModule *m = build_from_source(
        "int main(void) { int i = 0; while (i < 3) { i = i + 1; } return i; }", a);
    EXPECT_TRUE(m != NULL);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    /* entry + while_header + while_body + while_exit */
    EXPECT_EQ(vec_size(f->blocks), 4);

    IrBlock *header = (IrBlock *) vec_get(f->blocks, 1);
    EXPECT_TRUE(strcmp(header->label, "while_header_1") == 0);
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
    IrModule *m = build_from_source(
        "int main(void) { int s = 0; for (int i = 0; i < 4; i = i + 1) { s = s + i; } return s; }",
        a);
    EXPECT_TRUE(m != NULL);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    /* entry + for_header + for_body + for_latch + for_exit */
    EXPECT_EQ(vec_size(f->blocks), 5);

    IrBlock *header = (IrBlock *) vec_get(f->blocks, 1);
    EXPECT_TRUE(strcmp(header->label, "for_header_1") == 0);
    EXPECT_TRUE(header->is_loop_header);
    IrInstr *last = (IrInstr *) vec_last(header->instrs);
    EXPECT_EQ(last->opcode, OP_BRCOND);

    EXPECT_EQ(ir_interp_run(m), 6);

    /* No-cond form: the header falls through to the body with a plain BR. */
    IrModule *m2 = build_from_source(
        "int main(void) { int i = 0; for (;;) { i = i + 1; if (i > 2) break; } return i; }", a);
    EXPECT_TRUE(m2 != NULL);

    IrFunction *f2 = (IrFunction *) vec_get(m2->funcs, 0);
    IrBlock *hdr2 = (IrBlock *) vec_get(f2->blocks, 1);
    EXPECT_TRUE(strcmp(hdr2->label, "for_header_1") == 0);
    IrInstr *last2 = (IrInstr *) vec_last(hdr2->instrs);
    EXPECT_EQ(last2->opcode, OP_BR);

    EXPECT_EQ(ir_interp_run(m2), 3);
    arena_free(a);
}

TEST(ir_builder, short_circuit_structure)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source("int main(void) { return (1 && 0) ? 1 : 0; }", a);
    EXPECT_TRUE(m != NULL);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    EXPECT_TRUE(find_block_by_prefix(f, "land_true") != NULL);
    EXPECT_TRUE(find_block_by_prefix(f, "land_false") != NULL);
    EXPECT_TRUE(find_block_by_prefix(f, "land_rhs") != NULL);

    IrBlock *merge = find_block_by_prefix(f, "land_merge");
    EXPECT_TRUE(merge != NULL);
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
    IrModule *m = build_from_source("int main(void) { goto a; return 0; a: return 42; }", a);
    EXPECT_TRUE(m != NULL);

    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    IrBlock *label_bb = find_block(f, "a");
    EXPECT_TRUE(label_bb != NULL);
    EXPECT_TRUE(label_bb->is_loop_header);

    EXPECT_EQ(ir_interp_run(m), 42);
    arena_free(a);
}

TEST(ir_builder, nested_break_continue_preds)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source(
        "int main(void) { int s = 0; for (int i = 0; i < 10; i = i + 1) { if (i == 3) continue; "
        "if (i == 7) break; s = s + 1; } return s; }",
        a);
    EXPECT_TRUE(m != NULL);

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

TEST(ir_builder, global_string_literal)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source("int main(void) { char *s = \"abc\"; return s[1]; }", a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(vec_size(m->globals), 1);

    IrGlobal *g = (IrGlobal *) vec_get(m->globals, 0);
    EXPECT_TRUE(g->name != NULL);
    EXPECT_TRUE(strncmp(g->name, "__str_", 6) == 0);
    EXPECT_EQ(g->type->kind, TYPE_ARRAY);
    EXPECT_EQ(type_array_elem(g->type), type_char());
    EXPECT_EQ(type_array_len(g->type), 4); /* "abc" + NUL */
    EXPECT_EQ(g->init_len, 4);
    EXPECT_TRUE(g->init_data != NULL);
    EXPECT_TRUE(memcmp(g->init_data, "abc", 4) == 0);
    EXPECT_EQ(g->align, 1);
    EXPECT_EQ(g->section, IR_SECTION_RODATA);

    EXPECT_EQ(ir_interp_run(m), 'b');
    arena_free(a);
}

TEST(ir_builder, multiple_string_literals)
{
    Arena *a = arena_new();
    IrModule *m = build_from_source(
        "int main(void) { char *x = \"hello\"; char *y = \"world\"; return x[0] + y[0]; }", a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(vec_size(m->globals), 2);

    IrGlobal *g0 = (IrGlobal *) vec_get(m->globals, 0);
    IrGlobal *g1 = (IrGlobal *) vec_get(m->globals, 1);
    EXPECT_TRUE(strcmp(g0->name, g1->name) != 0); /* distinct names */
    EXPECT_EQ(g0->section, IR_SECTION_RODATA);
    EXPECT_EQ(g1->section, IR_SECTION_RODATA);

    EXPECT_EQ(ir_interp_run(m), 'h' + 'w');
    arena_free(a);
}
