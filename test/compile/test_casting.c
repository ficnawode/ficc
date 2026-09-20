#include "harness.h"
#include "testdriver.h"

/* Phase 11 casting: every program must produce the same result through the
   interpreter and through the compiled ELF. Checks return 42 on success and a
   distinct small code per guard, so a divergence names its area. */

TEST(casting, narrow_signed_positive_wrap)
{
    /* 300 fits neither byte: signed wrap to 44 (mod 256). */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((char)300 != 44) return 1;\n"
                          "    if ((int)(char)300 != 44) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, narrow_signed_negative)
{
    /* Two's-complement wrap into the signed range. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((char)255 != -1) return 1;\n"
                          "    if ((char)128 != -128) return 2;\n"
                          "    if ((char)44 != 44) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, narrow_unsigned)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((unsigned char)300 != 44) return 1;\n"
                          "    if ((unsigned char)-1 != 255) return 2;\n"
                          "    if ((unsigned char)200 != 200) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, narrow_short)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((short)70000 != 4464) return 1;\n"
                          "    if ((unsigned short)-1 != 65535) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, same_width_signedness_flip)
{
    /* (unsigned)-1 is 4294967295; cast then read the unsigned value. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned int u = (unsigned int)-1;\n"
                          "    if (u != 4294967295) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, widen_signed)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int neg = -7;\n"
                          "    if ((long)neg != -7) return 1;\n"
                          "    if ((unsigned long)-1 < 1000000) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, widen_unsigned)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned int u = (unsigned int)-1;\n"
                          "    if ((unsigned long)u != 4294967295) return 1;\n"
                          "    if ((long)(unsigned)0xFFFFFFFF != 4294967295) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, char_roundtrip_expression)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((char)300 + 1 != 45) return 1;\n"
                          "    int a = (char)300;\n"
                          "    a = a + 2;\n"
                          "    if (a != 46) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, int_to_void_roundtrip)
{
    /* Integer → pointer → integer round trip (implementation-defined, LP64:
       sign/zero-extend by source signedness, truncate going back). */
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    if ((unsigned long)(void *)(unsigned long)0x1234 != 0x1234) return 1;\n"
        "    return 42;\n"
        "}\n",
        42);
}

TEST(casting, deref_of_null_cast)
{
    /* `&*(int *)0` is the null-pointer idiom; the cast feeds a dereference. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int *p = &*(int *)0;\n"
                          "    if (p != 0) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, ptr_to_ptr_reinterpret)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    int *p = &x;\n"
                          "    char *c = (char *)p;\n"
                          "    if (*(int *)c != 5) return 1;\n"
                          "    void *v = (void *)p;\n"
                          "    if (*(int *)v != 5) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, ptr_int_reinterpret_bytes)
{
    /* Byte-wise write, integer read back: exercises int↔ptr reinterprets. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char arr[4];\n"
                          "    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;\n"
                          "    int v = *(int *)arr;\n"
                          "    if (v != 0x04030201) return 1;\n"
                          "    char *back = (char *)&v;\n"
                          "    if (back[0] != 1 || back[3] != 4) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, spill_interplay)
{
    /* `&x` spills x to a slot; the cast chain writes through it. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    *(int *)(void *)&x = 9;\n"
                          "    if (x != 9) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, addr_of_cast_deref)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 7;\n"
                          "    int *p = &*(int *)&x;\n"
                          "    if (*p != 7) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_of_const_rvalue)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const int ci = 7;\n"
                          "    int y = (int)ci;\n"
                          "    if (y != 7) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, const_pointer_cast_add)
{
    /* Adding pointee const via a cast is a legal conversion. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    int *q = &x;\n"
                          "    const int *p = (const int *)q;\n"
                          "    if (*p != 5) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_away_const_reads)
{
    /* Dropping pointee const is a legal conversion; UB only if the object is
       then *modified* — this test only reads, so it is well-defined C. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const int ci = 7;\n"
                          "    const int *cp = &ci;\n"
                          "    int *rp = (int *)(void *)cp;\n"
                          "    if (*rp != 7) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, void_cast_statement)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 5;\n"
                          "    (void)x;\n"
                          "    (void)123;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, void_cast_of_struct)
{
    /* `(void)` accepts any operand (§6.5.4p2); the value is discarded. */
    EXPECT_INTERP_AND_ELF("struct s { int a; int b; };\n"
                          "int main(void) {\n"
                          "    struct s v;\n"
                          "    v.b = 3;\n"
                          "    (void)v;\n"
                          "    struct s w;\n"
                          "    w = v;\n"
                          "    if (w.b != 3) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, void_cast_of_void_call)
{
    EXPECT_INTERP_AND_ELF("void g(void) { }\n"
                          "int main(void) {\n"
                          "    (void)g();\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, enum_cast)
{
    EXPECT_INTERP_AND_ELF("enum color { RED, GREEN, BLUE };\n"
                          "int main(void) {\n"
                          "    enum color c = (enum color)2;\n"
                          "    if ((int)c != BLUE) return 1;\n"
                          "    int i = (int)c;\n"
                          "    enum color d = (enum color)i;\n"
                          "    if ((int)d != 2) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, enum_cast_in_call_arg)
{
    EXPECT_INTERP_AND_ELF("enum mode { MO_A, MO_B };\n"
                          "int take(enum mode m) { return (int)m; }\n"
                          "int main(void) {\n"
                          "    if (take((enum mode)1) != MO_B) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, struct_ptr_cast_member_access)
{
    EXPECT_INTERP_AND_ELF("struct point { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct point pt;\n"
                          "    pt.x = 11;\n"
                          "    pt.y = 22;\n"
                          "    char *base = (char *)&pt;\n"
                          "    struct point *q = (struct point *)base;\n"
                          "    if (q->y != 22) return 1;\n"
                          "    if ((char)pt.x != 11) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, sizeof_of_cast)
{
    /* sizeof's operand is not evaluated; `(int)y` is fine there. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int y = 42;\n"
                          "    if (sizeof((int)y) != 4) return 1;\n"
                          "    if (sizeof((char)y) != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, array_decay_cast)
{
    /* `(unsigned long)arr` casts the decayed pointer. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[4];\n"
                          "    unsigned long base = (unsigned long)arr;\n"
                          "    unsigned long first = (unsigned long)&arr[0];\n"
                          "    unsigned long second = (unsigned long)&arr[1];\n"
                          "    if (base != first) return 1;\n"
                          "    if (second != base + 4) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_in_ternary)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 300;\n"
                          "    int r = x > 100 ? (char)x : 99;\n"
                          "    if (r != 44) return 1;\n"
                          "    r = x < 100 ? 99 : (unsigned char)-1;\n"
                          "    if (r != 255) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_case_labels)
{
    /* Casts are legal inside case-label integer constant expressions. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int r = 0;\n"
                          "    switch ((char)300) {\n"
                          "    case (int)(char)300:\n"
                          "        r = 42;\n"
                          "        break;\n"
                          "    default:\n"
                          "        r = 1;\n"
                          "        break;\n"
                          "    }\n"
                          "    if (r != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_case_sizeof_local)
{
    /* `(int)sizeof(arr)` needs a type known only at semantic time. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int arr[10];\n"
                          "    int r = 0;\n"
                          "    switch (40) {\n"
                          "    case (int)sizeof(arr):\n"
                          "        r = 42;\n"
                          "        break;\n"
                          "    default:\n"
                          "        r = 1;\n"
                          "        break;\n"
                          "    }\n"
                          "    if (r != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_in_loop)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int sum = 0;\n"
                          "    for (int i = 0; i < 3; i = i + 1) {\n"
                          "        sum = sum + (char)i;\n"
                          "    }\n"
                          "    if (sum != 3) return 1;\n"
                          "    int j = 0;\n"
                          "    while (j < 3) {\n"
                          "        j = j + 1;\n"
                          "    }\n"
                          "    if ((char)j != 3) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_of_global)
{
    EXPECT_INTERP_AND_ELF("unsigned char g = 200;\n"
                          "int main(void) {\n"
                          "    if ((int)g != 200) return 1;\n"
                          "    if ((char)g == 200) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, static_init_with_cast_constant)
{
    /* Casts fold in static/file-scope scalar constant initializers. */
    EXPECT_INTERP_AND_ELF("static int s = (char)300;\n"
                          "int main(void) {\n"
                          "    if (s != 44) return 1;\n"
                          "    static int t = (unsigned char)-1;\n"
                          "    if (t != 255) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, cast_of_pointer_param)
{
    EXPECT_INTERP_AND_ELF("int deref_as_int(int *p) {\n"
                          "    return *p;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 9;\n"
                          "    if (deref_as_int((int *)(void *)&x) != 9) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(casting, double_pointer_cast)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 3;\n"
                          "    int *p = &x;\n"
                          "    int **pp = &p;\n"
                          "    void **vp = (void **)pp;\n"
                          "    int **q = (int **)vp;\n"
                          "    if (**q != 3) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

/* negative: casts that must not compile */

TEST(casting, negative_cast_to_struct)
{
    EXPECT_BUILD_FAIL("struct s { int x; };\n"
                      "int main(void) {\n"
                      "    struct s v;\n"
                      "    (int)v;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(casting, negative_cast_of_struct)
{
    EXPECT_BUILD_FAIL("struct s { int x; };\n"
                      "int main(void) {\n"
                      "    int i;\n"
                      "    return (struct s)i;\n"
                      "}\n");
}

TEST(casting, negative_cast_assign_lvalue)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int i;\n"
                      "    (int)i = 1;\n"
                      "    return i;\n"
                      "}\n");
}

TEST(casting, negative_void_value_in_binary)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return (void)5 + 1;\n"
                      "}\n");
}

TEST(casting, negative_void_value_in_unary)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return !((void)5);\n"
                      "}\n");
}

/* A negative immediate keeps its sign when widened to a wider parameter. */
TEST(casting, negative_imm_sextended_into_wide_param)
{
    EXPECT_INTERP_AND_ELF("unsigned long long as_ull(long long v)\n"
                          "{\n"
                          "    return (unsigned long long)v;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return as_ull(-4) == 0xfffffffffffffffcULL ? 0 : 1;\n"
                          "}\n",
                          0);
}
