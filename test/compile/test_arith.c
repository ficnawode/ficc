#include "harness.h"
#include "testdriver.h"

TEST(arith, elf_div_immediate_divisor)
{
    EXPECT_EQ(tc_run_elf("int main(void) { return 10 / 5; }"), 2);
}

TEST(arith, elf_rem_immediate_divisor)
{
    EXPECT_EQ(tc_run_elf("int main(void) { return 10 % 3; }"), 1);
}

TEST(arith, elf_div_mixed_operands)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    int a = 37;\n"
                         "    int b = 7;\n"
                         "    return a / b + a % b;\n"
                         "}\n"),
              7);
}

static const char *arith_prog = "int add(int a, int b) {\n"
                                "    return a + b;\n"
                                "}\n"
                                "int sub(int a, int b) {\n"
                                "    return a - b;\n"
                                "}\n"
                                "int main(void) {\n"
                                "    int x;\n"
                                "    x = add(10, 3);\n"
                                "    x = sub(x, 2);\n"
                                "    return x * 5;\n"
                                "}\n";

TEST(arith, interp_mixed)
{
    EXPECT_EQ(tc_run_interp(arith_prog), 55);
}

TEST(arith, elf_mixed)
{
    EXPECT_EQ(tc_run_elf(arith_prog), 55);
}

static const char *unsigned_div_src =
    "int main(void) {\n"
    "    unsigned long m = 0xFFFFFFFFFFFFFFFFUL;\n"
    "    if (m / 16UL != 1152921504606846975UL) { return 1; }\n"
    "    if (m % 16UL != 15UL) { return 2; }\n"
    "    if (0x1000000000000000UL / 4UL != 0x400000000000000UL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

TEST(arith, unsigned_div_remainder)
{
    EXPECT_INTERP_AND_ELF(unsigned_div_src, 0);
}

static const char *unsigned_fold_src =
    "static const unsigned long Q = 0xFFFFFFFFFFFFFFFFUL / 16;\n"
    "static const int P = 0xFFFFFFFFFFFFFFFFUL > 0x1000000000000000UL;\n"
    "int main(void) {\n"
    "    if (Q != 1152921504606846975UL) { return 1; }\n"
    "    if (P != 1) { return 2; }\n"
    "    if (0xFFFFFFFFFFFFFFFFUL >> 4 != 1152921504606846975UL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

TEST(arith, unsigned_constant_fold)
{
    EXPECT_INTERP_AND_ELF(unsigned_fold_src, 0);
}

TEST(arith, signed_division_truncates_toward_zero)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (-7 / 2 != -3) return 1;\n"
                          "    if (-7 % 2 != -1) return 2;\n"
                          "    if (7 / -2 != -3) return 3;\n"
                          "    if (7 % -2 != 1) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}
