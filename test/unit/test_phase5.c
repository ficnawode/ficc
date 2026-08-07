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
        fprintf(stderr, "[phase5] build_from_source failed for: %s\n", src);
        return 0;
    }
    return ir_interp_run(mod);
}

static int run_elf(const char *src, Arena *arena, const char *obj_path, const char *bin_path)
{
    IrModule *mod = build_from_source(src, arena);
    if (!mod)
    {
        fprintf(stderr, "[phase5] build_from_source failed for: %s\n", src);
        return -1;
    }

    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    elf_write(cm, obj_path);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc %s -o %s", obj_path, bin_path);
    EXPECT_EQ(run_shell(cmd), 0);

    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    return run_shell(cmd);
}

/* Type system tests */

TEST(phase5, type_widths)
{
    EXPECT_EQ(type_char()->width, 8);
    EXPECT_EQ(type_short()->width, 16);
    EXPECT_EQ(type_int()->width, 32);
    EXPECT_EQ(type_long()->width, 64);
    EXPECT_EQ(type_llong()->width, 64);
    EXPECT_EQ(type_uchar()->width, 8);
    EXPECT_EQ(type_ushort()->width, 16);
    EXPECT_EQ(type_uint()->width, 32);
    EXPECT_EQ(type_ulong()->width, 64);
    EXPECT_EQ(type_ullong()->width, 64);
}

TEST(phase5, type_align)
{
    EXPECT_EQ(type_char()->align, 1);
    EXPECT_EQ(type_short()->align, 2);
    EXPECT_EQ(type_int()->align, 4);
    EXPECT_EQ(type_long()->align, 8);
}

TEST(phase5, type_size)
{
    EXPECT_EQ(type_char()->size, 1);
    EXPECT_EQ(type_short()->size, 2);
    EXPECT_EQ(type_int()->size, 4);
    EXPECT_EQ(type_long()->size, 8);
}

TEST(phase5, type_is_signed)
{
    EXPECT_TRUE(type_is_signed(type_char()));
    EXPECT_TRUE(type_is_signed(type_short()));
    EXPECT_TRUE(type_is_signed(type_int()));
    EXPECT_TRUE(type_is_signed(type_long()));
    EXPECT_TRUE(type_is_signed(type_llong()));
    EXPECT_TRUE(!type_is_signed(type_uchar()));
    EXPECT_TRUE(!type_is_signed(type_uint()));
    EXPECT_TRUE(!type_is_signed(type_ulong()));
}

TEST(phase5, type_is_integer)
{
    EXPECT_TRUE(type_is_integer(type_char()));
    EXPECT_TRUE(type_is_integer(type_uchar()));
    EXPECT_TRUE(!type_is_integer(type_void()));
}

TEST(phase5, type_rank)
{
    EXPECT_TRUE(type_rank(type_char()) < type_rank(type_short()));
    EXPECT_TRUE(type_rank(type_short()) < type_rank(type_int()));
    EXPECT_TRUE(type_rank(type_int()) < type_rank(type_long()));
    EXPECT_TRUE(type_rank(type_uchar()) == type_rank(type_char()));
    EXPECT_TRUE(type_rank(type_uint()) == type_rank(type_int()));
}

TEST(phase5, promote_char)
{
    EXPECT_EQ(type_promote(type_char())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_short())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_uchar())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_ushort())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_int())->kind, TYPE_INT);
    EXPECT_EQ(type_promote(type_long())->kind, TYPE_LONG);
}

TEST(phase5, common_type_same)
{
    EXPECT_EQ(type_common(type_int(), type_int())->kind, TYPE_INT);
    EXPECT_EQ(type_common(type_long(), type_long())->kind, TYPE_LONG);
}

TEST(phase5, common_type_mixed_width)
{
    EXPECT_EQ(type_common(type_char(), type_int())->kind, TYPE_INT);
    EXPECT_EQ(type_common(type_int(), type_long())->kind, TYPE_LONG);
    EXPECT_EQ(type_common(type_char(), type_short())->kind, TYPE_INT);
}

TEST(phase5, common_type_signedness)
{
    EXPECT_EQ(type_common(type_uint(), type_int())->kind, TYPE_UINT);
    EXPECT_EQ(type_common(type_ulong(), type_int())->kind, TYPE_ULONG);
    EXPECT_EQ(type_common(type_ulong(), type_long())->kind, TYPE_ULONG);
}

/* Interpreter tests for promotion */

TEST(phase5, interp_char_promotion)
{
    const char *src = "int f(char a, char b) { return a + b; }\n"
                      "int main(void) { return f(100, 50); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 150);
    arena_free(arena);
}

TEST(phase5, interp_uchar_promotion)
{
    const char *src = "int f(unsigned char a, unsigned char b) { return a + b; }\n"
                      "int main(void) { return f(200, 50); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 250);
    arena_free(arena);
}

TEST(phase5, interp_short_promotion)
{
    const char *src = "int f(short a, short b) { return a + b; }\n"
                      "int main(void) { return f(1000, 2000); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 3000);
    arena_free(arena);
}

TEST(phase5, interp_char_truncation_assign)
{
    const char *src = "int main(void) { char x = 300; return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 44);
    arena_free(arena);
}

TEST(phase5, interp_short_truncation_assign)
{
    const char *src = "int main(void) { short x = 70000; return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 4464);
    arena_free(arena);
}

TEST(phase5, interp_uchar_truncation_assign)
{
    const char *src = "int main(void) { unsigned char x = 300; return x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 44);
    arena_free(arena);
}

TEST(phase5, interp_unsigned_comparison)
{
    const char *src = "int main(void) { unsigned a = 5; unsigned b = 10; return a < b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase5, interp_unsigned_gt_comparison)
{
    const char *src = "int main(void) { unsigned a = 20; unsigned b = 10; return a > b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase5, interp_signed_lt_negative)
{
    const char *src = "int main(void) { int a = -5; int b = 10; return a < b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase5, interp_shift_left)
{
    const char *src = "int main(void) { return 1 << 3; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 8);
    arena_free(arena);
}

TEST(phase5, interp_shift_right_signed)
{
    const char *src = "int main(void) { return (-8) >> 1; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), -4);
    arena_free(arena);
}

TEST(phase5, interp_shift_right_unsigned)
{
    const char *src = "int main(void) { unsigned a = 8; return a >> 1; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 4);
    arena_free(arena);
}

TEST(phase5, interp_unsigned_div)
{
    const char *src = "int main(void) { unsigned a = 10; unsigned b = 3; return a / b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 3);
    arena_free(arena);
}

TEST(phase5, interp_unsigned_rem)
{
    const char *src = "int main(void) { unsigned a = 10; unsigned b = 3; return a % b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 1);
    arena_free(arena);
}

TEST(phase5, interp_mixed_signedness_comparison)
{
    const char *src = "int main(void) { int a = -5; unsigned b = 10; return a < b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 0);
    arena_free(arena);
}

TEST(phase5, interp_long_add)
{
    const char *src = "long add(long a, long b) { return a + b; }\n"
                      "int main(void) { return add(1000000, 2000000); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 3000000);
    arena_free(arena);
}

TEST(phase5, interp_char_param_overflow)
{
    const char *src = "int f(char a, char b) { return a + b; }\n"
                      "int main(void) { return f(100, 200); }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 44);
    arena_free(arena);
}

TEST(phase5, interp_bitwise_not)
{
    const char *src = "int main(void) { char a = 0; return ~a; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), -1);
    arena_free(arena);
}

TEST(phase5, interp_ternary_mixed_types)
{
    const char *src = "int main(void) { char a = 10; short b = 20; return 1 ? a : b; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 10);
    arena_free(arena);
}

/* ELF round-trip tests */

TEST(phase5, elf_char_promotion)
{
    const char *src = "int f(char a, char b) { return a + b; }\n"
                      "int main(void) { return f(100, 50); }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_cpromo.o", "/tmp/ficc_phase5_cpromo");
    EXPECT_EQ(exit_code, 150);
    unlink("/tmp/ficc_phase5_cpromo.o");
    unlink("/tmp/ficc_phase5_cpromo");
    arena_free(arena);
}

TEST(phase5, elf_uchar_promotion)
{
    const char *src = "int f(unsigned char a, unsigned char b) { return a + b; }\n"
                      "int main(void) { return f(200, 50); }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_ucpromo.o", "/tmp/ficc_phase5_ucpromo");
    EXPECT_EQ(exit_code, 250);
    unlink("/tmp/ficc_phase5_ucpromo.o");
    unlink("/tmp/ficc_phase5_ucpromo");
    arena_free(arena);
}

TEST(phase5, elf_signed_shift)
{
    const char *src = "int main(void) { return (-8) >> 1; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_ssh.o", "/tmp/ficc_phase5_ssh");
    EXPECT_EQ((exit_code & 0xFF), 252);
    unlink("/tmp/ficc_phase5_ssh.o");
    unlink("/tmp/ficc_phase5_ssh");
    arena_free(arena);
}

TEST(phase5, elf_unsigned_shift)
{
    const char *src = "int main(void) { unsigned a = 8; return a >> 1; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_ush.o", "/tmp/ficc_phase5_ush");
    EXPECT_EQ(exit_code, 4);
    unlink("/tmp/ficc_phase5_ush.o");
    unlink("/tmp/ficc_phase5_ush");
    arena_free(arena);
}

TEST(phase5, elf_trunc_char)
{
    const char *src = "int main(void) { char x = 300; return x; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_tc.o", "/tmp/ficc_phase5_tc");
    EXPECT_EQ(exit_code, 44);
    unlink("/tmp/ficc_phase5_tc.o");
    unlink("/tmp/ficc_phase5_tc");
    arena_free(arena);
}

TEST(phase5, elf_unsigned_comparison)
{
    const char *src = "int main(void) { unsigned a = 5; unsigned b = 10; return a < b; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_ucmp.o", "/tmp/ficc_phase5_ucmp");
    EXPECT_EQ(exit_code, 1);
    unlink("/tmp/ficc_phase5_ucmp.o");
    unlink("/tmp/ficc_phase5_ucmp");
    arena_free(arena);
}

TEST(phase5, elf_unsigned_div)
{
    const char *src = "int main(void) { unsigned a = 10; unsigned b = 3; return a / b; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_udiv.o", "/tmp/ficc_phase5_udiv");
    EXPECT_EQ(exit_code, 3);
    unlink("/tmp/ficc_phase5_udiv.o");
    unlink("/tmp/ficc_phase5_udiv");
    arena_free(arena);
}

TEST(phase5, elf_long_param)
{
    const char *src = "int use_long(long x) { return x == 42L; }\n"
                      "int main(void) { return use_long(42L); }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_lparam.o", "/tmp/ficc_phase5_lparam");
    EXPECT_EQ(exit_code, 1);
    unlink("/tmp/ficc_phase5_lparam.o");
    unlink("/tmp/ficc_phase5_lparam");
    arena_free(arena);
}

TEST(phase5, elf_long_return)
{
    const char *src = "long ret_long(void) { return 99L; }\n"
                      "int main(void) { long r = ret_long(); return r; }\n";
    Arena *arena = arena_new();
    int exit_code = run_elf(src, arena, "/tmp/ficc_phase5_lret.o", "/tmp/ficc_phase5_lret");
    EXPECT_EQ(exit_code, 99);
    unlink("/tmp/ficc_phase5_lret.o");
    unlink("/tmp/ficc_phase5_lret");
    arena_free(arena);
}
