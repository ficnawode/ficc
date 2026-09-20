#include "harness.h"
#include "testdriver.h"

/* Declarator-grammar micro-phase: long long / signed specifiers,
   init-declarator lists, block-scope + combined tag definitions, anonymous
   struct/enum typedefs. Every program must produce the same result through
   the interpreter and the compiled ELF. */

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
    /* long, long long, signed forms all lower through the existing
       integer machinery; LP64 makes long and long long the same width. */
    EXPECT_INTERP_AND_ELF("typedef unsigned long ul;\n"
                          "typedef unsigned long long ull;\n"
                          "typedef long long ll;\n"
                          "typedef signed long s_l;\n"
                          "int main(void) {\n"
                          "    ll a = 1000000000LL;\n"
                          "    ll b = a * a;\n"
                          "    ull u = 42ULL;\n"
                          "    s_l s = -21;\n"
                          "    ul w = 7;\n"
                          "    if (b != 1000000000000000000LL) return 1;\n"
                          "    if (u != 42) return 2;\n"
                          "    if (s != -21) return 3;\n"
                          "    if (w != 7) return 4;\n"
                          "    if (sizeof(ll) != 8) return 5;\n"
                          "    return (*&a == 1000000000LL ? 42 : 6);\n"
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
    /* `int a[2], b[3];` — each declarator gets its own size. */
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

TEST(declarators, combined_struct_definition_declarator)
{
    /* `struct S { ... } v;` — the definition and the declarator in one
       declaration. */
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct Pair { int lo; int hi; } pr;\n"
                          "    pr.lo = 20;\n"
                          "    pr.hi = 22;\n"
                          "    if (pr.lo + pr.hi != 42) return 1;\n"
                          "    struct { int v; } av;\n"
                          "    av.v = pr.lo;\n"
                          "    return av.v + pr.hi;\n"
                          "}\n",
                          42);
}

TEST(declarators, block_scope_tag_definition)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    struct Local { int v; };\n"
                          "    struct Local l;\n"
                          "    l.v = 40;\n"
                          "    enum Mood { HAPPY, SAD };\n"
                          "    enum Mood m = SAD;\n"
                          "    if (m != 1) return 1;\n"
                          "    return l.v + m + 1;\n"
                          "}\n",
                          42);
}

TEST(declarators, anonymous_struct_typedef)
{
    /* The ficc source idiom: `typedef struct { ... } Name;`. */
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
    /* ficc source idiom: `typedef enum { ... } Name;`. */
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
    /* A tagged enum var + pointer to it (the pointer declarator derives
       from the same specifier). */
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
    /* Two distinct anonymous enum definitions are two distinct types, so
       the pointer-to-the-second cannot alias a var of the first. */
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

TEST(declarators, anonymous_struct_in_union_style)
{
    /* `struct { int lo; int hi; } u;` plus union spelling. */
    EXPECT_INTERP_AND_ELF("union U { int i; unsigned u; };\n"
                          "int main(void) {\n"
                          "    union U un;\n"
                          "    struct { int lo; int hi; } pair;\n"
                          "    un.i = 42;\n"
                          "    if (un.u != un.i) return 1;\n"
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
    /* `*` decorators on params/returns survive the specifier/declarator
       split. */
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

/* negatives (all must fail to build) */

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
    /* The shared specifier type still gets one scrutineer per declarator:
       a second var may not reuse the same name. */
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