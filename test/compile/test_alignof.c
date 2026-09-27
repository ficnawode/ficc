#include "harness.h"
#include "testdriver.h"

#include "util/arena.h"

TEST(alignof, type_forms)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (_Alignof(char) != 1) return 1;\n"
                          "    if (_Alignof(short) != 2) return 2;\n"
                          "    if (_Alignof(int) != 4) return 3;\n"
                          "    if (_Alignof(long) != 8) return 4;\n"
                          "    if (_Alignof(long long) != 8) return 5;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, pointer_and_array_forms)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (_Alignof(int *) != 8) return 1;\n"
                          "    if (_Alignof(int[4]) != 4) return 2;\n"
                          "    if (_Alignof(char[10]) != 1) return 3;\n"
                          "    if (_Alignof(int **) != 8) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, array_direct_operand_no_decay)
{
    /* C11 §6.3.2.1p3: array-to-pointer decay is suppressed for _Alignof, so
       this is the element alignment. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[4];\n"
                          "    if (_Alignof(a) != 4) return 1;\n"
                          "    if (_Alignof(a) == _Alignof(int *) ) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, record_forms)
{
    EXPECT_INTERP_AND_ELF("struct P { char c; long d; };\n"
                          "struct Q { char c; };\n"
                          "union U { char c; long l; };\n"
                          "int main(void) {\n"
                          "    if (_Alignof(struct P) != 8) return 1;\n"
                          "    if (_Alignof(struct Q) != 1) return 2;\n"
                          "    if (_Alignof(union U) != 8) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, typedef_type_name)
{
    EXPECT_INTERP_AND_ELF("typedef long LB;\n"
                          "typedef char CB[10];\n"
                          "int main(void) {\n"
                          "    if (_Alignof(LB) != 8) return 1;\n"
                          "    if (_Alignof(CB) != 1) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, const_qualified_forms)
{
    /* C11 §6.2.5p26: qualified and unqualified versions have the same
       alignment. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (_Alignof(const int) != _Alignof(int)) return 1;\n"
                          "    if (_Alignof(const long) != 8) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, expression_form)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long x;\n"
                          "    int y = 4;\n"
                          "    if (_Alignof(x) != 8) return 1;\n"
                          "    if (_Alignof(y) != 4) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, case_label_constant)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long x;\n"
                          "    switch (8) {\n"
                          "        case _Alignof(x): return 0;\n"
                          "        default: return 1;\n"
                          "    }\n"
                          "}\n",
                          0);
}

TEST(alignof, folds_in_file_scope_initializer)
{
    EXPECT_INTERP_AND_ELF("int ga = _Alignof(int);\n"
                          "int gl = _Alignof(long);\n"
                          "int main(void) {\n"
                          "    if (ga != 4) return 1;\n"
                          "    if (gl != 8) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, folds_in_arithmetic)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a = _Alignof(long) / _Alignof(char);\n"
                          "    if (a != 8) return 1;\n"
                          "    if ((int) _Alignof(short) != 2) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, folds_in_cast)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = (char) _Alignof(long);\n"
                          "    if (c != 8) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, floating_type_forms)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (_Alignof(float) != 4) return 1;\n"
                          "    if (_Alignof(double) != 8) return 2;\n"
                          "    if (_Alignof(long double) != 16) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignof, void_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return (int) _Alignof(void);\n"
                      "}\n");
}

TEST(alignof, incomplete_record_rejected)
{
    EXPECT_BUILD_FAIL("struct S;\n"
                      "int main(void) {\n"
                      "    return (int) _Alignof(struct S);\n"
                      "}\n");
}

TEST(alignof, unsized_array_rejected)
{
    /* `int[]` without a size is an incomplete type (§6.5.3.4p1). */
    EXPECT_BUILD_FAIL("extern int a[];\n"
                      "int main(void) {\n"
                      "    return (int) _Alignof(a);\n"
                      "}\n");
}