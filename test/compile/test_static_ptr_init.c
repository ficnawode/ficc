#include "harness.h"
#include "testdriver.h"

TEST(static_ptr_init, cast_null_pointer_constant)
{
    EXPECT_INTERP_AND_ELF("static int *p0 = (void *) 0;\n"
                          "static char *sp = (char *) 0;\n"
                          "int main(void) {\n"
                          "    if (p0 != 0) return 1;\n"
                          "    if (sp != (void *) 0) return 2;\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, address_of_global)
{
    EXPECT_INTERP_AND_ELF("static int g;\n"
                          "static int *gp = &g;\n"
                          "int main(void) {\n"
                          "    *gp = 5;\n"
                          "    return g == 5 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, array_of_string_pointer_const)
{
    EXPECT_INTERP_AND_ELF("static const char *const names[2] = { \"x\", \"y\" };\n"
                          "int main(void) {\n"
                          "    return names[0][0] == 'x' && names[1][0] == 'y' ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, cast_string_literal_pointer_array)
{
    EXPECT_INTERP_AND_ELF("static const char *const msgs[3] = {\n"
                          "    (const char *) \"a\", (char *) \"bb\", \"ccc\"\n"
                          "};\n"
                          "int main(void) {\n"
                          "    return msgs[0][0] == 'a' && msgs[1][1] == 'b' && msgs[2][2] == 'c'\n"
                          "               ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, pointer_array_of_static_arrays)
{
    EXPECT_INTERP_AND_ELF("static char a[3] = \"ab\";\n"
                          "static char *names[2] = { a, a };\n"
                          "int main(void) {\n"
                          "    names[1][1] = 'c';\n"
                          "    return a[1] == 'c' ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, nonstatic_pointer_array_reloc)
{
    EXPECT_INTERP_AND_ELF("static char a[3] = {1, 2, 3};\n"
                          "char *names[2] = { a, a };\n"
                          "int main(void) {\n"
                          "    names[0][0] = 7;\n"
                          "    return a[0] == 7 ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, const_object_decay_in_pointer_array)
{
    EXPECT_INTERP_AND_ELF("static const char a[3] = \"ab\";\n"
                          "static const char *const names[2] = { a, a };\n"
                          "int main(void) {\n"
                          "    return names[1][1] == 'b' ? 0 : 1;\n"
                          "}\n",
                          0);
}

TEST(static_ptr_init, function_pointer_initializer)
{
    EXPECT_INTERP_AND_ELF("static int seven(void) { return 7; }\n"
                          "static int (*fp)(void) = seven;\n"
                          "int main(void) { return fp() == 7 ? 0 : 1; }\n",
                          0);
}

TEST(static_ptr_init, function_pointer_array_initializer)
{
    EXPECT_INTERP_AND_ELF("static int one(void) { return 1; }\n"
                          "static int two(void) { return 2; }\n"
                          "static int (*ops[2])(void) = { one, two };\n"
                          "int main(void) { return ops[0]() + ops[1]() == 3 ? 0 : 1; }\n",
                          0);
}

TEST(static_ptr_init, address_of_member)
{
    EXPECT_INTERP_AND_ELF("struct opts { int a; int b; };\n"
                          "static struct opts o = { 1, 2 };\n"
                          "static int *pb = &o.b;\n"
                          "static int *pa = &o.a;\n"
                          "int main(void) {\n"
                          "    *pb = 41;\n"
                          "    return *pa + *pb;\n"
                          "}\n",
                          42);
}

TEST(static_ptr_init, address_plus_constant)
{
    EXPECT_INTERP_AND_ELF("static int a[4] = { 10, 20, 22, 40 };\n"
                          "static int *p = &a[1] + 1;\n"
                          "int main(void) { return *p; }\n",
                          22);
}
