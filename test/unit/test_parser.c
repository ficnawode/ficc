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

TEST(parser, void_ptr_param)
{
    /* `(void *p)` is a pointer-to-void parameter, not the empty `(void)` marker. */
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int use(void *p) {\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->sig.params), 1);
    ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->sig.params, 0));
    EXPECT_EQ(param->type->kind, TYPE_PTR);
    EXPECT_EQ(type_deref(param->type)->kind, TYPE_VOID);
    arena_free(a);
}

TEST(parser, void_still_empty_param_list)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(void) { return 42; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->sig.params), 0);
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
    EXPECT_STR_EQ(fn->sig.name, "main");
    EXPECT_TRUE(fn->sig.ret_type == type_int());
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

TEST(parser, prototype_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int add(int a, int b);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_NOTNULL(prog);
    EXPECT_EQ(vec_size(prog->decls), 1);

    ASTNode *decl = (ASTNode *) vec_get(prog->decls, 0);
    EXPECT_EQ(decl->kind, AST_FUNC_DECL);

    ASTFuncDecl *fd = ast_as(ASTFuncDecl, decl);
    EXPECT_STR_EQ(fd->sig.name, "add");
    EXPECT_TRUE(fd->sig.ret_type == type_int());
    EXPECT_EQ(vec_size(fd->sig.params), 2);
    EXPECT_EQ(fd->sig.spec.storage, SC_NONE);
    EXPECT_FALSE(fd->sig.is_variadic);

    arena_free(a);
}

TEST(parser, prototype_variadic)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int sum(int n, ...);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_STR_EQ(fd->sig.name, "sum");
    EXPECT_TRUE(fd->sig.is_variadic);
    EXPECT_EQ(vec_size(fd->sig.params), 1);

    arena_free(a);
}

/* C11 §6.2.1p4: parameter names have function-prototype scope, not file scope. */
TEST(parser, parenthesized_declarator_param_does_not_leak)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("extern int(foo)(int option);\n"
                            "typedef struct option { int x; } option;\n"
                            "option o;\n",
                            a);
    EXPECT_NOTNULL(ast);
    EXPECT_EQ(ast->kind, AST_PROGRAM);
    arena_free(a);
}

TEST(parser, prototype_storage_class)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("static int helper(int x);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(fd->sig.spec.storage, SC_STATIC);

    arena_free(a);
}

TEST(parser, prototype_then_definition)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int add(int a, int b);\n"
                            "int add(int a, int b) {\n"
                            "    return a + b;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 2);
    EXPECT_EQ(((ASTNode *) vec_get(prog->decls, 0))->kind, AST_FUNC_DECL);
    EXPECT_EQ(((ASTNode *) vec_get(prog->decls, 1))->kind, AST_FUNC_DEF);

    arena_free(a);
}

TEST(parser, empty_ellipsis_prototype_rejected)
{
    /* C11 §6.7.6.3p8: `...` must follow at least one named parameter. */
    EXPECT_PARSE_FAIL("int f(...);");
}

TEST(parser, prototype_unnamed_params)
{
    /* C11 §6.7.6.3: parameter names are optional in a function declaration. */
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int add(int, int);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fd->sig.params), 2);
    for (size_t i = 0; i < 2; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(fd->sig.params, i));
        EXPECT_NULL(param->name);
        EXPECT_TRUE(param->type == type_int());
    }

    arena_free(a);
}

TEST(parser, prototype_unnamed_pointer_params)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int *, int *);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fd->sig.params), 2);
    for (size_t i = 0; i < 2; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(fd->sig.params, i));
        EXPECT_NULL(param->name);
        EXPECT_EQ(param->type->kind, TYPE_PTR);
    }

    arena_free(a);
}

TEST(parser, prototype_unnamed_variadic)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int sum(int, ...);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_TRUE(fd->sig.is_variadic);
    EXPECT_EQ(vec_size(fd->sig.params), 1);
    ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(fd->sig.params, 0));
    EXPECT_NULL(param->name);

    arena_free(a);
}

TEST(parser, definition_unnamed_param_rejected)
{
    /* C11 §6.9.1p6: a function definition must name every parameter. */
    EXPECT_PARSE_FAIL("int f(int, int) {\n"
                      "    return 1;\n"
                      "}");
}

TEST(parser, mixed_named_unnamed_prototype)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int x, int);", a);
    EXPECT_NOTNULL(ast);

    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fd->sig.params), 2);
    ASTVarDecl *p0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fd->sig.params, 0));
    ASTVarDecl *p1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fd->sig.params, 1));
    EXPECT_STR_EQ(p0->name, "x");
    EXPECT_NULL(p1->name);

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
    EXPECT_STR_EQ(f->sig.name, "f");
    EXPECT_STR_EQ(g->sig.name, "g");
    arena_free(a);
}

TEST(parser, params_with_names)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int add(int a, int b) { return a + b; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->sig.params), 2);
    ASTVarDecl *p0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->sig.params, 0));
    ASTVarDecl *p1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->sig.params, 1));
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

TEST(parser, alignof_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return _Alignof(long); }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ret->expr->kind, AST_ALIGNOF_TYPE);
    EXPECT_EQ(ast_as(ASTAlignofType, ret->expr)->type->kind, TYPE_LONG);
    arena_free(a);
}

TEST(parser, alignof_var_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x;\n"
                            "    return _Alignof(x);\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(ret->expr->kind, AST_ALIGNOF_EXPR);
    ASTAlignofExpr *ae = ast_as(ASTAlignofExpr, ret->expr);
    EXPECT_EQ(ae->operand->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, static_assert_file_scope_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("_Static_assert(sizeof(int) == 4, \"msg\");\n"
                            "int main(void) {\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 2);
    ASTStaticAssert *sa = ast_as(ASTStaticAssert, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(sa->base.kind, AST_STATIC_ASSERT);
    EXPECT_STR_EQ(sa->msg, "msg");
    EXPECT_STR_EQ(ast_kind_name(sa->expr->kind), "AST_BINARY_EXPR");
    arena_free(a);
}

TEST(parser, static_assert_block_scope_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    _Static_assert(1, \"block\");\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTStaticAssert *sa = ast_as(ASTStaticAssert, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(sa->base.kind, AST_STATIC_ASSERT);
    EXPECT_STR_EQ(sa->msg, "block");
    EXPECT_EQ(sa->expr->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, static_assert_non_string_message_rejected)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("_Static_assert(1, 42);\n"
                            "int main(void) {\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NULL(ast);
    arena_free(a);
}

TEST(parser, bool_decl_specifier)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("_Bool g;\n"
                            "int main(void) {\n"
                            "    _Bool b;\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTVarDecl *g = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(g->type->kind, TYPE_BOOL);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *b = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(b->type->kind, TYPE_BOOL);
    arena_free(a);
}

TEST(parser, bool_cast_target)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x = 5;\n"
                            "    return (_Bool)x;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(ret->expr->kind, AST_CAST_EXPR);
    EXPECT_EQ(ast_as(ASTCastExpr, ret->expr)->target_type->kind, TYPE_BOOL);
    arena_free(a);
}

TEST(parser, alignas_records_requested_alignment)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("_Alignas(16) int g;\n"
                            "int main(void) {\n"
                            "    _Alignas(8) int x;\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTVarDecl *g = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(g->alignas, 16);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *x = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(x->alignas, 8);
    arena_free(a);
}

TEST(parser, alignas_typedef_rejected)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef _Alignas(16) int T;\n"
                            "int main(void) {\n"
                            "    return 0;\n"
                            "}",
                            a);
    EXPECT_NULL(ast);
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

TEST(parser, cast_node)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return (int)5; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ret->expr->kind, AST_CAST_EXPR);
    ASTCastExpr *ce = ast_as(ASTCastExpr, ret->expr);
    EXPECT_TRUE(ce->target_type == type_int());
    EXPECT_EQ(ce->operand->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, paren_is_not_cast)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return (5); }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ret->expr->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, call_is_not_cast)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int x) { return x; }\n"
                            "int main(void) { return f(3); }",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ret->expr->kind, AST_CALL_EXPR);
    ASTCallExpr *call = ast_as(ASTCallExpr, ret->expr);
    EXPECT_EQ(((ASTNode *) vec_get(call->args, 0))->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, call_arg_cast)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int x) { return x; }\n"
                            "int main(void) { return f((int)3); }",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCallExpr *call = ast_as(ASTCallExpr, ret->expr);
    EXPECT_EQ(((ASTNode *) vec_get(call->args, 0))->kind, AST_CAST_EXPR);
    arena_free(a);
}

TEST(parser, cast_qualified_target)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    int x;\n"
                            "    const int *p = (const int *)&x;\n"
                            "    return *p;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 1));
    EXPECT_EQ(vd->init->kind, AST_CAST_EXPR);
    ASTCastExpr *ce = ast_as(ASTCastExpr, vd->init);
    EXPECT_TRUE(type_is_ptr(ce->target_type));
    EXPECT_TRUE(type_is_const(type_deref(ce->target_type)));
    arena_free(a);
}

TEST(parser, cast_enum_target)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("enum color { RED };\n"
                            "int main(void) { enum color c = (enum color)1;\n"
                            "    return (int)c;\n"
                            "}",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(vd->init->kind, AST_CAST_EXPR);
    ASTCastExpr *ce = ast_as(ASTCastExpr, vd->init);
    EXPECT_TRUE(ce->target_type->kind == TYPE_ENUM);
    arena_free(a);
}

static ASTCaseStmt *first_case_of(const char *src, Arena *a)
{
    ASTNode *ast = tc_parse(src, a);
    if (!ast)
    {
        return NULL;
    }
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTSwitchStmt *sw = ast_as(ASTSwitchStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCompoundStmt *sw_body = ast_as(ASTCompoundStmt, sw->body);
    ASTNode *first = (ASTNode *) vec_get(sw_body->stmts, 0);
    if (first->kind != AST_CASE_STMT)
    {
        return NULL;
    }
    return ast_as(ASTCaseStmt, first);
}

TEST(parser, cast_folds_in_case_label)
{
    Arena *a = arena_new();
    /* (char)300 folds to 44 (C11 §6.6: casts are allowed in constant expressions). */
    ASTCaseStmt *cs = first_case_of("int main(void) {\n"
                                    "    switch (44) {\n"
                                    "    case (char)300:\n"
                                    "        return 1;\n"
                                    "    default:\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "}",
                                    a);
    EXPECT_NOTNULL(cs);
    EXPECT_TRUE(cs->value_known);
    EXPECT_EQ(cs->value, 44);
    arena_free(a);
}

TEST(parser, cast_folds_sizeof_in_case_label)
{
    Arena *a = arena_new();
    ASTCaseStmt *cs = first_case_of("int main(void) {\n"
                                    "    switch (4) {\n"
                                    "    case (int)sizeof(int):\n"
                                    "        return 1;\n"
                                    "    default:\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "}",
                                    a);
    EXPECT_NOTNULL(cs);
    EXPECT_TRUE(cs->value_known);
    EXPECT_EQ(cs->value, 4);
    arena_free(a);
}

TEST(parser, cast_folds_unsigned_in_case_label)
{
    Arena *a = arena_new();
    ASTCaseStmt *cs = first_case_of("int main(void) {\n"
                                    "    switch (255) {\n"
                                    "    case (unsigned char)-1:\n"
                                    "        return 1;\n"
                                    "    default:\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "}",
                                    a);
    EXPECT_NOTNULL(cs);
    EXPECT_TRUE(cs->value_known);
    EXPECT_EQ(cs->value, 255);
    arena_free(a);
}

TEST(parser, typedef_toplevel_decl_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef int Foo;\n"
                            "Foo g;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 3);
    EXPECT_EQ(((ASTNode *) vec_get(prog->decls, 0))->kind, AST_TYPEDEF_DECL);
    ASTTypedefDecl *td = ast_as(ASTTypedefDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_STR_EQ(td->name, "Foo");
    EXPECT_EQ(td->type, type_int());
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 1));
    EXPECT_EQ(vd->type, type_int());
    arena_free(a);
}

TEST(parser, typedef_block_scope_decl)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    typedef int T;\n"
                            "    T x;\n"
                            "    return 0;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_EQ(vec_size(body->stmts), 3);
    EXPECT_EQ(((ASTNode *) vec_get(body->stmts, 0))->kind, AST_TYPEDEF_DECL);
    EXPECT_EQ(((ASTNode *) vec_get(body->stmts, 1))->kind, AST_VAR_DECL);
    EXPECT_EQ(((ASTNode *) vec_get(body->stmts, 2))->kind, AST_RETURN_STMT);
    arena_free(a);
}

TEST(parser, typedef_ptr_const_quals)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef int *IP;\n"
                            "const IP p;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 1));
    /* `const IP` is a const *pointer* to int: top-level bit on the pointer. */
    EXPECT_TRUE(type_is_const(vd->type));
    EXPECT_EQ(type_deref(type_unqual(vd->type))->kind, TYPE_INT);
    arena_free(a);
}

TEST(parser, typedef_forward_record_cast_and_sizeof)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef struct Block Block;\n"
                            "struct Block { int data; Block *next; };\n"
                            "int main(void) {\n"
                            "    Block b;\n"
                            "    Block *p = (Block *)&b;\n"
                            "    int s = sizeof(Block);\n"
                            "    return 0;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 3);
    EXPECT_EQ(((ASTNode *) vec_get(prog->decls, 0))->kind, AST_TYPEDEF_DECL);
    EXPECT_EQ(((ASTNode *) vec_get(prog->decls, 1))->kind, AST_STRUCT_DECL);
    ASTStructDecl *sd = ast_as(ASTStructDecl, (ASTNode *) vec_get(prog->decls, 1));
    ASTVarDecl *next_field = ast_as(ASTVarDecl, (ASTNode *) vec_get(sd->fields, 1));
    EXPECT_STR_EQ(next_field->name, "next");
    EXPECT_EQ(type_deref(next_field->type)->kind, TYPE_STRUCT);
    arena_free(a);
}

TEST(parser, typedef_paren_not_cast)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int take(int x) { return x; }\n"
                            "int main(void) {\n"
                            "    int a = 3;\n"
                            "    return (a);\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    arena_free(a);
}

TEST(parser, typedef_cast_target)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef int T;\n"
                            "int main(void) {\n"
                            "    return (T)3 + (T *)0 != 0;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    arena_free(a);
}

TEST(parser, typedef_name_redeclared_as_var_rejected)
{
    EXPECT_PARSE_FAIL("typedef int T;\n"
                      "int T;\n"
                      "int main(void) { return 0; }\n");
}

TEST(parser, typedef_name_redeclared_locally_rejected)
{
    EXPECT_PARSE_FAIL("int main(void) {\n"
                      "    int T;\n"
                      "    typedef int T;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(parser, typedef_conflicting_redefinition_rejected)
{
    EXPECT_PARSE_FAIL("typedef int T;\n"
                      "typedef long T;\n"
                      "int main(void) { return 0; }\n");
}

TEST(parser, typedef_same_type_redecl_ok)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef int T;\n"
                            "typedef int T;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    arena_free(a);
}

TEST(parser, typedef_shadowed_by_var_not_type)
{
    EXPECT_PARSE_FAIL("typedef int T;\n"
                      "int main(void) {\n"
                      "    int T;\n"
                      "    T x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(parser, typedef_param_use)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef int T;\n"
                            "int twice(T v) { return v + v; }\n"
                            "int main(void) { return twice(3); }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTVarDecl *p = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->sig.params, 0));
    EXPECT_EQ(p->type, type_int());
    arena_free(a);
}

static ASTNode *first_main_stmt(const char *body, Arena *a)
{
    ASTNode *ast = tc_parse(body, a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body_ast = ast_as(ASTCompoundStmt, fn->body);
    return (ASTNode *) vec_get(body_ast->stmts, 0);
}

static ASTReturnStmt *first_return(const char *body, Arena *a)
{
    return ast_as(ASTReturnStmt, first_main_stmt(body, a));
}

TEST(parser, incdec_postfix_shape)
{
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return i++; }\n", a);
    ASTIncDecExpr *ie = ast_as(ASTIncDecExpr, ret->expr);
    EXPECT_NOTNULL(ie);
    EXPECT_EQ(ie->base.kind, AST_INCDEC_EXPR);
    EXPECT_TRUE(ie->is_inc);
    EXPECT_TRUE(ie->is_postfix);
    EXPECT_EQ(ie->operand->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, incdec_prefix_shape)
{
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return --x; }\n", a);
    ASTIncDecExpr *ie = ast_as(ASTIncDecExpr, ret->expr);
    EXPECT_NOTNULL(ie);
    EXPECT_FALSE(ie->is_inc);
    EXPECT_FALSE(ie->is_postfix);
    EXPECT_EQ(ie->operand->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, postfix_binds_tighter_than_deref)
{
    /* `*p++` is `*(p++)`. */
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return *p++; }\n", a);
    ASTUnaryExpr *de = ast_as(ASTUnaryExpr, ret->expr);
    EXPECT_NOTNULL(de);
    EXPECT_EQ(de->base.kind, AST_UNARY_EXPR);
    EXPECT_EQ(de->op, UN_DEREF);
    ASTIncDecExpr *ie = ast_as(ASTIncDecExpr, de->operand);
    EXPECT_NOTNULL(ie);
    EXPECT_TRUE(ie->is_postfix);
    arena_free(a);
}

TEST(parser, prefix_binds_looser_than_deref)
{
    /* `++*p` is `++(*p)`. */
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return ++*p; }\n", a);
    ASTIncDecExpr *ie = ast_as(ASTIncDecExpr, ret->expr);
    EXPECT_NOTNULL(ie);
    EXPECT_FALSE(ie->is_postfix);
    EXPECT_EQ(ie->operand->kind, AST_UNARY_EXPR);
    arena_free(a);
}

TEST(parser, postfix_in_subscript_index)
{
    Arena *a = arena_new();
    ASTVarDecl *vd =
        ast_as(ASTVarDecl, first_main_stmt("int main(void) { void *q = &a[i++]; return 0; }\n", a));
    ASTUnaryExpr *addr = ast_as(ASTUnaryExpr, vd->init);
    EXPECT_NOTNULL(addr);
    EXPECT_EQ(addr->op, UN_ADDR);
    ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, addr->operand);
    EXPECT_NOTNULL(se);
    ASTIncDecExpr *ie = ast_as(ASTIncDecExpr, se->index);
    EXPECT_NOTNULL(ie);
    EXPECT_TRUE(ie->is_postfix);
    arena_free(a);
}

TEST(parser, chained_postfix_ops)
{
    /* `p++->x` groups as `(p++)->x`. */
    Arena *a = arena_new();
    ASTVarDecl *vd =
        ast_as(ASTVarDecl, first_main_stmt("int main(void) { void *q = &p++->x; return 0; }\n", a));
    ASTUnaryExpr *addr = ast_as(ASTUnaryExpr, vd->init);
    EXPECT_NOTNULL(addr);
    ASTMemberAccess *ma = ast_as(ASTMemberAccess, addr->operand);
    EXPECT_NOTNULL(ma);
    EXPECT_TRUE(ma->is_arrow);
    ASTIncDecExpr *ie = ast_as(ASTIncDecExpr, ma->object);
    EXPECT_NOTNULL(ie);
    EXPECT_TRUE(ie->is_postfix);
    arena_free(a);
}

TEST(parser, comma_expression_shape)
{
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return (x, y); }\n", a);
    ASTBinaryExpr *be = ast_as(ASTBinaryExpr, ret->expr);
    EXPECT_NOTNULL(be);
    EXPECT_EQ(be->base.kind, AST_BINARY_EXPR);
    EXPECT_EQ(be->op, BIN_COMMA);
    EXPECT_EQ(be->left->kind, AST_IDENT);
    EXPECT_EQ(be->right->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, comma_left_associative)
{
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return (a, b, c); }\n", a);
    ASTBinaryExpr *outer = ast_as(ASTBinaryExpr, ret->expr);
    EXPECT_NOTNULL(outer);
    EXPECT_EQ(outer->op, BIN_COMMA);
    ASTBinaryExpr *inner = ast_as(ASTBinaryExpr, outer->left);
    EXPECT_NOTNULL(inner);
    EXPECT_EQ(inner->op, BIN_COMMA);
    EXPECT_EQ(inner->left->kind, AST_IDENT);
    EXPECT_EQ(inner->right->kind, AST_IDENT);
    EXPECT_EQ(outer->right->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, comma_in_args_is_separator)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int a, int b) { return 0; }\n"
                            "int main(void) { return f((1, 2), 3); }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCallExpr *call = ast_as(ASTCallExpr, ret->expr);
    EXPECT_NOTNULL(call);
    EXPECT_EQ(vec_size(call->args), 2);
    ASTBinaryExpr *first = ast_as(ASTBinaryExpr, (ASTNode *) vec_get(call->args, 0));
    EXPECT_NOTNULL(first);
    EXPECT_EQ(first->op, BIN_COMMA);
    ASTNode *second = (ASTNode *) vec_get(call->args, 1);
    EXPECT_EQ(second->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, comma_in_subscript_index)
{
    Arena *a = arena_new();
    ASTReturnStmt *ret = first_return("int main(void) { return a[x, y]; }\n", a);
    ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, ret->expr);
    EXPECT_NOTNULL(se);
    ASTBinaryExpr *idx = ast_as(ASTBinaryExpr, se->index);
    EXPECT_NOTNULL(idx);
    EXPECT_EQ(idx->op, BIN_COMMA);
    arena_free(a);
}

TEST(parser, comma_not_in_case)
{
    EXPECT_PARSE_FAIL("int main(void) { int x = 1; switch (x) { case 1, 2: return 0; } }\n");
}

TEST(parser, llong_specifier_types)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("long long ll;\n"
                            "unsigned long long ull;\n"
                            "signed char sc;\n"
                            "signed s;\n"
                            "long int li;\n"
                            "signed long long int slli;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 7);
    ASTVarDecl *d0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(d0->type->kind, TYPE_LLONG);
    ASTVarDecl *d1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 1));
    EXPECT_EQ(d1->type->kind, TYPE_ULLONG);
    ASTVarDecl *d2 = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 2));
    EXPECT_EQ(d2->type->kind, TYPE_CHAR);
    EXPECT_TRUE(type_is_signed_int(d2->type));
    ASTVarDecl *d3 = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 3));
    EXPECT_EQ(d3->type->kind, TYPE_INT);
    ASTVarDecl *d4 = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 4));
    EXPECT_EQ(d4->type->kind, TYPE_LONG);
    ASTVarDecl *d5 = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 5));
    EXPECT_EQ(d5->type->kind, TYPE_LLONG);
    arena_free(a);
}

TEST(parser, multi_declarator_shape)
{
    /* Multiple declarators share the specifier type and wrap in AST_DECL_LIST. */
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int a = 1, b = 2;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTDeclList *dl = ast_as(ASTDeclList, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_NOTNULL(dl);
    EXPECT_EQ(vec_size(dl->decls), 2);
    ASTVarDecl *a0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, 0));
    EXPECT_STR_EQ(a0->name, "a");
    EXPECT_EQ(a0->type->kind, TYPE_INT);
    ASTVarDecl *a1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, 1));
    EXPECT_STR_EQ(a1->name, "b");
    EXPECT_EQ(a1->type->kind, TYPE_INT);
    arena_free(a);
}

TEST(parser, declarator_star_split)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int *a, b;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTDeclList *dl = ast_as(ASTDeclList, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_NOTNULL(dl);
    ASTVarDecl *a0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, 0));
    EXPECT_EQ(a0->type->kind, TYPE_PTR);
    EXPECT_EQ(type_deref(a0->type)->kind, TYPE_INT);
    ASTVarDecl *a1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, 1));
    EXPECT_STR_EQ(a1->name, "b");
    EXPECT_EQ(a1->type->kind, TYPE_INT);
    arena_free(a);
}

TEST(parser, single_declarator_no_list)
{
    /* One declarator stays a bare AST_VAR_DECL (no wrapper node). */
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int a;\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(((ASTNode *) vec_get(prog->decls, 0))->kind, AST_VAR_DECL);
    arena_free(a);
}

TEST(parser, anon_struct_typedef)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef struct { int x; int y; } Point;\n"
                            "int main(void) {\n"
                            "    Point p;\n"
                            "    p.x = 1;\n"
                            "    return 0;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTTypedefDecl *td = ast_as(ASTTypedefDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_NOTNULL(td);
    EXPECT_STR_EQ(td->name, "Point");
    EXPECT_TRUE(type_is_record(td->type));
    EXPECT_TRUE(type_is_complete(td->type));
    arena_free(a);
}

TEST(parser, block_scope_tag_definition_statement)
{
    /* A block-scope `struct S { ... };` defines a complete, usable type. */
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    struct S { int v; };\n"
                            "    struct S s;\n"
                            "    s.v = 42;\n"
                            "    return s.v;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_EQ(vec_size(body->stmts), 4);
    EXPECT_EQ(((ASTNode *) vec_get(body->stmts, 0))->kind, AST_STRUCT_DECL);
    arena_free(a);
}

TEST(parser, combined_definition_declarator)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    struct P { int lo; } pr;\n"
                            "    pr.lo = 42;\n"
                            "    return pr.lo;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_EQ(vec_size(body->stmts), 3);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_NOTNULL(vd);
    EXPECT_TRUE(type_is_record(vd->type));
    EXPECT_TRUE(type_is_complete(vd->type));
    arena_free(a);
}

TEST(parser, anon_enum_typedef)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef enum { A, B } Kind;\n"
                            "int main(void) { return B; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTTypedefDecl *td = ast_as(ASTTypedefDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_NOTNULL(td);
    EXPECT_TRUE(type_is_enum(td->type));
    arena_free(a);
}

TEST(parser, variadic_param_list)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int a, ...) { return a; }\n", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->sig.params), 1);
    EXPECT_TRUE(fn->sig.is_variadic);
    arena_free(a);
}

TEST(parser, variadic_multi_named)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(char a, int b, ...) { return b; }\n", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->sig.params), 2);
    EXPECT_TRUE(fn->sig.is_variadic);
    arena_free(a);
}

TEST(parser, variadic_empty_params_rejected)
{
    /* C11 §6.7.6.3p8: `...` must follow at least one named parameter. */
    EXPECT_PARSE_FAIL("int f(...) { return 0; }");
}

TEST(parser, variadic_not_last_rejected)
{
    EXPECT_PARSE_FAIL("int f(int a, ..., int b) { return a; }");
}

TEST(parser, fixed_param_list_not_variadic)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(void) { return 0; }\n", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->sig.params), 0);
    EXPECT_FALSE(fn->sig.is_variadic);
    arena_free(a);
}

TEST(parser, builtin_va_list_is_builtin_typedef)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(void) {\n"
                            "    __builtin_va_list ap;\n"
                            "    return 0;\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_TRUE(vd->type->kind == TYPE_ARRAY);
    EXPECT_TRUE(type_array_elem(vd->type) == type_array_elem(type_va_list()));
    arena_free(a);
}

TEST(parser, raw_va_list_not_a_type_before_shim)
{
    EXPECT_PARSE_FAIL("int main(void) {\n"
                      "    va_list ap;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(parser, builtin_va_arg_special_form)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int a, ...) {\n"
                            "    __builtin_va_list ap;\n"
                            "    __builtin_va_start(ap, a);\n"
                            "    return __builtin_va_arg(ap, int);\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 2));
    ASTVaArgExpr *va = ast_as(ASTVaArgExpr, ret->expr);
    EXPECT_EQ(va->base.kind, AST_VA_ARG_EXPR);
    EXPECT_TRUE(va->type == type_int());
    EXPECT_TRUE(ast_as(ASTIdent, va->ap)->name != NULL);
    arena_free(a);
}

TEST(parser, builtin_va_arg_missing_type_rejected)
{
    EXPECT_PARSE_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, a);\n"
                      "    return __builtin_va_arg(ap);\n"
                      "}\n");
}

TEST(parser, builtin_va_arg_non_type_second_arg_rejected)
{
    EXPECT_PARSE_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, a);\n"
                      "    return __builtin_va_arg(ap, a);\n"
                      "}\n");
}

TEST(parser, builtin_va_arg_pointer_type)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int f(int a, ...) {\n"
                            "    __builtin_va_list ap;\n"
                            "    __builtin_va_start(ap, a);\n"
                            "    return *(char *)__builtin_va_arg(ap, char *);\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 2));
    ASTUnaryExpr *deref = ast_as(ASTUnaryExpr, ret->expr);
    ASTCastExpr *cast = ast_as(ASTCastExpr, deref->operand);
    EXPECT_EQ(cast->target_type->kind, TYPE_PTR);
    ASTVaArgExpr *va = ast_as(ASTVaArgExpr, cast->operand);
    EXPECT_EQ(va->base.kind, AST_VA_ARG_EXPR);
    EXPECT_TRUE(type_is_ptr(va->type));
    arena_free(a);
}

TEST(parser, fn_ptr_declarator)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int (*fp)(int);", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_STR_EQ(vd->name, "fp");
    EXPECT_EQ(vd->type->kind, TYPE_PTR);
    Type *pointee = type_deref(vd->type);
    EXPECT_EQ(pointee->kind, TYPE_FUNC);
    EXPECT_TRUE(pointee->func.ret == type_int());
    EXPECT_EQ(vec_size(pointee->func.params), 1);
    arena_free(a);
}

TEST(parser, fn_ptr_typedef)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("typedef int (*Op)(int a, int b);", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTTypedefDecl *td = ast_as(ASTTypedefDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_STR_EQ(td->name, "Op");
    EXPECT_EQ(td->type->kind, TYPE_PTR);
    EXPECT_EQ(type_deref(td->type)->kind, TYPE_FUNC);
    arena_free(a);
}

TEST(parser, fn_returning_pointer_stays_function)
{
    /* `int *f(int)` is a function returning `int *`, not a pointer to function. */
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int *f(int a) { return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fd = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(fd->sig.ret_type->kind, TYPE_PTR);
    EXPECT_EQ(type_sizeof(fd->sig.ret_type), 8);
    arena_free(a);
}

TEST(parser, fn_ptr_param)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int apply(int (*fn)(int), int x);", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDecl *fd = ast_as(ASTFuncDecl, (ASTNode *) vec_get(prog->decls, 0));
    ASTVarDecl *p0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fd->sig.params, 0));
    EXPECT_EQ(p0->type->kind, TYPE_PTR);
    EXPECT_EQ(type_deref(p0->type)->kind, TYPE_FUNC);
    arena_free(a);
}

TEST(parser, abstract_fn_ptr_cast_typename)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { int (*fp)(int) = (int (*)(int)) 0; return 0; }", a);
    EXPECT_NOTNULL(ast);
    arena_free(a);
}

TEST(parser, generic_selection_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    return _Generic(1, int: 2, double: 3, default: 4);\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ret->expr->kind, AST_GENERIC_SELECTION);
    ASTGenericSelection *gs = ast_as(ASTGenericSelection, ret->expr);
    EXPECT_EQ(vec_size(gs->assocs), 2);
    EXPECT_NOTNULL(gs->default_expr);
    GenericAssoc *first = (GenericAssoc *) vec_get(gs->assocs, 0);
    EXPECT_EQ(first->type->kind, TYPE_INT);
    arena_free(a);
}

TEST(parser, generic_selection_requires_comma)
{
    EXPECT_PARSE_FAIL("int main(void) { return _Generic(1 int: 2); }");
}

TEST(parser, generic_selection_requires_colon)
{
    EXPECT_PARSE_FAIL("int main(void) { return _Generic(1, int 2); }");
}

TEST(parser, generic_selection_duplicate_default_fails)
{
    EXPECT_PARSE_FAIL("int main(void) { return _Generic(1, default: 2, default: 3); }");
}

TEST(parser, while_loop_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { while (1) { } return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTWhileStmt *ws = ast_as(ASTWhileStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ws->base.kind, AST_WHILE_STMT);
    EXPECT_EQ(ws->cond->kind, AST_INT_LITERAL);
    EXPECT_NOTNULL(ws->body);
    arena_free(a);
}

TEST(parser, do_while_loop_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { do { } while (1); return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTDoWhileStmt *dw = ast_as(ASTDoWhileStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(dw->base.kind, AST_DO_WHILE_STMT);
    EXPECT_EQ(dw->cond->kind, AST_INT_LITERAL);
    EXPECT_NOTNULL(dw->body);
    arena_free(a);
}

TEST(parser, for_loop_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { for (int i = 0; i < 3; i++) { } return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTForStmt *fs = ast_as(ASTForStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(fs->base.kind, AST_FOR_STMT);
    EXPECT_EQ(fs->init->kind, AST_VAR_DECL);
    EXPECT_EQ(fs->cond->kind, AST_BINARY_EXPR);
    EXPECT_EQ(fs->post->kind, AST_INCDEC_EXPR);
    EXPECT_NOTNULL(fs->body);
    arena_free(a);
}

TEST(parser, break_stmt_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { while (1) { break; } return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTWhileStmt *ws = ast_as(ASTWhileStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCompoundStmt *loop_body = ast_as(ASTCompoundStmt, ws->body);
    EXPECT_EQ(((ASTNode *) vec_get(loop_body->stmts, 0))->kind, AST_BREAK_STMT);
    arena_free(a);
}

TEST(parser, continue_stmt_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { while (1) { continue; } return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTWhileStmt *ws = ast_as(ASTWhileStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCompoundStmt *loop_body = ast_as(ASTCompoundStmt, ws->body);
    EXPECT_EQ(((ASTNode *) vec_get(loop_body->stmts, 0))->kind, AST_CONTINUE_STMT);
    arena_free(a);
}

TEST(parser, goto_stmt_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { goto done; done: return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTGotoStmt *gs = ast_as(ASTGotoStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(gs->base.kind, AST_GOTO_STMT);
    EXPECT_STR_EQ(gs->label, "done");
    arena_free(a);
}

TEST(parser, label_stmt_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { done: return 0; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTLabelStmt *ls = ast_as(ASTLabelStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(ls->base.kind, AST_LABEL_STMT);
    EXPECT_STR_EQ(ls->label, "done");
    EXPECT_EQ(ls->stmt->kind, AST_RETURN_STMT);
    arena_free(a);
}

TEST(parser, goto_missing_label_rejected)
{
    EXPECT_PARSE_FAIL("int main(void) { goto ; }");
}

TEST(parser, do_missing_while_rejected)
{
    EXPECT_PARSE_FAIL("int main(void) { do { } return 0; }");
}

TEST(parser, switch_stmt_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) {\n"
                            "    switch (x) {\n"
                            "    case 1:\n"
                            "        return 0;\n"
                            "    default:\n"
                            "        return 1;\n"
                            "    }\n"
                            "}\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTSwitchStmt *sw = ast_as(ASTSwitchStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_EQ(sw->base.kind, AST_SWITCH_STMT);
    EXPECT_EQ(sw->cond->kind, AST_IDENT);
    ASTCompoundStmt *sw_body = ast_as(ASTCompoundStmt, sw->body);
    EXPECT_EQ(vec_size(sw_body->stmts), 2);
    EXPECT_EQ(((ASTNode *) vec_get(sw_body->stmts, 0))->kind, AST_CASE_STMT);
    EXPECT_EQ(((ASTNode *) vec_get(sw_body->stmts, 1))->kind, AST_DEFAULT_STMT);
    arena_free(a);
}

TEST(parser, ternary_expr_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return a ? b : c; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTTernaryExpr *te = ast_as(ASTTernaryExpr, ret->expr);
    EXPECT_EQ(te->base.kind, AST_TERNARY_EXPR);
    EXPECT_EQ(te->cond->kind, AST_IDENT);
    EXPECT_EQ(te->then_expr->kind, AST_IDENT);
    EXPECT_EQ(te->else_expr->kind, AST_IDENT);
    arena_free(a);
}

TEST(parser, char_literal_expr)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("int main(void) { return 'A'; }", a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTIntLiteral *lit = ast_as(ASTIntLiteral, ret->expr);
    EXPECT_EQ(lit->base.kind, AST_INT_LITERAL);
    EXPECT_EQ(lit->value, 65);
    arena_free(a);
}

TEST(parser, union_definition_shape)
{
    Arena *a = arena_new();
    ASTNode *ast = tc_parse("union U {\n"
                            "    int i;\n"
                            "    char c;\n"
                            "};\n"
                            "int main(void) { return 0; }\n",
                            a);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTStructDecl *sd = ast_as(ASTStructDecl, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(sd->base.kind, AST_STRUCT_DECL);
    EXPECT_TRUE(sd->is_union);
    EXPECT_EQ(vec_size(sd->fields), 2);
    arena_free(a);
}
