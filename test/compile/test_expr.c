#include "harness.h"
#include "testdriver.h"

static const char *logical_src = "int main(void) {\n"
                                 "    int a = 1;\n"
                                 "    int b = 0;\n"
                                 "    int c = 0;\n"
                                 "    if (a && b) {\n"
                                 "        c = 1;\n"
                                 "    }\n"
                                 "    if (a || b) {\n"
                                 "        c = c + 10;\n"
                                 "    }\n"
                                 "    if (b && a) {\n"
                                 "        c = c + 100;\n"
                                 "    }\n"
                                 "    return c;\n"
                                 "}\n";

TEST(expr, interp_logical_and_or)
{
    EXPECT_EQ(tc_run_interp(logical_src), 10);
}

TEST(expr, elf_logical_and_or)
{
    EXPECT_EQ(tc_run_elf(logical_src), 10);
}

static const char *ternary_src = "int main(void) {\n"
                                 "    int a = 5;\n"
                                 "    int b = 3;\n"
                                 "    int c = a > b ? 7 : 2;\n"
                                 "    return c;\n"
                                 "}\n";

TEST(expr, interp_ternary)
{
    EXPECT_EQ(tc_run_interp(ternary_src), 7);
}

TEST(expr, elf_ternary)
{
    EXPECT_EQ(tc_run_elf(ternary_src), 7);
}

static const char *comparisons_src = "int main(void) {\n"
                                     "    int r = 0;\n"
                                     "    if (1 < 2) {\n"
                                     "        r = r + 1;\n"
                                     "    }\n"
                                     "    if (2 > 1) {\n"
                                     "        r = r + 2;\n"
                                     "    }\n"
                                     "    if (1 <= 1) {\n"
                                     "        r = r + 4;\n"
                                     "    }\n"
                                     "    if (2 >= 2) {\n"
                                     "        r = r + 8;\n"
                                     "    }\n"
                                     "    if (1 == 1) {\n"
                                     "        r = r + 16;\n"
                                     "    }\n"
                                     "    if (1 != 2) {\n"
                                     "        r = r + 32;\n"
                                     "    }\n"
                                     "    return r;\n"
                                     "}\n";

TEST(expr, interp_comparisons)
{
    EXPECT_EQ(tc_run_interp(comparisons_src), 63);
}

TEST(expr, elf_comparisons)
{
    EXPECT_EQ(tc_run_elf(comparisons_src), 63);
}

static const char *bitwise_src = "int main(void) {\n"
                                 "    int r = 0;\n"
                                 "    r = (5 & 3);\n"
                                 "    r = r | 8;\n"
                                 "    r = r ^ 2;\n"
                                 "    r = r << 1;\n"
                                 "    r = r >> 1;\n"
                                 "    return r;\n"
                                 "}\n";

TEST(expr, interp_bitwise)
{
    EXPECT_EQ(tc_run_interp(bitwise_src), 11);
}

TEST(expr, elf_bitwise)
{
    EXPECT_EQ(tc_run_elf(bitwise_src), 11);
}

static const char *call7_src = "int sum7(int a, int b, int c, int d, int e, int f, int g) {\n"
                               "    return a + b + c + d + e + f + g;\n"
                               "}\n"
                               "int main(void) {\n"
                               "    return sum7(1, 2, 3, 4, 5, 6, 7) == 28 ? 0 : 1;\n"
                               "}\n";

TEST(expr, interp_call_7_args)
{
    EXPECT_EQ(tc_run_interp(call7_src), 0);
}

TEST(expr, elf_call_7_args)
{
    EXPECT_EQ(tc_run_elf(call7_src), 0);
}

static const char *wide_compare_src =
    "int main(void) {\n"
    "    const char *s = \"abcdefgh\";\n"
    "    int n = 0;\n"
    "    while (*(s + n)) { n++; }\n"
    "    if (n != 8) { return 1; }\n"
    "    if (s[0] != 'a') { return 2; }\n"
    "    if (!(0xFFFFFFFF00000000UL > 0x7FFFFFFFFFFFFFFFUL)) { return 3; }\n"
    "    if (18446744073709551615UL < 0UL != 0) { return 4; }\n"
    "    if (18446744073709551615UL > 0UL != 1) { return 5; }\n"
    "    return 0;\n"
    "}\n";

TEST(expr, wide_compare)
{
    EXPECT_INTERP_AND_ELF(wide_compare_src, 0);
}

TEST(expr, logical_and_or_short_circuit)
{
    EXPECT_INTERP_AND_ELF("static int calls;\n"
                          "static int hit(void) { calls++; return 1; }\n"
                          "int main(void) {\n"
                          "    calls = 0;\n"
                          "    if (0 && hit()) return 1;\n"
                          "    if (calls != 0) return 2;\n"
                          "    calls = 0;\n"
                          "    if (1 || hit()) { }\n"
                          "    if (calls != 0) return 3;\n"
                          "    if (0 || hit()) { }\n"
                          "    if (calls != 1) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(expr, unary_operators_apply)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    if (-x != -5) return 1;\n"
                          "    if (+x != 5) return 2;\n"
                          "    if (~0 != -1) return 3;\n"
                          "    if (!x != 0) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}