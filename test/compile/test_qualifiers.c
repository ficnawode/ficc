#include "harness.h"
#include "testdriver.h"

TEST(qualifiers, volatile_local_write_read)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    volatile int y = 0;\n"
                          "    volatile int *p = &y;\n"
                          "    *p = 42;\n"
                          "    return y;\n"
                          "}\n",
                          42);
}

TEST(qualifiers, volatile_global_increment_loop)
{
    EXPECT_INTERP_AND_ELF("volatile unsigned long counter;\n"
                          "int main(void) {\n"
                          "    unsigned long i;\n"
                          "    for (i = 0; i < 10; i++)\n"
                          "        counter++;\n"
                          "    return (int) counter;\n"
                          "}\n",
                          10);
}

TEST(qualifiers, volatile_params_arithmetic)
{
    EXPECT_INTERP_AND_ELF("int add(volatile int a, volatile int b) { return a + b; }\n"
                          "int main(void) { return add(20, 22); }\n",
                          42);
}

TEST(qualifiers, volatile_pointer_target)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 10;\n"
                          "    const volatile int *cp = &x;\n"
                          "    volatile int *vp = (volatile int *) cp;\n"
                          "    *vp = 3;\n"
                          "    return x;\n"
                          "}\n",
                          3);
}

TEST(qualifiers, restrict_params_copy)
{
    EXPECT_INTERP_AND_ELF("void copy(void *restrict dest, const void *restrict src, int n) {\n"
                          "    char *d = dest;\n"
                          "    const char *s = src;\n"
                          "    int i;\n"
                          "    for (i = 0; i < n; i++)\n"
                          "        d[i] = s[i];\n"
                          "}\n"
                          "int main(void) {\n"
                          "    char dst[3] = {0};\n"
                          "    copy(dst, \"hi\", 2);\n"
                          "    return dst[1] == 'i' && dst[2] == 0 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(qualifiers, restrict_local_pointers)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 7;\n"
                          "    int *restrict rp = &x;\n"
                          "    *rp = 8;\n"
                          "    return x;\n"
                          "}\n",
                          8);
}

TEST(qualifiers, volatile_deref_in_loop)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    volatile int x = 0;\n"
                          "    volatile int *p = &x;\n"
                          "    for (int i = 0; i < 5; i = i + 1) *p = *p + 2;\n"
                          "    return x;\n"
                          "}\n",
                          10);
}

TEST(qualifiers, volatile_through_phi)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    volatile int v = 0;\n"
                          "    for (int i = 0; i < 3; i = i + 1) {\n"
                          "        if (i & 1) v = 10;\n"
                          "        else v = 20;\n"
                          "    }\n"
                          "    return v;\n"
                          "}\n",
                          20);
}

TEST(qualifiers, volatile_deref_pointer_casts)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    volatile char c = 'a';\n"
                          "    volatile char *cp = &c;\n"
                          "    char *p = (char *) cp;\n"
                          "    *p = 'z';\n"
                          "    return c == 'z' ? 0 : 1;\n"
                          "}\n",
                          0);
}

/* C11 §6.7.1p6: declaration specifiers are unordered, so a qualifier may precede the base type. */
TEST(qualifiers, qualifier_between_type_specifiers)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    unsigned const char c = 200;\n"
                          "    unsigned const int i = 7;\n"
                          "    const unsigned long l = 9;\n"
                          "    return (c == 200 && i == 7 && l == 9) ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(qualifiers, volatile_struct_field)
{
    EXPECT_INTERP_AND_ELF("struct S { volatile int v; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    s.v = 42;\n"
                          "    return s.v;\n"
                          "}\n",
                          42);
}

TEST(qualifiers, volatile_array_elements)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    volatile int a[2];\n"
                          "    a[0] = 40;\n"
                          "    a[1] = 2;\n"
                          "    if (a[0] + a[1] != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}
