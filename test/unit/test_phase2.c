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
    {
        return -1;
    }
    return WEXITSTATUS(rc);
}

static int arena_compile_run(const char *src, Arena *arena, const char *obj_path,
                             const char *bin_path)
{
    LexResult lexed = lex("<test>", src, arena);
    if (!lexed.tokens)
    {
        return -1;
    }
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
    if (!ast)
    {
        return -1;
    }
    ast = semantic_check(ast, arena);
    if (!ast)
    {
        return -1;
    }
    Module *mod = ir_build_module(ast, arena);
    if (!mod)
    {
        return -1;
    }

    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    elf_write(cm, obj_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    int rc = run_shell(cmd);
    if (rc != 0)
    {
        return rc;
    }

    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    rc = run_shell(cmd);

    unlink(obj_path);
    unlink(bin_path);
    return rc;
}

TEST(phase2, elf_div_immediate_divisor)
{
    /* Regression: the machine SDIV path previously loaded an immediate divisor
       through %eax, clobbering the dividend in edx:eax (text and machine paths
       diverged). 10 / 5 must be 2, not 1. */
    Arena *arena = arena_new();
    int exit_code = arena_compile_run("int main(void) { return 10 / 5; }", arena,
                                      "/tmp/ficc_div_imm.o", "/tmp/ficc_div_imm");
    EXPECT_EQ(exit_code, 2);
    arena_free(arena);
}

TEST(phase2, elf_rem_immediate_divisor)
{
    /* Same clobber bug in SREM: 10 %% 3 must be 1. */
    const char *src = "int main(void) { return 10 % 3; }";
    Arena *arena = arena_new();
    int exit_code = arena_compile_run(src, arena, "/tmp/ficc_rem_imm.o", "/tmp/ficc_rem_imm");
    EXPECT_EQ(exit_code, 1);
    arena_free(arena);
}

TEST(phase2, elf_div_mixed_operands)
{
    /* Both operands from stack slots (register divisor) must round-trip too. */
    const char *src = "int main(void) { int a = 37; int b = 7; return a / b + a % b; }";
    Arena *arena = arena_new();
    int exit_code = arena_compile_run(src, arena, "/tmp/ficc_div_mixed.o", "/tmp/ficc_div_mixed");
    EXPECT_EQ(exit_code, 7);
    arena_free(arena);
}

TEST(phase2, interp_arith)
{
    const char *src = "int add(int a, int b) {\n"
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
    LexResult lexed = lex("<test>", src, arena);
    EXPECT_TRUE(lexed.tokens != NULL);
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
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
    const char *src = "int add(int a, int b) {\n"
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
    LexResult lexed = lex("<test>", src, arena);
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
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
