#include "codegen.h"
#include "elf.h"
#include "harness.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "type.h"
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

static IrModule *build_from_source(const char *src, Arena *arena)
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
    IrModule *mod = build_from_source(src, arena);
    if (!mod)
    {
        return 0;
    }
    return ir_interp_run(mod);
}

static int run_elf(const char *src, Arena *arena, const char *obj_path, const char *bin_path)
{
    IrModule *mod = build_from_source(src, arena);
    if (!mod)
    {
        return -1;
    }
    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    if (!cm)
    {
        return -1;
    }
    elf_write(cm, obj_path);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s", obj_path, bin_path);
    if (run_shell(cmd) != 0)
    {
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    return run_shell(cmd);
}

TEST(phase6, type_ptr_width)
{
    Type *p = type_ptr(type_int());
    EXPECT_EQ(p->width, 64);
    EXPECT_EQ(p->align, 8);
    EXPECT_EQ(p->size, 8);
}

TEST(phase6, type_ptr_interning)
{
    Type *a = type_ptr(type_int());
    Type *b = type_ptr(type_int());
    EXPECT_TRUE(a == b);
}

TEST(phase6, type_array_basic)
{
    Type *arr = type_array(type_int(), 10);
    EXPECT_EQ(arr->size, 40);
    EXPECT_EQ(type_array_elem(arr), type_int());
}

TEST(phase6, type_decay_deref)
{
    Type *p = type_ptr(type_char());
    EXPECT_EQ(type_deref(p), type_char());
}

TEST(phase6, interp_simple)
{
    const char *src = "int main(void) { return 42; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 42);
    arena_free(arena);
}

TEST(phase6, interp_sizeof_int)
{
    const char *src = "int main(void) { return sizeof(int); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 4);
    arena_free(arena);
}

TEST(phase6, interp_sizeof_char)
{
    const char *src = "int main(void) { return sizeof(char); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase6, interp_string_literal)
{
    const char *src = "int main(void) { char *s = \"hello\"; return s[0]; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 'h');
    arena_free(arena);
}

TEST(phase6, elf_golden_pointers)
{
    const char *src = "int main(void) {\n"
                      "    if (sizeof(int) != 4) return 1;\n"
                      "    if (sizeof(char) != 1) return 2;\n"
                      "    return 0;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase6_golden.o", "/tmp/ficc_phase6_golden");
    EXPECT_EQ(exit_code, 0);
    unlink("/tmp/ficc_phase6_golden.o");
    unlink("/tmp/ficc_phase6_golden");
    arena_free(arena);
}
