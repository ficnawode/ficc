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

TEST(phase6, interp_multiple_strings)
{
    const char *src =
        "int main(void) {\n"
        "    char *a = \"foo\";\n"
        "    char *b = \"bar\";\n"
        "    return a[0] + a[1] + a[2] + b[0] + b[1] + b[2];\n"
        "}\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 'f' + 'o' + 'o' + 'b' + 'a' + 'r');
    arena_free(arena);
}

TEST(phase6, elf_string_literal)
{
    const char *src = "int main(void) {\n"
                      "    char *s = \"ok\";\n"
                      "    if (s[0] != 111) return 1;\n"
                      "    if (s[1] != 107) return 2;\n"
                      "    return 42;\n"
                      "}\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase6_str.o", "/tmp/ficc_phase6_str");
    EXPECT_EQ(exit_code, 42);
    unlink("/tmp/ficc_phase6_str.o");
    unlink("/tmp/ficc_phase6_str");
    arena_free(arena);
}

TEST(phase6, interp_array_subscript)
{
    const char *src =
        "int main(void) { int arr[3]; arr[0] = 10; arr[1] = 20; arr[2] = 30; "
        "return arr[0] + arr[1] + arr[2]; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 60);
    arena_free(arena);
}

TEST(phase6, interp_array_write_read)
{
    const char *src = "int main(void) { int arr[4]; int i; "
                      "for (i = 0; i < 4; i = i + 1) arr[i] = i * 7; return arr[3]; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 21);
    arena_free(arena);
}

TEST(phase6, interp_ptr_arithmetic)
{
    const char *src = "int main(void) { int arr[3]; arr[0] = 5; arr[1] = 6; arr[2] = 7; "
                      "int *p = arr; return *(p + 2); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 7);
    arena_free(arena);
}

TEST(phase6, interp_ptr_neg_offset)
{
    const char *src = "int main(void) { int arr[3]; arr[0] = 5; arr[1] = 6; arr[2] = 7; "
                      "int *p = &arr[2]; return *(p - 1); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 6);
    arena_free(arena);
}

TEST(phase6, interp_addr_deref)
{
    const char *src = "int main(void) { int arr[2]; arr[0] = 9; return *(&arr[0]); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 9);
    arena_free(arena);
}

TEST(phase6, interp_array_param)
{
    const char *src = "int sum(int a[], int n) { int s = 0; int i; "
                      "for (i = 0; i < n; i = i + 1) s = s + a[i]; return s; } "
                      "int main(void) { int x[3]; x[0] = 1; x[1] = 2; x[2] = 3; "
                      "return sum(x, 3); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 6);
    arena_free(arena);
}

TEST(phase6, interp_void_ptr_assign)
{
    const char *src = "int main(void) { void *v; int *p; v = p; p = v; return sizeof(v); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 8);
    arena_free(arena);
}

TEST(phase6, interp_string_pass_to_func)
{
    const char *src =
        "int first(char *s) { return s[0]; } int main(void) { return first(\"hi\"); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 'h');
    arena_free(arena);
}

TEST(phase6, interp_null_deref_trap)
{
    const char *src = "int main(void) { int *p = 0; *p = 42; return 0; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase6, elf_array_read_write)
{
    const char *src = "int main(void) { int arr[3]; arr[0] = 4; arr[1] = 5; arr[2] = 6; "
                      "return arr[0] + arr[1] + arr[2]; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase6_arr.o", "/tmp/ficc_phase6_arr");
    EXPECT_EQ(exit_code, 15);
    unlink("/tmp/ficc_phase6_arr.o");
    unlink("/tmp/ficc_phase6_arr");
    arena_free(arena);
}

TEST(phase6, elf_ptr_arithmetic)
{
    const char *src = "int main(void) { int arr[3]; arr[0] = 10; arr[1] = 20; arr[2] = 30; "
                      "int *p = arr; return *(p + 2); }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase6_pa.o", "/tmp/ficc_phase6_pa");
    EXPECT_EQ(exit_code, 30);
    unlink("/tmp/ficc_phase6_pa.o");
    unlink("/tmp/ficc_phase6_pa");
    arena_free(arena);
}

TEST(phase6, elf_array_param)
{
    const char *src = "int sum(int a[], int n) { int s = 0; int i; "
                      "for (i = 0; i < n; i = i + 1) s = s + a[i]; return s; } "
                      "int main(void) { int x[3]; x[0] = 1; x[1] = 2; x[2] = 3; "
                      "return sum(x, 3); }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase6_ap.o", "/tmp/ficc_phase6_ap");
    EXPECT_EQ(exit_code, 6);
    unlink("/tmp/ficc_phase6_ap.o");
    unlink("/tmp/ficc_phase6_ap");
    arena_free(arena);
}

TEST(phase6, interp_string_loop_sum)
{
    const char *src = "int main(void) { char *s = \"hello\"; int i = 0; int n = 0; "
                      "while (s[i] != 0) { n = n + s[i]; i = i + 1; } return n; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 'h' + 'e' + 'l' + 'l' + 'o');
    arena_free(arena);
}

TEST(phase6, elf_string_loop)
{
    const char *src = "int main(void) { char *s = \"hello\"; int i = 0; "
                      "while (s[i] != 0) i = i + 1; return i; }\n";
    Arena *arena = arena_new();
    int exit_code =
        run_elf(src, arena, "/tmp/ficc_phase6_ss.o", "/tmp/ficc_phase6_ss");
    EXPECT_EQ(exit_code, 5);
    unlink("/tmp/ficc_phase6_ss.o");
    unlink("/tmp/ficc_phase6_ss");
    arena_free(arena);
}

TEST(phase6, negative_deref_non_pointer)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { int x; return *x; }\n", arena) == NULL);
    arena_free(arena);
}

TEST(phase6, negative_addr_non_lvalue)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { return &42; }\n", arena) == NULL);
    arena_free(arena);
}

TEST(phase6, negative_sizeof_void)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { return sizeof(void); }\n", arena) == NULL);
    arena_free(arena);
}

TEST(phase6, negative_incompatible_ptr_assign)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { int *p; char *q; p = q; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}
