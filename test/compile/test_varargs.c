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

TEST(varargs, variadic_call_overflows_reg_area)
{
    /* Seven trailing args: the first five ride in GP regs, the rest go to the
       caller's stack; a variadic call site must still work with the %al
       zeroing in between (interp == ELF). */
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(1, 2, 3, 4, 5, 6, 7);\n"
                          "}\n",
                          1);
}

TEST(varargs, variadic_tail_small_types_promote)
{
    /* Default argument promotions (§6.5.2.2p7) run on the variadic tail in
       the IR builder; char/_Bool/unsigned char re-rank to int before the
       call. Not yet readable on the callee side, so interp == ELF is the
       floor. */
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    char c = 3;\n"
                          "    _Bool b = 1;\n"
                          "    return f(1, c, b);\n"
                          "}\n",
                          1);
}