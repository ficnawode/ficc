#include "harness.h"
#include "testdriver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static const char *fold_src = "double f(void) { return 2.0 + 3.0; }\n"
                              "int main(void) { return (int) f(); }\n";

TEST(opt, level1_fp_fold)
{
    opt_run_level(fold_src, "-O1", 5);
}

TEST(opt, level2_fp_fold)
{
    opt_run_level(fold_src, "-O2", 5);
}

TEST(opt, level3_fp_fold)
{
    opt_run_level(fold_src, "-O3", 5);
}

static const char *cast_src = "int main(void) {\n"
                              "    char c = (char) 200;\n"
                              "    return c;\n"
                              "}\n";

TEST(opt, level1_cast_preserves_sign)
{
    opt_run_level(cast_src, "-O1", 200);
}

TEST(opt, level2_cast_preserves_sign)
{
    opt_run_level(cast_src, "-O2", 200);
}

static const char *dead_src = "int main(void) {\n"
                              "    return 42;\n"
                              "    return 7;\n"
                              "}\n";

TEST(opt, level1_dead_code_pruned)
{
    opt_run_level(dead_src, "-O1", 42);
}

TEST(opt, level2_dead_code_pruned)
{
    opt_run_level(dead_src, "-O2", 42);
}

static const char *branch_src = "int main(void) {\n"
                                "    if (2 < 3) return 30;\n"
                                "    return 99;\n"
                                "}\n";

TEST(opt, level1_constant_branch)
{
    opt_run_level(branch_src, "-O1", 30);
}

TEST(opt, level2_constant_branch)
{
    opt_run_level(branch_src, "-O2", 30);
}

static const char *do_while_src = "int main(void) {\n"
                                  "    int s = 0;\n"
                                  "    int i = 0;\n"
                                  "    do { s = s + i; i = i + 1; } while (i < 5);\n"
                                  "    return s;\n"
                                  "}\n";

TEST(opt, level2_do_while)
{
    opt_run_level(do_while_src, "-O2", 10);
}

static const char *volatile_src = "volatile int g;\n"
                                  "int main(void) {\n"
                                  "    for (int i = 0; i < 5; i = i + 1) g = g + 1;\n"
                                  "    return g;\n"
                                  "}\n";

TEST(opt, level2_volatile_loop)
{
    opt_run_level(volatile_src, "-O2", 5);
}

static const char *long_double_src = "long double g;\n"
                                     "int main(void) {\n"
                                     "    g = 1.5L;\n"
                                     "    if (g > 0) return (int) g;\n"
                                     "    return -1;\n"
                                     "}\n";

TEST(opt, level2_long_double)
{
    opt_run_level(long_double_src, "-O2", 1);
}

static const char *gvn_src = "int f(int x, int y) {\n"
                             "    int a = x + y;\n"
                             "    int b = x + y;\n"
                             "    return a * b;\n"
                             "}\n"
                             "int main(void) { return f(6, 1); }\n";

TEST(opt, level1_gvn_merges)
{
    opt_run_level(gvn_src, "-O1", 49);
}

TEST(opt, level2_gvn_merges)
{
    opt_run_level(gvn_src, "-O2", 49);
}

TEST(opt, level3_gvn_merges)
{
    opt_run_level(gvn_src, "-O3", 49);
}

static const char *licm_src = "int f(int k) {\n"
                              "    int s = 0;\n"
                              "    for (int i = 0; i < 100; i = i + 1) s = s + k * 3;\n"
                              "    return s;\n"
                              "}\n"
                              "int main(void) { return f(5) == 1500; }\n";

TEST(opt, level1_licm_hoists)
{
    opt_run_level(licm_src, "-O1", 1);
}

TEST(opt, level2_licm_hoists)
{
    opt_run_level(licm_src, "-O2", 1);
}

TEST(opt, level3_licm_hoists)
{
    opt_run_level(licm_src, "-O3", 1);
}

static const char *mem_fwd_src = "int g;\n"
                                 "int main(void) { g = 40; return g; }\n";

TEST(opt, level1_mem_fwd_store_to_load)
{
    opt_run_level(mem_fwd_src, "-O1", 40);
}

TEST(opt, level2_mem_fwd_store_to_load)
{
    opt_run_level(mem_fwd_src, "-O2", 40);
}

TEST(opt, level3_mem_fwd_store_to_load)
{
    opt_run_level(mem_fwd_src, "-O3", 40);
}

static const char *vol_barrier_src = "volatile int g;\n"
                                     "int main(void) { g = 40; return g; }\n";

TEST(opt, level2_mem_fwd_volatile_barrier)
{
    opt_run_level(vol_barrier_src, "-O2", 40);
}

static const char *redundant_load_src = "int g;\n"
                                        "int main(void) {\n"
                                        "    g = 20;\n"
                                        "    int a = g;\n"
                                        "    int b = g;\n"
                                        "    return a + b;\n"
                                        "}\n";

TEST(opt, level2_mem_fwd_redundant_load)
{
    opt_run_level(redundant_load_src, "-O2", 40);
}

static const char *inline_leaf_src = "static inline int sq(int x) { return x * x; }\n"
                                     "int main(void) { return sq(7) == 49 ? 0 : 1; }\n";

TEST(opt, level0_inline_off)
{
    opt_run_level(inline_leaf_src, "-O0", 0);
}

TEST(opt, level1_inline_static_leaf)
{
    opt_run_level(inline_leaf_src, "-O1", 0);
}

TEST(opt, level2_inline_static_leaf)
{
    opt_run_level(inline_leaf_src, "-O2", 0);
}

TEST(opt, level3_inline_static_leaf)
{
    opt_run_level(inline_leaf_src, "-O3", 0);
}

static const char *inline_chain_src = "static inline int seven(void) { return 7; }\n"
                                      "static inline int plus(int x) { return x + seven(); }\n"
                                      "int main(void) { return plus(10) == 17 ? 0 : 1; }\n";

TEST(opt, level1_inline_transitive_leaf)
{
    opt_run_level(inline_chain_src, "-O1", 0);
}

static const char *inline_address_taken_src = "static inline int dbl(int v) { return v * 2; }\n"
                                              "int main(void)\n"
                                              "{\n"
                                              "    int (*fp)(int) = &dbl;\n"
                                              "    return fp(21) == 42 ? 0 : 1;\n"
                                              "}\n";

TEST(opt, level1_inline_address_taken_negative)
{
    opt_run_level(inline_address_taken_src, "-O1", 0);
}

static const char *inline_recursion_src = "static inline int down(int n)\n"
                                          "{\n"
                                          "    return n <= 0 ? n : down(n - 1) + 1;\n"
                                          "}\n"
                                          "int main(void) { return down(200) == 200 ? 0 : 1; }\n";

TEST(opt, level1_inline_recursion_negative)
{
    opt_run_level(inline_recursion_src, "-O1", 0);
}

static const char *inline_loop_leaf_src = "int sq(int x) { return x * x; }\n"
                                          "int main(void)\n"
                                          "{\n"
                                          "    int s = 0;\n"
                                          "    for (int i = 0; i < 8; i = i + 1) s = s + sq(i);\n"
                                          "    return s == 140 ? 0 : 1;\n"
                                          "}\n";

TEST(opt, level1_inline_loop_discount)
{
    opt_run_level(inline_loop_leaf_src, "-O1", 0);
}

TEST(opt, level2_inline_loop_discount)
{
    opt_run_level(inline_loop_leaf_src, "-O2", 0);
}

static const char *dowhile_postdec_src = "int main(void)\n"
                                         "{\n"
                                         "    int b = 3;\n"
                                         "    int count = 0;\n"
                                         "    do {\n"
                                         "        count = count + 1;\n"
                                         "    } while (b--);\n"
                                         "    return count;\n"
                                         "}\n";

TEST(opt, level1_dowhile_postdec)
{
    opt_run_level(dowhile_postdec_src, "-O1", 4);
}

TEST(opt, level2_dowhile_postdec)
{
    opt_run_level(dowhile_postdec_src, "-O2", 4);
}

TEST(opt, level3_dowhile_postdec)
{
    opt_run_level(dowhile_postdec_src, "-O3", 4);
}

static const char *inline_alloca_loop_src =
    "struct big { int a[64]; };\n"
    "static int leaf(int x) { struct big s; s.a[0] = x; return s.a[0]; }\n"
    "int main(void)\n"
    "{\n"
    "    int sum = 0;\n"
    "    for (int i = 0; i < 100000; i = i + 1) sum = sum + leaf(i);\n"
    "    return sum == 704982704 ? 0 : 1;\n"
    "}\n";

TEST(opt, level1_inline_alloca_loop)
{
    opt_run_level(inline_alloca_loop_src, "-O1", 0);
}

TEST(opt, level2_inline_alloca_loop)
{
    opt_run_level(inline_alloca_loop_src, "-O2", 0);
}

static const char *fold_unsigned_shift_src =
    "int main(void)\n"
    "{\n"
    "    unsigned max = ((unsigned)-1 >> 2) + 1;\n"
    "    unsigned half = (unsigned)-1 >> 1;\n"
    "    return (max == 1073741824u && half == 2147483647u) ? 0 : 1;\n"
    "}\n";

TEST(opt, level1_fold_unsigned_shift_width)
{
    opt_run_level(fold_unsigned_shift_src, "-O1", 0);
}

TEST(opt, level2_fold_unsigned_shift_width)
{
    opt_run_level(fold_unsigned_shift_src, "-O2", 0);
}

TEST(opt, level3_fold_unsigned_shift_width)
{
    opt_run_level(fold_unsigned_shift_src, "-O3", 0);
}

/* A shift's result type is the promoted left operand (§6.5.7p3). */
static const char *shift_result_type_src =
    "int main(void)\n"
    "{\n"
    "    return ((long)1 << (sizeof(long) * 8 - 2)) > -1 ? 0 : 1;\n"
    "}\n";

TEST(opt, level1_shift_result_type_signed)
{
    opt_run_level(shift_result_type_src, "-O1", 0);
}

TEST(opt, level2_shift_result_type_signed)
{
    opt_run_level(shift_result_type_src, "-O2", 0);
}

TEST(opt, level3_shift_result_type_signed)
{
    opt_run_level(shift_result_type_src, "-O3", 0);
}
