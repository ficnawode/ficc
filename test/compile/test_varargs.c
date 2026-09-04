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

TEST(varargs, va_start_fields)
{
    /* __builtin_va_start writes gp_offset = named*8 and fp_offset = 48 (no xmm use);
       both are linkage-independent constants, so interp == ELF. */
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, a);\n"
                          "    return *(int *)ap + *(int *)((char *)ap + 4);\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(7, 2, 3);\n"
                          "}\n",
                          56);
}

TEST(varargs, reg_save_area_spilled_args)
{
    /* The six GP registers are captured into the save area (reg_save_area)
       before param shuffle; the first three slots hold the raw incoming
       values of f's args. Shared memory in both backends -> interp == ELF. */
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, a);\n"
                          "    char *regs = *(char **)((char *)ap + 16);\n"
                          "    return *(int *)regs + *(int *)(regs + 8) + *(int *)(regs + 16);\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(1, 20, 21);\n"
                          "}\n",
                          42);
}

TEST(varargs, overflow_arg_area_after_named_stack_args)
{
    /* Six named params exhaust the GP area (gp_offset = 48), so the first
       unnamed arg comes from the stack region; overflow_arg_area skips the
       named stack args (here: none past the GP area, skip = 0). */
    EXPECT_INTERP_AND_ELF("int f(int a, int b, int c, int d, int e, int g, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, g);\n"
                          "    char *ovf = *(char **)((char *)ap + 8);\n"
                          "    return *(int *)ovf;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(1, 2, 3, 4, 5, 6, 42);\n"
                          "}\n",
                          42);
}

TEST(varargs, va_end_runs)
{
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, a);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(42);\n"
                          "}\n",
                          42);
}