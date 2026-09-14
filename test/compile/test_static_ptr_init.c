#include "harness.h"
#include "testdriver.h"

/* File-scope pointer initializers added for the Lua build: a null pointer
   constant spelled `(void *)0`, an object/array designator decaying to its
   address, and pointer arrays whose elements are array objects. */

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