#include "harness.h"
#include "testdriver.h"

/* Phase 12b: block-scope initializer lists. */

/* --- 1-D arrays --- */

TEST(init, one_d_simple)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {10, 20, 30};\n"
                          "    return a[0] + a[1] + a[2];\n"
                          "}\n",
                          60);
}

TEST(init, one_d_partial)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int b[5] = {1};\n"
                          "    return b[0] + b[1] + b[2] + b[3] + b[4];\n"
                          "}\n",
                          1);
}

TEST(init, one_d_trailing_comma)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {10, 20, 30,};\n"
                          "    return a[0] + a[1] + a[2];\n"
                          "}\n",
                          60);
}

/* --- Scalar unwrap --- */

TEST(init, scalar_unwrap)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = {42};\n"
                          "    return x;\n"
                          "}\n",
                          42);
}

/* --- Structs --- */

TEST(init, struct_simple)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct S s = {10, 20};\n"
                          "    return s.x + s.y;\n"
                          "}\n",
                          30);
}

TEST(init, struct_nested)
{
    EXPECT_INTERP_AND_ELF("struct Inner { int a; int b; };\n"
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
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int c[2][2] = {{1, 2}, {3, 4}};\n"
                          "    return c[0][0] + c[0][1] + c[1][0] + c[1][1];\n"
                          "}\n",
                          10);
}

TEST(init, two_d_partial_inner)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int c[2][3] = {{1, 2}, {4}};\n"
                          "    return c[0][0] + c[0][1] + c[0][2] + c[1][0] + c[1][1] + c[1][2];\n"
                          "}\n",
                          7);
}

TEST(init, two_d_flattened)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int c[2][2] = {1, 2, 3, 4};\n"
                          "    return c[0][0] + c[0][1] + c[1][0] + c[1][1];\n"
                          "}\n",
                          10);
}

/* --- Brace elision --- */

TEST(init, brace_elision_struct)
{
    EXPECT_INTERP_AND_ELF("struct P { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct P p = {10, 20};\n"
                          "    return p.x + p.y;\n"
                          "}\n",
                          30);
}

TEST(init, brace_elision_nested_struct_array)
{
    EXPECT_INTERP_AND_ELF("struct P { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct P a[2] = {1, 2, 3, 4};\n"
                          "    return a[0].x + a[0].y + a[1].x + a[1].y;\n"
                          "}\n",
                          10);
}

/* --- Designated initializers --- */

TEST(init, designator_index)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[5] = {[2] = 30, [0] = 10};\n"
                          "    return a[0] + a[2];\n"
                          "}\n",
                          40);
}

TEST(init, designator_field)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; int z; };\n"
                          "int main(void) {\n"
                          "    struct S s = {.y = 20, .x = 10};\n"
                          "    return s.x + s.y + s.z;\n"
                          "}\n",
                          30);
}

TEST(init, designator_chained)
{
    EXPECT_INTERP_AND_ELF("struct P { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct P a[2] = {[1].y = 40, [0].x = 10};\n"
                          "    return a[0].x + a[1].y;\n"
                          "}\n",
                          50);
}

/* --- Char-array from string --- */

TEST(init, char_array_from_string)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[4] = \"hi\";\n"
                          "    return s[0] + s[1];\n"
                          "}\n",
                          'h' + 'i');
}

/* --- Empty init zero-fills --- */

TEST(init, array_zero_fill)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
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
    /* §6.7.9p14: only `strlen > size` is an error — the NUL is dropped when
       there is no room (`char s[2]="hi"` fits). */
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    char s[1] = \"hi\";\n"
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
                      "    char s[1] = {\"hi\"};\n"
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

/* --- Phase 12 coverage completes: C11 §6.7.9 boundary rows --- */

TEST(init, char_array_exact_fit_keeps_nul)
{
    /* `char s[3] = "hi"` stores the 3 bytes h,i,\0 — the NUL has room. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[3] = \"hi\";\n"
                          "    if (s[0] != 104 || s[1] != 105) return 1;\n"
                          "    if (s[2] != 0) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(init, char_array_string_drops_nul)
{
    /* §6.7.9p14: the NUL is stored only if there is room — `char s[2]="hi"`
       fits both chars and drops the terminator (legal C11). */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[2] = \"hi\";\n"
                          "    if (s[0] != 104 || s[1] != 105) return 1;\n"
                          "    if (sizeof(s) != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(init, char_array_braced_string_drops_nul)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s[2] = {\"hi\"};\n"
                          "    if (s[0] != 104 || s[1] != 105) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(init, empty_braces_array)
{
    /* `{}` zero-inits: documented gcc extension / C23 (D12.11) — the plan
       pins acceptance, it is not a C11 constraint violation here. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[5] = {};\n"
                          "    return a[0] + a[1] + a[2] + a[3] + a[4];\n"
                          "}\n",
                          0);
}

TEST(init, empty_braces_record_scalar)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "union U { int i; };\n"
                          "int main(void) {\n"
                          "    struct S s = {};\n"
                          "    union U u = {};\n"
                          "    int z = {};\n"
                          "    return s.x + s.y + u.i + z;\n"
                          "}\n",
                          0);
}

TEST(init, designator_last_wins)
{
    /* §6.7.9p19: the last initializer for a designated subobject wins —
       `[0]=1, [0]=2` yields `{2,0}`. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[2] = {[0] = 1, [0] = 2};\n"
                          "    if (a[0] != 2 || a[1] != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(init, enum_scalar_braced)
{
    /* §6.7.9p11: a scalar may take a braced single-element list — an enum
       initializes from its int underlay. */
    EXPECT_INTERP_AND_ELF("enum E { A = 41, B };\n"
                          "int main(void) {\n"
                          "    enum E e = {A};\n"
                          "    if (e != A) return 1;\n"
                          "    return e + 1;\n"
                          "}\n",
                          42);
}

TEST(init, const_member_via_list)
{
    /* Initialization is not assignment (§6.7.9p4): a list may write a const
       member; only post-construction stores hit the §9 write gate. */
    EXPECT_INTERP_AND_ELF("struct C { const int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct C c = {.x = 40, .y = 2};\n"
                          "    if (c.x + c.y != 42) return 1;\n"
                          "    c.y = c.y + 1;\n"
                          "    return c.x + c.y;\n"
                          "}\n",
                          43);
}

TEST(init, negative_const_member_assign)
{
    EXPECT_BUILD_FAIL("struct C { const int x; };\n"
                      "int main(void) {\n"
                      "    struct C c = {.x = 1};\n"
                      "    c.x = 2;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(init, union_first_member)
{
    /* §6.7.9p13: an undesignated union list initializes the first member at
       the union's base; the rest of the union's bytes are zero. */
    EXPECT_INTERP_AND_ELF("union U { char c; int i; };\n"
                          "int main(void) {\n"
                          "    union U u = {5};\n"
                          "    if (u.c != 5 || u.i != 5) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(init, union_designated_member)
{
    EXPECT_INTERP_AND_ELF("union U { char c; int i; };\n"
                          "int main(void) {\n"
                          "    union U u = {.i = 300};\n"
                          "    if (u.i != 300) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(init, negative_overlong_struct)
{
    /* Excess elements past a known-size record are a constraint violation
       (§6.7.9p2) — `struct S { int a, b; } s = {1,2,3};`. */
    EXPECT_BUILD_FAIL("struct S { int a; int b; };\n"
                      "int main(void) {\n"
                      "    struct S s = {1, 2, 3};\n"
                      "    return 0;\n"
                      "}\n");
}