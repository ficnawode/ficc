#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <unistd.h>

/* Compiles each value-path fixture on both backends and requires the same
   exit code from the linked program at -O0 and -O1. */

static unsigned int backend_seq;

static void backend_path(char *buf, size_t sz, const char *backend, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_22e_%u_%s.%s", backend_seq++, backend, ext);
}

static void backend_write_src(char *out, size_t sz, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_22e_%u_src.c", backend_seq++);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void backend_run_one(const char *src_path, const char *backend, const char *level,
                            int expected)
{
    char obj[256], bin[256], cmd[2048];
    backend_path(obj, sizeof(obj), backend, "o");
    backend_path(bin, sizeof(bin), backend, "bin");

    snprintf(cmd, sizeof(cmd), "%s -backend %s %s -c %s -o %s >/dev/null 2>&1", FICC_BIN, backend,
             level, src_path, obj);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);
    if (rc != 0)
    {
        return;
    }
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj, bin, bin);
    rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, expected);

    unlink(obj);
    unlink(bin);
}

static void backend_run_level(const char *src, const char *level, int expected)
{
    char src_path[256];
    backend_write_src(src_path, sizeof(src_path), src);
    backend_run_one(src_path, "stack", level, expected);
    backend_run_one(src_path, "linear", level, expected);
    unlink(src_path);
}

static void backend_run(const char *src, int expected)
{
    backend_run_level(src, "-O0", expected);
    backend_run_level(src, "-O1", expected);
}

/* The in-process oracle: same fixture through both backends via the testdriver. */
static void backend_run_inproc(const char *src, int expected)
{
    tc_set_codegen_backend(CG_STACK);
    EXPECT_EQ(tc_run_elf(src), expected);
    tc_set_codegen_backend(CG_LINEAR);
    EXPECT_EQ(tc_run_elf(src), expected);
    tc_set_codegen_backend(CG_STACK);
}

TEST(backend, return_literal)
{
    backend_run("int main(void) { return 40 + 2; }\n", 42);
}

TEST(backend, local_arithmetic)
{
    backend_run("int main(void) { int a = 20; int b = 3; return a * b + a / b + a % b; }\n", 68);
}

TEST(backend, division_and_remainder)
{
    backend_run("int main(void) { int a = 100; int b = 7; return a / b * 7 + a % b; }\n", 100);
}

TEST(backend, unsigned_division)
{
    backend_run("int main(void) { unsigned a = 4000000000u; unsigned b = 7u;\n"
                "                return (int) (a / b); }\n",
                219);
}

TEST(backend, shifts_signed_and_unsigned)
{
    backend_run("int main(void) { int a = -16; int b = a >> 2;\n"
                "                int c = (int) ((unsigned) a >> 1); return b + c; }\n",
                244);
}

TEST(backend, char_truncation_preserves_sign)
{
    backend_run("int main(void) { char c = (char) 200; return c; }\n", 200);
}

TEST(backend, bitops)
{
    backend_run("int main(void) { int x = 42; int y = x & 0xFF; y = y | 0x100;\n"
                "                y = y ^ 0x100; return y; }\n",
                42);
}

TEST(backend, comparisons)
{
    backend_run("int main(void) { unsigned a = 100; unsigned b = 200;\n"
                "                return (a < b) + (a > b) * 10 + 42; }\n",
                43);
}

TEST(backend, long_arithmetic)
{
    backend_run("int main(void) { long a = 1234567890123L; long b = 7; return (int) (a % b); }\n",
                1);
}

TEST(backend, negative_long_immediate)
{
    backend_run("int main(void) { long x = -1L; return (int) x; }\n", 255);
}

TEST(backend, long_immediate_shift_keeps_sign_bits)
{
    backend_run("int main(void) { unsigned long x = 0xFFFFFFFFFFFFFFFFUL;\n"
                "                return (int) (x >> 60); }\n",
                15);
}

TEST(backend, narrow_unsigned_and_signed_loads)
{
    backend_run("int main(void) { unsigned char c = 200; int x = c + 56;\n"
                "                signed char d = -2; return x + d * 3 + 8; }\n",
                2);
}

TEST(backend, array_alloca_gep_load_store)
{
    backend_run("int main(void) { int a[4]; a[0] = 1; a[1] = 2; a[2] = 3; a[3] = 36;\n"
                "                return a[0] + a[1] + a[2] + a[3]; }\n",
                42);
}

TEST(backend, global_load_store)
{
    backend_run("int g;\nint main(void) { g = 21; int x = g; return x * 2; }\n", 42);
}

TEST(backend, pointer_arithmetic)
{
    backend_run("int main(void) { int a[3]; a[0] = 10; a[1] = 20; a[2] = 12;\n"
                "                int *p = a; return p[0] + p[1] + p[2]; }\n",
                42);
}

TEST(backend, parameters_compile)
{
    backend_run("int f(int x, int y) { return x * 3 + y; }\n"
                "int main(void) { return 42; }\n",
                42);
}

TEST(backend, register_pressure_spills)
{
    backend_run("int g0; int g1; int g2; int g3; int g4; int g5; int g6; int g7; int g8;\n"
                "int g9;\n"
                "int main(void) { g0 = 0; g1 = 1; g2 = 2; g3 = 3; g4 = 4; g5 = 5; g6 = 6;\n"
                "                g7 = 7; g8 = 8; g9 = 9;\n"
                "                return g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9; }\n",
                45);
}

TEST(backend, in_process_oracle)
{
    backend_run_inproc("int main(void) { int a = 6; int b = 7; int c[1]; c[0] = a * b;\n"
                       "                       return c[0]; }\n",
                       42);
}
