#include "harness.h"
#include "testdriver.h"

/* Phase 12b: block-scope initializer lists. */

/* --- 1-D arrays --- */

TEST(init, one_d_simple)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int a[3] = {10, 20, 30};\n"
        "    return a[0] + a[1] + a[2];\n"
        "}\n",
        60);
}

TEST(init, one_d_partial)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int b[5] = {1};\n"
        "    return b[0] + b[1] + b[2] + b[3] + b[4];\n"
        "}\n",
        1);
}

TEST(init, one_d_trailing_comma)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int a[3] = {10, 20, 30,};\n"
        "    return a[0] + a[1] + a[2];\n"
        "}\n",
        60);
}

/* --- Scalar unwrap --- */

TEST(init, scalar_unwrap)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int x = {42};\n"
        "    return x;\n"
        "}\n",
        42);
}

/* --- Structs --- */

TEST(init, struct_simple)
{
    EXPECT_INTERP_AND_ELF(
        "struct S { int x; int y; };\n"
        "int main(void) {\n"
        "    struct S s = {10, 20};\n"
        "    return s.x + s.y;\n"
        "}\n",
        30);
}

TEST(init, struct_nested)
{
    EXPECT_INTERP_AND_ELF(
        "struct Inner { int a; int b; };\n"
        "struct Outer { struct Inner in; int c; };\n"
        "int main(void) {\n"
        "    struct Outer o = {{1, 2}, 3};\n"
        "    return o.in.a + o.in.b + o.c;\n"
        "}\n",
        6);
}

/* --- 2-D arrays --- */

TEST(init, two_d_full)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int c[2][2] = {{1, 2}, {3, 4}};\n"
        "    return c[0][0] + c[0][1] + c[1][0] + c[1][1];\n"
        "}\n",
        10);
}

TEST(init, two_d_partial_inner)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int c[2][3] = {{1, 2}, {4}};\n"
        "    return c[0][0] + c[0][1] + c[0][2] + c[1][0] + c[1][1] + c[1][2];\n"
        "}\n",
        7);
}

TEST(init, two_d_flattened)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int c[2][2] = {1, 2, 3, 4};\n"
        "    return c[0][0] + c[0][1] + c[1][0] + c[1][1];\n"
        "}\n",
        10);
}

/* --- Brace elision --- */

TEST(init, brace_elision_struct)
{
    EXPECT_INTERP_AND_ELF(
        "struct P { int x; int y; };\n"
        "int main(void) {\n"
        "    struct P p = {10, 20};\n"
        "    return p.x + p.y;\n"
        "}\n",
        30);
}

TEST(init, brace_elision_nested_struct_array)
{
    EXPECT_INTERP_AND_ELF(
        "struct P { int x; int y; };\n"
        "int main(void) {\n"
        "    struct P a[2] = {1, 2, 3, 4};\n"
        "    return a[0].x + a[0].y + a[1].x + a[1].y;\n"
        "}\n",
        10);
}

/* --- Designated initializers --- */

TEST(init, designator_index)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int a[5] = {[2] = 30, [0] = 10};\n"
        "    return a[0] + a[2];\n"
        "}\n",
        40);
}

TEST(init, designator_field)
{
    EXPECT_INTERP_AND_ELF(
        "struct S { int x; int y; int z; };\n"
        "int main(void) {\n"
        "    struct S s = {.y = 20, .x = 10};\n"
        "    return s.x + s.y + s.z;\n"
        "}\n",
        30);
}

TEST(init, designator_chained)
{
    EXPECT_INTERP_AND_ELF(
        "struct P { int x; int y; };\n"
        "int main(void) {\n"
        "    struct P a[2] = {[1].y = 40, [0].x = 10};\n"
        "    return a[0].x + a[1].y;\n"
        "}\n",
        50);
}

/* --- Char-array from string --- */

TEST(init, char_array_from_string)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    char s[4] = \"hi\";\n"
        "    return s[0] + s[1];\n"
        "}\n",
        'h' + 'i');
}

/* --- Empty init zero-fills --- */

TEST(init, array_zero_fill)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int a[4] = {0};\n"
        "    return a[0] + a[1] + a[2] + a[3];\n"
        "}\n",
        0);
}

/* --- negatives (all must fail to build) --- */

TEST(init, negative_overlong_array)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a[2] = {1, 2, 3};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_scalar_twice)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = {5, 6};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_bad_field)
{
    EXPECT_BUILD_FAIL("struct S { int x; int y; };\n"
                      "int main(void) {\n"
                      "    struct S s = {.z = 10};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_index_out_of_range)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a[2] = {[5] = 1};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_field_on_array)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a[2] = {.y = 10};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_index_on_struct)
{
    EXPECT_BUILD_FAIL("struct S { int x; };\n"
                      "int main(void) {\n"
                      "    struct S s = {[0] = 10};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_string_too_long)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    char s[2] = \"hi\";\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, incomplete_array_infers_length)
{
    /* Phase 12d: `[]` is completed from its initializer. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[] = {1, 2, 3};\n"
                          "    return a[0] + a[1] + a[2];\n"
                          "}\n",
                          6);
}

TEST(init, incomplete_array_infers_designator)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[] = {[3] = 7};\n"
                          "    return a[0] + a[1] + a[2] + a[3];\n"
                          "}\n",
                          7);
}

TEST(init, incomplete_array_infers_two_d)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int m[][2] = {{1}, {2, 3}};\n"
                          "    return sizeof(m);\n"
                          "}\n",
                          16);
}

TEST(init, incomplete_array_infers_elided_rows)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int m[][2] = {1, 2, 3, 4};\n"
                          "    return m[0][0] + m[0][1] + m[1][0] + m[1][1];\n"
                          "}\n",
                          10);
}

TEST(init, incomplete_char_array_from_string)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[] = \"hi\";\n"
                          "    return sizeof(s);\n"
                          "}\n",
                          3);
}

TEST(init, incomplete_array_of_strings)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[][4] = {\"ab\", \"cde\"};\n"
                          "    return s[0][1] + s[1][2];\n"
                          "}\n",
                          'b' + 'e');
}

TEST(init, char_array_braced_string)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[5] = {\"hi\"};\n"
                          "    return s[0] + s[1];\n"
                          "}\n",
                          'h' + 'i');
}

TEST(init, char_array_unsized_braced_string)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[] = {\"ab\"};\n"
                          "    return sizeof(s);\n"
                          "}\n",
                          3);
}

TEST(init, negative_braced_string_too_long)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    char s[2] = {\"hi\"};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_bare_incomplete_array)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a[];\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_inner_empty_brackets)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a[2][] = {0, 1};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, negative_list_not_expression)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x;\n"
                      "    return x = {1, 2};\n"
                      "}\n");
}