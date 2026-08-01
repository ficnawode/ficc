#include "harness.h"
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "util/arena.h"

static ASTNode *check_from_source(const char *src, Arena *arena)
{
    u64 count;
    Token *tokens = lex("<test>", src, arena, &count);
    if (!tokens)
        return NULL;
    ASTNode *ast = parse(tokens, count, arena);
    if (!ast)
        return NULL;
    return semantic_check(ast, arena);
}

TEST(semantic, ok_single_func)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int main(void) { return 42; }", a);
    EXPECT_TRUE(ast != NULL);
    arena_free(a);
}

TEST(semantic, duplicate_function)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int f(void) { return 1; } int f(void) { return 2; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, undeclared_var)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int main(void) { return x; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, redeclaration_local)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int main(void) { int x; int x; return x; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, undeclared_function_call)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int main(void) { return foo(); }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, wrong_arity)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int foo(int a) { return a; } int main(void) { return foo(); }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, param_used_ok)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int add(int a, int b) { return a + b; }", a);
    EXPECT_TRUE(ast != NULL);
    arena_free(a);
}

TEST(semantic, void_return_with_value)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("void f(void) { return 1; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, nonvoid_return_without_value)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int f(void) { return; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, assignment_undeclared)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int main(void) { x = 5; return x; }", a);
    EXPECT_TRUE(ast == NULL);
    arena_free(a);
}

TEST(semantic, call_across_functions)
{
    Arena *a = arena_new();
    ASTNode *ast = check_from_source("int add(int a, int b) { return a + b; } int main(void) { return add(1, 2); }", a);
    EXPECT_TRUE(ast != NULL);
    arena_free(a);
}
