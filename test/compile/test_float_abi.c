#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <unistd.h>

/* glibc printf interop, ELF-only: variadic doubles ride xmm and %al reports the count. */

static int drv_seq;

static void drv_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_fpabi_%d_%s.%s", drv_seq++, tag, ext);
}

static void drv_write_src(char *out, size_t sz, const char *tag, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_fpabi_%d_%s.c", drv_seq++, tag);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

TEST(float_abi, printf_double_variadic_via_glibc)
{
    char src[256], obj[256], bin[256];
    const char *prog = "int printf(const char *fmt, ...);\n"
                       "int main(void) {\n"
                       "    printf(\"%%.2f\\n\", 1.75);\n"
                       "    printf(\"%%.2f %%d\\n\", 2.5, 9);\n"
                       "    printf(\"%%d %%.2f\\n\", 7, 3.5);\n"
                       "    return 42;\n"
                       "}\n";
    drv_write_src(src, sizeof(src), "printfsrc", prog);
    drv_path(obj, sizeof(obj), "printf_obj", "o");
    drv_path(bin, sizeof(bin), "printf_bin", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "%s -c %s -o %s >/dev/null 2>&1 && gcc -no-pie %s -o %s "
             ">/dev/null 2>&1 && %s",
             FICC_BIN, src, obj, obj, bin, bin);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 42);

    char *paths[] = {src, obj, bin};
    for (size_t i = 0; i < 3; i++)
    {
        unlink(paths[i]);
    }
}

TEST(float_abi, printf_many_doubles_overflow)
{
    char src[256], obj[256], bin[256];
    const char *prog = "int printf(const char *fmt, ...);\n"
                       "int main(void) {\n"
                       "    printf(\"%%.2f %%.2f %%.2f %%.2f %%.2f %%.2f %%.2f %%.2f %%.2f "
                       "%%.2f\\n\",\n"
                       "           1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0);\n"
                       "    return 42;\n"
                       "}\n";
    drv_write_src(src, sizeof(src), "manyf", prog);
    drv_path(obj, sizeof(obj), "manyf_obj", "o");
    drv_path(bin, sizeof(bin), "manyf_bin", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "%s -c %s -o %s >/dev/null 2>&1 && gcc -no-pie %s -o %s "
             ">/dev/null 2>&1 && %s",
             FICC_BIN, src, obj, obj, bin, bin);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 42);

    char *paths[] = {src, obj, bin};
    for (size_t i = 0; i < 3; i++)
    {
        unlink(paths[i]);
    }
}

TEST(float_abi, printf_long_double_via_glibc)
{
    /* A stack-passed ld variadic argument that glibc's va_arg walk reads back. */
    char src[256], obj[256], bin[256];
    const char *prog = "int printf(const char *fmt, ...);\n"
                       "int main(void) {\n"
                       "    long double x = 1.75L;\n"
                       "    printf(\"%%.2Lf\\n\", x);\n"
                       "    printf(\"%%.2Lf %%.2f %%.2Lf %%d\\n\", 2.5L, 3.25, 4.5L, 9);\n"
                       "    printf(\"%%d %%.2Lf\\n\", 7, 3.5L);\n"
                       "    return 42;\n"
                       "}\n";
    drv_write_src(src, sizeof(src), "plf", prog);
    drv_path(obj, sizeof(obj), "plf_obj", "o");
    drv_path(bin, sizeof(bin), "plf_bin", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "%s -c %s -o %s >/dev/null 2>&1 && gcc -no-pie %s -o %s "
             ">/dev/null 2>&1 && %s",
             FICC_BIN, src, obj, obj, bin, bin);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 42);

    char *paths[] = {src, obj, bin};
    for (size_t i = 0; i < 3; i++)
    {
        unlink(paths[i]);
    }
}

TEST(float_abi, long_double_libm_call)
{
    /* A `long double` libm call: stack arg in, %st0 return out. */
    char src[256], obj[256], bin[256];
    const char *prog = "long double floorl(long double);\n"
                       "int main(void) {\n"
                       "    long double r = floorl(3.75L);\n"
                       "    if (r != 3.0L) return 1;\n"
                       "    if (floorl(-1.25L) != -2.0L) return 2;\n"
                       "    long double d = floorl(100.0L);\n"
                       "    if (d != 100.0L) return 3;\n"
                       "    return 42;\n"
                       "}\n";
    drv_write_src(src, sizeof(src), "floorl", prog);
    drv_path(obj, sizeof(obj), "floorl_obj", "o");
    drv_path(bin, sizeof(bin), "floorl_bin", "bin");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "%s -c %s -o %s >/dev/null 2>&1 && gcc -no-pie %s -lm -o %s "
             ">/dev/null 2>&1 && %s",
             FICC_BIN, src, obj, obj, bin, bin);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 42);

    char *paths[] = {src, obj, bin};
    for (size_t i = 0; i < 3; i++)
    {
        unlink(paths[i]);
    }
}

TEST(float_abi, fp_value_live_across_call)
{
    /* SysV AMD64: no callee-saved XMM, so a live-across-call double must spill.
       The host TU clobbers xmm9 to expose an allocator that keeps it there. */
    EXPECT_EQ(tc_run_elf_with_extra_tu(
                  "double clobber(double x);\n"
                  "double combine(double a, double b) { return clobber(a) + clobber(b); }\n"
                  "int main(void) { return (int)(combine(1.5, 2.5) * 10.0); }\n",
                  "double clobber(double x) {\n"
                  "    __asm__ volatile(\"movsd %%xmm0, %%xmm9\" ::: \"xmm9\");\n"
                  "    return x;\n"
                  "}\n"),
              40);
}