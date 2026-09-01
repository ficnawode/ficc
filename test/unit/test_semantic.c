#include "harness.h"
#include "testdriver.h"

TEST(semantic, ok_single_func)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    return 42;\n"
                         "}");
}

TEST(semantic, duplicate_function)
{
    EXPECT_BUILD_FAIL("int f(void) {\n"
                      "    return 1;\n"
                      "}\n"
                      "int f(void) {\n"
                      "    return 2;\n"
                      "}");
}

TEST(semantic, undeclared_var)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return x;\n"
                      "}");
}

TEST(semantic, redeclaration_local)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x;\n"
                      "    int x;\n"
                      "    return x;\n"
                      "}");
}

TEST(semantic, undeclared_function_call)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return foo();\n"
                      "}");
}

TEST(semantic, wrong_arity)
{
    EXPECT_BUILD_FAIL("int foo(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return foo();\n"
                      "}");
}

TEST(semantic, param_used_ok)
{
    EXPECT_BUILD_SUCCEED("int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}");
}

TEST(semantic, void_return_with_value)
{
    EXPECT_BUILD_FAIL("void f(void) {\n"
                      "    return 1;\n"
                      "}");
}

TEST(semantic, nonvoid_return_without_value)
{
    EXPECT_BUILD_FAIL("int f(void) {\n"
                      "    return;\n"
                      "}");
}

TEST(semantic, assignment_undeclared)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    x = 5;\n"
                      "    return x;\n"
                      "}");
}

TEST(semantic, call_across_functions)
{
    EXPECT_BUILD_SUCCEED("int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return add(1, 2);\n"
                         "}");
}

TEST(semantic, if_else_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int x;\n"
                         "    if (1) {\n"
                         "        x = 10;\n"
                         "    } else {\n"
                         "        x = 20;\n"
                         "    }\n"
                         "    return x;\n"
                         "}");
}

TEST(semantic, if_undeclared_cond)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    if (x) {\n"
                      "        return 1;\n"
                      "    }\n"
                      "    return 0;\n"
                      "}");
}

TEST(semantic, nested_shadowing_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int x;\n"
                         "    {\n"
                         "        int x;\n"
                         "        return x;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}");
}

TEST(semantic, param_shadow_nested_ok)
{
    EXPECT_BUILD_SUCCEED("int f(int x) {\n"
                         "    {\n"
                         "        int x;\n"
                         "        return x;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}");
}

TEST(semantic, param_body_redeclaration_error)
{
    EXPECT_BUILD_FAIL("int f(int x) {\n"
                      "    int x;\n"
                      "    return 0;\n"
                      "}");
}
