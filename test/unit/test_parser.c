#include "harness.h"
#include "lexer.h"
#include "parser.h"
#include "util/arena.h"
#include <string.h>

static ASTNode *parse_string(const char *src, Arena *arena)
{
    LexResult lexed = lex("<test>", src, arena);
    if (!lexed.tokens)
    {
        return NULL;
    }
    return parse(lexed.tokens, lexed.count, arena);
}

TEST(parser, minimal_program)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 42; }", a);
    EXPECT_TRUE(ast != NULL);
    EXPECT_EQ(ast->kind, AST_PROGRAM);
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
    EXPECT_EQ(ast->kind, AST_PROGRAM);
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
    EXPECT_EQ(ast->kind, AST_PROGRAM);
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

    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_TRUE(prog != NULL);
    EXPECT_EQ(vec_size(prog->decls), 1);

    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_TRUE(fn != NULL);
    EXPECT_EQ(fn->base.kind, AST_FUNC_DEF);
    EXPECT_TRUE(strcmp(fn->name, "main") == 0);
    EXPECT_TRUE(fn->ret_type == type_int());
    EXPECT_TRUE(fn->body != NULL);

    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_TRUE(body != NULL);
    EXPECT_EQ(vec_size(body->stmts), 1);

    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_TRUE(ret != NULL);
    EXPECT_TRUE(ret->expr != NULL);

    ASTIntLiteral *lit = ast_as(ASTIntLiteral, ret->expr);
    EXPECT_TRUE(lit != NULL);
    EXPECT_EQ(lit->value, 42);

    arena_free(a);
}

TEST(parser, multiple_functions)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int f(void) { return 1; } int g(void) { return 2; }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    EXPECT_EQ(vec_size(prog->decls), 2);
    ASTFuncDef *f = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTFuncDef *g = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 1));
    EXPECT_TRUE(strcmp(f->name, "f") == 0);
    EXPECT_TRUE(strcmp(g->name, "g") == 0);
    arena_free(a);
}

TEST(parser, params_with_names)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int add(int a, int b) { return a + b; }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    EXPECT_EQ(vec_size(fn->params), 2);
    ASTVarDecl *p0 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->params, 0));
    ASTVarDecl *p1 = ast_as(ASTVarDecl, (ASTNode *) vec_get(fn->params, 1));
    EXPECT_TRUE(strcmp(p0->name, "a") == 0);
    EXPECT_TRUE(strcmp(p1->name, "b") == 0);
    arena_free(a);
}

TEST(parser, local_var_decl)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { int x; return x; }", a);
    EXPECT_TRUE(ast != NULL);
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
    ASTNode *ast = parse_string("int main(void) { int x = 5; return x; }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_TRUE(vd->init != NULL);
    EXPECT_EQ(vd->init->kind, AST_INT_LITERAL);
    arena_free(a);
}

TEST(parser, binary_precedence)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { return 1 + 2 * 3; }", a);
    EXPECT_TRUE(ast != NULL);
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
    ASTNode *ast = parse_string("int main(void) { return -42; }", a);
    EXPECT_TRUE(ast != NULL);
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
    ASTNode *ast = parse_string("int main(void) { return foo(1, 2); }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTReturnStmt *ret = ast_as(ASTReturnStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTCallExpr *call = ast_as(ASTCallExpr, ret->expr);
    EXPECT_TRUE(strcmp(call->callee, "foo") == 0);
    EXPECT_EQ(vec_size(call->args), 2);
    arena_free(a);
}

TEST(parser, assignment)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { int x; x = 5; return x; }", a);
    EXPECT_TRUE(ast != NULL);
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
    ASTNode *ast = parse_string("int main(void) { if (1) { return 10; } else { return 20; } }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    EXPECT_EQ(vec_size(body->stmts), 1);
    ASTIfStmt *is = ast_as(ASTIfStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_TRUE(is->cond != NULL);
    EXPECT_TRUE(is->then_branch != NULL);
    EXPECT_TRUE(is->else_branch != NULL);
    arena_free(a);
}

TEST(parser, if_no_else)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { if (1) return 10; }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTIfStmt *is = ast_as(ASTIfStmt, (ASTNode *) vec_get(body->stmts, 0));
    EXPECT_TRUE(is->cond != NULL);
    EXPECT_TRUE(is->then_branch != NULL);
    EXPECT_TRUE(is->else_branch == NULL);
    arena_free(a);
}

TEST(parser, dangling_else)
{
    Arena *a = arena_new();
    ASTNode *ast = parse_string("int main(void) { if (1) if (0) return 10; else return 20; }", a);
    EXPECT_TRUE(ast != NULL);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTFuncDef *fn = ast_as(ASTFuncDef, (ASTNode *) vec_get(prog->decls, 0));
    ASTCompoundStmt *body = ast_as(ASTCompoundStmt, fn->body);
    ASTIfStmt *outer = ast_as(ASTIfStmt, (ASTNode *) vec_get(body->stmts, 0));
    ASTIfStmt *inner = ast_as(ASTIfStmt, outer->then_branch);
    EXPECT_TRUE(inner->else_branch != NULL);
    arena_free(a);
}
