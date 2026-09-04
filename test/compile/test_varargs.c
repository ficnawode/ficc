#include "harness.h"
#include "testdriver.h"

/* Phase 15: variadic functions. Rows added per sub-phase; 15a seeds the
   "variadic definition that ignores its varargs" floor: every program must
   produce the same result through the interpreter and the compiled ELF. */

TEST(varargs, variadic_def_ignores_extra_args)
{
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(1, 2, 3, 4);\n"
                          "}\n",
                          1);
}

TEST(varargs, variadic_multi_named_extra_args)
{
    EXPECT_INTERP_AND_ELF("int f(int a, int b, ...) {\n"
                          "    return a + b;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(20, 22, 9, 8, 7);\n"
                          "}\n",
                          42);
}

TEST(varargs, variadic_only_named_args)
{
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(42);\n"
                          "}\n",
                          42);
}