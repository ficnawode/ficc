#include "codegen.h"
#include "elf.h"
#include "harness.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "util/arena.h"
#include "util/sbuf.h"
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

TEST(phase1, interp_return42)
{
    const char *src = "int main(void) { return 42; }";
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
    EXPECT_EQ(result, 42);
    arena_free(arena);
}

TEST(phase1, elf_return42)
{
    const char *src = "int main(void) { return 42; }";
    Arena *arena = arena_new();
    LexResult lexed = lex("<test>", src, arena);
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
    ast = semantic_check(ast, arena);
    Module *mod = ir_build_module(ast, arena);

    /* Generate ELF object */
    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    const char *obj_path = "/tmp/ficc_phase1_test.o";
    const char *bin_path = "/tmp/ficc_phase1_test";
    elf_write(cm, obj_path);

    /* Link with gcc */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    EXPECT_EQ(run_shell(cmd), 0);

    /* Run binary */
    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    int exit_code = run_shell(cmd);
    EXPECT_EQ(exit_code, 42);

    /* Cleanup */
    unlink(obj_path);
    unlink(bin_path);

    arena_free(arena);
}
