#include "codegen.h"
#include "elf.h"
#include "harness.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_shell(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
        return -1;
    return WEXITSTATUS(rc);
}

TEST(phase2, interp_arith)
{
    const char *src =
        "int add(int a, int b) {\n"
        "    return a + b;\n"
        "}\n"
        "int sub(int a, int b) {\n"
        "    return a - b;\n"
        "}\n"
        "int main(void) {\n"
        "    int x;\n"
        "    x = add(10, 3);\n"
        "    x = sub(x, 2);\n"
        "    return x * 5;\n"
        "}\n";
    Arena *arena = arena_new();
    u64 tok_count;
    Token *tokens = lex("<test>", src, arena, &tok_count);
    EXPECT_TRUE(tokens != NULL);
    ASTNode *ast = parse(tokens, tok_count, arena);
    EXPECT_TRUE(ast != NULL);
    ast = semantic_check(ast, arena);
    EXPECT_TRUE(ast != NULL);
    Module *mod = ir_build_module(ast, arena);
    EXPECT_TRUE(mod != NULL);
    i64 result = ir_interp_run(mod);
    EXPECT_EQ(result, 55);
    arena_free(arena);
}

TEST(phase2, elf_arith)
{
    const char *src =
        "int add(int a, int b) {\n"
        "    return a + b;\n"
        "}\n"
        "int sub(int a, int b) {\n"
        "    return a - b;\n"
        "}\n"
        "int main(void) {\n"
        "    int x;\n"
        "    x = add(10, 3);\n"
        "    x = sub(x, 2);\n"
        "    return x * 5;\n"
        "}\n";
    Arena *arena = arena_new();
    u64 tok_count;
    Token *tokens = lex("<test>", src, arena, &tok_count);
    ASTNode *ast = parse(tokens, tok_count, arena);
    ast = semantic_check(ast, arena);
    Module *mod = ir_build_module(ast, arena);

    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    const char *obj_path = "/tmp/ficc_phase2_test.o";
    const char *bin_path = "/tmp/ficc_phase2_test";
    elf_write(cm, obj_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    EXPECT_EQ(run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    int exit_code = run_shell(cmd);
    EXPECT_EQ(exit_code, 55);

    unlink(obj_path);
    unlink(bin_path);

    arena_free(arena);
}
