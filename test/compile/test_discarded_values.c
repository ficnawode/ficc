#include "harness.h"
#include "testdriver.h"

/* Discarded-value expressions and void-void conditionals (§6.5.17p3,
   §6.5.15p3). Lua's barrier/audit macros spell `cond ? (void)f() : (void)0`
   and `((void)L, (void)0)`; these must parse, type-check, and run. */

TEST(discarded_values, void_void_ternary_statement)
{
    EXPECT_INTERP_AND_ELF("int g;\n"
                          "void side(int x) { g = x; }\n"
                          "int main(void) {\n"
                          "    side(0);\n"
                          "    !g ? (void) side(10) : (void) 0;\n"
                          "    if (g != 10) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(discarded_values, void_void_ternary_unwanted_branch)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 1;\n"
                          "    x == 2 ? (void) 0 : (void) 0;\n"
                          "    return x;\n"
                          "}\n",
                          1);
}

TEST(discarded_values, comma_with_void_operands)
{
    EXPECT_INTERP_AND_ELF("int g;\n"
                          "int bump(void) { return ++g; }\n"
                          "int main(void) {\n"
                          "    ((void) bump(), (void) 0);\n"
                          "    if (g != 1) return 1;\n"
                          "    ((void) 0, (void) bump());\n"
                          "    return g == 2 ? 0 : 2;\n"
                          "}\n",
                          0);
}

TEST(discarded_values, void_cast_of_value_operand)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    (void) x;\n"
                          "    x > 0 ? (void) 1 : (void) 0;\n"
                          "    return x;\n"
                          "}\n",
                          5);
}

TEST(discarded_values, real_operand_is_void_not_skipped)
{
    EXPECT_INTERP_AND_ELF("int calls;\n"
                          "void mark(void) { calls++; }\n"
                          "int main(void) {\n"
                          "    int cond = 0;\n"
                          "    cond ? (void) mark() : (void) 0;\n"
                          "    if (calls != 0) return 1;\n"
                          "    cond = 1;\n"
                          "    cond ? (void) mark() : (void) 0;\n"
                          "    return calls == 1 ? 0 : 2;\n"
                          "}\n",
                          0);
}