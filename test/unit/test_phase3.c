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

static Module *build_from_source(const char *src, Arena *arena)
{
    LexResult lexed = lex("<test>", src, arena);
    if (!lexed.tokens)
        return NULL;
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
    if (!ast)
        return NULL;
    ast = semantic_check(ast, arena);
    if (!ast)
        return NULL;
    return ir_build_module(ast, arena);
}

TEST(phase3, interp_if_then)
{
    const char *src = "int main(void) {\n"
                      "    int x = 5;\n"
                      "    if (x) {\n"
                      "        x = 10;\n"
                      "    } else {\n"
                      "        x = 20;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    Module *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);
    i64 result = ir_interp_run(mod);
    EXPECT_EQ(result, 10);
    arena_free(arena);
}

TEST(phase3, interp_if_else)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "    if (x) {\n"
                      "        x = 10;\n"
                      "    } else {\n"
                      "        x = 20;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    Module *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);
    i64 result = ir_interp_run(mod);
    EXPECT_EQ(result, 20);
    arena_free(arena);
}

TEST(phase3, elf_if_then)
{
    const char *src = "int main(void) {\n"
                      "    int x = 5;\n"
                      "    if (x) {\n"
                      "        x = 10;\n"
                      "    } else {\n"
                      "        x = 20;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    Module *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);

    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    const char *obj_path = "/tmp/ficc_phase3_test.o";
    const char *bin_path = "/tmp/ficc_phase3_test";
    elf_write(cm, obj_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    EXPECT_EQ(run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    int exit_code = run_shell(cmd);
    EXPECT_EQ(exit_code, 10);

    unlink(obj_path);
    unlink(bin_path);

    arena_free(arena);
}

TEST(phase3, elf_if_else)
{
    const char *src = "int main(void) {\n"
                      "    int x = 0;\n"
                      "    if (x) {\n"
                      "        x = 10;\n"
                      "    } else {\n"
                      "        x = 20;\n"
                      "    }\n"
                      "    return x;\n"
                      "}\n";
    Arena *arena = arena_new();
    Module *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);

    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    const char *obj_path = "/tmp/ficc_phase3_test2.o";
    const char *bin_path = "/tmp/ficc_phase3_test2";
    elf_write(cm, obj_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    EXPECT_EQ(run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    int exit_code = run_shell(cmd);
    EXPECT_EQ(exit_code, 20);

    unlink(obj_path);
    unlink(bin_path);

    arena_free(arena);
}
