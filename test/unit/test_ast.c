#include "harness.h"
#include <string.h>
#include "ast.h"
#include "util/arena.h"

TEST(ast, func_def_constructor)
{
    Arena *a = arena_new();
    Vec *params = vec_new(a);
    ASTNode *body = ast_compound_stmt(a, vec_new(a), (Loc){"t", 1, 1});
    ASTNode *node = ast_func_def(a, type_int(), "foo", params, body, (Loc){"t", 1, 1});

    EXPECT_TRUE(node != NULL);
    EXPECT_EQ(node->kind, AST_FUNC_DEF);

    ASTFuncDef *fn = ast_as(ASTFuncDef, node);
    EXPECT_TRUE(fn->ret_type == type_int());
    EXPECT_TRUE(strcmp(fn->name, "foo") == 0);
    EXPECT_TRUE(fn->body == body);

    arena_free(a);
}

TEST(ast, compound_stmt_constructor)
{
    Arena *a = arena_new();
    Vec *stmts = vec_new(a);
    ASTNode *node = ast_compound_stmt(a, stmts, (Loc){"t", 1, 1});

    EXPECT_TRUE(node != NULL);
    EXPECT_EQ(node->kind, AST_COMPOUND_STMT);

    ASTCompoundStmt *cs = ast_as(ASTCompoundStmt, node);
    EXPECT_TRUE(cs->stmts == stmts);

    arena_free(a);
}

TEST(ast, return_stmt_constructor)
{
    Arena *a = arena_new();
    ASTNode *expr = ast_int_literal(a, 7, (Loc){"t", 1, 1});
    ASTNode *node = ast_return_stmt(a, expr, (Loc){"t", 1, 1});

    EXPECT_TRUE(node != NULL);
    EXPECT_EQ(node->kind, AST_RETURN_STMT);

    ASTReturnStmt *rs = ast_as(ASTReturnStmt, node);
    EXPECT_TRUE(rs->expr == expr);

    arena_free(a);
}

TEST(ast, return_stmt_null_expr)
{
    Arena *a = arena_new();
    ASTNode *node = ast_return_stmt(a, NULL, (Loc){"t", 1, 1});

    EXPECT_TRUE(node != NULL);
    EXPECT_EQ(node->kind, AST_RETURN_STMT);

    ASTReturnStmt *rs = ast_as(ASTReturnStmt, node);
    EXPECT_TRUE(rs->expr == NULL);

    arena_free(a);
}

TEST(ast, int_literal_constructor)
{
    Arena *a = arena_new();
    ASTNode *node = ast_int_literal(a, -123, (Loc){"t", 1, 1});

    EXPECT_TRUE(node != NULL);
    EXPECT_EQ(node->kind, AST_INT_LITERAL);

    ASTIntLiteral *lit = ast_as(ASTIntLiteral, node);
    EXPECT_EQ(lit->value, -123);

    arena_free(a);
}

TEST(ast, dump_does_not_crash)
{
    Arena *a = arena_new();
    Vec *params = vec_new(a);
    Vec *stmts = vec_new(a);
    ASTNode *body = ast_compound_stmt(a, stmts, (Loc){"t", 1, 1});
    ASTNode *fn = ast_func_def(a, type_int(), "bar", params, body, (Loc){"t", 1, 1});
    ast_dump(fn);
    arena_free(a);
}

TEST(ast, kind_names)
{
    EXPECT_TRUE(strcmp(ast_kind_name(AST_FUNC_DEF), "AST_FUNC_DEF") == 0);
    EXPECT_TRUE(strcmp(ast_kind_name(AST_INT_LITERAL), "AST_INT_LITERAL") == 0);
}
