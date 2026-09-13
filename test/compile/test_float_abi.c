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