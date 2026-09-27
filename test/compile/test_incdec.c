#include "harness.h"
#include "testdriver.h"

TEST(incdec, postfix_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 1;\n"
                          "    int j = i++;\n"
                          "    if (j != 1) return 1;\n"
                          "    if (i != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, prefix_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 1;\n"
                          "    int j = ++i;\n"
                          "    if (j != 2) return 1;\n"
                          "    if (i != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, postfix_decrement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 2;\n"
                          "    int j = i--;\n"
                          "    if (j != 2) return 1;\n"
                          "    if (i != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, prefix_decrement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 2;\n"
                          "    int j = --i;\n"
                          "    if (j != 1) return 1;\n"
                          "    if (i != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, char_postfix_increment_wraps)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = 127;\n"
                          "    int r = c++;\n"
                          "    if (r != 127) return 1;\n"
                          "    if (c != -128) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, char_prefix_decrement_wraps)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = 0;\n"
                          "    --c;\n"
                          "    if (c != -1) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, uchar_postfix_increment_wraps)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned char c = 255;\n"
                          "    c++;\n"
                          "    if (c != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, uchar_postfix_decrement_wraps)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned char c = 0;\n"
                          "    c--;\n"
                          "    if (c != 255) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, short_wraps)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    short s = 32767;\n"
                          "    s++;\n"
                          "    if (s != -32768) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, enum_postfix_increment)
{
    EXPECT_INTERP_AND_ELF("enum E { A = 5 };\n"
                          "int main(void) {\n"
                          "    enum E e = A;\n"
                          "    e++;\n"
                          "    if (e != 6) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, enum_prefix_decrement)
{
    EXPECT_INTERP_AND_ELF("enum E { A = 5 };\n"
                          "int main(void) {\n"
                          "    enum E e = A;\n"
                          "    --e;\n"
                          "    if (e != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, bool_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Bool b = 0;\n"
                          "    b++;\n"
                          "    if (b != 1) return 1;\n"
                          "    b++;\n"
                          "    if (b != 1) return 2;\n"
                          "    --b;\n"
                          "    if (b != 0) return 3;\n"
                          "    ++b;\n"
                          "    if (b != 1) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, float_increment_decrement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    float f = 1.5f;\n"
                          "    f++;\n"
                          "    if (f != 2.5f) return 1;\n"
                          "    double d = 3.0;\n"
                          "    d--;\n"
                          "    if (d != 2.0) return 2;\n"
                          "    f--;\n"
                          "    if (f != 1.5f) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, ssa_scalar_roundtrip)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 41;\n"
                          "    x++;\n"
                          "    x--;\n"
                          "    if (x != 41) return 1;\n"
                          "    ++x;\n"
                          "    if (x != 42) return 2;\n"
                          "    return x;\n"
                          "}\n",
                          42);
}

TEST(incdec, unsigned_int_decrement_wraps)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned int u = 0;\n"
                          "    u--;\n"
                          "    if (u != 4294967295u) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, pointer_increment_decrement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3];\n"
                          "    int *p = &a[0];\n"
                          "    p++;\n"
                          "    if (p != &a[1]) return 1;\n"
                          "    ++p;\n"
                          "    if (p != &a[2]) return 2;\n"
                          "    p--;\n"
                          "    if (p != &a[1]) return 3;\n"
                          "    int *q = p++;\n"
                          "    if (q != &a[1]) return 4;\n"
                          "    if (p != &a[2]) return 5;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, pointer_to_pointer_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[4];\n"
                          "    int *p = &a[0];\n"
                          "    int **pp = &p;\n"
                          "    (*pp)++;\n"
                          "    if (p != &a[1]) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, pointer_struct_scale)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct S arr[2];\n"
                          "    struct S *p = &arr[0];\n"
                          "    p++;\n"
                          "    p->x = 42;\n"
                          "    if (arr[1].x != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, member_access)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    s.x = 41;\n"
                          "    int old = s.x++;\n"
                          "    if (old != 41) return 1;\n"
                          "    if (s.x != 42) return 2;\n"
                          "    return s.x;\n"
                          "}\n",
                          42);
}

TEST(incdec, arrow_prefix_increment)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    struct S *p = &s;\n"
                          "    s.x = 41;\n"
                          "    ++p->x;\n"
                          "    if (s.x != 42) return 1;\n"
                          "    return s.x;\n"
                          "}\n",
                          42);
}

TEST(incdec, deref_member_postfix_increment)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    struct S *p = &s;\n"
                          "    s.x = 41;\n"
                          "    (*p).x++;\n"
                          "    if (s.x != 42) return 1;\n"
                          "    return s.x;\n"
                          "}\n",
                          42);
}

TEST(incdec, deref_postfix_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {1, 2, 3};\n"
                          "    int *p = &a[1];\n"
                          "    (*p)++;\n"
                          "    if (a[1] != 3) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, deref_prefix_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {1, 2, 3};\n"
                          "    int *p = &a[1];\n"
                          "    ++*p;\n"
                          "    if (a[1] != 3) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, pointer_postfix_decrement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {1, 2, 3};\n"
                          "    int *p = &a[1];\n"
                          "    int old = *p--;\n"
                          "    if (old != 2) return 1;\n"
                          "    if (p != &a[0]) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, subscript_postfix_index)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {10, 20, 30};\n"
                          "    int i = 0;\n"
                          "    int v = a[i++];\n"
                          "    if (v != 10) return 1;\n"
                          "    if (i != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, subscript_prefix_index)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {10, 20, 30};\n"
                          "    int i = 0;\n"
                          "    int v = a[++i];\n"
                          "    if (v != 20) return 1;\n"
                          "    if (i != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, subscript_object_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[3] = {1, 2, 3};\n"
                          "    int old = a[0]++;\n"
                          "    if (old != 1) return 1;\n"
                          "    if (a[0] != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, while_postfix_decrement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int i = 3;\n"
                          "    int n = 0;\n"
                          "    while (i-- > 0) { n++; }\n"
                          "    if (i != -1) return 1;\n"
                          "    if (n != 3) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, for_postfix_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int sum = 0;\n"
                          "    for (int k = 0; k < 7; k++) { sum = sum + k; }\n"
                          "    if (sum != 21) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, spilled_scalar_increment)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 0;\n"
                          "    int *p = &x;\n"
                          "    x++;\n"
                          "    ++x;\n"
                          "    if (x != 2) return 1;\n"
                          "    if (*p != 2) return 2;\n"
                          "    (*p)--;\n"
                          "    if (x != 1) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, global_postfix_increment)
{
    EXPECT_INTERP_AND_ELF("int g = 40;\n"
                          "int main(void) {\n"
                          "    g++;\n"
                          "    if (g != 41) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, global_prefix_increment)
{
    EXPECT_INTERP_AND_ELF("int g = 40;\n"
                          "int main(void) {\n"
                          "    ++g;\n"
                          "    if (g != 41) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, block_static_increment)
{
    EXPECT_INTERP_AND_ELF("int bump(void) {\n"
                          "    static int s = 0;\n"
                          "    return s++;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    if (bump() != 0) return 1;\n"
                          "    if (bump() != 1) return 2;\n"
                          "    if (bump() != 2) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_literal_lvalue)
{
    /* C11 §6.5.2.5: a compound literal is a modifiable lvalue. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int *p = &(int){5};\n"
                          "    (*p)++;\n"
                          "    if (*p != 6) return 1;\n"
                          "    ++*p;\n"
                          "    if (*p != 7) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, postfix_old_value_chain)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[4] = {0, 0, 0, 0};\n"
                          "    int i = 0;\n"
                          "    a[i++] = 1;\n"
                          "    a[i++] = 2;\n"
                          "    a[i++] = 3;\n"
                          "    if (a[0] != 1 || a[1] != 2 || a[2] != 3) return 1;\n"
                          "    if (i != 3) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, negative_const_scalar)
{
    EXPECT_BUILD_FAIL("int main(void) { const int c = 1; c++; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { const int c = 1; c--; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { const int c = 1; ++c; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { const int c = 1; --c; return 0; }\n");
}

TEST(incdec, negative_const_pointee)
{
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; const int *p = &x; (*p)++; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; const int *p = &x; *p = 2; return 0; }\n");
}

TEST(incdec, negative_const_pointer)
{
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; int *const p = &x; p++; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; int *const p = &x; --p; return 0; }\n");
}

TEST(incdec, negative_const_pointer_typedef)
{
    EXPECT_BUILD_FAIL("typedef int *IP;\n"
                      "int main(void) { int x = 1; const IP p = &x; p++; return 0; }\n");
}

TEST(incdec, negative_rvalue)
{
    EXPECT_BUILD_FAIL("int main(void) { return ++5; }\n");
    EXPECT_BUILD_FAIL("int main(void) { return 5--; }\n");
}

TEST(incdec, negative_void_operand)
{
    EXPECT_BUILD_FAIL("int main(void) { return ++(void)0; }\n");
}

TEST(incdec, negative_array_operand)
{
    EXPECT_BUILD_FAIL("int main(void) { int a[3]; a++; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int a[3]; --a; return 0; }\n");
}

TEST(incdec, negative_record_operand)
{
    EXPECT_BUILD_FAIL("struct S { int x; };\n"
                      "int main(void) { struct S s; s.x = 1; s++; return 0; }\n");
}

TEST(incdec, negative_increment_of_binary)
{
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int b = 2; return (a + b)++; }\n");
}

TEST(incdec, compound_add)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x += 3;\n"
                          "    if (x != 8) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_sub)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x -= 2;\n"
                          "    if (x != 3) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_mul)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x *= 7;\n"
                          "    if (x != 35) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_div)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x /= 2;\n"
                          "    if (x != 2) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_mod)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x %= 2;\n"
                          "    if (x != 1) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_shl)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x <<= 4;\n"
                          "    if (x != 80) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_shr)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x >>= 1;\n"
                          "    if (x != 2) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_and)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x &= 12;\n"
                          "    if (x != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_or)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x |= 4;\n"
                          "    if (x != 5) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_xor)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    x ^= 2;\n"
                          "    if (x != 7) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_unsigned_div)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned u = 13;\n"
                          "    u /= 2;\n"
                          "    if (u != 6) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_unsigned_mod)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned u = 13;\n"
                          "    u %= 5;\n"
                          "    if (u != 3) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_unsigned_shr)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned v = 255;\n"
                          "    v >>= 1;\n"
                          "    if (v != 127) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_char_ashr)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s = -8;\n"
                          "    s >>= 1;\n"
                          "    if (s != -4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_int_shr)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r = 16;\n"
                          "    r >>= 2;\n"
                          "    if (r != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_int_shl)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int y = 15;\n"
                          "    y <<= 4;\n"
                          "    if (y != 240) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, narrow_shift_promotes_operands)
{
    /* C11 §6.5.7: the integer promotions are performed on each shift operand. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char s = -8;\n"
                          "    if ((s >> 1) != -4) return 1;\n"
                          "    unsigned char u = 240;\n"
                          "    if ((u >> 1) != 120) return 2;\n"
                          "    if ((u << 2) != 960) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_signed_char_wrap)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = 120;\n"
                          "    int r = (c += 10);\n"
                          "    if (c != -126) return 1;\n"
                          "    if (r != -126) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_unsigned_char_wrap)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned char u = 250;\n"
                          "    u += 10;\n"
                          "    if (u != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_pointer_scale)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "int main(void) {\n"
                          "    int a[5] = {0, 1, 2, 3, 4};\n"
                          "    int *p = &a[0];\n"
                          "    p += 2;\n"
                          "    if (*p != 2) return 1;\n"
                          "    p -= 1;\n"
                          "    if (*p != 1) return 2;\n"
                          "    struct S arr[2];\n"
                          "    arr[0].x = 1; arr[0].y = 2;\n"
                          "    arr[1].x = 3; arr[1].y = 4;\n"
                          "    struct S *sp = &arr[0];\n"
                          "    sp->x += 41;\n"
                          "    sp++;\n"
                          "    sp->y += 1;\n"
                          "    if (arr[0].x != 42) return 3;\n"
                          "    if (arr[1].y != 5) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_single_eval)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[4] = {10, 20, 30, 40};\n"
                          "    int i = 0;\n"
                          "    a[i++] += 100;\n"
                          "    a[i] -= 5;\n"
                          "    if (a[0] != 110) return 1;\n"
                          "    if (i != 1) return 2;\n"
                          "    if (a[1] != 15) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_add_self)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 3;\n"
                          "    x += x;\n"
                          "    if (x != 6) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_mul_self)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 6;\n"
                          "    x *= x;\n"
                          "    if (x != 36) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_member_add)
{
    EXPECT_INTERP_AND_ELF("struct S { int v; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    s.v = 10;\n"
                          "    s.v += 1;\n"
                          "    if (s.v != 11) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_arrow_mul)
{
    EXPECT_INTERP_AND_ELF("struct S { int v; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    struct S *p = &s;\n"
                          "    s.v = 11;\n"
                          "    p->v *= 2;\n"
                          "    if (s.v != 22) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_deref_add)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[1] = {1};\n"
                          "    int *q = &arr[0];\n"
                          "    (*q) += 41;\n"
                          "    if (arr[0] != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_subscript_or)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[2] = {1, 2};\n"
                          "    arr[1] |= 4;\n"
                          "    if (arr[1] != 6) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_global_add)
{
    EXPECT_INTERP_AND_ELF("int g = 40;\n"
                          "int main(void) {\n"
                          "    g += 2;\n"
                          "    if (g != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_spilled_global_and_static)
{
    EXPECT_INTERP_AND_ELF("int bump(void) {\n"
                          "    static int s = 41;\n"
                          "    s += 1;\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 30;\n"
                          "    int *p = &x;\n"
                          "    x += 10;\n"
                          "    if (x != 40) return 1;\n"
                          "    if (*p != 40) return 2;\n"
                          "    *p |= 2;\n"
                          "    if (x != 42) return 3;\n"
                          "    if (bump() != 42) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, compound_rvalue_result)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    int y = (x += 3);\n"
                          "    if (y != 8) return 1;\n"
                          "    if (x != 8) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(incdec, negative_compound_const)
{
    EXPECT_BUILD_FAIL("int main(void) { const int c = 1; c += 2; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { const int c = 1; c *= 3; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; const int *p = &x; *p += 1; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int x = 1; int *const p = &x; p -= 1; return 0; }\n");
}

TEST(incdec, negative_compound_const_typedef_pointer)
{
    EXPECT_BUILD_FAIL("typedef int *IP;\n"
                      "int main(void) { int x = 1; const IP p = &x; p += 1; return 0; }\n");
}

TEST(incdec, negative_compound_ptr_bad_op)
{
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int *p = &a; p *= 2; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int *p = &a; p /= 2; return 0; }\n");
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int *p = &a; p &= 1; return 0; }\n");
}

TEST(incdec, negative_compound_ptr_minus_ptr)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a = 1; int b = 2;\n"
                      "    int *p = &a; int *q = &b;\n"
                      "    p -= q;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(incdec, negative_compound_record)
{
    EXPECT_BUILD_FAIL("struct S { int x; };\n"
                      "int main(void) { struct S s; s.x = 1; s += 1; return 0; }\n");
}

TEST(incdec, negative_compound_array)
{
    EXPECT_BUILD_FAIL("int main(void) { int a[3]; a += 1; return 0; }\n");
}

TEST(incdec, negative_compound_rvalue_lhs)
{
    EXPECT_BUILD_FAIL("int main(void) { int a = 1; int b = 2; (a + b) += 3; return 0; }\n");
}
