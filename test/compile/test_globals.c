#include "harness.h"
#include "testdriver.h"

static const char *data_bss_src = "int counter = 5;\n"
                                  "static int acc;\n"
                                  "int main(void) {\n"
                                  "    counter = counter + 2;\n"
                                  "    acc = acc + counter + 1;\n"
                                  "    return acc;\n"
                                  "}\n";

TEST(globals, interp_data_bss)
{
    EXPECT_EQ(tc_run_interp(data_bss_src), 8);
}

TEST(globals, interp_accumulate_across_calls)
{
    EXPECT_EQ(tc_run_interp("static int total;\n"
                            "int add(int v) {\n"
                            "    total = total + v;\n"
                            "    return total;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    add(3);\n"
                            "    add(4);\n"
                            "    return add(5);\n"
                            "}\n"),
              12);
}

TEST(globals, interp_record_array_globals)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "struct Pt origin;\n"
                            "int arr[3];\n"
                            "int main(void) {\n"
                            "    origin.x = 4;\n"
                            "    origin.y = 5;\n"
                            "    arr[1] = 7;\n"
                            "    return origin.x + origin.y + arr[1];\n"
                            "}\n"),
              16);
}

TEST(globals, interp_global_enum)
{
    EXPECT_EQ(tc_run_interp("enum Color { RED, GREEN, BLUE };\n"
                            "enum Color state;\n"
                            "int main(void) {\n"
                            "    state = BLUE;\n"
                            "    return state + RED;\n"
                            "}\n"),
              2);
}

TEST(globals, interp_addr_of_scalar_global)
{
    EXPECT_EQ(tc_run_interp("int g = 9;\n"
                            "int *p;\n"
                            "int main(void) {\n"
                            "    p = &g;\n"
                            "    return *p;\n"
                            "}\n"),
              9);
}

TEST(globals, interp_pointer_global_string)
{
    EXPECT_EQ(tc_run_interp("char *msg = \"hello\";\n"
                            "int main(void) {\n"
                            "    return msg[1] + msg[4] + msg[0];\n"
                            "}\n"),
              'e' + 'o' + 'h');
}

TEST(globals, interp_extern_is_zero)
{
    EXPECT_EQ(tc_run_interp("extern int e;\n"
                            "int main(void) {\n"
                            "    return e;\n"
                            "}\n"),
              0);
}

TEST(globals, interp_tentative_defs_merge)
{
    EXPECT_EQ(tc_run_interp("int x;\n"
                            "int x;\n"
                            "int main(void) {\n"
                            "    x = 9;\n"
                            "    return x;\n"
                            "}\n"),
              9);
}

TEST(globals, extern_then_definition)
{
    EXPECT_INTERP_AND_ELF("extern int e;\n"
                          "int e;\n"
                          "int main(void) {\n"
                          "    e = 7;\n"
                          "    return e;\n"
                          "}\n",
                          7);
}

TEST(globals, global_double)
{
    EXPECT_INTERP_AND_ELF("double d = 3.5;\n"
                          "int main(void) {\n"
                          "    return (int) (d * 2.0);\n"
                          "}\n",
                          7);
}

TEST(globals, interp_zero_init)
{
    EXPECT_EQ(tc_run_interp("int z;\n"
                            "long l;\n"
                            "char c;\n"
                            "int main(void) {\n"
                            "    l = 1;\n"
                            "    if (z != 0) {\n"
                            "        return 1;\n"
                            "    }\n"
                            "    if (c != 0) {\n"
                            "        return 2;\n"
                            "    }\n"
                            "    if (l != 1) {\n"
                            "        return 3;\n"
                            "    }\n"
                            "    return 0;\n"
                            "}\n"),
              0);
}

TEST(globals, elf_data_bss)
{
    EXPECT_EQ(tc_run_elf(data_bss_src), 8);
}

TEST(globals, elf_pointer_global_string)
{
    /* exit code truncates to the low byte: 'e'+'o'+'h' = 316 & 0xFF = 60 */
    EXPECT_EQ(tc_run_elf("char *msg = \"hello\";\n"
                         "int main(void) {\n"
                         "    return msg[1] + msg[4] + msg[0];\n"
                         "}\n"),
              60);
}

TEST(globals, elf_extern_two_tu)
{
    EXPECT_EQ(tc_run_elf_with_extra_tu("extern int shared;\n"
                                       "extern int other_ext;\n"
                                       "int main(void) {\n"
                                       "    shared = shared + 2;\n"
                                       "    return shared + other_ext;\n"
                                       "}\n",
                                       "int shared = 40;\n"
                                       "int other_ext = 5;\n"),
              47);
}

TEST(globals, negative_global_nonconst_init)
{
    EXPECT_BUILD_FAIL("int v = 1 + 2 + main;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_global_redefine)
{
    EXPECT_BUILD_FAIL("int x = 1;\n"
                      "int x = 2;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

static const char *block_static_src = "int counter(void) {\n"
                                      "    static int n = 5;\n"
                                      "    n = n + 1;\n"
                                      "    return n;\n"
                                      "}\n"
                                      "int main(void) {\n"
                                      "    return counter() + counter();\n"
                                      "}\n";

TEST(globals, interp_block_static)
{
    EXPECT_EQ(tc_run_interp(block_static_src), 13);
}

TEST(globals, elf_block_static)
{
    EXPECT_EQ(tc_run_elf(block_static_src), 13);
}

TEST(globals, interp_block_static_distinct)
{
    EXPECT_EQ(tc_run_interp("int f(void) {\n"
                            "    static int n;\n"
                            "    n = n + 2;\n"
                            "    return n;\n"
                            "}\n"
                            "int g(void) {\n"
                            "    static int n;\n"
                            "    n = n + 3;\n"
                            "    return n;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f() + f() + g();\n"
                            "}\n"),
              9);
}

TEST(globals, local_shadows_global)
{
    EXPECT_EQ(tc_run_interp("int counter = 100;\n"
                            "int main(void) {\n"
                            "    int counter = 7;\n"
                            "    counter = counter + 1;\n"
                            "    return counter;\n"
                            "}\n"),
              8);
}

static const char *static_function_src = "static int helper(int x) {\n"
                                         "    return x * 2;\n"
                                         "}\n"
                                         "int visible(int x) {\n"
                                         "    return helper(x) + 1;\n"
                                         "}\n"
                                         "int main(void) {\n"
                                         "    return visible(5);\n"
                                         "}\n";

TEST(globals, interp_static_function)
{
    EXPECT_EQ(tc_run_interp(static_function_src), 11);
}

TEST(globals, elf_static_function_binding)
{
    EXPECT_EQ(tc_run_elf(static_function_src), 11);
}

TEST(globals, extern_with_init_is_definition)
{
    EXPECT_INTERP_AND_ELF("extern int x = 5;\n"
                          "int main(void) {\n"
                          "    return x;\n"
                          "}\n",
                          5);
}

TEST(globals, negative_var_linkage_mixing)
{
    EXPECT_BUILD_FAIL("int x;\n"
                      "static int x;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_func_linkage_mixing)
{
    EXPECT_BUILD_FAIL("static int f(void) {\n"
                      "    return 0;\n"
                      "}\n"
                      "int f(void) {\n"
                      "    return 1;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_static_nonconst_init)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    static int x = 1 + main;\n"
                      "    return x;\n"
                      "}\n");
}

TEST(globals, negative_aggregate_init)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "struct Pt p = 5;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_string_init_non_char_ptr)
{
    EXPECT_BUILD_FAIL("int *p = \"hi\";\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_incomplete_global)
{
    EXPECT_BUILD_FAIL("struct S;\n"
                      "struct S s;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, interp_block_extern)
{
    EXPECT_EQ(tc_run_interp("int shared = 10;\n"
                            "int main(void) {\n"
                            "    extern int shared;\n"
                            "    shared = shared + 5;\n"
                            "    return shared;\n"
                            "}\n"),
              15);
}

TEST(globals, elf_block_extern_shared)
{
    EXPECT_EQ(tc_run_elf("int n;\n"
                         "int a(void) {\n"
                         "    extern int n;\n"
                         "    n = n + 1;\n"
                         "    return n;\n"
                         "}\n"
                         "int b(void) {\n"
                         "    extern int n;\n"
                         "    n = n + 2;\n"
                         "    return n;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    return a() + a() + b();\n"
                         "}\n"),
              7);
}

TEST(globals, negative_block_extern_with_init)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    extern int q = 5;\n"
                      "    return q;\n"
                      "}\n");
}

static const char *nested_shadow_src = "int main(void) {\n"
                                       "    int x = 1;\n"
                                       "    {\n"
                                       "        int x = 2;\n"
                                       "        x = x + 1;\n"
                                       "    }\n"
                                       "    return x;\n"
                                       "}\n";

TEST(globals, interp_nested_shadow)
{
    EXPECT_EQ(tc_run_interp(nested_shadow_src), 1);
}

TEST(globals, interp_shadow_param_nested)
{
    EXPECT_EQ(tc_run_interp("int f(int x) {\n"
                            "    {\n"
                            "        int x = 5;\n"
                            "        x = x + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return f(10);\n"
                            "}\n"),
              10);
}

TEST(globals, interp_shadow_across_branches)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int x = 1;\n"
                            "    if (x) {\n"
                            "        int x = 2;\n"
                            "        x = x + 1;\n"
                            "    } else {\n"
                            "        int x = 3;\n"
                            "        x = x + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"),
              1);
}

TEST(globals, interp_shadow_different_type)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int x = 10;\n"
                            "    {\n"
                            "        char *x = \"ab\";\n"
                            "        x = x + 1;\n"
                            "    }\n"
                            "    return x;\n"
                            "}\n"),
              10);
}

static const char *shadow_in_loop_src = "int main(void) {\n"
                                        "    int total = 0;\n"
                                        "    int i = 0;\n"
                                        "    while (i < 5) {\n"
                                        "        int total = 7;\n"
                                        "        total = total + 1;\n"
                                        "        i = i + 1;\n"
                                        "    }\n"
                                        "    return total;\n"
                                        "}\n";

TEST(globals, interp_shadow_in_loop)
{
    EXPECT_EQ(tc_run_interp(shadow_in_loop_src), 0);
}

TEST(globals, interp_local_shadows_block_static)
{
    EXPECT_EQ(tc_run_interp("int counter(void) {\n"
                            "    static int n = 5;\n"
                            "    {\n"
                            "        int n = 50;\n"
                            "        n = n + 1;\n"
                            "    }\n"
                            "    n = n + 1;\n"
                            "    return n;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return counter() + counter();\n"
                            "}\n"),
              13);
}

TEST(globals, elf_nested_shadow)
{
    EXPECT_EQ(tc_run_elf(nested_shadow_src), 1);
}

TEST(globals, elf_shadow_in_loop)
{
    EXPECT_EQ(tc_run_elf(shadow_in_loop_src), 0);
}

TEST(globals, file_scope_array_list)
{
    EXPECT_INTERP_AND_ELF("int g[3] = {10, 20, 30};\n"
                          "int main(void) {\n"
                          "    return g[0] + g[1] + g[2];\n"
                          "}\n",
                          60);
}

TEST(globals, file_scope_short_list_zero_fills)
{
    EXPECT_INTERP_AND_ELF("int g[5] = {1, 2};\n"
                          "int main(void) {\n"
                          "    return g[0] + g[1] + g[2] + g[3] + g[4];\n"
                          "}\n",
                          3);
}

TEST(globals, file_scope_designated_array)
{
    EXPECT_INTERP_AND_ELF("int g[5] = {1, 2, [4] = 9};\n"
                          "int main(void) {\n"
                          "    return g[0] + g[1] + g[2] + g[3] + g[4];\n"
                          "}\n",
                          12);
}

TEST(globals, file_scope_scalar_braced)
{
    EXPECT_INTERP_AND_ELF("int g = {9};\n"
                          "int main(void) {\n"
                          "    return g;\n"
                          "}\n",
                          9);
}

TEST(globals, file_scope_struct_list)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "struct S s = {10, 20};\n"
                          "int main(void) {\n"
                          "    return s.x + s.y;\n"
                          "}\n",
                          30);
}

TEST(globals, file_scope_struct_designated)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; int y; };\n"
                          "struct S s = {.y = 20, .x = 10};\n"
                          "int main(void) {\n"
                          "    return s.x + s.y;\n"
                          "}\n",
                          30);
}

TEST(globals, file_scope_nested_aggregate)
{
    EXPECT_INTERP_AND_ELF("struct S { int a[2]; int x; };\n"
                          "struct S g = {{1, 2}, 3};\n"
                          "int main(void) {\n"
                          "    return g.a[0] + g.a[1] + g.x;\n"
                          "}\n",
                          6);
}

TEST(globals, file_scope_char_array_from_string)
{
    EXPECT_INTERP_AND_ELF("char s[4] = \"hi\";\n"
                          "int main(void) {\n"
                          "    return s[0] + s[1];\n"
                          "}\n",
                          'h' + 'i');
}

TEST(globals, file_scope_const_aggregate_rodata)
{
    EXPECT_INTERP_AND_ELF("const int ca[2] = {3, 4};\n"
                          "int main(void) {\n"
                          "    return ca[0] + ca[1];\n"
                          "}\n",
                          7);
}

TEST(globals, file_scope_ptr_member_reloc)
{
    EXPECT_INTERP_AND_ELF("int a = 1;\n"
                          "int *g[2] = {&a, 0};\n"
                          "int main(void) {\n"
                          "    if (g[0] == &a) { } else { return 1; }\n"
                          "    if (g[1] != 0) { return 2; }\n"
                          "    return *g[0];\n"
                          "}\n",
                          1);
}

TEST(globals, file_scope_string_ptr_array_relocs)
{
    EXPECT_INTERP_AND_ELF("char *gs[2] = {\"ab\", \"cde\"};\n"
                          "int main(void) {\n"
                          "    if (gs[1][2] != 101) { return 1; }\n"
                          "    return gs[0][0] + gs[0][1];\n"
                          "}\n",
                          195);
}

TEST(globals, file_scope_struct_ptr_member)
{
    EXPECT_INTERP_AND_ELF("struct S { int *p; int x; };\n"
                          "int a = 1;\n"
                          "struct S s = {.p = &a, .x = 7};\n"
                          "int main(void) {\n"
                          "    if (*s.p != 1) { return 1; }\n"
                          "    return s.x;\n"
                          "}\n",
                          7);
}

TEST(globals, block_static_list)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static int a[2] = {5, 6};\n"
                          "    a[0] = a[0] + 1;\n"
                          "    return a[0] + a[1];\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f() + f();\n"
                          "}\n",
                          25);
}

TEST(globals, block_static_short_list_zero_fills)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static int a[4] = {1, 2};\n"
                          "    return a[0] + a[1] + a[2] + a[3];\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f();\n"
                          "}\n",
                          3);
}

TEST(globals, block_static_struct_list)
{
    EXPECT_INTERP_AND_ELF("struct S { int a[2]; int x; };\n"
                          "int f(void) {\n"
                          "    static struct S s = {.x = 11, .a = {7, 8}};\n"
                          "    s.a[0] = s.a[0] + 1;\n"
                          "    return s.a[0] + s.a[1] + s.x;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f() + f();\n"
                          "}\n",
                          55);
}

TEST(globals, block_static_ptr_member_reloc)
{
    EXPECT_INTERP_AND_ELF("int a = 1;\n"
                          "struct S { int *p; };\n"
                          "int f(void) {\n"
                          "    static struct S s = {.p = &a};\n"
                          "    return *s.p;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f() + f();\n"
                          "}\n",
                          2);
}

TEST(globals, block_static_char_array_from_string)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static char s[4] = \"hi\";\n"
                          "    s[0] = s[0] + 1;\n"
                          "    return s[0] + s[2];\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f() + f();\n"
                          "}\n",
                          211);
}

TEST(globals, block_static_string_ptr)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static char *p = \"xy\";\n"
                          "    return p[0] + p[1];\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f();\n"
                          "}\n",
                          'x' + 'y');
}

TEST(globals, file_scope_ptr_initializer_reloc)
{
    EXPECT_INTERP_AND_ELF("int a = 1;\n"
                          "int *p = &a;\n"
                          "int main(void) {\n"
                          "    if (p == &a) { } else { return 1; }\n"
                          "    return *p;\n"
                          "}\n",
                          1);
}

TEST(globals, file_scope_struct_ptr_initializer_reloc)
{
    EXPECT_INTERP_AND_ELF("struct S { int x; };\n"
                          "struct S s = {.x = 1};\n"
                          "struct S *sp = &s;\n"
                          "int main(void) {\n"
                          "    return sp->x;\n"
                          "}\n",
                          1);
}

TEST(globals, block_static_ptr_initializer_reloc)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static int a = 5;\n"
                          "    static int *p = &a;\n"
                          "    *p = *p + 1;\n"
                          "    return a;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f() + f();\n"
                          "}\n",
                          13);
}

TEST(globals, negative_block_static_addr_of_auto)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x = 5;\n"
                      "    static int *p = &x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_file_scope_nonconst_element)
{
    EXPECT_BUILD_FAIL("int x = 5;\n"
                      "int g[2] = {x, 0};\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_block_static_nonconst_element)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int y = 5;\n"
                      "    static int g[2] = {y, 0};\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, negative_file_scope_overlong_list)
{
    EXPECT_BUILD_FAIL("int g[2] = {1, 2, 3};\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(globals, file_scope_incomplete_array_infers)
{
    EXPECT_INTERP_AND_ELF("int g[] = {1, 2, 3};\n"
                          "int main(void) {\n"
                          "    return g[0] + g[1] + g[2];\n"
                          "}\n",
                          6);
}

TEST(globals, file_scope_incomplete_two_d_infers)
{
    EXPECT_INTERP_AND_ELF("int m[][3] = {{1, 2}, {3, 4, 5}};\n"
                          "int main(void) {\n"
                          "    return sizeof(m);\n"
                          "}\n",
                          24);
}

TEST(globals, file_scope_incomplete_char_array_from_string)
{
    EXPECT_INTERP_AND_ELF("char s[] = \"hi\";\n"
                          "int main(void) {\n"
                          "    return s[1];\n"
                          "}\n",
                          'i');
}

TEST(globals, block_static_incomplete_array_infers)
{
    EXPECT_INTERP_AND_ELF("int f(void) {\n"
                          "    static int a[] = {5, 6};\n"
                          "    return a[0] + a[1];\n"
                          "}\n"
                          "int main(void) {\n"
                          "    return f();\n"
                          "}\n",
                          11);
}

TEST(globals, negative_file_scope_bare_incomplete_array)
{
    EXPECT_BUILD_FAIL("int g[];\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

/* C11 §6.7.9p14: an exact-fit string initializer drops the NUL, braced or not. */
TEST(globals, file_scope_char_array_string_drops_nul)
{
    EXPECT_INTERP_AND_ELF("char g[2] = \"hi\";\n"
                          "char h[2] = {\"hi\"};\n"
                          "int main(void) {\n"
                          "    if (g[0] != 104 || g[1] != 105) return 1;\n"
                          "    if (h[0] != 104 || h[1] != 105) return 2;\n"
                          "    if (sizeof(g) != 2 || sizeof(h) != 2) return 3;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(globals, file_scope_empty_braces)
{
    /* `{}` zero-init is a ficc extension, not C11. */
    EXPECT_INTERP_AND_ELF("int g[5] = {};\n"
                          "struct S { int a; int b; };\n"
                          "struct S s = {};\n"
                          "union U { int i; };\n"
                          "union U u = {};\n"
                          "int main(void) {\n"
                          "    return g[0]+g[1]+g[2]+g[3]+g[4] + s.a + s.b + u.i;\n"
                          "}\n",
                          0);
}

TEST(globals, pointer_constant_cast)
{
    EXPECT_INTERP_AND_ELF("#include <stdint.h>\n"
                          "static const char *sentinel = (const char *) -1;\n"
                          "int main(void) { return (intptr_t) sentinel == -1 ? 42 : 1; }\n",
                          42);
}
