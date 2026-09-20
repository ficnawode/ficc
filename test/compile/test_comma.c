#include "harness.h"
#include "testdriver.h"

/* Phase 13d: the comma operator (§6.5.17). It is the C11 *expression* level:
   the left operand is evaluated as a void expression and discarded, the value
   is the right operand's. Commas in argument lists, initializer lists, and
   designator indexes are *separators*, not the operator — those stay at the
   assignment-expression level. Every program must produce the same result
   through the interpreter and through the compiled ELF. */

TEST(comma, value_is_right_operand)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a = 1;\n"
                          "    int b = 2;\n"
                          "    int v = (a, b);\n"
                          "    if (v != 2) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, left_is_evaluated_and_discarded)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 0;\n"
                          "    int v = (i++, i++);\n"
                          "    if (i != 2) return 1;\n"
                          "    if (v != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, chain_left_associative)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 0;\n"
                          "    int v = (i++, i++, i++);\n"
                          "    if (i != 3) return 1;\n"
                          "    if (v != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, in_return)
{
    EXPECT_INTERP_AND_ELF("int callee(void) { return 7; }\n"
                          "int main(void) {\n"
                          "    return (callee(), 6 * 7);\n"
                          "}\n",
                          42);
}

TEST(comma, in_for_clauses)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 0;\n"
                          "    int j = 0;\n"
                          "    for (i = 0, j = 0; i < 3; i++, j = j + 2) {}\n"
                          "    if (i != 3) return 1;\n"
                          "    if (j != 6) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, in_while_condition)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = -3;\n"
                          "    while (i++, i < 0) {}\n"
                          "    if (i != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, in_switch_condition)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 40;\n"
                          "    switch ((x, 42)) {\n"
                          "        case 42: return 42;\n"
                          "        default: return 1;\n"
                          "    }\n"
                          "}\n",
                          42);
}

TEST(comma, in_subscript_index)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[4] = {10, 20, 30, 40};\n"
                          "    int i = 1;\n"
                          "    int w = a[i, 2];\n"
                          "    if (w != 30) return 1;\n"
                          "    if (a[0, 0] != 10) return 2;\n"
                          "    if (a[1 == 1, 3] != 40) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, in_ternary_middle)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    int y = x > 0 ? (x, 41) : 99;\n"
                          "    if (y != 41) return 1;\n"
                          "    int z = x < 0 ? (0, 1) : 42;\n"
                          "    if (z != 42) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, void_left_operand)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    int v = ((void)0, 42);\n"
                          "    if (v != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, in_call_arguments_is_separator)
{
    EXPECT_INTERP_AND_ELF("int add3(int a, int b, int c) { return a + b + c; }\n"
                          "int main(void) {\n"
                          "    int i = 0;\n"
                          "    int r = add3((i++, 10), (i++, 20), 12);\n"
                          "    if (r != 42) return 1;\n"
                          "    if (i != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, in_init_list_is_separator)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {1, 2, 3};\n"
                          "    int i = 0;\n"
                          "    a[i++] = (4, 41);\n" /* inner paren → comma op */
                          "    a[i++] = 1;\n"
                          "    if (a[0] != 41) return 1;\n"
                          "    if (i != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, size_of_paren_primary)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char a = 1;\n"
                          "    long b = 2;\n"
                          "    if (sizeof (a, b) != sizeof(long)) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(comma, left_side_effects_before_right)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[2] = {1, 2};\n"
                          "    int i = 0;\n"
                          "    int v = (a[i++] = 99, a[i]);\n"
                          "    if (a[0] != 99) return 1;\n"
                          "    if (v != 2) return 2;\n"
                          "    if (i != 1) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

/* negatives (all must fail to build) */

TEST(comma, negative_not_an_lvalue)
{
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int b = 2; (a, b) = 5; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int b = 2; (a, b)++; return 0; }\n");
}

TEST(comma, negative_case_constant)
{
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; switch (x) { case (1, 2): return 0; } }\n");
}

TEST(comma, negative_sizeof_type_name)
{
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; return sizeof (int, x); }\n");
}

TEST(comma, negative_array_size)
{
    EXPECT_BUILD_FAIL("int main(void) { int a[(1, 2)]; return 0; }\n");
}