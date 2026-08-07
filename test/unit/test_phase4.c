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

static Module *build_from_source(const char *src, Arena *arena)
{
    LexResult lexed = lex("<test>", src, arena);
    if (!lexed.tokens)
    {
        return NULL;
    }
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
    if (!ast)
    {
        return NULL;
    }
    ast = semantic_check(ast, arena);
    if (!ast)
    {
        return NULL;
    }
    return ir_build_module(ast, arena);
}

static i64 run_interp(const char *src, Arena *arena)
{
    Module *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);
    return ir_interp_run(mod);
}

static int run_elf(const char *src, Arena *arena, const char *obj_path, const char *bin_path)
{
    Module *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);

    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    elf_write(cm, obj_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    EXPECT_EQ(run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    return run_shell(cmd);
}

TEST(phase4, interp_while)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i = 0;\n"
                      "    while (i < 5) {\n"
                      "        s = s + i;\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 10);
    arena_free(arena);
}

TEST(phase4, elf_while)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i = 0;\n"
                      "    while (i < 5) {\n"
                      "        s = s + i;\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_while.o", "/tmp/ficc_phase4_while");
    EXPECT_EQ(exit_code, 10);
    arena_free(arena);
}

TEST(phase4, interp_for)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i;\n"
                      "    for (i = 0; i < 5; i = i + 1) {\n"
                      "        s = s + i;\n"
                      "    }\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 10);
    arena_free(arena);
}

TEST(phase4, elf_for)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i;\n"
                      "    for (i = 0; i < 5; i = i + 1) {\n"
                      "        s = s + i;\n"
                      "    }\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_for.o", "/tmp/ficc_phase4_for");
    EXPECT_EQ(exit_code, 10);
    arena_free(arena);
}

TEST(phase4, interp_do_while)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i = 0;\n"
                      "    do {\n"
                      "        s = s + i;\n"
                      "        i = i + 1;\n"
                      "    } while (i < 5);\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 10);
    arena_free(arena);
}

TEST(phase4, elf_do_while)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i = 0;\n"
                      "    do {\n"
                      "        s = s + i;\n"
                      "        i = i + 1;\n"
                      "    } while (i < 5);\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_do_while.o", "/tmp/ficc_phase4_do_while");
    EXPECT_EQ(exit_code, 10);
    arena_free(arena);
}

TEST(phase4, interp_break_continue)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i = 0;\n"
                      "    while (i < 10) {\n"
                      "        if (i >= 5) {\n"
                      "            break;\n"
                      "        }\n"
                      "        if (i == 2) {\n"
                      "            i = i + 1;\n"
                      "            continue;\n"
                      "        }\n"
                      "        s = s + i;\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    /* 0 + 1 + 3 + 4 = 8 (skip 2 because of continue) */
    EXPECT_EQ(result, 8);
    arena_free(arena);
}

TEST(phase4, interp_logical_and_or)
{
    const char *src = "int main(void) {\n"
                      "    int a = 1;\n"
                      "    int b = 0;\n"
                      "    int c = 0;\n"
                      "    if (a && b) {\n"
                      "        c = 1;\n"
                      "    }\n"
                      "    if (a || b) {\n"
                      "        c = c + 10;\n"
                      "    }\n"
                      "    if (b && a) {\n"
                      "        c = c + 100;\n"
                      "    }\n"
                      "    return c;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 10);
    arena_free(arena);
}

TEST(phase4, interp_ternary)
{
    const char *src = "int main(void) {\n"
                      "    int a = 5;\n"
                      "    int b = 3;\n"
                      "    int c = a > b ? 7 : 2;\n"
                      "    return c;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 7);
    arena_free(arena);
}

TEST(phase4, interp_comparisons)
{
    const char *src = "int main(void) {\n"
                      "    int r = 0;\n"
                      "    if (1 < 2) r = r + 1;\n"
                      "    if (2 > 1) r = r + 2;\n"
                      "    if (1 <= 1) r = r + 4;\n"
                      "    if (2 >= 2) r = r + 8;\n"
                      "    if (1 == 1) r = r + 16;\n"
                      "    if (1 != 2) r = r + 32;\n"
                      "    return r;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 63);
    arena_free(arena);
}

TEST(phase4, interp_bitwise)
{
    const char *src = "int main(void) {\n"
                      "    int r = 0;\n"
                      "    r = (5 & 3);\n"
                      "    r = r | 8;\n"
                      "    r = r ^ 2;\n"
                      "    r = r << 1;\n"
                      "    r = r >> 1;\n"
                      "    return r;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    /* 5 & 3 = 1; 1 | 8 = 9; 9 ^ 2 = 11; 11 << 1 = 22; 22 >> 1 = 11 */
    EXPECT_EQ(result, 11);
    arena_free(arena);
}

TEST(phase4, interp_goto)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "    goto skip;\n"
                      "    x = 10;\n"
                      "skip:\n"
                      "    x = x + 5;\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 5);
    arena_free(arena);
}

TEST(phase4, elf_goto)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "    goto skip;\n"
                      "    x = 10;\n"
                      "skip:\n"
                      "    x = x + 5;\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_goto.o", "/tmp/ficc_phase4_goto");
    EXPECT_EQ(exit_code, 5);
    arena_free(arena);
}

TEST(phase4, elf_break_continue)
{
    const char *src = "int main(void) {\n"
                      "    int s = 0;\n"
                      "    int i = 0;\n"
                      "    while (i < 10) {\n"
                      "        if (i >= 5) {\n"
                      "            break;\n"
                      "        }\n"
                      "        if (i == 2) {\n"
                      "            i = i + 1;\n"
                      "            continue;\n"
                      "        }\n"
                      "        s = s + i;\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase4_break_continue.o", "/tmp/ficc_phase4_break_continue");
    EXPECT_EQ(exit_code, 8);
    arena_free(arena);
}

TEST(phase4, elf_logical_and_or)
{
    const char *src = "int main(void) {\n"
                      "    int a = 1;\n"
                      "    int b = 0;\n"
                      "    int c = 0;\n"
                      "    if (a && b) {\n"
                      "        c = 1;\n"
                      "    }\n"
                      "    if (a || b) {\n"
                      "        c = c + 10;\n"
                      "    }\n"
                      "    if (b && a) {\n"
                      "        c = c + 100;\n"
                      "    }\n"
                      "    return c;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_logical.o", "/tmp/ficc_phase4_logical");
    EXPECT_EQ(exit_code, 10);
    arena_free(arena);
}

TEST(phase4, elf_ternary)
{
    const char *src = "int main(void) {\n"
                      "    int a = 5;\n"
                      "    int b = 3;\n"
                      "    int c = a > b ? 7 : 2;\n"
                      "    return c;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_ternary.o", "/tmp/ficc_phase4_ternary");
    EXPECT_EQ(exit_code, 7);
    arena_free(arena);
}

TEST(phase4, elf_comparisons)
{
    const char *src = "int main(void) {\n"
                      "    int r = 0;\n"
                      "    if (1 < 2) r = r + 1;\n"
                      "    if (2 > 1) r = r + 2;\n"
                      "    if (1 <= 1) r = r + 4;\n"
                      "    if (2 >= 2) r = r + 8;\n"
                      "    if (1 == 1) r = r + 16;\n"
                      "    if (1 != 2) r = r + 32;\n"
                      "    return r;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase4_comparisons.o", "/tmp/ficc_phase4_comparisons");
    EXPECT_EQ(exit_code, 63);
    arena_free(arena);
}

TEST(phase4, elf_bitwise)
{
    const char *src = "int main(void) {\n"
                      "    int r = 0;\n"
                      "    r = (5 & 3);\n"
                      "    r = r | 8;\n"
                      "    r = r ^ 2;\n"
                      "    r = r << 1;\n"
                      "    r = r >> 1;\n"
                      "    return r;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase4_bitwise.o", "/tmp/ficc_phase4_bitwise");
    EXPECT_EQ(exit_code, 11);
    arena_free(arena);
}

TEST(phase4, interp_goto_backward)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "loop:\n"
                      "    x = x + 1;\n"
                      "    if (x < 5) {\n"
                      "        goto loop;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 5);
    arena_free(arena);
}

TEST(phase4, elf_goto_backward)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "loop:\n"
                      "    x = x + 1;\n"
                      "    if (x < 5) {\n"
                      "        goto loop;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase4_goto_backward.o", "/tmp/ficc_phase4_goto_backward");
    EXPECT_EQ(exit_code, 5);
    arena_free(arena);
}

TEST(phase4, interp_goto_out_of_loop)
{
    const char *src = "int main(void) {\n"
                      "    int i = 0;\n"
                      "    while (i < 10) {\n"
                      "        if (i == 4) {\n"
                      "            goto done;\n"
                      "        }\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    i = 99;\n"
                      "done:\n"
                      "    return i;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 4);
    arena_free(arena);
}

TEST(phase4, elf_goto_out_of_loop)
{
    const char *src = "int main(void) {\n"
                      "    int i = 0;\n"
                      "    while (i < 10) {\n"
                      "        if (i == 4) {\n"
                      "            goto done;\n"
                      "        }\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    i = 99;\n"
                      "done:\n"
                      "    return i;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase4_goto_out_loop.o", "/tmp/ficc_phase4_goto_out_loop");
    EXPECT_EQ(exit_code, 4);
    arena_free(arena);
}

TEST(phase4, interp_for_empty_init)
{
    const char *src = "int main(void) {\n"
                      "    int c = 0;\n"
                      "    for (; c < 3; c = c + 1) {\n"
                      "        c = c;\n"
                      "    }\n"
                      "    return c;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 3);
    arena_free(arena);
}

TEST(phase4, elf_for_empty_init)
{
    const char *src = "int main(void) {\n"
                      "    int c = 0;\n"
                      "    for (; c < 3; c = c + 1) {\n"
                      "        c = c;\n"
                      "    }\n"
                      "    return c;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase4_for_empty_init.o", "/tmp/ficc_phase4_for_empty_init");
    EXPECT_EQ(exit_code, 3);
    arena_free(arena);
}

TEST(phase4, interp_for_no_clauses)
{
    const char *src = "int main(void) {\n"
                      "    int i = 0;\n"
                      "    for (;;) {\n"
                      "        i = i + 1;\n"
                      "        if (i >= 5) break;\n"
                      "    }\n"
                      "    return i;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 5);
    arena_free(arena);
}

TEST(phase4, interp_do_while_continue)
{
    const char *src = "int main(void) {\n"
                      "    int i = 0;\n"
                      "    int s = 0;\n"
                      "    do {\n"
                      "        s = s + 1;\n"
                      "        if (i == 2) {\n"
                      "            i = i + 1;\n"
                      "            continue;\n"
                      "        }\n"
                      "        i = i + 1;\n"
                      "    } while (i < 5);\n"
                      "    return s;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 5);
    arena_free(arena);
}

TEST(phase4, interp_var_assigned_in_one_branch)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "    int i = 0;\n"
                      "    while (i < 3) {\n"
                      "        if (i == 1) {\n"
                      "            x = 42;\n"
                      "        }\n"
                      "        i = i + 1;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    i64 result = run_interp(src, arena);
    EXPECT_EQ(result, 42);
    arena_free(arena);
}

TEST(phase4, semantic_break_outside_loop)
{
    Arena *arena = arena_new();
    Module *mod = build_from_source("int main(void) { break; return 0; }", arena);
    EXPECT_TRUE(mod == NULL);
    arena_free(arena);
}

TEST(phase4, semantic_continue_outside_loop)
{
    Arena *arena = arena_new();
    Module *mod = build_from_source("int main(void) { continue; return 0; }", arena);
    EXPECT_TRUE(mod == NULL);
    arena_free(arena);
}

TEST(phase4, semantic_goto_undefined_label)
{
    Arena *arena = arena_new();
    Module *mod = build_from_source("int main(void) { goto nope; return 0; }", arena);
    EXPECT_TRUE(mod == NULL);
    arena_free(arena);
}

TEST(phase4, semantic_duplicate_label)
{
    Arena *arena = arena_new();
    Module *mod = build_from_source("int main(void) { goto a; a: goto a; a: return 0; }", arena);
    EXPECT_TRUE(mod == NULL);
    arena_free(arena);
}

TEST(phase4, elf_call_7_args)
{
    Arena *arena = arena_new();
    int exit_code =
        run_elf("int sum7(int a, int b, int c, int d, int e, int f, int g)\n"
                "{\n"
                "    return a + b + c + d + e + f + g;\n"
                "}\n"
                "int main(void) { return sum7(1, 2, 3, 4, 5, 6, 7) == 28 ? 0 : 1; }",
                arena, "/tmp/ficc_phase4_args7.o", "/tmp/ficc_phase4_args7");
    EXPECT_EQ(exit_code, 0);
    arena_free(arena);
}

TEST(phase4, interp_call_7_args)
{
    Arena *arena = arena_new();
    i64 result = run_interp("int sum7(int a, int b, int c, int d, int e, int f, int g)\n"
                            "{\n"
                            "    return a + b + c + d + e + f + g;\n"
                            "}\n"
                            "int main(void) { return sum7(1, 2, 3, 4, 5, 6, 7) == 28 ? 0 : 1; }",
                            arena);
    EXPECT_EQ(result, 0);
    arena_free(arena);
}
