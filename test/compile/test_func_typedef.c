#include "harness.h"
#include "testdriver.h"

TEST(func_typedef, parenthesized_name_definition)
{
    EXPECT_INTERP_AND_ELF("typedef int (*PFn)(int);\n"
                          "int add1(int y) { return y + 1; }\n"
                          "PFn (getf)(int tag) { return tag == 1 ? add1 : 0; }\n"
                          "int main(void) {\n"
                          "    PFn f = getf(1);\n"
                          "    return f(41);\n"
                          "}\n",
                          42);
}

TEST(func_typedef, prototype_then_definition)
{
    EXPECT_INTERP_AND_ELF("typedef int (*PFn)(int);\n"
                          "int add1(int y) { return y + 1; }\n"
                          "PFn (getf)(int tag);\n"
                          "int main(void) {\n"
                          "    PFn f = getf(1);\n"
                          "    return f(41);\n"
                          "}\n"
                          "PFn (getf)(int tag) { return add1; }\n",
                          42);
}

TEST(func_typedef, variadic_callee)
{
    EXPECT_INTERP_AND_ELF("typedef int (*CFn)(int count, ...);\n"
                          "int one(int a, ...) { return a; }\n"
                          "CFn (getc)(int x) { return x ? one : (CFn) 0; }\n"
                          "int main(void) {\n"
                          "    return getc(1)(3);\n"
                          "}\n",
                          3);
}

TEST(func_typedef, pointer_to_funcptr_typedef_definition)
{
    EXPECT_INTERP_AND_ELF("typedef int (*F1)(int);\n"
                          "typedef F1 (*F2)(int);\n"
                          "F2 (mk2)(void) { return (F2) 0; }\n"
                          "int main(void) {\n"
                          "    F2 g = mk2();\n"
                          "    return g == 0 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(func_typedef, plain_name_form_unchanged)
{
    EXPECT_INTERP_AND_ELF("typedef int (*PFn)(int);\n"
                          "int add1(int y) { return y + 1; }\n"
                          "PFn getf(int tag) { return add1; }\n"
                          "int main(void) {\n"
                          "    return getf(1)(41);\n"
                          "}\n",
                          42);
}

TEST(func_typedef, returns_pointer_to_function_without_typedef)
{
    EXPECT_INTERP_AND_ELF("int add1(int y) { return y + 1; }\n"
                          "int (*getf(int tag))(int) { return tag == 1 ? add1 : 0; }\n"
                          "int main(void) {\n"
                          "    return getf(1)(41);\n"
                          "}\n",
                          42);
}

TEST(func_typedef, pointer_typedef_return_type)
{
    EXPECT_INTERP_AND_ELF("typedef void *VP;\n"
                          "typedef VP (*AF)(VP, unsigned);\n"
                          "struct S { AF f; };\n"
                          "static VP ident(VP p, unsigned n) { (void) n; return p; }\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    int v = 7;\n"
                          "    s.f = ident;\n"
                          "    return s.f(&v, 1) == &v ? 0 : 1;\n"
                          "}\n",
                          0);
}
