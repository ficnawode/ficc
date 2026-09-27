#include "harness.h"
#include "testdriver.h"

/* offsetof (C11 §7.19p3) folds to a compile-time integer constant. */

TEST(offsetof, fields_basic)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct S { int a; char b; int c; };\n"
                          "int main(void) {\n"
                          "    if (offsetof(struct S, a) != 0) return 1;\n"
                          "    if (offsetof(struct S, b) != 4) return 2;\n"
                          "    if (offsetof(struct S, c) != 8) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(offsetof, nested_struct)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct Inner { char c; long l; };\n"
                          "struct Outer { int a; struct Inner in; char x; };\n"
                          "int main(void) {\n"
                          "    if (offsetof(struct Outer, in) != 8) return 1;\n"
                          "    if (offsetof(struct Outer, in.l) != 16) return 2;\n"
                          "    if (offsetof(struct Outer, x) != 24) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(offsetof, in_static_array_size)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct S { char b; int c; long d; };\n"
                          "static int idx[offsetof(struct S, d)];\n"
                          "int main(void) {\n"
                          "    idx[7] = 42;\n"
                          "    if (idx[7] != 42) return 1;\n"
                          "    if (sizeof(idx) / sizeof(idx[0]) != 8) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(offsetof, in_static_initializer)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct S { int a; int b; };\n"
                          "static const int off_a = offsetof(struct S, a);\n"
                          "static const int off_b = offsetof(struct S, b);\n"
                          "int main(void) {\n"
                          "    if (off_a != 0) return 1;\n"
                          "    if (off_b != 4) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(offsetof, matches_address_arithmetic)
{
    EXPECT_INTERP_AND_ELF(
        "#include <stddef.h>\n"
        "struct S { char a; char b; char c; int d; };\n"
        "struct S s;\n"
        "int main(void) {\n"
        "    char *base = (char *) &s;\n"
        "    if ((char *) &s.d - base != (long) offsetof(struct S, d)) return 1;\n"
        "    return 0;\n"
        "}\n",
        0);
}

TEST(offsetof, array_member_subscript)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "struct S { int a[4]; };\n"
                          "int main(void) {\n"
                          "    if (offsetof(struct S, a[2]) != 8) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(offsetof, negative_unknown_member)
{
    EXPECT_BUILD_FAIL("#include <stddef.h>\n"
                      "struct S { int a; };\n"
                      "int main(void) {\n"
                      "    return (int) offsetof(struct S, z);\n"
                      "}\n");
}

TEST(offsetof, wchar_t_typedef_and_store)
{
    EXPECT_INTERP_AND_ELF("#include <stddef.h>\n"
                          "wchar_t w;\n"
                          "int main(void) {\n"
                          "    wchar_t local = (wchar_t) 65;\n"
                          "    w = local;\n"
                          "    return w == 65 && sizeof(w) == 4 ? 0 : 1;\n"
                          "}\n",
                          0);
}