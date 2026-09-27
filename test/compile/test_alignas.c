#include "harness.h"
#include "testdriver.h"

#include "util/arena.h"

TEST(alignas, file_scope_positions)
{
    EXPECT_INTERP_AND_ELF("_Alignas(16) int g1 = 5;\n"
                          "static _Alignas(8) int g2 = 6;\n"
                          "int _Alignas(32) g3 = 7;\n"
                          "const _Alignas(16) int g4 = 8;\n"
                          "_Alignas(16) const int g5 = 9;\n"
                          "int main(void) {\n"
                          "    if (g1 != 5) return 1;\n"
                          "    if (g2 != 6) return 2;\n"
                          "    if (g3 != 7) return 3;\n"
                          "    if (g4 != 8) return 4;\n"
                          "    if (g5 != 9) return 5;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignas, block_scope_positions)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Alignas(16) int x = 1;\n"
                          "    static _Alignas(8) int s = 2;\n"
                          "    int _Alignas(4) y = 3;\n"
                          "    const _Alignas(8) int cx = 4;\n"
                          "    if (x != 1) return 1;\n"
                          "    if (s != 2) return 2;\n"
                          "    if (y != 3) return 3;\n"
                          "    if (cx != 4) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignas, type_name_form)
{
    EXPECT_INTERP_AND_ELF("_Alignas(long) int gl = 11;\n"
                          "int main(void) {\n"
                          "    _Alignas(int) int x = 12;\n"
                          "    if (gl != 11) return 1;\n"
                          "    if (x != 12) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignas, struct_member_accepted)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct S { _Alignas(16) int f; };\n"
                          "    struct S s = {7};\n"
                          "    if (s.f != 7) return 1;\n"
                          "    s.f = 9;\n"
                          "    if (s.f != 9) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignas, repeated_specifiers_combine)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Alignas(8) _Alignas(16) int x = 3;\n"
                          "    if (x != 3) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignas, over_natural_alignment_accepted)
{
    EXPECT_INTERP_AND_ELF("_Alignas(64) int g = 7;\n"
                          "int main(void) {\n"
                          "    if (g != 7) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(alignas, zero_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    _Alignas(0) int x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(alignas, non_power_of_two_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    _Alignas(3) int x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(alignas, non_constant_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int v = 4;\n"
                      "    _Alignas(v) int x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(alignas, typedef_rejected)
{
    EXPECT_BUILD_FAIL("typedef _Alignas(16) int T;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(alignas, function_rejected)
{
    EXPECT_BUILD_FAIL("_Alignas(16) int f(void) {\n"
                      "    return 0;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}