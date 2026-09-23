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

/* Phase 11: cast legality */

TEST(semantic, cast_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int x = 300;\n"
                         "    char c = (char)x;\n"
                         "    unsigned long u = (unsigned long)x;\n"
                         "    void *v = (void *)u;\n"
                         "    int *p = (int *)v;\n"
                         "    return *p + c;\n"
                         "}\n");
}

TEST(semantic, void_cast_ok)
{
    EXPECT_BUILD_SUCCEED("struct s { int a; };\n"
                         "int main(void) {\n"
                         "    int x = 5;\n"
                         "    struct s v;\n"
                         "    (void)x;\n"
                         "    (void)v;\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(semantic, enum_cast_ok)
{
    EXPECT_BUILD_SUCCEED("enum E { A, B };\n"
                         "int main(void) {\n"
                         "    enum E e = (enum E)1;\n"
                         "    int i = (int)e;\n"
                         "    return i;\n"
                         "}\n");
}

TEST(semantic, cast_to_struct_rejected)
{
    EXPECT_BUILD_FAIL("struct s { int x; };\n"
                      "int main(void) {\n"
                      "    int i;\n"
                      "    return (struct s)i;\n"
                      "}\n");
}

TEST(semantic, cast_of_struct_rejected)
{
    EXPECT_BUILD_FAIL("struct s { int x; };\n"
                      "int main(void) {\n"
                      "    struct s v;\n"
                      "    (int)v;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, cast_target_array_rejected)
{
    /* `(int[3])` disambiguates as `(int)` + subscript-to-cast in the parser;
       the cast's type-name can never name an array. */
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int i;\n"
                      "    return (int[3])i;\n"
                      "}\n");
}

TEST(semantic, cast_not_lvalue)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int i;\n"
                      "    (int)i = 1;\n"
                      "    return i;\n"
                      "}\n");
}

TEST(semantic, void_value_in_binary)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return (void)5 + 1;\n"
                      "}\n");
}

TEST(semantic, void_value_in_init)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a = (void)5;\n"
                      "    return a;\n"
                      "}\n");
}

TEST(semantic, void_value_in_condition)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    if ((void)1) {\n"
                      "        return 1;\n"
                      "    }\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, void_value_in_call_arg)
{
    EXPECT_BUILD_FAIL("int f(int x) { return x; }\n"
                      "int main(void) {\n"
                      "    return f((void)5);\n"
                      "}\n");
}

TEST(semantic, void_ptr_param_ok)
{
    EXPECT_BUILD_SUCCEED("int f(void *p) {\n"
                         "    return p == 0;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(0);\n"
                         "}\n");
}

TEST(semantic, void_named_param_rejected)
{
    EXPECT_BUILD_FAIL("void f(void p) {\n"
                      "}\n");
}

TEST(semantic, void_ptr_param_wrong_deep_pointer_rejected)
{
    /* `int **` is not implicitly convertible to `void **` (only one pointee
       level converts; §6.5.16.1) — matches gcc. */
    EXPECT_BUILD_FAIL("void **dbl(void **pp) { return pp; }\n"
                      "int main(void) {\n"
                      "    int *p = 0;\n"
                      "    int **pp = &p;\n"
                      "    return dbl(pp) == 0;\n"
                      "}\n");
}

/* Phase 12a: typedef */

TEST(semantic, typedef_void_pointer)
{
    /* `typedef void V; V *p;` is the legal "opaque handle" idiom; the void
       check fires only at the point of a V *variable*. */
    EXPECT_BUILD_SUCCEED("typedef void V;\n"
                         "int main(void) {\n"
                         "    V *p = 0;\n"
                         "    return p == 0;\n"
                         "}\n");
}

TEST(semantic, typedef_void_variable_rejected)
{
    EXPECT_BUILD_FAIL("typedef void V;\n"
                      "V v;\n"
                      "int main(void) { return 0; }\n");
}

TEST(semantic, typedef_incomplete_record_ok)
{
    /* A typedef to a forward-declared record is the ficc coding style. */
    EXPECT_BUILD_SUCCEED("typedef struct Foo Foo;\n"
                         "struct Foo { int x; };\n"
                         "int main(void) {\n"
                         "    Foo f;\n"
                         "    f.x = 42;\n"
                         "    return f.x;\n"
                         "}\n");
}

TEST(semantic, typedef_const_ptr_write_rejected)
{
    EXPECT_BUILD_FAIL("typedef int *IP;\n"
                      "int main(void) {\n"
                      "    int x = 1;\n"
                      "    const IP p = &x;\n"
                      "    p = &x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, typedef_const_ptr_read_ok)
{
    EXPECT_BUILD_SUCCEED("typedef int *IP;\n"
                         "int main(void) {\n"
                         "    int x = 7;\n"
                         "    const IP p = &x;\n"
                         "    return *p;\n"
                         "}\n");
}

TEST(semantic, incdec_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int x = 1;\n"
                         "    x++;\n"
                         "    ++x;\n"
                         "    x--;\n"
                         "    --x;\n"
                         "    return x;\n"
                         "}\n");
}

TEST(semantic, incdec_const_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const int c = 1;\n"
                      "    c++;\n"
                      "    return 0;\n"
                      "}\n");
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const int c = 1;\n"
                      "    --c;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, incdec_const_pointee_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 1;\n"
                      "    const int *p = &x;\n"
                      "    (*p)++;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, incdec_pointer_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int a[3] = {0, 0, 0};\n"
                         "    int *p = &a[0];\n"
                         "    p++;\n"
                         "    p--;\n"
                         "    return p == &a[0];\n"
                         "}\n");
}

TEST(semantic, incdec_array_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a[3] = {0, 0, 0};\n"
                      "    a++;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, comma_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int a = 1;\n"
                         "    int b = 2;\n"
                         "    int v = (a, b);\n"
                         "    return v;\n"
                         "}\n");
}

TEST(semantic, comma_void_left_ok)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    int v = ((void)0, 42);\n"
                         "    return v;\n"
                         "}\n");
}

TEST(semantic, comma_not_lvalue_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a = 1;\n"
                      "    int b = 2;\n"
                      "    (a, b) = 5;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, alignof_folds_to_size_t)
{
    EXPECT_BUILD_SUCCEED("int g = _Alignof(long);\n"
                         "int main(void) {\n"
                         "    return g == 8 ? 0 : 1;\n"
                         "}\n");
}

TEST(semantic, alignof_expr_form)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    long x;\n"
                         "    if (_Alignof(x) != 8) {\n"
                         "        return 1;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(semantic, alignof_void_invalid)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return (int) _Alignof(void);\n"
                      "}\n");
}

TEST(semantic, alignof_incomplete_type_invalid)
{
    EXPECT_BUILD_FAIL("struct S;\n"
                      "int main(void) {\n"
                      "    return (int) _Alignof(struct S);\n"
                      "}\n");
}

TEST(semantic, static_assert_pass_file_scope)
{
    EXPECT_BUILD_SUCCEED("_Static_assert(1, \"ok\");\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(semantic, static_assert_pass_with_sizeof)
{
    EXPECT_BUILD_SUCCEED("_Static_assert(sizeof(long) == 8, \"long\");\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(semantic, static_assert_fail_reported)
{
    EXPECT_BUILD_FAIL("_Static_assert(0, \"boom\");\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, static_assert_fail_block_scope)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    _Static_assert(2 == 3, \"nope\");\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, static_assert_non_ice_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 1;\n"
                      "    _Static_assert(x, \"not frozen\");\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, bool_assign_normalizes)
{
    EXPECT_BUILD_SUCCEED("int main(void) {\n"
                         "    _Bool b = 9;\n"
                         "    b = 7;\n"
                         "    if (b != 1) return 1;\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(semantic, bool_cast_is_ice)
{
    EXPECT_BUILD_SUCCEED("_Static_assert((_Bool)5 == 1, \"bool cast\");\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(semantic, bool_name_not_keyword)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    bool b = 1;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, const_bool_write_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    const _Bool cb = 1;\n"
                      "    cb = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, alignas_accepted)
{
    EXPECT_BUILD_SUCCEED("_Alignas(32) int g;\n"
                         "int main(void) {\n"
                         "    _Alignas(16) int x = 1;\n"
                         "    return x;\n"
                         "}\n");
}

TEST(semantic, alignas_non_power_of_two_rejected)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    _Alignas(6) int x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(semantic, variadic_call_named_only)
{
    EXPECT_BUILD_SUCCEED("int f(int a, ...) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}\n");
}

TEST(semantic, variadic_call_extra_args)
{
    /* Extra trailing args are legal (C11 §6.5.2.2p6) and flow through the
       fixed-arg path in the IR builder, which guards callee param indexing. */
    EXPECT_BUILD_SUCCEED("int f(int a, ...) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1, 2, 3);\n"
                         "}\n");
}

TEST(semantic, variadic_call_too_few_args)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f();\n"
                      "}\n");
}

TEST(semantic, variadic_multi_named_call)
{
    EXPECT_BUILD_SUCCEED("int f(int a, int b, ...) {\n"
                         "    return a + b;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(20, 22, 1, 2, 3);\n"
                         "}\n");
}

TEST(semantic, variadic_only_named_args_matched)
{
    /* The named parameters must still be assignability-checked; the trailing
       arg is not (it has no declared type yet — default promotions land in
       the IR builder). */
    EXPECT_BUILD_SUCCEED("int f(int a, ...) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    char c = 5;\n"
                         "    return f(7, c);\n"
                         "}\n");
}

TEST(semantic, nonvariadic_arity_unchanged)
{
    EXPECT_BUILD_FAIL("int f(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1, 2);\n"
                      "}\n");
}

TEST(semantic, va_start_valid)
{
    EXPECT_BUILD_SUCCEED("int f(int a, ...) {\n"
                         "    __builtin_va_list ap;\n"
                         "    __builtin_va_start(ap, a);\n"
                         "    __builtin_va_end(ap);\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}\n");
}

TEST(semantic, va_start_bad_arity)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_start_second_arg_not_parameter)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    int x = 0;\n"
                      "    __builtin_va_start(ap, x);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_start_second_arg_not_identifier)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, 42);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_start_first_arg_not_va_list)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    int x = 0;\n"
                      "    __builtin_va_start(x, a);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_end_bad_arity)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_end(ap, ap);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, raw_names_are_free_identifiers)
{
    /* Phase 15 does not reserve the raw names: `va_start` is a plain
       identifier until the Phase 17 <stdarg.h> shim provides it. A user
       function called va_start is ordinary code, not a builtin. */
    EXPECT_BUILD_SUCCEED("int va_start(int a) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return va_start(42);\n"
                         "}\n");
}

TEST(semantic, raw_va_start_call_is_undeclared)
{
    /* Without the shim (Phase 17), a call to the raw name is just an
       undeclared function — the diagnostic a user relies on when they forget
       <stdarg.h>. */
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    va_start(ap, a);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_arg_valid)
{
    EXPECT_BUILD_SUCCEED("int f(int a, ...) {\n"
                         "    __builtin_va_list ap;\n"
                         "    __builtin_va_start(ap, a);\n"
                         "    int v = __builtin_va_arg(ap, int);\n"
                         "    __builtin_va_end(ap);\n"
                         "    return a + v;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1, 2);\n"
                         "}\n");
}

TEST(semantic, va_arg_void_target_rejected)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, a);\n"
                      "    __builtin_va_arg(ap, void);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_arg_record_target_rejected)
{
    EXPECT_BUILD_FAIL("struct S { int x; };\n"
                      "int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, a);\n"
                      "    __builtin_va_arg(ap, struct S);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_arg_array_target_rejected)
{
    EXPECT_BUILD_FAIL("typedef int IA[4];\n"
                      "int f(int a, ...) {\n"
                      "    __builtin_va_list ap;\n"
                      "    __builtin_va_start(ap, a);\n"
                      "    __builtin_va_arg(ap, IA);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

TEST(semantic, va_arg_first_arg_not_va_list)
{
    EXPECT_BUILD_FAIL("int f(int a, ...) {\n"
                      "    int x = 0;\n"
                      "    __builtin_va_arg(x, int);\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}\n");
}

/* Phase 16a: function prototypes / forward declarations */

TEST(semantic, prototype_then_definition)
{
    EXPECT_BUILD_SUCCEED("int add(int a, int b);\n"
                         "int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return add(1, 2);\n"
                         "}");
}

TEST(semantic, prototype_call_before_definition)
{
    EXPECT_BUILD_SUCCEED("int add(int a, int b);\n"
                         "int main(void) {\n"
                         "    return add(1, 2);\n"
                         "}\n"
                         "int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}");
}

TEST(semantic, repeated_prototypes)
{
    EXPECT_BUILD_SUCCEED("int add(int a, int b);\n"
                         "int add(int a, int b);\n"
                         "int add(int a, int b);\n"
                         "int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return add(1, 2);\n"
                         "}");
}

TEST(semantic, prototype_after_definition)
{
    EXPECT_BUILD_SUCCEED("int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}\n"
                         "int add(int a, int b);\n"
                         "int main(void) {\n"
                         "    return add(1, 2);\n"
                         "}");
}

TEST(semantic, prototype_variadic)
{
    EXPECT_BUILD_SUCCEED("int sum(int n, ...);\n"
                         "int main(void) {\n"
                         "    return sum(0);\n"
                         "}\n"
                         "int sum(int n, ...) {\n"
                         "    return n;\n"
                         "}");
}

TEST(semantic, prototype_const_param_compatible)
{
    /* §6.7.6.3p15: top-level parameter qualifiers are ignored for
       compatibility. */
    EXPECT_BUILD_SUCCEED("int f(const int a);\n"
                         "int f(int a) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(5);\n"
                         "}");
}

TEST(semantic, prototype_conflicting_param_type)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "int f(long a);\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}");
}

TEST(semantic, prototype_conflicting_arity)
{
    EXPECT_BUILD_FAIL("int f(int a, int b);\n"
                      "int f(int a);\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}");
}

TEST(semantic, prototype_static_then_plain_definition_inherits_linkage)
{
    /* §6.2.2p5: a definition with no storage-class specifier has the linkage of
       `extern` and so inherits the prior `static` declaration's internal
       linkage. */
    EXPECT_BUILD_SUCCEED("static int f(int a);\n"
                         "int f(int a) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}");
}

TEST(semantic, prototype_static_then_extern_allowed)
{
    EXPECT_BUILD_SUCCEED("static int f(int a);\n"
                         "extern int f(int a);\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}");
}

TEST(semantic, prototype_global_then_static_rejected)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "static int f(int a) {\n"
                      "    return a;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}");
}

TEST(semantic, prototype_static_then_static)
{
    EXPECT_BUILD_SUCCEED("static int f(int a);\n"
                         "static int f(int a) {\n"
                         "    return a;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return f(1);\n"
                         "}");
}

TEST(semantic, prototype_two_definitions_rejected)
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
                      "}");
}

TEST(semantic, prototype_variadic_vs_definition_arity)
{
    EXPECT_BUILD_FAIL("int f(int a);\n"
                      "int f(int a, ...);\n"
                      "int main(void) {\n"
                      "    return f(1);\n"
                      "}");
}

TEST(semantic, prototype_unnamed_params_then_definition)
{
    EXPECT_BUILD_SUCCEED("int add(int, int);\n"
                         "int add(int a, int b) {\n"
                         "    return a + b;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return add(1, 2);\n"
                         "}");
}

TEST(semantic, definition_unnamed_param_rejected)
{
    EXPECT_BUILD_FAIL("int f(int, int) {\n"
                      "    return 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}");
}

TEST(semantic, prototype_collides_with_global_var)
{
    EXPECT_BUILD_FAIL("int x(void);\n"
                      "int x = 5;\n"
                      "int main(void) {\n"
                      "    return x(1);\n"
                      "}");
}

/* Phase 16b: function designators / function-pointer semantics */

TEST(semantic, function_designator_in_expression)
{
    EXPECT_BUILD_SUCCEED("int f(int x) {\n"
                         "    return x;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    int (*fp)(int) = f;\n"
                         "    return fp == f ? 1 : 0;\n"
                         "}");
}

TEST(semantic, function_designator_undeclared_error)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int (*fp)(int) = not_a_function;\n"
                      "    return 0;\n"
                      "}");
}

TEST(semantic, sizeof_function_designator_rejected)
{
    EXPECT_BUILD_FAIL("int f(int x);\n"
                      "int main(void) {\n"
                      "    int x = sizeof(f);\n"
                      "    (void) x;\n"
                      "    return 0;\n"
                      "}");
}

TEST(semantic, incompatible_function_pointer_assign)
{
    EXPECT_BUILD_FAIL("int a(int);\n"
                      "int b(long);\n"
                      "int main(void) {\n"
                      "    int (*fp)(int) = (int (*)(long)) b;\n"
                      "    (void) fp;\n"
                      "    return 0;\n"
                      "}");
}

TEST(semantic, file_scope_fn_ptr_init_referencing_function)
{
    /* The function must resolve even though globals are planned before
       function bodies are checked (D16.1 pass order). */
    EXPECT_BUILD_SUCCEED("int f(int x);\n"
                         "int main(void) {\n"
                         "    return f(0) + 1;\n"
                         "}\n"
                         "int f(int x) {\n"
                         "    return x;\n"
                         "}");
}
