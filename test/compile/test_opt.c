#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Drives the real ficc binary at each -O level, asserting the same exit value. */

static unsigned int opt_seq;

static void opt_path(char *buf, size_t sz, const char *tag, const char *ext)
{
    snprintf(buf, sz, "/tmp/ficc_21d_%u_%s.%s", opt_seq++, tag, ext);
}

static void opt_write_src(char *out, size_t sz, const char *src)
{
    snprintf(out, sz, "/tmp/ficc_21d_%u_src.c", opt_seq++);
    FILE *f = fopen(out, "w");
    EXPECT_NOTNULL(f);
    if (f)
    {
        fputs(src, f);
        fclose(f);
    }
}

static void opt_run_level(const char *src, const char *level, int expected)
{
    char src_path[256], obj[256], bin[256], cmd[2048];
    opt_write_src(src_path, sizeof(src_path), src);
    opt_path(obj, sizeof(obj), "obj", "o");
    opt_path(bin, sizeof(bin), "bin", "bin");

    snprintf(cmd, sizeof(cmd), "%s %s -c %s -o %s >/dev/null 2>&1", FICC_BIN, level, src_path, obj);
    int rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, 0);
    if (rc != 0)
    {
        unlink(src_path);
        return;
    }

    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj, bin, bin);
    rc = tc_run_shell(cmd);
    EXPECT_EQ(rc, expected);

    char *paths[] = {src_path, obj, bin};
    for (size_t i = 0; i < 3; i++)
    {
        unlink(paths[i]);
    }
}

/* A loop-heavy program: any mis-stepping pass diverges across levels. */
static const char *loop_sum_src = "int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    for (int i = 0; i < 10; i = i + 1) s = s + i;\n"
                                  "    return s;\n"
                                  "}\n";

TEST(opt, level0_loop_sum)
{
    opt_run_level(loop_sum_src, "-O0", 45);
}

TEST(opt, level1_loop_sum)
{
    opt_run_level(loop_sum_src, "-O1", 45);
}

TEST(opt, level2_loop_sum)
{
    opt_run_level(loop_sum_src, "-O2", 45);
}

TEST(opt, bare_O_loop_sum)
{
    opt_run_level(loop_sum_src, "-O", 45);
}

static const char *expr_src = "int main(void) {\n"
                              "    int a = 20;\n"
                              "    int b = 22;\n"
                              "    if (a < b) return a + b;\n"
                              "    return 0;\n"
                              "}\n";

TEST(opt, level2_if_returns)
{
    opt_run_level(expr_src, "-O2", 42);
}