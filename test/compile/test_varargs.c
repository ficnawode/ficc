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

TEST(varargs, sum_over_named_regs)
{
    EXPECT_INTERP_AND_ELF("int sum(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    int t = 0;\n"
                          "    for (int i = 0; i < n; i++) {\n"
                          "        t += __builtin_va_arg(ap, int);\n"
                          "    }\n"
                          "    __builtin_va_end(ap);\n"
                          "    return t;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return sum(3, 10, 20, 30);\n"
                          "}\n",
                          60);
}

TEST(varargs, sum_zero_args)
{
    EXPECT_INTERP_AND_ELF("int sum(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    int t = 0;\n"
                          "    for (int i = 0; i < n; i++) {\n"
                          "        t += __builtin_va_arg(ap, int);\n"
                          "    }\n"
                          "    __builtin_va_end(ap);\n"
                          "    return t;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return sum(0);\n"
                          "}\n",
                          0);
}

TEST(varargs, sum_crosses_into_overflow)
{
    /* Nine trailing args: five ride GP slots past the named `n` (rdi), the
       remaining seven spill to the caller's stack. The walk must cross the
       gp >= 48 branch and keep reading through the overflow region. */
    EXPECT_INTERP_AND_ELF("int sum(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    int t = 0;\n"
                          "    for (int i = 0; i < n; i++) {\n"
                          "        t += __builtin_va_arg(ap, int);\n"
                          "    }\n"
                          "    __builtin_va_end(ap);\n"
                          "    return t;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return sum(9, 1,2,3,4,5,6,7,8,9);\n"
                          "}\n",
                          45);
}

TEST(varargs, char_varargs_read)
{
    /* Default promotions rank char up to int at the call site; va_arg with a
       char target truncates the promoted slot back to its byte. */
    EXPECT_INTERP_AND_ELF("int f(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    char a = __builtin_va_arg(ap, char);\n"
                          "    char b = __builtin_va_arg(ap, char);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return a + b;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(2, (char)40, (char)2);\n"
                          "}\n",
                          42);
}

TEST(varargs, pointer_varargs_read)
{
    EXPECT_INTERP_AND_ELF("int f(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    int a = *__builtin_va_arg(ap, int *);\n"
                          "    int b = *__builtin_va_arg(ap, int *);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return a + b;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 20;\n"
                          "    int y = 22;\n"
                          "    return f(2, &x, &y);\n"
                          "}\n",
                          42);
}

TEST(varargs, mixed_type_sequence)
{
    EXPECT_INTERP_AND_ELF("int f(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    int i = __builtin_va_arg(ap, int);\n"
                          "    char c = __builtin_va_arg(ap, char);\n"
                          "    long l = __builtin_va_arg(ap, long);\n"
                          "    int *p = __builtin_va_arg(ap, int *);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return i + c + (int)l + *p;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 2;\n"
                          "    return f(4, 20, (char)7, (long)13, &x);\n"
                          "}\n",
                          42);
}

TEST(varargs, va_list_passed_to_helper)
{
    /* The passing pattern: a helper reads a va_list that the caller
       va_start'd, so the same ap object is walked from a different frame.
       Both backends must advance gp_offset identically across the handoff. */
    EXPECT_INTERP_AND_ELF("int read_two(__builtin_va_list ap) {\n"
                          "    return __builtin_va_arg(ap, int) + __builtin_va_arg(ap, int);\n"
                          "}\n"
                          "int f(int a, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, a);\n"
                          "    int s = read_two(ap);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(1, 20, 21, 99);\n"
                          "}\n",
                          41);
}

TEST(varargs, const_first_named_param)
{
    /* Const interplay: a `const` first named parameter is fine in a variadic
       signature (top-level qualifiers are ignored for the function type). */
    EXPECT_INTERP_AND_ELF("int f(const int a, ...) {\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(42, 1, 2);\n"
                          "}\n",
                          42);
}