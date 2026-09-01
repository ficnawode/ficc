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
