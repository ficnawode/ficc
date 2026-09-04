#include "harness.h"
#include "testdriver.h"

#include "util/arena.h"

/* --- Phase 14b: _Static_assert — compile-time constant-expression check. --- */

TEST(static_assert, file_scope_pass)
{
    EXPECT_BUILD_SUCCEED("_Static_assert(1, \"true\");\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(static_assert, block_scope_pass)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    _Static_assert(1, \"true\");\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(static_assert, sizeof_expr)
{
    EXPECT_BUILD_SUCCEED("_Static_assert(sizeof(int) == 4, \"int\");\n"
                         "_Static_assert(sizeof(long) == 8, \"long\");\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(static_assert, alignof_expr)
{
    EXPECT_BUILD_SUCCEED("_Static_assert(_Alignof(long) == 8, \"align\");\n"
                         "int main(void) {\n"
                         "    _Static_assert(_Alignof(char) == 1, \"char\");\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(static_assert, arithmetic_and_cast_ice)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    _Static_assert((1 + 2) * 3 == 9, \"arith\");\n"
                         "    _Static_assert((char)300 == 44, \"wraps\");\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(static_assert, folded_with_runtime_program)
{
    EXPECT_INTERP_AND_ELF("_Static_assert(sizeof(long) == 8, \"pre\");\n"
                          "int main(void) {\n"
                          "    _Static_assert(sizeof(int) == 4, \"fn\");\n"
                          "    return _Alignof(long);\n"
                          "}\n",
                          8);
}

TEST(static_assert, between_declarations_file_scope)
{
    EXPECT_BUILD_SUCCEED("int a = 1;\n"
                         "_Static_assert(1, \"mid\");\n"
                         "int b = 2;\n"
                         "int main(void) {\n"
                         "    return a + b;\n"
                         "}\n");
}

TEST(static_assert, false_at_file_scope)
{
    EXPECT_BUILD_FAIL("_Static_assert(0, \"boom\");\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(static_assert, false_at_block_scope)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    _Static_assert(3 < 2, \"nope\");\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(static_assert, non_constant_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 5;\n"
                      "    _Static_assert(x == 5, \"not an ICE\");\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(static_assert, non_string_message_rejected)
{
    EXPECT_BUILD_FAIL("_Static_assert(1, 42);\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(static_assert, empty_message_ok)
{
    EXPECT_BUILD_SUCCEED("_Static_assert(1, \"\");\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}