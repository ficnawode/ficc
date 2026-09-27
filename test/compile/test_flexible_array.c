#include "harness.h"
#include "testdriver.h"

/* A flexible array member (C11 §6.7.2.1p18) contributes no bytes: sizeof
   stops at its aligned offset. */

TEST(flexible_array, sizeof_excludes_member)
{
    EXPECT_INTERP_AND_ELF("struct S { int n; char d[]; };\n"
                          "int main(void) {\n"
                          "    if (sizeof(struct S) != 4) return 1;\n"
                          "    if (_Alignof(struct S) != 4) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(flexible_array, offset_aligned_to_element)
{
    EXPECT_INTERP_AND_ELF("struct T { char c; int d[]; };\n"
                          "int main(void) {\n"
                          "    if (sizeof(struct T) != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(flexible_array, member_access_through_pointer)
{
    EXPECT_INTERP_AND_ELF("struct S { int n; char d[]; };\n"
                          "int main(void) {\n"
                          "    char storage[16];\n"
                          "    struct S *p = (struct S *) storage;\n"
                          "    p->n = 3;\n"
                          "    for (int i = 0; i < 3; i++)\n"
                          "        p->d[i] = (char) (i + 1);\n"
                          "    return p->d[0] + p->d[1] + p->d[2];\n"
                          "}\n",
                          6);
}

TEST(flexible_array, negative_not_last)
{
    EXPECT_BUILD_FAIL("struct S { char d[]; int n; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(flexible_array, negative_in_union)
{
    EXPECT_BUILD_FAIL("union U { int n; char d[]; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(flexible_array, member_with_fam_is_allowed)
{
    EXPECT_INTERP_AND_ELF("struct A { int n; char d[]; };\n"
                          "struct B { struct A a; };\n"
                          "int main(void) {\n"
                          "    if (sizeof(struct B) != 4) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(flexible_array, negative_only_member)
{
    EXPECT_BUILD_FAIL("struct S { char d[]; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(flexible_array, negative_array_of_struct_with_member)
{
    EXPECT_BUILD_FAIL("struct A { int n; char d[]; };\n"
                      "struct A arr[2];\n"
                      "int main(void) { return 0; }\n");
}

TEST(flexible_array, negative_incomplete_element)
{
    EXPECT_BUILD_FAIL("struct I;\n"
                      "struct S { int n; struct I d[]; };\n"
                      "int main(void) { return 0; }\n");
}
