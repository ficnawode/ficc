#include "harness.h"
#include "testdriver.h"

/* Phase 12e: compound literals `(type){ ... }` (C11 §6.5.2.5, D12.9). Every
   program must produce the same result through the interpreter and through
   the compiled ELF (including the file-scope anonymous-global relocation
   path). Elevated checks return 42 on success and a distinct code per guard. */

TEST(compound_literal, block_scalar_value)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int x = (int){7} + (int){5};\n"
                          "    if (x != 12) return 1;\n"
                          "    if ((char){65} != 65) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, block_scalar_address)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int *p = &(int){23};\n"
                          "    if (p[0] != 23) return 1;\n"
                          "    if (*&(int){9} != 9) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, block_record_lvalue)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "int main(void) {\n"
                          "    struct S *sp = &(struct S){5, 7};\n"
                          "    if (sp->x != 5) return 1;\n"
                          "    if (sp->y != 7) return 2;\n"
                          "    if ((struct S){3, 4}.x != 3) return 3;\n"
                          "    if ((struct S){1, 2}.y != 2) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, call_with_address)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "int sum(struct S *p) { return p->x + p->y; }\n"
                          "int main(void) {\n"
                          "    if (sum(&(struct S){10, 20}) != 30) return 1;\n"
                          "    if (sum(&(struct S){.y = 4, .x = 1}) != 5) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, designated_end_nested)
{
    EXPECT_INTERP_AND_ELF("struct Inner { int a; int b; };\n"
                          "struct S { int x; struct Inner in; };\n"
                          "int main(void) {\n"
                          "    struct S s = (struct S){.x = 1, .in = {2, 3}};\n"
                          "    if (s.x + s.in.a + s.in.b != 6) return 1;\n"
                          "    if ((struct S){5, {6, 7}}.in.a != 6) return 2;\n"
                          "    if ((struct S){.in.b = 8, .x = 1}.in.b != 8) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, array_literal)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    int sum = 0;\n"
                          "    int i = 0;\n"
                          "    while (i < 3) {\n"
                          "        sum = sum + ((int[]){10, 20, 30})[i];\n"
                          "        i = i + 1;\n"
                          "    }\n"
                          "    if (sum != 60) return 1;\n"
                          "    if (((int[4]){1, 2, 3, 4})[3] != 4) return 2;\n"
                          "    if (sizeof((int[]){1, 2, 3}) != 12) return 3;\n"
                          "    if (sizeof((int[2]){9, 9}) != 8) return 4;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, const_target_allowed)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; };\n"
                          "int main(void) {\n"
                          "    if ((const struct S){.x = 1}.x != 1) return 1;\n"
                          "    const int *cp = &(const int){40};\n"
                          "    if (*cp != 40) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, union_literal)
{
    EXPECT_INTERP_AND_ELF("union U { char c; int i; };\n"
                          "int main(void) {\n"
                          "    union U u = (union U){7};\n"
                          "    if (u.c != 7) return 1;\n"
                          "    if ((union U){.i = 300}.i != 300) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, value_consumed_return_and_arg)
{
    EXPECT_INTERP_AND_ELF("struct S { int a; int b; };\n"
                          "struct S mk(int x) { struct S r = {x, x + 1}; return r; }\n"
                          "int sum(struct S s) { return s.a + s.b; }\n"
                          "int main(void) {\n"
                          "    struct S s = mk(41);\n"
                          "    if (s.a + s.b != 83) return 1;\n"
                          "    if (sum((struct S){3, 4}) != 7) return 2;\n"
                          "    if (mk((struct S){1, 2}.a).b != 2) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, address_of_auto_inside)
{
    EXPECT_INTERP_AND_ELF("struct S { int *p; int v; };\n"
                          "int main(void) {\n"
                          "    int g = 5;\n"
                          "    struct S *sp = &(struct S){&g, 1};\n"
                          "    if (*sp->p != 5) return 1;\n"
                          "    if (sp->v != 1) return 2;\n"
                          "    struct S s = (struct S){&g, 2};\n"
                          "    if (*s.p + s.v != 7) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, file_scope_scalar)
{
    EXPECT_INTERP_AND_ELF("int *p = &(int){77};\n"
                          "int main(void) {\n"
                          "    if (p[0] != 77) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, file_scope_record)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "struct S *sp = &(struct S){5, 7};\n"
                          "int main(void) {\n"
                          "    if (sp->x != 5) return 1;\n"
                          "    if (sp->y != 7) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, file_scope_const_target)
{
    EXPECT_INTERP_AND_ELF("const int *cp = &(const int){40};\n"
                          "int main(void) {\n"
                          "    if (*cp != 40) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, file_scope_nested_addresses)
{
    EXPECT_INTERP_AND_ELF("struct Inner { int v; };\n"
                          "struct Outer { struct Inner *in; int tag; };\n"
                          "struct Outer *po = &(struct Outer){&(struct Inner){42}, 9};\n"
                          "int main(void) {\n"
                          "    if (po->in->v != 42) return 1;\n"
                          "    if (po->tag != 9) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, block_static_references_literal)
{
    EXPECT_INTERP_AND_ELF("struct S { int *p; int v; };\n"
                          "int gfile = 5;\n"
                          "int main(void) {\n"
                          "    static struct S *st = &(struct S){&gfile, 7};\n"
                          "    if (*st->p != 5) return 1;\n"
                          "    if (st->v != 7) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, typedef_target)
{
    EXPECT_INTERP_AND_ELF("struct Point { int x; int y; };\n"
                          "typedef struct Point P;\n"
                          "typedef int I;\n"
                          "int main(void) {\n"
                          "    P *pp = &(P){3, 4};\n"
                          "    if (pp->x + pp->y != 7) return 1;\n"
                          "    if ((I){5} != 5) return 2;\n"
                          "    P p2 = (P){.y = 6, .x = 1};\n"
                          "    if (p2.x != 1 || p2.y != 6) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, incomplete_array_completes)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (((int[]){1, 2, 3})[2] != 3) return 1;\n"
                          "    if (sizeof((int[]){1, 2}) != 8) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, char_array_from_string)
{
    EXPECT_INTERP_AND_ELF("int main(void) {\n"
                          "    if (((char[5]){\"hi\"})[0] != 104) return 1;\n"
                          "    if (((char[5]){\"hi\"})[1] != 105) return 2;\n"
                          "    if (sizeof((char[]){\"ab\"}) != 3) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(compound_literal, negative_void_target)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    (void){5};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(compound_literal, negative_incomplete_record)
{
    EXPECT_BUILD_FAIL("struct S;\n"
                      "int main(void) {\n"
                      "    struct S *p = &(struct S){0};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(compound_literal, negative_inner_empty_brackets)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return (int[2][]){1, 2, 3, 4}[1][0];\n"
                      "}\n");
}

TEST(compound_literal, negative_file_scope_value)
{
    EXPECT_BUILD_FAIL("int g = (int){5};\n"
                      "int main(void) { return g; }\n");
}