#include "ast.h"
#include "harness.h"
#include "testdriver.h"
#include "util/arena.h"

#include <string.h>

TEST(parser, minimal_program)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return 42; }", a);
    EXPECT_NOTNULL(ast);
    EXPECT_EQ(ast->kind, AST_PROGRAM);
    arena_free(a);
}

TEST(parser, missing_semicolon)
{
    EXPECT_PARSE_FAIL("int main(void) { return 42 }");
}

TEST(parser, missing_brace)
{
    EXPECT_PARSE_FAIL("int main(void) { return 42;");
}

TEST(parser, missing_lparen)
{
    EXPECT_PARSE_FAIL("int main void) { return 42; }");
}

TEST(parser, missing_rparen)
{
    EXPECT_PARSE_FAIL("int main(void { return 42; }");
}

TEST(parser, empty_param_list)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main() { return 42; }", a);
    EXPECT_NOTNULL(ast);
    EXPECT_EQ(ast->kind, AST_PROGRAM);
    arena_free(a);
}

TEST(parser, unknown_type)
{
    EXPECT_PARSE_FAIL("foo main(void) { return 42; }");
}

TEST(parser, no_return_value)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return; }", a);
    EXPECT_NOTNULL(ast);
    EXPECT_EQ(ast->kind, AST_PROGRAM);
    arena_free(a);
}

TEST(parser, trailing_junk)
{
    EXPECT_PARSE_FAIL("int main(void) { return 42; } extra");
}

TEST(parser, func_def_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return 42; }", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_NOTNULL(prog);
    EXPECT_EQ(vec_size(prog->decls), 1);

    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_NOTNULL(fn);
    EXPECT_EQ(fn->base.kind, AST_FUNC_DEF);
    EXPECT_STR_EQ(fn->name, "main");
    EXPECT_TRUE(fn->ret_type == type_int());
    EXPECT_NOTNULL(fn->body);

    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_NOTNULL(body);
    EXPECT_EQ(vec_size(body->stmts), 1);

    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_NOTNULL(ret);
    EXPECT_NOTNULL(ret->expr);

    ASTIntLiteral *lit = ast_as(ASTIntLiteral, ret->expr);
    EXPECT_NOTNULL(lit);
    EXPECT_EQ(lit->value, 42);

    arena_free(a);
}

TEST(parser, multiple_functions)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(void) {\n"
                            "    return 1;\n"
                            "}\n"
                            "int g(void) {\n"
                            "    return 2;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 2);
    ASTFuncDef *f = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTFuncDef *g = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    EXPECT_STR_EQ(f->name, "f");
    EXPECT_STR_EQ(g->name, "g");
    arena_free(a);
}

TEST(parser, params_with_names)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int add(int a, int b) { return a + b; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->params), 2);
    ASTVarDecl *p0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->params, 0));
    ASTVarDecl *p1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->params, 1));
    EXPECT_STR_EQ(p0->name, "a");
    EXPECT_STR_EQ(p1->name, "b");
    arena_free(a);
}

TEST(parser, local_var_decl)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x;\n"
                            "    return x;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_EQ(vec_size(body->stmts), 2);
    EXPECT_EQ(((ASTNode *) vec_get(body->stmts, 0))->kind, AST_VAR_DECL);
    arena_free(a);
}

TEST(parser, var_decl_with_init)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x = 5;\n"
                            "    return x;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_NOTNULL(vd->init);
    EXPECT_EQ(vd->init->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, binary_precedence)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return 1 + 2 * 3; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTBinaryExpr *add = ast_as(ASTBinaryExpr, ret->expr);
    EXPECT_EQ(add->op, BIN_ADD);
    EXPECT_EQ(add->left->kind, AST_INT_LITERAL);
    EXPECT_EQ(add->right->kind, AST_BINARY_EXPR);
    ASTBinaryExpr *mul = ast_as(ASTBinaryExpr, add->right);
    EXPECT_EQ(mul->op, BIN_MUL);
    arena_free(a);
}

TEST(parser, unary_minus)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return -42; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTUnaryExpr *un = ast_as(ASTUnaryExpr, ret->expr);
    EXPECT_EQ(un->op, UN_NEG);
    EXPECT_EQ(un->operand->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, function_call)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return foo(1, 2); }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCallExpr *call = ast_as(ASTCallExpr, ret->expr);
    EXPECT_STR_EQ(call->callee, "foo");
    EXPECT_EQ(vec_size(call->args), 2);
    arena_free(a);
}

TEST(parser, assignment)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x;\n"
                            "    x = 5;\n"
                            "    return x;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTExprStmt *es = ast_as(ASTExprStmt, (ASTNode *) vec_get(body->stmts, 1));
    ASTBinaryExpr *assign = ast_as(ASTBinaryExpr, es->expr);
    EXPECT_EQ(assign->op, BIN_ASSIGN);
    EXPECT_EQ(assign->left->kind, AST_IDENT);
    EXPECT_EQ(assign->right->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, if_else)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    if (1) {\n"
                            "        return 10;\n"
                            "    } else {\n"
                            "        return 20;\n"
                            "    }\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_EQ(vec_size(body->stmts), 1);
    ASTIfStmt *is = ast_as(ASTIfStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_NOTNULL(is->cond);
    EXPECT_NOTNULL(is->then_branch);
    EXPECT_NOTNULL(is->else_branch);
    arena_free(a);
}

TEST(parser, if_no_else)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    if (1)\n"
                            "        return 10;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTIfStmt *is = ast_as(ASTIfStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_NOTNULL(is->cond);
    EXPECT_NOTNULL(is->then_branch);
    EXPECT_NULL(is->else_branch);
    arena_free(a);
}

TEST(parser, dangling_else)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    if (1)\n"
                            "        if (0)\n"
                            "            return 10;\n"
                            "        else\n"
                            "            return 20;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTIfStmt *outer = ast_as(ASTIfStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTIfStmt *inner = ast_as(ASTIfStmt, outer->then_branch);
    EXPECT_NOTNULL(inner->else_branch);
    arena_free(a);
}

TEST(parser, string_literal_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    char *s = \"hello\";\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(vd->base.kind, AST_VAR_DECL);
    EXPECT_EQ(vd->init->kind, AST_STRING_LITERAL);
    ASTStringLiteral *sl = ast_as(ASTStringLiteral, vd->init);
    EXPECT_EQ(sl->length, 5);
    EXPECT_TRUE(memcmp(sl->data, "hello", 5) == 0);
    arena_free(a);
}

TEST(parser, sizeof_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return sizeof(int); }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ret->expr->kind, AST_SIZEOF_TYPE);
    arena_free(a);
}

TEST(parser, sizeof_var_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x;\n"
                            "    return sizeof x;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(ret->expr->kind, AST_SIZEOF_EXPR);
    ASTSizeofExpr *se = ast_as(ASTSizeofExpr, ret->expr);
    EXPECT_EQ(se->operand->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, deref_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int *p;\n"
                            "    return *p;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(ret->expr->kind, AST_UNARY_EXPR);
    EXPECT_EQ(ast_as(ASTUnaryExpr, ret->expr)->op, UN_DEREF);
    arena_free(a);
}

TEST(parser, addr_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int arr[1];\n"
                            "    int *p = &arr;\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(vd->init->kind, AST_UNARY_EXPR);
    EXPECT_EQ(ast_as(ASTUnaryExpr, vd->init)->op, UN_ADDR);
    arena_free(a);
}

TEST(parser, subscript_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int arr[2];\n"
                            "    return arr[0];\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(ret->expr->kind, AST_SUBSCRIPT_EXPR);
    ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, ret->expr);
    EXPECT_EQ(se->array->kind, AST_IDENT);
    EXPECT_EQ(se->index->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, pointer_type_specifier)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int *p;\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(vd->type->kind, TYPE_PTR);
    EXPECT_EQ(type_deref(vd->type), type_int());
    arena_free(a);
}

TEST(parser, array_declarator)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int arr[10];\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(vd->type->kind, TYPE_ARRAY);
    EXPECT_EQ(type_array_elem(vd->type), type_int());
    EXPECT_EQ(type_array_len(vd->type), 10);
    arena_free(a);
}
