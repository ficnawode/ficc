#include "harness.h"
#include "testdriver.h"

/* Phase 16b: function-pointer types + parenthesized declarators */

TEST(function_ptr, assign_compare_designator)
{
    const char *src = "int f(int x) {\n"
                      "    return x + 1;\n"
                      "}\n"
                      "int g(int x) {\n"
                      "    return x - 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    int (*fp)(int);\n"
                      "    fp = f;\n"
                      "    if (fp != f)\n"
                      "        return 1;\n"
                      "    if (fp == g)\n"
                      "        return 2;\n"
                      "    fp = g;\n"
                      "    if (fp != g)\n"
                      "        return 3;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, address_of_and_deref)
{
    const char *src = "int inc(int x) { return x + 1; }\n"
                      "int main(void) {\n"
                      "    int (*fp)(int) = &inc;\n"
                      "    if (fp != &inc)\n"
                      "        return 1;\n"
                      "    if (*fp != inc)\n"
                      "        return 2;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, passed_and_returned)
{
    const char *src = "int inc(int x) { return x + 1; }\n"
                      "int dec(int x) { return x - 1; }\n"
                      "typedef int (*Op)(int);\n"
                      "int is_inc(Op op) {\n"
                      "    return op == inc;\n"
                      "}\n"
                      "Op pick(int which) {\n"
                      "    if (which == 0)\n"
                      "        return inc;\n"
                      "    return dec;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    Op op = pick(0);\n"
                      "    if (!is_inc(op))\n"
                      "        return 1;\n"
                      "    if (pick(1) != dec)\n"
                      "        return 2;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, function_ptr_param)
{
    const char *src = "int apply(int (*fn)(int), int x) {\n"
                      "    (void) fn;\n"
                      "    return x;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return apply(0, 42);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, struct_fields)
{
    const char *src = "typedef unsigned long u64;\n"
                      "typedef _Bool bool;\n"
                      "typedef struct HM HM;\n"
                      "struct HM {\n"
                      "    u64 (*hash)(const void *key);\n"
                      "    bool (*eq)(const void *a, const void *b);\n"
                      "    int calls;\n"
                      "};\n"
                      "int g_calls;\n"
                      "u64 hasha(const void *key) {\n"
                      "    return (u64) *(const int *) key;\n"
                      "}\n"
                      "bool eqa(const void *a, const void *b) {\n"
                      "    g_calls++;\n"
                      "    return *(const int *) a == *(const int *) b;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    HM hm = {hasha, eqa, 0};\n"
                      "    HM *p = &hm;\n"
                      "    if (p->hash != hasha)\n"
                      "        return 1;\n"
                      "    if (p->eq != &eqa)\n"
                      "        return 2;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, function_shape_smoke)
{
    /* Temporary: once file-scope fn-ptr initializers land (Phase 16c), this
       becomes the ficc `lower_fns` self-compile pattern. */
    EXPECT_BUILD_SUCCEED("int f(int x);\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(function_ptr, negative_sizeof_function)
{
    EXPECT_BUILD_FAIL("int f(int x);\n"
                      "int main(void) {\n"
                      "    int x = sizeof(f);\n"
                      "    return x;\n"
                      "}\n");
}

TEST(function_ptr, negative_sizeof_function_type)
{
    EXPECT_BUILD_FAIL("typedef int F(int);\n"
                      "int main(void) {\n"
                      "    int x = sizeof(F);\n"
                      "    return x;\n"
                      "}\n");
}

TEST(function_ptr, negative_incompatible_assign)
{
    EXPECT_BUILD_FAIL("int a(int);\n"
                      "int b(long);\n"
                      "int main(void) {\n"
                      "    int (*fp)(int) = (int (*)(long)) b;\n"
                      "    (void) fp;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(function_ptr, negative_assign_constability)
{
    /* A const-qualified function pointer is not a modifiable lvalue. */
    EXPECT_BUILD_FAIL("typedef int (*FP)(int);\n"
                      "int f(int);\n"
                      "int main(void) {\n"
                      "    const FP fp = f;\n"
                      "    fp = f;\n"
                      "    return 0;\n"
                      "}\n");
}
/* Phase 16c: indirect call lowering */

TEST(function_ptr, indirect_call_through_variable)
{
    const char *src = "int add(int a, int b) { return a + b; }\n"
                      "int sub(int a, int b) { return a - b; }\n"
                      "int main(void) {\n"
                      "    int (*fp)(int, int);\n"
                      "    fp = add;\n"
                      "    if (fp(20, 22) != 42)\n"
                      "        return 1;\n"
                      "    fp = sub;\n"
                      "    if (fp(50, 8) != 42)\n"
                      "        return 2;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, indirect_call_through_deref_and_parens)
{
    const char *src = "typedef int (*Op)(int, int);\n"
                      "int add(int a, int b) { return a + b; }\n"
                      "int apply(Op op, int a, int b) {\n"
                      "    return (*op)(a, b);\n"
                      "}\n"
                      "int main(void) {\n"
                      "    Op op = add;\n"
                      "    if (apply(op, 20, 22) != 42)\n"
                      "        return 1;\n"
                      "    if ((*op)(1, 41) != 42)\n"
                      "        return 2;\n"
                      "    if ((add)(20, 22) != 42)\n"
                      "        return 3;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, indirect_call_through_struct_member)
{
    const char *src = "typedef int (*Op)(int);\n"
                      "typedef struct D D;\n"
                      "struct D {\n"
                      "    Op op;\n"
                      "    int tag;\n"
                      "};\n"
                      "int inc(int x) { return x + 1; }\n"
                      "int main(void) {\n"
                      "    D d = {inc, 7};\n"
                      "    D *p = &d;\n"
                      "    if (p->op(41) != 42)\n"
                      "        return 1;\n"
                      "    if (p->tag != 7)\n"
                      "        return 2;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, indirect_call_through_param)
{
    const char *src = "int inc(int x) { return x + 1; }\n"
                      "int twice(int (*fn)(int), int x) {\n"
                      "    return fn(x) + fn(x);\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return twice(inc, 20);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, variadic_indirect_call)
{
    const char *src = "typedef int (*VarFn)(int count, ...);\n"
                      "int sum(int count, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, count);\n"
                      "    int t = 0;\n"
                      "    for (int i = 0; i < count; i++)\n"
                      "        t += __builtin_va_arg(ap, int);\n"
                      "    __builtin_va_end(ap);\n"
                      "    return t;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    VarFn fp = sum;\n"
                      "    if (fp(4, 10, 20, 12, 0) != 42)\n"
                      "        return 1;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, elf_indirect_callback_two_tu)
{
    /* ficc passes a self-defined comparator to a host-compiled caller, which
       invokes it through the pointer. */
    EXPECT_EQ(tc_run_elf_with_extra_tu("typedef int (*CmpFn)(const void *a, const void *b);\n"
                                       "int call_cmp(int a, int b, CmpFn cmp);\n"
                                       "int compare(const void *a, const void *b) {\n"
                                       "    return *(const int *) a - *(const int *) b;\n"
                                       "}\n"
                                       "int main(void) {\n"
                                       "    if (call_cmp(30, 10, compare) != 20)\n"
                                       "        return 1;\n"
                                       "    return 42;\n"
                                       "}\n",
                                       "typedef int (*CmpFn)(const void *a, const void *b);\n"
                                       "int call_cmp(int a, int b, CmpFn cmp) {\n"
                                       "    int x = a, y = b;\n"
                                       "    return cmp(&x, &y);\n"
                                       "}\n"),
              42);
}

TEST(function_ptr, negative_call_non_function_pointer)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int *p = 0;\n"
                      "    return p(1);\n"
                      "}\n");
}

TEST(function_ptr, negative_call_through_int)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 5;\n"
                      "    return (*x)(1);\n"
                      "}\n");
}

TEST(function_ptr, negative_indirect_wrong_arity)
{
    EXPECT_BUILD_FAIL("typedef int (*F)(int);\n"
                      "int main(void) {\n"
                      "    F f = 0;\n"
                      "    return f(1, 2);\n"
                      "}\n");
}

TEST(function_ptr, negative_indirect_missing_arity)
{
    EXPECT_BUILD_FAIL("typedef int (*F)(int);\n"
                      "int main(void) {\n"
                      "    F f = 0;\n"
                      "    return f();\n"
                      "}\n");
}

TEST(function_ptr, file_scope_table_indirect)
{
    /* The ficc `lower_fns` dispatch pattern: a file-scope array of function
       pointers, selected and called indirectly. */
    const char *src = "typedef int (*Op)(int);\n"
                      "int add10(int x) { return x + 10; }\n"
                      "int mul2(int x) { return x * 2; }\n"
                      "int sub7(int x) { return x - 7; }\n"
                      "static const Op dispatch[3] = {add10, mul2, sub7};\n"
                      "int run_slot(int which, int x) {\n"
                      "    Op op = dispatch[which];\n"
                      "    return op(x);\n"
                      "}\n"
                      "int main(void) {\n"
                      "    if (run_slot(0, 32) != 42)\n"
                      "        return 1;\n"
                      "    if (run_slot(1, 21) != 42)\n"
                      "        return 2;\n"
                      "    if (run_slot(2, 49) != 42)\n"
                      "        return 3;\n"
                      "    if (dispatch[1] != mul2)\n"
                      "        return 4;\n"
                      "    return 42;\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, file_scope_single_fp_init)
{
    const char *src = "int f(int x) { return x; }\n"
                      "int (*gfp)(int) = f;\n"
                      "int main(void) {\n"
                      "    return gfp(42);\n"
                      "}\n";
    EXPECT_EQ(tc_run_interp(src), 42);
    EXPECT_EQ(tc_run_elf(src), 42);
}

TEST(function_ptr, enum_param_matches_unsigned)
{
    /* An enum is compatible with its underlying integer type, so a callback
       declared with `enum E` and defined with `unsigned` still matches. */
    EXPECT_INTERP_AND_ELF("enum flags { F_ONE = 1, F_TWO = 2 };\n"
                          "struct ops { int (*fn)(enum flags); };\n"
                          "static int impl(unsigned f) { return (int) f + 40; }\n"
                          "static struct ops o;\n"
                          "int main(void) {\n"
                          "    o.fn = impl;\n"
                          "    return o.fn(F_TWO);\n"
                          "}\n",
                          42);
}
