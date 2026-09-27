#include "harness.h"
#include "testdriver.h"

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
    /* psABI §3.2.3: trailing ints beyond the GP area ride on the caller's stack. */
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
    /* §6.5.2.2p7: default argument promotions rank char/_Bool to int at the
       call. */
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
    /* psABI §3.5.7: va_start sets gp_offset to the first unnamed GP slot. */
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
    /* psABI §3.5.7: the reg_save_area captures the six GP registers. */
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
    /* psABI §3.5.7: gp_offset == 48 means unnamed args come from
       overflow_arg_area. */
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

TEST(varargs, va_copy_copies_state)
{
    /* §7.16.1.2: va_copy makes an independent copy of the argument state. */
    EXPECT_INTERP_AND_ELF("int f(int a, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_list cp;\n"
                          "    __builtin_va_start(ap, a);\n"
                          "    __builtin_va_copy(cp, ap);\n"
                          "    int x = __builtin_va_arg(ap, int);\n"
                          "    int y = __builtin_va_arg(cp, int);\n"
                          "    return x + y;\n"
                          "}\n"
                          "int main(void) { return f(0, 21); }\n",
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
    /* psABI §3.5.7: the walk crosses gp_offset == 48 into overflow_arg_area. */
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
    /* §6.5.2.2p7 promotes char to int; §7.16.1.1 va_arg truncates back. */
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
    /* psABI §3.5.7: va_list is an array of one tag, so a by-value handoff
       shares the cursor. */
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
    /* §6.7.6.3p15: top-level `const` on a parameter is ignored in the function
       type. */
    EXPECT_INTERP_AND_ELF("int f(const int a, ...) {\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(42, 1, 2);\n"
                          "}\n",
                          42);
}

TEST(varargs, double_varargs_read)
{
    EXPECT_INTERP_AND_ELF("int f(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    double d = __builtin_va_arg(ap, double);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return (int) d;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(1, 42.0);\n"
                          "}\n",
                          42);
}

TEST(varargs, mixed_int_and_double_varargs)
{
    EXPECT_INTERP_AND_ELF("int f(int n, ...) {\n"
                          "    __builtin_va_list ap;\n"
                          "    __builtin_va_start(ap, n);\n"
                          "    int a = __builtin_va_arg(ap, int);\n"
                          "    double b = __builtin_va_arg(ap, double);\n"
                          "    __builtin_va_end(ap);\n"
                          "    return a + (int) b;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f(2, 20, 22.0);\n"
                          "}\n",
                          42);
}
