#include "harness.h"
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "util/arena.h"

static ASTNode *parse_string(const char *src, Arena *arena)
{
    u64 count;
    Token *tokens = lex("<test>", src, arena, &count);
    if (!tokens)
        return NULL;
    return parse(tokens, count, arena);
}

TEST(parser, minimal_program)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 42; }", a);
    EXPECT_TRUE(ast != NULL);
    EXPECT_EQ(ast->kind, AST_FUNC_DEF);
    arena_free(a);
}

TEST(parser, missing_semicolon)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 42 }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(parser, missing_brace)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 42;", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(parser, missing_lparen)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main void) { return 42; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(parser, missing_rparen)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void { return 42; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(parser, empty_param_list)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main() { return 42; }", a);
    EXPECT_TRUE(ast != NULL);
    EXPECT_EQ(ast->kind, AST_FUNC_DEF);
    arena_free(a);
}

TEST(parser, unknown_type)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("foo main(void) { return 42; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(parser, no_return_value)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return; }", a);
    EXPECT_TRUE(ast != NULL);
    EXPECT_EQ(ast->kind, AST_FUNC_DEF);
    arena_free(a);
}

TEST(parser, trailing_junk)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 42; } extra", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(parser, func_def_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 42; }", a);
    EXPECT_TRUE(ast != NULL);

    ASTFuncDef *fn = ast_as(ASTFuncDef, ast);
    EXPECT_TRUE(fn != NULL);
    EXPECT_EQ(fn->base.kind, AST_FUNC_DEF);
    EXPECT_TRUE(strcmp(fn->name, "main") == 0);
    EXPECT_TRUE(fn->ret_type == type_int());
    EXPECT_TRUE(fn->body != NULL);

    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_TRUE(body != NULL);
    EXPECT_EQ(vec_size(body->stmts), 1);

    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *)vec_get(body->stmts, 0));
    EXPECT_TRUE(ret != NULL);
    EXPECT_TRUE(ret->expr != NULL);

    ASTIntLiteral *lit = ast_as(ASTIntLiteral, ret->expr);
    EXPECT_TRUE(lit != NULL);
    EXPECT_EQ(lit->value, 42);

    arena_free(a);
}
