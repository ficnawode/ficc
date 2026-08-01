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

TEST(ast, program_constructor)
{
    Arena *a = arena_new();
    Vec *decls = vec_new(a);
    ASTNode *p = ast_program(a, decls, (Loc){"t", 1, 1});
    EXPECT_TRUE(p != NULL);
    EXPECT_EQ(p->kind, AST_PROGRAM);
    EXPECT_TRUE(ast_as(ASTProgram, p)->decls == decls);
    arena_free(a);
}

TEST(ast, var_decl_constructor)
{
    Arena *a = arena_new();
    ASTNode *init = ast_int_literal(a, 7, (Loc){"t", 1, 1});
    ASTNode *d = ast_var_decl(a, type_int(), "x", init, (Loc){"t", 1, 1});
    EXPECT_TRUE(d != NULL);
    EXPECT_EQ(d->kind, AST_VAR_DECL);
    ASTVarDecl *vd = ast_as(ASTVarDecl, d);
    EXPECT_TRUE(strcmp(vd->name, "x") == 0);
    EXPECT_TRUE(vd->init == init);
    arena_free(a);
}

TEST(ast, binary_expr_constructor)
{
    Arena *a = arena_new();
    ASTNode *l = ast_int_literal(a, 1, (Loc){"t", 1, 1});
    ASTNode *r = ast_int_literal(a, 2, (Loc){"t", 1, 1});
    ASTNode *b = ast_binary_expr(a, BIN_ADD, l, r, (Loc){"t", 1, 1});
    EXPECT_TRUE(b != NULL);
    EXPECT_EQ(b->kind, AST_BINARY_EXPR);
    ASTBinaryExpr *be = ast_as(ASTBinaryExpr, b);
    EXPECT_EQ(be->op, BIN_ADD);
    EXPECT_TRUE(be->left == l);
    EXPECT_TRUE(be->right == r);
    arena_free(a);
}

TEST(ast, unary_expr_constructor)
{
    Arena *a = arena_new();
    ASTNode *o = ast_int_literal(a, 5, (Loc){"t", 1, 1});
    ASTNode *u = ast_unary_expr(a, UN_NEG, o, (Loc){"t", 1, 1});
    EXPECT_TRUE(u != NULL);
    EXPECT_EQ(u->kind, AST_UNARY_EXPR);
    EXPECT_TRUE(ast_as(ASTUnaryExpr, u)->operand == o);
    arena_free(a);
}

TEST(ast, call_expr_constructor)
{
    Arena *a = arena_new();
    Vec *args = vec_new(a);
    ASTNode *c = ast_call_expr(a, "foo", args, (Loc){"t", 1, 1});
    EXPECT_TRUE(c != NULL);
    EXPECT_EQ(c->kind, AST_CALL_EXPR);
    EXPECT_TRUE(strcmp(ast_as(ASTCallExpr, c)->callee, "foo") == 0);
    arena_free(a);
}

TEST(ast, ident_constructor)
{
    Arena *a = arena_new();
    ASTNode *id = ast_ident(a, "bar", (Loc){"t", 1, 1});
    EXPECT_TRUE(id != NULL);
    EXPECT_EQ(id->kind, AST_IDENT);
    EXPECT_TRUE(strcmp(ast_as(ASTIdent, id)->name, "bar") == 0);
    arena_free(a);
}
