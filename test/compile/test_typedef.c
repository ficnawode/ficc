#include "harness.h"
#include "testdriver.h"

/* Phase 12a: typedef. Every program must produce the same result through the
   interpreter and through the compiled ELF. Checks return 42 on success and a
   distinct small code per guard, so a divergence names its area. */

TEST(typedef, basic_file_scope)
{
    EXPECT_INTERP_AND_ELF("typedef int Int;\n"
                          "static Int g = 10;\n"
                          "int main(void) {\n"
                          "    Int x = 30;\n"
                          "    if (x + g != 40) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, pointer_alias)
{
    EXPECT_INTERP_AND_ELF("typedef int *Ip;\n"
                          "int main(void) {\n"
                          "    int a = 1;\n"
                          "    int b = 2;\n"
                          "    Ip p = &a;\n"
                          "    p = &b;\n"
                          "    *p = 40;\n"
                          "    if (b != 40) return 1;\n"
                          "    if (a != 1) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, param_and_return)
{
    EXPECT_INTERP_AND_ELF("typedef int Int;\n"
                          "static Int twice(Int v) {\n"
                          "    return v + v;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    Int n = twice(21);\n"
                          "    if (n != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, cast_through_typedef)
{
    EXPECT_INTERP_AND_ELF("typedef unsigned long ulong;\n"
                          "typedef char Byte;\n"
                          "int main(void) {\n"
                          "    ulong u = (ulong)(unsigned)-1;\n"
                          "    Byte b = (Byte)300;\n"
                          "    if (b != 44) return 1;\n"
                          "    if (u != 4294967295UL) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, sizeof_through_typedef)
{
    EXPECT_INTERP_AND_ELF("typedef int Int;\n"
                          "typedef int *Ip;\n"
                          "typedef char Byte;\n"
                          "int main(void) {\n"
                          "    if (sizeof(Int) != 4) return 1;\n"
                          "    if (sizeof(Ip) != 8) return 2;\n"
                          "    if (sizeof(Byte) != 1) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, block_scope_alias)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    typedef int A;\n"
                          "    A x = 42;\n"
                          "    {\n"
                          "        typedef A B;\n"
                          "        B y = x;\n"
                          "        x = y;\n"
                          "    }\n"
                          "    return x;\n"
                          "}\n",
                          42);
}

TEST(typedef, shadowed_by_local_var)
{
    EXPECT_INTERP_AND_ELF("typedef int T;\n"
                          "int main(void) {\n"
                          "    T n = 1;\n"
                          "    {\n"
                          "        int T = 100;\n"
                          "        n = T - n;\n"
                          "    }\n"
                          "    T *p = &n;\n"
                          "    *p = 1;\n"
                          "    return *p + 41;\n"
                          "}\n",
                          42);
}

TEST(typedef, forward_record_style)
{
    /* ficc's own coding style: typedef struct Tag Tag; then complete it;
       fields and expressions use the aliased name. */
    EXPECT_INTERP_AND_ELF("typedef struct Cell Cell;\n"
                          "struct Cell { int v; Cell *next; };\n"
                          "static int sumc(Cell *c) {\n"
                          "    int t = 0;\n"
                          "    while (c) {\n"
                          "        t = t + c->v;\n"
                          "        c = c->next;\n"
                          "    }\n"
                          "    return t;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    Cell a;\n"
                          "    Cell b;\n"
                          "    a.v = 10; a.next = &b;\n"
                          "    b.v = 32; b.next = 0;\n"
                          "    return sumc(&a);\n"
                          "}\n",
                          42);
}

TEST(typedef, enum_and_typedef_coexist)
{
    EXPECT_INTERP_AND_ELF("enum Color { RED, GREEN, BLUE };\n"
                          "typedef enum Color Color;\n"
                          "int main(void) {\n"
                          "    Color c = GREEN;\n"
                          "    if (sizeof(Color) != sizeof(int)) return 1;\n"
                          "    return 42 + c - 1;\n"
                          "}\n",
                          42);
}

TEST(typedef, typedef_after_tag)
{
    /* `typedef enum Color Color;` may also follow the tag definition. */
    EXPECT_INTERP_AND_ELF("typedef enum Color Color;\n"
                          "enum Color { RED, GREEN, BLUE };\n"
                          "Color f(Color c) {\n"
                          "    return c;\n"
                          "}\n"
                          "int main(void) { return f(BLUE) + 40; }\n",
                          42);
}

TEST(typedef, const_pointer_alias)
{
    /* `const IP` is `int * const`: the pointer itself is readonly, the
       pointee is writable through it. */
    EXPECT_INTERP_AND_ELF("typedef int *IP;\n"
                          "int main(void) {\n"
                          "    int x = 5;\n"
                          "    int y = 6;\n"
                          "    const IP p = &x;\n"
                          "    *p = 41;\n"
                          "    if (*p != 41) return 1;\n"
                          "    (void)y;\n"
                          "    return *p + 1;\n"
                          "}\n",
                          42);
}

TEST(typedef, char_ptr_and_array_of_alias)
{
    EXPECT_INTERP_AND_ELF("typedef char *Str;\n"
                          "typedef int Int;\n"
                          "int main(void) {\n"
                          "    Str s = \"hello\";\n"
                          "    if (s[0] - 104 != 0) return 1;\n"
                          "    Int a[2];\n"
                          "    a[0] = 40;\n"
                          "    a[1] = 2;\n"
                          "    if (a[0] + a[1] != 42) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, same_type_redecl_allowed)
{
    /* C11 §6.7: a typedef may be redeclared in the same scope as long as it
       refers to the same type. (gcc accepts this silently.) */
    EXPECT_INTERP_AND_ELF("typedef int T;\n"
                          "typedef int T;\n"
                          "int main(void) { T x = 42; return x; }\n",
                          42);
}

TEST(typedef, ternary_and_arith_through_alias)
{
    EXPECT_INTERP_AND_ELF("typedef long L;\n"
                          "int main(void) {\n"
                          "    L a = 20;\n"
                          "    L b = 22;\n"
                          "    L r = a < b ? a + 1 : b;\n"
                          "    return r + r;\n"
                          "}\n",
                          42);
}

TEST(typedef, for_init_declaration)
{
    /* The for-init clause recognizes a typedef name as a declaration. */
    EXPECT_INTERP_AND_ELF("typedef int Int;\n"
                          "int main(void) {\n"
                          "    int sum = 0;\n"
                          "    for (Int i = 0; i < 14; i = i + 1) {\n"
                          "        sum = sum + 3;\n"
                          "    }\n"
                          "    return sum;\n"
                          "}\n",
                          42);
}

/* negatives (all must fail to build) */

TEST(typedef, negative_same_scope_var_clash)
{
    EXPECT_BUILD_FAIL("typedef int T;\n"
                      "int T;\n"
                      "int main(void) { return 0; }\n");
}

TEST(typedef, negative_var_then_typedef)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int T;\n"
                      "    typedef int T;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(typedef, negative_fun_over_typedef)
{
    EXPECT_BUILD_FAIL("typedef int f;\n"
                      "int f(void) { return 0; }\n");
}

TEST(typedef, negative_typedef_over_var)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x;\n"
                      "    typedef int x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(typedef, negative_different_type_redef)
{
    EXPECT_BUILD_FAIL("typedef int T;\n"
                      "typedef long T;\n"
                      "int main(void) { return 0; }\n");
}

TEST(typedef, negative_shadowed_typedef_used_as_type)
{
    EXPECT_BUILD_FAIL("typedef int T;\n"
                      "int main(void) {\n"
                      "    int T;\n"
                      "    T x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(typedef, negative_const_pointer_write)
{
    EXPECT_BUILD_FAIL("typedef int *IP;\n"
                      "int main(void) {\n"
                      "    int x = 1;\n"
                      "    const IP p = &x;\n"
                      "    p = &x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(typedef, negative_void_alias_variable)
{
    EXPECT_BUILD_FAIL("typedef void V;\n"
                      "V v;\n"
                      "int main(void) { return 0; }\n");
}

TEST(typedef, const_qualified_cast_target)
{
    /* C11 §6.5.4: a cast may open with `const` applied to a typedef name —
       `(const T)x` and `(const T *)&x` take the typedef-aware type-name
       grammar (D12.3). */
    EXPECT_INTERP_AND_ELF("typedef int T;\n"
                          "int main(void) {\n"
                          "    int x = 42;\n"
                          "    if ((const T)x != 42) return 1;\n"
                          "    const T *p = (const T *)&x;\n"
                          "    if (*p != 42) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(typedef, negative_cast_discards_const_then_write)
{
    /* Casting away const is legal, but storing *through* the const pointer
       writes to a const object — rejected by the §9 write gate. */
    EXPECT_BUILD_FAIL("typedef int T;\n"
                      "int main(void) {\n"
                      "    int x = 5;\n"
                      "    const T *p = &x;\n"
                      "    *p = 6;\n"
                      "    return 0;\n"
                      "}\n");
}