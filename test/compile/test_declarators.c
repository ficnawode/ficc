#include "harness.h"
#include "testdriver.h"

TEST(declarators, llong_basic)
{
    EXPECT_INTERP_AND_ELF("long long f(long long v) {\n"
                          "    return v * 2;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    long long x = 21;\n"
                          "    return (int)f(x) + f(0) - 0;\n"
                          "}\n",
                          42);
}

TEST(declarators, llong_signed_combos)
{
    /* LP64: long and long long are both 64-bit. */
    EXPECT_INTERP_AND_ELF("typedef unsigned long ul;\n"
                          "typedef unsigned long long ull;\n"
                          "typedef long long ll;\n"
                          "typedef signed long s_l;\n"
                          "int main(void) {\n"
                          "    ll a = 1000000000LL;\n"
                          "    ull u = 42ULL;\n"
                          "    s_l s = -21;\n"
                          "    ul w = 7;\n"
                          "    if (u != 42) return 1;\n"
                          "    if (s != -21) return 2;\n"
                          "    if (w != 7) return 3;\n"
                          "    if (sizeof(ll) != 8) return 4;\n"
                          "    return (*&a == 1000000000LL ? 42 : 5);\n"
                          "}\n",
                          42);
}

TEST(declarators, llong_multiply)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long long a = 1000000000LL;\n"
                          "    long long b = a * a;\n"
                          "    if (b != 1000000000000000000LL) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, integer_specifier_int_suffix)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long long int a = 42;\n"
                          "    unsigned long long int b = 42ULL;\n"
                          "    if ((int)a != 42) return 1;\n"
                          "    if ((int)b != 42) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, llong_mod_shift)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    long long a = 1;\n"
                          "    long long b = 100;\n"
                          "    a = a << 20;\n"
                          "    if (a != 1048576) return 1;\n"
                          "    if (b % 7 != 2) return 2;\n"
                          "    if (b / 7 != 14) return 3;\n"
                          "    b >>= 4;\n"
                          "    return (int)b + 36;\n"
                          "}\n",
                          42);
}

TEST(declarators, signed_char_and_short)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    signed char sc = -3;\n"
                          "    signed short ss = -1000;\n"
                          "    signed int si = -40000;\n"
                          "    if (sc + 3 != 0) return 1;\n"
                          "    if (ss / 2 != -500) return 2;\n"
                          "    if (si + 40000 != 0) return 3;\n"
                          "    return (si + 40000) + 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, multi_block_scope)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a = 10, b = 20, c = 30;\n"
                          "    int *p = &a, q = 7;\n"
                          "    if (a + b + c != 60) return 1;\n"
                          "    if (*p != 10) return 2;\n"
                          "    if (q != 7) return 3;\n"
                          "    *p = 12;\n"
                          "    if (a != 12) return 4;\n"
                          "    return a + b + q + 3;\n"
                          "}\n",
                          42);
}

TEST(declarators, multi_pointer_declarators)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 20, y = 22;\n"
                          "    void *p, *q;\n"
                          "    p = &x; q = &y;\n"
                          "    return *(int *)p + *(int *)q;\n"
                          "}\n",
                          42);
}

TEST(declarators, multi_file_scope)
{
    EXPECT_INTERP_AND_ELF("static int g1 = 1, g2 = 2, g3 = 3;\n"
                          "static int *gp = &g2, gq = 30;\n"
                          "int main(void) {\n"
                          "    if (g1 + g2 + g3 != 6) return 1;\n"
                          "    if (*gp != 2) return 2;\n"
                          "    if (gq != 30) return 3;\n"
                          "    *gp = 6;\n"
                          "    return g1 * g2 + g3 + gq + 3;\n"
                          "}\n",
                          42);
}

TEST(declarators, multi_arrays_share_base)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[2], b[3];\n"
                          "    a[0] = 10; a[1] = 12;\n"
                          "    b[0] = 20;\n"
                          "    if (sizeof(a) != 8) return 1;\n"
                          "    if (sizeof(b) != 12) return 2;\n"
                          "    return a[0] + a[1] + b[0];\n"
                          "}\n",
                          42);
}

TEST(declarators, multi_dim_array_declarator)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void) {\n"
        "    int a[2][3];\n"
        "    a[0][0] = 1; a[0][1] = 2; a[0][2] = 3;\n"
        "    a[1][0] = 4; a[1][1] = 5; a[1][2] = 6;\n"
        "    return a[0][0] + a[0][1] + a[0][2] + a[1][0] + a[1][1] + a[1][2] + 21;\n"
        "}\n",
        42);
}

TEST(declarators, array_of_pointer_declarators)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 1, y = 2, z = 3;\n"
                          "    int *a[3];\n"
                          "    a[0] = &x; a[1] = &y; a[2] = &z;\n"
                          "    return *a[0] + *a[1] + *a[2] + 36;\n"
                          "}\n",
                          42);
}

TEST(declarators, multi_with_initializers)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = 1, y = x + 1, z = y + 1;\n"
                          "    if (x + y + z != 6) return 1;\n"
                          "    for (int i = 1, j = 2; i + j < 12; i = i + 1, j = j + 1) {\n"
                          "    }\n"
                          "    return (x + y + z) * 7;\n"
                          "}\n",
                          42);
}

TEST(declarators, for_init_declarator_list)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int acc = 0;\n"
                          "    for (int i = 0, j = 10; i < j; i = i + 1, j = j - 1) {\n"
                          "        acc = acc + 7;\n"
                          "    }\n"
                          "    if (acc != 35) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, struct_field_list)
{
    EXPECT_INTERP_AND_ELF("struct S { int a, b; int *p, c; };\n"
                          "int main(void) {\n"
                          "    int x = 1;\n"
                          "    struct S s;\n"
                          "    s.a = 20; s.c = 22; s.p = &x;\n"
                          "    s.b = s.a + s.c;\n"
                          "    *s.p = 20;\n"
                          "    if (s.b != 42) return 1;\n"
                          "    if (s.a + s.c != 42) return 2;\n"
                          "    if (sizeof(s) != 24) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, struct_definition_with_declarator)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct Pair { int lo; int hi; } pr;\n"
                          "    pr.lo = 20;\n"
                          "    pr.hi = 22;\n"
                          "    if (pr.lo + pr.hi != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, anon_struct_definition_with_declarator)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct { int v; } av;\n"
                          "    av.v = 42;\n"
                          "    return av.v;\n"
                          "}\n",
                          42);
}

TEST(declarators, tag_definition_pointer_declarator)
{
    EXPECT_INTERP_AND_ELF("struct P { int x; int y; } *pp;\n"
                          "int main(void) {\n"
                          "    struct P v;\n"
                          "    v.x = 20; v.y = 22;\n"
                          "    pp = &v;\n"
                          "    return pp->x + pp->y;\n"
                          "}\n",
                          42);
}

TEST(declarators, block_scope_struct_definition)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct Local { int v; };\n"
                          "    struct Local l;\n"
                          "    l.v = 42;\n"
                          "    return l.v;\n"
                          "}\n",
                          42);
}

TEST(declarators, block_scope_enum_definition)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    enum Mood { HAPPY, SAD };\n"
                          "    enum Mood m = SAD;\n"
                          "    if (m != 1) return 1;\n"
                          "    return m + 41;\n"
                          "}\n",
                          42);
}

TEST(declarators, anonymous_struct_typedef)
{
    EXPECT_INTERP_AND_ELF("typedef struct {\n"
                          "    int x;\n"
                          "    int y;\n"
                          "} Frame;\n"
                          "typedef struct { int v; } Cell;\n"
                          "int main(void) {\n"
                          "    Frame f;\n"
                          "    Cell c;\n"
                          "    f.x = 20; f.y = 22;\n"
                          "    c.v = f.x;\n"
                          "    if (f.x + f.y != 42) return 1;\n"
                          "    if (c.v + f.y != 42) return 2;\n"
                          "    if (sizeof(Frame) != 8) return 3;\n"
                          "    if (sizeof(Cell) != 4) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, anonymous_enum_typedef)
{
    EXPECT_INTERP_AND_ELF("typedef enum {\n"
                          "    TOK_INT,\n"
                          "    TOK_CHAR,\n"
                          "    TOK_EOF,\n"
                          "} Kind;\n"
                          "int main(void) {\n"
                          "    Kind k = TOK_CHAR;\n"
                          "    if (k != 1) return 1;\n"
                          "    if (sizeof(Kind) != 4) return 2;\n"
                          "    return TOK_EOF + 40;\n"
                          "}\n",
                          42);
}

TEST(declarators, enum_pointer_declarator)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    enum E { A, B } e = B;\n"
                          "    enum E *p = &e;\n"
                          "    if (*p != 1) return 1;\n"
                          "    *p = A;\n"
                          "    return (int)e + 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, negative_anon_enum_pointer_mismatch)
{
    /* Distinct anonymous enum definitions are distinct types. */
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    enum { A, B } e = B;\n"
                      "    enum { C } *p = &e;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, nested_inline_definitions)
{
    EXPECT_INTERP_AND_ELF("struct Outer {\n"
                          "    struct Inner { int x; } in;\n"
                          "    int arr[2], y;\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct Outer o;\n"
                          "    o.in.x = 40;\n"
                          "    o.arr[0] = 1;\n"
                          "    o.arr[1] = 1;\n"
                          "    o.y = 0;\n"
                          "    if (o.in.x + o.arr[0] + o.arr[1] + o.y != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, union_member_alias)
{
    EXPECT_INTERP_AND_ELF("union U { int i; unsigned u; };\n"
                          "int main(void) {\n"
                          "    union U un;\n"
                          "    un.i = 42;\n"
                          "    if (un.u != un.i) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(declarators, anon_struct_variable)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct { int lo; int hi; } pair;\n"
                          "    pair.lo = 20;\n"
                          "    pair.hi = 22;\n"
                          "    return pair.lo + pair.hi;\n"
                          "}\n",
                          42);
}

TEST(declarators, const_static_multi)
{
    EXPECT_INTERP_AND_ELF("static const int a = 20, b = 22;\n"
                          "int main(void) {\n"
                          "    const int x = 1, y = 2;\n"
                          "    if (a + b != 42) return 1;\n"
                          "    if (x + y != 3) return 2;\n"
                          "    return a + b;\n"
                          "}\n",
                          42);
}

TEST(declarators, pointer_param_and_return_types)
{
    EXPECT_INTERP_AND_ELF("int *add(int *p, int *q, int n) {\n"
                          "    p[0] = *q + n;\n"
                          "    return p;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int a[1] = { 0 };\n"
                          "    int b = 22;\n"
                          "    int *r = add(a, &b, 20);\n"
                          "    return *r;\n"
                          "}\n",
                          42);
}

TEST(declarators, array_param_qualifiers)
{
    /* §6.7.6.3p7: qualifiers between `[]` adjust the parameter's pointer type. */
    EXPECT_INTERP_AND_ELF("int sum(int n, const int a[restrict], int b[static 1]) {\n"
                          "    int s = 0;\n"
                          "    for (int i = 0; i < n; i++)\n"
                          "        s += a[i] + b[i];\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x[2] = { 20, 0 };\n"
                          "    int y[2] = { 22, 0 };\n"
                          "    return sum(1, x, y);\n"
                          "}\n",
                          42);
}

TEST(declarators, array_param_unspecified_bound)
{
    /* §6.7.6.3p4: `T a[*]` is a prototype-only adjusted pointer. */
    EXPECT_INTERP_AND_ELF("int first(int a[*]);\n"
                          "int first(int a[4]) { return a[0]; }\n"
                          "int main(void) {\n"
                          "    int x[2] = { 42, 0 };\n"
                          "    return first(x);\n"
                          "}\n",
                          42);
}

TEST(declarators, negative_char_with_long)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    long long char x = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_char_int)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    char int x = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_signed_unsigned)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    signed unsigned x = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_short_long)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    short long x = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_three_longs)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    long long long x = 0;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_trailing_comma_declarator)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int a = 1, ;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_declares_nothing)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_anon_struct_bare)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    struct { int a; };\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_forward_enum)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    enum E;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_datatype_redefinition_list)
{
    /* A second declarator may not redeclare the same name. */
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 1, x = 2;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, negative_incomplete_tag_list)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    struct S;\n"
                      "    struct S a, b;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(declarators, pointer_to_array_address_of)
{
    /* §6.5.3.2p3: `&array` is a pointer to the array, not its first element. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int a[4];\n"
                          "    int (*p)[4] = &a;\n"
                          "    (*p)[0] = 7;\n"
                          "    (*p)[3] = 35;\n"
                          "    return a[0] + a[3];\n"
                          "}\n",
                          42);
}

TEST(declarators, array_of_function_pointers)
{
    /* §6.7.6.1: `[N]` binds tighter than `*`, so this is an array of pointers. */
    EXPECT_INTERP_AND_ELF("int add1(int x) { return x + 1; }\n"
                          "int add2(int x) { return x + 2; }\n"
                          "int (*tab[2])(int);\n"
                          "int main(void) {\n"
                          "    tab[0] = add1;\n"
                          "    tab[1] = add2;\n"
                          "    return tab[1](40);\n"
                          "}\n",
                          42);
}

TEST(declarators, pointer_qualifiers_per_level)
{
    /* §6.7.6: a qualifier after `*` binds to that pointer level. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    const char *s = \"x\";\n"
                          "    const char * const p = s;\n"
                          "    const char * const *pp = &p;\n"
                          "    return (*pp)[0] == 'x' ? 42 : 0;\n"
                          "}\n",
                          42);
}

TEST(declarators, function_returning_function_pointer)
{
    EXPECT_INTERP_AND_ELF("int add1(int x) { return x + 1; }\n"
                          "int (*get(int tag))(int) { return tag == 1 ? add1 : 0; }\n"
                          "int main(void) { return get(1)(41); }\n",
                          42);
}

TEST(declarators, typedef_multiple_declarators)
{
    /* §6.7.7: one typedef may introduce several names sharing the specifier. */
    EXPECT_INTERP_AND_ELF("typedef int A, B[3];\n"
                          "int main(void) {\n"
                          "    A x = 42;\n"
                          "    B y = {1, 2, 3};\n"
                          "    return x + (int)sizeof(y) - 12;\n"
                          "}\n",
                          42);
}

TEST(declarators, abstract_function_pointer_cast)
{
    EXPECT_INTERP_AND_ELF("typedef void *(*Alloc)(long);\n"
                          "static void *impl(long n) { (void) n; return 0; }\n"
                          "int main(void) {\n"
                          "    Alloc a = (void *(*)(long)) impl;\n"
                          "    return a(1) == 0 ? 42 : 0;\n"
                          "}\n",
                          42);
}
