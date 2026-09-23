#include "harness.h"
#include "testdriver.h"

/* Array bounds that cannot be folded by the parser (they need expression
   types) are recorded on the type and resolved by semantic as integer
   constant expressions (C11 §6.7.6.2, §6.6). */

TEST(deferred_bounds, sizeof_identifier)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    unsigned char buf[sizeof(x)];\n"
                          "    return (int) sizeof(buf);\n"
                          "}\n",
                          4);
}

TEST(deferred_bounds, swap_pattern_deref)
{
    EXPECT_INTERP_AND_ELF("struct S { int a; int b; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    struct S *p = &s;\n"
                          "    unsigned char buf[sizeof(*p)];\n"
                          "    return (int) sizeof(buf);\n"
                          "}\n",
                          8);
}

TEST(deferred_bounds, member_expression)
{
    EXPECT_INTERP_AND_ELF("struct S { int n; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    int a[sizeof(s.n) * 10 + 2];\n"
                          "    return (int) (sizeof(a) / sizeof(int));\n"
                          "}\n",
                          42);
}

TEST(deferred_bounds, subscript_expression)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[7];\n"
                          "    int a[sizeof(arr[0]) * 10 + 2];\n"
                          "    return (int) (sizeof(a) / sizeof(int));\n"
                          "}\n",
                          42);
}

TEST(deferred_bounds, typedef_expression)
{
    EXPECT_INTERP_AND_ELF("typedef int Word;\n"
                          "int main(void) {\n"
                          "    Word a[sizeof(Word) * 10 + 2];\n"
                          "    return (int) (sizeof(a) / sizeof(Word));\n"
                          "}\n",
                          42);
}

TEST(deferred_bounds, nested_dimensions)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    int a[sizeof(x)][sizeof(x) + 1];\n"
                          "    return (int) (sizeof(a) / sizeof(int));\n"
                          "}\n",
                          20);
}

TEST(deferred_bounds, pointer_to_deferred_array)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    int (*p)[sizeof(x)] = 0;\n"
                          "    return (int) sizeof(*p);\n"
                          "}\n",
                          16);
}

TEST(deferred_bounds, file_scope_bound)
{
    EXPECT_INTERP_AND_ELF("int g = 0;\n"
                          "int a[sizeof(g) * 10 + 2];\n"
                          "int main(void) {\n"
                          "    return (int) (sizeof(a) / sizeof(int));\n"
                          "}\n",
                          42);
}

TEST(deferred_bounds, file_scope_scalar_init)
{
    /* A file-scope scalar whose initializer needs expression types (here
       sizeof of an earlier array) is folded in semantic, not the parser. */
    EXPECT_INTERP_AND_ELF("static const int arr[3] = { 1, 2, 3 };\n"
                          "const int n = sizeof(arr) / sizeof(arr[0]);\n"
                          "int main(void) { return n * 14; }\n",
                          42);
}

TEST(deferred_bounds, negative_variable_length)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int n = 5;\n"
                      "    int a[n];\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(deferred_bounds, negative_negative_length)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 0;\n"
                      "    int a[sizeof(x) - 100];\n"
                      "    return 0;\n"
                      "}\n");
}
