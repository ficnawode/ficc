#include "harness.h"
#include "testdriver.h"

#include "util/arena.h"

/* Phase 14c: _Bool — 1-byte unsigned integer that normalizes to 0/1. */

TEST(bool, declare_and_normalize)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Bool a = 42;\n"
                          "    _Bool b = 0;\n"
                          "    _Bool c = -1;\n"
                          "    if (a != 1) return 1;\n"
                          "    if (b != 0) return 2;\n"
                          "    if (c != 1) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, store_normalizes_from_arith)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Bool b = 0;\n"
                          "    b = 7 - 2;\n"
                          "    if (b != 1) return 1;\n"
                          "    b = b + 1;\n"
                          "    if (b != 1) return 2;\n"
                          "    b = 0;\n"
                          "    if (b != 0) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, char_to_bool_and_back)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    char c = 0x77;\n"
                          "    _Bool b = c;\n"
                          "    if (b != 1) return 1;\n"
                          "    char c2 = b;\n"
                          "    if (c2 != 1) return 2;\n"
                          "    b = c2;\n"
                          "    if (b != 1) return 3;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, addresses_and_pointer_cast)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int *p = 0;\n"
                          "    _Bool bp = (_Bool)p;\n"
                          "    if (bp != 0) return 1;\n"
                          "    int x = 5;\n"
                          "    p = &x;\n"
                          "    if ((_Bool)p != 1) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, globals_normalize)
{
    EXPECT_INTERP_AND_ELF("_Bool g1 = 42;\n"
                          "_Bool g2 = 0;\n"
                          "_Bool g3 = -3;\n"
                          "const _Bool g4 = 7;\n"
                          "int main(void) {\n"
                          "    if (g1 != 1) return 1;\n"
                          "    if (g2 != 0) return 2;\n"
                          "    if (g3 != 1) return 3;\n"
                          "    if (g4 != 1) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, block_static_normalizes)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static _Bool s = 33;\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    if (f() != 1) return 1;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, arrays_and_records)
{
    EXPECT_INTERP_AND_ELF("struct S { _Bool f; int n; };\n"
                          "int main(void) {\n"
                          "    _Bool arr[3] = {17, 0, 1};\n"
                          "    if (arr[0] != 1) return 1;\n"
                          "    if (arr[1] != 0) return 2;\n"
                          "    if (arr[2] != 1) return 3;\n"
                          "    struct S s = {9, 10};\n"
                          "    if (s.f != 1) return 4;\n"
                          "    if (s.n != 10) return 5;\n"
                          "    struct S t = s;\n"
                          "    if (t.f != 1) return 6;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, params_and_returns)
{
    EXPECT_INTERP_AND_ELF("_Bool make(int v) {\n"
                          "    return v;\n"
                          "}\n"
                          "_Bool pass(_Bool x) {\n"
                          "    return x;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    if (pass(1) != 1) return 1;\n"
                          "    if (pass(0) != 0) return 2;\n"
                          "    if (make(5) != 1) return 3;\n"
                          "    if (make(0) != 0) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, casts_fold_and_run)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if ((_Bool)0 != 0) return 1;\n"
                          "    if ((_Bool)257 != 1) return 2;\n"
                          "    if ((_Bool)-8 != 1) return 3;\n"
                          "    int x = 5;\n"
                          "    if ((_Bool)x != 1) return 4;\n"
                          "    x = 0;\n"
                          "    if ((_Bool)x != 0) return 5;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, logic_and_switch)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Bool b = (5 > 3);\n"
                          "    if (b != 1) return 1;\n"
                          "    if (!b) return 2;\n"
                          "    if (b && 1 != 1) return 3;\n"
                          "    switch ((_Bool)2) {\n"
                          "        case 1: return 0;\n"
                          "        default: return 4;\n"
                          "    }\n"
                          "}\n",
                          0);
}

TEST(bool, incdec_and_compound_assign)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    _Bool b = 0;\n"
                          "    b++;\n"
                          "    ++b;\n"
                          "    if (b != 1) return 1;\n"
                          "    b--;\n"
                          "    if (b != 0) return 2;\n"
                          "    b += 1;\n"
                          "    if (b != 1) return 3;\n"
                          "    b *= 0;\n"
                          "    if (b != 0) return 4;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, basic_type_query)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (sizeof(_Bool) != 1) return 1;\n"
                          "    if (_Alignof(_Bool) != 1) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(bool, const_write_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const _Bool cb = 1;\n"
                      "    cb = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(bool, bool_name_stays_identifier)
{
    /* bool/true/false arrive later via <stdbool.h> (Phase 17). */
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    bool b = 1;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(bool, no_mixing_with_integer_specifiers)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    _Bool unsigned b;\n"
                      "    return 0;\n"
                      "}\n");
}