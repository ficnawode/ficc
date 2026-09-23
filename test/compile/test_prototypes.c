#include "harness.h"
#include "testdriver.h"

/* Phase 16a: function prototypes / forward declarations */

TEST(prototypes, prototype_then_definition)
{
    const char *src = "int add(int a, int b);\n"
                      "int add(int a, int b) {\n"
                      "    return a + b;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return add(20, 22);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, call_before_definition)
{
    const char *src = "int twice(int x);\n"
                      "int main(void) {\n"
                      "    return twice(21);\n"
                      "}\n"
                      "int twice(int x) {\n"
                      "    return x * 2;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, repeated_prototypes)
{
    const char *src = "int f(int a);\n"
                      "int f(int a);\n"
                      "int f(int a);\n"
                      "int f(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(42);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, const_param_interplay)
{
    /* §6.7.6.3p15: `const int` and `int` parameters are the same signature. */
    const char *src = "int f(const int a);\n"
                      "int f(int a) {\n"
                      "    return a + 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(41);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, variadic_prototype_then_definition)
{
    const char *src = "int sum(int n, ...);\n"
                      "int sum(int n, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, n);\n"
                      "    int t = n;\n"
                      "    t += __builtin_va_arg(ap, int);\n"
                      "    t += __builtin_va_arg(ap, int);\n"
                      "    __builtin_va_end(ap);\n"
                      "    return t;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return sum(40, 1, 1);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, elf_direct_call_via_elf_extern)
{
    /* A declaration-only extern resolved at link time against a second,
       host-compiled translation unit. The ficc TU emits an SHN_UNDEF symbol +
       R_X86_64_PLT32. */
    EXPECT_EQ(tc_run_elf_with_extra_tu("int shared_add(int a, int b);\n"
                                       "int main(void) {\n"
                                       "    return shared_add(20, 22);\n"
                                       "}\n",
                                       "int shared_add(int a, int b) {\n"
                                       "    return a + b;\n"
                                       "}\n"),
              42);
}

TEST(prototypes, elf_variadic_extern_two_tu)
{
    /* The caller zeroes %al for a variadic prototype even when the callee
       lives in another translation unit. */
    EXPECT_EQ(tc_run_elf_with_extra_tu("int sum_vals(int count, ...);\n"
                                       "int main(void) {\n"
                                       "    return sum_vals(4, 1, 2, 3, 4);\n"
                                       "}\n",
                                       "#include <stdarg.h>\n"
                                       "int sum_vals(int count, ...) {\n"
                                       "    va_list ap;\n"
                                       "    va_start(ap, count);\n"
                                       "    int sum = 0;\n"
                                       "    for (int i = 0; i < count; i++) {\n"
                                       "        sum += va_arg(ap, int);\n"
                                       "    }\n"
                                       "    va_end(ap);\n"
                                       "    return sum;\n"
                                       "}\n"),
              10);
}

TEST(prototypes, elf_same_tu_prototype_and_call)
{
    /* Prototype-first, definition-in-TU, exercised through the ELF backend
       too (interp covers it above; this row pins the external-call path stays
       unused when the function is defined here). */
    const char *src = "int f(int a);\n"
                      "int f(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(42);\n"
                      "}\n";
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, negative_conflicting_param_type)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "int f(long a);\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(prototypes, negative_conflicting_return)
{
    EXPECT_BUILD_FAIL("int f(void);\n"
                      "long f(void);\n"
                      "int main(void) {\n"
                      "    return (int) f();\n"
                      "}\n");
}

TEST(prototypes, positive_static_then_plain_definition_inherits_linkage)
{
    /* §6.2.2p5: the later definition has no storage-class specifier, so it has
       the linkage of `extern` and inherits the prior `static` declaration. */
    EXPECT_BUILD_SUCCEED("static int f(int a);\n"
                         "int f(int a) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}\n");
}

TEST(prototypes, negative_global_then_static)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "static int f(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(prototypes, negative_two_definitions)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "int f(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int f(int a) {\n"
                      "    return a + 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(prototypes, negative_variadic_mismatch)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "int f(int a, ...);\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(prototypes, unnamed_prototype_params)
{
    /* §6.7.6.3: declaration parameters may omit their names; the definition
       supplies them (§6.9.1p6). */
    const char *src = "int add(int, int);\n"
                      "int add(int a, int b) {\n"
                      "    return a + b;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return add(20, 22);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, unnamed_pointer_prototype_params)
{
    const char *src = "int mul(int *, int *);\n"
                      "int mul(int *p, int *q) {\n"
                      "    return *p * *q;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    int a = 21, b = 2;\n"
                      "    return mul(&a, &b);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(prototypes, unnamed_prototype_param_mismatch_rejected)
{
    /* The unnamed-param prototype still type-checks each call argument. */
    EXPECT_BUILD_FAIL("int f(int);\n"
                      "int main(void) {\n"
                      "    return f(1, 2);\n"
                      "}\n"
                      "int f(int x) {\n"
                      "    return x;\n"
                      "}\n");
}

TEST(prototypes, definition_unnamed_param_rejected)
{
    EXPECT_BUILD_FAIL("int f(int, int) {\n"
                      "    return 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(prototypes, negative_incomplete_ellipsis)
{
    EXPECT_BUILD_FAIL("int f(...);\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(prototypes, negative_prototype_collides_with_var)
{
    EXPECT_BUILD_FAIL("int x(void);\n"
                      "int x = 5;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}