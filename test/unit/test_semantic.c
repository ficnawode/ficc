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

/* --- Phase 11: cast legality --- */

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

/* --- Phase 12a: typedef --- */

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
