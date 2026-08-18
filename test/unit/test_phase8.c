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
#include "util/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_shell_cmd(const char *cmd);

static IrModule *build_from_source(const char *src, Arena *arena)
{
    type_reset();
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
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj_path, bin_path,
             bin_path);
    return run_shell_cmd(cmd);
}

static int run_shell_cmd(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
    {
        return -1;
    }
    return WEXITSTATUS(rc);
}

TEST(phase8, interp_data_bss)
{
    const char *src = "int counter = 5;\n"
                      "static int acc;\n"
                      "int main(void) { counter = counter + 2; "
                      "acc = acc + counter + 1; return acc; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 8);
    arena_free(arena);
}

TEST(phase8, interp_accumulate_across_calls)
{
    const char *src = "static int total;\n"
                      "int add(int v) { total = total + v; return total; }\n"
                      "int main(void) { add(3); add(4); return add(5); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 12);
    arena_free(arena);
}

TEST(phase8, interp_record_array_globals)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "struct Pt origin;\n"
                      "int arr[3];\n"
                      "int main(void) { origin.x = 4; origin.y = 5; arr[1] = 7; "
                      "return origin.x + origin.y + arr[1]; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 16);
    arena_free(arena);
}

TEST(phase8, interp_global_enum)
{
    const char *src = "enum Color { RED, GREEN, BLUE };\n"
                      "enum Color state;\n"
                      "int main(void) { state = BLUE; return state + RED; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 2);
    arena_free(arena);
}

TEST(phase8, interp_addr_of_scalar_global)
{
    const char *src = "int g = 9;\n"
                      "int *p;\n"
                      "int main(void) { p = &g; return *p; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 9);
    arena_free(arena);
}

TEST(phase8, interp_pointer_global_string)
{
    const char *src = "char *msg = \"hello\";\n"
                      "int main(void) { return msg[1] + msg[4] + msg[0]; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 'e' + 'o' + 'h');
    arena_free(arena);
}

TEST(phase8, interp_extern_is_zero)
{
    const char *src = "extern int e;\n"
                      "int main(void) { return e; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 0);
    arena_free(arena);
}

TEST(phase8, interp_tentative_defs_merge)
{
    const char *src = "int x;\n"
                      "int x;\n"
                      "int main(void) { x = 9; return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 9);
    arena_free(arena);
}

TEST(phase8, interp_zero_init)
{
    const char *src = "int z;\n"
                      "long l;\n"
                      "char c;\n"
                      "int main(void) { l = 1; if (z != 0) return 1; "
                      "if (c != 0) return 2; if (l != 1) return 3; return 0; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 0);
    arena_free(arena);
}

TEST(phase8, elf_data_bss)
{
    const char *src = "int counter = 5;\n"
                      "static int acc;\n"
                      "int main(void) { counter = counter + 2; "
                      "acc = acc + counter + 1; return acc; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_db.o", "/tmp/ficc_p8_db");
    EXPECT_EQ(rc, 8);
    unlink("/tmp/ficc_p8_db.o");
    unlink("/tmp/ficc_p8_db");
    arena_free(arena);
}

TEST(phase8, elf_pointer_global_string)
{
    const char *src = "char *msg = \"hello\";\n"
                      "int main(void) { return msg[1] + msg[4] + msg[0]; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_ps.o", "/tmp/ficc_p8_ps");
    /* exit code truncates to the low byte: 'e'+'o'+'h' = 316 & 0xFF = 60 */
    EXPECT_EQ(rc, 60);
    unlink("/tmp/ficc_p8_ps.o");
    unlink("/tmp/ficc_p8_ps");
    arena_free(arena);
}

TEST(phase8, elf_extern_two_tu)
{
    Arena *arena = arena_new();
    const char *other_c = "int shared = 40;\n"
                          "int other_ext = 5;\n";
    FILE *f = fopen("/tmp/ficc_p8_other.c", "w");
    EXPECT_TRUE(f != NULL);
    fwrite(other_c, 1, strlen(other_c), f);
    fclose(f);
    EXPECT_EQ(run_shell_cmd("gcc -c /tmp/ficc_p8_other.c -o /tmp/ficc_p8_other.o >/dev/null 2>&1"),
              0);

    const char *src = "extern int shared;\n"
                      "extern int other_ext;\n"
                      "int main(void) { shared = shared + 2; return shared + other_ext; }\n";
    IrModule *mod = build_from_source(src, arena);
    EXPECT_TRUE(mod != NULL);
    CodegenModule *cm = mod ? codegen_ir_to_machine(mod, arena) : NULL;
    EXPECT_TRUE(cm != NULL);
    if (cm)
    {
        elf_write(cm, "/tmp/ficc_p8_ext.o");
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "gcc -no-pie /tmp/ficc_p8_ext.o /tmp/ficc_p8_other.o -o /tmp/ficc_p8_ext "
                 ">/dev/null 2>&1 && /tmp/ficc_p8_ext");
        EXPECT_EQ(run_shell_cmd(cmd), 47);
    }
    unlink("/tmp/ficc_p8_other.c");
    unlink("/tmp/ficc_p8_other.o");
    unlink("/tmp/ficc_p8_ext.o");
    unlink("/tmp/ficc_p8_ext");
    arena_free(arena);
}

TEST(phase8, negative_global_nonconst_init)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int v = 1 + 2 + main;\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, negative_global_redefine)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int x = 1;\n"
                                  "int x = 2;\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, interp_block_static)
{
    const char *src = "int counter(void) { static int n = 5; n = n + 1; return n; }\n"
                      "int main(void) { return counter() + counter(); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 13);
    arena_free(arena);
}

TEST(phase8, elf_block_static)
{
    const char *src = "int counter(void) { static int n = 5; n = n + 1; return n; }\n"
                      "int main(void) { return counter() + counter(); }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_bstat.o", "/tmp/ficc_p8_bstat");
    EXPECT_EQ(rc, 13);
    unlink("/tmp/ficc_p8_bstat.o");
    unlink("/tmp/ficc_p8_bstat");
    arena_free(arena);
}

TEST(phase8, interp_block_static_distinct)
{
    const char *src = "int f(void) { static int n; n = n + 2; return n; }\n"
                      "int g(void) { static int n; n = n + 3; return n; }\n"
                      "int main(void) { return f() + f() + g(); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 9);
    arena_free(arena);
}

TEST(phase8, local_shadows_global)
{
    const char *src =
        "int counter = 100;\n"
        "int main(void) { int counter = 7; counter = counter + 1; return counter; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 8);
    arena_free(arena);
}

TEST(phase8, interp_static_function)
{
    const char *src = "static int helper(int x) { return x * 2; }\n"
                      "int visible(int x) { return helper(x) + 1; }\n"
                      "int main(void) { return visible(5); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 11);
    arena_free(arena);
}

TEST(phase8, elf_static_function_binding)
{
    const char *src = "static int helper(int x) { return x * 2; }\n"
                      "int visible(int x) { return helper(x) + 1; }\n"
                      "int main(void) { return visible(5); }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_sfunc.o", "/tmp/ficc_p8_sfunc");
    EXPECT_EQ(rc, 11);
    unlink("/tmp/ficc_p8_sfunc.o");
    unlink("/tmp/ficc_p8_sfunc");
    arena_free(arena);
}

TEST(phase8, extern_with_init_is_definition)
{
    const char *src = "extern int x = 5;\n"
                      "int main(void) { return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 5);
    int rc = run_elf(src, arena, "/tmp/ficc_p8_extinit.o", "/tmp/ficc_p8_extinit");
    EXPECT_EQ(rc, 5);
    unlink("/tmp/ficc_p8_extinit.o");
    unlink("/tmp/ficc_p8_extinit");
    arena_free(arena);
}

TEST(phase8, negative_linkage_mixing)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int x;\n"
                                  "static int x;\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    EXPECT_TRUE(build_from_source("static int f(void) { return 0; }\n"
                                  "int f(void) { return 1; }\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, negative_static_nonconst_init)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { static int x = 1 + main; return x; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, negative_aggregate_init)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "struct Pt p = 5;\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, negative_string_init_non_char_ptr)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int *p = \"hi\";\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, negative_incomplete_global)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct S;\n"
                                  "struct S s;\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase8, interp_block_extern)
{
    const char *src = "int shared = 10;\n"
                      "int main(void) { extern int shared; shared = shared + 5; return shared; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 15);
    arena_free(arena);
}

TEST(phase8, elf_block_extern_shared)
{
    const char *src = "int n;\n"
                      "int a(void) { extern int n; n = n + 1; return n; }\n"
                      "int b(void) { extern int n; n = n + 2; return n; }\n"
                      "int main(void) { return a() + a() + b(); }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_blext.o", "/tmp/ficc_p8_blext");
    EXPECT_EQ(rc, 7);
    unlink("/tmp/ficc_p8_blext.o");
    unlink("/tmp/ficc_p8_blext");
    arena_free(arena);
}

TEST(phase8, negative_block_extern_with_init)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void){ extern int q = 5; return q; }\n", arena) ==
                NULL);
    arena_free(arena);
}

TEST(phase8, interp_nested_shadow)
{
    const char *src = "int main(void) { int x = 1; { int x = 2; x = x + 1; } return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase8, interp_shadow_param_nested)
{
    const char *src = "int f(int x) { { int x = 5; x = x + 1; } return x; }\n"
                      "int main(void) { return f(10); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 10);
    arena_free(arena);
}

TEST(phase8, interp_shadow_across_branches)
{
    const char *src = "int main(void) { int x = 1; "
                      "if (x) { int x = 2; x = x + 1; } else { int x = 3; x = x + 1; } "
                      "return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase8, interp_shadow_different_type)
{
    const char *src = "int main(void) { int x = 10; { char *x = \"ab\"; x = x + 1; } "
                      "return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 10);
    arena_free(arena);
}

TEST(phase8, interp_shadow_in_loop)
{
    const char *src = "int main(void) { int total = 0; int i = 0; "
                      "while (i < 5) { int total = 7; total = total + 1; i = i + 1; } "
                      "return total; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 0);
    arena_free(arena);
}

TEST(phase8, interp_local_shadows_block_static)
{
    const char *src = "int counter(void) { static int n = 5; "
                      "{ int n = 50; n = n + 1; } n = n + 1; return n; }\n"
                      "int main(void) { return counter() + counter(); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 13);
    arena_free(arena);
}

TEST(phase8, elf_nested_shadow)
{
    const char *src = "int main(void) { int x = 1; { int x = 2; x = x + 1; } return x; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_sh.o", "/tmp/ficc_p8_sh");
    EXPECT_EQ(rc, 1);
    unlink("/tmp/ficc_p8_sh.o");
    unlink("/tmp/ficc_p8_sh");
    arena_free(arena);
}

TEST(phase8, elf_shadow_in_loop)
{
    const char *src = "int main(void) { int total = 0; int i = 0; "
                      "while (i < 5) { int total = 7; total = total + 1; i = i + 1; } "
                      "return total; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p8_shloop.o", "/tmp/ficc_p8_shloop");
    EXPECT_EQ(rc, 0);
    unlink("/tmp/ficc_p8_shloop.o");
    unlink("/tmp/ficc_p8_shloop");
    arena_free(arena);
}
