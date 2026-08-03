#include "harness.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "util/arena.h"
#include <string.h>

static Module *build_from_source(const char *src, Arena *arena)
{
    u64 count;
    Token *tokens = lex("<test>", src, arena, &count);
    if (!tokens)
        return NULL;
    ASTNode *ast = parse(tokens, count, arena);
    if (!ast)
        return NULL;
    ast = semantic_check(ast, arena);
    if (!ast)
        return NULL;
    return ir_build_module(ast, arena);
}

TEST(ir_builder, return42)
{
    Arena *a = arena_new();
    Module *m = build_from_source("int main(void) { return 42; }", a);
    EXPECT_TRUE(m != NULL);
    EXPECT_EQ(vec_size(m->funcs), 1);

    Function *f = (Function *) vec_get(m->funcs, 0);
    EXPECT_TRUE(strcmp(f->name, "main") == 0);
    EXPECT_EQ(vec_size(f->blocks), 1);

    Block *bb = (Block *) vec_get(f->blocks, 0);
    EXPECT_EQ(vec_size(bb->instrs), 1);

    Instr *ret = (Instr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->nops, 1);
    EXPECT_TRUE(ret->ops[0].is_imm);
    EXPECT_EQ(ret->ops[0].u.imm, 42);

    arena_free(a);
}

TEST(ir_builder, return_void)
{
    Arena *a = arena_new();
    Module *m = build_from_source("void f(void) { return; }", a);
    EXPECT_TRUE(m != NULL);

    Function *fn = (Function *) vec_get(m->funcs, 0);
    EXPECT_TRUE(strcmp(fn->name, "f") == 0);
    Block *bb = (Block *) vec_get(fn->blocks, 0);
    Instr *ret = (Instr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(ret->opcode, OP_RET);
    EXPECT_EQ(ret->nops, 0);

    arena_free(a);
}

TEST(ir_builder, empty_body_gets_unreachable)
{
    Arena *a = arena_new();
    Module *m = build_from_source("int main(void) { }", a);
    EXPECT_TRUE(m != NULL);

    Function *f = (Function *) vec_get(m->funcs, 0);
    Block *bb = (Block *) vec_get(f->blocks, 0);
    EXPECT_EQ(vec_size(bb->instrs), 1);

    Instr *u = (Instr *) vec_get(bb->instrs, 0);
    EXPECT_EQ(u->opcode, OP_UNREACHABLE);

    arena_free(a);
}

TEST(ir_builder, wrong_toplevel_returns_null)
{
    Arena *a = arena_new();
    /* Build an int literal AST directly, bypass parser */
    ASTNode *lit = ast_int_literal(a, 42, (Loc) {"t", 1, 1});
    Module *m = ir_build_module(lit, a);
    EXPECT_TRUE(m == NULL);
    arena_free(a);
}

TEST(ir_builder, if_else_then_taken)
{
    Arena *a = arena_new();
    Module *m = build_from_source(
        "int main(void) { int x; if (1) { x = 10; } else { x = 20; } return x; }", a);
    EXPECT_TRUE(m != NULL);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}

TEST(ir_builder, if_else_else_taken)
{
    Arena *a = arena_new();
    Module *m = build_from_source(
        "int main(void) { int x; if (0) { x = 10; } else { x = 20; } return x; }", a);
    EXPECT_TRUE(m != NULL);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 20);
    arena_free(a);
}

TEST(ir_builder, if_no_else)
{
    Arena *a = arena_new();
    Module *m =
        build_from_source("int main(void) { int x; x = 5; if (1) { x = 10; } return x; }", a);
    EXPECT_TRUE(m != NULL);
    i64 result = ir_interp_run(m);
    EXPECT_EQ(result, 10);
    arena_free(a);
}
