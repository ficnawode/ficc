#include "harness.h"
#include "testdriver.h"

TEST(generic, selects_int_arm)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 1;\n"
                          "    return _Generic(x, int: 40, double: 0) + 2;\n"
                          "}\n",
                          42);
}

TEST(generic, selects_default)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long x = 0;\n"
                          "    return _Generic(x, int: 1, default: 42);\n"
                          "}\n",
                          42);
}

TEST(generic, pointer_constness_distinguishes_associations)
{
    /* C11 §6.5.15p2: the controlling expression undergoes lvalue conversion and
       is matched by type compatibility, so `const char *` and `char *` select
       different associations. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const char *cs = \"x\";\n"
                          "    char *us = (char *)\"y\";\n"
                          "    return _Generic(cs, const char *: 10, default: 0) +\n"
                          "           _Generic(us, const char *: 1, default: 2);\n"
                          "}\n",
                          12);
}

TEST(generic, array_controlling_decays_to_pointer)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = { 1, 2, 3 };\n"
                          "    return _Generic(a, int *: 42, default: 0);\n"
                          "}\n",
                          42);
}

TEST(generic, function_controlling_decays_to_pointer)
{
    EXPECT_INTERP_AND_ELF("int f(void) { return 42; }\n"
                          "int main(void) {\n"
                          "    return _Generic(f, int (*)(void): 42, default: 0);\n"
                          "}\n",
                          42);
}

TEST(generic, selected_type_is_result_type)
{
    /* §6.5.15p5: the result type is the selected expression's type. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    return (int) sizeof(_Generic(1, int: 0, char: 0));\n"
                          "}\n",
                          4);
}

TEST(generic, only_selected_arm_evaluated)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int calls = 0;\n"
                          "    int v = _Generic(1, int: 42, default: (calls = 9));\n"
                          "    return v + calls;\n"
                          "}\n",
                          42);
}

TEST(generic, controlling_expression_not_evaluated)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int calls = 0;\n"
                          "    (void)_Generic((calls = 5), int: 0, default: 0);\n"
                          "    if (calls != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(generic, lvalue_conversion_drops_const)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const int x = 0;\n"
                          "    if (_Generic(x, int: 42, default: 1) != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(generic, unsigned_int_association_distinct)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned u = 1;\n"
                          "    if (_Generic(u, unsigned int: 42, int: 0) != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(generic, negative_no_match_no_default)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    double d = 0;\n"
                      "    return _Generic(d, int: 1);\n"
                      "}\n");
}

TEST(generic, negative_duplicate_default)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return _Generic(1, default: 1, default: 2);\n"
                      "}\n");
}

TEST(generic, negative_duplicate_compatible_association)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return _Generic(1, int: 1, int: 2);\n"
                      "}\n");
}

TEST(generic, negative_void_association)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return _Generic(1, void: 1, default: 2);\n"
                      "}\n");
}
